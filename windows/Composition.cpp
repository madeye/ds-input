// Composition.cpp — composition orchestration, debounce, and conversion plumbing.
//
// This file ties the STA-thread composition state in CTextService to the edit
// sessions (EditSessions.cpp) and to the dsime core's async conversion.
//
// Flow recap (all on the STA thread except where noted):
//   _StartComposition       -> opens the ITfComposition (first pinyin key).
//   _UpdateCompositionText   -> rewrites the pre-edit (pinyin or converted).
//   _CommitComposition       -> writes final text and ends composition.
//   _EndComposition          -> ends composition with no/empty final text.
//   _ArmDebounce/_CancelDebounce -> thread-pool single-shot timer; on expiry it
//       PostMessages WM_DSIME_DEBOUNCE_FIRE to the STA thread (the timer runs on
//       a pool thread, so it must NOT touch TSF directly).
//   _FireConversion          -> STA thread: snapshot the buffer, AddRef self,
//       and ds_session_convert with _ConvertCallbackThunk as the C callback.
//   _ConvertCallbackThunk    -> CORE WORKER THREAD: package the result and
//       PostMessage WM_DSIME_CONVERT_RESULT back to the STA window.
//   _OnConvertResultOnStaThread -> STA thread: drop stale ids, else show the
//       converted sentence in the composition.

#include "TextService.h"
#include "Globals.h"

// Prototypes for the edit-session submitters defined in EditSessions.cpp.
HRESULT Dsime_RequestStartComposition(CTextService* pSvc, ITfContext* pic,
                                      TfClientId tid, ITfComposition** ppComp);
HRESULT Dsime_RequestSetText(CTextService* pSvc, ITfContext* pic, TfClientId tid,
                             ITfComposition* pComp, TfGuidAtom gaAttr,
                             const std::wstring& text, BOOL underline);
HRESULT Dsime_RequestEndComposition(CTextService* pSvc, ITfContext* pic,
                                    TfClientId tid, ITfComposition* pComp,
                                    const std::wstring& finalText, BOOL hasFinal);

// CTextService needs the display-attribute atom from a private member; expose a
// tiny accessor via friend-free indirection by reading it through a getter we
// add inline here. To keep the header lean we instead pass it through the
// methods below using a helper that reads the member. Since these methods are
// CTextService members, they can read _gaDisplayAttribute directly.

// ---- composition orchestration --------------------------------------------

HRESULT CTextService::_StartComposition(ITfContext* pic) {
    if (_HasComposition()) return S_OK;  // already composing

    ITfComposition* pComp = nullptr;
    HRESULT hr = Dsime_RequestStartComposition(this, pic, _tid, &pComp);
    if (FAILED(hr) || !pComp) return FAILED(hr) ? hr : E_FAIL;

    // Keep the composition and its owning context alive for the whole session.
    _pComposition = pComp;            // ownership transferred from edit session
    _pCompositionContext = pic;
    _pCompositionContext->AddRef();
    return S_OK;
}

HRESULT CTextService::_UpdateCompositionText(ITfContext* pic,
                                             const std::wstring& text,
                                             BOOL underline) {
    if (!_HasComposition()) return E_UNEXPECTED;
    return Dsime_RequestSetText(this, pic, _tid, _pComposition,
                                _gaDisplayAttribute, text, underline);
}

HRESULT CTextService::_CommitComposition(ITfContext* pic,
                                         const std::wstring& text) {
    if (!_HasComposition()) return S_OK;
    HRESULT hr = Dsime_RequestEndComposition(this, pic, _tid, _pComposition,
                                             text, TRUE /*hasFinal*/);
    // Release our hold on the composition/context; it is finished.
    if (_pComposition) { _pComposition->Release(); _pComposition = nullptr; }
    if (_pCompositionContext) {
        _pCompositionContext->Release();
        _pCompositionContext = nullptr;
    }
    return hr;
}

HRESULT CTextService::_EndComposition(ITfContext* pic) {
    if (!_HasComposition()) return S_OK;
    // End with the raw pinyin already shown (no replacement text): pass hasFinal
    // = FALSE so whatever is in the range stays, then composition ends. For the
    // Esc path the caller has already left the raw pinyin in the range.
    HRESULT hr = Dsime_RequestEndComposition(this, pic, _tid, _pComposition,
                                             std::wstring(), FALSE /*hasFinal*/);
    if (_pComposition) { _pComposition->Release(); _pComposition = nullptr; }
    if (_pCompositionContext) {
        _pCompositionContext->Release();
        _pCompositionContext = nullptr;
    }
    return hr;
}

