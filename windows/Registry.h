// Registry.h — registration helpers used by DllRegisterServer/Unregister.
//
// Split into two concerns:
//   * Plain COM server registration under HKCR\CLSID\{clsid}\InprocServer32.
//   * TSF-specific registration via the ITfInputProcessorProfiles +
//     ITfCategoryMgr COM APIs (the correct, forward-compatible way — never hand
//     -write the TSF registry keys).

#pragma once

#include <windows.h>

// COM server (InprocServer32) registration.
BOOL RegisterComServer(HINSTANCE hInst, REFCLSID clsid, const wchar_t* desc);
BOOL UnregisterComServer(REFCLSID clsid);

// TSF profile (language profile under our CLSID, with icon + display name).
BOOL RegisterTsfProfile();
BOOL UnregisterTsfProfile();

// TSF categories (keyboard TIP + capability flags).
BOOL RegisterTsfCategories();
BOOL UnregisterTsfCategories();
