//! C ABI for the DS Input core engine. See `core/include/dsime.h` for the
//! authoritative documented interface. This file is the thin, `unsafe` FFI shell
//! over the safe `engine` / `api` / `config` modules.

mod api;
mod config;
mod engine;

pub use engine::{Engine, Session};
use std::cell::RefCell;
use std::ffi::{c_char, c_void, CStr, CString};
use std::ptr;
use std::sync::Arc;

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").unwrap());
}

fn set_last_error(msg: impl Into<String>) {
    let c = CString::new(msg.into()).unwrap_or_else(|_| CString::new("error").unwrap());
    LAST_ERROR.with(|e| *e.borrow_mut() = c);
}

/// Borrow a `&str` from a C string pointer; returns None for NULL/invalid UTF-8.
unsafe fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    CStr::from_ptr(p).to_str().ok()
}

/// Allocate a C string the caller must free with `ds_string_free`.
fn to_c_string(s: impl Into<Vec<u8>>) -> *mut c_char {
    match CString::new(s) {
        Ok(c) => c.into_raw(),
        Err(_) => ptr::null_mut(),
    }
}

// ---- Engine lifecycle ------------------------------------------------------

/// # Safety
/// `config_path` is NULL or a valid NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_new(config_path: *const c_char) -> *mut Engine {
    let path = cstr(config_path).map(std::path::PathBuf::from);
    match Engine::new(path) {
        Ok(engine) => Arc::into_raw(engine) as *mut Engine,
        Err(e) => {
            set_last_error(e);
            ptr::null_mut()
        }
    }
}

/// # Safety
/// `engine` is a pointer returned by `ds_engine_new`, used at most once here.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_free(engine: *mut Engine) {
    if !engine.is_null() {
        drop(Arc::from_raw(engine as *const Engine));
    }
}

/// Borrow an `Arc<Engine>` without taking ownership of the refcount.
unsafe fn engine_ref<'a>(engine: *mut Engine) -> Option<&'a Engine> {
    if engine.is_null() {
        None
    } else {
        Some(&*engine)
    }
}

/// # Safety
/// `engine` is a valid pointer from `ds_engine_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_reload_config(engine: *mut Engine) -> i32 {
    let Some(e) = engine_ref(engine) else {
        return 5; // DS_ERR_CONFIG
    };
    match e.reload_config() {
        Ok(()) => 0,
        Err(msg) => {
            set_last_error(msg);
            5
        }
    }
}

/// # Safety
/// `engine` is a valid pointer from `ds_engine_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_get_config_json(engine: *mut Engine) -> *mut c_char {
    let Some(e) = engine_ref(engine) else {
        return ptr::null_mut();
    };
    match e.get_config_json() {
        Ok(json) => to_c_string(json),
        Err(msg) => {
            set_last_error(msg);
            ptr::null_mut()
        }
    }
}

/// # Safety
/// `engine` is valid; `json_utf8` is a valid NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_set_config_json(
    engine: *mut Engine,
    json_utf8: *const c_char,
) -> i32 {
    let Some(e) = engine_ref(engine) else {
        return 5;
    };
    let Some(json) = cstr(json_utf8) else {
        set_last_error("config json is NULL or not UTF-8");
        return 5;
    };
    match e.set_config_json(json) {
        Ok(()) => 0,
        Err(msg) => {
            set_last_error(msg);
            5
        }
    }
}

/// # Safety
/// `engine` is a valid pointer from `ds_engine_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_config_path(engine: *mut Engine) -> *mut c_char {
    let Some(e) = engine_ref(engine) else {
        return ptr::null_mut();
    };
    to_c_string(e.config_path().to_string_lossy().into_owned())
}

/// # Safety
/// `engine` is a valid pointer from `ds_engine_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_engine_debounce_ms(engine: *mut Engine) -> u32 {
    engine_ref(engine).map(|e| e.debounce_ms()).unwrap_or(100)
}

// ---- Session lifecycle -----------------------------------------------------

/// # Safety
/// `engine` is a valid pointer from `ds_engine_new` and outlives the session.
#[no_mangle]
pub unsafe extern "C" fn ds_session_new(engine: *mut Engine) -> *mut Session {
    let Some(_) = engine_ref(engine) else {
        return ptr::null_mut();
    };
    // Bump the engine refcount for the lifetime of this session.
    Arc::increment_strong_count(engine as *const Engine);
    let engine_arc = Arc::from_raw(engine as *const Engine);
    let session = Box::new(Session::new(engine_arc));
    Box::into_raw(session)
}

/// # Safety
/// `session` is a pointer from `ds_session_new`, used at most once here.
#[no_mangle]
pub unsafe extern "C" fn ds_session_free(session: *mut Session) {
    if !session.is_null() {
        drop(Box::from_raw(session));
    }
}

