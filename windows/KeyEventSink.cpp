// KeyEventSink.cpp — ITfKeyEventSink: decide which keys we eat and act on them.
//
// TSF calls OnTestKeyDown first to ask "would you consume this key?" without
// side effects, then OnKeyDown to actually handle it. Both must agree, so they
// share _IsKeyEaten for the decision. The real work lives in _HandleKey, which
// drives the composition (start / update / commit / cancel).
//
// Input alphabet (no candidate UI — the whole design): a-z and apostrophe build
// the pinyin buffer. Conversion is automatic (an idle debounce after the last
// keystroke). Space does 分词: it inserts a word-boundary space into the pinyin
// and re-converts; a DOUBLE space (two in a row) confirms/commits — falling back
// to the raw pinyin if no conversion has landed yet, so text can always be
// output. Enter commits immediately. Esc reverts/cancels. Backspace edits.
// Everything else passes through to the app, after committing any pending
// composition so the pre-edit isn't stranded.

#include "TextService.h"
#include "Globals.h"

namespace {

// Is this a pinyin-building character? Lower-case latin letters and the
// apostrophe (syllable separator, e.g. xi'an).
bool IsPinyinChar(WPARAM vk, wchar_t ch) {
    if (vk >= 'A' && vk <= 'Z') return true;          // letters (VK is upper)
    if (ch == L'\'') return true;                       // apostrophe
    return false;
}

// Translate a VK + keyboard state into the produced character, honoring the
// current Shift/CapsLock so we can tell letters apart and reject shifted
// punctuation we don't want. Returns 0 if it doesn't map to a useful char.
wchar_t VkToChar(WPARAM vk, LPARAM /*lParam*/) {
    BYTE keyState[256];
    if (!::GetKeyboardState(keyState)) return 0;
    wchar_t buf[4] = {};
    UINT scan = 0;  // ToUnicode tolerates 0 scan code for our purposes
    int n = ::ToUnicode(static_cast<UINT>(vk), scan, keyState, buf, 4, 0);
    if (n == 1) return buf[0];
    return 0;
}

// Modifier keys held? We only want bare keys (no Ctrl/Alt) to feed the buffer,
// so Ctrl+C etc. always pass through to the app.
bool CtrlOrAltDown() {
    return (::GetKeyState(VK_CONTROL) & 0x8000) ||
           (::GetKeyState(VK_MENU) & 0x8000);
}

// Map an ASCII punctuation char to its full-width (全角) equivalent, or 0 if it
// isn't one we remap. \uXXXX escapes keep this independent of the source-file
// encoding (the MSVC build doesn't pass /utf-8).
wchar_t FullWidthPunct(wchar_t ch) {
    switch (ch) {
        case L',':  return 0xFF0C;  // ，
        case L'.':  return 0x3002;  // 。
        case L'?':  return 0xFF1F;  // ？
        case L'!':  return 0xFF01;  // ！
        case L';':  return 0xFF1B;  // ；
        case L':':  return 0xFF1A;  // ：
        case L'(':  return 0xFF08;  // （
        case L')':  return 0xFF09;  // ）
        case L'\\': return 0x3001;  // 、
        default:    return 0;
    }
}

}  // namespace

// ---- ITfKeyEventSink::OnSetFocus (foreground/background) -------------------

STDMETHODIMP CTextService::OnSetFocus(BOOL /*fForeground*/) {
    // Distinct from ITfThreadMgrEventSink::OnSetFocus; this one just tells us
    // whether our key sink is foreground. Nothing to do.
    return S_OK;
}

// ---- decision: would we eat this key? --------------------------------------

BOOL CTextService::_IsKeyEaten(ITfContext* /*pic*/, WPARAM wParam, LPARAM lParam) {
    // Never intercept while a modifier is down — let shortcuts through.
    if (CtrlOrAltDown()) return FALSE;

    switch (wParam) {
        case VK_SPACE:
        case VK_RETURN:
        case VK_ESCAPE:
        case VK_BACK:
            // Only meaningful while we have something composing. With an empty
            // buffer, let the app handle Space/Enter/Backspace normally.
            return _HasComposition() ? TRUE : FALSE;
        case VK_UP:
        case VK_DOWN:
        case VK_TAB:
            // Up/down (and Tab = next) cycle through LLM candidates, but only once
            // a converted sentence is shown; otherwise let the key reach the app.
            return (_HasComposition() && _showingConverted) ? TRUE : FALSE;
        default:
            break;
    }

    wchar_t ch = VkToChar(wParam, lParam);
    if (IsPinyinChar(wParam, ch)) {
        // We only consume lower-case latin (no Shift) so users can still type
        // capitals / shifted symbols verbatim into the app if they want. But
        // once we ARE composing, also eat letters typed with Shift so the
        // buffer stays coherent; simplest rule: eat any bare a-z / apostrophe.
        return TRUE;
    }
    // Punctuation we remap to full-width (全角) is eaten whenever the IME is on.
    if (FullWidthPunct(ch) != 0) return TRUE;
    return FALSE;
}

