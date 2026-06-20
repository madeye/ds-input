// Globals.h — process-wide constants and the DLL module ref counter.
//
// A TSF text service is an in-proc COM server. Two counters govern its life:
//   * g_cRefDll  — number of live COM objects we've handed out + class-factory
//                  locks. DllCanUnloadNow returns S_OK only when this hits 0,
//                  letting the loader unload us.
//   * g_hInst    — our module handle, captured in DllMain, needed to load icons
//                  and to write the InprocServer32 path during registration.

#pragma once

#include <windows.h>

extern HINSTANCE g_hInst;     // module handle (set in DllMain)
extern LONG      g_cRefDll;   // outstanding object + lock count

inline void DllAddRef()     { ::InterlockedIncrement(&g_cRefDll); }
inline void DllRelease()    { ::InterlockedDecrement(&g_cRefDll); }

// Human-readable names shown in the language list / language bar.
#define DSIME_DESC_W      L"DS Input (LLM Pinyin)"
#define DSIME_DESC_A       "DS Input (LLM Pinyin)"

// Settings executable launched from the language-bar menu. Looked up next to
// the DLL (same install directory).
#define DSIME_SETTINGS_EXE L"DSInputSettings.exe"

// Resource ids (see resource.h / dsime_tsf.rc).
#define IDI_DSIME         101
