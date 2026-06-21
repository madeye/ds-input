// resource.h — ids for the DS Input universal installer.
#pragma once

#define IDI_INSTALLER       1

#define IDD_INSTALLER       100
#define IDC_TITLE           101
#define IDC_BODY            102
#define IDC_STATUS          103
#define IDC_PROGRESS        104
// The action button is IDOK ("Install" → "Close"); IDCANCEL is the close box.

// Embedded payloads (RCDATA). One trio per architecture; the installer extracts
// only the set matching the host. A missing arch is simply absent at runtime.
#define IDR_X64_TSF         300
#define IDR_X64_CORE        301
#define IDR_X64_SETTINGS    302
#define IDR_ARM64_TSF       310
#define IDR_ARM64_CORE      311
#define IDR_ARM64_SETTINGS  312
