#include "main_window.h"
#include "clip_trimmer.h"
#include "com_ptr.h"
#include "hotkey_engine.h"
#include "sound_fx.h"

#include <objidl.h>
#include <gdiplus.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <commctrl.h>
#include <mfapi.h>
#include <mfplay.h>
#include <mfobjects.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

namespace {

constexpr COLORREF kRgbBg      = RGB(13, 16, 23);
constexpr COLORREF kRgbCard    = RGB(23, 28, 38);
constexpr COLORREF kRgbInput   = RGB(32, 39, 53);
constexpr COLORREF kRgbBorder  = RGB(45, 55, 74);
constexpr COLORREF kRgbText    = RGB(238, 242, 248);
constexpr COLORREF kRgbMuted   = RGB(148, 160, 180);

constexpr UINT_PTR TIMER_PLAYER_ID    = 2;
constexpr UINT_PTR TIMER_MIC_METER_ID = 3;
constexpr UINT WM_MW_THUMB_READY      = WM_APP + 20;
constexpr UINT WM_MW_PLAYER_EVENT     = WM_APP + 21;
constexpr UINT WM_MW_TRIM_DONE        = WM_APP + 22;

inline Gdiplus::Color Gc(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return Gdiplus::Color(a, r, g, b);
}

void AddRoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    path.Reset();
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRoundRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, Gdiplus::Color fill) {
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, x, y, w, h, r);
    Gdiplus::SolidBrush br(fill);
    g.FillPath(&br, &path);
}

void StrokeRoundRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, Gdiplus::Color col, float width = 1.0f) {
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, x, y, w, h, r);
    Gdiplus::Pen pen(col, width);
    g.DrawPath(&pen, &path);
}

enum CustomBtnId {
    BTN_TAB_CLIPS = 1,
    BTN_TAB_SETTINGS,
    BTN_SIDE_SAVE,
    BTN_SIDE_TOGGLE,
    BTN_SIDE_MIC,
    BTN_TOP_FAV_FILTER,
    BTN_TOP_CLEAR_SEARCH,
    BTN_TOP_FOLDER,
    BTN_TOP_REFRESH,
    BTN_CLIP_PLAY,
    BTN_CLIP_FAV,
    BTN_CLIP_RENAME,
    BTN_CLIP_REVEAL,
    BTN_CLIP_DELETE,
    BTN_SET_720P,
    BTN_SET_1080P,

    // Player & Trim controls
    BTN_PLY_BACK = 50,
    BTN_PLY_FAV,
    BTN_PLY_RENAME,
    BTN_PLY_FOLDER,
    BTN_PLY_DELETE,
    BTN_PLY_PLAYPAUSE,
    BTN_PLY_MINUS5,
    BTN_PLY_PLUS5,
    BTN_PLY_RESTART,
    BTN_PLY_RATE,
    BTN_PLY_TRIM,
    BTN_PLY_MUTE,
    BTN_PLY_EXTERNAL,
    BTN_TRIM_SET_START,
    BTN_TRIM_SET_END,
    BTN_TRIM_SAVE_COPY,
    BTN_TRIM_REPLACE,
    BTN_TRIM_CANCEL
};

enum NativeCtrlId {
    ID_EDT_SEARCH = 2999,
    ID_CMB_DURATION = 3001,
    ID_CMB_RESFPS,
    ID_CMB_BITRATE,
    ID_CHK_GAME_AUDIO,
    ID_TRK_GAME_VOL,
    ID_CHK_MIC_AUDIO,
    ID_CHK_MIC_MUTE,
    ID_TRK_MIC_VOL,
    ID_CMB_MIC_DEV,
    ID_CHK_MIC_GATE,
    ID_TRK_MIC_GATE,
    ID_BTN_SET_CLIP_HK,
    ID_BTN_SET_MIC_HK,
    ID_BTN_CLR_MIC_HK,
    ID_BTN_RESTART_ADMIN,
    ID_BTN_BROWSE_DIR,
    ID_BTN_OPEN_DIR,
    ID_CHK_SOUND,
    ID_BTN_TEST_SOUND,
    ID_CHK_AUTOSTART,
    ID_BTN_APPLY,
    ID_BTN_EXIT_APP,

    ID_CTX_PLAY = 4001,
    ID_CTX_FAV,
    ID_CTX_RENAME,
    ID_CTX_REVEAL,
    ID_CTX_DELETE
};

struct TrimDoneMsg {
    bool ok = false;
    bool replaceOriginal = false;
    std::wstring origPath;
    std::wstring outPath;
    std::wstring message;
};

void DrawStarPolygon(Gdiplus::Graphics& g, float cx, float cy, float rOut, float rIn, bool filled) {
    Gdiplus::PointF pts[10];
    const float startAngle = -3.14159265f / 2.0f;
    const float step = 3.14159265f / 5.0f;
    for (int i = 0; i < 10; ++i) {
        float r = (i % 2 == 0) ? rOut : rIn;
        float a = startAngle + step * static_cast<float>(i);
        pts[i].X = cx + r * std::cos(a);
        pts[i].Y = cy + r * std::sin(a);
    }
    if (filled) {
        Gdiplus::SolidBrush br(Gc(250, 204, 21));
        g.FillPolygon(&br, pts, 10);
        Gdiplus::Pen pen(Gc(253, 224, 71), 1.0f);
        g.DrawPolygon(&pen, pts, 10);
    } else {
        Gdiplus::Pen pen(Gc(203, 213, 225, 210), 1.3f);
        g.DrawPolygon(&pen, pts, 10);
    }
}

std::wstring ToLowerStr(std::wstring s) {
    if (!s.empty()) {
        CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
    }
    return s;
}

std::wstring FormatDurPrecise(double sec) {
    if (sec < 0.0) sec = 0.0;
    int m = static_cast<int>(sec) / 60;
    double rem = sec - m * 60;
    wchar_t buf[32] = {};
    swprintf(buf, 32, L"%02d:%04.1f", m, rem);
    return buf;
}

struct ThumbMsg {
    uint64_t gen = 0;
    std::wstring path;
    HBITMAP hBmp = nullptr;
    double dur = 0.0;
    uint32_t w = 0;
    uint32_t h = 0;
};

struct PlayerEventMsg {
    uint64_t gen = 0;
    MFP_EVENT_TYPE type = MFP_EVENT_TYPE_PLAY;
    HRESULT hr = S_OK;
};

constexpr int kDurations[] = { 30, 60, 120, 180, 300 };
struct ResPreset { int w, h, fps; const wchar_t* label; };
constexpr ResPreset kResPresets[] = {
    { 1280, 720, 60, L"720p HD (1280x720) @ 60 FPS" },
    { 1280, 720, 30, L"720p HD (1280x720) @ 30 FPS" },
    { 1920, 1080, 60, L"1080p Full HD (1920x1080) @ 60" },
    { 1920, 1080, 30, L"1080p Full HD (1920x1080) @ 30" }
};
constexpr int kBitrates[] = { 4000, 6000, 8000, 10000, 12000, 15000 };

void ApplyDarkTitleBarAndControls(HWND hwnd) {
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using PFN_DwmSet = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (auto pfn = reinterpret_cast<PFN_DwmSet>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) {
            BOOL dark = TRUE;
            pfn(hwnd, 20, &dark, sizeof(dark));
        }
        FreeLibrary(dwm);
    }
    if (HMODULE uxt = LoadLibraryW(L"uxtheme.dll")) {
        using PFN_SetPref = int(WINAPI*)(int);
        if (auto pref = reinterpret_cast<PFN_SetPref>(GetProcAddress(uxt, MAKEINTRESOURCEA(135)))) {
            pref(2);
        }
        FreeLibrary(uxt);
    }
}

void ClearControlTheme(HWND ctrl) {
    if (HMODULE uxt = LoadLibraryW(L"uxtheme.dll")) {
        using PFN_SetTheme = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
        if (auto st = reinterpret_cast<PFN_SetTheme>(GetProcAddress(uxt, "SetWindowTheme"))) {
            st(ctrl, L"", L"");
        }
        FreeLibrary(uxt);
    }
}

std::wstring FormatDurClock(double sec) {
    if (sec < 0.0) sec = 0.0;
    int t = static_cast<int>(sec + 0.5);
    wchar_t buf[24] = {};
    swprintf(buf, 24, L"%02d:%02d", t / 60, t % 60);
    return buf;
}

std::wstring FormatMb(uint64_t bytes) {
    wchar_t buf[32] = {};
    swprintf(buf, 32, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

std::wstring StripMp4Ext(const std::wstring& name) {
    if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".mp4") == 0) {
        return name.substr(0, name.size() - 4);
    }
    return name;
}

struct RenameDlgState {
    std::wstring text;
    bool accepted = false;
    bool done = false;
    HWND edt = nullptr;
    HBRUSH brBg = nullptr;
    HBRUSH brInput = nullptr;
};

LRESULT CALLBACK RenameDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<RenameDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        st = reinterpret_cast<RenameDlgState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return TRUE;
    }
    if (!st) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc{}; GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, st->brBg);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wParam), kRgbText);
            SetBkColor(reinterpret_cast<HDC>(wParam), kRgbCard);
            return reinterpret_cast<LRESULT>(st->brBg);
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wParam), kRgbText);
            SetBkColor(reinterpret_cast<HDC>(wParam), kRgbInput);
            return reinterpret_cast<LRESULT>(st->brInput);
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                wchar_t buf[256] = {};
                GetWindowTextW(st->edt, buf, 256);
                st->text = buf;
                st->accepted = true;
                st->done = true;
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) { st->done = true; return 0; }
            break;
        case WM_CLOSE:
            st->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowRenamePopup(HWND owner, HINSTANCE hInst, UINT dpi, HFONT font, std::wstring& ioName) {
    auto S = [&](int px) { return MulDiv(px, static_cast<int>(dpi), 96); };
    static bool reg = false;
    const wchar_t* cls = L"AnlikKlipRenameDlg2";
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = RenameDlgProc;
        wc.hInstance = hInst;
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        reg = true;
    }
    RenameDlgState st;
    st.text = ioName;
    st.brBg = CreateSolidBrush(kRgbCard);
    st.brInput = CreateSolidBrush(kRgbInput);

    RECT orc{}; GetWindowRect(owner, &orc);
    int w = S(390), h = S(148);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"Klibi Yeniden Adlandır",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, owner, nullptr, hInst, &st);
    ApplyDarkTitleBarAndControls(dlg);

    HWND lbl = CreateWindowExW(0, L"STATIC", L"Yeni klip adı:", WS_CHILD | WS_VISIBLE,
        S(16), S(14), S(340), S(20), dlg, nullptr, hInst, nullptr);
    st.edt = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", ioName.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        S(16), S(38), S(342), S(26), dlg, nullptr, hInst, nullptr);
    HWND okBtn = CreateWindowExW(0, L"BUTTON", L"Kaydet",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        S(192), S(76), S(80), S(28), dlg, reinterpret_cast<HMENU>(IDOK), hInst, nullptr);
    HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"İptal",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        S(278), S(76), S(80), S(28), dlg, reinterpret_cast<HMENU>(IDCANCEL), hInst, nullptr);

    for (HWND c : { lbl, st.edt, okBtn, cancelBtn }) {
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }

    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.edt);
    SendMessageW(st.edt, EM_SETSEL, 0, -1);

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { st.done = true; break; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) { SendMessageW(dlg, WM_COMMAND, IDOK, 0); break; }
        if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }

    EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    SetForegroundWindow(owner);
    DeleteObject(st.brBg);
    DeleteObject(st.brInput);

    if (st.accepted && !st.text.empty()) {
        ioName = st.text;
        return true;
    }
    return false;
}

} // namespace

class PlayerCallbackImpl final : public IMFPMediaPlayerCallback {
public:
    PlayerCallbackImpl(HWND notifyHwnd, uint64_t gen)
        : notifyHwnd_(notifyHwnd), gen_(gen) {}

    void Detach() { alive_.store(false); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (r == 0) delete this;
        return r;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* pEventHeader) override {
        if (!pEventHeader || !alive_.load() || !notifyHwnd_) return;
        auto* m = new PlayerEventMsg{ gen_, pEventHeader->eEventType, pEventHeader->hrEvent };
        if (!PostMessageW(notifyHwnd_, WM_MW_PLAYER_EVENT, 0, reinterpret_cast<LPARAM>(m))) {
            delete m;
        }
    }

private:
    ~PlayerCallbackImpl() = default;
    volatile LONG refCount_ = 1;
    HWND notifyHwnd_;
    uint64_t gen_;
    std::atomic<bool> alive_{true};
};

MainWindow::MainWindow(HINSTANCE hInst, RecorderApp& app, HotkeyEngine& hotkeys, Callbacks cb)
    : hInst_(hInst), app_(app), hotkeys_(hotkeys), cb_(std::move(cb)) {}

MainWindow::~MainWindow() {
    StopTrimWorker();
    CloseEmbeddedPlayer();
    StopThumbnailWorker();
    ClearThumbnails();
    if (hwnd_) {
        KillTimer(hwnd_, TIMER_MIC_METER_ID);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    if (boldFont_) { DeleteObject(boldFont_); boldFont_ = nullptr; }
    if (brBg_) { DeleteObject(brBg_); brBg_ = nullptr; }
    if (brCard_) { DeleteObject(brCard_); brCard_ = nullptr; }
    if (brInput_) { DeleteObject(brInput_); brInput_ = nullptr; }
    if (gdipToken_) {
        Gdiplus::GdiplusShutdown(gdipToken_);
        gdipToken_ = 0;
    }
}

bool MainWindow::Create(int initialTab) {
    Gdiplus::GdiplusStartupInput si;
    if (Gdiplus::GdiplusStartup(&gdipToken_, &si, nullptr) != Gdiplus::Ok) {
        return false;
    }

    brBg_ = CreateSolidBrush(kRgbBg);
    brCard_ = CreateSolidBrush(kRgbCard);
    brInput_ = CreateSolidBrush(kRgbInput);

    static bool reg = false;
    const wchar_t* cls = L"AnlikKlipMainWinClass";
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = MainWindow::WndProc;
        wc.hInstance = hInst_;
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = brBg_;
        RegisterClassExW(&wc);
        reg = true;
    }

    dpi_ = GetDpiForSystem();
    if (dpi_ < 96) dpi_ = 96;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT wr{ 0, 0, S(920), S(620) };
    AdjustWindowRectEx(&wr, style, FALSE, 0);

    hwnd_ = CreateWindowExW(0, cls, L"Anlık Klip", style,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInst_, this);
    if (!hwnd_) return false;

    ApplyDarkTitleBarAndControls(hwnd_);
    CreateControls();
    LoadSettingsControls();
    RefreshClips();
    SwitchTab(initialTab);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    return true;
}

void MainWindow::BringToFront(int tab) {
    if (!hwnd_) return;
    if (tab >= 0) SwitchTab(tab);
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
}