// ---- test phase (no side effects) ------------------------------------------

STDMETHODIMP CTextService::OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                                   BOOL* pfEaten) {
    *pfEaten = _IsKeyEaten(pic, wParam, lParam);
    return S_OK;
}

STDMETHODIMP CTextService::OnTestKeyUp(ITfContext* /*pic*/, WPARAM /*wParam*/,
                                 LPARAM /*lParam*/, BOOL* pfEaten) {
    // We act on key-down only; report not-eaten so key-up flows to the app.
    *pfEaten = FALSE;
    return S_OK;
}

STDMETHODIMP CTextService::OnKeyUp(ITfContext* /*pic*/, WPARAM /*wParam*/,
                             LPARAM /*lParam*/, BOOL* pfEaten) {
    *pfEaten = FALSE;
    return S_OK;
}

STDMETHODIMP CTextService::OnPreservedKey(ITfContext* /*pic*/, REFGUID /*rguid*/,
                                    BOOL* pfEaten) {
    *pfEaten = FALSE;
    return S_OK;
}

// ---- handle phase (the real work) ------------------------------------------

STDMETHODIMP CTextService::OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                               BOOL* pfEaten) {
    if (!_IsKeyEaten(pic, wParam, lParam)) {
        *pfEaten = FALSE;
        return S_OK;
    }
    *pfEaten = TRUE;
    return _HandleKey(pic, wParam, lParam, pfEaten);
}

