// EditSessions.cpp — ITfEditSession implementations.
//
// TSF forbids mutating a document directly. Instead you ask for an edit cookie
// by submitting an ITfEditSession to ITfContext::RequestEditSession; TSF calls
// your DoEditSession(ec) back with a cookie that authorizes reads/writes for the
// duration of that call. We use synchronous, read/write sessions
// (TF_ES_SYNC | TF_ES_READWRITE) requested from the STA thread, so the document
// is edited inline and the call returns once done.
//
// Three concrete sessions:
//   CStartCompositionEditSession — opens an ITfComposition at the selection.
//   CSetTextEditSession          — replaces the composition's text and applies
//                                  (or clears) the underline display attribute.
//   CEndCompositionEditSession   — writes optional final text and ends the
//                                  composition (the commit / cancel path).
//
// Each holds a borrowed ref on CTextService and the context; the orchestration
// in Composition.cpp owns lifetime.

#include "TextService.h"
#include "Globals.h"
#include "Guids.h"

// Helper: set the selection to a range (used to position the caret after edits).
static HRESULT SelectRange(TfEditCookie ec, ITfContext* pic, ITfRange* pRange) {
    TF_SELECTION sel;
    sel.range = pRange;
    sel.style.ase = TF_AE_NONE;
    sel.style.fInterimChar = FALSE;
    return pic->SetSelection(ec, 1, &sel);
}

// Apply our underline display attribute to a range, or clear it when
// fApply == FALSE. The atom comes from CTextService; passed in to avoid coupling.
static HRESULT ApplyDisplayAttribute(TfEditCookie ec, TfClientId tid,
                                     ITfContext* pic, ITfRange* pRange,
                                     TfGuidAtom gaAttr, BOOL fApply) {
    ITfProperty* pProp = nullptr;
    if (FAILED(pic->GetProperty(GUID_PROP_ATTRIBUTE, &pProp)) || !pProp)
        return E_FAIL;

    HRESULT hr;
    if (fApply && gaAttr != TF_INVALID_GUIDATOM) {
        VARIANT var;
        ::VariantInit(&var);
        var.vt = VT_I4;
        var.lVal = static_cast<LONG>(gaAttr);
        hr = pProp->SetValue(ec, pRange, &var);
    } else {
        hr = pProp->Clear(ec, pRange);
    }
    pProp->Release();
    return hr;
}

// ---------------------------------------------------------------------------
// CStartCompositionEditSession
// ---------------------------------------------------------------------------

class CStartCompositionEditSession final : public ITfEditSession {
public:
    CStartCompositionEditSession(CTextService* pSvc, ITfContext* pic,
                                 ITfComposition** ppCompositionOut)
        : _cRef(1), _pSvc(pSvc), _pic(pic), _ppCompositionOut(ppCompositionOut) {
        _pSvc->AddRef();
        _pic->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = ::InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        // Composition must start at the current insertion point. Grab the
        // selection's range and ask the context-composition service to open a
        // composition over it (empty for now; text is written separately).
        ITfContextComposition* pCtxComp = nullptr;
        if (FAILED(_pic->QueryInterface(IID_ITfContextComposition,
                                        reinterpret_cast<void**>(&pCtxComp))))
            return E_FAIL;

        HRESULT hr = E_FAIL;
        TF_SELECTION sel;
        ULONG fetched = 0;
        if (SUCCEEDED(_pic->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) &&
            fetched == 1) {
            ITfRange* pRangeStart = nullptr;
            if (SUCCEEDED(sel.range->Clone(&pRangeStart))) {
                // Collapse to the start so the composition has zero length; the
                // first SetText grows it.
                pRangeStart->Collapse(ec, TF_ANCHOR_START);
                ITfComposition* pComposition = nullptr;
                hr = pCtxComp->StartComposition(
                    ec, pRangeStart,
                    static_cast<ITfCompositionSink*>(_pSvc),
                    &pComposition);
                if (SUCCEEDED(hr) && pComposition) {
                    *_ppCompositionOut = pComposition;  // ownership to caller
                }
                pRangeStart->Release();
            }
            sel.range->Release();
        }
        pCtxComp->Release();
        return hr;
    }

private:
    ~CStartCompositionEditSession() { _pic->Release(); _pSvc->Release(); }
    LONG _cRef;
    CTextService* _pSvc;
    ITfContext* _pic;
    ITfComposition** _ppCompositionOut;
};

// ---------------------------------------------------------------------------
// CSetTextEditSession — replace composition text + (un)underline
// ---------------------------------------------------------------------------

