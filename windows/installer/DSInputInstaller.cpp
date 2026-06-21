// DSInputInstaller.cpp — a guided, macOS-style installer for the DS Input TSF
// IME. One self-contained, elevated exe that embeds both the x64 and ARM64
// builds, installs the set matching the host, registers the text service, and
// adds it to the user's language list.
//
// Flow (single window): describe → Install → progress → done / failed.
//
//   1. Extract the host-arch payload (dsime_tsf.dll, dsime.dll,
//      DSInputSettings.exe) to %ProgramFiles%\DSInput.
//   2. regsvr32 the TSF DLL (the native-arch regsvr32 in System32 matches the
//      native-arch DLL we install).
//   3. Add the DS Input profile to the user's zh-Hans language list.
//   4. Tell the user to sign out / back in (Windows enrolls a freshly registered
//      text service at the next logon, like macOS does at login).

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <string>

#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr wchar_t kInstallDirName[] = L"DSInput";
// TSF profile id for the language list: "0804:{CLSID}{PROFILE}" — must match
// windows/Guids.h (c_clsidDsimeTextService / c_guidDsimeProfile) and LANGID 0804.
constexpr wchar_t kTipId[] =
    L"0804:{6F3D9A21-7C44-4E1B-9C2A-1B2C3D4E5F60}{A1B2C3D4-55E6-47F8-8901-23456789ABCD}";

constexpr UINT WM_APP_PROGRESS = WM_APP + 1;  // lParam = wchar_t* (proc frees)
constexpr UINT WM_APP_DONE     = WM_APP + 2;  // wParam = 1 ok / 0 failed; lParam = wchar_t*

// ---- arch detection --------------------------------------------------------

bool HostIsArm64() {
    USHORT processMachine = 0, nativeMachine = 0;
    if (::IsWow64Process2(::GetCurrentProcess(), &processMachine, &nativeMachine)) {
        return nativeMachine == IMAGE_FILE_MACHINE_ARM64;
    }
    return false;  // default to x64 if the API is unavailable
}

// ---- small helpers ---------------------------------------------------------

std::wstring ProgramFilesDir() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &p)) && p) {
        out = p;
    }
    if (p) ::CoTaskMemFree(p);
    if (out.empty()) {
        wchar_t buf[MAX_PATH];
        DWORD n = ::GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH);
        out.assign(buf, n);
    }
    return out;
}

// Write an embedded RCDATA resource to a file. On a sharing violation (a prior
// install's DLL is loaded) rename the existing file aside first.
bool ExtractResource(int id, const std::wstring& path, std::wstring* err) {
    HRSRC hr = ::FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!hr) { if (err) *err = L"missing payload resource"; return false; }
    DWORD size = ::SizeofResource(nullptr, hr);
    HGLOBAL hg = ::LoadResource(nullptr, hr);
    if (!hg || size == 0) { if (err) *err = L"could not load payload"; return false; }
    const void* data = ::LockResource(hg);
    if (!data) { if (err) *err = L"could not lock payload"; return false; }

    for (int attempt = 0; attempt < 2; ++attempt) {
        HANDLE f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            if (attempt == 0 && ::GetLastError() == ERROR_SHARING_VIOLATION) {
                // Loaded by a running app — rename it aside (allowed on NTFS) and retry.
                std::wstring old = path + L".old";
                ::DeleteFileW(old.c_str());
                ::MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
                continue;
            }
            if (err) *err = L"could not write " + path;
            return false;
        }
        DWORD wrote = 0;
        BOOL ok = ::WriteFile(f, data, size, &wrote, nullptr) && wrote == size;
        ::CloseHandle(f);
        if (!ok) { if (err) *err = L"short write to " + path; return false; }
        return true;
    }
    if (err) *err = L"could not replace " + path + L" (in use)";
    return false;
}

// Run a process and wait; returns its exit code, or -1 on launch failure.
DWORD RunWait(const std::wstring& cmdline) {
    std::wstring mutableCmd = cmdline;  // CreateProcessW may modify the buffer
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return static_cast<DWORD>(-1);
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread); ::CloseHandle(pi.hProcess);
    return code;
}

