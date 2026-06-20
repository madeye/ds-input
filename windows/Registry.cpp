// Registry.cpp — COM-server + TSF registration.
//
// COM server registration is plain registry writes under HKCR\CLSID. TSF
// registration goes through the documented COM objects
// (ITfInputProcessorProfiles, ITfCategoryMgr) rather than raw keys, so it stays
// correct across Windows versions and immersive/secure contexts.
//
// Categories we register (per the brief):
//   GUID_TFCAT_TIP_KEYBOARD                  — we are a keyboard TIP.
//   GUID_TFCAT_TIPCAP_UIELEMENTENABLED       — we draw our own UI elements.
//   GUID_TFCAT_TIPCAP_SECUREMODE             — usable on the secure desktop.
//   GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT       — works in immersive (UWP) apps.
//   GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT         — shows in the system tray.
//   GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER      — we provide display attributes.

#include "Registry.h"
#include "Guids.h"
#include "Globals.h"

#include <msctf.h>
#include <strsafe.h>
#include <olectl.h>

// ---- small helpers ---------------------------------------------------------

// Format a GUID as "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}".
static BOOL GuidToString(REFGUID guid, wchar_t* buf, size_t cch) {
    return ::StringFromGUID2(guid, buf, static_cast<int>(cch)) > 0;
}

static LONG SetStringValue(HKEY hKey, const wchar_t* name, const wchar_t* value) {
    return ::RegSetValueExW(hKey, name, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value),
                            static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
}

// ---- COM server registration ----------------------------------------------

BOOL RegisterComServer(HINSTANCE hInst, REFCLSID clsid, const wchar_t* desc) {
    wchar_t clsidStr[64];
    if (!GuidToString(clsid, clsidStr, ARRAYSIZE(clsidStr))) return FALSE;

    wchar_t modulePath[MAX_PATH];
    if (::GetModuleFileNameW(hInst, modulePath, ARRAYSIZE(modulePath)) == 0)
        return FALSE;

    // HKCR\CLSID\{clsid}
    wchar_t keyPath[128];
    ::StringCchPrintfW(keyPath, ARRAYSIZE(keyPath), L"CLSID\\%s", clsidStr);

    HKEY hKey = nullptr;
    if (::RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, nullptr,
                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                          &hKey, nullptr) != ERROR_SUCCESS)
        return FALSE;
    SetStringValue(hKey, nullptr, desc);

    // ...\InprocServer32 = <dll path>, ThreadingModel = Apartment.
    HKEY hSub = nullptr;
    BOOL ok = FALSE;
    if (::RegCreateKeyExW(hKey, L"InprocServer32", 0, nullptr,
                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                          &hSub, nullptr) == ERROR_SUCCESS) {
        SetStringValue(hSub, nullptr, modulePath);
        SetStringValue(hSub, L"ThreadingModel", L"Apartment");
        ::RegCloseKey(hSub);
        ok = TRUE;
    }
    ::RegCloseKey(hKey);
    return ok;
}

BOOL UnregisterComServer(REFCLSID clsid) {
    wchar_t clsidStr[64];
    if (!GuidToString(clsid, clsidStr, ARRAYSIZE(clsidStr))) return FALSE;
    wchar_t keyPath[128];
    ::StringCchPrintfW(keyPath, ARRAYSIZE(keyPath), L"CLSID\\%s", clsidStr);
    // RegDeleteTree removes the key and all subkeys (InprocServer32).
    ::RegDeleteTreeW(HKEY_CLASSES_ROOT, keyPath);
    return TRUE;
}

// ---- TSF profile -----------------------------------------------------------

BOOL RegisterTsfProfile() {
    ITfInputProcessorProfiles* pProfiles = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfiles,
                                  reinterpret_cast<void**>(&pProfiles))))
        return FALSE;

    BOOL ok = FALSE;
    if (SUCCEEDED(pProfiles->Register(c_clsidDsimeTextService))) {
        wchar_t modulePath[MAX_PATH];
        ::GetModuleFileNameW(g_hInst, modulePath, ARRAYSIZE(modulePath));

        const wchar_t* desc = DSIME_DESC_W;
        HRESULT hr = pProfiles->AddLanguageProfile(
            c_clsidDsimeTextService,
            DSIME_LANGID,
            c_guidDsimeProfile,
            desc, static_cast<ULONG>(wcslen(desc)),
            modulePath,                       // icon file: this DLL
            static_cast<ULONG>(-IDI_DSIME),   // negative => resource id
            0);
        ok = SUCCEEDED(hr);
    }
    pProfiles->Release();
    return ok;
}

BOOL UnregisterTsfProfile() {
    ITfInputProcessorProfiles* pProfiles = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfiles,
                                  reinterpret_cast<void**>(&pProfiles))))
        return FALSE;
    pProfiles->Unregister(c_clsidDsimeTextService);
    pProfiles->Release();
    return TRUE;
}

// ---- TSF categories --------------------------------------------------------

BOOL RegisterTsfCategories() {
    ITfCategoryMgr* pCat = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_TF_CategoryMgr, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                                  reinterpret_cast<void**>(&pCat))))
        return FALSE;

    static const GUID* kCategories[] = {
        &GUID_TFCAT_TIP_KEYBOARD,
        &GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
        &GUID_TFCAT_TIPCAP_SECUREMODE,
        &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
        &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
        &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    };

    BOOL ok = TRUE;
    for (const GUID* cat : kCategories) {
        if (FAILED(pCat->RegisterCategory(c_clsidDsimeTextService, *cat,
                                          c_clsidDsimeTextService)))
            ok = FALSE;
    }
    pCat->Release();
    return ok;
}

BOOL UnregisterTsfCategories() {
    ITfCategoryMgr* pCat = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_TF_CategoryMgr, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                                  reinterpret_cast<void**>(&pCat))))
        return FALSE;

    static const GUID* kCategories[] = {
        &GUID_TFCAT_TIP_KEYBOARD,
        &GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
        &GUID_TFCAT_TIPCAP_SECUREMODE,
        &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
        &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
        &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    };
    for (const GUID* cat : kCategories) {
        pCat->UnregisterCategory(c_clsidDsimeTextService, *cat,
                                 c_clsidDsimeTextService);
    }
    pCat->Release();
    return TRUE;
}