class CSetTextEditSession final : public ITfEditSession {
public:
    CSetTextEditSession(CTextService* pSvc, ITfContext* pic,
                        ITfComposition* pComposition, TfClientId tid,
                        TfGuidAtom gaAttr, std::wstring text, BOOL underline)
        : _cRef(1), _pSvc(pSvc), _pic(pic), _pComposition(pComposition),
          _tid(tid), _gaAttr(gaAttr), _text(std::move(text)), _underline(underline) {
        _pSvc->AddRef();
        _pic->AddRef();
        _pComposition->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = ::InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        ITfRange* pRange = nullptr;
        if (FAILED(_pComposition->GetRange(&pRange)) || !pRange) return E_FAIL;

        // Overwrite the whole composition range with the new text. SetText on
        // the composition range replaces its content and grows/shrinks it.
        HRESULT hr = pRange->SetText(ec, 0, _text.c_str(),
                                     static_cast<LONG>(_text.length()));
        if (SUCCEEDED(hr)) {
            // Underline the pre-edit (or clear it). The range now spans the new
            // text because SetText adjusted its anchors.
            ApplyDisplayAttribute(ec, _tid, _pic, pRange, _gaAttr, _underline);

            // Park the caret at the end of the composition so further typing
            // visually appends.
            ITfRange* pCaret = nullptr;
            if (SUCCEEDED(pRange->Clone(&pCaret))) {
                pCaret->Collapse(ec, TF_ANCHOR_END);
                SelectRange(ec, _pic, pCaret);
                pCaret->Release();
            }
        }
        pRange->Release();
        return hr;
    }

private:
    ~CSetTextEditSession() {
        _pComposition->Release();
        _pic->Release();
        _pSvc->Release();
    }
    LONG _cRef;
    CTextService* _pSvc;
    ITfContext* _pic;
    ITfComposition* _pComposition;
    TfClientId _tid;
    TfGuidAtom _gaAttr;
    std::wstring _text;
    BOOL _underline;
};

// ---------------------------------------------------------------------------
// CEndCompositionEditSession — write final text (optional) + end composition
// ---------------------------------------------------------------------------

class CEndCompositionEditSession final : public ITfEditSession {
public:
    CEndCompositionEditSession(CTextService* pSvc, ITfContext* pic,
                               ITfComposition* pComposition,
                               std::wstring finalText, BOOL hasFinal)
        : _cRef(1), _pSvc(pSvc), _pic(pic), _pComposition(pComposition),
          _finalText(std::move(finalText)), _hasFinal(hasFinal) {
        _pSvc->AddRef();
        _pic->AddRef();
        _pComposition->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = ::InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        ITfRange* pRange = nullptr;
        if (FAILED(_pComposition->GetRange(&pRange)) || !pRange) {
            // Range gone; just end the composition.
            _pComposition->EndComposition(ec);
            return S_OK;
        }

        if (_hasFinal) {
            // Replace the pre-edit with the committed final text...
            pRange->SetText(ec, 0, _finalText.c_str(),
                            static_cast<LONG>(_finalText.length()));
        }
        // ...clear the underline attribute over the committed run so it reads as
        // ordinary text once composition ends.
        ITfProperty* pProp = nullptr;
        if (SUCCEEDED(_pic->GetProperty(GUID_PROP_ATTRIBUTE, &pProp)) && pProp) {
            pProp->Clear(ec, pRange);
            pProp->Release();
        }

        // Move the caret to the end of the committed text, then end composition.
        ITfRange* pCaret = nullptr;
        if (SUCCEEDED(pRange->Clone(&pCaret))) {
            pCaret->Collapse(ec, TF_ANCHOR_END);
            SelectRange(ec, _pic, pCaret);
            pCaret->Release();
        }
        pRange->Release();

        _pComposition->EndComposition(ec);
        return S_OK;
    }

private:
    ~CEndCompositionEditSession() {
        _pComposition->Release();
        _pic->Release();
        _pSvc->Release();
    }
    LONG _cRef;
    CTextService* _pSvc;
    ITfContext* _pic;
    ITfComposition* _pComposition;
    std::wstring _finalText;
    BOOL _hasFinal;
};

// ---------------------------------------------------------------------------
// Free functions that submit the sessions. Declared in Composition.cpp's view
// via these prototypes; kept here next to the session classes they build.
// ---------------------------------------------------------------------------

HRESULT Dsime_RequestStartComposition(CTextService* pSvc, ITfContext* pic,
                                      TfClientId tid, ITfComposition** ppComp) {
    *ppComp = nullptr;
    CStartCompositionEditSession* pES =
        new (std::nothrow) CStartCompositionEditSession(pSvc, pic, ppComp);
    if (!pES) return E_OUTOFMEMORY;
    HRESULT hrSession = S_OK;
    // Synchronous read/write session: edits happen inline before this returns.
    HRESULT hr = pic->RequestEditSession(tid, pES,
                                         TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
    pES->Release();
    if (FAILED(hr)) return hr;
    return hrSession;
}

HRESULT Dsime_RequestSetText(CTextService* pSvc, ITfContext* pic, TfClientId tid,
                             ITfComposition* pComp, TfGuidAtom gaAttr,
                             const std::wstring& text, BOOL underline) {
    if (!pComp) return E_UNEXPECTED;
    CSetTextEditSession* pES = new (std::nothrow)
        CSetTextEditSession(pSvc, pic, pComp, tid, gaAttr, text, underline);
    if (!pES) return E_OUTOFMEMORY;
    HRESULT hrSession = S_OK;
    HRESULT hr = pic->RequestEditSession(tid, pES,
                                         TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
    pES->Release();
    if (FAILED(hr)) return hr;
    return hrSession;
}

HRESULT Dsime_RequestEndComposition(CTextService* pSvc, ITfContext* pic,
                                    TfClientId tid, ITfComposition* pComp,
                                    const std::wstring& finalText, BOOL hasFinal) {
    if (!pComp) return S_OK;
    CEndCompositionEditSession* pES = new (std::nothrow)
        CEndCompositionEditSession(pSvc, pic, pComp, finalText, hasFinal);
    if (!pES) return E_OUTOFMEMORY;
    HRESULT hrSession = S_OK;
    HRESULT hr = pic->RequestEditSession(tid, pES,
                                         TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
    pES->Release();
    if (FAILED(hr)) return hr;
    return hrSession;
}
