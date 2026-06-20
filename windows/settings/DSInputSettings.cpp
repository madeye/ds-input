// DSInputSettings.cpp — standalone Win32 settings dialog for DS Input.
//
// A tiny separate process (DSInputSettings.exe) launched from the IME's
// language-bar menu. It shares the SAME core as the text service, so it reads
// and writes the SAME config file via ds_engine_get_config_json /
// ds_engine_set_config_json — there is exactly one source of truth.
//
// Why a separate exe (not in-proc UI): a TSF text service is loaded into every
// app that takes input; popping a real settings window from inside that host is
// awkward and risky. A standalone process keeps the IME DLL lean and the UI
// robust.
//
// JSON handling: the core hands us a JSON *object* string matching
// core::config::Config and expects the same shape back. To avoid pulling in a
// JSON library we do minimal, targeted parsing (find a key, read its value) and
// rebuild the object from the field values with proper escaping. The core
// fills any omitted field from its defaults, so we always send the full set.

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <cstdlib>   // strtol
#include <cctype>    // isdigit

#include "../resource.h"
#include "../DsimeCore.h"

#pragma comment(lib, "comctl32.lib")

// ---- minimal JSON helpers --------------------------------------------------

namespace {

// Escape a UTF-8 string for embedding in a JSON string literal.
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    wsprintfA(buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Find a top-level string value: "key": "value". Returns unescaped value.
// Good enough for the flat Config object the core emits (pretty-printed).
std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return std::string();
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return std::string();
    size_t q = json.find('"', colon + 1);
    if (q == std::string::npos) return std::string();
    std::string out;
    for (size_t i = q + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            char n = json[++i];
            switch (n) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '"': out += '"';  break;
                case '\\': out += '\\'; break;
                case '/': out += '/';  break;
                case 'u': {
                    if (i + 4 < json.size()) {
                        std::string hex = json.substr(i + 1, 4);
                        int cp = static_cast<int>(strtol(hex.c_str(), nullptr, 16));
                        i += 4;
                        // Encode the (BMP) code point as UTF-8.
                        if (cp < 0x80) {
                            out += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                }
                default: out += n; break;
            }
        } else if (c == '"') {
            break;  // end of string
        } else {
            out += c;
        }
    }
    return out;
}

// Find a top-level numeric value: "key": 123(.45). Returns the raw token text.
std::string JsonGetNumber(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return std::string();
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return std::string();
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    std::string num;
    while (i < json.size() &&
           (isdigit(static_cast<unsigned char>(json[i])) || json[i] == '.' ||
            json[i] == '-' || json[i] == '+' || json[i] == 'e' || json[i] == 'E')) {
        num += json[i++];
    }
    return num;
}

// ---- dialog field <-> control glue ----------------------------------------

std::wstring GetText(HWND dlg, int id) {
    HWND h = ::GetDlgItem(dlg, id);
    int len = ::GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(len), L'\0');
    if (len > 0) ::GetWindowTextW(h, s.data(), len + 1);
    return s;
}

void SetText(HWND dlg, int id, const std::wstring& s) {
    ::SetDlgItemTextW(dlg, id, s.c_str());
}

// The engine is shared by the dialog proc via a single global for simplicity
// (this is a one-window, one-engine process).
dsime::Engine g_engine;

void LoadIntoDialog(HWND dlg) {
    dsime::CoreString json = g_engine.GetConfigJson();
    std::string j = json.to_string();

    SetText(dlg, IDC_BASE_URL,      dsime::Utf8ToUtf16(JsonGetString(j, "base_url")));
    SetText(dlg, IDC_API_KEY,       dsime::Utf8ToUtf16(JsonGetString(j, "api_key")));
    SetText(dlg, IDC_MODEL,         dsime::Utf8ToUtf16(JsonGetString(j, "model")));
    SetText(dlg, IDC_SYSTEM_PROMPT, dsime::Utf8ToUtf16(JsonGetString(j, "system_prompt")));
    SetText(dlg, IDC_TEMPERATURE,   dsime::Utf8ToUtf16(JsonGetNumber(j, "temperature")));
    SetText(dlg, IDC_MAX_TOKENS,    dsime::Utf8ToUtf16(JsonGetNumber(j, "max_tokens")));
    SetText(dlg, IDC_TIMEOUT_MS,    dsime::Utf8ToUtf16(JsonGetNumber(j, "timeout_ms")));
    SetText(dlg, IDC_DEBOUNCE_MS,   dsime::Utf8ToUtf16(JsonGetNumber(j, "debounce_ms")));

    dsime::CoreString path = g_engine.ConfigPath();
    SetText(dlg, IDC_CONFIG_PATH, L"Config: " + path.to_wstring());
}

