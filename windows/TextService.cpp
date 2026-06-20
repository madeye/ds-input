// TextService.cpp — lifecycle, IUnknown, sink wiring, and the marshaling window.
//
// See TextService.h for the threading/composition design. This file owns:
//   * IUnknown reference counting and QueryInterface across all the interfaces
//     CTextService implements.
//   * Activate/ActivateEx/Deactivate: create the core engine+session, advise all
//     sinks, register the display-attribute atom, build the language bar, and
//     stand up the hidden message-only window used for cross-thread marshaling.
//   * ITfThreadMgrEventSink + ITfThreadFocusSink stubs (we mostly care about
//     focus to drop a stale composition cleanly).
//   * The hidden window proc that receives WM_DSIME_CONVERT_RESULT from the core
//     worker thread and WM_DSIME_DEBOUNCE_FIRE from the timer.

#include "TextService.h"
#include "Globals.h"
#include "Guids.h"

#include <new>

// Class name for the hidden message-only window. Registered lazily.
static const wchar_t kMsgWndClass[] = L"DSInputMsgWnd";

CTextService::CTextService() {
    DllAddRef();  // the module stays loaded while any object is alive
}

CTextService::~CTextService() {
    // By the time we're destroyed Deactivate should already have run, but be
    // defensive: never leave the core or windows dangling.
    _DestroyMessageWindow();
    DllRelease();
}

// ---- IUnknown --------------------------------------------------------------

STDMETHODIMP CTextService::QueryInterface(REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) return E_INVALIDARG;
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfTextInputProcessor) ||
        IsEqualIID(riid, IID_ITfTextInputProcessorEx)) {
        *ppvObj = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink)) {
        *ppvObj = static_cast<ITfThreadMgrEventSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfThreadFocusSink)) {
        *ppvObj = static_cast<ITfThreadFocusSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfKeyEventSink)) {
        *ppvObj = static_cast<ITfKeyEventSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfCompositionSink)) {
        *ppvObj = static_cast<ITfCompositionSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider)) {
        *ppvObj = static_cast<ITfDisplayAttributeProvider*>(this);
    }

    if (*ppvObj) {
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CTextService::AddRef() {
    return ::InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG) CTextService::Release() {
    LONG cr = ::InterlockedDecrement(&_cRef);
    if (cr == 0) {
        delete this;
    }
    return cr;
}

// ---- Activation ------------------------------------------------------------

STDMETHODIMP CTextService::Activate(ITfThreadMgr* ptim, TfClientId tid) {
    return ActivateEx(ptim, tid, 0);
}

STDMETHODIMP CTextService::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD /*dwFlags*/) {
    _pThreadMgr = ptim;
    _pThreadMgr->AddRef();
    _tid = tid;

    // 1) Stand up the core engine (per-user config) and one session. If the
    //    engine fails to init we still activate so the IME doesn't vanish from
    //    the language list; conversion simply yields errors and we keep showing
    //    raw pinyin. (The Settings UI lets the user fix the config.)
    _engine.Create(nullptr);
    if (_engine.valid()) {
        _session.Create(_engine);
    }

    // 2) Hidden message-only window must exist before any conversion can post
    //    back to us. Created on THIS (the STA) thread, so its window proc runs
    //    here.
    if (!_CreateMessageWindow()) {
        goto fail;
    }

    // 3) Resolve our display-attribute GUID to an atom for fast tagging of the
    //    composition range.
    if (!_InitDisplayAttributeGuidAtom()) {
        // Non-fatal: composition still works, just without the underline.
    }

    // 4) Advise the sinks. Order doesn't matter much, but key-event last so we
    //    never receive a key before the rest of our state exists.
    if (!_InitThreadMgrEventSink()) goto fail;
    if (!_InitThreadFocusSink())    { /* non-fatal */ }
    if (!_InitKeyEventSink())       goto fail;

    // 5) Language-bar button that opens Settings. Non-fatal if it fails.
    _InitLanguageBar();

    return S_OK;

fail:
    Deactivate();
    return E_FAIL;
}

