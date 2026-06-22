//! End-to-end tests for local n-gram *speculative* conversion across the FFI.
//!
//! Covers the three things a frontend relies on:
//!   1. `ds_engine_learn` + `ds_session_speculate` give an instant local guess;
//!   2. a successful remote conversion auto-trains the model, so the next
//!      identical input is speculated locally without any network;
//!   3. in streaming mode the local speculation is painted as the FIRST partial,
//!      before the remote tokens arrive and overwrite it.

use std::ffi::{c_char, c_void, CStr, CString};
use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::mpsc::{sync_channel, SyncSender};

use dsime::{
    ds_engine_free, ds_engine_learn, ds_engine_new, ds_engine_set_config_json, ds_session_convert,
    ds_session_convert_stream, ds_session_free, ds_session_new, ds_session_set_input,
    ds_session_speculate, ds_string_free, Engine, Session,
};

/// A throwaway config path in a unique temp dir. The sibling `ngram.json` the
/// engine writes lands in the same dir, so each test is fully isolated.
fn temp_config(tag: &str) -> std::path::PathBuf {
    let dir = std::env::temp_dir().join(format!("dsime-spec-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir.join("config.json")
}

/// Read + free a `ds_*` string that the caller owns. NULL becomes "".
unsafe fn take_string(p: *mut c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    let s = CStr::from_ptr(p).to_string_lossy().into_owned();
    ds_string_free(p);
    s
}

/// Serve exactly one chat-completions request with a canned JSON body.
fn spawn_mock(body_json: &'static str) -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    std::thread::spawn(move || {
        if let Ok((mut stream, _)) = listener.accept() {
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
fn learn_then_speculate_locally() {
    let cfg_path = temp_config("learn");
    let cpath = CString::new(cfg_path.to_string_lossy().as_bytes()).unwrap();

    unsafe {
        let engine: *mut Engine = ds_engine_new(cpath.as_ptr());
        assert!(!engine.is_null());

        // Teach the model a couple of phrases, exactly as a frontend would after
        // the user commits some text.
        let py = CString::new("nihao").unwrap();
        let hz = CString::new("你好").unwrap();
        assert_eq!(ds_engine_learn(engine, py.as_ptr(), hz.as_ptr()), 0);
        let py2 = CString::new("shijie").unwrap();
        let hz2 = CString::new("世界").unwrap();
        assert_eq!(ds_engine_learn(engine, py2.as_ptr(), hz2.as_ptr()), 0);

        let session: *mut Session = ds_session_new(engine);

        // A fully-covered input speculates locally, even recombining syllables
        // learned from different phrases.
        let input = CString::new("nihaoshijie").unwrap();
        ds_session_set_input(session, input.as_ptr());
        assert_eq!(take_string(ds_session_speculate(session)), "你好世界");

        // An input with an unseen syllable yields no speculation (empty string).
        let unseen = CString::new("nihaowomen").unwrap();
        ds_session_set_input(session, unseen.as_ptr());
        assert_eq!(take_string(ds_session_speculate(session)), "");

        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_dir_all(cfg_path.parent().unwrap());
}

#[test]
fn successful_conversion_auto_trains_model() {
    // A real (mock) conversion should train the local model, so a later
    // speculation of the same input needs no network at all.
    let port = spawn_mock(r#"{"choices":[{"message":{"role":"assistant","content":"你好世界"}}]}"#);
    let cfg_path = temp_config("auto");
    let cpath = CString::new(cfg_path.to_string_lossy().as_bytes()).unwrap();

    unsafe {
        let engine = ds_engine_new(cpath.as_ptr());
        assert!(!engine.is_null());
        // Non-streaming so the single terminal callback marks completion (and
        // thus that auto-learn has run).
        let cfg = format!(
            r#"{{"base_url":"http://127.0.0.1:{port}","api_key":"sk-test","model":"mock","stream":false}}"#
        );
        let ccfg = CString::new(cfg).unwrap();
        assert_eq!(ds_engine_set_config_json(engine, ccfg.as_ptr()), 0);

        let session = ds_session_new(engine);
        let input = CString::new("nihaoshijie").unwrap();
        ds_session_set_input(session, input.as_ptr());

        // Before any conversion the model is empty → no speculation.
        assert_eq!(take_string(ds_session_speculate(session)), "");

        let (tx, rx) = sync_channel::<(i32, String)>(1);
        let req = ds_session_convert(session, capture, &tx as *const _ as *mut c_void);
        assert!(req > 0);
        let (status, text) = rx
            .recv_timeout(std::time::Duration::from_secs(10))
            .expect("callback should fire");
        assert_eq!(status, 0, "expected DS_OK, got {status}: {text}");
        assert_eq!(text, "你好世界");

        // The conversion auto-trained the model: the same input now speculates
        // locally, no server involved.
        assert_eq!(take_string(ds_session_speculate(session)), "你好世界");

        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_dir_all(cfg_path.parent().unwrap());
}

/// Serve one SSE stream: a `data:` frame per delta, then `[DONE]`.
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
                std::thread::sleep(std::time::Duration::from_millis(20));
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
fn speculation_is_the_first_stream_partial() {
    // Pre-train the model, then stream a conversion: the very first partial must
    // be the instant local guess, ahead of the remote deltas, and the terminal
    // result is still the authoritative remote text.
    let port = spawn_mock_sse(&["你", "好", "世界"]);
    let cfg_path = temp_config("stream");
    let cpath = CString::new(cfg_path.to_string_lossy().as_bytes()).unwrap();

    unsafe {
        let engine = ds_engine_new(cpath.as_ptr());
        assert!(!engine.is_null());
        let cfg = format!(
            r#"{{"base_url":"http://127.0.0.1:{port}","api_key":"sk-test","model":"mock","stream":true}}"#
        );
        let ccfg = CString::new(cfg).unwrap();
        assert_eq!(ds_engine_set_config_json(engine, ccfg.as_ptr()), 0);

        // Pre-train so a local guess exists the instant the request starts.
        let py = CString::new("nihaoshijie").unwrap();
        let hz = CString::new("你好世界").unwrap();
        assert_eq!(ds_engine_learn(engine, py.as_ptr(), hz.as_ptr()), 0);

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

        assert_eq!(
            partials.first().map(String::as_str),
            Some("你好世界"),
            "the first partial must be the instant local speculation"
        );
        assert_eq!(final_text, "你好世界", "remote result is authoritative");

        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_dir_all(cfg_path.parent().unwrap());
}