void CTextService::_ResetBuffer() {
    _pinyin.clear();
    _displayText.clear();
    _showingConverted = false;
    _lastRequestId = 0;
    _session.Reset();  // clears core buffer + cancels in-flight
}

// ---- debounce timer (thread-pool, single-shot) -----------------------------

void CTextService::_ArmDebounce() {
    // Lazily create the timer object.
    if (_debounceTimer == nullptr) {
        _debounceTimer = ::CreateThreadpoolTimer(&CTextService::_DebounceTimerCallback,
                                                 this, nullptr);
        if (_debounceTimer == nullptr) {
            // Fallback: fire conversion immediately if we can't get a timer.
            _FireConversion();
            return;
        }
    }
    // Relative due time in 100ns units, negative => relative. debounce_ms from
    // the engine config (default 100ms).
    uint32_t ms = _engine.valid() ? _engine.DebounceMs() : 100;
    ULARGE_INTEGER due;
    due.QuadPart = static_cast<ULONGLONG>(-(static_cast<LONGLONG>(ms) * 10000LL));
    FILETIME ft;
    ft.dwLowDateTime = due.LowPart;
    ft.dwHighDateTime = due.HighPart;
    // window length 0, period 0 => single-shot. Re-arming replaces the pending
    // due time, which is exactly the debounce behaviour we want.
    ::SetThreadpoolTimer(_debounceTimer, &ft, 0, 0);
}

void CTextService::_CancelDebounce() {
    if (_debounceTimer) {
        // Cancel a pending fire (NULL due time) and wait for any running
        // callback to finish so it can't post after we tear down.
        ::SetThreadpoolTimer(_debounceTimer, nullptr, 0, 0);
        ::WaitForThreadpoolTimerCallbacks(_debounceTimer, TRUE);
    }
}

VOID CALLBACK CTextService::_DebounceTimerCallback(PTP_CALLBACK_INSTANCE,
                                                   PVOID ctx, PTP_TIMER) {
    // POOL THREAD: must not touch TSF/composition. Marshal to the STA thread by
    // posting to the hidden window; _FireConversion runs there.
    CTextService* self = static_cast<CTextService*>(ctx);
    if (self && self->_msgWnd) {
        ::PostMessageW(self->_msgWnd, WM_DSIME_DEBOUNCE_FIRE, 0, 0);
    }
}

// ---- conversion: issue request (STA thread) --------------------------------

void CTextService::_FireConversion() {
    // STA thread. Only convert if we still have a non-empty buffer and an active
    // composition (the user may have committed/cancelled while the timer ran).
    if (!_HasComposition() || _pinyin.empty() || !_session.valid()) return;

    // The core's set_input was already called on each keystroke; ensure the
    // latest buffer is what gets converted.
    _session.SetInput(_pinyin);

    // We hand a borrowed `this` to the worker thread via the callback's
    // user_data. To keep `this` alive until the TERMINAL result is delivered
    // (and its posted message processed), take one reference now; the terminal
    // (is_final=1) stream callback releases it. Partial updates do not.
    AddRef();

    // Stream the conversion so the pre-edit fills in incrementally (lower
    // perceived latency). Honors the `stream` config flag: when false the core
    // fires only the single terminal call. Reuses the engine's pooled, keep-alive
    // connection and DeepSeek's cached system-prompt prefix across keystrokes.
    uint64_t reqId = _session.ConvertStream(&CTextService::_StreamCallbackThunk, this);
    if (reqId == 0) {
        // Empty buffer per the core (shouldn't happen given the check above):
        // no callback will fire, so release the ref we just took.
        Release();
        return;
    }
    _lastRequestId = reqId;
}

// ---- regeneration: ask for a DIFFERENT candidate (STA thread) --------------

void CTextService::_FireRegenerate() {
    // STA thread. Like _FireConversion, but asks the core for an alternative
    // conversion that excludes the candidates already shown. The result streams
    // back through the same thunk/handler and replaces the pre-edit.
    if (!_HasComposition() || _pinyin.empty() || !_session.valid()) return;

    _session.SetInput(_pinyin);
    AddRef();  // terminal (is_final=1) stream callback releases it
    uint64_t reqId = _session.Regenerate(&CTextService::_StreamCallbackThunk, this);
    if (reqId == 0) {
        Release();
        return;
    }
    _lastRequestId = reqId;
}