void PostProgress(HWND dlg, const wchar_t* text) {
    if (!dlg) return;  // silent mode: no UI to update
    ::PostMessageW(dlg, WM_APP_PROGRESS, 0,
                   reinterpret_cast<LPARAM>(::_wcsdup(text)));
}

// Write a UTF-8 file (used for the temp language-list script).
bool WriteAllBytes(const std::wstring& path, const std::string& bytes) {
    HANDLE f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = ::WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
    ::CloseHandle(f);
    return ok && wrote == bytes.size();
}

// Add the DS Input profile to the current user's zh-Hans language list via a
// short PowerShell script (the supported, forward-compatible API). Best-effort.
void AddToLanguageList(HWND dlg) {
    wchar_t tmp[MAX_PATH]; ::GetTempPathW(MAX_PATH, tmp);
    std::wstring ps = std::wstring(tmp) + L"dsinput-lang.ps1";
    // ASCII-only script; the TIP id is constant.
    std::string script =
        "Import-Module International -ErrorAction SilentlyContinue\r\n"
        "$tip = '0804:{6F3D9A21-7C44-4E1B-9C2A-1B2C3D4E5F60}{A1B2C3D4-55E6-47F8-8901-23456789ABCD}'\r\n"
        "$ll = Get-WinUserLanguageList\r\n"
        "$zh = $ll | Where-Object { $_.LanguageTag -like 'zh*' } | Select-Object -First 1\r\n"
        "if (-not $zh) { $ll.Add('zh-Hans-CN'); $zh = $ll | Where-Object { $_.LanguageTag -like 'zh*' } | Select-Object -First 1 }\r\n"
        "if (-not ($zh.InputMethodTips -contains $tip)) { $zh.InputMethodTips.Add($tip); Set-WinUserLanguageList $ll -Force }\r\n";
    if (!WriteAllBytes(ps, script)) return;
    PostProgress(dlg, L"Adding DS Input to your language list…");
    RunWait(L"powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + ps + L"\"");
    ::DeleteFileW(ps.c_str());
}

// ---- the install job (worker thread) ---------------------------------------

// Core install steps. Posts progress to `dlg` (nullptr => silent). Returns
// success and the final user-facing message in `outMsg`.
bool RunInstall(HWND dlg, std::wstring& outMsg) {
    const bool arm64 = HostIsArm64();
    const int rTsf      = arm64 ? IDR_ARM64_TSF      : IDR_X64_TSF;
    const int rCore     = arm64 ? IDR_ARM64_CORE     : IDR_X64_CORE;
    const int rSettings = arm64 ? IDR_ARM64_SETTINGS : IDR_X64_SETTINGS;

    std::wstring dst = ProgramFilesDir() + L"\\" + kInstallDirName;
    std::wstring err;

    PostProgress(dlg, (std::wstring(L"Installing the ") + (arm64 ? L"ARM64" : L"x64")
                       + L" build to " + dst + L"…").c_str());
    ::SHCreateDirectoryExW(nullptr, dst.c_str(), nullptr);

    bool ok =
        ExtractResource(rCore,     dst + L"\\dsime.dll",          &err) &&
        ExtractResource(rTsf,      dst + L"\\dsime_tsf.dll",      &err) &&
        ExtractResource(rSettings, dst + L"\\DSInputSettings.exe", &err);

    if (ok) {
        PostProgress(dlg, L"Registering the text service…");
        std::wstring dll = dst + L"\\dsime_tsf.dll";
        DWORD rc = RunWait(L"regsvr32 /s \"" + dll + L"\"");
        if (rc != 0) { ok = false; err = L"regsvr32 failed (code " + std::to_wstring(rc) + L")"; }
    }

    if (ok) {
        AddToLanguageList(dlg);  // best-effort
        outMsg =
            L"DS Input is installed.\r\n\r\nSign out and back in to finish — Windows "
            L"enables a newly registered input method at the next logon. Then switch to "
            L"it with Win+Space and set your API key in DS Input Settings.";
        return true;
    }
    outMsg = L"Installation failed: " + err;
    return false;
}

struct Job { HWND dlg; };