void MainWindow::NotifyClipSaved(const ClipSaveResult& res) {
    SetFooterMessage(res.message);
    if (res.ok) {
        RefreshClips(playerOpen_ ? L"" : res.filePath);
    } else if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::RefreshLiveStats() {
    if (!hwnd_) return;
    // Repaint sidebar only so embedded video never flickers
    RECT sideRc{ 0, 0, S(220), S(620) };
    InvalidateRect(hwnd_, &sideRc, FALSE);
}

bool MainWindow::HandlePreTranslateMessage(const MSG& msg) {
    if (captureHotkeyMode_ != 0) {
        if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
        UINT vk = static_cast<UINT>(msg.wParam);
        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
            vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_LWIN || vk == VK_RWIN) {
            return true;
        }
        if (vk == VK_ESCAPE) {
            captureHotkeyMode_ = 0;
            hotkeys_.SetPaused(false);
            SetWindowTextW(edtClipHotkey_, FormatHotkey(pendingClipMods_, pendingClipVk_).c_str());
            SetWindowTextW(edtMicHotkey_, FormatHotkey(pendingMicMods_, pendingMicVk_).c_str());
            return true;
        }
        UINT mods = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
        if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;

        if (captureHotkeyMode_ == 1) {
            pendingClipMods_ = mods;
            pendingClipVk_ = vk;
            SetWindowTextW(edtClipHotkey_, FormatHotkey(mods, vk).c_str());
        } else if (captureHotkeyMode_ == 2) {
            pendingMicMods_ = mods;
            pendingMicVk_ = vk;
            SetWindowTextW(edtMicHotkey_, FormatHotkey(mods, vk).c_str());
        }
        captureHotkeyMode_ = 0;
        hotkeys_.SetPaused(false);

        AppConfig cfg = app_.Config();
        cfg.hotkeyMods = pendingClipMods_;
        cfg.hotkeyVk = pendingClipVk_;
        cfg.micHotkeyMods = pendingMicMods_;
        cfg.micHotkeyVk = pendingMicVk_;
        if (cb_.onConfigApplied) cb_.onConfigApplied(cfg);
        SetFooterMessage(L"Kısayol güncellendi: " + FormatHotkey(cfg.hotkeyMods, cfg.hotkeyVk));
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    // Embedded player keyboard shortcuts
    if (activeTab_ == 0 && playerOpen_ && !trimBusy_ && msg.message == WM_KEYDOWN) {
        UINT vk = static_cast<UINT>(msg.wParam);
        if (vk == VK_SPACE) { TogglePlayerPlayPause(); return true; }
        if (vk == VK_LEFT)  { SeekPlayerRelative(-5.0); return true; }
        if (vk == VK_RIGHT) { SeekPlayerRelative(+5.0); return true; }
        if (vk == VK_ESCAPE) {
            if (trimMode_) ToggleTrimMode();
            else CloseEmbeddedPlayer();
            return true;
        }
    }
    if (activeTab_ == 0 && !playerOpen_ && msg.message == WM_KEYDOWN &&
        msg.wParam == VK_ESCAPE && edtSearch_ && GetFocus() == edtSearch_ && !searchQuery_.empty()) {
        SetWindowTextW(edtSearch_, L"");
        return true;
    }
    return false;
}

void MainWindow::CreateControls() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_)) {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    }
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lfBold = ncm.lfMessageFont;
    lfBold.lfWeight = FW_BOLD;
    boldFont_ = CreateFontIndirectW(&lfBold);

    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, DWORD ex = 0) {
        HWND hw = CreateWindowExW(ex, cls, text, WS_CHILD | style,
            S(x), S(y), S(w), S(h), hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst_, nullptr);
        SendMessageW(hw, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        settingsCtrls_.push_back(hw);
        return hw;
    };
    auto makeLbl = [&](const wchar_t* text, int x, int y, int w, int h) {
        return make(L"STATIC", text, SS_LEFT | SS_NOPREFIX, x, y, w, h);
    };
    auto makeChk = [&](const wchar_t* text, int x, int y, int w, int h, int id) {
        HWND c = make(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, h, id);
        ClearControlTheme(c);
        return c;
    };

    // Gallery search box (not added to settingsCtrls_; toggled by UpdateGalleryControlVisibility)
    edtSearch_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
        S(242), S(48), S(256), S(24), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDT_SEARCH)), hInst_, nullptr);
    if (edtSearch_) {
        SendMessageW(edtSearch_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(edtSearch_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Klip ara..."));
    }

    // Card 1: Video (x=242..562, y=68..248)
    makeLbl(L"Klip Süresi:", 258, 104, 82, 20);
    cmbDuration_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 344, 101, 206, 180, ID_CMB_DURATION);
    for (int sec : kDurations) {
        wchar_t buf[64] = {};
        if (sec < 60) swprintf(buf, 64, L"Son %d sn", sec);
        else if (sec == 60) swprintf(buf, 64, L"Son 1 dk (60 sn) — Önerilen");
        else swprintf(buf, 64, L"Son %d dk", sec / 60);
        SendMessageW(cmbDuration_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }

    makeLbl(L"Çözünürlük:", 258, 139, 82, 20);
    cmbResFps_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 344, 136, 206, 180, ID_CMB_RESFPS);
    for (const auto& p : kResPresets) {
        SendMessageW(cmbResFps_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.label));
    }

    makeLbl(L"Bit Hızı:", 258, 174, 82, 20);
    cmbBitrate_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 344, 171, 206, 180, ID_CMB_BITRATE);
    SendMessageW(cmbBitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"4 Mbps (Hafif RAM)"));
    SendMessageW(cmbBitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"6 Mbps (720p60 Dengeli)"));
    SendMessageW(cmbBitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"8 Mbps (Yüksek Kalite)"));
    SendMessageW(cmbBitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"10 Mbps (1080p30 / Yüksek)"));
    SendMessageW(cmbBitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"12 Mbps (1080p60 Önerilen)"));
    SendMessageW(cmbBitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"15 Mbps (1080p60 Çok Yüksek)"));

    // Card 2: Audio & Mic Noise Gate (x=578..898, y=68..248)
    chkGameAudio_ = makeChk(L"Oyun / Sistem Sesi", 594, 98, 134, 22, ID_CHK_GAME_AUDIO);
    trkGameVol_ = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 730, 96, 114, 26, ID_TRK_GAME_VOL);
    SendMessageW(trkGameVol_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    SendMessageW(trkGameVol_, TBM_SETTICFREQ, 50, 0);
    lblGameVol_ = make(L"STATIC", L"%100", SS_RIGHT, 846, 100, 38, 20);

    chkMicAudio_ = makeChk(L"Mikrofon", 594, 130, 72, 22, ID_CHK_MIC_AUDIO);
    chkMicMute_ = makeChk(L"Sustur", 668, 130, 58, 22, ID_CHK_MIC_MUTE);
    trkMicVol_ = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 730, 128, 114, 26, ID_TRK_MIC_VOL);
    SendMessageW(trkMicVol_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    SendMessageW(trkMicVol_, TBM_SETTICFREQ, 50, 0);
    lblMicVol_ = make(L"STATIC", L"%100", SS_RIGHT, 846, 132, 38, 20);

    makeLbl(L"Cihaz:", 594, 163, 46, 20);
    cmbMicDev_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 642, 160, 240, 180, ID_CMB_MIC_DEV);

    chkMicGate_ = makeChk(L"Gürültü Kapısı", 594, 194, 108, 22, ID_CHK_MIC_GATE);
    trkMicGate_ = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 704, 192, 122, 26, ID_TRK_MIC_GATE);
    SendMessageW(trkMicGate_, TBM_SETRANGE, TRUE, MAKELPARAM(-60, -15));
    SendMessageW(trkMicGate_, TBM_SETTICFREQ, 5, 0);
    lblMicGate_ = make(L"STATIC", L"-36 dB", SS_RIGHT, 828, 195, 56, 20);

    // Card 3: Hotkeys, Folder, Notifications (x=242..898, y=258..528)
    makeLbl(L"Klip Alma Tuşu (ör. F12):", 258, 294, 165, 20);
    edtClipHotkey_ = make(L"EDIT", L"", ES_READONLY | WS_TABSTOP, 428, 291, 196, 24, 0, WS_EX_CLIENTEDGE);
    btnSetClipHotkey_ = make(L"BUTTON", L"Tuş Ata", BS_OWNERDRAW | WS_TABSTOP, 632, 290, 96, 26, ID_BTN_SET_CLIP_HK);

    makeLbl(L"Mikrofon Susturma Tuşu:", 258, 328, 165, 20);
    edtMicHotkey_ = make(L"EDIT", L"", ES_READONLY | WS_TABSTOP, 428, 325, 196, 24, 0, WS_EX_CLIENTEDGE);
    btnSetMicHotkey_ = make(L"BUTTON", L"Tuş Ata", BS_OWNERDRAW | WS_TABSTOP, 632, 324, 96, 26, ID_BTN_SET_MIC_HK);
    btnClearMicHotkey_ = make(L"BUTTON", L"Temizle", BS_OWNERDRAW | WS_TABSTOP, 736, 324, 86, 26, ID_BTN_CLR_MIC_HK);

    bool elev = HotkeyEngine::IsProcessElevated();
    wchar_t hkMsg[200] = {};
    swprintf(hkMsg, 200, L"Tuş algılama: Çift katman (RawInput + LLHook) • %ls",
        elev ? L"Yönetici Modu Aktif" : L"Normal Mod (Yönetici açılan oyunlar için yandaki butonu kullanın)");
    lblHotkeyStatus_ = makeLbl(hkMsg, 258, 360, 488, 20);
    btnRestartAdmin_ = make(L"BUTTON", elev ? L"Yönetici Aktif" : L"Yönetici Başlat",
        BS_OWNERDRAW | WS_TABSTOP, 752, 357, 130, 26, ID_BTN_RESTART_ADMIN);
    if (elev) EnableWindow(btnRestartAdmin_, FALSE);

    makeLbl(L"Kayıt Klasörü:", 258, 396, 95, 20);
    edtOutDir_ = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 358, 393, 366, 24, 0, WS_EX_CLIENTEDGE);
    btnBrowseDir_ = make(L"BUTTON", L"Seç...", BS_OWNERDRAW | WS_TABSTOP, 732, 392, 72, 26, ID_BTN_BROWSE_DIR);
    btnOpenDir_ = make(L"BUTTON", L"Aç", BS_OWNERDRAW | WS_TABSTOP, 810, 392, 72, 26, ID_BTN_OPEN_DIR);

    chkSound_ = makeChk(L"Klip alınca \"çıng\" bildirim sesi çal", 258, 432, 246, 22, ID_CHK_SOUND);
    btnTestSound_ = make(L"BUTTON", L"Sesi Test Et", BS_OWNERDRAW | WS_TABSTOP, 512, 429, 98, 26, ID_BTN_TEST_SOUND);
    chkAutoStart_ = makeChk(L"Windows açılışında arka planda başlat", 258, 466, 290, 22, ID_CHK_AUTOSTART);

    btnApply_ = make(L"BUTTON", L"Ayarları Kaydet ve Uygula", BS_OWNERDRAW | WS_TABSTOP, 242, 540, 224, 38, ID_BTN_APPLY);
    btnExitApp_ = make(L"BUTTON", L"Uygulamayı Tamamen Kapat", BS_OWNERDRAW | WS_TABSTOP, 684, 540, 214, 38, ID_BTN_EXIT_APP);
}

void MainWindow::UpdateGalleryControlVisibility() {
    if (edtSearch_) {
        ShowWindow(edtSearch_, (activeTab_ == 0 && !playerOpen_) ? SW_SHOW : SW_HIDE);
    }
}

