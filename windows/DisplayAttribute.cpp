// DisplayAttribute.cpp — ITfDisplayAttributeInfo + enumerator for the pre-edit.
//
// TSF asks a text service to describe the visual styles it tags ranges with via
// ITfDisplayAttributeProvider (implemented on CTextService). The provider hands
// back ITfDisplayAttributeInfo objects keyed by GUID. We expose exactly one: a
// blue single underline for the in-progress composition (the classic IME look).
//
// CDisplayAttributeInfo is immutable except for an optional user override that
// TSF lets the user set; we honor SetAttributeInfo so the host's "edit display
// attributes" UI can round-trip, but reset just restores our default.

#include "TextService.h"
#include "Guids.h"
#include "Globals.h"

#include <new>

// Our one and only display attribute: blue text color cue + single underline.
static const TF_DISPLAYATTRIBUTE kComposingAttr = {
    { TF_CT_NONE, 0 },                 // text color: leave to app default
    { TF_CT_NONE, 0 },                 // background color: none
    TF_LS_SOLID,                        // line style: solid underline
    FALSE,                              // not bold underline
    { TF_CT_COLOR, RGB(0x33, 0x66, 0xCC) },  // underline color: blue-ish
    TF_ATTR_INPUT                       // semantic: input (composing) text
};

static const wchar_t kAttrDesc[] = L"DS Input Composing";

// ---------------------------------------------------------------------------
// CDisplayAttributeInfo
// ---------------------------------------------------------------------------

class CDisplayAttributeInfo final : public ITfDisplayAttributeInfo {
public:
    CDisplayAttributeInfo() : _cRef(1), _attr(kComposingAttr) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ITfDisplayAttributeInfo)) {
            *ppv = static_cast<ITfDisplayAttributeInfo*>(this);
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

    STDMETHODIMP GetGUID(GUID* pguid) override {
        if (!pguid) return E_INVALIDARG;
        *pguid = c_guidDsimeDisplayAttribute;
        return S_OK;
    }

    STDMETHODIMP GetDescription(BSTR* pbstr) override {
        if (!pbstr) return E_INVALIDARG;
        *pbstr = ::SysAllocString(kAttrDesc);
        return *pbstr ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* pda) override {
        if (!pda) return E_INVALIDARG;
        *pda = _attr;
        return S_OK;
    }

    STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE* pda) override {
        if (!pda) return E_INVALIDARG;
        _attr = *pda;  // honor a host/user override
        return S_OK;
    }

    STDMETHODIMP Reset() override {
        _attr = kComposingAttr;
        return S_OK;
    }

private:
    ~CDisplayAttributeInfo() = default;
    LONG _cRef;
    TF_DISPLAYATTRIBUTE _attr;
};

// ---------------------------------------------------------------------------
// CEnumDisplayAttributeInfo — single-element enumerator
// ---------------------------------------------------------------------------

class CEnumDisplayAttributeInfo final : public IEnumTfDisplayAttributeInfo {
public:
    CEnumDisplayAttributeInfo() : _cRef(1), _index(0) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_IEnumTfDisplayAttributeInfo)) {
            *ppv = static_cast<IEnumTfDisplayAttributeInfo*>(this);
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

    STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** ppEnum) override {
        if (!ppEnum) return E_INVALIDARG;
        CEnumDisplayAttributeInfo* p = new (std::nothrow) CEnumDisplayAttributeInfo();
        if (!p) return E_OUTOFMEMORY;
        p->_index = _index;
        *ppEnum = p;
        return S_OK;
    }

    STDMETHODIMP Next(ULONG ulCount, ITfDisplayAttributeInfo** rgInfo,
                      ULONG* pcFetched) override {
        ULONG fetched = 0;
        // We expose exactly one attribute (index 0).
        while (fetched < ulCount && _index < 1) {
            CDisplayAttributeInfo* p = new (std::nothrow) CDisplayAttributeInfo();
            if (!p) break;
            rgInfo[fetched] = p;  // already AddRef'd (ctor cRef=1)
            ++fetched;
            ++_index;
        }
        if (pcFetched) *pcFetched = fetched;
        return (fetched == ulCount) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Reset() override { _index = 0; return S_OK; }

    STDMETHODIMP Skip(ULONG ulCount) override {
        _index += ulCount;
        if (_index > 1) _index = 1;
        return S_OK;
    }

private:
    ~CEnumDisplayAttributeInfo() = default;
    LONG _cRef;
    ULONG _index;
};

// ---------------------------------------------------------------------------
// ITfDisplayAttributeProvider on CTextService
// ---------------------------------------------------------------------------

STDMETHODIMP CTextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) {
    if (!ppEnum) return E_INVALIDARG;
    *ppEnum = new (std::nothrow) CEnumDisplayAttributeInfo();
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CTextService::GetDisplayAttributeInfo(REFGUID guid,
                                             ITfDisplayAttributeInfo** ppInfo) {
    if (!ppInfo) return E_INVALIDARG;
    *ppInfo = nullptr;
    if (IsEqualGUID(guid, c_guidDsimeDisplayAttribute)) {
        *ppInfo = new (std::nothrow) CDisplayAttributeInfo();
        return *ppInfo ? S_OK : E_OUTOFMEMORY;
    }
    return E_INVALIDARG;
}
