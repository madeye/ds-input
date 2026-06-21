# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

DS Input is an LLM whole-sentence pinyin IME. The user types toneless ASCII pinyin
(`nihaoshijie`); an OpenAI-compatible chat model converts the *whole sentence* to
Chinese (`你好世界`) and shows it inline as the pre-edit. **There is no candidate
window / no candidate picking** — Space/Enter commits, Esc reverts to raw pinyin.

## Architecture (Rime-style split)

One cross-platform Rust engine wrapped by thin native frontends — mirroring
librime + Squirrel (macOS) / Weasel (Windows).

```
core/    Rust crate `dsime` → libdsime (cdylib + staticlib).  Owns ALL
         OS-independent logic: pinyin buffer state machine, OpenAI-compatible
         async client (reqwest+tokio), JSON config load/save, single-in-flight
         cancellation.  Exposes a C ABI.
   │ C FFI — the ONE contract, authoritatively defined in core/include/dsime.h
   ├── macos/    Objective-C + InputMethodKit (IMKit) app bundle, DSInput.app
   └── windows/  C++ + Text Services Framework (TSF) in-proc COM server, dsime_tsf.dll
```

Frontends only: capture keys, render the inline pre-edit, commit text, host
Settings. They are deliberately thin — when adding behavior, prefer putting logic
in `core/` so both platforms get it. `core/include/dsime.h` is the source of truth
for the boundary; `core/examples/cli.rs` drives that exact FFI and doubles as a
runnable reference frontend.

### Key invariants of the C ABI (don't break these)

- **Exactly-once callback**: every `ds_session_convert` that returns a non-zero id
  invokes its callback exactly once, on a worker thread — even when superseded
  (then with `DS_ERR_CANCELLED`). Frontends tie per-request resources (e.g. a
  retained context pointer) to this guarantee; see the macOS `__bridge_transfer`
  in the convert callback. The `engine.rs` `convert()` `tokio::select!` + `active_gen`
  generation counter implements it.
- **Threading**: the result callback fires on a Tokio worker thread. Frontends MUST
  hop to the UI thread (`dispatch_get_main_queue` on macOS; a message-only window
  `PostMessage` on the TSF STA thread) before touching composition state. A single
  `DsSession` must be called from one thread at a time; `DsEngine` is internally
  synchronized and shared across sessions.
- **Strings**: all UTF-8, NUL-terminated. Anything documented "caller frees" must go
  through `ds_string_free`.
- **Status codes** (`DS_OK`, `DS_ERR_*`) in `dsime.h` must stay in sync with
  `api.rs::ConvertError::status_code()` and `lib.rs` — there's a test asserting this.

## Common commands

Core (run from `core/`):
```bash
cargo build --release            # libdsime.{dylib,a}; also via build.sh/build.ps1
cargo test --all                 # unit tests + tests/mock_server.rs integration test
cargo test --lib                 # unit tests only
# end-to-end against a real provider (also the reference frontend):
DSIME_API_KEY=sk-... cargo run --example cli -- ni hao shi jie
# override DSIME_BASE_URL / DSIME_MODEL to target any OpenAI-compatible endpoint
```

CI (`.github/workflows/ci.yml`) gates strictly on the core across macOS/Windows/Linux;
match it before pushing:
```bash
cargo fmt --all -- --check
cargo clippy --all-targets -- -D warnings
cargo test --all
```
The frontend build jobs are `continue-on-error` (WIP), but the core jobs are not.

macOS frontend:
```bash
bash macos/build.sh              # builds core + clang-compiles ObjC, assembles DSInput.app
bash macos/build.sh --debug      # unoptimized; ad-hoc sign
# Xcode project is git-ignored; regenerate with XcodeGen:
cd macos && xcodegen generate --spec project.yml
```

Windows frontend (run on Windows, from a *x64 Native Tools Command Prompt for VS 2022*):
```powershell
cd windows; ./build.ps1          # core dsime.dll + CMake builds dsime_tsf.dll + DSInputSettings.exe
```
The Windows sources are authored/reviewed on macOS and **cannot be compiled here** —
treat any change as unverified until a Windows build + smoke test passes.

## Things that bite

- **macOS IME registration — the `.inputmethod.` rule (hard-won)**: the bundle id
  and EVERY `TISInputSourceID` must contain a `.inputmethod.` segment, e.g.
  `io.github.madeye.inputmethod.dsinput[.pinyin]`. macOS only enrolls a bundle as a
  keyboard input method when its identifier follows this convention (every Apple IME,
  WeType, vChewing do). Without it, `TISRegisterInputSource()` returns `noErr` but the
  source NEVER appears in Settings — regardless of signing, notarization, or location.
  This silently cost hours; do not drop the segment.
  Other requirements: the bundle must be **Developer ID-signed** (ad-hoc launches but
  never surfaces), and `Resources/menu_icon.pdf` must exist (referenced by
  `tsInputMethodIconFileKey`). After install you must **log out and back in** — the
  input-source scan runs at login; `pbs -flush` won't pick up a brand-new source.
  The picker label comes from `Resources/<lang>.lproj/InfoPlist.strings` keyed by the
  `TISInputSourceID` (otherwise the picker shows the raw id).
- **Notarization is for DISTRIBUTION, not local testing**: a locally-built, locally-
  installed bundle carries no `com.apple.quarantine` flag, so Gatekeeper never checks
  it and it registers/runs fine un-notarized. `build.sh` skips notarization by default;
  pass `--notarize` (needs `SIGN_IDENTITY` + a notarytool key) only when shipping to
  other machines.
- **macOS linking**: the app `-force_load`s `libdsime.a` and links the system
  frameworks the Rust archive pulls in transitively (Security, CFNetwork,
  SystemConfiguration via reqwest→rustls, etc.) — configured in `project.yml`
  (`OTHER_LDFLAGS` + the target's `dependencies:`). If you add a Cargo dependency that
  needs a new framework, add it there too.
- **Windows DLL co-location**: `dsime_tsf.dll` and `DSInputSettings.exe` load
  `dsime.dll` at runtime — all three must live in the same folder, and `regsvr32`
  records the exact path it was registered from.

## Config

Single source of truth: a JSON file (`~/Library/Application Support/.../config.json`
on macOS, `%APPDATA%\DSInput\config.json` on Windows) matching `core::config::Config`.
Both Settings UIs read/write it *only* through `ds_engine_get_config_json` /
`ds_engine_set_config_json` — never parse or write the file from a frontend. Defaults
target DeepSeek (`https://api.deepseek.com/v1`, `deepseek-v4-flash`); the API key is
empty and the user must set it. The conversion behavior lives in
`config.rs::DEFAULT_SYSTEM_PROMPT`.
