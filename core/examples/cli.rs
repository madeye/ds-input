//! `dsime-cli` — a headless smoke tester for the C ABI.
//!
//! It drives the exact same FFI surface the platform frontends use, so it both
//! validates the engine end-to-end and serves as a runnable reference for how a
//! frontend should call the library.
//!
//! Usage:
//!   # one-shot conversion (reads pinyin from args, or stdin if none)
//!   DSIME_API_KEY=sk-... cargo run --example cli -- ni hao shi jie
//!   echo "wo ai beijing" | DSIME_API_KEY=sk-... cargo run --example cli
//!
//! Env overrides (applied to the in-memory config before converting):
//!   DSIME_API_KEY, DSIME_BASE_URL, DSIME_MODEL
//!
//! With no DSIME_API_KEY set it still runs and exercises config + session,
//! printing the auth/config error the engine returns (useful for CI smoke).

use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::mpsc::{sync_channel, SyncSender};

// The crate exposes the C ABI as public `extern "C"` functions; call them
// directly so this binary tests the real FFI boundary.
use dsime::{
    ds_engine_get_config_json, ds_engine_new, ds_engine_set_config_json, ds_session_convert_stream,
    ds_session_free, ds_session_new, ds_session_reset, ds_session_set_input, ds_string_free,
    ds_version, Engine,
};

struct Done {
    tx: SyncSender<(i32, String)>,
}

/// Streaming callback: print each partial (cumulative) update live on stderr,
/// and hand the terminal outcome to the main thread via the channel.
extern "C" fn on_stream(
    user_data: *mut c_void,
    _request_id: u64,
    status: i32,
    is_final: i32,
    text_utf8: *const c_char,
) {
    let text = if text_utf8.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(text_utf8) }
            .to_string_lossy()
            .into_owned()
    };
    let done = unsafe { &*(user_data as *const Done) };
    if is_final != 0 {
        let _ = done.tx.send((status, text));
    } else {
        // Overwrite the line so the pre-edit appears to fill in live.
        use std::io::Write;
        eprint!("\r\x1b[2K… {text}");
        let _ = std::io::stderr().flush();
    }
}

fn main() {
    let pinyin: String = {
        let args: Vec<String> = std::env::args().skip(1).collect();
        if args.is_empty() {
            use std::io::Read;
            let mut s = String::new();
            std::io::stdin().read_to_string(&mut s).ok();
            s.trim().to_string()
        } else {
            args.join(" ")
        }
    };
    if pinyin.is_empty() {
        eprintln!("usage: cli <pinyin...>   (or pipe pinyin on stdin)");
        std::process::exit(2);
    }

    unsafe {
        let version = CStr::from_ptr(ds_version()).to_string_lossy();
        eprintln!("dsime core v{version}");

        // Use a throwaway config file so we never touch the user's real config.
        let tmp = std::env::temp_dir().join("dsime-cli-config.json");
        let cpath = CString::new(tmp.to_string_lossy().as_bytes()).unwrap();
        let engine: *mut Engine = ds_engine_new(cpath.as_ptr());
        assert!(!engine.is_null(), "engine creation failed");

        apply_env_overrides(engine);

        let session = ds_session_new(engine);
        assert!(!session.is_null());

        let cpinyin = CString::new(pinyin.as_bytes()).unwrap();
        ds_session_set_input(session, cpinyin.as_ptr());

        let (tx, rx) = sync_channel::<(i32, String)>(1);
        let done = Box::new(Done { tx });
        let done_ptr = Box::into_raw(done);

        let req = ds_session_convert_stream(session, on_stream, done_ptr as *mut c_void);
        eprintln!("request #{req} sent for: {pinyin:?}");

        match rx.recv_timeout(std::time::Duration::from_secs(30)) {
            Ok((0, text)) => {
                eprintln!(); // end the live partial line before the final result
                println!("{text}");
            }
            Ok((status, msg)) => {
                eprintln!("conversion failed (status {status}): {msg}");
                cleanup(session, engine, done_ptr);
                std::process::exit(1);
            }
            Err(_) => {
                eprintln!("timed out waiting for conversion");
                cleanup(session, engine, done_ptr);
                std::process::exit(1);
            }
        }

        ds_session_reset(session);
        cleanup(session, engine, done_ptr);
    }
}

unsafe fn cleanup(session: *mut dsime::Session, engine: *mut Engine, done_ptr: *mut Done) {
    ds_session_free(session);
    dsime::ds_engine_free(engine);
    drop(Box::from_raw(done_ptr));
}

/// Patch the freshly-created config with any env overrides via the JSON API,
/// exactly as a Settings UI would.
unsafe fn apply_env_overrides(engine: *mut Engine) {
    let json_ptr = ds_engine_get_config_json(engine);
    if json_ptr.is_null() {
        return;
    }
    let json = CStr::from_ptr(json_ptr).to_string_lossy().into_owned();
    ds_string_free(json_ptr);

    let mut value: serde_json::Value = serde_json::from_str(&json).unwrap();
    if let Ok(k) = std::env::var("DSIME_API_KEY") {
        value["api_key"] = k.into();
    }
    if let Ok(u) = std::env::var("DSIME_BASE_URL") {
        value["base_url"] = u.into();
    }
    if let Ok(m) = std::env::var("DSIME_MODEL") {
        value["model"] = m.into();
    }
    let patched = CString::new(serde_json::to_string(&value).unwrap()).unwrap();
    let _ = ds_engine_set_config_json(engine, patched.as_ptr());
}