STDMETHODIMP CTextService::Deactivate() {
    // Tear down in reverse order of Activate. Each helper is idempotent.
    _CancelDebounce();
    _UninitLanguageBar();
    _UninitKeyEventSink();
    _UninitThreadFocusSink();
    _UninitThreadMgrEventSink();

    // Drop any live composition reference without trying to mutate the doc (the
    // host may be tearing down). TSF will clean the range up.
    if (_pComposition) {
        _pComposition->Release();
        _pComposition = nullptr;
    }
    if (_pCompositionContext) {
        _pCompositionContext->Release();
        _pCompositionContext = nullptr;
    }

    _DestroyMessageWindow();

    // Core teardown: session first (it holds a refcount on the engine), then
    // engine. RAII does this in member-destruction order too, but we release
    // explicitly here so a re-Activate starts clean.
    _session.reset();
    _engine.reset();

    _pinyin.clear();
    _displayText.clear();
    _showingConverted = false;
    _lastRequestId = 0;

    if (_pThreadMgr) {
        _pThreadMgr->Release();
        _pThreadMgr = nullptr;
    }
    _tid = TF_CLIENTID_NULL;
    return S_OK;
}

// ---- ITfThreadMgrEventSink -------------------------------------------------
// We don't need most of these, but a clean composition teardown on focus change
// avoids leaking a half-finished pre-edit into the wrong document.

STDMETHODIMP CTextService::OnInitDocumentMgr(ITfDocumentMgr*)   { return S_OK; }
STDMETHODIMP CTextService::OnUninitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
STDMETHODIMP CTextService::OnPushContext(ITfContext*)           { return S_OK; }
STDMETHODIMP CTextService::OnPopContext(ITfContext*)            { return S_OK; }

STDMETHODIMP CTextService::OnSetFocus(ITfDocumentMgr* /*pdimFocus*/,
                                ITfDocumentMgr* /*pdimPrevFocus*/) {
    // Focus moved to another document. If we were composing, abandon it: cancel
    // in-flight conversion and forget our buffer. We do not try to commit into
    // the old document here (it may be gone); TSF terminates the composition
    // and OnCompositionTerminated fires to release our reference.
    if (_HasComposition()) {
        _CancelDebounce();
        _session.Cancel();
        _ResetBuffer();
    }
    return S_OK;
}

// ---- ITfThreadFocusSink ----------------------------------------------------

STDMETHODIMP CTextService::OnSetThreadFocus()  { return S_OK; }
STDMETHODIMP CTextService::OnKillThreadFocus() {
    // Whole thread lost focus (app switch). Drop any in-flight conversion so a
    // late result doesn't pop into a background window.
    _CancelDebounce();
    _session.Cancel();
    return S_OK;
}

// ---- ITfCompositionSink ----------------------------------------------------

STDMETHODIMP CTextService::OnCompositionTerminated(TfEditCookie /*ecWrite*/,
                                             ITfComposition* pComposition) {
    // TSF (or the app) ended our composition out from under us. Release our
    // reference and reset state; do NOT issue further edits on this range.
    if (_pComposition == pComposition) {
        _CancelDebounce();
        _session.Cancel();
        if (_pComposition) {
            _pComposition->Release();
            _pComposition = nullptr;
        }
        if (_pCompositionContext) {
            _pCompositionContext->Release();
            _pCompositionContext = nullptr;
        }
        _ResetBuffer();
    }
    return S_OK;
}

// ---- sink advise/unadvise --------------------------------------------------

BOOL CTextService::_InitThreadMgrEventSink() {
    ITfSource* pSource = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource))))
        return FALSE;
    HRESULT hr = pSource->AdviseSink(IID_ITfThreadMgrEventSink,
                                     static_cast<ITfThreadMgrEventSink*>(this),
                                     &_dwThreadMgrEventSinkCookie);
    pSource->Release();
    return SUCCEEDED(hr);
}

void CTextService::_UninitThreadMgrEventSink() {
    if (_dwThreadMgrEventSinkCookie == TF_INVALID_COOKIE) return;
    ITfSource* pSource = nullptr;
    if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource)))) {
        pSource->UnadviseSink(_dwThreadMgrEventSinkCookie);
        pSource->Release();
    }
    _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
}

