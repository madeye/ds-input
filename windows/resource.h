// resource.h — resource ids shared by the .rc scripts and the C++ code.
#pragma once

// Icon for the IME profile + language-bar button (used by both the DLL and the
// settings exe). IDI_DSIME is also defined in Globals.h to the same value; keep
// them in sync.
#ifndef IDI_DSIME
#define IDI_DSIME 101
#endif

// ---- Settings dialog (DSInputSettings.exe) --------------------------------
#define IDD_SETTINGS        200

#define IDC_BASE_URL        201
#define IDC_API_KEY         202
#define IDC_MODEL           203
#define IDC_TEMPERATURE     204
#define IDC_MAX_TOKENS      205
#define IDC_TIMEOUT_MS      206
#define IDC_DEBOUNCE_MS     207
#define IDC_SYSTEM_PROMPT   208
#define IDC_CONFIG_PATH     209
#define IDC_STATUS          210
#define IDC_SAVE            211   // maps to IDOK in the dialog
#define IDC_TEST            212   // runs a sample conversion against current fields
