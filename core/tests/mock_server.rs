//! End-to-end test of the FFI against a mock OpenAI-compatible endpoint.
//!
//! Spins up a one-shot localhost HTTP server that returns a canned
//! chat-completions response, points the engine's `base_url` at it via the JSON
//! config API, then drives a real `ds_session_convert` and asserts the async
//! callback delivers the converted sentence. No network, no API key needed.

use std::ffi::{c_char, c_void, CStr, CString};
use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::mpsc::{sync_channel, SyncSender};

use dsime::{
    ds_engine_free, ds_engine_new, ds_engine_set_config_json, ds_session_cancel,
    ds_session_convert, ds_session_convert_stream, ds_session_free, ds_session_new,
    ds_session_set_input, Engine, Session,
};

/// Serve exactly one request: read the (ignored) body, reply with `body_json`.
fn spawn_mock(body_json: &'static str) -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    std::thread::spawn(move || {
        if let Ok((mut stream, _)) = listener.accept() {
            // Drain what's available; enough to let the client finish sending.
            let mut buf = [0u8; 4096];
            let _ = stream.read(&mut buf);
            let resp = format!(
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                body_json.len(),
                body_json
            );
            let _ = stream.write_all(resp.as_bytes());
            let _ = stream.flush();
        }
    });
    port
}

extern "C" fn capture(user_data: *mut c_void, _req: u64, status: i32, text: *const c_char) {
    let s = if text.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(text) }
            .to_string_lossy()
            .into_owned()
    };
    let tx = unsafe { &*(user_data as *const SyncSender<(i32, String)>) };
    let _ = tx.send((status, s));
}

#[test]
fn convert_round_trips_through_mock_provider() {
    let port = spawn_mock(r#"{"choices":[{"message":{"role":"assistant","content":"你好世界"}}]}"#);

    // Isolated temp config so we never touch the user's real file.
    let tmp = std::env::temp_dir().join(format!("dsime-mock-{}.json", std::process::id()));
    let cpath = CString::new(tmp.to_string_lossy().as_bytes()).unwrap();

    unsafe {
        let engine: *mut Engine = ds_engine_new(cpath.as_ptr());
        assert!(!engine.is_null());

        // Point at the mock and give a dummy key so the engine proceeds.
        let cfg = format!(
            r#"{{"base_url":"http://127.0.0.1:{port}","api_key":"sk-test","model":"mock"}}"#
        );
        let ccfg = CString::new(cfg).unwrap();
        assert_eq!(ds_engine_set_config_json(engine, ccfg.as_ptr()), 0);

        let session: *mut Session = ds_session_new(engine);
        let input = CString::new("nihaoshijie").unwrap();
        ds_session_set_input(session, input.as_ptr());

        let (tx, rx) = sync_channel::<(i32, String)>(1);
        let req = ds_session_convert(session, capture, &tx as *const _ as *mut c_void);
        assert!(req > 0, "non-empty buffer should produce a request id");

        let (status, text) = rx
            .recv_timeout(std::time::Duration::from_secs(10))
            .expect("callback should fire");
        assert_eq!(status, 0, "expected DS_OK, got status {status}: {text}");
        assert_eq!(text, "你好世界");

        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_file(&tmp);
}

/// A server that accepts the connection but never replies, so the request stays
/// in flight until cancelled. Returns the bound port.
fn spawn_hanging_server() -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    std::thread::spawn(move || {
        // Hold the connection open for a while without responding.
        if let Ok((stream, _)) = listener.accept() {
            std::thread::sleep(std::time::Duration::from_secs(5));
            drop(stream);
        }
    });
    port
}

#[test]
fn cancel_delivers_exactly_one_cancelled_callback() {
    // Guarantees the EXACTLY-ONCE contract: an in-flight request that is
    // cancelled still fires its callback (with DS_ERR_CANCELLED == 4) precisely
    // once. Frontends rely on this to release per-request resources.
    let port = spawn_hanging_server();
    let tmp = std::env::temp_dir().join(format!("dsime-cancel-{}.json", std::process::id()));
    let cpath = CString::new(tmp.to_string_lossy().as_bytes()).unwrap();

    unsafe {
        let engine = ds_engine_new(cpath.as_ptr());
        let cfg = format!(
            r#"{{"base_url":"http://127.0.0.1:{port}","api_key":"sk-test","model":"mock","timeout_ms":4000}}"#
        );
        let ccfg = CString::new(cfg).unwrap();
        assert_eq!(ds_engine_set_config_json(engine, ccfg.as_ptr()), 0);

        let session = ds_session_new(engine);
        let input = CString::new("nihao").unwrap();
        ds_session_set_input(session, input.as_ptr());

        let (tx, rx) = sync_channel::<(i32, String)>(4);
        let req = ds_session_convert(session, capture, &tx as *const _ as *mut c_void);
        assert!(req > 0);

        // Give the task a moment to enter its await, then cancel.
        std::thread::sleep(std::time::Duration::from_millis(50));
        ds_session_cancel(session);

        let (status, _) = rx
            .recv_timeout(std::time::Duration::from_secs(3))
            .expect("cancelled request must still fire its callback");
        assert_eq!(status, 4, "expected DS_ERR_CANCELLED");

        // And it must fire EXACTLY once — no second delivery.
        assert!(
            rx.recv_timeout(std::time::Duration::from_millis(300))
                .is_err(),
            "callback must fire exactly once"
        );

        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_file(&tmp);
}

/// Serve one SSE chat-completions stream: a `data:` frame per delta, then
/// `data: [DONE]`, then close the connection.
fn spawn_mock_sse(deltas: &'static [&'static str]) -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    std::thread::spawn(move || {
        if let Ok((mut stream, _)) = listener.accept() {
            let mut buf = [0u8; 4096];
            let _ = stream.read(&mut buf);
            let _ = stream.write_all(
                b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n",
            );
            for d in deltas {
                let frame =
                    format!("data: {{\"choices\":[{{\"delta\":{{\"content\":\"{d}\"}}}}]}}\n\n");
                let _ = stream.write_all(frame.as_bytes());
                let _ = stream.flush();
                std::thread::sleep(std::time::Duration::from_millis(15));
            }
            let _ = stream.write_all(b"data: [DONE]\n\n");
            let _ = stream.flush();
        }
    });
    port
}

