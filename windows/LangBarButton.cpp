// LangBarButton.cpp — ITfLangBarItemButton: the system-tray / language-bar entry
// that opens the Settings dialog.
//
// TSF shows a button for each ITfLangBarItem a service registers with the
// ITfLangBarItemMgr (reached via the thread manager). Clicking it (or selecting
// its single menu item) launches DSInputSettings.exe, which edits the shared
// config through the same core (ds_engine_get/set_config_json), so the IME and
// the settings UI share one source of truth.
//
// We keep this self-contained: CLangBarButton holds a back-pointer to the
// CTextService only to read its module/thread context; it does not mutate
// composition state.

#include "TextService.h"
#include "Globals.h"
#include "Guids.h"

#include <new>
#include <shellapi.h>
#include <strsafe.h>

// Menu command id for the single "Settings…" entry.
#define DSIME_MENU_SETTINGS 1

// Launch DSInputSettings.exe from the directory this DLL lives in.
static void LaunchSettings() {
    wchar_t path[MAX_PATH] = {};
    DWORD n = ::GetModuleFileNameW(g_hInst, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    // Trim the DLL file name, leaving the trailing backslash.
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    slash[1] = L'\0';

    std::wstring exe = std::wstring(path) + DSIME_SETTINGS_EXE;
    // ShellExecute so it runs as a separate process; the IME keeps running.
    ::ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, path, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// CLangBarButton
// ---------------------------------------------------------------------------

class CLangBarButton final : public ITfLangBarItemButton,
                             public ITfSource {
public:
    explicit CLangBarButton(CTextService* pSvc) : _cRef(1), _pSvc(pSvc) {
        _pSvc->AddRef();

        _info.clsidService = c_clsidDsimeTextService;
        _info.guidItem = c_guidDsimeLangBarItem;
        _info.dwStyle = TF_LBI_STYLE_BTN_MENU | TF_LBI_STYLE_SHOWNINTRAY;
        _info.ulSort = 0;
        ::StringCchCopyW(_info.szDescription, ARRAYSIZE(_info.szDescription),
                         DSIME_DESC_W);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ITfLangBarItem) ||
            IsEqualIID(riid, IID_ITfLangBarItemButton)) {
            *ppv = static_cast<ITfLangBarItemButton*>(this);
        } else if (IsEqualIID(riid, IID_ITfSource)) {
            *ppv = static_cast<ITfSource*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = ::InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return c;
    }

    // ---- ITfLangBarItem ----
    STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* pInfo) override {
        if (!pInfo) return E_INVALIDARG;
        *pInfo = _info;
        return S_OK;
    }
    STDMETHODIMP GetStatus(DWORD* pdwStatus) override {
        if (!pdwStatus) return E_INVALIDARG;
        *pdwStatus = 0;  // enabled, visible
        return S_OK;
    }
    STDMETHODIMP Show(BOOL /*fShow*/) override { return E_NOTIMPL; }
    STDMETHODIMP GetTooltipString(BSTR* pbstrToolTip) override {
        if (!pbstrToolTip) return E_INVALIDARG;
        *pbstrToolTip = ::SysAllocString(DSIME_DESC_W);
        return *pbstrToolTip ? S_OK : E_OUTOFMEMORY;
    }

    // ---- ITfLangBarItemButton ----
    STDMETHODIMP OnClick(TfLBIClick /*click*/, POINT /*pt*/,
                         const RECT* /*prcArea*/) override {
        // A direct click opens settings too (in addition to the menu).
        LaunchSettings();
        return S_OK;
    }

    STDMETHODIMP InitMenu(ITfMenu* pMenu) override {
        if (!pMenu) return E_INVALIDARG;
        // One entry: "Settings…".
        pMenu->AddMenuItem(DSIME_MENU_SETTINGS, 0, nullptr, nullptr,
                           L"Settings…", 9, nullptr);
        return S_OK;
    }

    STDMETHODIMP OnMenuSelect(UINT wID) override {
        if (wID == DSIME_MENU_SETTINGS) {
            LaunchSettings();
        }
        return S_OK;
    }

    STDMETHODIMP GetIcon(HICON* phIcon) override {
        if (!phIcon) return E_INVALIDARG;
        *phIcon = static_cast<HICON>(::LoadImageW(g_hInst,
            MAKEINTRESOURCEW(IDI_DSIME), IMAGE_ICON,
            ::GetSystemMetrics(SM_CXSMICON),
            ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        return *phIcon ? S_OK : E_FAIL;
    }

    STDMETHODIMP GetText(BSTR* pbstrText) override {
        if (!pbstrText) return E_INVALIDARG;
        *pbstrText = ::SysAllocString(DSIME_DESC_W);
        return *pbstrText ? S_OK : E_OUTOFMEMORY;
    }

    // ---- ITfSource (the lang-bar host advises a sink to learn of updates) ----
    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie) override {
        if (!IsEqualIID(riid, IID_ITfLangBarItemSink) || !punk || !pdwCookie)
            return E_INVALIDARG;
        if (_pSink) return CONNECT_E_ADVISELIMIT;  // single sink only
        if (FAILED(punk->QueryInterface(IID_ITfLangBarItemSink,
                                        reinterpret_cast<void**>(&_pSink))))
            return E_NOINTERFACE;
        *pdwCookie = 0;  // one sink; fixed cookie
        return S_OK;
    }
    STDMETHODIMP UnadviseSink(DWORD dwCookie) override {
        if (dwCookie != 0 || !_pSink) return CONNECT_E_NOCONNECTION;
        _pSink->Release();
        _pSink = nullptr;
        return S_OK;
    }

private:
    ~CLangBarButton() {
        if (_pSink) _pSink->Release();
        _pSvc->Release();
    }
    LONG _cRef;
    CTextService* _pSvc;
    TF_LANGBARITEMINFO _info = {};
    ITfLangBarItemSink* _pSink = nullptr;
};

// ---------------------------------------------------------------------------
// CTextService language-bar wiring
// ---------------------------------------------------------------------------

BOOL CTextService::_InitLanguageBar() {
    ITfLangBarItemMgr* pMgr = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfLangBarItemMgr,
                                           reinterpret_cast<void**>(&pMgr))))
        return FALSE;

    CLangBarButton* pButton = new (std::nothrow) CLangBarButton(this);
    if (!pButton) { pMgr->Release(); return FALSE; }

    HRESULT hr = pMgr->AddItem(pButton);
    if (SUCCEEDED(hr)) {
        _pLangBarButton = pButton;  // keep our ref (AddItem took its own)
    } else {
        pButton->Release();
    }
    pMgr->Release();
    return SUCCEEDED(hr);
}

void CTextService::_UninitLanguageBar() {
    if (!_pLangBarButton) return;
    ITfLangBarItemMgr* pMgr = nullptr;
    if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfLangBarItemMgr,
                                              reinterpret_cast<void**>(&pMgr)))) {
        pMgr->RemoveItem(_pLangBarButton);
        pMgr->Release();
    }
    _pLangBarButton->Release();
    _pLangBarButton = nullptr;
}