HRESULT CTextService::_HandleKey(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                                 BOOL* pfEaten) {
    switch (wParam) {
        case VK_SPACE: {
            // Space does 分词 (insert a word-boundary space and re-convert); a
            // DOUBLE space confirms. We detect the second of two consecutive
            // spaces by a trailing space already sitting in the buffer.
            if (!_pinyin.empty() && _pinyin.back() == ' ') {
                // Second consecutive space -> commit. Prefer the converted
                // sentence; otherwise fall back to the raw pinyin (boundary
                // spaces trimmed) so text is always output, even with no/slow
                // conversion.
                std::wstring committed;
                if (_showingConverted) {
                    committed = _displayText;
                } else {
                    std::string trimmed = _pinyin;
                    while (!trimmed.empty() && trimmed.back() == ' ')
                        trimmed.pop_back();
                    committed = dsime::Utf8ToUtf16(trimmed);
                }
                HRESULT hr = _CommitComposition(pic, committed);
                _ResetBuffer();
                return hr;
            }
            // First space: append a 分词 boundary and re-arm auto-conversion.
            // Keep showing the current conversion (if any) until the new result
            // lands — a trailing boundary space doesn't change the sentence, and
            // this lets a quick double-space commit the Chinese, not the pinyin.
            _pinyin.push_back(' ');
            _session.SetInput(_pinyin);
            if (!_showingConverted) {
                _displayText = dsime::Utf8ToUtf16(_pinyin);
                HRESULT hr = _UpdateCompositionText(pic, _displayText, TRUE);
                _ArmDebounce();
                return hr;
            }
            _ArmDebounce();
            return S_OK;
        }
        case VK_RETURN: {
            // Enter always commits whatever is currently shown: the converted
            // Chinese if a conversion has landed, else the raw pinyin (so the user
            // can take the letters verbatim without converting). Then reset.
            std::wstring committed = _showingConverted ? _displayText
                                                        : dsime::Utf8ToUtf16(_pinyin);
            HRESULT hr = _CommitComposition(pic, committed);
            _ResetBuffer();
            return hr;
        }
        case VK_ESCAPE: {
            // Revert: drop the conversion and end the composition writing the
            // raw pinyin (so the user can keep editing/retyping outside the IME)
            // — or, if you prefer cancel semantics, end with empty text. We end
            // with the raw pinyin to avoid silently eating the user's keystrokes.
            _CancelDebounce();
            _session.Cancel();
            HRESULT hr = _EndComposition(pic);  // empty -> just tears down range
            _ResetBuffer();
            (void)hr;
            return S_OK;
        }
        case VK_UP:
        case VK_DOWN:
        case VK_TAB: {
            // Cycle alternative LLM conversions of the current input. A cached
            // candidate shows instantly; going down (or Tab) past the last asks the
            // core to regenerate a different one (streamed back via the usual
            // handler); going up past the primary is a no-op (but still eaten).
            int32_t dir = (wParam == VK_UP) ? -1 : 1;
            dsime::CoreString alt = _session.CandidateCached(dir);
            if (!alt.empty()) {
                _displayText = alt.to_wstring();
                _showingConverted = true;
                return _UpdateCompositionText(pic, _displayText, TRUE);
            }
            if (dir > 0) _FireRegenerate();
            return S_OK;
        }
        case VK_BACK: {
            // Edit the buffer: drop the last pinyin char. If that empties it,
            // end the composition; otherwise re-show pinyin and re-arm convert.
            if (!_pinyin.empty()) {
                _pinyin.pop_back();
            }
            if (_pinyin.empty()) {
                _session.Cancel();
                _CancelDebounce();
                HRESULT hr = _EndComposition(pic);
                _ResetBuffer();
                (void)hr;
                return S_OK;
            }
            _session.SetInput(_pinyin);
            _showingConverted = false;
            _displayText = dsime::Utf8ToUtf16(_pinyin);
            HRESULT hr = _UpdateCompositionText(pic, _displayText, TRUE);
            _ArmDebounce();  // re-convert after the edit
            return hr;
        }
        default: {
            wchar_t ch = VkToChar(wParam, lParam);

            // Full-width punctuation (全角): a punctuation key commits whatever is
            // composing (with the symbol appended), or inserts the symbol on its
            // own when idle. The conversion is deterministic and local.
            if (wchar_t full = FullWidthPunct(ch)) {
                std::wstring tail(1, full);
                if (_HasComposition()) {
                    std::string py = _pinyin;
                    while (!py.empty() && py.back() == ' ') py.pop_back();
                    std::wstring base = _showingConverted ? _displayText
                                                          : dsime::Utf8ToUtf16(py);
                    HRESULT hr = _CommitComposition(pic, base + tail);
                    _ResetBuffer();
                    return hr;
                }
                HRESULT hr = _StartComposition(pic);
                if (FAILED(hr)) { *pfEaten = FALSE; return hr; }
                hr = _CommitComposition(pic, tail);
                _ResetBuffer();
                return hr;
            }

            // A pinyin-building character. Append the produced char (lower-cased
            // letter or apostrophe) to the buffer.
            char ascii = 0;
            if (ch >= L'A' && ch <= L'Z') ascii = static_cast<char>(ch - L'A' + 'a');
            else if (ch >= L'a' && ch <= L'z') ascii = static_cast<char>(ch);
            else if (ch == L'\'') ascii = '\'';
            else {
                // Defensive: VK said letter but ToUnicode didn't agree. Derive
                // from VK directly.
                if (wParam >= 'A' && wParam <= 'Z')
                    ascii = static_cast<char>(wParam - 'A' + 'a');
            }
            if (ascii == 0) { *pfEaten = FALSE; return S_OK; }

            // Context cap: if the uncommitted buffer is already at the token
            // budget, flush it (commit the current Chinese, else the raw pinyin)
            // and start a fresh composition so the next request stays small. The
            // char being typed begins the new buffer.
            if (_HasComposition() && _session.ContextFull()) {
                std::wstring committed = _showingConverted
                                             ? _displayText
                                             : dsime::Utf8ToUtf16(_pinyin);
                _CommitComposition(pic, committed);
                _ResetBuffer();
            }

            if (!_HasComposition()) {
                HRESULT hr = _StartComposition(pic);
                if (FAILED(hr)) { *pfEaten = FALSE; return hr; }
            }
            _pinyin.push_back(ascii);
            _session.SetInput(_pinyin);

            // Show the raw pinyin immediately so typing never blocks on the
            // network; the auto-conversion replaces it when it arrives.
            _showingConverted = false;
            _displayText = dsime::Utf8ToUtf16(_pinyin);
            HRESULT hr = _UpdateCompositionText(pic, _displayText, TRUE);
            _ArmDebounce();
            return hr;
        }
    }
}
