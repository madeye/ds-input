# DS Input — macOS IMKit Frontend

LLM-powered whole-sentence pinyin input method for macOS.
Types ASCII pinyin, waits for the LLM to convert it to Chinese,
shows the result as inline pre-edit text. Space/Return commits; Esc reverts.

## Requirements

- macOS 13+ (Ventura or later)
- Xcode Command Line Tools (`xcode-select --install`)
- Rust toolchain (`rustup` + stable)
- An OpenAI-compatible API key (DeepSeek by default)

## Build

```bash
# From the repo root:
bash macos/build.sh
```

The script:
1. Runs `cargo build --release` in `../core` to produce `libdsime.a`.
2. Compiles the three Swift source files with `swiftc`, statically linking
   `libdsime.a` plus the required system frameworks.
3. Assembles `macos/build/DSInput.app` and ad-hoc signs it.

Use `--debug` for an unoptimised debug build:

```bash
bash macos/build.sh --debug
```

## Install

Copy the app to the system Input Methods folder:

```bash
cp -R macos/build/DSInput.app ~/Library/Input\ Methods/
```

Then re-login or run:

```bash
/System/Library/CoreServices/pbs -flush
```

## Enable in System Settings

1. Open **System Settings** → **Keyboard** → **Input Sources**.
2. Click **+**.
3. Search for **Chinese (Simplified)** → select **DS Input**.
4. Click **Add**.

Switch to DS Input with the Input Sources menu in the menu bar (or the
keyboard shortcut you configure in System Settings).

## Configure

The first time you switch to DS Input, open **Preferences…** from the IME
menu in the menu bar:

| Field | Default | Notes |
|-------|---------|-------|
| Base URL | `https://api.deepseek.com/v1` | Any OpenAI-compatible endpoint |
| API Key | _(empty)_ | Required — enter your key here |
| Model | `deepseek-v4-flash` | Any model on the endpoint |
| Temperature | `0.3` | Lower = more deterministic |
| Max Tokens | `512` | Enough for a sentence |
| Timeout (ms) | `10000` | Network request timeout |
| Debounce (ms) | `180` | Idle wait before sending to LLM |
| System Prompt | _(built-in)_ | Instructs the model to emit Chinese only |

Config is stored at `~/Library/Application Support/DSInput/config.json`.

## Typing

1. Type toneless pinyin (e.g. `nihaoshijie`).
   The raw pinyin appears underlined immediately.
2. After the debounce idle (default 180 ms), the LLM converts it.
   The underlined text updates to the Chinese sentence (`你好世界`).
3. Press **Space** or **Return** to commit.
4. Press **Esc** to revert to raw pinyin (press again to clear).
5. **Backspace** edits the pinyin buffer and re-triggers conversion.

## Architecture

```
AppDelegate.swift           — NSApplication + IMKServer startup; holds shared DsEngine
DSInputController.swift     — IMKInputController subclass; owns one DsSession per client
PreferencesWindowController.swift — AppKit settings window backed by ds_engine_get/set_config_json
dsime_bridge.h              — Bridging header exposing core/include/dsime.h to Swift
```

The Rust core (`libdsime.a`) is statically linked — the distributed `.app`
has no external dylib dependencies beyond system frameworks.

## Troubleshooting

**Input method not listed in System Settings**
Run `killall pbs` and re-login. The `pbs` daemon caches the list of input methods.

**"DS Input" appears but typing does nothing**
Check Console.app for `[DSInput]` log entries. Ensure the API key is set.

**LLM conversion is slow**
Increase the Debounce value so conversion fires less often while you type.
Or switch to a faster model (e.g. `deepseek-v4-flash`).

**The app crashes on launch**
Run from Terminal to see the error:
```bash
~/Library/Input\ Methods/DSInput.app/Contents/MacOS/DSInput
```
