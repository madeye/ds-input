# DS Input — LLM Whole‑Sentence Pinyin IME

Type a whole sentence in pinyin; an LLM converts it to Chinese inline. **No
candidate picking** — the model chooses the best sentence and you just press
Space/Enter to commit. Works with any **OpenAI‑compatible** API; defaults to
`deepseek-v4-flash`. macOS and Windows.

```
type:   nihaoshijie woshiyigechengxuyuan
commit: 你好世界，我是一个程序员
```

## Architecture (Rime‑style split)

| Layer | Path | Tech | Status |
|-------|------|------|--------|
| Shared core engine (C ABI) | [`core/`](core/) | Rust → `libdsime` cdylib/staticlib | ✅ builds, tested |
| macOS frontend | [`macos/`](macos/) | Swift + InputMethodKit | see `macos/README.md` |
| Windows frontend | [`windows/`](windows/) | C++ + Text Services Framework | see `windows/README.md` |

The core owns everything OS‑independent: the pinyin buffer state machine, the
async OpenAI‑compatible client, config load/save, and single‑in‑flight
cancellation. Frontends are thin: capture keys, render the inline pre‑edit,
commit text, host Settings. The one contract between them is
[`core/include/dsime.h`](core/include/dsime.h). See [`DESIGN.md`](DESIGN.md).

## Quick start (core)

```bash
cd core
cargo build --release          # produces target/release/libdsime.{dylib,a}
cargo test --lib               # unit tests
# end-to-end smoke test against a real provider:
DSIME_API_KEY=sk-... cargo run --example cli -- ni hao shi jie
#  → 你好世界
```

`cli` drives the exact FFI the platform frontends use, so it doubles as a
runnable reference. Override `DSIME_BASE_URL` / `DSIME_MODEL` to point at any
OpenAI‑compatible endpoint (OpenAI, OpenRouter, Azure, local Ollama/vLLM/LM
Studio).

## Configuration

Stored as JSON at `~/Library/Application Support/DSInput/config.json` (macOS) /
`%APPDATA%\DSInput\config.json` (Windows), and edited through each platform's
Settings window (single source of truth via `ds_engine_{get,set}_config_json`):

| Field | Default | Meaning |
|-------|---------|---------|
| `base_url` | `https://api.deepseek.com/v1` | OpenAI‑compatible endpoint |
| `api_key` | _(empty — set this!)_ | Bearer key |
| `model` | `deepseek-v4-flash` | Chat model id |
| `system_prompt` | _(pinyin→Chinese instruction)_ | Conversion behaviour |
| `temperature` | `0.3` | Lower = more deterministic |
| `max_tokens` | `256` | Cap per sentence |
| `timeout_ms` | `8000` | Per‑request network timeout |
| `debounce_ms` | `180` | Idle wait after last keystroke before converting |

## Building the frontends

- **macOS:** `cd macos && ./build.sh` → `DSInput.app`; copy to
  `~/Library/Input Methods/`, then enable in System Settings ▸ Keyboard ▸ Input
  Sources. Details in [`macos/README.md`](macos/README.md).
- **Windows:** `cd windows && ./build.ps1` (VS 2022 + Windows SDK + Rust msvc
  toolchain), `regsvr32` the DLL, then add the input method in Settings ▸ Time &
  Language ▸ Language. Details in [`windows/README.md`](windows/README.md).

## License

MIT.