// Build the JSON object from the dialog fields and persist it.
bool SaveFromDialog(HWND dlg, std::wstring* errOut) {
    std::string base_url   = dsime::Utf16ToUtf8(GetText(dlg, IDC_BASE_URL));
    std::string api_key    = dsime::Utf16ToUtf8(GetText(dlg, IDC_API_KEY));
    std::string model      = dsime::Utf16ToUtf8(GetText(dlg, IDC_MODEL));
    std::string prompt     = dsime::Utf16ToUtf8(GetText(dlg, IDC_SYSTEM_PROMPT));
    std::string temp       = dsime::Utf16ToUtf8(GetText(dlg, IDC_TEMPERATURE));
    std::string max_tokens = dsime::Utf16ToUtf8(GetText(dlg, IDC_MAX_TOKENS));
    std::string timeout    = dsime::Utf16ToUtf8(GetText(dlg, IDC_TIMEOUT_MS));
    std::string debounce   = dsime::Utf16ToUtf8(GetText(dlg, IDC_DEBOUNCE_MS));

    // Default numeric fields if the user blanked them, so the JSON stays valid.
    if (temp.empty())       temp = "0.3";
    if (max_tokens.empty()) max_tokens = "256";
    if (timeout.empty())    timeout = "8000";
    if (debounce.empty())   debounce = "180";

    std::string json;
    json += "{\n";
    json += "  \"base_url\": \""      + JsonEscape(base_url) + "\",\n";
    json += "  \"api_key\": \""       + JsonEscape(api_key)  + "\",\n";
    json += "  \"model\": \""         + JsonEscape(model)    + "\",\n";
    json += "  \"system_prompt\": \"" + JsonEscape(prompt)   + "\",\n";
    json += "  \"temperature\": "     + temp       + ",\n";
    json += "  \"max_tokens\": "      + max_tokens + ",\n";
    json += "  \"timeout_ms\": "      + timeout    + ",\n";
    json += "  \"debounce_ms\": "     + debounce   + "\n";
    json += "}\n";

    int32_t rc = g_engine.SetConfigJson(json.c_str());
    if (rc != DS_OK) {
        if (errOut) *errOut = dsime::LastError();
        return false;
    }
    return true;
}

INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM /*lParam*/) {
    switch (msg) {
        case WM_INITDIALOG: {
            // Icon on the dialog.
            HICON hIcon = ::LoadIconW(::GetModuleHandleW(nullptr),
                                      MAKEINTRESOURCEW(IDI_DSIME));
            if (hIcon) {
                ::SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
                ::SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
            }
            LoadIntoDialog(dlg);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    std::wstring err;
                    if (SaveFromDialog(dlg, &err)) {
                        ::EndDialog(dlg, IDOK);
                    } else {
                        std::wstring msgText = L"Could not save settings.\n\n" + err;
                        ::MessageBoxW(dlg, msgText.c_str(), L"DS Input",
                                      MB_OK | MB_ICONERROR);
                    }
                    return TRUE;
                }
                case IDCANCEL:
                    ::EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
        case WM_CLOSE:
            ::EndDialog(dlg, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // COM is not strictly needed by the engine, but init it in case future core
    // changes use COM-backed paths; STA matches a UI process.
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    ::InitCommonControlsEx(&icc);

    // One shared engine for the dialog lifetime; default per-user config path.
    if (!g_engine.Create(nullptr)) {
        std::wstring err = dsime::LastError();
        std::wstring msgText = L"Failed to load DS Input core engine.\n\n" + err;
        ::MessageBoxW(nullptr, msgText.c_str(), L"DS Input", MB_OK | MB_ICONERROR);
        ::CoUninitialize();
        return 1;
    }

    ::DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
                      DlgProc, 0);

    g_engine.reset();
    ::CoUninitialize();
    return 0;
}
