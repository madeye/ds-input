//! Regression test for the engine-teardown P0.
//!
//! The Tokio runtime used to live *inside* the `Arc<Engine>` that every in-flight
//! worker task holds. Freeing the engine while a conversion was still running let
//! the task drop the last `Arc<Engine>` on a worker thread, which ran
//! `Runtime::drop` there and panicked ("Cannot drop a runtime in a context where
//! blocking is not allowed") — aborting the host process. The runtime now lives in
//! `EngineHandle` and is dropped on the caller's thread by `ds_engine_free`, so the
//! teardown-during-flight loop below must stay clean.

use std::ffi::{c_char, c_void, CString};
use std::net::TcpListener;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::time::Duration;

use dsime::{
    ds_engine_free, ds_engine_new, ds_session_convert, ds_session_free, ds_session_new,
    ds_session_set_input,
};

extern "C" fn on_done(user_data: *mut c_void, _id: u64, _status: i32, _text: *const c_char) {
    // For a request that happens to finish before teardown cancels it, this fires
    // on a worker thread. `user_data` points at a deliberately-leaked counter that
    // is never freed by this test, so touching it here is always sound.
    if !user_data.is_null() {
        unsafe { (*(user_data as *const AtomicU32)).fetch_add(1, Ordering::SeqCst) };
    }
}

#[test]
fn free_engine_while_conversion_in_flight_does_not_panic() {
    // Catch panics on ANY thread, including tokio workers. The crate builds with
    // `panic = "unwind"`, so a worker-thread panic would otherwise only kill that
    // worker and the test would still report success — which is exactly how the
    // pre-fix bug slips past an in-process test. The pre-fix code panics on a
    // worker here ("Cannot drop a runtime in a context where blocking is not
    // allowed"); the hook records it so the assertion at the end fails.
    static PANICKED: AtomicBool = AtomicBool::new(false);
    let prev_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        PANICKED.store(true, Ordering::SeqCst);
        prev_hook(info);
    }));

    // A TCP server that accepts connections and never replies, so every conversion
    // stays in flight until its per-request timeout — guaranteeing we free the
    // engine mid-request (the scenario that used to panic).
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind black-hole listener");
    let port = listener.local_addr().unwrap().port();
    std::thread::spawn(move || {
        let mut held = Vec::new();
        for stream in listener.incoming() {
            match stream {
                Ok(s) => held.push(s), // keep the socket open; never respond
                Err(_) => break,
            }
        }
    });

    // Throwaway config pointing at the black-hole endpoint with a short timeout.
    let dir = std::env::temp_dir().join(format!("dsime-teardown-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let cfg_path = dir.join("config.json");
    std::fs::write(
        &cfg_path,
        format!(
            r#"{{"api_key":"sk-test","base_url":"http://127.0.0.1:{port}/v1","model":"m","timeout_ms":400}}"#
        ),
    )
    .unwrap();
    let cpath = CString::new(cfg_path.to_string_lossy().as_bytes()).unwrap();

    // A leaked counter the callback can always safely touch, regardless of when it
    // fires (it never asserts ordering against engine teardown).
    let counter: &'static AtomicU32 = Box::leak(Box::new(AtomicU32::new(0)));
    let pinyin = CString::new("nihaoshijie").unwrap();

    for _ in 0..12 {
        unsafe {
            let engine = ds_engine_new(cpath.as_ptr());
            assert!(!engine.is_null(), "engine creation failed");
            let session = ds_session_new(engine);
            assert!(!session.is_null());
            ds_session_set_input(session, pinyin.as_ptr());
            // Kick off a conversion (the endpoint never replies, so it is now in
            // flight)...
            let id =
                ds_session_convert(session, on_done, counter as *const AtomicU32 as *mut c_void);
            assert_ne!(id, 0, "convert should return a non-zero request id");
            // ...then immediately tear down while it is still in flight. With the
            // bug, the in-flight task would later drop the last Arc<Engine> on a
            // worker thread and panic; with the fix, ds_engine_free drops the
            // runtime here on this thread and cancels the task.
            ds_session_free(session);
            ds_engine_free(engine);
        }
    }

    // Give any conversion the OLD code left running on a worker thread time to
    // reach its drop point (and panic) before the test ends.
    std::thread::sleep(Duration::from_millis(700));
    let _ = std::fs::remove_dir_all(&dir);

    // The counter is best-effort (most requests are cancelled before completing);
    // the real assertion is that no thread panicked while tearing the engine down.
    let _ = counter.load(Ordering::SeqCst);
    assert!(
        !PANICKED.load(Ordering::SeqCst),
        "a thread panicked during engine teardown — the P0 regression is back"
    );
}