void MainWindow::SwitchTab(int tabIndex) {
    int nextTab = (tabIndex == 1) ? 1 : 0;
    if (nextTab == 1 && playerOpen_) {
        CloseEmbeddedPlayer();
    }
    activeTab_ = nextTab;
    for (HWND h : settingsCtrls_) {
        ShowWindow(h, activeTab_ == 1 ? SW_SHOW : SW_HIDE);
    }
    UpdateGalleryControlVisibility();
    if (hwnd_) {
        if (activeTab_ == 1) SetTimer(hwnd_, TIMER_MIC_METER_ID, 120, nullptr);
        else KillTimer(hwnd_, TIMER_MIC_METER_ID);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::LoadSettingsControls() {
    const AppConfig& cfg = app_.Config();
    int durIdx = 1;
    for (int i = 0; i < 5; ++i) if (kDurations[i] == cfg.clipSeconds) { durIdx = i; break; }
    SendMessageW(cmbDuration_, CB_SETCURSEL, durIdx, 0);

    int resIdx = 0;
    for (int i = 0; i < static_cast<int>(sizeof(kResPresets) / sizeof(kResPresets[0])); ++i) {
        if (kResPresets[i].w == cfg.width && kResPresets[i].h == cfg.height && kResPresets[i].fps == cfg.fps) {
            resIdx = i; break;
        }
    }
    SendMessageW(cmbResFps_, CB_SETCURSEL, resIdx, 0);

    int brIdx = 1;
    for (int i = 0; i < static_cast<int>(sizeof(kBitrates) / sizeof(kBitrates[0])); ++i) {
        if (kBitrates[i] == cfg.bitrateKbps) { brIdx = i; break; }
    }
    SendMessageW(cmbBitrate_, CB_SETCURSEL, brIdx, 0);

    SendMessageW(chkGameAudio_, BM_SETCHECK, cfg.captureGameAudio ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(trkGameVol_, TBM_SETPOS, TRUE, cfg.gameVolumePct);
    wchar_t vbuf[16] = {};
    swprintf(vbuf, 16, L"%%%d", cfg.gameVolumePct);
    SetWindowTextW(lblGameVol_, vbuf);

    SendMessageW(chkMicAudio_, BM_SETCHECK, cfg.captureMic ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(chkMicMute_, BM_SETCHECK, cfg.micMuted ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(trkMicVol_, TBM_SETPOS, TRUE, cfg.micVolumePct);
    swprintf(vbuf, 16, L"%%%d", cfg.micVolumePct);
    SetWindowTextW(lblMicVol_, vbuf);

    SendMessageW(chkMicGate_, BM_SETCHECK, cfg.micNoiseGate ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(trkMicGate_, TBM_SETPOS, TRUE, cfg.micGateThresholdDb);
    swprintf(vbuf, 16, L"%d dB", cfg.micGateThresholdDb);
    SetWindowTextW(lblMicGate_, vbuf);

    micDevices_ = EnumerateMicDevices();
    SendMessageW(cmbMicDev_, CB_RESETCONTENT, 0, 0);
    SendMessageW(cmbMicDev_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Varsayılan Windows Mikrofonu"));
    int micSel = 0;
    for (size_t i = 0; i < micDevices_.size(); ++i) {
        SendMessageW(cmbMicDev_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(micDevices_[i].name.c_str()));
        if (!cfg.micDeviceId.empty() && micDevices_[i].id == cfg.micDeviceId) micSel = static_cast<int>(i) + 1;
    }
    SendMessageW(cmbMicDev_, CB_SETCURSEL, micSel, 0);

    pendingClipMods_ = cfg.hotkeyMods;
    pendingClipVk_ = cfg.hotkeyVk;
    pendingMicMods_ = cfg.micHotkeyMods;
    pendingMicVk_ = cfg.micHotkeyVk;
    SetWindowTextW(edtClipHotkey_, FormatHotkey(pendingClipMods_, pendingClipVk_).c_str());
    SetWindowTextW(edtMicHotkey_, FormatHotkey(pendingMicMods_, pendingMicVk_).c_str());

    SetWindowTextW(edtOutDir_, cfg.outputDir.c_str());
    SendMessageW(chkSound_, BM_SETCHECK, cfg.soundNotify ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(chkAutoStart_, BM_SETCHECK, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
}

void MainWindow::ReadSettingsControls(AppConfig& out) {
    out = app_.Config();
    int dIdx = static_cast<int>(SendMessageW(cmbDuration_, CB_GETCURSEL, 0, 0));
    if (dIdx >= 0 && dIdx < 5) out.clipSeconds = kDurations[dIdx];

    int rIdx = static_cast<int>(SendMessageW(cmbResFps_, CB_GETCURSEL, 0, 0));
    constexpr int kNumRes = static_cast<int>(sizeof(kResPresets) / sizeof(kResPresets[0]));
    if (rIdx >= 0 && rIdx < kNumRes) {
        out.width = kResPresets[rIdx].w;
        out.height = kResPresets[rIdx].h;
        out.fps = kResPresets[rIdx].fps;
    }

    int bIdx = static_cast<int>(SendMessageW(cmbBitrate_, CB_GETCURSEL, 0, 0));
    constexpr int kNumBr = static_cast<int>(sizeof(kBitrates) / sizeof(kBitrates[0]));
    if (bIdx >= 0 && bIdx < kNumBr) out.bitrateKbps = kBitrates[bIdx];

    out.captureGameAudio = (SendMessageW(chkGameAudio_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    out.gameVolumePct = static_cast<int>(SendMessageW(trkGameVol_, TBM_GETPOS, 0, 0));
    out.captureMic = (SendMessageW(chkMicAudio_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    out.micMuted = (SendMessageW(chkMicMute_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    out.micVolumePct = static_cast<int>(SendMessageW(trkMicVol_, TBM_GETPOS, 0, 0));
    out.micNoiseGate = (SendMessageW(chkMicGate_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    out.micGateThresholdDb = static_cast<int>(SendMessageW(trkMicGate_, TBM_GETPOS, 0, 0));

    int micSel = static_cast<int>(SendMessageW(cmbMicDev_, CB_GETCURSEL, 0, 0));
    if (micSel <= 0 || micSel - 1 >= static_cast<int>(micDevices_.size())) out.micDeviceId.clear();
    else out.micDeviceId = micDevices_[static_cast<size_t>(micSel - 1)].id;

    out.hotkeyMods = pendingClipMods_;
    out.hotkeyVk = pendingClipVk_;
    out.micHotkeyMods = pendingMicMods_;
    out.micHotkeyVk = pendingMicVk_;

    wchar_t dirBuf[MAX_PATH] = {};
    GetWindowTextW(edtOutDir_, dirBuf, MAX_PATH);
    if (wcslen(dirBuf) > 0) out.outputDir = dirBuf;

    out.soundNotify = (SendMessageW(chkSound_, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

void MainWindow::ClearThumbnails() {
    for (auto& kv : thumbBmps_) {
        if (kv.second) DeleteObject(kv.second);
    }
    thumbBmps_.clear();
}

void MainWindow::StopThumbnailWorker() {
    thumbStop_.store(true);
    ++thumbGen_;
    if (thumbThread_.joinable()) thumbThread_.join();
}

void MainWindow::StartThumbnailWorker() {
    StopThumbnailWorker();
    if (filteredIndices_.empty() || !hwnd_) return;

    thumbStop_.store(false);
    const uint64_t gen = ++thumbGen_;
    std::vector<std::wstring> paths;
    const size_t limit = std::min<size_t>(filteredIndices_.size(), 32);
    for (size_t vi = 0; vi < limit; ++vi) {
        const auto& c = clips_[filteredIndices_[vi]];
        if (thumbBmps_.find(c.filePath) == thumbBmps_.end()) {
            paths.push_back(c.filePath);
        }
    }
    if (paths.empty()) return;

    const int tw = S(298);
    const int th = S(168);
    HWND target = hwnd_;

    thumbThread_ = std::thread([this, gen, paths = std::move(paths), tw, th, target]() {
        ComScope com(COINIT_MULTITHREADED);
        for (const auto& p : paths) {
            if (thumbStop_.load() || thumbGen_.load() != gen) break;
            double dur = 0.0;
            uint32_t w = 0, h = 0;
            HBITMAP bmp = DecodeClipThumbnail(p, tw, th, &dur, &w, &h);
            if (thumbStop_.load() || thumbGen_.load() != gen) {
                if (bmp) DeleteObject(bmp);
                break;
            }
            if (bmp) {
                auto* m = new ThumbMsg{ gen, p, bmp, dur, w, h };
                if (!PostMessageW(target, WM_MW_THUMB_READY, 0, reinterpret_cast<LPARAM>(m))) {
                    DeleteObject(bmp);
                    delete m;
                }
            }
        }
    });
}

void MainWindow::RebuildFilteredView(const std::wstring& preferPath) {
    std::wstring want = preferPath;
    if (want.empty() && selectedIdx_ >= 0 && selectedIdx_ < static_cast<int>(filteredIndices_.size())) {
        want = clips_[filteredIndices_[static_cast<size_t>(selectedIdx_)]].filePath;
    }

    std::wstring qLower = ToLowerStr(searchQuery_);
    filteredIndices_.clear();
    filteredIndices_.reserve(clips_.size());
    for (size_t i = 0; i < clips_.size(); ++i) {
        const auto& c = clips_[i];
        if (favOnly_ && !c.isFavorite) continue;
        if (!qLower.empty()) {
            std::wstring nameLower = ToLowerStr(c.fileName);
            if (nameLower.find(qLower) == std::wstring::npos) continue;
        }
        filteredIndices_.push_back(i);
    }

    selectedIdx_ = filteredIndices_.empty() ? -1 : 0;
    if (!want.empty()) {
        for (size_t vi = 0; vi < filteredIndices_.size(); ++vi) {
            if (_wcsicmp(clips_[filteredIndices_[vi]].filePath.c_str(), want.c_str()) == 0) {
                selectedIdx_ = static_cast<int>(vi);
                break;
            }
        }
    }
    hoverCardIdx_ = -1;
    ClampScroll();
    StartThumbnailWorker();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RefreshClips(const std::wstring& selectPath) {
    std::wstring want = selectPath;
    if (want.empty() && selectedIdx_ >= 0 && selectedIdx_ < static_cast<int>(filteredIndices_.size())) {
        want = clips_[filteredIndices_[static_cast<size_t>(selectedIdx_)]].filePath;
    }

    clips_ = ScanClipsFolder(app_.Config().outputDir, GetFavoritesFilePath());
    RebuildFilteredView(want);
}

// ================= EMBEDDED MFPLAY PLAYER =================

LRESULT CALLBACK MainWindow::VideoHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (self) {
        switch (msg) {
            case WM_ERASEBKGND:
                if (self->player_ && self->playerReady_) return 1;
                {
                    RECT rc{}; GetClientRect(hwnd, &rc);
                    HBRUSH b = CreateSolidBrush(RGB(8, 10, 15));
                    FillRect(reinterpret_cast<HDC>(wParam), &rc, b);
                    DeleteObject(b);
                    return 1;
                }
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                BeginPaint(hwnd, &ps);
                if (self->player_) self->player_->UpdateVideo();
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_SIZE:
                if (self->player_) self->player_->UpdateVideo();
                return 0;
            case WM_LBUTTONUP:
                self->TogglePlayerPlayPause();
                SetFocus(self->hwnd_);
                return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void MainWindow::OpenEmbeddedPlayer(int viewIdx) {
    if (viewIdx < 0 || viewIdx >= static_cast<int>(filteredIndices_.size()) || !hwnd_) return;
    CloseEmbeddedPlayer();

    static bool vReg = false;
    const wchar_t* vCls = L"AnlikKlipVideoHost";
    if (!vReg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = MainWindow::VideoHostProc;
        wc.hInstance = hInst_;
        wc.lpszClassName = vCls;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        RegisterClassExW(&wc);
        vReg = true;
    }

    const size_t rawIdx = filteredIndices_[static_cast<size_t>(viewIdx)];
    const auto& clip = clips_[rawIdx];
    selectedIdx_ = viewIdx;
    playerClipIdx_ = static_cast<int>(rawIdx);
    playerClipPath_ = clip.filePath;
    playerClipTitle_ = StripMp4Ext(clip.fileName);
    playerDurSec_ = clip.durationSeconds;
    playerPosSec_ = 0.0;
    playerReady_ = false;
    playerPlaying_ = false;
    playerEnded_ = false;
    playerRate_ = 1.0f;
    trimMode_ = false;
    trimStartSec_ = 0.0;
    trimEndSec_ = (clip.durationSeconds > 0.2) ? clip.durationSeconds : 1.0;
    draggingTrim_ = 0;

    videoHwnd_ = CreateWindowExW(0, vCls, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        S(250), S(62), S(640), S(360), hwnd_, nullptr, hInst_, this);
    if (!videoHwnd_) return;

    ++playerGen_;
    playerCb_ = new PlayerCallbackImpl(hwnd_, playerGen_);
    HRESULT hr = MFPCreateMediaPlayer(
        playerClipPath_.c_str(), FALSE, MFP_OPTION_NONE,
        playerCb_, videoHwnd_, &player_);
    if (FAILED(hr) || !player_) {
        Logger::Log("OpenEmbeddedPlayer failed hr=0x%08lx", (unsigned long)hr);
        CloseEmbeddedPlayer();
        SetFooterMessage(L"Klip uygulama içinde açılamadı, dış oynatıcı deneniyor.");
        PlayExternalSelected();
        return;
    }

    playerOpen_ = true;
    UpdateGalleryControlVisibility();
    SetTimer(hwnd_, TIMER_PLAYER_ID, 80, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CloseEmbeddedPlayer() {
    StopTrimWorker();
    if (hwnd_) KillTimer(hwnd_, TIMER_PLAYER_ID);
    ++playerGen_;
    if (playerCb_) {
        playerCb_->Detach();
        playerCb_->Release();
        playerCb_ = nullptr;
    }
    if (player_) {
        player_->Shutdown();
        player_->Release();
        player_ = nullptr;
    }
    if (videoHwnd_) {
        DestroyWindow(videoHwnd_);
        videoHwnd_ = nullptr;
    }
    playerOpen_ = false;
    playerReady_ = false;
    playerPlaying_ = false;
    playerEnded_ = false;
    draggingSeek_ = false;
    draggingVol_ = false;
    trimMode_ = false;
    draggingTrim_ = 0;
    trimBusy_ = false;
    UpdateGalleryControlVisibility();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::StopTrimWorker() {
    trimCancel_.store(true);
    if (trimThread_.joinable()) {
        trimThread_.join();
    }
    trimCancel_.store(false);
    trimBusy_ = false;
}

void MainWindow::ToggleTrimMode() {
    if (!playerOpen_ || trimBusy_) return;
    trimMode_ = !trimMode_;
    if (trimMode_) {
        double dur = (playerDurSec_ > 0.2) ? playerDurSec_ : 1.0;
        trimStartSec_ = 0.0;
        trimEndSec_ = dur;
    }
    RECT ctrlRc{ S(242), S(428), S(900), S(606) };
    InvalidateRect(hwnd_, &ctrlRc, FALSE);
}

void MainWindow::StartTrimAsync(bool replaceOriginal) {
    if (!playerOpen_ || trimBusy_ || playerClipPath_.empty()) return;
    double dur = (playerDurSec_ > 0.2) ? playerDurSec_ : 1.0;
    double tStart = std::max(0.0, std::min(dur - 0.2, trimStartSec_));
    double tEnd   = std::max(tStart + 0.2, std::min(dur, trimEndSec_));
    if (tEnd - tStart < 0.2) {
        SetFooterMessage(L"Kırpma aralığı en az 0.2 saniye olmalı.");
        return;
    }

    StopTrimWorker();
    trimCancel_.store(false);
    trimBusy_ = true;

    // Release active MFPlay reader/decoder handle cleanly so the source MP4 has zero lock
    if (hwnd_) KillTimer(hwnd_, TIMER_PLAYER_ID);
    ++playerGen_;
    if (playerCb_) {
        playerCb_->Detach();
        playerCb_->Release();
        playerCb_ = nullptr;
    }
    if (player_) {
        player_->Shutdown();
        player_->Release();
        player_ = nullptr;
    }
    playerReady_ = false;
    playerPlaying_ = false;
    if (videoHwnd_) ShowWindow(videoHwnd_, SW_HIDE);

    const std::wstring origPath = playerClipPath_;
    const std::wstring outPath = replaceOriginal
        ? (origPath + L".trim.tmp.mp4")
        : BuildTrimmedCopyPath(origPath);

    SetFooterMessage(replaceOriginal
        ? L"Ana klip kırpılıyor, lütfen bekleyin..."
        : L"Kesitten yeni kopya oluşturuluyor...");

    HWND target = hwnd_;
    trimThread_ = std::thread([this, target, origPath, outPath, tStart, tEnd, replaceOriginal]() {
        ComScope com(COINIT_MULTITHREADED);
        std::string errUtf8;
        bool ok = TrimMp4Clip(origPath, outPath, tStart, tEnd, errUtf8, &trimCancel_);
        if (trimCancel_.load()) {
            DeleteFileW(outPath.c_str());
            return;
        }
        auto* m = new TrimDoneMsg{};
        m->ok = ok;
        m->replaceOriginal = replaceOriginal;
        m->origPath = origPath;
        m->outPath = outPath;
        if (!ok) {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, errUtf8.c_str(), -1, nullptr, 0);
            std::wstring werr(wlen > 0 ? wlen - 1 : 0, L'\0');
            if (wlen > 1) MultiByteToWideChar(CP_UTF8, 0, errUtf8.c_str(), -1, &werr[0], wlen);
            m->message = L"Kırpma başarısız: " + werr;
        }
        if (!PostMessageW(target, WM_MW_TRIM_DONE, 0, reinterpret_cast<LPARAM>(m))) {
            delete m;
        }
    });
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::TogglePlayerPlayPause() {
    if (!player_ || !playerReady_) return;
    if (playerEnded_) {
        SeekPlayerAbsolute(0.0);
        player_->Play();
        playerPlaying_ = true;
        playerEnded_ = false;
    } else if (playerPlaying_) {
        player_->Pause();
        playerPlaying_ = false;
    } else {
        player_->Play();
        playerPlaying_ = true;
    }
    RECT ctrlRc{ S(242), S(428), S(900), S(606) };
    InvalidateRect(hwnd_, &ctrlRc, FALSE);
}

void MainWindow::SeekPlayerAbsolute(double targetSec) {
    if (!player_ || !playerReady_) return;
    double dur = playerDurSec_ > 0.0 ? playerDurSec_ : 1.0;
    targetSec = std::max(0.0, std::min(dur, targetSec));

    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = static_cast<LONGLONG>(targetSec * 1e7);
    if (SUCCEEDED(player_->SetPosition(MFP_POSITIONTYPE_100NS, &pos))) {
        playerPosSec_ = targetSec;
        playerEnded_ = false;
    }
    PropVariantClear(&pos);

    RECT ctrlRc{ S(242), S(428), S(900), S(606) };
    InvalidateRect(hwnd_, &ctrlRc, FALSE);
}

void MainWindow::SeekPlayerRelative(double deltaSec) {
    SeekPlayerAbsolute(playerPosSec_ + deltaSec);
}

void MainWindow::SetPlayerVolume(float vol01) {
    playerVol_ = std::max(0.0f, std::min(1.0f, vol01));
    if (player_) {
        player_->SetVolume(playerVol_);
        if (playerVol_ > 0.01f && playerMuted_) {
            playerMuted_ = false;
            player_->SetMute(FALSE);
        }
    }
    RECT ctrlRc{ S(242), S(428), S(900), S(606) };
    InvalidateRect(hwnd_, &ctrlRc, FALSE);
}

void MainWindow::TogglePlayerMute() {
    playerMuted_ = !playerMuted_;
    if (player_) player_->SetMute(playerMuted_ ? TRUE : FALSE);
    RECT ctrlRc{ S(242), S(428), S(900), S(606) };
    InvalidateRect(hwnd_, &ctrlRc, FALSE);
}

void MainWindow::CyclePlayerRate() {
    if (playerRate_ < 0.75f) playerRate_ = 1.0f;
    else if (playerRate_ < 1.25f) playerRate_ = 1.5f;
    else if (playerRate_ < 1.75f) playerRate_ = 2.0f;
    else playerRate_ = 0.5f;
    if (player_) player_->SetRate(playerRate_);
    RECT ctrlRc{ S(242), S(428), S(900), S(606) };
    InvalidateRect(hwnd_, &ctrlRc, FALSE);
}

void MainWindow::UpdatePlayerMetrics() {
    if (!player_ || !playerReady_) return;
    if (!draggingSeek_ && draggingTrim_ == 0) {
        PROPVARIANT pos;
        PropVariantInit(&pos);
        if (SUCCEEDED(player_->GetPosition(MFP_POSITIONTYPE_100NS, &pos)) && pos.vt == VT_I8) {
            playerPosSec_ = static_cast<double>(pos.hVal.QuadPart) / 1e7;
        }
        PropVariantClear(&pos);
    }
    MFP_MEDIAPLAYER_STATE st = MFP_MEDIAPLAYER_STATE_EMPTY;
    if (SUCCEEDED(player_->GetState(&st))) {
        playerPlaying_ = (st == MFP_MEDIAPLAYER_STATE_PLAYING);
    }
    RECT ctrlRc{ S(242), S(428), S(900), S(606) };
    InvalidateRect(hwnd_, &ctrlRc, FALSE);
}

RECT MainWindow::GetSeekBarRect() const {
    return RECT{ S(254), S(442), S(886), S(466) };
}

RECT MainWindow::GetVolumeBarRect() const {
    return RECT{ S(780), S(500), S(886), S(524) };
}

// ================= GALLERY & HIT TESTING =================

RECT MainWindow::GetGridViewportRect() const {
    return RECT{ S(242), S(82), S(898), S(534) };
}

void MainWindow::ClampScroll() {
    RECT vp = GetGridViewportRect();
    int vpH = vp.bottom - vp.top;
    int rows = (static_cast<int>(filteredIndices_.size()) + 1) / 2;
    int totalH = rows > 0 ? (rows * S(226) + (rows - 1) * S(14)) : 0;
    maxScrollY_ = std::max(0, totalH - vpH);
    scrollY_ = std::max(0, std::min(maxScrollY_, scrollY_));
}

int MainWindow::HitTestCard(int mx, int my) const {
    if (activeTab_ != 0 || playerOpen_) return -1;
    RECT vp = GetGridViewportRect();
    if (mx < vp.left || mx > vp.right - S(12) || my < vp.top || my > vp.bottom) return -1;

    const int cardW = S(312);
    const int cardH = S(226);
    const int gapX = S(16);
    const int gapY = S(14);

    for (size_t vi = 0; vi < filteredIndices_.size(); ++vi) {
        int col = static_cast<int>(vi % 2);
        int row = static_cast<int>(vi / 2);
        int x = vp.left + col * (cardW + gapX);
        int y = vp.top + row * (cardH + gapY) - scrollY_;
        if (mx >= x && mx <= x + cardW && my >= y && my <= y + cardH) {
            return static_cast<int>(vi);
        }
    }
    return -1;
}

int MainWindow::HitTestCardStar(int mx, int my) const {
    if (activeTab_ != 0 || playerOpen_) return -1;
    RECT vp = GetGridViewportRect();
    if (mx < vp.left || mx > vp.right - S(12) || my < vp.top || my > vp.bottom) return -1;

    const int cardW = S(312);
    const int cardH = S(226);
    const int gapX = S(16);
    const int gapY = S(14);

    for (size_t vi = 0; vi < filteredIndices_.size(); ++vi) {
        int col = static_cast<int>(vi % 2);
        int row = static_cast<int>(vi / 2);
        int cx = vp.left + col * (cardW + gapX);
        int cy = vp.top + row * (cardH + gapY) - scrollY_;
        int tx = cx + S(7), ty = cy + S(7), tw = S(298);
        int sx = tx + tw - S(32), sy = ty + S(6);
        if (mx >= sx && mx <= sx + S(26) && my >= sy && my <= sy + S(24)) {
            return static_cast<int>(vi);
        }
    }
    return -1;
}

int MainWindow::HitTestTrimHandle(int mx, int my) const {
    if (activeTab_ != 0 || !playerOpen_ || !trimMode_ || trimBusy_) return 0;
    RECT sk = GetSeekBarRect();
    if (my < sk.top - S(4) || my > sk.bottom + S(4)) return 0;
    double dur = (playerDurSec_ > 0.05) ? playerDurSec_ : 1.0;
    int skW = sk.right - sk.left;
    int xStart = sk.left + static_cast<int>(skW * std::max(0.0, std::min(1.0, trimStartSec_ / dur)));
    int xEnd   = sk.left + static_cast<int>(skW * std::max(0.0, std::min(1.0, trimEndSec_ / dur)));
    int dStart = std::abs(mx - xStart);
    int dEnd   = std::abs(mx - xEnd);
    const int tol = S(11);
    if (dStart <= tol && dStart <= dEnd) return 1;
    if (dEnd <= tol) return 2;
    return 0;
}

int MainWindow::HitTestCustomButton(int mx, int my) const {
    for (const auto& b : customButtons_) {
        if (b.enabled && mx >= b.rc.left && mx <= b.rc.right && my >= b.rc.top && my <= b.rc.bottom) {
            return b.id;
        }
    }
    return 0;
}

void MainWindow::UpdateHover(int mx, int my) {
    int nb = HitTestCustomButton(mx, my);
    int nc = HitTestCard(mx, my);
    if (nb != hoverBtnId_ || nc != hoverCardIdx_) {
        hoverBtnId_ = nb;
        hoverCardIdx_ = nc;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::ToggleFavoriteForViewIdx(int viewIdx) {
    int vi = (viewIdx >= 0) ? viewIdx : selectedIdx_;
    if (vi < 0 || vi >= static_cast<int>(filteredIndices_.size())) return;
    size_t rawIdx = filteredIndices_[static_cast<size_t>(vi)];
    ClipItem& item = clips_[rawIdx];
    item.isFavorite = !item.isFavorite;
    SetClipFavorite(GetFavoritesFilePath(), item.fileName, item.isFavorite);
    SetFooterMessage(item.isFavorite ? L"Klip favorilere eklendi." : L"Klip favorilerden çıkarıldı.");
    if (favOnly_ && !playerOpen_) {
        RebuildFilteredView();
    } else if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::PlaySelected() {
    if (selectedIdx_ < 0 || selectedIdx_ >= static_cast<int>(filteredIndices_.size())) return;
    OpenEmbeddedPlayer(selectedIdx_);
}

void MainWindow::PlayExternalSelected() {
    if (selectedIdx_ < 0 || selectedIdx_ >= static_cast<int>(filteredIndices_.size())) return;
    const auto& c = clips_[filteredIndices_[static_cast<size_t>(selectedIdx_)]];
    ShellExecuteW(nullptr, L"open", c.filePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::RevealSelected() {
    if (selectedIdx_ < 0 || selectedIdx_ >= static_cast<int>(filteredIndices_.size())) return;
    const auto& c = clips_[filteredIndices_[static_cast<size_t>(selectedIdx_)]];
    std::wstring param = L"/select,\"" + c.filePath + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
}

void MainWindow::RenameSelected() {
    if (selectedIdx_ < 0 || selectedIdx_ >= static_cast<int>(filteredIndices_.size())) return;
    const auto& c = clips_[filteredIndices_[static_cast<size_t>(selectedIdx_)]];
    const std::wstring oldPath = c.filePath;
    const std::wstring oldName = c.fileName;
    std::wstring stem = StripMp4Ext(oldName);
    if (!ShowRenamePopup(hwnd_, hInst_, dpi_, font_, stem)) return;

    bool wasPlayingThis = (playerOpen_ && _wcsicmp(playerClipPath_.c_str(), oldPath.c_str()) == 0);
    if (wasPlayingThis) CloseEmbeddedPlayer();

    std::wstring newPath;
    if (RenameClipFile(oldPath, stem, newPath)) {
        size_t sl = newPath.find_last_of(L"\\/");
        std::wstring newName = (sl != std::wstring::npos) ? newPath.substr(sl + 1) : newPath;
        UpdateFavoriteOnRename(GetFavoritesFilePath(), oldName, newName);
        InvalidateClipMetadata(oldPath);

        auto it = thumbBmps_.find(oldPath);
        if (it != thumbBmps_.end()) {
            thumbBmps_[newPath] = it->second;
            thumbBmps_.erase(it);
        }
        SetFooterMessage(L"Klip adı güncellendi.");
        RefreshClips(newPath);
        if (wasPlayingThis && selectedIdx_ >= 0) OpenEmbeddedPlayer(selectedIdx_);
    } else {
        MessageBoxW(hwnd_, L"Klip adı değiştirilemedi.", L"Anlık Klip", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::DeleteSelected() {
    if (selectedIdx_ < 0 || selectedIdx_ >= static_cast<int>(filteredIndices_.size())) return;
    const auto c = clips_[filteredIndices_[static_cast<size_t>(selectedIdx_)]];
    std::wstring msg = L"'" + c.fileName + L"' çöp kutusuna gönderilsin mi?";
    if (MessageBoxW(hwnd_, msg.c_str(), L"Klibi Sil", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    if (playerOpen_ && _wcsicmp(playerClipPath_.c_str(), c.filePath.c_str()) == 0) {
        CloseEmbeddedPlayer();
    }

    if (TrashClipFile(c.filePath)) {
        SetClipFavorite(GetFavoritesFilePath(), c.fileName, false);
        InvalidateClipMetadata(c.filePath);
        auto it = thumbBmps_.find(c.filePath);
        if (it != thumbBmps_.end()) {
            if (it->second) DeleteObject(it->second);
            thumbBmps_.erase(it);
        }
        SetFooterMessage(L"Klip çöp kutusuna gönderildi.");
        RefreshClips();
    }
}

void MainWindow::PickOutputFolder() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
        IID_IFileOpenDialog, dlg.PutVoid())) || !dlg) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Klip Kayıt Klasörünü Seçin");
    if (SUCCEEDED(dlg->Show(hwnd_))) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(dlg->GetResult(item.Put())) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                SetWindowTextW(edtOutDir_, p);
                CoTaskMemFree(p);
            }
        }
    }
}

void MainWindow::OpenFolder(const std::wstring& path) {
    EnsureDirectoryExists(path);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::SetFooterMessage(const std::wstring& msg) {
    footerMsg_ = msg;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::PaintWindow(HDC targetDc) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int cw = client.right - client.left;
    const int ch = client.bottom - client.top;
    if (cw <= 0 || ch <= 0) return;

    HDC memDc = CreateCompatibleDC(targetDc);
    HBITMAP backBmp = CreateCompatibleBitmap(targetDc, cw, ch);
    HGDIOBJ oldBmp = SelectObject(memDc, backBmp);

    {
        Gdiplus::Graphics g(memDc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        g.Clear(Gc(13, 16, 23));

        // Sidebar panel
        Gdiplus::SolidBrush sbSide(Gc(19, 23, 32));
        g.FillRectangle(&sbSide, 0, 0, S(220), ch);
        Gdiplus::Pen sepPen(Gc(38, 46, 64), 1.0f);
        g.DrawLine(&sepPen, S(220), 0, S(220), ch);

        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font fTitle(&ff, 14.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::Font fSub(&ff, 8.8f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
        Gdiplus::Font fBold(&ff, 9.8f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::Font fMed(&ff, 9.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
        Gdiplus::Font fSmall(&ff, 8.3f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);

        Gdiplus::SolidBrush brText(Gc(238, 242, 248));
        Gdiplus::SolidBrush brMuted(Gc(148, 160, 180));
        Gdiplus::StringFormat sfLeft;
        sfLeft.SetAlignment(Gdiplus::StringAlignmentNear);
        sfLeft.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        sfLeft.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        sfLeft.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        Gdiplus::StringFormat sfCenter;
        sfCenter.SetAlignment(Gdiplus::StringAlignmentCenter);
        sfCenter.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        sfCenter.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        // Brand emblem
        FillRoundRect(g, (float)S(16), (float)S(18), (float)S(36), (float)S(36), (float)S(10), Gc(37, 99, 235));
        {
            Gdiplus::SolidBrush brDot(Gc(255, 255, 255));
            g.FillEllipse(&brDot, (float)S(26), (float)S(28), (float)S(16), (float)S(16));
            Gdiplus::SolidBrush brInner(app_.IsRunning() ? Gc(239, 68, 68) : Gc(100, 116, 139));
            g.FillEllipse(&brInner, (float)S(29), (float)S(31), (float)S(10), (float)S(10));
        }
        g.DrawString(L"Anlık Klip", -1, &fTitle, Gdiplus::RectF((float)S(60), (float)S(16), (float)S(146), (float)S(24)), &sfLeft, &brText);
        wchar_t resSub[48] = {};
        swprintf(resSub, 48, L"%dp • %d FPS", app_.Config().height, app_.Config().fps);
        g.DrawString(resSub, -1, &fSub, Gdiplus::RectF((float)S(60), (float)S(38), (float)S(146), (float)S(18)), &sfLeft, &brMuted);

        // Live status capsule
        AppRuntimeStats st = app_.GetStats();
        bool recOn = st.running;
        FillRoundRect(g, (float)S(16), (float)S(72), (float)S(188), (float)S(28), (float)S(14),
            recOn ? Gc(20, 83, 45, 160) : Gc(51, 65, 85, 160));
        StrokeRoundRect(g, (float)S(16), (float)S(72), (float)S(188), (float)S(28), (float)S(14),
            recOn ? Gc(34, 197, 94, 180) : Gc(100, 116, 139, 160));
        {
            Gdiplus::SolidBrush dotB(recOn ? Gc(74, 222, 128) : Gc(148, 163, 184));
            g.FillEllipse(&dotB, (float)S(28), (float)S(82), (float)S(8), (float)S(8));
            Gdiplus::SolidBrush statusTxtB(recOn ? Gc(187, 247, 208) : Gc(203, 213, 225));
            g.DrawString(recOn ? L"Kayıt Aktif" : L"Duraklatıldı", -1, &fSub,
                Gdiplus::RectF((float)S(42), (float)S(72), (float)S(154), (float)S(28)), &sfLeft, &statusTxtB);
        }

        customButtons_.clear();
        auto addBtn = [&](int id, int x, int y, int w, int h, const std::wstring& label,
                          Gdiplus::Color baseCol, Gdiplus::Color hoverCol, Gdiplus::Color borderCol,
                          bool enabled = true, bool activeTabBtn = false, const std::wstring& badge = L"") {
            RECT rc{ S(x), S(y), S(x + w), S(y + h) };
            customButtons_.push_back({ id, rc, enabled });
            bool hov = enabled && (hoverBtnId_ == id);
            bool prs = enabled && (pressedBtnId_ == id);
            Gdiplus::Color fill = !enabled ? Gc(28, 33, 45) : (prs ? hoverCol : (hov ? hoverCol : baseCol));
            FillRoundRect(g, (float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), (float)S(8), fill);
            if (borderCol.GetA() > 0) {
                StrokeRoundRect(g, (float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), (float)S(8), borderCol);
            }
            if (activeTabBtn) {
                FillRoundRect(g, (float)rc.left + S(4), (float)rc.top + S(8), (float)S(3), (float)(rc.bottom - rc.top - S(16)), 1.5f, Gc(96, 165, 250));
            }
            Gdiplus::SolidBrush tb(!enabled ? Gc(100, 112, 132) : Gc(242, 246, 252));
            if (!badge.empty()) {
                g.DrawString(label.c_str(), -1, &fBold,
                    Gdiplus::RectF((float)rc.left + S(14), (float)rc.top, (float)(rc.right - rc.left - S(58)), (float)(rc.bottom - rc.top)),
                    &sfLeft, &tb);
                FillRoundRect(g, (float)(rc.right - S(40)), (float)(rc.top + S(8)), (float)S(30), (float)S(20), (float)S(10), Gc(37, 99, 235, 190));
                g.DrawString(badge.c_str(), -1, &fSmall,
                    Gdiplus::RectF((float)(rc.right - S(40)), (float)(rc.top + S(8)), (float)S(30), (float)S(20)),
                    &sfCenter, &tb);
            } else if (id == BTN_TAB_CLIPS || id == BTN_TAB_SETTINGS) {
                g.DrawString(label.c_str(), -1, &fBold,
                    Gdiplus::RectF((float)rc.left + S(14), (float)rc.top, (float)(rc.right - rc.left - S(20)), (float)(rc.bottom - rc.top)),
                    &sfLeft, &tb);
            } else {
                g.DrawString(label.c_str(), -1, &fBold,
                    Gdiplus::RectF((float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top)),
                    &sfCenter, &tb);
            }
        };

        // Sidebar tabs
        wchar_t cntStr[16] = {};
        swprintf(cntStr, 16, L"%zu", clips_.size());
        addBtn(BTN_TAB_CLIPS, 16, 118, 188, 36, L"Kliplerim",
            activeTab_ == 0 ? Gc(30, 41, 66) : Gc(19, 23, 32), Gc(34, 45, 70),
            activeTab_ == 0 ? Gc(59, 130, 246, 180) : Gc(0, 0, 0, 0), true, activeTab_ == 0, cntStr);
        addBtn(BTN_TAB_SETTINGS, 16, 160, 188, 36, L"Ayarlar",
            activeTab_ == 1 ? Gc(30, 41, 66) : Gc(19, 23, 32), Gc(34, 45, 70),
            activeTab_ == 1 ? Gc(59, 130, 246, 180) : Gc(0, 0, 0, 0), true, activeTab_ == 1);

        // Telemetry card in sidebar
        FillRoundRect(g, (float)S(16), (float)S(358), (float)S(188), (float)S(120), (float)S(10), Gc(26, 32, 44));
        StrokeRoundRect(g, (float)S(16), (float)S(358), (float)S(188), (float)S(120), (float)S(10), Gc(44, 54, 72));
        double bufMb = static_cast<double>(st.ring.videoBytes + st.ring.audioBytes) / (1024.0 * 1024.0);
        wchar_t t1[64], t2[64], t3[64], t4[64];
        swprintf(t1, 64, L"RAM: %.1f MB", st.workingSetMb);
        swprintf(t2, 64, L"Tampon: %.1f MB", bufMb);
        swprintf(t3, 64, L"Süre: %.0f / %d sn", st.ring.bufferedSeconds, app_.Config().clipSeconds);
        std::wstring hk = FormatHotkey(app_.Config().hotkeyMods, app_.Config().hotkeyVk);
        swprintf(t4, 64, L"Kısayol: %ls", hk.c_str());
        g.DrawString(t1, -1, &fSub, Gdiplus::RectF((float)S(28), (float)S(368), (float)S(164), (float)S(20)), &sfLeft, &brText);
        g.DrawString(t2, -1, &fSub, Gdiplus::RectF((float)S(28), (float)S(392), (float)S(164), (float)S(20)), &sfLeft, &brMuted);
        g.DrawString(t3, -1, &fSub, Gdiplus::RectF((float)S(28), (float)S(416), (float)S(164), (float)S(20)), &sfLeft, &brMuted);
        FillRoundRect(g, (float)S(28), (float)S(443), (float)S(164), (float)S(24), (float)S(6), Gc(37, 99, 235, 55));
        Gdiplus::SolidBrush brBlueTxt(Gc(147, 197, 253));
        g.DrawString(t4, -1, &fSmall, Gdiplus::RectF((float)S(28), (float)S(443), (float)S(164), (float)S(24)), &sfCenter, &brBlueTxt);

        // Quick controls at bottom of sidebar
        addBtn(BTN_SIDE_SAVE, 16, 492, 188, 38, L"●  Hemen Klip Al",
            Gc(37, 99, 235), Gc(59, 130, 246), Gc(96, 165, 250, 150), st.running && !st.saving);
        addBtn(BTN_SIDE_TOGGLE, 16, 538, 90, 32, st.running ? L"Duraklat" : L"Başlat",
            st.running ? Gc(30, 37, 52) : Gc(22, 163, 74),
            st.running ? Gc(42, 51, 70) : Gc(34, 197, 94), Gc(52, 64, 86));
        addBtn(BTN_SIDE_MIC, 114, 538, 90, 32, app_.Config().micMuted ? L"Mik: Sustu" : L"Mik: Açık",
            app_.Config().micMuted ? Gc(180, 83, 9) : Gc(30, 37, 52),
            app_.Config().micMuted ? Gc(217, 119, 6) : Gc(42, 51, 70), Gc(52, 64, 86));
        g.DrawString(L"X ile kapatınca tepside çalışır", -1, &fSmall,
            Gdiplus::RectF((float)S(16), (float)S(582), (float)S(188), (float)S(20)), &sfCenter, &brMuted);

        if (activeTab_ == 0 && playerOpen_) {
            // ================= TAB 0 (EMBEDDED VIDEO PLAYER + TRIM) =================
            bool curFav = false;
            for (const auto& cc : clips_) {
                if (_wcsicmp(cc.filePath.c_str(), playerClipPath_.c_str()) == 0) {
                    curFav = cc.isFavorite;
                    break;
                }
            }
            addBtn(BTN_PLY_BACK, 242, 18, 114, 34, L"←  Kliplere", Gc(28, 34, 47), Gc(40, 49, 68), Gc(54, 66, 90), !trimBusy_);
            g.DrawString(playerClipTitle_.c_str(), -1, &fBold,
                Gdiplus::RectF((float)S(364), (float)S(16), (float)S(194), (float)S(20)), &sfLeft, &brText);
            g.DrawString(L"Space: Oynat • ←/→: ±5sn • Esc: Geri", -1, &fSmall,
                Gdiplus::RectF((float)S(364), (float)S(36), (float)S(198), (float)S(16)), &sfLeft, &brMuted);

            addBtn(BTN_PLY_FAV, 564, 18, 74, 34, curFav ? L"★ Fav" : L"☆ Fav",
                curFav ? Gc(92, 68, 14) : Gc(28, 34, 47),
                curFav ? Gc(120, 88, 18) : Gc(40, 49, 68),
                curFav ? Gc(250, 204, 21) : Gc(52, 64, 86), !trimBusy_);
            addBtn(BTN_PLY_RENAME, 644, 18, 84, 34, L"Adlandır", Gc(28, 34, 47), Gc(40, 49, 68), Gc(52, 64, 86), !trimBusy_);
            addBtn(BTN_PLY_FOLDER, 734, 18, 94, 34, L"Klasörde Aç", Gc(28, 34, 47), Gc(40, 49, 68), Gc(52, 64, 86), !trimBusy_);
            addBtn(BTN_PLY_DELETE, 834, 18, 64, 34, L"Sil", Gc(153, 27, 27), Gc(185, 28, 28), Gc(220, 38, 38), !trimBusy_);

            // Frame border around child video HWND (250, 62, 640, 360)
            StrokeRoundRect(g, (float)S(249), (float)S(61), (float)S(642), (float)S(362), 4.0f, Gc(45, 55, 74), 1.0f);
            if (trimBusy_) {
                FillRoundRect(g, (float)S(250), (float)S(62), (float)S(640), (float)S(360), 4.0f, Gc(15, 19, 27));
                g.DrawString(L"✂  Klip kırpılıyor, lütfen bekleyin...", -1, &fTitle,
                    Gdiplus::RectF((float)S(250), (float)S(62), (float)S(640), (float)S(360)), &sfCenter, &brText);
            }

            // Control card below video (242, 430, 656, 174)
            FillRoundRect(g, (float)S(242), (float)S(430), (float)S(656), (float)S(174), (float)S(12), Gc(20, 25, 35));
            StrokeRoundRect(g, (float)S(242), (float)S(430), (float)S(656), (float)S(174), (float)S(12), Gc(40, 48, 65));

            // Seekbar track + optional trim highlight & handles
            RECT sk = GetSeekBarRect();
            float skW = static_cast<float>(sk.right - sk.left);
            float trackY = static_cast<float>(sk.top + (sk.bottom - sk.top) / 2 - S(3));
            float trackH = static_cast<float>(S(6));
            FillRoundRect(g, (float)sk.left, trackY, skW, trackH, (float)S(3), Gc(42, 51, 68));

            double dur = (playerDurSec_ > 0.05) ? playerDurSec_ : 1.0;
            double ratio = std::max(0.0, std::min(1.0, playerPosSec_ / dur));
            float fillW = static_cast<float>(skW * ratio);
            if (fillW > 2.0f) {
                FillRoundRect(g, (float)sk.left, trackY, fillW, trackH, (float)S(3), Gc(59, 130, 246));
            }
            if (trimMode_) {
                double sr0 = std::max(0.0, std::min(1.0, trimStartSec_ / dur));
                double sr1 = std::max(sr0, std::min(1.0, trimEndSec_ / dur));
                float xs = static_cast<float>(sk.left) + static_cast<float>(skW * sr0);
                float xe = static_cast<float>(sk.left) + static_cast<float>(skW * sr1);
                if (xe - xs >= 2.0f) {
                    FillRoundRect(g, xs, trackY - S(1), xe - xs, trackH + S(2), (float)S(3), Gc(16, 185, 129, 200));
                }
                FillRoundRect(g, xs - S(4), trackY - S(7), (float)S(8), (float)S(20), (float)S(3), Gc(34, 197, 94));
                FillRoundRect(g, xe - S(4), trackY - S(7), (float)S(8), (float)S(20), (float)S(3), Gc(245, 158, 11));
            }

            float knobX = static_cast<float>(sk.left) + fillW;
            float knobR = static_cast<float>(draggingSeek_ ? S(8) : S(6));
            Gdiplus::SolidBrush knobGlow(Gc(59, 130, 246, 90));
            g.FillEllipse(&knobGlow, knobX - knobR - S(3), trackY + trackH * 0.5f - knobR - S(3), (knobR + S(3)) * 2.0f, (knobR + S(3)) * 2.0f);
            Gdiplus::SolidBrush knobBr(Gc(248, 250, 252));
            g.FillEllipse(&knobBr, knobX - knobR, trackY + trackH * 0.5f - knobR, knobR * 2.0f, knobR * 2.0f);

            // Time display & trim range label
            wchar_t timeTxt[64] = {};
            swprintf(timeTxt, 64, L"%ls  /  %ls",
                FormatDurClock(playerPosSec_).c_str(), FormatDurClock(playerDurSec_).c_str());
            g.DrawString(timeTxt, -1, &fBold,
                Gdiplus::RectF((float)S(254), (float)S(468), (float)S(150), (float)S(22)), &sfLeft, &brText);

            if (trimMode_) {
                double spanSec = std::max(0.0, trimEndSec_ - trimStartSec_);
                wchar_t trimInfo[128] = {};
                swprintf(trimInfo, 128, L"Seçili kesit: %ls  →  %ls  (%.1f sn)",
                    FormatDurPrecise(trimStartSec_).c_str(), FormatDurPrecise(trimEndSec_).c_str(), spanSec);
                Gdiplus::SolidBrush brMint(Gc(110, 231, 183));
                g.DrawString(trimInfo, -1, &fSmall,
                    Gdiplus::RectF((float)S(410), (float)S(468), (float)S(476), (float)S(22)), &sfLeft, &brMint);
            } else if (!playerReady_ && !trimBusy_) {
                g.DrawString(L"Video hazırlanıyor...", -1, &fSmall,
                    Gdiplus::RectF((float)S(416), (float)S(468), (float)S(200), (float)S(22)), &sfLeft, &brMuted);
            }

            // Transport buttons
            bool canPlay = playerReady_ && !trimBusy_;
            addBtn(BTN_PLY_PLAYPAUSE, 254, 494, 102, 34,
                playerPlaying_ ? L"⏸  Duraklat" : (playerEnded_ ? L"↻  Tekrar" : L"▶  Oynat"),
                Gc(37, 99, 235), Gc(59, 130, 246), Gc(96, 165, 250, 150), canPlay);
            addBtn(BTN_PLY_MINUS5, 362, 494, 54, 34, L"-5 sn", Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), canPlay);
            addBtn(BTN_PLY_PLUS5, 422, 494, 54, 34, L"+5 sn", Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), canPlay);
            addBtn(BTN_PLY_RESTART, 482, 494, 56, 34, L"Başa", Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), canPlay);

            wchar_t rateStr[16] = {};
            swprintf(rateStr, 16, L"%.1fx", playerRate_);
            addBtn(BTN_PLY_RATE, 544, 494, 48, 34, rateStr, Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), canPlay);

            addBtn(BTN_PLY_TRIM, 600, 494, 92, 34, L"✂  Kırp",
                trimMode_ ? Gc(13, 148, 136) : Gc(32, 39, 53),
                trimMode_ ? Gc(20, 184, 166) : Gc(44, 54, 72),
                trimMode_ ? Gc(94, 234, 212) : Gc(56, 68, 90), canPlay);

            addBtn(BTN_PLY_MUTE, 726, 494, 48, 34, playerMuted_ ? L"Sessiz" : L"Ses",
                playerMuted_ ? Gc(180, 83, 9) : Gc(32, 39, 53),
                playerMuted_ ? Gc(217, 119, 6) : Gc(44, 54, 72), Gc(56, 68, 90), canPlay);

            // Volume slider bar
            RECT vb = GetVolumeBarRect();
            float vbW = static_cast<float>(vb.right - vb.left);
            float vbY = static_cast<float>(vb.top + (vb.bottom - vb.top) / 2 - S(3));
            FillRoundRect(g, (float)vb.left, vbY, vbW, (float)S(6), (float)S(3), Gc(42, 51, 68));
            float vFill = vbW * (playerMuted_ ? 0.0f : playerVol_);
            if (vFill > 1.0f) {
                FillRoundRect(g, (float)vb.left, vbY, vFill, (float)S(6), (float)S(3), Gc(34, 197, 94));
            }
            float vKnobX = static_cast<float>(vb.left) + vFill;
            g.FillEllipse(&knobBr, vKnobX - S(6), vbY + S(3) - S(6), (float)S(12), (float)S(12));

            if (trimMode_) {
                bool validSpan = canPlay && (trimEndSec_ - trimStartSec_ >= 0.2);
                addBtn(BTN_TRIM_SET_START, 254, 536, 104, 30, L"[  Başlangıç", Gc(20, 83, 45), Gc(22, 101, 52), Gc(74, 222, 128), canPlay);
                addBtn(BTN_TRIM_SET_END, 364, 536, 90, 30, L"Bitiş  ]", Gc(120, 53, 15), Gc(146, 64, 14), Gc(251, 191, 36), canPlay);
                addBtn(BTN_TRIM_SAVE_COPY, 462, 536, 148, 30, L"Yeni Kopya Kaydet", Gc(37, 99, 235), Gc(59, 130, 246), Gc(96, 165, 250), validSpan);
                addBtn(BTN_TRIM_REPLACE, 616, 536, 132, 30, L"Ana Klip Yap", Gc(13, 148, 136), Gc(20, 184, 166), Gc(94, 234, 212), validSpan);
                addBtn(BTN_TRIM_CANCEL, 754, 536, 68, 30, L"İptal", Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), !trimBusy_);
                g.DrawString(trimBusy_
                    ? footerMsg_.c_str()
                    : L"İpucu: Yeşil/turuncu tutamaçları sürükleyin veya istediğiniz anda Başlangıç / Bitiş'e basın.",
                    -1, &fSmall, Gdiplus::RectF((float)S(254), (float)S(574), (float)S(632), (float)S(22)), &sfLeft, &brMuted);
            } else {
                addBtn(BTN_PLY_EXTERNAL, 742, 566, 144, 30, L"Dış Oynatıcıda Aç", Gc(26, 32, 44), Gc(38, 46, 64), Gc(48, 58, 78));
                g.DrawString(footerMsg_.c_str(), -1, &fSmall,
                    Gdiplus::RectF((float)S(254), (float)S(570), (float)S(476), (float)S(22)), &sfLeft, &brMuted);
            }
        } else if (activeTab_ == 0) {
            // ================= TAB 0: KLİPLERİM (GALLERY + FAVORITES + SEARCH) =================
            g.DrawString(L"Kayıtlı Klipler", -1, &fTitle,
                Gdiplus::RectF((float)S(242), (float)S(12), (float)S(310), (float)S(28)), &sfLeft, &brText);

            addBtn(BTN_TOP_FAV_FILTER, 572, 14, 130, 30, L"★  Favoriler",
                favOnly_ ? Gc(120, 83, 14) : Gc(28, 34, 47),
                favOnly_ ? Gc(146, 100, 18) : Gc(40, 49, 68),
                favOnly_ ? Gc(250, 204, 21) : Gc(52, 64, 86));
            addBtn(BTN_TOP_FOLDER, 710, 14, 98, 30, L"Klasörü Aç", Gc(28, 34, 47), Gc(40, 49, 68), Gc(52, 64, 86));
            addBtn(BTN_TOP_REFRESH, 816, 14, 82, 30, L"Yenile", Gc(28, 34, 47), Gc(40, 49, 68), Gc(52, 64, 86));

            if (!searchQuery_.empty()) {
                addBtn(BTN_TOP_CLEAR_SEARCH, 504, 48, 26, 24, L"✕", Gc(44, 54, 72), Gc(64, 78, 104), Gc(82, 98, 128));
            }
            size_t favCount = 0;
            for (const auto& cc : clips_) if (cc.isFavorite) ++favCount;
            wchar_t metaInfo[160] = {};
            if (searchQuery_.empty() && !favOnly_) {
                swprintf(metaInfo, 160, L"%zu klip  •  %zu favori", clips_.size(), favCount);
            } else {
                swprintf(metaInfo, 160, L"%zu / %zu klip gösteriliyor", filteredIndices_.size(), clips_.size());
            }
            g.DrawString(metaInfo, -1, &fSmall,
                Gdiplus::RectF((float)S(540), (float)S(50), (float)S(358), (float)S(20)), &sfLeft, &brMuted);

            RECT vp = GetGridViewportRect();
            if (filteredIndices_.empty()) {
                FillRoundRect(g, (float)vp.left, (float)vp.top, (float)(vp.right - vp.left), (float)(vp.bottom - vp.top),
                    (float)S(14), Gc(20, 25, 35));
                StrokeRoundRect(g, (float)vp.left, (float)vp.top, (float)(vp.right - vp.left), (float)(vp.bottom - vp.top),
                    (float)S(14), Gc(40, 48, 65));
                if (clips_.empty()) {
                    g.DrawString(L"Henüz kayıtlı klip yok", -1, &fTitle,
                        Gdiplus::RectF((float)vp.left, (float)(vp.top + S(160)), (float)(vp.right - vp.left), (float)S(32)),
                        &sfCenter, &brText);
                    wchar_t hint[160] = {};
                    swprintf(hint, 160, L"Oyun oynarken %ls tuşuna bastığınızda son %d saniye otomatik olarak buraya kaydedilir.",
                        hk.c_str(), app_.Config().clipSeconds);
                    g.DrawString(hint, -1, &fMed,
                        Gdiplus::RectF((float)(vp.left + S(40)), (float)(vp.top + S(200)), (float)(vp.right - vp.left - S(80)), (float)S(36)),
                        &sfCenter, &brMuted);
                } else {
                    g.DrawString(favOnly_ ? L"Henüz favori klip yok veya arama eşleşmedi" : L"Aramanızla eşleşen klip bulunamadı",
                        -1, &fTitle, Gdiplus::RectF((float)vp.left, (float)(vp.top + S(175)), (float)(vp.right - vp.left), (float)S(32)),
                        &sfCenter, &brText);
                }
            } else {
                g.SetClip(Gdiplus::Rect(vp.left, vp.top, vp.right - vp.left, vp.bottom - vp.top));
                const int cardW = S(312);
                const int cardH = S(226);
                const int gapX = S(16);
                const int gapY = S(14);

                for (size_t vi = 0; vi < filteredIndices_.size(); ++vi) {
                    int col = static_cast<int>(vi % 2);
                    int row = static_cast<int>(vi / 2);
                    int cx = vp.left + col * (cardW + gapX);
                    int cy = vp.top + row * (cardH + gapY) - scrollY_;
                    if (cy + cardH < vp.top || cy > vp.bottom) continue;

                    bool sel = (static_cast<int>(vi) == selectedIdx_);
                    bool hov = (static_cast<int>(vi) == hoverCardIdx_);
                    const auto& c = clips_[filteredIndices_[vi]];

                    FillRoundRect(g, (float)cx, (float)cy, (float)cardW, (float)cardH, (float)S(12),
                        sel ? Gc(28, 39, 62) : (hov ? Gc(29, 36, 50) : Gc(23, 28, 38)));
                    StrokeRoundRect(g, (float)cx, (float)cy, (float)cardW, (float)cardH, (float)S(12),
                        sel ? Gc(59, 130, 246) : (hov ? Gc(75, 89, 115) : Gc(42, 51, 68)),
                        sel ? 2.0f : 1.0f);

                    int tx = cx + S(7), ty = cy + S(7), tw = S(298), th = S(168);
                    auto it = thumbBmps_.find(c.filePath);
                    if (it != thumbBmps_.end() && it->second) {
                        Gdiplus::GraphicsState saved = g.Save();
                        Gdiplus::GraphicsPath thumbClip;
                        AddRoundedRect(thumbClip, (float)tx, (float)ty, (float)tw, (float)th, (float)S(8));
                        g.SetClip(&thumbClip, Gdiplus::CombineModeIntersect);

                        Gdiplus::Bitmap bm(it->second, nullptr);
                        g.DrawImage(&bm, tx, ty, tw, th);
                        g.Restore(saved);
                    } else {
                        FillRoundRect(g, (float)tx, (float)ty, (float)tw, (float)th, (float)S(8), Gc(15, 19, 27));
                        g.DrawString(L"Önizleme hazırlanıyor...", -1, &fSmall,
                            Gdiplus::RectF((float)tx, (float)ty, (float)tw, (float)th), &sfCenter, &brMuted);
                    }

                    if (hov) {
                        FillRoundRect(g, (float)tx, (float)ty, (float)tw, (float)th, (float)S(8), Gc(0, 0, 0, 70));
                        Gdiplus::SolidBrush pCirc(Gc(15, 23, 42, 200));
                        int pcx = tx + tw / 2 - S(20), pcy = ty + th / 2 - S(20);
                        g.FillEllipse(&pCirc, pcx, pcy, S(40), S(40));
                        Gdiplus::Point tri[3] = {
                            { tx + tw / 2 - S(5), ty + th / 2 - S(8) },
                            { tx + tw / 2 - S(5), ty + th / 2 + S(8) },
                            { tx + tw / 2 + S(9), ty + th / 2 }
                        };
                        Gdiplus::SolidBrush triB(Gc(255, 255, 255));
                        g.FillPolygon(&triB, tri, 3);
                    }

                    // Favorite star button badge at top-right of thumbnail
                    int stX = tx + tw - S(32), stY = ty + S(6);
                    FillRoundRect(g, (float)stX, (float)stY, (float)S(26), (float)S(24), (float)S(6),
                        c.isFavorite ? Gc(46, 36, 12, 225) : Gc(10, 12, 18, 180));
                    if (c.isFavorite) {
                        StrokeRoundRect(g, (float)stX, (float)stY, (float)S(26), (float)S(24), (float)S(6), Gc(250, 204, 21, 180));
                    }
                    DrawStarPolygon(g, static_cast<float>(stX + S(13)), static_cast<float>(stY + S(12)),
                        static_cast<float>(S(7)), static_cast<float>(S(3)), c.isFavorite);

                    std::wstring durLbl = FormatDurClock(c.durationSeconds);
                    FillRoundRect(g, (float)(tx + tw - S(52)), (float)(ty + th - S(26)), (float)S(44), (float)S(19), (float)S(5), Gc(10, 12, 18, 210));
                    g.DrawString(durLbl.c_str(), -1, &fSmall,
                        Gdiplus::RectF((float)(tx + tw - S(52)), (float)(ty + th - S(26)), (float)S(44), (float)S(19)), &sfCenter, &brText);

                    if (c.height > 0) {
                        wchar_t rb[16] = {};
                        swprintf(rb, 16, L"%up", c.height);
                        FillRoundRect(g, (float)(tx + S(8)), (float)(ty + S(8)), (float)S(42), (float)S(19), (float)S(5), Gc(10, 12, 18, 195));
                        g.DrawString(rb, -1, &fSmall,
                            Gdiplus::RectF((float)(tx + S(8)), (float)(ty + S(8)), (float)S(42), (float)S(19)), &sfCenter, &brText);
                    }

                    std::wstring title = StripMp4Ext(c.fileName);
                    g.DrawString(title.c_str(), -1, &fBold,
                        Gdiplus::RectF((float)(cx + S(10)), (float)(cy + S(181)), (float)(cardW - S(20)), (float)S(20)),
                        &sfLeft, &brText);
                    std::wstring meta = c.displayDate + L"  •  " + FormatMb(c.fileSizeBytes);
                    g.DrawString(meta.c_str(), -1, &fSmall,
                        Gdiplus::RectF((float)(cx + S(10)), (float)(cy + S(201)), (float)(cardW - S(20)), (float)S(18)),
                        &sfLeft, &brMuted);
                }
                g.ResetClip();

                if (maxScrollY_ > 0) {
                    int sbX = vp.right - S(6);
                    int sbH = vp.bottom - vp.top;
                    FillRoundRect(g, (float)sbX, (float)vp.top, (float)S(5), (float)sbH, 2.5f, Gc(28, 34, 47));
                    float ratio = static_cast<float>(sbH) / static_cast<float>(sbH + maxScrollY_);
                    int thumbH = std::max(S(32), static_cast<int>(sbH * ratio));
                    int thumbY = vp.top + static_cast<int>(static_cast<float>(scrollY_) / maxScrollY_ * (sbH - thumbH));
                    FillRoundRect(g, (float)sbX, (float)thumbY, (float)S(5), (float)thumbH, 2.5f, Gc(90, 108, 140));
                }
            }

            // Bottom action dock
            bool hasSel = (selectedIdx_ >= 0 && selectedIdx_ < static_cast<int>(filteredIndices_.size()));
            bool selFav = hasSel && clips_[filteredIndices_[static_cast<size_t>(selectedIdx_)]].isFavorite;
            FillRoundRect(g, (float)S(242), (float)S(544), (float)S(656), (float)S(56), (float)S(10), Gc(20, 25, 35));
            StrokeRoundRect(g, (float)S(242), (float)S(544), (float)S(656), (float)S(56), (float)S(10), Gc(40, 48, 65));
            addBtn(BTN_CLIP_PLAY, 254, 555, 100, 34, L"▶  İzle", Gc(37, 99, 235), Gc(59, 130, 246), Gc(96, 165, 250, 140), hasSel);
            addBtn(BTN_CLIP_FAV, 360, 555, 92, 34, selFav ? L"★ Favori" : L"☆ Favori",
                selFav ? Gc(92, 68, 14) : Gc(32, 39, 53),
                selFav ? Gc(120, 88, 18) : Gc(44, 54, 72),
                selFav ? Gc(250, 204, 21) : Gc(56, 68, 90), hasSel);
            addBtn(BTN_CLIP_RENAME, 458, 555, 84, 34, L"Adlandır", Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), hasSel);
            addBtn(BTN_CLIP_REVEAL, 548, 555, 102, 34, L"Klasörde Aç", Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), hasSel);
            addBtn(BTN_CLIP_DELETE, 656, 555, 52, 34, L"Sil", Gc(153, 27, 27), Gc(185, 28, 28), Gc(220, 38, 38), hasSel);
            g.DrawString(footerMsg_.c_str(), -1, &fSmall,
                Gdiplus::RectF((float)S(716), (float)S(552), (float)S(174), (float)S(40)), &sfLeft, &brMuted);
        } else {
            // ================= TAB 1: AYARLAR =================
            g.DrawString(L"Ayarlar", -1, &fTitle,
                Gdiplus::RectF((float)S(242), (float)S(14), (float)S(220), (float)S(28)), &sfLeft, &brText);
            g.DrawString(L"Video, ses ve kısayol değişiklikleri anında uygulanır.", -1, &fSmall,
                Gdiplus::RectF((float)S(242), (float)S(42), (float)S(460), (float)S(18)), &sfLeft, &brMuted);

            FillRoundRect(g, (float)S(242), (float)S(68), (float)S(320), (float)S(180), (float)S(12), Gc(23, 28, 38));
            StrokeRoundRect(g, (float)S(242), (float)S(68), (float)S(320), (float)S(180), (float)S(12), Gc(44, 54, 72));
            g.DrawString(L"Video ve Tampon", -1, &fBold,
                Gdiplus::RectF((float)S(258), (float)S(76), (float)S(220), (float)S(22)), &sfLeft, &brBlueTxt);
            g.DrawString(L"Hızlı Seçim:", -1, &fSmall,
                Gdiplus::RectF((float)S(258), (float)S(211), (float)S(84), (float)S(22)), &sfLeft, &brMuted);
            {
                bool is1080 = (app_.Config().height >= 1080);
                addBtn(BTN_SET_720P, 344, 208, 96, 28, L"720p HD",
                    !is1080 ? Gc(37, 99, 235) : Gc(32, 39, 53),
                    !is1080 ? Gc(59, 130, 246) : Gc(44, 54, 72),
                    !is1080 ? Gc(96, 165, 250, 160) : Gc(56, 68, 90));
                addBtn(BTN_SET_1080P, 446, 208, 104, 28, L"1080p Full HD",
                    is1080 ? Gc(37, 99, 235) : Gc(32, 39, 53),
                    is1080 ? Gc(59, 130, 246) : Gc(44, 54, 72),
                    is1080 ? Gc(96, 165, 250, 160) : Gc(56, 68, 90));
            }

            FillRoundRect(g, (float)S(578), (float)S(68), (float)S(320), (float)S(180), (float)S(12), Gc(23, 28, 38));
            StrokeRoundRect(g, (float)S(578), (float)S(68), (float)S(320), (float)S(180), (float)S(12), Gc(44, 54, 72));
            Gdiplus::SolidBrush brGreenTxt(Gc(134, 239, 172));
            g.DrawString(L"Ses (Oyun + Mikrofon)", -1, &fBold,
                Gdiplus::RectF((float)S(594), (float)S(74), (float)S(240), (float)S(22)), &sfLeft, &brGreenTxt);

            // Live microphone input level vs Noise Gate threshold meter (594..882, y=224..240)
            {
                float db = static_cast<float>(st.micLevelDb);
                float norm = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));
                int thrDb = trkMicGate_ ? static_cast<int>(SendMessageW(trkMicGate_, TBM_GETPOS, 0, 0)) : app_.Config().micGateThresholdDb;
                float thrNorm = std::max(0.0f, std::min(1.0f, (static_cast<float>(thrDb) + 60.0f) / 60.0f));
                float barX = static_cast<float>(S(594)), barY = static_cast<float>(S(227));
                float barW = static_cast<float>(S(212)), barH = static_cast<float>(S(8));
                FillRoundRect(g, barX, barY, barW, barH, (float)S(4), Gc(34, 42, 58));
                if (norm > 0.01f) {
                    bool openNow = (db >= static_cast<float>(thrDb));
                    FillRoundRect(g, barX, barY, barW * norm, barH, (float)S(4),
                        openNow ? Gc(34, 197, 94) : Gc(245, 158, 11));
                }
                float markerX = barX + barW * thrNorm;
                FillRoundRect(g, markerX - 1.5f, barY - S(2), 3.0f, barH + S(4), 1.0f, Gc(248, 250, 252));

                wchar_t dbTxt[32] = {};
                swprintf(dbTxt, 32, L"Mik: %.0f dB", db);
                g.DrawString(dbTxt, -1, &fSmall,
                    Gdiplus::RectF((float)S(812), (float)S(221), (float)S(76), (float)S(20)), &sfLeft, &brMuted);
            }

            FillRoundRect(g, (float)S(242), (float)S(258), (float)S(656), (float)S(270), (float)S(12), Gc(23, 28, 38));
            StrokeRoundRect(g, (float)S(242), (float)S(258), (float)S(656), (float)S(270), (float)S(12), Gc(44, 54, 72));
            Gdiplus::SolidBrush brPurpTxt(Gc(216, 180, 254));
            g.DrawString(L"Oyun İçi Kısayol, Bildirim ve Klasör", -1, &fBold,
                Gdiplus::RectF((float)S(258), (float)S(264), (float)S(340), (float)S(22)), &sfLeft, &brPurpTxt);

            g.DrawString(footerMsg_.c_str(), -1, &fSmall,
                Gdiplus::RectF((float)S(258), (float)S(498), (float)S(624), (float)S(20)), &sfLeft, &brMuted);
        }
    }

    BitBlt(targetDc, 0, 0, cw, ch, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteObject(backBmp);
    DeleteDC(memDc);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (self) return self->OnMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd_, &ps);
            PaintWindow(hdc);
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_TIMER:
            if (wParam == TIMER_PLAYER_ID) {
                UpdatePlayerMetrics();
            } else if (wParam == TIMER_MIC_METER_ID && activeTab_ == 1) {
                RECT mrc{ S(590), S(220), S(892), S(245) };
                InvalidateRect(hwnd_, &mrc, FALSE);
            }
            return 0;

        case WM_MW_TRIM_DONE: {
            auto* tm = reinterpret_cast<TrimDoneMsg*>(lParam);
            if (trimThread_.joinable()) trimThread_.join();
            trimBusy_ = false;
            if (tm) {
                if (tm->ok) {
                    if (tm->replaceOriginal) {
                        bool wasFav = false;
                        for (const auto& cc : clips_) {
                            if (_wcsicmp(cc.filePath.c_str(), tm->origPath.c_str()) == 0) {
                                wasFav = cc.isFavorite;
                                break;
                            }
                        }
                        TrashClipFile(tm->origPath);
                        MoveFileExW(tm->outPath.c_str(), tm->origPath.c_str(), MOVEFILE_REPLACE_EXISTING);
                        if (wasFav) {
                            size_t sl = tm->origPath.find_last_of(L"\\/");
                            std::wstring fname = (sl != std::wstring::npos) ? tm->origPath.substr(sl + 1) : tm->origPath;
                            SetClipFavorite(GetFavoritesFilePath(), fname, true);
                        }
                        InvalidateClipMetadata(tm->origPath);
                        auto it = thumbBmps_.find(tm->origPath);
                        if (it != thumbBmps_.end()) {
                            if (it->second) DeleteObject(it->second);
                            thumbBmps_.erase(it);
                        }
                        trimMode_ = false;
                        RefreshClips(tm->origPath);
                        if (selectedIdx_ >= 0) OpenEmbeddedPlayer(selectedIdx_);
                        SetFooterMessage(L"Klip kırpıldı ve ana klip güncellendi (eski kopya çöp kutusunda).");
                    } else {
                        trimMode_ = false;
                        favOnly_ = false;
                        if (edtSearch_ && !searchQuery_.empty()) {
                            searchQuery_.clear();
                            SetWindowTextW(edtSearch_, L"");
                        }
                        RefreshClips(tm->outPath);
                        if (selectedIdx_ >= 0) OpenEmbeddedPlayer(selectedIdx_);
                        SetFooterMessage(L"Kırpılan kesit yeni klip olarak kaydedildi.");
                    }
                } else {
                    if (tm->replaceOriginal) DeleteFileW(tm->outPath.c_str());
                    std::wstring msg = tm->message;
                    double savedStart = trimStartSec_, savedEnd = trimEndSec_;
                    bool keepTrim = trimMode_;
                    RefreshClips(tm->origPath);
                    if (selectedIdx_ >= 0) OpenEmbeddedPlayer(selectedIdx_);
                    trimMode_ = keepTrim;
                    trimStartSec_ = savedStart;
                    trimEndSec_ = savedEnd;
                    SetFooterMessage(msg);
                }
                delete tm;
            }
            return 0;
        }

        case WM_MW_PLAYER_EVENT: {
            auto* ev = reinterpret_cast<PlayerEventMsg*>(lParam);
            if (ev) {
                if (ev->gen == playerGen_ && player_) {
                    if (ev->type == MFP_EVENT_TYPE_MEDIAITEM_SET) {
                        playerReady_ = true;
                        PROPVARIANT dur; PropVariantInit(&dur);
                        if (SUCCEEDED(player_->GetDuration(MFP_POSITIONTYPE_100NS, &dur)) && dur.vt == VT_I8) {
                            playerDurSec_ = static_cast<double>(dur.hVal.QuadPart) / 1e7;
                            if (!trimMode_ || trimEndSec_ > playerDurSec_ || trimEndSec_ <= 0.1) {
                                trimEndSec_ = playerDurSec_;
                            }
                        }
                        PropVariantClear(&dur);
                        player_->SetVolume(playerVol_);
                        player_->SetMute(playerMuted_ ? TRUE : FALSE);
                        player_->SetRate(playerRate_);
                        player_->UpdateVideo();
                        player_->Play();
                        playerPlaying_ = true;
                        playerEnded_ = false;
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    } else if (ev->type == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
                        playerPlaying_ = false;
                        playerEnded_ = true;
                        playerPosSec_ = playerDurSec_;
                        RECT ctrlRc{ S(242), S(428), S(900), S(606) };
                        InvalidateRect(hwnd_, &ctrlRc, FALSE);
                    } else if (ev->type == MFP_EVENT_TYPE_PLAY) {
                        playerPlaying_ = true;
                        playerEnded_ = false;
                    } else if (ev->type == MFP_EVENT_TYPE_PAUSE) {
                        playerPlaying_ = false;
                    } else if (ev->type == MFP_EVENT_TYPE_ERROR) {
                        SetFooterMessage(L"Oynatma hatası oluştu.");
                    }
                }
                delete ev;
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, kRgbText);
            SetBkColor(hdc, kRgbCard);
            return reinterpret_cast<LRESULT>(brCard_);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, kRgbText);
            SetBkColor(hdc, kRgbInput);
            return reinterpret_cast<LRESULT>(brInput_);
        }
        case WM_DRAWITEM: {
            const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (!dis) break;
            HDC hdc = dis->hDC;
            RECT rc = dis->rcItem;
            bool disabled = (dis->itemState & ODS_DISABLED) != 0;
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;

            COLORREF fill = kRgbInput;
            COLORREF border = kRgbBorder;
            if (dis->CtlID == ID_BTN_APPLY) {
                fill = pressed ? RGB(29, 78, 216) : RGB(37, 99, 235);
                border = RGB(96, 165, 250);
            } else if (dis->CtlID == ID_BTN_EXIT_APP) {
                fill = pressed ? RGB(127, 29, 29) : RGB(153, 27, 27);
                border = RGB(220, 38, 38);
            } else if (pressed) {
                fill = RGB(48, 58, 78);
            }
            HBRUSH br = CreateSolidBrush(fill);
            FillRect(hdc, &rc, br);
            DeleteObject(br);
            HPEN pen = CreatePen(PS_SOLID, 1, border);
            HGDIOBJ op = SelectObject(hdc, pen);
            HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(pen);

            wchar_t txt[128] = {};
            GetWindowTextW(dis->hwndItem, txt, 128);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, disabled ? kRgbMuted : kRgbText);
            SelectObject(hdc, (dis->CtlID == ID_BTN_APPLY || dis->CtlID == ID_BTN_EXIT_APP) ? boldFont_ : font_);
            DrawTextW(hdc, txt, -1, const_cast<RECT*>(&rc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            if (!trackingMouse_) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
                TrackMouseEvent(&tme);
                trackingMouse_ = true;
            }
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));
            if (draggingTrim_ != 0 && playerOpen_ && !trimBusy_) {
                RECT sk = GetSeekBarRect();
                double ratio = static_cast<double>(mx - sk.left) / std::max(1L, sk.right - sk.left);
                ratio = std::max(0.0, std::min(1.0, ratio));
                double dur = (playerDurSec_ > 0.2) ? playerDurSec_ : 1.0;
                double t = ratio * dur;
                if (draggingTrim_ == 1) {
                    trimStartSec_ = std::min(t, trimEndSec_ - 0.25);
                    if (trimStartSec_ < 0.0) trimStartSec_ = 0.0;
                    playerPosSec_ = trimStartSec_;
                } else {
                    trimEndSec_ = std::max(t, trimStartSec_ + 0.25);
                    if (trimEndSec_ > dur) trimEndSec_ = dur;
                    playerPosSec_ = trimEndSec_;
                }
                RECT ctrlRc{ S(242), S(428), S(900), S(606) };
                InvalidateRect(hwnd_, &ctrlRc, FALSE);
                return 0;
            }
            if (draggingSeek_ && playerOpen_ && playerReady_) {
                RECT sk = GetSeekBarRect();
                double ratio = static_cast<double>(mx - sk.left) / std::max(1L, sk.right - sk.left);
                ratio = std::max(0.0, std::min(1.0, ratio));
                playerPosSec_ = ratio * playerDurSec_;
                RECT ctrlRc{ S(242), S(428), S(900), S(606) };
                InvalidateRect(hwnd_, &ctrlRc, FALSE);
                return 0;
            }
            if (draggingVol_ && playerOpen_) {
                RECT vb = GetVolumeBarRect();
                float ratio = static_cast<float>(mx - vb.left) / std::max(1L, vb.right - vb.left);
                SetPlayerVolume(ratio);
                return 0;
            }
            UpdateHover(mx, my);
            return 0;
        }
        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            hoverBtnId_ = 0;
            hoverCardIdx_ = -1;
            if (!draggingSeek_ && !draggingVol_ && draggingTrim_ == 0) pressedBtnId_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_MOUSEWHEEL:
            if (activeTab_ == 0 && !playerOpen_ && maxScrollY_ > 0) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                scrollY_ -= (delta / WHEEL_DELTA) * S(68);
                ClampScroll();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));

            if (activeTab_ == 0 && playerOpen_ && !trimBusy_) {
                int th = HitTestTrimHandle(mx, my);
                if (th != 0 && playerReady_) {
                    draggingTrim_ = th;
                    SetCapture(hwnd_);
                    return 0;
                }
                RECT sk = GetSeekBarRect();
                if (playerReady_ && mx >= sk.left - S(4) && mx <= sk.right + S(4) && my >= sk.top && my <= sk.bottom) {
                    draggingSeek_ = true;
                    SetCapture(hwnd_);
                    double ratio = static_cast<double>(mx - sk.left) / std::max(1L, sk.right - sk.left);
                    ratio = std::max(0.0, std::min(1.0, ratio));
                    playerPosSec_ = ratio * playerDurSec_;
                    RECT ctrlRc{ S(242), S(428), S(900), S(606) };
                    InvalidateRect(hwnd_, &ctrlRc, FALSE);
                    return 0;
                }
                RECT vb = GetVolumeBarRect();
                if (mx >= vb.left - S(4) && mx <= vb.right + S(4) && my >= vb.top && my <= vb.bottom) {
                    draggingVol_ = true;
                    SetCapture(hwnd_);
                    float ratio = static_cast<float>(mx - vb.left) / std::max(1L, vb.right - vb.left);
                    SetPlayerVolume(ratio);
                    return 0;
                }
            }

            if (activeTab_ == 0 && !playerOpen_) {
                int starVi = HitTestCardStar(mx, my);
                if (starVi >= 0) {
                    selectedIdx_ = starVi;
                    ToggleFavoriteForViewIdx(starVi);
                    return 0;
                }
            }

            int bid = HitTestCustomButton(mx, my);
            if (bid != 0) {
                pressedBtnId_ = bid;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            int card = HitTestCard(mx, my);
            if (card >= 0 && card != selectedIdx_) {
                selectedIdx_ = card;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));
            if (draggingTrim_ != 0) {
                int which = draggingTrim_;
                draggingTrim_ = 0;
                ReleaseCapture();
                SeekPlayerAbsolute(which == 1 ? trimStartSec_ : trimEndSec_);
                return 0;
            }
            if (draggingSeek_) {
                draggingSeek_ = false;
                ReleaseCapture();
                SeekPlayerAbsolute(playerPosSec_);
                return 0;
            }
            if (draggingVol_) {
                draggingVol_ = false;
                ReleaseCapture();
                return 0;
            }
            if (pressedBtnId_ != 0) {
                int target = pressedBtnId_;
                pressedBtnId_ = 0;
                ReleaseCapture();
                InvalidateRect(hwnd_, nullptr, FALSE);
                if (HitTestCustomButton(mx, my) == target) {
                    switch (target) {
                        case BTN_TAB_CLIPS:
                            if (activeTab_ == 0 && playerOpen_) CloseEmbeddedPlayer();
                            SwitchTab(0);
                            RefreshClips();
                            break;
                        case BTN_TAB_SETTINGS: SwitchTab(1); break;
                        case BTN_SIDE_SAVE: if (cb_.onSaveClipRequested) cb_.onSaveClipRequested(); break;
                        case BTN_SIDE_TOGGLE: if (cb_.onToggleBufferingRequested) cb_.onToggleBufferingRequested(); break;
                        case BTN_SIDE_MIC:
                            if (cb_.onToggleMicMuteRequested) cb_.onToggleMicMuteRequested();
                            SendMessageW(chkMicMute_, BM_SETCHECK, app_.Config().micMuted ? BST_CHECKED : BST_UNCHECKED, 0);
                            break;
                        case BTN_TOP_FAV_FILTER:
                            favOnly_ = !favOnly_;
                            RebuildFilteredView();
                            break;
                        case BTN_TOP_CLEAR_SEARCH:
                            if (edtSearch_) SetWindowTextW(edtSearch_, L"");
                            break;
                        case BTN_TOP_FOLDER: OpenFolder(app_.Config().outputDir); break;
                        case BTN_TOP_REFRESH: RefreshClips(); break;
                        case BTN_CLIP_PLAY: PlaySelected(); break;
                        case BTN_CLIP_FAV: ToggleFavoriteForViewIdx(); break;
                        case BTN_CLIP_RENAME: RenameSelected(); break;
                        case BTN_CLIP_REVEAL: RevealSelected(); break;
                        case BTN_CLIP_DELETE: DeleteSelected(); break;
                        case BTN_SET_720P:
                        case BTN_SET_1080P: {
                            AppConfig cfg = app_.Config();
                            bool want1080 = (target == BTN_SET_1080P);
                            int keepFps = (cfg.fps == 30) ? 30 : 60;
                            cfg.width = want1080 ? 1920 : 1280;
                            cfg.height = want1080 ? 1080 : 720;
                            cfg.fps = keepFps;
                            if (want1080 && cfg.bitrateKbps <= 6000) {
                                cfg.bitrateKbps = (keepFps == 60) ? 12000 : 10000;
                            }
                            if (cb_.onConfigApplied) cb_.onConfigApplied(cfg);
                            LoadSettingsControls();
                            wchar_t msg[96] = {};
                            swprintf(msg, 96, L"Kayıt kalitesi: %dp @ %d FPS (%d Mbps) uygulandı.",
                                cfg.height, cfg.fps, cfg.bitrateKbps / 1000);
                            SetFooterMessage(msg);
                            break;
                        }

                        // Embedded Player & Trim buttons
                        case BTN_PLY_BACK: CloseEmbeddedPlayer(); break;
                        case BTN_PLY_FAV: ToggleFavoriteForViewIdx(); break;
                        case BTN_PLY_RENAME: RenameSelected(); break;
                        case BTN_PLY_FOLDER: RevealSelected(); break;
                        case BTN_PLY_DELETE: DeleteSelected(); break;
                        case BTN_PLY_PLAYPAUSE: TogglePlayerPlayPause(); break;
                        case BTN_PLY_MINUS5: SeekPlayerRelative(-5.0); break;
                        case BTN_PLY_PLUS5: SeekPlayerRelative(+5.0); break;
                        case BTN_PLY_RESTART: SeekPlayerAbsolute(0.0); break;
                        case BTN_PLY_RATE: CyclePlayerRate(); break;
                        case BTN_PLY_TRIM: ToggleTrimMode(); break;
                        case BTN_PLY_MUTE: TogglePlayerMute(); break;
                        case BTN_PLY_EXTERNAL: PlayExternalSelected(); break;
                        case BTN_TRIM_SET_START:
                            trimStartSec_ = std::max(0.0, std::min(playerPosSec_, trimEndSec_ - 0.25));
                            InvalidateRect(hwnd_, nullptr, FALSE);
                            break;
                        case BTN_TRIM_SET_END: {
                            double dur = (playerDurSec_ > 0.2) ? playerDurSec_ : 1.0;
                            trimEndSec_ = std::min(dur, std::max(playerPosSec_, trimStartSec_ + 0.25));
                            InvalidateRect(hwnd_, nullptr, FALSE);
                            break;
                        }
                        case BTN_TRIM_SAVE_COPY: StartTrimAsync(false); break;
                        case BTN_TRIM_REPLACE: StartTrimAsync(true); break;
                        case BTN_TRIM_CANCEL:
                            if (trimMode_) ToggleTrimMode();
                            break;
                    }
                }
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));
            if (activeTab_ == 0 && !playerOpen_ && HitTestCardStar(mx, my) >= 0) return 0;
            int card = HitTestCard(mx, my);
            if (card >= 0) {
                selectedIdx_ = card;
                OpenEmbeddedPlayer(card);
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));
            int card = HitTestCard(mx, my);
            if (card >= 0) {
                selectedIdx_ = card;
                InvalidateRect(hwnd_, nullptr, FALSE);
                bool isFav = clips_[filteredIndices_[static_cast<size_t>(card)]].isFavorite;
                POINT pt{ mx, my };
                ClientToScreen(hwnd_, &pt);
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING, ID_CTX_PLAY, L"İzle (Uygulama İçi)");
                AppendMenuW(m, MF_STRING, ID_CTX_FAV, isFav ? L"☆ Favorilerden Çıkar" : L"★ Favorilere Ekle");
                AppendMenuW(m, MF_STRING, ID_CTX_RENAME, L"Adını Değiştir...");
                AppendMenuW(m, MF_STRING, ID_CTX_REVEAL, L"Klasörde Göster");
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(m, MF_STRING, ID_CTX_DELETE, L"Sil (Çöp Kutusu)");
                TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
                DestroyMenu(m);
            }
            return 0;
        }

        case WM_MW_THUMB_READY: {
            auto* tm = reinterpret_cast<ThumbMsg*>(lParam);
            if (tm) {
                if (tm->gen == thumbGen_.load()) {
                    auto it = thumbBmps_.find(tm->path);
                    if (it != thumbBmps_.end() && it->second) DeleteObject(it->second);
                    thumbBmps_[tm->path] = tm->hBmp;
                    for (auto& c : clips_) {
                        if (c.filePath == tm->path) {
                            if (tm->dur > 0.0) c.durationSeconds = tm->dur;
                            if (tm->w > 0) c.width = tm->w;
                            if (tm->h > 0) c.height = tm->h;
                            break;
                        }
                    }
                    if (!playerOpen_) InvalidateRect(hwnd_, nullptr, FALSE);
                } else if (tm->hBmp) {
                    DeleteObject(tm->hBmp);
                }
                delete tm;
            }
            return 0;
        }

        case WM_HSCROLL: {
            HWND src = reinterpret_cast<HWND>(lParam);
            if (src == trkGameVol_ || src == trkMicVol_ || src == trkMicGate_) {
                int gv = static_cast<int>(SendMessageW(trkGameVol_, TBM_GETPOS, 0, 0));
                int mv = static_cast<int>(SendMessageW(trkMicVol_, TBM_GETPOS, 0, 0));
                int gdb = static_cast<int>(SendMessageW(trkMicGate_, TBM_GETPOS, 0, 0));
                wchar_t buf[16] = {};
                swprintf(buf, 16, L"%%%d", gv); SetWindowTextW(lblGameVol_, buf);
                swprintf(buf, 16, L"%%%d", mv); SetWindowTextW(lblMicVol_, buf);
                swprintf(buf, 16, L"%d dB", gdb); SetWindowTextW(lblMicGate_, buf);
                AppConfig cfg = app_.Config();
                cfg.gameVolumePct = gv;
                cfg.micVolumePct = mv;
                cfg.micGateThresholdDb = gdb;
                if (cb_.onConfigApplied) cb_.onConfigApplied(cfg);
            }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (id == ID_EDT_SEARCH && code == EN_CHANGE && edtSearch_) {
                wchar_t sbuf[256] = {};
                GetWindowTextW(edtSearch_, sbuf, 256);
                searchQuery_ = sbuf;
                RebuildFilteredView();
                return 0;
            }
            switch (id) {
                case ID_CTX_PLAY: PlaySelected(); break;
                case ID_CTX_FAV: ToggleFavoriteForViewIdx(); break;
                case ID_CTX_RENAME: RenameSelected(); break;
                case ID_CTX_REVEAL: RevealSelected(); break;
                case ID_CTX_DELETE: DeleteSelected(); break;
                case ID_CMB_DURATION:
                case ID_CMB_RESFPS:
                case ID_CMB_BITRATE:
                    if (code == CBN_SELCHANGE) {
                        AppConfig prev = app_.Config();
                        AppConfig cfg = prev;
                        ReadSettingsControls(cfg);
                        if (id == ID_CMB_RESFPS && prev.height < 1080 && cfg.height >= 1080 && cfg.bitrateKbps <= 6000) {
                            cfg.bitrateKbps = (cfg.fps == 60) ? 12000 : 10000;
                            for (int i = 0; i < static_cast<int>(sizeof(kBitrates) / sizeof(kBitrates[0])); ++i) {
                                if (kBitrates[i] == cfg.bitrateKbps) {
                                    SendMessageW(cmbBitrate_, CB_SETCURSEL, i, 0);
                                    break;
                                }
                            }
                        }
                        if (cb_.onConfigApplied) cb_.onConfigApplied(cfg);
                        wchar_t msg[96] = {};
                        swprintf(msg, 96, L"Video ayarı güncellendi: %dp @ %d FPS (%d Mbps).",
                            cfg.height, cfg.fps, cfg.bitrateKbps / 1000);
                        SetFooterMessage(msg);
                    }
                    break;
                case ID_CHK_GAME_AUDIO:
                case ID_CHK_MIC_AUDIO:
                case ID_CHK_MIC_MUTE:
                case ID_CHK_MIC_GATE: {
                    AppConfig cfg = app_.Config();
                    cfg.captureGameAudio = (SendMessageW(chkGameAudio_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    cfg.captureMic = (SendMessageW(chkMicAudio_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    cfg.micMuted = (SendMessageW(chkMicMute_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    cfg.micNoiseGate = (SendMessageW(chkMicGate_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    if (cb_.onConfigApplied) cb_.onConfigApplied(cfg);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    break;
                }
                case ID_CMB_MIC_DEV:
                    if (code == CBN_SELCHANGE) {
                        AppConfig cfg = app_.Config();
                        int micSel = static_cast<int>(SendMessageW(cmbMicDev_, CB_GETCURSEL, 0, 0));
                        if (micSel <= 0 || micSel - 1 >= static_cast<int>(micDevices_.size())) cfg.micDeviceId.clear();
                        else cfg.micDeviceId = micDevices_[static_cast<size_t>(micSel - 1)].id;
                        if (cb_.onConfigApplied) cb_.onConfigApplied(cfg);
                    }
                    break;
                case ID_BTN_SET_CLIP_HK:
                    hotkeys_.SetPaused(true);
                    captureHotkeyMode_ = 1;
                    SetWindowTextW(edtClipHotkey_, L"Tuşa basın (ör. F12, İptal: Esc)...");
                    SetFocus(hwnd_);
                    break;
                case ID_BTN_SET_MIC_HK:
                    hotkeys_.SetPaused(true);
                    captureHotkeyMode_ = 2;
                    SetWindowTextW(edtMicHotkey_, L"Tuşa basın (İptal: Esc)...");
                    SetFocus(hwnd_);
                    break;
                case ID_BTN_CLR_MIC_HK: {
                    captureHotkeyMode_ = 0;
                    hotkeys_.SetPaused(false);
                    pendingMicMods_ = 0; pendingMicVk_ = 0;
                    SetWindowTextW(edtMicHotkey_, FormatHotkey(0, 0).c_str());
                    AppConfig cfg = app_.Config();
                    cfg.micHotkeyMods = 0; cfg.micHotkeyVk = 0;
                    if (cb_.onConfigApplied) cb_.onConfigApplied(cfg);
                    break;
                }
                case ID_BTN_RESTART_ADMIN:
                    if (HotkeyEngine::RestartElevated()) {
                        if (cb_.onExitRequested) cb_.onExitRequested();
                    }
                    break;
                case ID_BTN_BROWSE_DIR:
                    PickOutputFolder();
                    break;
                case ID_BTN_OPEN_DIR: {
                    wchar_t dir[MAX_PATH] = {};
                    GetWindowTextW(edtOutDir_, dir, MAX_PATH);
                    OpenFolder(wcslen(dir) > 0 ? dir : app_.Config().outputDir);
                    break;
                }
                case ID_BTN_TEST_SOUND:
                    PlayClipChime();
                    break;
                case ID_BTN_APPLY: {
                    AppConfig newCfg;
                    ReadSettingsControls(newCfg);
                    SetAutoStartEnabled(SendMessageW(chkAutoStart_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    if (cb_.onConfigApplied) cb_.onConfigApplied(newCfg);
                    RefreshClips();
                    SetFooterMessage(L"Ayarlar kaydedildi ve uygulandı.");
                    break;
                }
                case ID_BTN_EXIT_APP:
                    if (cb_.onExitRequested) cb_.onExitRequested();
                    break;
            }
            return 0;
        }

        case WM_CLOSE:
            captureHotkeyMode_ = 0;
            hotkeys_.SetPaused(false);
            CloseEmbeddedPlayer();
            if (cb_.onClosed) cb_.onClosed();
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}