// ---- conversion: core callback (WORKER THREAD) -----------------------------

void CTextService::_ConvertCallbackThunk(void* user_data, uint64_t request_id,
                                         int32_t status, const char* text_utf8) {
    // CORE WORKER THREAD. Do the minimum: copy out the (worker-owned, transient)
    // text, package it, and PostMessage to the STA window. NOTHING here touches
    // TSF or the composition.
    CTextService* self = static_cast<CTextService*>(user_data);
    if (self == nullptr) return;

    ConvertResult* r = new (std::nothrow) ConvertResult();
    if (r == nullptr) {
        // Can't deliver; release the ref taken in _FireConversion to avoid leak.
        self->Release();
        return;
    }
    r->pThis = self;          // carries the ref we took in _FireConversion
    r->request_id = request_id;
    r->status = status;
    r->text = (status == DS_OK) ? dsime::Utf8ToUtf16(text_utf8) : std::wstring();

    // If the window is gone (deactivated mid-flight), we still must release the
    // ref. PostMessage fails -> clean up here.
    HWND wnd = self->_msgWnd;
    if (wnd == nullptr || !::PostMessageW(wnd, WM_DSIME_CONVERT_RESULT, 0,
                                          reinterpret_cast<LPARAM>(r))) {
        self->Release();
        delete r;
    }
    // On success, ownership of `r` (and the ref) passes to the window proc.
}

// ---- streaming conversion: core callback (WORKER THREAD) --------------------

void CTextService::_StreamCallbackThunk(void* user_data, uint64_t request_id,
                                        int32_t status, int32_t is_final,
                                        const char* text_utf8) {
    // CORE WORKER THREAD. Package the update and PostMessage it to the STA window.
    // PARTIALS (is_final==0) carry NO ref and post WM_DSIME_CONVERT_PARTIAL; only
    // the TERMINAL call (is_final==1) carries the per-request ref and posts
    // WM_DSIME_CONVERT_RESULT (whose handler releases it). Partials are always
    // queued before the terminal call from this one worker task, so FIFO delivery
    // keeps `this` alive while partials are processed.
    CTextService* self = static_cast<CTextService*>(user_data);
    if (self == nullptr) return;

    ConvertResult* r = new (std::nothrow) ConvertResult();
    if (r == nullptr) {
        // Can't deliver. Only the terminal call owns the ref, so release it then.
        if (is_final) self->Release();
        return;
    }
    r->pThis = is_final ? self : nullptr;   // ref ownership only on the terminal call
    r->request_id = request_id;
    r->status = status;
    r->text = (status == DS_OK) ? dsime::Utf8ToUtf16(text_utf8) : std::wstring();

    HWND wnd = self->_msgWnd;
    UINT msg = is_final ? WM_DSIME_CONVERT_RESULT : WM_DSIME_CONVERT_PARTIAL;
    if (wnd == nullptr ||
        !::PostMessageW(wnd, msg, 0, reinterpret_cast<LPARAM>(r))) {
        if (is_final) self->Release();
        delete r;
    }
    // On success, ownership of `r` (and, for the terminal call, the ref) passes
    // to the window proc.
}

// ---- conversion: result handling (STA thread) ------------------------------

void CTextService::_OnConvertResultOnStaThread(uint64_t request_id, int32_t status,
                                               const std::wstring& text) {
    // Drop stale results: a newer request (or a commit/reset that zeroed
    // _lastRequestId) has superseded this one.
    if (request_id != _lastRequestId) return;

    // If the composition ended meanwhile, ignore.
    if (!_HasComposition() || _pCompositionContext == nullptr) return;

    if (status == DS_OK && !text.empty()) {
        // Replace the pre-edit with the converted Chinese sentence. Still
        // composing (underlined) — commit happens on Space/Enter.
        _displayText = text;
        _showingConverted = true;
        _UpdateCompositionText(_pCompositionContext, _displayText, TRUE);
    } else {
        // Error (or empty): keep showing the raw pinyin. Nothing to do because
        // the composition already displays it; we just don't switch to Chinese.
        // (Optionally surface ds_last_error in a future status UI.)
    }
}