extern "C" fn capture_stream(
    user_data: *mut c_void,
    _req: u64,
    status: i32,
    is_final: i32,
    text: *const c_char,
) {
    let s = if text.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(text) }
            .to_string_lossy()
            .into_owned()
    };
    let tx = unsafe { &*(user_data as *const SyncSender<(i32, i32, String)>) };
    let _ = tx.send((status, is_final, s));
}

#[test]
fn convert_stream_delivers_partials_then_one_final() {
    let port = spawn_mock_sse(&["你", "好", "世界"]);
    let tmp = std::env::temp_dir().join(format!("dsime-stream-{}.json", std::process::id()));
    let cpath = CString::new(tmp.to_string_lossy().as_bytes()).unwrap();

    unsafe {
        let engine = ds_engine_new(cpath.as_ptr());
        assert!(!engine.is_null());
        let cfg = format!(
            r#"{{"base_url":"http://127.0.0.1:{port}","api_key":"sk-test","model":"mock","stream":true}}"#
        );
        let ccfg = CString::new(cfg).unwrap();
        assert_eq!(ds_engine_set_config_json(engine, ccfg.as_ptr()), 0);

        let session = ds_session_new(engine);
        let input = CString::new("nihaoshijie").unwrap();
        ds_session_set_input(session, input.as_ptr());

        let (tx, rx) = sync_channel::<(i32, i32, String)>(16);
        let req =
            ds_session_convert_stream(session, capture_stream, &tx as *const _ as *mut c_void);
        assert!(req > 0);

        let mut partials: Vec<String> = Vec::new();
        let final_text = loop {
            let (status, is_final, text) = rx
                .recv_timeout(std::time::Duration::from_secs(10))
                .expect("a stream event should arrive");
            assert_eq!(status, 0, "expected DS_OK, got {status}: {text}");
            if is_final == 1 {
                break text;
            }
            partials.push(text);
        };

        assert_eq!(final_text, "你好世界", "final must be the full sentence");
        assert!(!partials.is_empty(), "at least one partial must arrive");
        assert_eq!(
            partials.last().map(String::as_str),
            Some("你好世界"),
            "partials are cumulative; the last equals the full text"
        );
        // The terminal (is_final=1) callback must fire EXACTLY once.
        assert!(
            rx.recv_timeout(std::time::Duration::from_millis(300))
                .is_err(),
            "no events after the terminal callback"
        );

        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_file(&tmp);
}

#[test]
fn empty_buffer_returns_zero_request_id() {
    let tmp = std::env::temp_dir().join(format!("dsime-empty-{}.json", std::process::id()));
    let cpath = CString::new(tmp.to_string_lossy().as_bytes()).unwrap();
    unsafe {
        let engine = ds_engine_new(cpath.as_ptr());
        let session = ds_session_new(engine);
        // No set_input → empty buffer → convert must be a no-op returning 0.
        let req = ds_session_convert(session, capture, std::ptr::null_mut());
        assert_eq!(req, 0);
        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_file(&tmp);
}
