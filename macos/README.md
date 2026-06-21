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
2. Compiles the Objective-C frontend with `clang`, statically linking
   `libdsime.a` plus the required system frameworks.
3. Assembles `macos/build/DSInput.app` and signs it with
   `DSInput.entitlements`.

Use `--debug` for an unoptimised debug build:

```bash
bash macos/build.sh --debug
```

## Xcode

Generate the local Xcode project with XcodeGen:

```bash
cd macos
xcodegen generate --spec project.yml
xed DSInput.xcodeproj
```

The generated `DSInput.xcodeproj` is intentionally ignored by git. The project
builds the Rust core in a pre-build phase, links `libdsime.a`, and signs debug
builds ad-hoc with `DSInput.entitlements` for local development. Use `build.sh`
for Developer ID signing and notarized installable builds.

## Install

Copy the app to the system Input Methods folder:

```bash
cp -R macos/build/DSInput.app ~/Library/Input\ Methods/
```

Then **log out and back in** (required for a first install — the input-source
picker is only repopulated at login; `pbs -flush` alone does not pick up a
brand-new input method).

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
main.m                      — NSApplication entry point
DSInputAppDelegate.m        — IMKServer startup; holds shared DsEngine
DSInputController.m         — IMKInputController subclass; owns one DsSession per client
PreferencesWindowController.m — AppKit settings window backed by ds_engine_get/set_config_json
DSInputShared.m             — shared Rust engine pointer + UTF-8 helpers
```

The Rust core (`libdsime.a`) is statically linked — the distributed `.app`
has no external dylib dependencies beyond system frameworks.

## Troubleshooting

**Input method not listed in System Settings**
On macOS 13+ (strictly enforced on macOS 26) the system only surfaces an input
method in the picker if its bundle is **Developer-ID signed AND notarized**. An
ad-hoc signed bundle launches fine and `TISRegisterInputSource()` even returns
`noErr`, but it is silently never shown. Make sure `build.sh` signed + notarized
the app (`spctl -a -t exec DSInput.app` should say "accepted / Notarized
Developer ID"), then **log out and back in** — newly installed input sources are
only scanned into the picker at login. `killall pbs` / `pbs -flush` alone is not
enough for a brand-new input source.

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
