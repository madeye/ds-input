// Guids.h — stable GUIDs that identify this text service to TSF and the OS.
//
// These values are baked into the registry by DllRegisterServer and are how
// Windows finds and loads the IME. They MUST NOT change once shipped, otherwise
// existing user profiles / language-bar entries would dangle. If you fork this
// project, regenerate every GUID below with `uuidgen` so two builds never claim
// the same identity.
//
// We declare the GUIDs `extern` here and *define* them exactly once in
// Guids.cpp via the DEFINE_GUID machinery, so every translation unit shares one
// instance and the linker is happy.

#pragma once

// IMPORTANT: do NOT include <initguid.h> here. DEFINE_GUID expands to a plain
// `extern const GUID` declaration *unless* <initguid.h> was included earlier in
// the same translation unit, in which case it expands to a definition. Exactly
// one .cpp (Guids.cpp) includes <initguid.h> before this header, so the storage
// is emitted exactly once. Every other TU sees only declarations.
#include <guiddef.h>

// {6F3D9A21-7C44-4E1B-9C2A-1B2C3D4E5F60}
// CLSID of the text-input processor (the COM object regsvr32 registers and that
// TSF instantiates). This is "our" class id.
DEFINE_GUID(c_clsidDsimeTextService,
    0x6f3d9a21, 0x7c44, 0x4e1b, 0x9c, 0x2a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x60);

// {A1B2C3D4-55E6-47F8-8901-23456789ABCD}
// GUID of the input profile (language profile) under our CLSID. A single CLSID
// can expose several profiles; we expose exactly one (zh-Hans whole-sentence
// pinyin).
DEFINE_GUID(c_guidDsimeProfile,
    0xa1b2c3d4, 0x55e6, 0x47f8, 0x89, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd);

// {B2C3D4E5-66F7-4809-9A12-3456789ABCDE}
// GUID for our composition display attribute (the underline style applied to the
// in-progress pre-edit text). Registered with the display-attribute category and
// returned by our ITfDisplayAttributeProvider.
DEFINE_GUID(c_guidDsimeDisplayAttribute,
    0xb2c3d4e5, 0x66f7, 0x4809, 0x9a, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde);

// {C3D4E5F6-7708-491A-AB23-456789ABCDEF}
// GUID identifying our language-bar item button (the system-tray / language-bar
// entry that opens Settings).
DEFINE_GUID(c_guidDsimeLangBarItem,
    0xc3d4e5f6, 0x7708, 0x491a, 0xab, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef);

// BCP-47 language tag for the profile. zh-Hans → LANGID 0x0804 (Simplified,
// PRC). TSF profiles are keyed by LANGID; 0x0804 is zh-CN.
#define DSIME_LANGID  0x0804