DWORD WINAPI InstallThread(LPVOID param) {
    Job* job = static_cast<Job*>(param);
    HWND dlg = job->dlg;
    delete job;
    std::wstring msg;
    bool ok = RunInstall(dlg, msg);
    ::PostMessageW(dlg, WM_APP_DONE, ok ? 1 : 0,
                   reinterpret_cast<LPARAM>(::_wcsdup(msg.c_str())));
    return 0;
}

// ---- dialog ----------------------------------------------------------------

void SetBodyWelcome(HWND dlg) {
    const bool arm64 = HostIsArm64();
    std::wstring body =
        L"This installs DS Input for this PC. You type toneless pinyin and an LLM "
        L"converts the whole sentence to Chinese inline — there is no candidate window.\r\n\r\n"
        L"The installer will:\r\n"
        L"  1.  Copy the ";
    body += arm64 ? L"ARM64" : L"x64";
    body +=
        L" build into Program Files.\r\n"
        L"  2.  Register the text service with Windows.\r\n"
        L"  3.  Add DS Input to your Chinese (Simplified) keyboards.\r\n\r\n"
        L"After it finishes, sign out and back in to activate it, then set your API key "
        L"in DS Input Settings.";
    ::SetDlgItemTextW(dlg, IDC_BODY, body.c_str());
}

INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            HICON ic = ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_INSTALLER));
            if (ic) {
                ::SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(ic));
                ::SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(ic));
            }
            SetBodyWelcome(dlg);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    // Begin install: lock the buttons, start the marquee, work off-thread.
                    ::EnableWindow(::GetDlgItem(dlg, IDOK), FALSE);
                    ::EnableWindow(::GetDlgItem(dlg, IDCANCEL), FALSE);
                    HWND pr = ::GetDlgItem(dlg, IDC_PROGRESS);
                    ::ShowWindow(pr, SW_SHOW);
                    ::SendMessageW(pr, PBM_SETMARQUEE, TRUE, 30);
                    Job* job = new Job{ dlg };
                    HANDLE t = ::CreateThread(nullptr, 0, InstallThread, job, 0, nullptr);
                    if (t) ::CloseHandle(t);
                    return TRUE;
                }
                case IDCANCEL:
                    ::EndDialog(dlg, 0);
                    return TRUE;
            }
            break;

        case WM_APP_PROGRESS: {
            wchar_t* text = reinterpret_cast<wchar_t*>(lParam);
            if (text) { ::SetDlgItemTextW(dlg, IDC_STATUS, text); ::free(text); }
            return TRUE;
        }

        case WM_APP_DONE: {
            wchar_t* text = reinterpret_cast<wchar_t*>(lParam);
            HWND pr = ::GetDlgItem(dlg, IDC_PROGRESS);
            ::SendMessageW(pr, PBM_SETMARQUEE, FALSE, 0);
            ::ShowWindow(pr, SW_HIDE);
            if (text) { ::SetDlgItemTextW(dlg, IDC_STATUS, text); ::free(text); }
            // Done: hide the action button, leave Close enabled.
            ::ShowWindow(::GetDlgItem(dlg, IDOK), SW_HIDE);
            ::EnableWindow(::GetDlgItem(dlg, IDCANCEL), TRUE);
            ::SetDlgItemTextW(dlg, IDCANCEL, L"Close");
            return TRUE;
        }

        case WM_CLOSE:
            ::EndDialog(dlg, 0);
            return TRUE;
    }
    return FALSE;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Silent / unattended mode: /S (or /silent) installs without UI and returns
    // 0 on success, 1 on failure — for automation and headless verification.
    std::wstring cmd = lpCmdLine ? lpCmdLine : L"";
    if (cmd.find(L"/S") != std::wstring::npos || cmd.find(L"/silent") != std::wstring::npos) {
        std::wstring msg;
        bool ok = RunInstall(nullptr, msg);
        ::CoUninitialize();
        return ok ? 0 : 1;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    ::InitCommonControlsEx(&icc);
    ::DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_INSTALLER), nullptr, DlgProc, 0);
    ::CoUninitialize();
    return 0;
}
