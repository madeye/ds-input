# DS Input — LLM Whole‑Sentence Pinyin Input Method

## Goal
A cross‑platform pinyin input method (IME) that converts a whole pinyin sentence
into Chinese using an **OpenAI‑compatible chat API** (default model
`deepseek-v4-flash`). The user **never picks a candidate** — they type pinyin,
the converted sentence appears inline as the pre‑edit, and Space/Enter commits it.
Providers/models/keys are user‑configurable in Settings.

## Why this architecture (Rime‑style split)
Rime ships one cross‑platform engine (`librime`) wrapped by thin native frontends
(`Squirrel` on macOS via InputMethodKit, `Weasel` on Windows via TSF). We reuse
that proven split, but replace the local dictionary engine with an LLM converter:

```
        ┌───────────────────────────────────────────────┐
        │  core/  — Rust crate `dsime` (cdylib+staticlib)│
        │  • session/buffer state machine                │
        │  • OpenAI‑compatible async client (reqwest)    │
        │  • config load/save (JSON)                     │
        │  • C ABI  →  core/include/dsime.h              │
        └───────────────┬───────────────────────────────┘
                        │ C FFI (stable, see dsime.h)
          ┌─────────────┴──────────────┐
          ▼                            ▼
  macos/  Swift + InputMethodKit   windows/  C++ + TSF (Text Services
  (Squirrel‑style IMKit app)       Framework) text service (Weasel‑style)
```

The core owns *all* logic that is not OS‑specific. Frontends only: capture keys,
render the pre‑edit / inline composition, commit text, and host the Settings UI.

## Input flow (no candidate selection)
1. User types ASCII pinyin → frontend appends to buffer → `ds_session_set_input`.
2. Frontend debounces (default 180 ms idle) then calls `ds_session_convert`.
3. Core sends the pinyin to the chat API; the model returns the best Chinese
   sentence. Core invokes the result callback (on a worker thread).
4. Frontend marshals to its UI thread and shows the sentence as the **pre‑edit**.
5. Space / Enter → commit the pre‑edit. Esc → revert to raw pinyin. Backspace
   edits the pinyin buffer and re‑triggers conversion.
6. While a request is in flight the raw pinyin stays visible (so typing never
   blocks on the network); the converted text replaces it when it arrives.

## Conversion prompt
System prompt instructs the model to treat the user message as toneless Hanyu
Pinyin (syllables possibly run together or separated by spaces/apostrophes) and
emit *only* the most natural Chinese sentence — no pinyin, no explanation, no
quotes. Latin words, digits and punctuation pass through. See `core/src/config.rs`
`DEFAULT_SYSTEM_PROMPT`.

## Default provider
- `base_url`: `https://api.deepseek.com/v1`
- `model`: `deepseek-v4-flash`
- `api_key`: empty (user must set it in Settings)
Any OpenAI‑compatible endpoint works (OpenAI, Azure, OpenRouter, local Ollama/
vLLM/LM Studio) by changing `base_url` + `model` + `api_key`.

## Config file
`~/Library/Application Support/io.DSInput.DSInput/config.json` (macOS) /
`%APPDATA%/DSInput/config.json` (Windows). Schema = `core::config::Config`.
The Settings UI reads/writes it via `ds_engine_get_config_json` /
`ds_engine_set_config_json` so there is a single source of truth.

## Threading contract
The result callback fires on a Tokio worker thread. Frontends MUST hop to the UI
thread (`DispatchQueue.main` / TSF UI thread) before touching composition state.
`ds_session_convert` cancels the previous in‑flight request for that session.

See `core/include/dsime.h` for the authoritative C ABI.
