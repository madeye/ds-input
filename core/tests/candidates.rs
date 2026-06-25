//! End-to-end tests for up/down candidate cycling across the FFI.
//!
//! Covers what a frontend relies on:
//!   1. a completed conversion becomes candidate 0;
//!   2. `ds_session_candidate_cached` returns "" when there is nothing in that
//!      direction (up at the primary, down past the last);
//!   3. `ds_session_regenerate` fetches a DIFFERENT conversion, excluding the
//!      ones already shown, and appends it;
//!   4. up/down then revisit the cached candidates with no further network.

use std::ffi::{c_char, c_void, CStr, CString};
use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::mpsc::{sync_channel, SyncSender};
use std::sync::{Arc, Mutex};

// Multiple tokio runtimes in one process are flaky under concurrent test
// scheduling; serialize these like the other FFI integration tests.
static SERIAL: Mutex<()> = Mutex::new(());

use dsime::{
    ds_engine_free, ds_engine_new, ds_engine_set_config_json, ds_session_candidate_cached,
    ds_session_convert, ds_session_free, ds_session_new, ds_session_regenerate,
    ds_session_set_input, ds_string_free, EngineHandle, Session,
};

fn temp_config(tag: &str) -> std::path::PathBuf {
    let dir = std::env::temp_dir().join(format!("dsime-cand-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir.join("config.json")
}

unsafe fn take_string(p: *mut c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    let s = CStr::from_ptr(p).to_string_lossy().into_owned();
    ds_string_free(p);
    s
}

/// Serve `bodies.len()` sequential chat-completions requests, each with the next
/// canned JSON body. Records every request's raw bytes so the test can assert
/// what was sent (e.g. that regeneration excludes prior candidates).
fn spawn_seq_mock(bodies: Vec<&'static str>) -> (u16, Arc<Mutex<Vec<String>>>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let seen = Arc::new(Mutex::new(Vec::<String>::new()));
    let seen_t = seen.clone();
    std::thread::spawn(move || {
        for body in bodies {
            let Ok((mut stream, _)) = listener.accept() else {
                return;
            };
            // Read headers, then the Content-Length body, so the full request
            // (including the exclusion instruction) is captured.
            let mut raw = Vec::new();
            let mut tmp = [0u8; 2048];
            let mut content_len = None;
            loop {
                let n = stream.read(&mut tmp).unwrap_or(0);
                if n == 0 {
                    break;
                }
                raw.extend_from_slice(&tmp[..n]);
                if content_len.is_none() {
                    if let Ok(text) = std::str::from_utf8(&raw) {
                        if let Some(i) = text.to_ascii_lowercase().find("content-length:") {
                            let rest = &text[i + "content-length:".len()..];
                            let num: String = rest
                                .trim_start()
                                .chars()
                                .take_while(|c| c.is_ascii_digit())
                                .collect();
                            content_len = num.parse::<usize>().ok();
                        }
                    }
                }
                if let (Some(cl), Some(hdr_end)) =
                    (content_len, raw.windows(4).position(|w| w == b"\r\n\r\n"))
                {
                    if raw.len() >= hdr_end + 4 + cl {
                        break;
                    }
                }
            }
            seen_t
                .lock()
                .unwrap()
                .push(String::from_utf8_lossy(&raw).into_owned());
            let resp = format!(
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                body.len(),
                body
            );
            let _ = stream.write_all(resp.as_bytes());
            let _ = stream.flush();
        }
    });
    (port, seen)
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

extern "C" fn capture_stream(
    user_data: *mut c_void,
    _req: u64,
    status: i32,
    is_final: i32,
    text: *const c_char,
) {
    if is_final != 1 {
        return; // ignore partials; we only assert the terminal result
    }
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
fn down_regenerates_and_up_down_revisit_cached_candidates() {
    let _g = SERIAL.lock().unwrap();
    // First request -> primary; second (regeneration) -> a different conversion.
    let (port, seen) = spawn_seq_mock(vec![
        r#"{"choices":[{"message":{"role":"assistant","content":"你好世界"}}]}"#,
        r#"{"choices":[{"message":{"role":"assistant","content":"你好视界"}}]}"#,
    ]);
    let cfg_path = temp_config("cycle");
    let cpath = CString::new(cfg_path.to_string_lossy().as_bytes()).unwrap();

    unsafe {
        let engine: *mut EngineHandle = ds_engine_new(cpath.as_ptr());
        assert!(!engine.is_null());
        // Non-streaming: one terminal callback per request.
        let cfg = format!(
            r#"{{"base_url":"http://127.0.0.1:{port}","api_key":"sk-test","model":"mock","stream":false}}"#
        );
        let ccfg = CString::new(cfg).unwrap();
        assert_eq!(ds_engine_set_config_json(engine, ccfg.as_ptr()), 0);

        let session: *mut Session = ds_session_new(engine);
        let input = CString::new("nihaoshijie").unwrap();
        ds_session_set_input(session, input.as_ptr());

        // 1. Primary conversion -> candidate 0.
        let (tx, rx) = sync_channel::<(i32, String)>(1);
        assert!(ds_session_convert(session, capture, &tx as *const _ as *mut c_void) > 0);
        let (st, txt) = rx.recv_timeout(std::time::Duration::from_secs(10)).unwrap();
        assert_eq!(st, 0);
        assert_eq!(txt, "你好世界");

        // 2. Only one candidate so far: nothing up or down.
        assert_eq!(take_string(ds_session_candidate_cached(session, 1)), "");
        assert_eq!(take_string(ds_session_candidate_cached(session, -1)), "");

        // 3. Down past the end -> regenerate a DIFFERENT conversion.
        let (tx2, rx2) = sync_channel::<(i32, String)>(1);
        assert!(
            ds_session_regenerate(session, capture_stream, &tx2 as *const _ as *mut c_void) > 0
        );
        let (st2, txt2) = rx2
            .recv_timeout(std::time::Duration::from_secs(10))
            .unwrap();
        assert_eq!(st2, 0);
        assert_eq!(txt2, "你好视界");

        // The regeneration request must have told the model to avoid the primary.
        let requests = seen.lock().unwrap();
        assert_eq!(
            requests.len(),
            2,
            "expected a convert + a regenerate request"
        );
        assert!(
            requests[1].contains("你好世界"),
            "regenerate request should exclude the already-shown candidate; body was:\n{}",
            requests[1]
        );
        drop(requests);

        // 4. Up returns to the primary; down revisits the regenerated one (cached,
        //    no third request); down again is exhausted.
        assert_eq!(
            take_string(ds_session_candidate_cached(session, -1)),
            "你好世界"
        );
        assert_eq!(
            take_string(ds_session_candidate_cached(session, 1)),
            "你好视界"
        );
        assert_eq!(take_string(ds_session_candidate_cached(session, 1)), "");

        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_dir_all(cfg_path.parent().unwrap());
}

#[test]
fn candidate_cache_is_invalidated_when_input_changes() {
    let _g = SERIAL.lock().unwrap();
    let (port, _seen) = spawn_seq_mock(vec![
        r#"{"choices":[{"message":{"role":"assistant","content":"你好"}}]}"#,
    ]);
    let cfg_path = temp_config("invalidate");
    let cpath = CString::new(cfg_path.to_string_lossy().as_bytes()).unwrap();

    unsafe {
        let engine = ds_engine_new(cpath.as_ptr());
        let cfg = format!(
            r#"{{"base_url":"http://127.0.0.1:{port}","api_key":"sk-test","model":"mock","stream":false}}"#
        );
        let ccfg = CString::new(cfg).unwrap();
        assert_eq!(ds_engine_set_config_json(engine, ccfg.as_ptr()), 0);

        let session = ds_session_new(engine);
        let a = CString::new("nihao").unwrap();
        ds_session_set_input(session, a.as_ptr());
        let (tx, rx) = sync_channel::<(i32, String)>(1);
        assert!(ds_session_convert(session, capture, &tx as *const _ as *mut c_void) > 0);
        assert_eq!(
            rx.recv_timeout(std::time::Duration::from_secs(10))
                .unwrap()
                .1,
            "你好"
        );

        // Typing more changes the buffer: the old candidate must not be offered.
        let b = CString::new("nihaoma").unwrap();
        ds_session_set_input(session, b.as_ptr());
        assert_eq!(take_string(ds_session_candidate_cached(session, -1)), "");
        assert_eq!(take_string(ds_session_candidate_cached(session, 1)), "");

        ds_session_free(session);
        ds_engine_free(engine);
    }
    let _ = std::fs::remove_dir_all(cfg_path.parent().unwrap());
}