BOOL CTextService::_InitThreadFocusSink() {
    ITfSource* pSource = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource))))
        return FALSE;
    HRESULT hr = pSource->AdviseSink(IID_ITfThreadFocusSink,
                                     static_cast<ITfThreadFocusSink*>(this),
                                     &_dwThreadFocusSinkCookie);
    pSource->Release();
    return SUCCEEDED(hr);
}

void CTextService::_UninitThreadFocusSink() {
    if (_dwThreadFocusSinkCookie == TF_INVALID_COOKIE) return;
    ITfSource* pSource = nullptr;
    if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource)))) {
        pSource->UnadviseSink(_dwThreadFocusSinkCookie);
        pSource->Release();
    }
    _dwThreadFocusSinkCookie = TF_INVALID_COOKIE;
}

BOOL CTextService::_InitKeyEventSink() {
    ITfKeystrokeMgr* pKeyMgr = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr,
                                           reinterpret_cast<void**>(&pKeyMgr))))
        return FALSE;
    HRESULT hr = pKeyMgr->AdviseKeyEventSink(_tid,
                                             static_cast<ITfKeyEventSink*>(this),
                                             TRUE /*fForeground*/);
    pKeyMgr->Release();
    return SUCCEEDED(hr);
}

void CTextService::_UninitKeyEventSink() {
    ITfKeystrokeMgr* pKeyMgr = nullptr;
    if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr,
                                              reinterpret_cast<void**>(&pKeyMgr)))) {
        pKeyMgr->UnadviseKeyEventSink(_tid);
        pKeyMgr->Release();
    }
}

BOOL CTextService::_InitDisplayAttributeGuidAtom() {
    ITfCategoryMgr* pCat = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfCategoryMgr, reinterpret_cast<void**>(&pCat))))
        return FALSE;
    HRESULT hr = pCat->RegisterGUID(c_guidDsimeDisplayAttribute, &_gaDisplayAttribute);
    pCat->Release();
    return SUCCEEDED(hr) && _gaDisplayAttribute != TF_INVALID_GUIDATOM;
}

// ---- hidden marshaling window ----------------------------------------------

BOOL CTextService::_CreateMessageWindow() {
    // Register the window class once per process. GetClassInfo tells us if it's
    // already there (a second activation on the same thread, or a prior one).
    WNDCLASSEXW wc = {};
    if (!::GetClassInfoExW(g_hInst, kMsgWndClass, &wc)) {
        wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &CTextService::_MsgWndProc;
        wc.hInstance = g_hInst;
        wc.lpszClassName = kMsgWndClass;
        if (!::RegisterClassExW(&wc)) {
            // class may have been registered concurrently; tolerate that.
            if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return FALSE;
        }
    }

    // HWND_MESSAGE => a message-only window: never visible, no z-order, just a
    // thread-affine message sink. Perfect for marshaling.
    _msgWnd = ::CreateWindowExW(0, kMsgWndClass, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, g_hInst, nullptr);
    if (_msgWnd == nullptr) return FALSE;

    // Stash `this` so the static window proc can reach the instance.
    ::SetWindowLongPtrW(_msgWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return TRUE;
}

void CTextService::_DestroyMessageWindow() {
    if (_msgWnd) {
        ::SetWindowLongPtrW(_msgWnd, GWLP_USERDATA, 0);
        ::DestroyWindow(_msgWnd);
        _msgWnd = nullptr;
    }
}

LRESULT CALLBACK CTextService::_MsgWndProc(HWND hWnd, UINT msg,
                                           WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DSIME_CONVERT_RESULT) {
        // lParam owns a heap ConvertResult* posted from the core worker thread.
        // We are now on the STA thread, so it is safe to touch the composition.
        ConvertResult* r = reinterpret_cast<ConvertResult*>(lParam);
        if (r) {
            CTextService* self =
                reinterpret_cast<CTextService*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
            if (self) {
                self->_OnConvertResultOnStaThread(r->request_id, r->status, r->text);
            }
            // We hold a ref that was taken when the request was issued; release
            // it now that the round trip is complete.
            if (r->pThis) r->pThis->Release();
            delete r;
        }
        return 0;
    }
    if (msg == WM_DSIME_DEBOUNCE_FIRE) {
        CTextService* self =
            reinterpret_cast<CTextService*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        if (self) self->_FireConversion();
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