unsafe fn session_ref<'a>(session: *mut Session) -> Option<&'a Session> {
    if session.is_null() {
        None
    } else {
        Some(&*session)
    }
}

/// # Safety
/// `session` is valid; `pinyin_ascii` is a valid NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn ds_session_set_input(session: *mut Session, pinyin_ascii: *const c_char) {
    if let (Some(s), Some(p)) = (session_ref(session), cstr(pinyin_ascii)) {
        s.set_input(p);
    }
}

/// # Safety
/// `session` is a valid pointer from `ds_session_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_session_get_input(session: *mut Session) -> *mut c_char {
    match session_ref(session) {
        Some(s) => to_c_string(s.get_input()),
        None => to_c_string(""),
    }
}

/// Callback type matching `DsConvertCallback` in dsime.h.
pub type DsConvertCallback =
    extern "C" fn(user_data: *mut c_void, request_id: u64, status: i32, text_utf8: *const c_char);

/// Wrapper so a raw `void*` can cross into the async task. The frontend owns the
/// pointed-to data and guarantees it stays valid until the callback fires.
struct UserData(*mut c_void);
unsafe impl Send for UserData {}

/// # Safety
/// `session` is valid; `callback` is a valid function pointer; `user_data`
/// stays valid until the callback is invoked.
#[no_mangle]
pub unsafe extern "C" fn ds_session_convert(
    session: *mut Session,
    callback: DsConvertCallback,
    user_data: *mut c_void,
) -> u64 {
    let Some(s) = session_ref(session) else {
        return 0;
    };
    let ud = UserData(user_data);
    s.convert(move |outcome| {
        let ud = ud; // move into closure; Send via wrapper
        let text = CString::new(outcome.text).unwrap_or_else(|_| CString::new("").unwrap());
        callback(ud.0, outcome.request_id, outcome.status, text.as_ptr());
    })
}

/// Callback type matching `DsStreamCallback` in dsime.h. Fires zero-or-more
/// times with `is_final = 0` (a partial, cumulative pre-edit), then exactly once
/// with `is_final = 1` (the terminal outcome — final text or DS_ERR_*).
pub type DsStreamCallback = extern "C" fn(
    user_data: *mut c_void,
    request_id: u64,
    status: i32,
    is_final: i32,
    text_utf8: *const c_char,
);

/// # Safety
/// `session` is valid; `callback` is a valid function pointer; `user_data`
/// stays valid until the terminal (`is_final = 1`) callback is invoked.
#[no_mangle]
pub unsafe extern "C" fn ds_session_convert_stream(
    session: *mut Session,
    callback: DsStreamCallback,
    user_data: *mut c_void,
) -> u64 {
    let Some(s) = session_ref(session) else {
        return 0;
    };
    let ud_partial = UserData(user_data);
    let ud_final = UserData(user_data);
    s.convert_stream(
        move |request_id, cumulative| {
            let ud = &ud_partial;
            let text = CString::new(cumulative).unwrap_or_else(|_| CString::new("").unwrap());
            callback(
                ud.0,
                request_id,
                0, /* DS_OK */
                0, /* partial */
                text.as_ptr(),
            );
        },
        move |outcome| {
            let ud = ud_final;
            let text = CString::new(outcome.text).unwrap_or_else(|_| CString::new("").unwrap());
            callback(
                ud.0,
                outcome.request_id,
                outcome.status,
                1, /* final */
                text.as_ptr(),
            );
        },
    )
}

/// # Safety
/// `session` is a valid pointer from `ds_session_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_session_cancel(session: *mut Session) {
    if let Some(s) = session_ref(session) {
        s.cancel_inflight();
    }
}

/// # Safety
/// `session` is a valid pointer from `ds_session_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_session_reset(session: *mut Session) {
    if let Some(s) = session_ref(session) {
        s.reset();
    }
}

/// Returns 1 when the current (uncommitted) buffer is at/over the configured
/// `max_context_tokens` budget, else 0. The frontend should flush (commit) and
/// start a fresh session before accepting more input so requests stay small.
///
/// # Safety
/// `session` is a valid pointer from `ds_session_new`.
#[no_mangle]
pub unsafe extern "C" fn ds_session_context_full(session: *mut Session) -> i32 {
    match session_ref(session) {
        Some(s) if s.context_full() => 1,
        _ => 0,
    }
}

// ---- Utilities -------------------------------------------------------------

/// # Safety
/// `s` was returned by a `ds_*` function documented as "caller frees".
#[no_mangle]
pub unsafe extern "C" fn ds_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

#[no_mangle]
pub extern "C" fn ds_last_error() -> *const c_char {
    LAST_ERROR.with(|e| e.borrow().as_ptr())
}

#[no_mangle]
pub extern "C" fn ds_version() -> *const c_char {
    concat!(env!("CARGO_PKG_VERSION"), "\0").as_ptr() as *const c_char
}
