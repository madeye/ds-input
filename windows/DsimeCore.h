// DsimeCore.h — thin, header-only C++ RAII wrapper over the dsime C ABI.
//
// The C contract (core/include/dsime.h) is authoritative; this wrapper adds no
// behaviour, only ownership/lifetime safety and UTF-8<->UTF-16 helpers. We use
// the EXACT symbol names and signatures declared in dsime.h — see that header
// for documented semantics, threading rules, and who-frees-what.
//
// Ownership model the TSF service uses (per the team-lead brief):
//   * ONE DsEngine for the whole text-service activation (created in
//     CTextService::Activate, freed in Deactivate). Internally synchronized, so
//     it can be shared by sessions and read from the Settings UI.
//   * ONE DsSession per activation (the IME tracks a single composition at a
//     time). Calls into a DsSession must be serialized onto the TSF UI/STA
//     thread; the only thing that touches it off-thread is the *core*, never us.
//
// All strings crossing the boundary are UTF-8. TSF speaks UTF-16, so convert at
// the edge with the Utf8/Utf16 helpers below.

#pragma once

#include <windows.h>
#include <string>
#include <utility>

// The authoritative C ABI. Relative include so the windows/ tree is
// self-contained against the in-repo core header.
extern "C" {
#include "../core/include/dsime.h"
}

namespace dsime {

// ---- UTF conversion helpers ----------------------------------------------

// UTF-8 (from core) -> UTF-16 (for TSF / Win32). Returns empty string on bad
// input rather than throwing; the IME must never crash the host.
inline std::wstring Utf8ToUtf16(const char* utf8) {
    if (utf8 == nullptr || *utf8 == '\0') return std::wstring();
    int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');  // -1 drops the NUL
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), needed);
    return out;
}

inline std::wstring Utf8ToUtf16(const std::string& utf8) {
    return Utf8ToUtf16(utf8.c_str());
}

// UTF-16 (TSF / Win32) -> UTF-8 (for core).
inline std::string Utf16ToUtf8(const wchar_t* utf16) {
    if (utf16 == nullptr || *utf16 == L'\0') return std::string();
    int needed = ::WideCharToMultiByte(CP_UTF8, 0, utf16, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(static_cast<size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, utf16, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

inline std::string Utf16ToUtf8(const std::wstring& utf16) {
    return Utf16ToUtf8(utf16.c_str());
}

// ---- RAII guard for a "caller frees" char* returned by the core ----------

// Wraps a char* that dsime.h documents as caller-owned and releases it with
// ds_string_free. Move-only.
class CoreString {
public:
    CoreString() = default;
    explicit CoreString(char* owned) : p_(owned) {}
    ~CoreString() { reset(); }

    CoreString(const CoreString&) = delete;
    CoreString& operator=(const CoreString&) = delete;

    CoreString(CoreString&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    CoreString& operator=(CoreString&& other) noexcept {
        if (this != &other) { reset(); p_ = other.p_; other.p_ = nullptr; }
        return *this;
    }

    const char* c_str() const { return p_ ? p_ : ""; }
    bool empty() const { return p_ == nullptr || *p_ == '\0'; }
    std::wstring to_wstring() const { return Utf8ToUtf16(c_str()); }
    std::string  to_string()  const { return std::string(c_str()); }

    void reset() {
        if (p_) { ds_string_free(p_); p_ = nullptr; }
    }

private:
    char* p_ = nullptr;
};

// ---- Engine wrapper -------------------------------------------------------

// Owns a DsEngine*. Created once per text-service activation.
class Engine {
public:
    Engine() = default;
    ~Engine() { reset(); }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&& o) noexcept : e_(o.e_) { o.e_ = nullptr; }
    Engine& operator=(Engine&& o) noexcept {
        if (this != &o) { reset(); e_ = o.e_; o.e_ = nullptr; }
        return *this;
    }

    // config_path == nullptr uses the per-user default
    // (%APPDATA%/DSInput/config.json). Returns false on fatal init error; call
    // LastError() for a message.
    bool Create(const char* config_path = nullptr) {
        reset();
        e_ = ds_engine_new(config_path);
        return e_ != nullptr;
    }

    bool valid() const { return e_ != nullptr; }
    DsEngine* raw() const { return e_; }

    // Idle debounce (ms) the frontend should wait after the last keystroke.
    uint32_t DebounceMs() const { return e_ ? ds_engine_debounce_ms(e_) : 180; }

    CoreString GetConfigJson() const {
        return CoreString(e_ ? ds_engine_get_config_json(e_) : nullptr);
    }
    int32_t SetConfigJson(const char* json_utf8) {
        return e_ ? ds_engine_set_config_json(e_, json_utf8) : DS_ERR_CONFIG;
    }
    int32_t ReloadConfig() {
        return e_ ? ds_engine_reload_config(e_) : DS_ERR_CONFIG;
    }
    CoreString ConfigPath() const {
        return CoreString(e_ ? ds_engine_config_path(e_) : nullptr);
    }

    void reset() {
        if (e_) { ds_engine_free(e_); e_ = nullptr; }
    }

private:
    DsEngine* e_ = nullptr;
};

// ---- Session wrapper ------------------------------------------------------

// Owns a DsSession*. One per activation. Not internally synchronized: every
// method here must be called from the TSF UI/STA thread.
class Session {
public:
    Session() = default;
    ~Session() { reset(); }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // The engine must outlive the session (the core bumps the engine refcount,
    // but keep the C++ Engine object alive too for clean teardown order).
    bool Create(const Engine& engine) {
        reset();
        if (!engine.valid()) return false;
        s_ = ds_session_new(engine.raw());
        return s_ != nullptr;
    }

    bool valid() const { return s_ != nullptr; }
    DsSession* raw() const { return s_; }

    // Replace the raw pinyin buffer with the whole string typed so far.
    void SetInput(const std::string& pinyin_utf8) {
        if (s_) ds_session_set_input(s_, pinyin_utf8.c_str());
    }

    CoreString GetInput() const {
        return CoreString(s_ ? ds_session_get_input(s_) : nullptr);
    }

    // True when the current buffer is at/over the configured context-token budget;
    // the frontend should flush (commit) and start fresh before adding more input.
    bool ContextFull() const {
        return s_ ? ds_session_context_full(s_) != 0 : false;
    }

    // Kick off async conversion. callback fires on a CORE WORKER THREAD; it must
    // marshal to the UI thread before touching composition state. Returns a
    // monotonic request id, or 0 if the buffer is empty (no callback then).
    uint64_t Convert(DsConvertCallback callback, void* user_data) {
        return s_ ? ds_session_convert(s_, callback, user_data) : 0;
    }

    // Like Convert, but streams: callback fires with is_final=0 for each partial
    // (cumulative) update, then exactly once with is_final=1 for the terminal
    // outcome. Honors the `stream` config flag. Returns a request id, or 0 if the
    // buffer is empty (then no callback fires).
    uint64_t ConvertStream(DsStreamCallback callback, void* user_data) {
        return s_ ? ds_session_convert_stream(s_, callback, user_data) : 0;
    }

    void Cancel() { if (s_) ds_session_cancel(s_); }
    void Reset()  { if (s_) ds_session_reset(s_); }

    void reset() {
        if (s_) { ds_session_free(s_); s_ = nullptr; }
    }

private:
    DsSession* s_ = nullptr;
};

// Thread-local last-error string from the core, as UTF-16.
inline std::wstring LastError() {
    return Utf8ToUtf16(ds_last_error());
}

}  // namespace dsime
