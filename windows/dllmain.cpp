// dllmain.cpp — COM in-proc server entry points and the class factory.
//
// A TSF text service is a regular in-proc COM server. The loader needs four
// exports (also listed in dsime_tsf.def):
//   DllGetClassObject  — hand back a class factory for our CLSID.
//   DllCanUnloadNow    — say whether the DLL can be unloaded (no live objects).
//   DllRegisterServer  — write the COM + TSF registration (regsvr32).
//   DllUnregisterServer— undo it (regsvr32 /u).
//
// The class factory (CClassFactory) creates CTextService instances. We only
// support our one CLSID; everything else is CLASS_E_CLASSNOTAVAILABLE.

#include <windows.h>
#include <msctf.h>
#include <new>

#include "Globals.h"
#include "Guids.h"
#include "TextService.h"
#include "Registry.h"

HINSTANCE g_hInst = nullptr;
LONG      g_cRefDll = 0;

// ---------------------------------------------------------------------------
// CClassFactory
// ---------------------------------------------------------------------------

class CClassFactory final : public IClassFactory {
public:
    CClassFactory() : _cRef(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
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

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid,
                                void** ppvObj) override {
        if (!ppvObj) return E_INVALIDARG;
        *ppvObj = nullptr;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;  // no aggregation support

        CTextService* pSvc = new (std::nothrow) CTextService();  // cRef starts at 1
        if (!pSvc) return E_OUTOFMEMORY;

        HRESULT hr = pSvc->QueryInterface(riid, ppvObj);
        pSvc->Release();  // QI took its own ref (or failed)
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) DllAddRef(); else DllRelease();
        return S_OK;
    }

private:
    ~CClassFactory() = default;
    LONG _cRef;
};

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID /*lpReserved*/) {
    switch (dwReason) {
        case DLL_PROCESS_ATTACH:
            g_hInst = hInstance;
            ::DisableThreadLibraryCalls(hInstance);  // we don't need thread notices
            break;
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// COM exports
// ---------------------------------------------------------------------------

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;

    if (!IsEqualCLSID(rclsid, c_clsidDsimeTextService))
        return CLASS_E_CLASSNOTAVAILABLE;

    CClassFactory* pFactory = new (std::nothrow) CClassFactory();
    if (!pFactory) return E_OUTOFMEMORY;

    HRESULT hr = pFactory->QueryInterface(riid, ppv);
    pFactory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    // Unload only when no objects or class-factory locks remain.
    return (g_cRefDll <= 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    // 1) Register the COM server (InprocServer32 -> this DLL, threading model
    //    Apartment) under our CLSID.
    if (!RegisterComServer(g_hInst, c_clsidDsimeTextService, DSIME_DESC_W))
        return E_FAIL;

    // 2) Register the TSF profile + categories (keyboard, UI-element-enabled,
    //    secure mode, immersive/systray support).
    if (!RegisterTsfProfile() || !RegisterTsfCategories()) {
        // Roll back the COM registration so we don't leave a half state.
        UnregisterTsfCategories();
        UnregisterTsfProfile();
        UnregisterComServer(c_clsidDsimeTextService);
        return E_FAIL;
    }
    return S_OK;
}

STDAPI DllUnregisterServer() {
    // Reverse order of registration; ignore individual failures so a partial
    // prior state still gets cleaned as much as possible.
    UnregisterTsfCategories();
    UnregisterTsfProfile();
    UnregisterComServer(c_clsidDsimeTextService);
    return S_OK;
}
