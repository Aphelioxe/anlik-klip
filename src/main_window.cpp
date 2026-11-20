#include "main_window.h"
#include "clip_trimmer.h"
#include "clip_uploader.h"
#include "com_ptr.h"
#include "hotkey_engine.h"
#include "qr_code.h"
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

constexpr COLORREF kRgbBg      = RGB(9, 12, 19);
constexpr COLORREF kRgbCard    = RGB(16, 21, 33);
constexpr COLORREF kRgbInput   = RGB(24, 31, 48);
constexpr COLORREF kRgbBorder  = RGB(42, 54, 78);
constexpr COLORREF kRgbText    = RGB(238, 242, 248);
constexpr COLORREF kRgbMuted   = RGB(148, 160, 180);

constexpr UINT_PTR TIMER_PLAYER_ID    = 2;
constexpr UINT_PTR TIMER_MIC_METER_ID = 3;
constexpr UINT_PTR TIMER_UI_ANIM_ID   = 4;
constexpr UINT WM_MW_THUMB_READY      = WM_APP + 20;
constexpr UINT WM_MW_PLAYER_EVENT     = WM_APP + 21;
constexpr UINT WM_MW_TRIM_DONE        = WM_APP + 22;
constexpr UINT WM_MW_UPLOAD_PROGRESS  = WM_APP + 24;
constexpr UINT WM_MW_UPLOAD_DONE      = WM_APP + 25;

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

void FillRoundRectGrad(Gdiplus::Graphics& g, float x, float y, float w, float h, float r,
                       Gdiplus::Color cTop, Gdiplus::Color cBot) {
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, x, y, w, h, r);
    Gdiplus::LinearGradientBrush br(
        Gdiplus::PointF(x, y), Gdiplus::PointF(x, y + std::max(1.0f, h)), cTop, cBot);
    g.FillPath(&br, &path);
}

std::wstring ExtractGameTagFromName(const std::wstring& fileName) {
    size_t us = fileName.find(L'_');
    if (us == std::wstring::npos || us <= 1 || us > 20) return L"";
    std::wstring prefix = fileName.substr(0, us);
    if (_wcsicmp(prefix.c_str(), L"Klip") == 0) return L"";
    return prefix;
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
    BTN_CLIP_SHARE,
    BTN_CLIP_COPY_LINK,
    BTN_CLIP_OPEN_LINK,
    BTN_CLIP_RENAME,
    BTN_CLIP_REVEAL,
    BTN_CLIP_DELETE,
    BTN_SET_720P,
    BTN_SET_1080P,

    // Player & Trim controls
    BTN_PLY_BACK = 50,
    BTN_PLY_FAV,
    BTN_PLY_SHARE,
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
    BTN_PLY_FULLSCREEN,
    BTN_PLY_EXTERNAL,
    BTN_TRIM_SET_START,
    BTN_TRIM_SET_END,
    BTN_EDIT_CUT_TOGGLE,
    BTN_EDIT_SPEED,
    BTN_EDIT_ADD_CLIP,
    BTN_EDIT_GAME_MUTE,
    BTN_EDIT_MIC_MUTE,
    BTN_EDIT_VERTICAL,
    BTN_EDIT_DISCORD10,
    BTN_EDIT_DISCORD25,
    BTN_TRIM_SAVE_COPY,
    BTN_TRIM_REPLACE,
    BTN_TRIM_CANCEL,
    BTN_CMB_DURATION,
    BTN_CMB_RESFPS,
    BTN_CMB_BITRATE,
    BTN_CMB_MIC_DEV
};

enum NativeCtrlId {
    ID_EDT_SEARCH = 2999,
    ID_CMB_DURATION = 3001,
    ID_CMB_RESFPS,
    ID_CMB_BITRATE,
    ID_CHK_GAME_AUDIO,
    ID_CHK_SPOTIFY_AUDIO,
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
    ID_CHK_MOBILE_SYNC,
    ID_BTN_MOBILE_QR,
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
    StopUploadWorker();
    CloseEmbeddedPlayer();
    StopThumbnailWorker();
    ClearThumbnails();
    if (hwnd_) {
        KillTimer(hwnd_, TIMER_MIC_METER_ID);
        KillTimer(hwnd_, TIMER_UI_ANIM_ID);
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

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN;
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int waW = wa.right - wa.left;
    const int waH = wa.bottom - wa.top;
    const int initCW = std::max(S(920), std::min(S(1180), waW - 48));
    const int initCH = std::max(S(620), std::min(S(740), waH - 64));
    RECT wr{ 0, 0, initCW, initCH };
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    const int winW = wr.right - wr.left;
    const int winH = wr.bottom - wr.top;
    const int winX = wa.left + std::max(0, (waW - winW) / 2);
    const int winY = wa.top + std::max(0, (waH - winH) / 2);

    settingsOffsetX_ = 0;
    openAnimAlpha_ = 0;
    openAnimT_ = 0.0f;
    openTargetX_ = winX;
    openTargetY_ = winY;
    animPhase_ = 0.0;
    hwnd_ = CreateWindowExW(WS_EX_LAYERED, cls, L"Anlık Klip", style,
        winX, winY + S(18), winW, winH,
        nullptr, nullptr, hInst_, this);
    if (!hwnd_) return false;
    SetLayeredWindowAttributes(hwnd_, 0, 0, LWA_ALPHA);

    ApplyDarkTitleBarAndControls(hwnd_);
    CreateControls();
    LoadSettingsControls();
    {
        RECT cli{}; GetClientRect(hwnd_, &cli);
        UpdateLayoutForSize(cli.right, cli.bottom);
    }
    RefreshClips();
    SwitchTab(initialTab);
    tabAnimT_ = 1.0f;
    tabIndicatorY_ = (activeTab_ == 1) ? 160.0f : 118.0f;

    openAnimStartMs_ = GetTickCount64();
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    SetTimer(hwnd_, TIMER_UI_ANIM_ID, 10, nullptr);
    return true;
}

void MainWindow::FreeBackbuffer() {
    if (memDc_) {
        if (memOldBmp_) SelectObject(memDc_, memOldBmp_);
        if (memBmp_) DeleteObject(memBmp_);
        DeleteDC(memDc_);
    }
    memDc_ = nullptr;
    memBmp_ = nullptr;
    memOldBmp_ = nullptr;
    memW_ = 0;
    memH_ = 0;
}

void MainWindow::UpdateLayoutForSize(int cw, int ch) {
    if (!hwnd_ || cw <= 0 || ch <= 0) return;

    const int newSox = std::max(0, (cw - S(220) - S(700)) / 2);
    const int dx = newSox - settingsOffsetX_;
    if (dx != 0) {
        struct ShiftCtx { int dx; HWND edtSearch; HWND videoHwnd; } sc{ dx, edtSearch_, videoHwnd_ };
        EnumChildWindows(hwnd_, [](HWND child, LPARAM lp) -> BOOL {
            auto* s = reinterpret_cast<ShiftCtx*>(lp);
            if (child == s->edtSearch || child == s->videoHwnd) return TRUE;
            RECT rc{};
            if (GetWindowRect(child, &rc)) {
                HWND parent = GetParent(child);
                MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<LPPOINT>(&rc), 2);
                SetWindowPos(child, nullptr, rc.left + s->dx, rc.top, 0, 0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&sc));
        settingsOffsetX_ = newSox;
    }

    if (edtSearch_) {
        int sw = std::max(S(240), cw - S(646));
        SetWindowPos(edtSearch_, nullptr, S(248), S(52), sw - S(12), S(16), SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (playerOpen_ && videoHwnd_ && !playerFullscreen_) {
        if (trimMode_) {
            int availW = std::max(S(320), cw - S(422));
            int availH = std::max(S(200), ch - S(340));
            int vw = availW;
            int vh = (vw * 9) / 16;
            if (vh > availH) { vh = availH; vw = (vh * 16) / 9; }
            int vx = S(246) + (availW - vw) / 2;
            int vy = S(58) + (availH - vh) / 2;
            SetWindowPos(videoHwnd_, nullptr, vx, vy, vw, vh, SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            int availW = std::max(S(480), cw - S(272));
            int availH = std::max(S(240), ch - S(254));
            int vw = availW;
            int vh = (vw * 9) / 16;
            if (vh > availH) { vh = availH; vw = (vh * 16) / 9; }
            int vx = S(246) + (availW - vw) / 2;
            int vy = S(60) + (availH - vh) / 2;
            SetWindowPos(videoHwnd_, nullptr, vx, vy, vw, vh, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (player_) player_->UpdateVideo();
    }
    ClampScroll();
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
    RECT cli{}; GetClientRect(hwnd_, &cli);
    RECT sideRc{ 0, 0, S(220), std::max<LONG>(S(620), cli.bottom) };
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
        if (vk == 'F' || vk == VK_F11) { TogglePlayerFullscreen(); return true; }
        if (vk == VK_ESCAPE) {
            if (playerFullscreen_) TogglePlayerFullscreen();
            else if (trimMode_) ToggleTrimMode();
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
    edtSearch_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
        S(248), S(52), S(244), S(16), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDT_SEARCH)), hInst_, nullptr);
    if (edtSearch_) {
        SendMessageW(edtSearch_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(edtSearch_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Klip ara..."));
    }

    auto makeHiddenCombo = [&](int x, int y, int w, int h, int id) {
        HWND hw = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
            S(x), S(y), S(w), S(h), hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst_, nullptr);
        if (hw) ShowWindow(hw, SW_HIDE);
        return hw;
    };

    // Card 1: Video (x=242..562, y=68..248)
    makeLbl(L"Klip Süresi:", 258, 104, 82, 20);
    cmbDuration_ = makeHiddenCombo(344, 101, 206, 180, ID_CMB_DURATION);
    for (int sec : kDurations) {
        wchar_t buf[64] = {};
        if (sec < 60) swprintf(buf, 64, L"Son %d sn", sec);
        else if (sec == 60) swprintf(buf, 64, L"Son 1 dk (60 sn) — Önerilen");
        else swprintf(buf, 64, L"Son %d dk", sec / 60);
        SendMessageW(cmbDuration_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }

    makeLbl(L"Çözünürlük:", 258, 139, 82, 20);
    cmbResFps_ = makeHiddenCombo(344, 136, 206, 180, ID_CMB_RESFPS);
    for (const auto& p : kResPresets) {
        SendMessageW(cmbResFps_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.label));
    }

    makeLbl(L"Bit Hızı:", 258, 174, 82, 20);
    cmbBitrate_ = makeHiddenCombo(344, 171, 206, 180, ID_CMB_BITRATE);
    SendMessageW(cmbBitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"4 Mbps (Hafif RAM)"));
    SendMessageW(cmbBitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"6 Mbps (720p60 Dengeli)"));
    SendMessageW(cmbBitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"8 Mbps (Yüksek Kalite)"));
    SendMessageW(cmbBitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"10 Mbps (1080p30 / Yüksek)"));
    SendMessageW(cmbBitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"12 Mbps (1080p60 Önerilen)"));
    SendMessageW(cmbBitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"15 Mbps (1080p60 Çok Yüksek)"));

    // Card 2: Audio & Mic Noise Gate (x=578..898, y=68..248)
    chkGameAudio_ = makeChk(L"Oyun / Sistem Sesi", 594, 98, 134, 22, ID_CHK_GAME_AUDIO);
    trkGameVol_ = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 730, 96, 114, 26, ID_TRK_GAME_VOL);
    SendMessageW(trkGameVol_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    lblGameVol_ = make(L"STATIC", L"%100", SS_RIGHT, 846, 100, 38, 20);

    chkMicAudio_ = makeChk(L"Mikrofon", 594, 130, 72, 22, ID_CHK_MIC_AUDIO);
    chkMicMute_ = makeChk(L"Sustur", 668, 130, 58, 22, ID_CHK_MIC_MUTE);
    trkMicVol_ = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 730, 128, 114, 26, ID_TRK_MIC_VOL);
    SendMessageW(trkMicVol_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    lblMicVol_ = make(L"STATIC", L"%100", SS_RIGHT, 846, 132, 38, 20);

    makeLbl(L"Cihaz:", 594, 163, 46, 20);
    cmbMicDev_ = makeHiddenCombo(642, 160, 240, 180, ID_CMB_MIC_DEV);

    chkMicGate_ = makeChk(L"Gürültü Kapısı", 594, 194, 108, 22, ID_CHK_MIC_GATE);
    trkMicGate_ = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 704, 192, 122, 26, ID_TRK_MIC_GATE);
    SendMessageW(trkMicGate_, TBM_SETRANGE, TRUE, MAKELPARAM(-60, -15));
    lblMicGate_ = make(L"STATIC", L"-36 dB", SS_RIGHT, 828, 195, 56, 20);

    // Card 3: Hotkeys, Folder, Notifications (x=242..898, y=258..528)
    makeLbl(L"Klip Alma Tuşu (ör. F12):", 258, 294, 165, 20);
    edtClipHotkey_ = make(L"EDIT", L"", ES_READONLY | WS_TABSTOP, 434, 295, 184, 17, 0, 0);
    btnSetClipHotkey_ = make(L"BUTTON", L"Tuş Ata", BS_OWNERDRAW | WS_TABSTOP, 632, 290, 96, 26, ID_BTN_SET_CLIP_HK);

    makeLbl(L"Mikrofon Susturma Tuşu:", 258, 328, 165, 20);
    edtMicHotkey_ = make(L"EDIT", L"", ES_READONLY | WS_TABSTOP, 434, 329, 184, 17, 0, 0);
    btnSetMicHotkey_ = make(L"BUTTON", L"Tuş Ata", BS_OWNERDRAW | WS_TABSTOP, 632, 324, 96, 26, ID_BTN_SET_MIC_HK);
    btnClearMicHotkey_ = make(L"BUTTON", L"Temizle", BS_OWNERDRAW | WS_TABSTOP, 736, 324, 86, 26, ID_BTN_CLR_MIC_HK);

    bool elev = HotkeyEngine::IsProcessElevated();
    wchar_t hkMsg[200] = {};
    swprintf(hkMsg, 200, L"Tuş algılama: Çift katman • %ls",
        elev ? L"Yönetici Modu Aktif" : L"Normal Mod (Yönetici oyunlar için yandaki buton)");
    lblHotkeyStatus_ = makeLbl(hkMsg, 258, 359, 488, 22);
    btnRestartAdmin_ = make(L"BUTTON", elev ? L"Yönetici Aktif" : L"Yönetici Başlat",
        BS_OWNERDRAW | WS_TABSTOP, 752, 357, 130, 26, ID_BTN_RESTART_ADMIN);
    if (elev) EnableWindow(btnRestartAdmin_, FALSE);

    makeLbl(L"Kayıt Klasörü:", 258, 396, 95, 20);
    edtOutDir_ = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 364, 397, 354, 17, 0, 0);
    btnBrowseDir_ = make(L"BUTTON", L"Seç...", BS_OWNERDRAW | WS_TABSTOP, 732, 392, 72, 26, ID_BTN_BROWSE_DIR);
    btnOpenDir_ = make(L"BUTTON", L"Aç", BS_OWNERDRAW | WS_TABSTOP, 810, 392, 72, 26, ID_BTN_OPEN_DIR);

    chkSound_ = makeChk(L"Klip alınca \"çıng\" bildirim sesi çal", 258, 432, 246, 22, ID_CHK_SOUND);
    btnTestSound_ = make(L"BUTTON", L"Sesi Test Et", BS_OWNERDRAW | WS_TABSTOP, 512, 429, 98, 26, ID_BTN_TEST_SOUND);
    chkAutoStart_ = makeChk(L"Windows açılışında arka planda başlat", 258, 466, 290, 22, ID_CHK_AUTOSTART);
    chkSpotifyAudio_ = makeChk(L"Spotify müziğini kliplere kaydet", 258, 496, 262, 22, ID_CHK_SPOTIFY_AUDIO);
    chkMobileSync_ = makeChk(L"Telefon Aktarımı (Wi-Fi)", 620, 432, 148, 22, ID_CHK_MOBILE_SYNC);
    btnShowQr_ = make(L"BUTTON", L"QR Kod / Bağlan", BS_OWNERDRAW | WS_TABSTOP, 772, 429, 110, 26, ID_BTN_MOBILE_QR);

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
    const bool changed = (nextTab != activeTab_);
    if (changed) {
        tabAnimDir_ = (nextTab > activeTab_) ? 1 : -1;
        tabAnimT_ = 0.0f;
        tabAnimStartMs_ = GetTickCount64();
    }
    activeTab_ = nextTab;
    if (hwnd_) SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
    for (HWND h : settingsCtrls_) {
        ShowWindow(h, activeTab_ == 1 ? SW_SHOW : SW_HIDE);
    }
    UpdateGalleryControlVisibility();
    if (hwnd_) {
        SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
        if (activeTab_ == 1) SetTimer(hwnd_, TIMER_MIC_METER_ID, 120, nullptr);
        else KillTimer(hwnd_, TIMER_MIC_METER_ID);
        if (changed) SetTimer(hwnd_, TIMER_UI_ANIM_ID, 10, nullptr);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
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
    SendMessageW(chkSpotifyAudio_, BM_SETCHECK, cfg.captureSpotify ? BST_CHECKED : BST_UNCHECKED, 0);
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
    SendMessageW(chkMobileSync_, BM_SETCHECK, cfg.mobileSyncEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
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
    out.captureSpotify = (SendMessageW(chkSpotifyAudio_, BM_GETCHECK, 0, 0) == BST_CHECKED);
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
    out.mobileSyncEnabled = (SendMessageW(chkMobileSync_, BM_GETCHECK, 0, 0) == BST_CHECKED);
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
    std::stable_partition(filteredIndices_.begin(), filteredIndices_.end(),
        [&](size_t idx) { return clips_[idx].isFavorite; });

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
            case WM_LBUTTONDBLCLK:
                self->TogglePlayerFullscreen();
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
        wc.style = CS_DBLCLKS;
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
    editCutActive_ = false;
    cutStartSec_ = 0.0;
    cutEndSec_ = 0.0;
    draggingTrim_ = 0;
    editHasMultiTrack_ = false;
    ProbeClipAudioTracks(playerClipPath_, editHasMultiTrack_, nullptr);
    editGameVolPct_ = 100;
    editMicVolPct_ = 100;
    editGameMuted_ = false;
    editMicMuted_ = false;
    draggingEditGameVol_ = false;
    draggingEditMicVol_ = false;
    editSpeed_ = 1.0;
    editVertical916_ = false;
    editInsertClipPath_.clear();

    videoHwnd_ = CreateWindowExW(0, vCls, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        S(250), S(62), S(640), S(360), hwnd_, nullptr, hInst_, this);
    if (!videoHwnd_) return;

    ++playerGen_;
    playerCb_ = new PlayerCallbackImpl(hwnd_, playerGen_);
    HRESULT hr = MFPCreateMediaPlayer(
        nullptr, FALSE, MFP_OPTION_NONE,
        playerCb_, videoHwnd_, &player_);
    if (SUCCEEDED(hr) && player_) {
        IMFPMediaItem* item = nullptr;
        hr = player_->CreateMediaItemFromURL(playerClipPath_.c_str(), TRUE, 0, &item);
        if (SUCCEEDED(hr) && item) {
            DWORD nStreams = 0;
            BOOL hasA = FALSE, selA = FALSE;
            item->HasAudio(&hasA, &selA);
            if (SUCCEEDED(item->GetNumberOfStreams(&nStreams)) && nStreams > 1) {
                std::vector<DWORD> audioStreams;
                for (DWORD si = 0; si < nStreams; ++si) {
                    PROPVARIANT vt;
                    PropVariantInit(&vt);
                    if (SUCCEEDED(item->GetStreamAttribute(si, MF_MT_MAJOR_TYPE, &vt)) &&
                        vt.vt == VT_CLSID && vt.puuid && *vt.puuid == MFMediaType_Audio) {
                        audioStreams.push_back(si);
                    }
                    PropVariantClear(&vt);
                }
                if (!audioStreams.empty()) {
                    // MPEG4MediaSource enumerates SinkWriter audio tracks in reverse (Mic, Game, Mix),
                    // so the last audio stream is the full Mix track.
                    DWORD chosenAudio = audioStreams.back();
                    for (DWORD asi : audioStreams) {
                        item->SetStreamSelection(asi, (asi == chosenAudio) ? TRUE : FALSE);
                    }
                }
            }
            Logger::Log("OpenEmbeddedPlayer: streams=%lu hasAudio=%d selAudio=%d",
                static_cast<unsigned long>(nStreams), hasA ? 1 : 0, selA ? 1 : 0);
            hr = player_->SetMediaItem(item);
            item->Release();
        }
    }
    if (FAILED(hr) || !player_) {
        Logger::Log("OpenEmbeddedPlayer failed hr=0x%08lx", (unsigned long)hr);
        CloseEmbeddedPlayer();
        SetFooterMessage(L"Klip uygulama içinde açılamadı, dış oynatıcı deneniyor.");
        PlayExternalSelected();
        return;
    }

    playerOpen_ = true;
    UpdateGalleryControlVisibility();
    {
        RECT cli{}; GetClientRect(hwnd_, &cli);
        UpdateLayoutForSize(cli.right, cli.bottom);
    }
    SetTimer(hwnd_, TIMER_PLAYER_ID, 80, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CloseEmbeddedPlayer() {
    if (playerFullscreen_) {
        TogglePlayerFullscreen();
    }
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

void MainWindow::StopUploadWorker() {
    uploadCancel_.store(true);
    if (uploadThread_.joinable()) {
        uploadThread_.join();
    }
    uploadCancel_.store(false);
    uploadBusy_ = false;
}

void MainWindow::StartUploadClip(const std::wstring& filePath) {
    if (uploadBusy_ || trimBusy_ || filePath.empty() || !hwnd_) return;
    StopUploadWorker();
    uploadBusy_ = true;
    uploadFilePath_ = filePath;
    uploadCancel_.store(false);
    SetFooterMessage(L"Link oluşturuluyor... %0");

    HWND target = hwnd_;
    uploadThread_ = std::thread([this, target, filePath]() {
        auto* res = new ClipUploadResult(UploadClipToShareLink(
            filePath,
            [target](int pct) {
                PostMessageW(target, WM_MW_UPLOAD_PROGRESS, static_cast<WPARAM>(pct), 0);
            },
            uploadCancel_));
        if (!PostMessageW(target, WM_MW_UPLOAD_DONE, 0, reinterpret_cast<LPARAM>(res))) {
            delete res;
        }
    });
}

void MainWindow::ShareSelected() {
    if (playerOpen_ && !playerClipPath_.empty()) {
        StartUploadClip(playerClipPath_);
        return;
    }
    if (selectedIdx_ < 0 || selectedIdx_ >= static_cast<int>(filteredIndices_.size())) return;
    StartUploadClip(clips_[filteredIndices_[static_cast<size_t>(selectedIdx_)]].filePath);
}

void MainWindow::CopySelectedShareLink() {
    if (selectedIdx_ < 0 || selectedIdx_ >= static_cast<int>(filteredIndices_.size())) return;
    const std::wstring& url = clips_[filteredIndices_[static_cast<size_t>(selectedIdx_)]].shareUrl;
    if (url.empty() || !hwnd_) return;
    if (OpenClipboard(hwnd_)) {
        EmptyClipboard();
        size_t bytes = (url.size() + 1) * sizeof(wchar_t);
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hg) {
            void* p = GlobalLock(hg);
            if (p) {
                memcpy(p, url.c_str(), bytes);
                GlobalUnlock(hg);
                SetClipboardData(CF_UNICODETEXT, hg);
            } else {
                GlobalFree(hg);
            }
        }
        CloseClipboard();
    }
    copyFeedbackUntilMs_ = GetTickCount64() + 1800;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OpenSelectedShareLink() {
    if (selectedIdx_ < 0 || selectedIdx_ >= static_cast<int>(filteredIndices_.size())) return;
    const std::wstring& url = clips_[filteredIndices_[static_cast<size_t>(selectedIdx_)]].shareUrl;
    if (url.empty()) return;
    HINSTANCE res = ShellExecuteW(nullptr, L"open", L"chrome.exe", url.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(res) <= 32) {
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void MainWindow::TogglePlayerFullscreen() {
    if (!hwnd_ || !playerOpen_ || trimBusy_) return;
    if (!playerFullscreen_) {
        savedWindowStyle_ = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
        savedWindowExStyle_ = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
        GetWindowRect(hwnd_, &savedWindowRc_);

        HMONITOR hMon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hMon, &mi);
        const int mw = mi.rcMonitor.right - mi.rcMonitor.left;
        const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;

        playerFullscreen_ = true;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (savedWindowStyle_ & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_POPUP);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, savedWindowExStyle_ & ~WS_EX_WINDOWEDGE);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        if (videoHwnd_) {
            SetWindowPos(videoHwnd_, nullptr, 0, 0, mw, std::max(100, mh - S(62)), SWP_NOZORDER | SWP_SHOWWINDOW);
        }
    } else {
        playerFullscreen_ = false;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, savedWindowStyle_);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, savedWindowExStyle_);
        SetWindowPos(hwnd_, nullptr, savedWindowRc_.left, savedWindowRc_.top,
            savedWindowRc_.right - savedWindowRc_.left, savedWindowRc_.bottom - savedWindowRc_.top,
            SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        if (videoHwnd_) {
            RECT cli{}; GetClientRect(hwnd_, &cli);
            UpdateLayoutForSize(cli.right, cli.bottom);
            ShowWindow(videoHwnd_, SW_SHOW);
        }
    }
    if (player_) player_->UpdateVideo();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ToggleTrimMode() {
    if (!playerOpen_ || trimBusy_) return;
    if (playerFullscreen_) TogglePlayerFullscreen();
    trimMode_ = !trimMode_;
    if (trimMode_) {
        double dur = (playerDurSec_ > 0.2) ? playerDurSec_ : 1.0;
        trimStartSec_ = 0.0;
        trimEndSec_ = dur;
        editCutActive_ = false;
        cutStartSec_ = 0.0;
        cutEndSec_ = 0.0;
    }
    if (videoHwnd_) {
        RECT cli{}; GetClientRect(hwnd_, &cli);
        UpdateLayoutForSize(cli.right, cli.bottom);
    }
    if (player_) player_->UpdateVideo();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::PickInsertClip() {
    if (!trimMode_ || trimBusy_) return;
    if (!editInsertClipPath_.empty()) {
        editInsertClipPath_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(dlg.Put()))) || !dlg) {
        return;
    }
    COMDLG_FILTERSPEC spec[] = { { L"MP4 Klipler (*.mp4)", L"*.mp4" } };
    dlg->SetFileTypes(1, spec);
    dlg->SetTitle(L"Araya / Sona Eklenecek Klibi Seçin");
    ComPtr<IShellItem> defFolder;
    if (!app_.Config().outputDir.empty() &&
        SUCCEEDED(SHCreateItemFromParsingName(app_.Config().outputDir.c_str(), nullptr, IID_PPV_ARGS(defFolder.Put()))) && defFolder) {
        dlg->SetFolder(defFolder.Get());
    }
    if (SUCCEEDED(dlg->Show(hwnd_))) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(dlg->GetResult(item.Put())) && item) {
            PWSTR psz = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
                if (_wcsicmp(psz, playerClipPath_.c_str()) != 0) {
                    editInsertClipPath_ = psz;
                } else {
                    SetFooterMessage(L"Aynı klibi kendisine ekleyemezsiniz, başka bir klip seçin.");
                }
                CoTaskMemFree(psz);
            }
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::StartTrimAsync(bool replaceOriginal, int targetSizeMb) {
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

    ClipEditSpec editSpec;
    editSpec.srcPath = origPath;
    editSpec.dstPath = outPath;
    editSpec.startSec = tStart;
    editSpec.endSec = tEnd;
    if (editCutActive_ && cutEndSec_ > cutStartSec_ + 0.1) {
        editSpec.cutStartSec = std::max(tStart + 0.1, cutStartSec_);
        editSpec.cutEndSec = std::min(tEnd - 0.1, cutEndSec_);
    }
    editSpec.insertClipPath = editInsertClipPath_;
    editSpec.speed = editSpeed_;
    editSpec.gameVolPct = editGameMuted_ ? 0 : editGameVolPct_;
    editSpec.micVolPct = editMicMuted_ ? 0 : editMicVolPct_;
    editSpec.targetSizeMb = targetSizeMb;
    editSpec.vertical916 = editVertical916_;

    SetFooterMessage(replaceOriginal
        ? L"Ana klip düzenleniyor, lütfen bekleyin..."
        : L"Düzenlenen klipten yeni kopya oluşturuluyor...");

    HWND target = hwnd_;
    trimThread_ = std::thread([this, target, origPath, outPath, editSpec, replaceOriginal]() {
        ComScope com(COINIT_MULTITHREADED);
        std::string errUtf8;
        bool ok = EditMp4Clip(editSpec, errUtf8, &trimCancel_);
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
    if (player_) {
        HRESULT hr = player_->SetRate(playerRate_);
        if (FAILED(hr)) {
            float minR = 0.5f, maxR = 1.0f;
            if (SUCCEEDED(player_->GetSupportedRates(FALSE, &minR, &maxR)) && maxR >= 0.5f) {
                playerRate_ = std::max(minR, std::min(maxR, playerRate_));
                player_->SetRate(playerRate_);
            }
        }
    }
    if (hwnd_) {
        RECT cli{}; GetClientRect(hwnd_, &cli);
        RECT ctrlRc = playerFullscreen_
            ? RECT{ 0, std::max<LONG>(0, cli.bottom - S(72)), cli.right, cli.bottom }
            : RECT{ S(236), std::max<LONG>(0, cli.bottom - S(276)), cli.right, cli.bottom };
        InvalidateRect(hwnd_, &ctrlRc, FALSE);
    }
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
    if (hwnd_) {
        RECT cli{}; GetClientRect(hwnd_, &cli);
        RECT ctrlRc = playerFullscreen_
            ? RECT{ 0, std::max<LONG>(0, cli.bottom - S(72)), cli.right, cli.bottom }
            : RECT{ S(236), std::max<LONG>(0, cli.bottom - S(276)), cli.right, cli.bottom };
        InvalidateRect(hwnd_, &ctrlRc, FALSE);
    }
}

RECT MainWindow::GetSeekBarRect() const {
    RECT cli{ 0, 0, S(920), S(620) };
    if (hwnd_) GetClientRect(hwnd_, &cli);
    const int cw = cli.right, ch = cli.bottom;
    if (playerFullscreen_) {
        return RECT{ S(16), ch - S(58), cw - S(16), ch - S(36) };
    }
    if (trimMode_) {
        return RECT{ S(254), ch - S(264), cw - S(34), ch - S(236) };
    }
    return RECT{ S(254), ch - S(176), cw - S(34), ch - S(152) };
}

RECT MainWindow::GetVolumeBarRect() const {
    RECT cli{ 0, 0, S(920), S(620) };
    if (hwnd_) GetClientRect(hwnd_, &cli);
    const int cw = cli.right, ch = cli.bottom;
    if (playerFullscreen_) {
        return RECT{ cw - S(230), ch - S(28), cw - S(126), ch - S(8) };
    }
    if (trimMode_) {
        return RECT{ cw - S(140), ch - S(148), cw - S(34), ch - S(124) };
    }
    return RECT{ cw - S(140), ch - S(118), cw - S(34), ch - S(94) };
}

RECT MainWindow::GetGameVolBarRect() const {
    RECT cli{ 0, 0, S(920), S(620) };
    if (hwnd_) GetClientRect(hwnd_, &cli);
    int rx = cli.right - S(156);
    return RECT{ rx, S(100), cli.right - S(32), S(120) };
}

RECT MainWindow::GetMicVolBarRect() const {
    RECT cli{ 0, 0, S(920), S(620) };
    if (hwnd_) GetClientRect(hwnd_, &cli);
    int rx = cli.right - S(156);
    return RECT{ rx, S(182), cli.right - S(32), S(202) };
}

// ================= GALLERY & HIT TESTING =================

RECT MainWindow::GetGridViewportRect() const {
    RECT cli{ 0, 0, S(920), S(620) };
    if (hwnd_) GetClientRect(hwnd_, &cli);
    return RECT{ S(242), S(82), cli.right - S(22), cli.bottom - S(86) };
}

void MainWindow::GetGridMetrics(int& outCols, int& outCardW, int& outCardH, int& outGapX, int& outGapY) const {
    RECT vp = GetGridViewportRect();
    int availW = std::max(S(320), static_cast<int>(vp.right - vp.left - S(14)));
    outCols = (availW >= S(920)) ? 3 : 2;
    outGapX = S(16);
    outGapY = S(14);
    outCardW = (availW - (outCols - 1) * outGapX) / outCols;
    int thumbW = std::max(S(160), outCardW - S(14));
    int thumbH = (thumbW * 9) / 16;
    outCardH = thumbH + S(58);
}

void MainWindow::ComputeGalleryLayout(std::vector<RECT>& outCardRects, int& outFavHeaderY, int& outNormHeaderY,
                                      int& outTotalH, int& outFavCount, int& outNormCount) const {
    outCardRects.clear();
    outFavHeaderY = -1;
    outNormHeaderY = -1;
    outTotalH = 0;
    outFavCount = 0;
    outNormCount = 0;
    if (filteredIndices_.empty()) return;

    RECT vp = GetGridViewportRect();
    int cols = 2, cardW = S(312), cardH = S(226), gapX = S(16), gapY = S(14);
    GetGridMetrics(cols, cardW, cardH, gapX, gapY);

    for (size_t idx : filteredIndices_) {
        if (clips_[idx].isFavorite) ++outFavCount;
        else break;
    }
    outNormCount = static_cast<int>(filteredIndices_.size()) - outFavCount;
    outCardRects.resize(filteredIndices_.size());

    int curY = vp.top;
    if (outFavCount > 0) {
        outFavHeaderY = curY;
        curY += S(30);
        for (int i = 0; i < outFavCount; ++i) {
            int col = i % cols;
            int row = i / cols;
            int x = vp.left + col * (cardW + gapX);
            int y = curY + row * (cardH + gapY);
            outCardRects[static_cast<size_t>(i)] = RECT{ x, y, x + cardW, y + cardH };
        }
        int favRows = (outFavCount + cols - 1) / cols;
        curY += favRows * cardH + (favRows - 1) * gapY;
        if (outNormCount > 0) {
            curY += S(20);
        }
    }

    if (outNormCount > 0) {
        outNormHeaderY = curY;
        curY += S(30);
        for (int j = 0; j < outNormCount; ++j) {
            int col = j % cols;
            int row = j / cols;
            int x = vp.left + col * (cardW + gapX);
            int y = curY + row * (cardH + gapY);
            outCardRects[static_cast<size_t>(outFavCount + j)] = RECT{ x, y, x + cardW, y + cardH };
        }
        int normRows = (outNormCount + cols - 1) / cols;
        curY += normRows * cardH + (normRows - 1) * gapY;
    }

    outTotalH = curY - vp.top;
}

void MainWindow::ClampScroll() {
    RECT vp = GetGridViewportRect();
    int vpH = vp.bottom - vp.top;
    std::vector<RECT> cardRects;
    int favHy = -1, normHy = -1, totalH = 0, favCnt = 0, normCnt = 0;
    ComputeGalleryLayout(cardRects, favHy, normHy, totalH, favCnt, normCnt);
    maxScrollY_ = std::max(0, totalH - vpH);
    scrollY_ = std::max(0, std::min(maxScrollY_, scrollY_));
}

int MainWindow::HitTestCard(int mx, int my) const {
    if (activeTab_ != 0 || playerOpen_) return -1;
    RECT vp = GetGridViewportRect();
    if (mx < vp.left || mx > vp.right - S(12) || my < vp.top || my > vp.bottom) return -1;

    std::vector<RECT> cardRects;
    int favHy = -1, normHy = -1, totalH = 0, favCnt = 0, normCnt = 0;
    ComputeGalleryLayout(cardRects, favHy, normHy, totalH, favCnt, normCnt);

    for (size_t vi = 0; vi < cardRects.size(); ++vi) {
        RECT r = cardRects[vi];
        r.top -= scrollY_;
        r.bottom -= scrollY_;
        if (mx >= r.left && mx <= r.right && my >= r.top && my <= r.bottom) {
            return static_cast<int>(vi);
        }
    }
    return -1;
}

int MainWindow::HitTestCardStar(int mx, int my) const {
    if (activeTab_ != 0 || playerOpen_) return -1;
    RECT vp = GetGridViewportRect();
    if (mx < vp.left || mx > vp.right - S(12) || my < vp.top || my > vp.bottom) return -1;

    std::vector<RECT> cardRects;
    int favHy = -1, normHy = -1, totalH = 0, favCnt = 0, normCnt = 0;
    ComputeGalleryLayout(cardRects, favHy, normHy, totalH, favCnt, normCnt);

    for (size_t vi = 0; vi < cardRects.size(); ++vi) {
        int cx = cardRects[vi].left;
        int cy = cardRects[vi].top - scrollY_;
        int cardW = cardRects[vi].right - cardRects[vi].left;
        int tx = cx + S(7), ty = cy + S(7), tw = cardW - S(14);
        int sx = tx + tw - S(32), sy = ty + S(6);
        if (mx >= sx && mx <= sx + S(26) && my >= sy && my <= sy + S(24)) {
            return static_cast<int>(vi);
        }
    }
    return -1;
}

int MainWindow::HitTestTrimHandle(int mx, int my) const {
    if (activeTab_ != 0 || !playerOpen_ || !trimMode_ || trimBusy_ || playerFullscreen_) return 0;
    RECT sk = GetSeekBarRect();
    if (my < sk.top - S(4) || my > sk.bottom + S(4)) return 0;
    double dur = (playerDurSec_ > 0.05) ? playerDurSec_ : 1.0;
    int skW = sk.right - sk.left;
    const int tol = S(10);
    if (editCutActive_) {
        int xc0 = sk.left + static_cast<int>(skW * std::max(0.0, std::min(1.0, cutStartSec_ / dur)));
        int xc1 = sk.left + static_cast<int>(skW * std::max(0.0, std::min(1.0, cutEndSec_ / dur)));
        if (std::abs(mx - xc0) <= tol) return 3;
        if (std::abs(mx - xc1) <= tol) return 4;
    }
    int xStart = sk.left + static_cast<int>(skW * std::max(0.0, std::min(1.0, trimStartSec_ / dur)));
    int xEnd   = sk.left + static_cast<int>(skW * std::max(0.0, std::min(1.0, trimEndSec_ / dur)));
    int dStart = std::abs(mx - xStart);
    int dEnd   = std::abs(mx - xEnd);
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
        UpdateClipLinkOnRename(GetClipLinksFilePath(), oldName, newName);
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
        SetClipShareLink(GetClipLinksFilePath(), c.fileName, L"");
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

    if (!memDc_ || memW_ != cw || memH_ != ch) {
        FreeBackbuffer();
        memDc_ = CreateCompatibleDC(targetDc);
        memBmp_ = CreateCompatibleBitmap(targetDc, cw, ch);
        memOldBmp_ = SelectObject(memDc_, memBmp_);
        memW_ = cw;
        memH_ = ch;
    }
    HDC memDc = memDc_;

    {
        Gdiplus::Graphics g(memDc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        g.Clear(Gc(9, 12, 19));

        // Ambient radial/gradient glow orbs (vibrant modern backdrop)
        if (!playerFullscreen_) {
            {
                Gdiplus::GraphicsPath gp;
                gp.AddEllipse(S(180), -S(120), S(520), S(420));
                Gdiplus::PathGradientBrush pgb(&gp);
                pgb.SetCenterColor(Gc(37, 99, 235, 42));
                Gdiplus::Color sur[1] = { Gc(37, 99, 235, 0) };
                int cnt = 1;
                pgb.SetSurroundColors(sur, &cnt);
                g.FillPath(&pgb, &gp);
            }
            {
                Gdiplus::GraphicsPath gp2;
                gp2.AddEllipse(cw - S(340), -S(80), S(460), S(380));
                Gdiplus::PathGradientBrush pgb2(&gp2);
                pgb2.SetCenterColor(Gc(139, 92, 246, 36));
                Gdiplus::Color sur2[1] = { Gc(139, 92, 246, 0) };
                int cnt2 = 1;
                pgb2.SetSurroundColors(sur2, &cnt2);
                g.FillPath(&pgb2, &gp2);
            }
        }

        // Sleek gradient sidebar panel
        {
            Gdiplus::LinearGradientBrush sbSide(
                Gdiplus::PointF(0, 0), Gdiplus::PointF(0, (float)std::max(1, ch)),
                Gc(18, 24, 38), Gc(11, 15, 24));
            g.FillRectangle(&sbSide, 0, 0, S(220), ch);
        }
        Gdiplus::Pen sepPen(Gc(42, 54, 80), 1.0f);
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

        Gdiplus::StringFormat sfRight;
        sfRight.SetAlignment(Gdiplus::StringAlignmentFar);
        sfRight.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        sfRight.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        const float pulse01 = static_cast<float>(0.5 + 0.5 * std::sin(animPhase_));

        // Brand emblem (blue->violet gradient + pulsing live ring)
        FillRoundRectGrad(g, (float)S(16), (float)S(18), (float)S(36), (float)S(36), (float)S(10),
            Gc(59, 130, 246), Gc(124, 58, 237));
        {
            Gdiplus::SolidBrush brDot(Gc(255, 255, 255));
            g.FillEllipse(&brDot, (float)S(26), (float)S(28), (float)S(16), (float)S(16));
            Gdiplus::SolidBrush brInner(app_.IsRunning() ? Gc(239, 68, 68) : Gc(100, 116, 139));
            g.FillEllipse(&brInner, (float)S(29), (float)S(31), (float)S(10), (float)S(10));
        }
        g.DrawString(L"Anlık Klip", -1, &fTitle, Gdiplus::RectF((float)S(60), (float)S(16), (float)S(102), (float)S(24)), &sfLeft, &brText);
        FillRoundRect(g, (float)S(166), (float)S(20), (float)S(38), (float)S(18), (float)S(9), Gc(59, 130, 246, 48));
        StrokeRoundRect(g, (float)S(166), (float)S(20), (float)S(38), (float)S(18), (float)S(9), Gc(96, 165, 250, 125));
        {
            Gdiplus::SolidBrush brVer(Gc(147, 197, 253));
            g.DrawString(L"v1.4", -1, &fSmall, Gdiplus::RectF((float)S(166), (float)S(20), (float)S(38), (float)S(18)), &sfCenter, &brVer);
        }
        wchar_t resSub[48] = {};
        swprintf(resSub, 48, L"%dp • %d FPS", app_.Config().height, app_.Config().fps);
        g.DrawString(resSub, -1, &fSub, Gdiplus::RectF((float)S(60), (float)S(38), (float)S(146), (float)S(18)), &sfLeft, &brMuted);

        // Live status capsule with animated pulse halo
        AppRuntimeStats st = app_.GetStats();
        bool recOn = st.running;
        FillRoundRectGrad(g, (float)S(16), (float)S(72), (float)S(188), (float)S(28), (float)S(14),
            recOn ? Gc(22, 101, 52, 190) : Gc(51, 65, 85, 160),
            recOn ? Gc(15, 72, 38, 175) : Gc(36, 46, 62, 160));
        StrokeRoundRect(g, (float)S(16), (float)S(72), (float)S(188), (float)S(28), (float)S(14),
            recOn ? Gc(74, 222, 128, static_cast<uint8_t>(140 + 80 * pulse01)) : Gc(100, 116, 139, 160));
        {
            if (recOn) {
                Gdiplus::SolidBrush haloB(Gc(74, 222, 128, static_cast<uint8_t>(40 + 90 * pulse01)));
                g.FillEllipse(&haloB, (float)S(24), (float)S(78), (float)S(16), (float)S(16));
            }
            Gdiplus::SolidBrush dotB(recOn ? Gc(74, 222, 128) : Gc(148, 163, 184));
            g.FillEllipse(&dotB, (float)S(28), (float)S(82), (float)S(8), (float)S(8));
            Gdiplus::SolidBrush statusTxtB(recOn ? Gc(209, 250, 229) : Gc(203, 213, 225));
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
            Gdiplus::Color fill = !enabled ? Gc(26, 31, 42) : (prs ? hoverCol : (hov ? hoverCol : baseCol));
            if (enabled && !prs) {
                Gdiplus::Color topC(fill.GetA(),
                    static_cast<uint8_t>(std::min(255, fill.GetR() + 12)),
                    static_cast<uint8_t>(std::min(255, fill.GetG() + 12)),
                    static_cast<uint8_t>(std::min(255, fill.GetB() + 14)));
                FillRoundRectGrad(g, (float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), (float)S(9), topC, fill);
            } else {
                FillRoundRect(g, (float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), (float)S(9), fill);
            }
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

        // Animated sliding active tab pill in sidebar
        {
            float targetY = (activeTab_ == 1) ? 160.0f : 118.0f;
            float pillY = tabIndicatorY_ + (targetY - tabIndicatorY_) * tabAnimT_;
            FillRoundRectGrad(g, (float)S(16), (float)S(static_cast<int>(pillY)), (float)S(188), (float)S(36), (float)S(9),
                Gc(37, 99, 235, 70), Gc(99, 102, 241, 40));
            StrokeRoundRect(g, (float)S(16), (float)S(static_cast<int>(pillY)), (float)S(188), (float)S(36), (float)S(9),
                Gc(96, 165, 250, 170));
            FillRoundRect(g, (float)S(20), (float)S(static_cast<int>(pillY) + 8), (float)S(3), (float)S(20), 1.5f, Gc(56, 189, 248));
        }

        // Sidebar tabs
        wchar_t cntStr[16] = {};
        swprintf(cntStr, 16, L"%zu", clips_.size());
        addBtn(BTN_TAB_CLIPS, 16, 118, 188, 36, L"Kliplerim",
            Gc(0, 0, 0, 0), Gc(34, 45, 70, 140),
            Gc(0, 0, 0, 0), true, false, cntStr);
        addBtn(BTN_TAB_SETTINGS, 16, 160, 188, 36, L"Ayarlar",
            Gc(0, 0, 0, 0), Gc(34, 45, 70, 140),
            Gc(0, 0, 0, 0), true, false);

        // Telemetry card & quick controls anchored cleanly to bottom of sidebar
        const int sideAnchorY = std::max(0, (ch - S(620)));
        const int telY = S(358) + sideAnchorY;
        FillRoundRect(g, (float)S(16), (float)telY, (float)S(188), (float)S(120), (float)S(10), Gc(22, 28, 42));
        StrokeRoundRect(g, (float)S(16), (float)telY, (float)S(188), (float)S(120), (float)S(10), Gc(44, 56, 80));
        double bufMb = static_cast<double>(st.ring.videoBytes + st.ring.audioBytes) / (1024.0 * 1024.0);
        wchar_t v1[32], v2[32], v3[32], t4[64];
        swprintf(v1, 32, L"%.1f MB", st.workingSetMb);
        swprintf(v2, 32, L"%.1f MB", bufMb);
        double dispSec = std::max(0.0, std::min(st.ring.bufferedSeconds, static_cast<double>(app_.Config().clipSeconds)));
        swprintf(v3, 32, L"%.0f / %d sn", dispSec, app_.Config().clipSeconds);
        std::wstring hk = FormatHotkey(app_.Config().hotkeyMods, app_.Config().hotkeyVk);
        swprintf(t4, 64, L"Kısayol: %ls", hk.c_str());
        g.DrawString(L"RAM", -1, &fSmall, Gdiplus::RectF((float)S(28), (float)(telY + S(10)), (float)S(72), (float)S(20)), &sfLeft, &brMuted);
        g.DrawString(v1, -1, &fSub, Gdiplus::RectF((float)S(92), (float)(telY + S(10)), (float)S(100), (float)S(20)), &sfRight, &brText);
        g.DrawString(L"Tampon", -1, &fSmall, Gdiplus::RectF((float)S(28), (float)(telY + S(32)), (float)S(72), (float)S(20)), &sfLeft, &brMuted);
        g.DrawString(v2, -1, &fSub, Gdiplus::RectF((float)S(92), (float)(telY + S(32)), (float)S(100), (float)S(20)), &sfRight, &brText);
        g.DrawString(L"Süre", -1, &fSmall, Gdiplus::RectF((float)S(28), (float)(telY + S(54)), (float)S(72), (float)S(20)), &sfLeft, &brMuted);
        g.DrawString(v3, -1, &fSub, Gdiplus::RectF((float)S(92), (float)(telY + S(54)), (float)S(100), (float)S(20)), &sfRight, &brText);
        {
            float pFrac = app_.Config().clipSeconds > 0
                ? static_cast<float>(std::max(0.0, std::min(1.0, st.ring.bufferedSeconds / app_.Config().clipSeconds)))
                : 0.0f;
            FillRoundRect(g, (float)S(28), (float)(telY + S(75)), (float)S(164), (float)S(4), 2.0f, Gc(36, 44, 62));
            if (pFrac > 0.01f) {
                FillRoundRectGrad(g, (float)S(28), (float)(telY + S(75)), (float)S(164) * pFrac, (float)S(4), 2.0f,
                    Gc(59, 130, 246), Gc(37, 99, 235));
            }
        }
        FillRoundRect(g, (float)S(28), (float)(telY + S(87)), (float)S(164), (float)S(24), (float)S(6), Gc(37, 99, 235, 55));
        Gdiplus::SolidBrush brBlueTxt(Gc(147, 197, 253));
        g.DrawString(t4, -1, &fSmall, Gdiplus::RectF((float)S(28), (float)(telY + S(87)), (float)S(164), (float)S(24)), &sfCenter, &brBlueTxt);

        auto addBtnPx = [&](int id, int px, int py, int pw, int ph, const std::wstring& label,
                            Gdiplus::Color baseCol, Gdiplus::Color hoverCol, Gdiplus::Color borderCol,
                            bool enabled = true) {
            RECT rc{ px, py, px + pw, py + ph };
            customButtons_.push_back({ id, rc, enabled });
            bool hov = enabled && (hoverBtnId_ == id);
            bool prs = enabled && (pressedBtnId_ == id);
            Gdiplus::Color fill = !enabled ? Gc(26, 31, 42) : (prs ? hoverCol : (hov ? hoverCol : baseCol));
            if (enabled && !prs) {
                Gdiplus::Color topC(fill.GetA(),
                    static_cast<uint8_t>(std::min(255, fill.GetR() + 12)),
                    static_cast<uint8_t>(std::min(255, fill.GetG() + 12)),
                    static_cast<uint8_t>(std::min(255, fill.GetB() + 14)));
                FillRoundRectGrad(g, (float)rc.left, (float)rc.top, (float)pw, (float)ph, (float)S(9), topC, fill);
            } else {
                FillRoundRect(g, (float)rc.left, (float)rc.top, (float)pw, (float)ph, (float)S(9), fill);
            }
            if (borderCol.GetA() > 0) StrokeRoundRect(g, (float)rc.left, (float)rc.top, (float)pw, (float)ph, (float)S(9), borderCol);
            Gdiplus::SolidBrush tb(!enabled ? Gc(100, 112, 132) : Gc(242, 246, 252));
            g.DrawString(label.c_str(), -1, &fBold, Gdiplus::RectF((float)rc.left, (float)rc.top, (float)pw, (float)ph), &sfCenter, &tb);
        };

        // Quick controls at bottom of sidebar
        const int btnBaseY = S(492) + sideAnchorY;
        addBtnPx(BTN_SIDE_SAVE, S(16), btnBaseY, S(188), S(38), L"●  Hemen Klip Al",
            Gc(37, 99, 235), Gc(59, 130, 246), Gc(96, 165, 250, 150), st.running && !st.saving);
        addBtnPx(BTN_SIDE_TOGGLE, S(16), btnBaseY + S(46), S(90), S(32), st.running ? L"Duraklat" : L"Başlat",
            st.running ? Gc(30, 37, 52) : Gc(22, 163, 74),
            st.running ? Gc(42, 51, 70) : Gc(34, 197, 94), Gc(52, 64, 86));
        addBtnPx(BTN_SIDE_MIC, S(114), btnBaseY + S(46), S(90), S(32), app_.Config().micMuted ? L"Mik: Sustu" : L"Mik: Açık",
            app_.Config().micMuted ? Gc(180, 83, 9) : Gc(30, 37, 52),
            app_.Config().micMuted ? Gc(217, 119, 6) : Gc(42, 51, 70), Gc(52, 64, 86));
        g.DrawString(L"X ile kapatınca tepside çalışır", -1, &fSmall,
            Gdiplus::RectF((float)S(16), (float)(btnBaseY + S(90)), (float)S(188), (float)S(20)), &sfCenter, &brMuted);

        if (activeTab_ == 0 && playerOpen_ && playerFullscreen_) {
            // ================= FULLSCREEN PLAYER OVERLAY BAR =================
            Gdiplus::SolidBrush fsBg(Gc(10, 12, 18));
            g.FillRectangle(&fsBg, 0, 0, cw, ch);
            Gdiplus::SolidBrush barBg(Gc(16, 20, 29));
            g.FillRectangle(&barBg, 0, ch - S(62), cw, S(62));

            RECT sk = GetSeekBarRect();
            float skW = static_cast<float>(sk.right - sk.left);
            float trackY = static_cast<float>(sk.top + (sk.bottom - sk.top) / 2 - S(3));
            float trackH = static_cast<float>(S(6));
            FillRoundRect(g, (float)sk.left, trackY, skW, trackH, (float)S(3), Gc(42, 51, 68));
            double dur = (playerDurSec_ > 0.05) ? playerDurSec_ : 1.0;
            double ratio = std::max(0.0, std::min(1.0, playerPosSec_ / dur));
            float fillW = static_cast<float>(skW * ratio);
            if (fillW > 2.0f) FillRoundRect(g, (float)sk.left, trackY, fillW, trackH, (float)S(3), Gc(59, 130, 246));
            Gdiplus::SolidBrush knobBr(Gc(248, 250, 252));
            float knobX = static_cast<float>(sk.left) + fillW;
            g.FillEllipse(&knobBr, knobX - S(6), trackY + trackH * 0.5f - S(6), (float)S(12), (float)S(12));

            auto addFsBtn = [&](int id, int px, int py, int pw, int ph, const wchar_t* text,
                                Gdiplus::Color base, Gdiplus::Color hov, Gdiplus::Color brd, bool en = true) {
                RECT rc{ px, py, px + pw, py + ph };
                customButtons_.push_back({ id, rc, en });
                Gdiplus::Color bg = !en ? Gc(24, 29, 40) : (pressedBtnId_ == id ? hov : (hoverBtnId_ == id ? hov : base));
                FillRoundRect(g, (float)rc.left, (float)rc.top, (float)pw, (float)ph, (float)S(7), bg);
                StrokeRoundRect(g, (float)rc.left, (float)rc.top, (float)pw, (float)ph, (float)S(7), en ? brd : Gc(38, 45, 58));
                Gdiplus::SolidBrush tb(en ? Gc(242, 246, 252) : Gc(110, 122, 142));
                g.DrawString(text, -1, &fBold, Gdiplus::RectF((float)rc.left, (float)rc.top, (float)pw, (float)ph), &sfCenter, &tb);
            };

            bool canPlay = playerReady_ && !trimBusy_;
            int by = ch - S(34);
            addFsBtn(BTN_PLY_PLAYPAUSE, S(16), by, S(96), S(28),
                playerPlaying_ ? L"⏸ Duraklat" : (playerEnded_ ? L"↻ Tekrar" : L"▶ Oynat"),
                Gc(37, 99, 235), Gc(59, 130, 246), Gc(96, 165, 250, 150), canPlay);
            addFsBtn(BTN_PLY_MINUS5, S(118), by, S(50), S(28), L"-5sn", Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), canPlay);
            addFsBtn(BTN_PLY_PLUS5, S(174), by, S(50), S(28), L"+5sn", Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), canPlay);
            wchar_t rateStr[16] = {};
            swprintf(rateStr, 16, L"%.1fx", playerRate_);
            addFsBtn(BTN_PLY_RATE, S(230), by, S(46), S(28), rateStr, Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), canPlay);

            wchar_t timeTxt[64] = {};
            swprintf(timeTxt, 64, L"%ls / %ls", FormatDurClock(playerPosSec_).c_str(), FormatDurClock(playerDurSec_).c_str());
            g.DrawString(timeTxt, -1, &fBold, Gdiplus::RectF((float)S(288), (float)by, (float)S(160), (float)S(28)), &sfLeft, &brText);

            addFsBtn(BTN_PLY_MUTE, cw - S(284), by, S(48), S(28), playerMuted_ ? L"Sessiz" : L"Ses",
                playerMuted_ ? Gc(180, 83, 9) : Gc(32, 39, 53),
                playerMuted_ ? Gc(217, 119, 6) : Gc(44, 54, 72), Gc(56, 68, 90), canPlay);
            RECT vb = GetVolumeBarRect();
            float vbW = static_cast<float>(vb.right - vb.left);
            float vbY = static_cast<float>(vb.top + (vb.bottom - vb.top) / 2 - S(3));
            FillRoundRect(g, (float)vb.left, vbY, vbW, (float)S(6), (float)S(3), Gc(42, 51, 68));
            float vFill = vbW * (playerMuted_ ? 0.0f : playerVol_);
            if (vFill > 1.0f) FillRoundRect(g, (float)vb.left, vbY, vFill, (float)S(6), (float)S(3), Gc(34, 197, 94));
            g.FillEllipse(&knobBr, (float)vb.left + vFill - S(6), vbY + S(3) - S(6), (float)S(12), (float)S(12));

            addFsBtn(BTN_PLY_FULLSCREEN, cw - S(116), by, S(100), S(28), L"⛶ Çık (Esc)",
                Gc(32, 39, 53), Gc(59, 130, 246), Gc(96, 165, 250), !trimBusy_);
        } else if (activeTab_ == 0 && playerOpen_) {
            // ================= TAB 0 (EMBEDDED VIDEO PLAYER + TRIM) =================
            bool curFav = false;
            for (const auto& cc : clips_) {
                if (_wcsicmp(cc.filePath.c_str(), playerClipPath_.c_str()) == 0) {
                    curFav = cc.isFavorite;
                    break;
                }
            }
            addBtnPx(BTN_PLY_BACK, S(242), S(18), S(114), S(34), L"←  Kliplere", Gc(28, 34, 47), Gc(40, 49, 68), Gc(54, 66, 90), !trimBusy_);
            g.DrawString(playerClipTitle_.c_str(), -1, &fBold,
                Gdiplus::RectF((float)S(364), (float)S(16), (float)std::max(S(140), cw - S(816)), (float)S(20)), &sfLeft, &brText);
            g.DrawString(L"Space: Oynat • ←/→: ±5sn • F: Tam Ekran • Esc: Geri", -1, &fSmall,
                Gdiplus::RectF((float)S(364), (float)S(36), (float)std::max(S(180), cw - S(816)), (float)S(16)), &sfLeft, &brMuted);

            addBtnPx(BTN_PLY_SHARE, cw - S(444), S(18), S(82), S(34), uploadBusy_ ? L"Yükleniyor" : L"Link Al",
                Gc(28, 34, 47), Gc(40, 49, 68), Gc(52, 64, 86), !trimBusy_ && !uploadBusy_);
            addBtnPx(BTN_PLY_FAV, cw - S(356), S(18), S(74), S(34), curFav ? L"★ Fav" : L"☆ Fav",
                curFav ? Gc(92, 68, 14) : Gc(28, 34, 47),
                curFav ? Gc(120, 88, 18) : Gc(40, 49, 68),
                curFav ? Gc(250, 204, 21) : Gc(52, 64, 86), !trimBusy_);
            addBtnPx(BTN_PLY_RENAME, cw - S(276), S(18), S(84), S(34), L"Adlandır", Gc(28, 34, 47), Gc(40, 49, 68), Gc(52, 64, 86), !trimBusy_);
            addBtnPx(BTN_PLY_FOLDER, cw - S(186), S(18), S(94), S(34), L"Klasörde Aç", Gc(28, 34, 47), Gc(40, 49, 68), Gc(52, 64, 86), !trimBusy_);
            addBtnPx(BTN_PLY_DELETE, cw - S(86), S(18), S(64), S(34), L"Sil", Gc(153, 27, 27), Gc(185, 28, 28), Gc(220, 38, 38), !trimBusy_);

            // Frame border around dynamic child video HWND
            RECT vrc{ S(246), S(58), cw - S(26), ch - (trimMode_ ? S(286) : S(198)) };
            if (videoHwnd_) {
                GetWindowRect(videoHwnd_, &vrc);
                MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<LPPOINT>(&vrc), 2);
            }
            StrokeRoundRect(g, (float)(vrc.left - 1), (float)(vrc.top - 1),
                (float)(vrc.right - vrc.left + 2), (float)(vrc.bottom - vrc.top + 2), 4.0f, Gc(45, 55, 74), 1.0f);
            if (trimBusy_) {
                FillRoundRect(g, (float)vrc.left, (float)vrc.top, (float)(vrc.right - vrc.left), (float)(vrc.bottom - vrc.top), 4.0f, Gc(15, 19, 27));
                g.DrawString(L"✂  Klip düzenleniyor, lütfen bekleyin...", -1, &fTitle,
                    Gdiplus::RectF((float)vrc.left, (float)vrc.top, (float)(vrc.right - vrc.left), (float)(vrc.bottom - vrc.top)), &sfCenter, &brText);
            }

            if (trimMode_) {
                // Right-side Audio & Edit panel anchored to right edge
                const int rpX = cw - S(166);
                const int rpH = std::max(S(266), ch - S(340));
                FillRoundRect(g, (float)rpX, (float)S(56), (float)S(144), (float)rpH, (float)S(10), Gc(20, 25, 35));
                StrokeRoundRect(g, (float)rpX, (float)S(56), (float)S(144), (float)rpH, (float)S(10), Gc(40, 48, 65));
                g.DrawString(L"Ses & Edit", -1, &fBold,
                    Gdiplus::RectF((float)(rpX + S(10)), (float)S(64), (float)S(124), (float)S(20)), &sfLeft, &brText);

                auto drawMiniSlider = [&](RECT r, int pct, bool muted, Gdiplus::Color col) {
                    float w = static_cast<float>(r.right - r.left);
                    float y = static_cast<float>(r.top + (r.bottom - r.top) / 2 - S(3));
                    FillRoundRect(g, (float)r.left, y, w, (float)S(6), (float)S(3), Gc(42, 51, 68));
                    float frac = muted ? 0.0f : std::max(0.0f, std::min(1.0f, static_cast<float>(pct) / 200.0f));
                    if (w * frac > 2.0f) FillRoundRect(g, (float)r.left, y, w * frac, (float)S(6), (float)S(3), col);
                    Gdiplus::SolidBrush kb(Gc(248, 250, 252));
                    g.FillEllipse(&kb, (float)r.left + w * frac - S(5), y + S(3) - S(5), (float)S(10), (float)S(10));
                };

                bool canPlay = playerReady_ && !trimBusy_;
                wchar_t gLbl[48] = {};
                swprintf(gLbl, 48, L"Oyun/PC: %d%%", editGameMuted_ ? 0 : editGameVolPct_);
                g.DrawString(gLbl, -1, &fSmall, Gdiplus::RectF((float)(rpX + S(10)), (float)S(84), (float)S(124), (float)S(16)), &sfLeft, &brMuted);
                drawMiniSlider(GetGameVolBarRect(), editGameVolPct_, editGameMuted_, Gc(16, 185, 129));
                addBtnPx(BTN_EDIT_GAME_MUTE, rpX + S(10), S(124), S(124), S(24), editGameMuted_ ? L"Oyun: Sustu" : L"Oyun Sustur",
                    editGameMuted_ ? Gc(180, 83, 9) : Gc(32, 39, 53),
                    editGameMuted_ ? Gc(217, 119, 6) : Gc(44, 54, 72), Gc(56, 68, 90), canPlay);

                wchar_t mLbl[48] = {};
                if (editHasMultiTrack_) swprintf(mLbl, 48, L"Mikrofon: %d%%", editMicMuted_ ? 0 : editMicVolPct_);
                else swprintf(mLbl, 48, L"Mikrofon (Tek İz)");
                g.DrawString(mLbl, -1, &fSmall, Gdiplus::RectF((float)(rpX + S(10)), (float)S(162), (float)S(124), (float)S(16)), &sfLeft, &brMuted);
                drawMiniSlider(GetMicVolBarRect(), editMicVolPct_, editMicMuted_ || !editHasMultiTrack_, Gc(59, 130, 246));
                addBtnPx(BTN_EDIT_MIC_MUTE, rpX + S(10), S(206), S(124), S(24), editMicMuted_ ? L"Mik: Sustu" : L"Mik Sustur",
                    editMicMuted_ ? Gc(180, 83, 9) : Gc(32, 39, 53),
                    editMicMuted_ ? Gc(217, 119, 6) : Gc(44, 54, 72), Gc(56, 68, 90), canPlay && editHasMultiTrack_);

                wchar_t spdLbl[32] = {};
                swprintf(spdLbl, 32, L"Kayıt Hızı: %.1fx", editSpeed_);
                addBtnPx(BTN_EDIT_SPEED, rpX + S(10), S(240), S(124), S(24), spdLbl, Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), canPlay);
                addBtnPx(BTN_EDIT_ADD_CLIP, rpX + S(10), S(268), S(124), S(24),
                    editInsertClipPath_.empty() ? L"+ Klip Ekle" : L"✕ Eklenen Klip",
                    editInsertClipPath_.empty() ? Gc(32, 39, 53) : Gc(30, 64, 175),
                    Gc(44, 54, 72), Gc(96, 165, 250), canPlay);
                addBtnPx(BTN_EDIT_VERTICAL, rpX + S(10), S(296), S(124), S(24),
                    editVertical916_ ? L"9:16 Dikey: Açık" : L"9:16 Dikey: Kapalı",
                    editVertical916_ ? Gc(124, 58, 237) : Gc(32, 39, 53),
                    editVertical916_ ? Gc(139, 92, 246) : Gc(44, 54, 72), Gc(167, 139, 250), canPlay);

                // Multi-track timeline & control card anchored to bottom
                const int edY = ch - S(278);
                FillRoundRect(g, (float)S(242), (float)edY, (float)(cw - S(264)), (float)S(262), (float)S(12), Gc(20, 25, 35));
                StrokeRoundRect(g, (float)S(242), (float)edY, (float)(cw - S(264)), (float)S(262), (float)S(12), Gc(40, 48, 65));
            } else {
                const int pcY = ch - S(190);
                FillRoundRect(g, (float)S(242), (float)pcY, (float)(cw - S(264)), (float)S(174), (float)S(12), Gc(20, 25, 35));
                StrokeRoundRect(g, (float)S(242), (float)pcY, (float)(cw - S(264)), (float)S(174), (float)S(12), Gc(40, 48, 65));
            }

            // Seekbar track + optional trim/cut highlight & handles
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
                if (editCutActive_ && cutEndSec_ > cutStartSec_) {
                    double cr0 = std::max(sr0, std::min(sr1, cutStartSec_ / dur));
                    double cr1 = std::max(cr0, std::min(sr1, cutEndSec_ / dur));
                    float xcs = static_cast<float>(sk.left) + static_cast<float>(skW * cr0);
                    float xce = static_cast<float>(sk.left) + static_cast<float>(skW * cr1);
                    if (xce - xcs >= 2.0f) {
                        FillRoundRect(g, xcs, trackY - S(2), xce - xcs, trackH + S(4), (float)S(2), Gc(239, 68, 68, 220));
                    }
                    FillRoundRect(g, xcs - S(3), trackY - S(6), (float)S(6), (float)S(18), (float)S(2), Gc(239, 68, 68));
                    FillRoundRect(g, xce - S(3), trackY - S(6), (float)S(6), (float)S(18), (float)S(2), Gc(239, 68, 68));
                }
                FillRoundRect(g, xs - S(4), trackY - S(7), (float)S(8), (float)S(20), (float)S(3), Gc(34, 197, 94));
                FillRoundRect(g, xe - S(4), trackY - S(7), (float)S(8), (float)S(20), (float)S(3), Gc(245, 158, 11));

                auto drawTrackStrip = [&](int ty, const wchar_t* lbl, Gdiplus::Color col, bool active) {
                    g.DrawString(lbl, -1, &fSmall, Gdiplus::RectF((float)S(254), (float)ty, (float)S(82), (float)S(16)), &sfLeft, &brMuted);
                    float tx = (float)S(340), tw = (float)std::max(S(200), cw - S(374));
                    FillRoundRect(g, tx, (float)(ty + S(2)), tw, (float)S(12), (float)S(4), Gc(28, 34, 46));
                    float s0 = tx + tw * static_cast<float>(sr0);
                    float s1 = tx + tw * static_cast<float>(sr1);
                    if (s1 - s0 > 2.0f) {
                        FillRoundRect(g, s0, (float)(ty + S(2)), s1 - s0, (float)S(12), (float)S(4),
                            active ? col : Gc(70, 80, 96, 110));
                    }
                    if (editCutActive_ && cutEndSec_ > cutStartSec_) {
                        float cs0 = tx + tw * static_cast<float>(std::max(sr0, std::min(sr1, cutStartSec_ / dur)));
                        float cs1 = tx + tw * static_cast<float>(std::max(sr0, std::min(sr1, cutEndSec_ / dur)));
                        if (cs1 - cs0 > 2.0f) {
                            FillRoundRect(g, cs0, (float)(ty + S(2)), cs1 - cs0, (float)S(12), (float)S(2), Gc(185, 28, 28, 180));
                        }
                    }
                };
                drawTrackStrip(ch - S(232), L"Oyun / PC", Gc(16, 185, 129, 175), !editGameMuted_ && editGameVolPct_ > 0);
                drawTrackStrip(ch - S(210), L"Mikrofon", Gc(59, 130, 246, 175), editHasMultiTrack_ && !editMicMuted_ && editMicVolPct_ > 0);
            }

            float knobX = static_cast<float>(sk.left) + fillW;
            float knobR = static_cast<float>(draggingSeek_ ? S(8) : S(6));
            Gdiplus::SolidBrush knobGlow(Gc(59, 130, 246, 90));
            g.FillEllipse(&knobGlow, knobX - knobR - S(3), trackY + trackH * 0.5f - knobR - S(3), (knobR + S(3)) * 2.0f, (knobR + S(3)) * 2.0f);
            Gdiplus::SolidBrush knobBr(Gc(248, 250, 252));
            g.FillEllipse(&knobBr, knobX - knobR, trackY + trackH * 0.5f - knobR, knobR * 2.0f, knobR * 2.0f);

            const int timeY = trimMode_ ? (ch - S(184)) : (ch - S(152));
            const int btnY  = trimMode_ ? (ch - S(156)) : (ch - S(126));

            wchar_t timeTxt[64] = {};
            swprintf(timeTxt, 64, L"%ls  /  %ls",
                FormatDurClock(playerPosSec_).c_str(), FormatDurClock(playerDurSec_).c_str());
            g.DrawString(timeTxt, -1, &fBold,
                Gdiplus::RectF((float)S(254), (float)timeY, (float)S(150), (float)S(22)), &sfLeft, &brText);

            if (trimMode_) {
                double spanSec = std::max(0.0, trimEndSec_ - trimStartSec_);
                if (editCutActive_ && cutEndSec_ > cutStartSec_) spanSec = std::max(0.0, spanSec - (cutEndSec_ - cutStartSec_));
                wchar_t trimInfo[128] = {};
                swprintf(trimInfo, 128, L"Kesit: %ls → %ls  (Çıkış: %.1f sn%ls)",
                    FormatDurPrecise(trimStartSec_).c_str(), FormatDurPrecise(trimEndSec_).c_str(),
                    spanSec / editSpeed_, editCutActive_ ? L", orta kesildi" : L"");
                Gdiplus::SolidBrush brMint(Gc(110, 231, 183));
                g.DrawString(trimInfo, -1, &fSmall,
                    Gdiplus::RectF((float)S(410), (float)timeY, (float)std::max(S(240), cw - S(434)), (float)S(22)), &sfLeft, &brMint);
            } else if (!playerReady_ && !trimBusy_) {
                g.DrawString(L"Video hazırlanıyor...", -1, &fSmall,
                    Gdiplus::RectF((float)S(416), (float)timeY, (float)S(200), (float)S(22)), &sfLeft, &brMuted);
            }

            // Transport buttons
            bool canPlay = playerReady_ && !trimBusy_;
            addBtnPx(BTN_PLY_PLAYPAUSE, S(254), btnY, S(98), S(34),
                playerPlaying_ ? L"⏸ Duraklat" : (playerEnded_ ? L"↻ Tekrar" : L"▶ Oynat"),
                Gc(37, 99, 235), Gc(59, 130, 246), Gc(96, 165, 250, 150), canPlay);
            addBtnPx(BTN_PLY_MINUS5, S(358), btnY, S(52), S(34), L"-5 sn", Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), canPlay);
            addBtnPx(BTN_PLY_PLUS5, S(416), btnY, S(52), S(34), L"+5 sn", Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), canPlay);
            addBtnPx(BTN_PLY_RESTART, S(474), btnY, S(54), S(34), L"Başa", Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), canPlay);

            wchar_t rateStr[16] = {};
            swprintf(rateStr, 16, L"%.1fx", playerRate_);
            addBtnPx(BTN_PLY_RATE, S(534), btnY, S(46), S(34), rateStr, Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), canPlay);

            addBtnPx(BTN_PLY_TRIM, S(586), btnY, S(96), S(34), L"✂ Düzenle",
                trimMode_ ? Gc(13, 148, 136) : Gc(32, 39, 53),
                trimMode_ ? Gc(20, 184, 166) : Gc(44, 54, 72),
                trimMode_ ? Gc(94, 234, 212) : Gc(56, 68, 90), canPlay);

            addBtnPx(BTN_PLY_FULLSCREEN, cw - S(232), btnY, S(32), S(34), L"⛶", Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), canPlay);

            addBtnPx(BTN_PLY_MUTE, cw - S(194), btnY, S(48), S(34), playerMuted_ ? L"Sessiz" : L"Ses",
                playerMuted_ ? Gc(180, 83, 9) : Gc(32, 39, 53),
                playerMuted_ ? Gc(217, 119, 6) : Gc(44, 54, 72), Gc(56, 68, 90), canPlay);

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
                const int row1Y = ch - S(112);
                const int row2Y = ch - S(76);
                bool validSpan = canPlay && (trimEndSec_ - trimStartSec_ >= 0.2);
                addBtnPx(BTN_TRIM_SET_START, S(254), row1Y, S(92), S(30), L"[ Başlangıç", Gc(20, 83, 45), Gc(22, 101, 52), Gc(74, 222, 128), canPlay);
                addBtnPx(BTN_TRIM_SET_END, S(352), row1Y, S(76), S(30), L"Bitiş ]", Gc(120, 53, 15), Gc(146, 64, 14), Gc(251, 191, 36), canPlay);
                addBtnPx(BTN_EDIT_CUT_TOGGLE, S(434), row1Y, S(104), S(30),
                    editCutActive_ ? L"✕ Orta Kesim" : L"✂ Ortadan Kes",
                    editCutActive_ ? Gc(153, 27, 27) : Gc(32, 39, 53),
                    editCutActive_ ? Gc(185, 28, 28) : Gc(44, 54, 72), Gc(239, 68, 68), canPlay);
                addBtnPx(BTN_TRIM_SAVE_COPY, S(544), row1Y, S(142), S(30), L"Yeni Kopya Kaydet", Gc(37, 99, 235), Gc(59, 130, 246), Gc(96, 165, 250), validSpan);
                addBtnPx(BTN_TRIM_REPLACE, S(692), row1Y, S(118), S(30), L"Ana Klip Yap", Gc(13, 148, 136), Gc(20, 184, 166), Gc(94, 234, 212), validSpan);
                addBtnPx(BTN_TRIM_CANCEL, S(816), row1Y, S(70), S(30), L"İptal", Gc(32, 39, 53), Gc(44, 54, 72), Gc(56, 68, 90), !trimBusy_);
                addBtnPx(BTN_EDIT_DISCORD10, S(254), row2Y, S(126), S(28), L"Discord ≤10 MB", Gc(88, 101, 242), Gc(104, 117, 248), Gc(165, 180, 252), validSpan);
                addBtnPx(BTN_EDIT_DISCORD25, S(386), row2Y, S(126), S(28), L"Discord ≤25 MB", Gc(67, 56, 202), Gc(79, 70, 229), Gc(165, 180, 252), validSpan);
                g.DrawString(trimBusy_
                    ? footerMsg_.c_str()
                    : L"İpucu: Seçili kesiti veya tüm klibi tek tıkla 10/25 MB Discord ya da 9:16 Dikey çıkarın.",
                    -1, &fSmall, Gdiplus::RectF((float)S(520), (float)row2Y, (float)std::max(S(240), cw - S(546)), (float)S(48)), &sfLeft, &brMuted);
            } else {
                addBtnPx(BTN_PLY_EXTERNAL, cw - S(178), ch - S(54), S(144), S(30), L"Dış Oynatıcıda Aç", Gc(26, 32, 44), Gc(38, 46, 64), Gc(48, 58, 78));
                g.DrawString(footerMsg_.c_str(), -1, &fSmall,
                    Gdiplus::RectF((float)S(254), (float)(ch - S(50)), (float)std::max(S(240), cw - S(444)), (float)S(22)), &sfLeft, &brMuted);
            }
        } else if (activeTab_ == 0) {
            // ================= TAB 0: KLİPLERİM (GALLERY + FAVORITES + SEARCH) =================
            g.DrawString(L"Kayıtlı Klipler", -1, &fTitle,
                Gdiplus::RectF((float)S(242), (float)S(12), (float)S(160), (float)S(28)), &sfLeft, &brText);
            {
                wchar_t totBuf[32] = {};
                swprintf(totBuf, 32, L"%zu Klip", clips_.size());
                FillRoundRect(g, (float)S(404), (float)S(16), (float)S(74), (float)S(22), (float)S(11), Gc(28, 36, 54));
                StrokeRoundRect(g, (float)S(404), (float)S(16), (float)S(74), (float)S(22), (float)S(11), Gc(54, 70, 102));
                Gdiplus::SolidBrush brBadge(Gc(148, 163, 184));
                g.DrawString(totBuf, -1, &fSmall,
                    Gdiplus::RectF((float)S(404), (float)S(16), (float)S(74), (float)S(22)), &sfCenter, &brBadge);
            }

            addBtn(BTN_TOP_FAV_FILTER, 572, 14, 130, 30, L"★  Favoriler",
                favOnly_ ? Gc(120, 83, 14) : Gc(28, 34, 47),
                favOnly_ ? Gc(146, 100, 18) : Gc(40, 49, 68),
                favOnly_ ? Gc(250, 204, 21) : Gc(52, 64, 86));
            addBtn(BTN_TOP_FOLDER, 710, 14, 98, 30, L"Klasörü Aç", Gc(28, 34, 47), Gc(40, 49, 68), Gc(52, 64, 86));
            addBtn(BTN_TOP_REFRESH, 816, 14, 82, 30, L"Yenile", Gc(28, 34, 47), Gc(40, 49, 68), Gc(52, 64, 86));

            {
                int sw = std::max(S(240), cw - S(646));
                FillRoundRect(g, (float)S(242), (float)S(48), (float)sw, (float)S(24), (float)S(7), Gc(24, 30, 44));
                StrokeRoundRect(g, (float)S(242), (float)S(48), (float)sw, (float)S(24), (float)S(7), Gc(54, 70, 102));
            }
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
                std::vector<RECT> cardRects;
                int favHy = -1, normHy = -1, totalH = 0, favCnt = 0, normCnt = 0;
                ComputeGalleryLayout(cardRects, favHy, normHy, totalH, favCnt, normCnt);

                auto drawSectionHeader = [&](int hy, const wchar_t* title, int count, Gdiplus::Color accent, bool withTopDivider) {
                    int y = hy - scrollY_;
                    if (y + S(28) < vp.top || y > vp.bottom) return;
                    if (withTopDivider) {
                        Gdiplus::Pen divPen(Gc(38, 48, 68), 1.0f);
                        g.DrawLine(&divPen, vp.left, y - S(10), vp.right - S(16), y - S(10));
                    }
                    FillRoundRect(g, (float)vp.left, (float)(y + S(4)), (float)S(3), (float)S(16), 1.5f, accent);
                    Gdiplus::SolidBrush hdrBr(accent);
                    g.DrawString(title, -1, &fBold,
                        Gdiplus::RectF((float)(vp.left + S(10)), (float)y, (float)S(160), (float)S(24)), &sfLeft, &hdrBr);

                    wchar_t cntBuf[16] = {};
                    swprintf(cntBuf, 16, L"%d", count);
                    FillRoundRect(g, (float)(vp.left + S(148)), (float)(y + S(3)), (float)S(32), (float)S(18), (float)S(9),
                        Gc(accent.GetR(), accent.GetG(), accent.GetB(), 38));
                    g.DrawString(cntBuf, -1, &fSmall,
                        Gdiplus::RectF((float)(vp.left + S(148)), (float)(y + S(3)), (float)S(32), (float)S(18)), &sfCenter, &hdrBr);
                };

                if (favHy >= 0) {
                    drawSectionHeader(favHy, L"★  Favoriler", favCnt, Gc(250, 204, 21), false);
                }
                if (normHy >= 0) {
                    drawSectionHeader(normHy, L"Normal Klipler", normCnt, Gc(147, 197, 253), favCnt > 0);
                }

                for (size_t vi = 0; vi < cardRects.size(); ++vi) {
                    int cx = cardRects[vi].left;
                    int cy = cardRects[vi].top - scrollY_;
                    int cardW = cardRects[vi].right - cardRects[vi].left;
                    int cardH = cardRects[vi].bottom - cardRects[vi].top;
                    if (cy + cardH < vp.top || cy > vp.bottom) continue;

                    bool sel = (static_cast<int>(vi) == selectedIdx_);
                    bool hov = (static_cast<int>(vi) == hoverCardIdx_);
                    const auto& c = clips_[filteredIndices_[vi]];

                    if (sel || hov) {
                        FillRoundRect(g, (float)(cx - S(2)), (float)(cy - S(2)), (float)(cardW + S(4)), (float)(cardH + S(4)),
                            (float)S(14), sel ? Gc(56, 189, 248, 45) : Gc(59, 130, 246, 30));
                    }
                    FillRoundRectGrad(g, (float)cx, (float)cy, (float)cardW, (float)cardH, (float)S(12),
                        sel ? Gc(32, 46, 78) : (hov ? Gc(31, 40, 58) : Gc(22, 28, 41)),
                        sel ? Gc(22, 32, 56) : (hov ? Gc(23, 30, 44) : Gc(17, 22, 32)));
                    StrokeRoundRect(g, (float)cx, (float)cy, (float)cardW, (float)cardH, (float)S(12),
                        sel ? Gc(56, 189, 248) : (hov ? Gc(96, 165, 250) : Gc(44, 54, 76)),
                        sel ? 2.0f : (hov ? 1.5f : 1.0f));

                    int tx = cx + S(7), ty = cy + S(7), tw = std::max(S(160), cardW - S(14)), th = (tw * 9) / 16;
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

                    // Bottom gradient scrim on thumbnail so badges & duration pop cleanly
                    {
                        float gradH = static_cast<float>(S(42));
                        float gradY = static_cast<float>(ty + th) - gradH;
                        Gdiplus::LinearGradientBrush scrim(
                            Gdiplus::PointF((float)tx, gradY), Gdiplus::PointF((float)tx, (float)(ty + th)),
                            Gc(8, 11, 17, 0), Gc(8, 11, 17, 215));
                        g.FillRectangle(&scrim, (float)tx, gradY, (float)tw, gradH);
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
                    FillRoundRect(g, (float)(tx + tw - S(52)), (float)(ty + th - S(26)), (float)S(44), (float)S(19), (float)S(5), Gc(10, 12, 18, 220));
                    g.DrawString(durLbl.c_str(), -1, &fSmall,
                        Gdiplus::RectF((float)(tx + tw - S(52)), (float)(ty + th - S(26)), (float)S(44), (float)S(19)), &sfCenter, &brText);

                    std::wstring gTag = ExtractGameTagFromName(c.fileName);
                    int badgeX = tx + S(8);
                    if (!gTag.empty()) {
                        Gdiplus::Color tagTop = Gc(79, 70, 229, 230), tagBot = Gc(55, 48, 163, 230);
                        if (_wcsicmp(gTag.c_str(), L"Valorant") == 0) {
                            tagTop = Gc(244, 63, 94, 235); tagBot = Gc(190, 18, 60, 235);
                        } else if (_wcsicmp(gTag.c_str(), L"LeagueOfLegends") == 0) {
                            tagTop = Gc(14, 165, 233, 235); tagBot = Gc(3, 105, 161, 235);
                        } else if (_wcsicmp(gTag.c_str(), L"CS2") == 0) {
                            tagTop = Gc(245, 158, 11, 235); tagBot = Gc(180, 83, 9, 235);
                        } else if (_wcsicmp(gTag.c_str(), L"osu") == 0) {
                            tagTop = Gc(236, 72, 153, 235); tagBot = Gc(190, 24, 93, 235);
                        }
                        int tagW = std::min(S(124), S(18) + static_cast<int>(gTag.size()) * S(6));
                        FillRoundRectGrad(g, (float)badgeX, (float)(ty + S(8)), (float)tagW, (float)S(20), (float)S(6), tagTop, tagBot);
                        g.DrawString(gTag.c_str(), -1, &fSmall,
                            Gdiplus::RectF((float)badgeX, (float)(ty + S(8)), (float)tagW, (float)S(20)), &sfCenter, &brText);
                        badgeX += tagW + S(6);
                    }
                    if (c.height > 0) {
                        wchar_t rb[16] = {};
                        swprintf(rb, 16, L"%up", c.height);
                        FillRoundRect(g, (float)badgeX, (float)(ty + S(8)), (float)S(42), (float)S(20), (float)S(6), Gc(10, 12, 18, 205));
                        g.DrawString(rb, -1, &fSmall,
                            Gdiplus::RectF((float)badgeX, (float)(ty + S(8)), (float)S(42), (float)S(20)), &sfCenter, &brText);
                    }

                    std::wstring title = StripMp4Ext(c.fileName);
                    g.DrawString(title.c_str(), -1, &fBold,
                        Gdiplus::RectF((float)(cx + S(10)), (float)(ty + th + S(6)), (float)(cardW - S(20)), (float)S(20)),
                        &sfLeft, &brText);
                    std::wstring meta = c.displayDate + L"  •  " + FormatMb(c.fileSizeBytes);
                    g.DrawString(meta.c_str(), -1, &fSmall,
                        Gdiplus::RectF((float)(cx + S(10)), (float)(ty + th + S(26)), (float)(cardW - S(20)), (float)S(18)),
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

            // Bottom action dock anchored to bottom of window
            bool hasSel = (selectedIdx_ >= 0 && selectedIdx_ < static_cast<int>(filteredIndices_.size()));
            bool selFav = hasSel && clips_[filteredIndices_[static_cast<size_t>(selectedIdx_)]].isFavorite;
            const int dockY = ch - S(76);
            FillRoundRect(g, (float)S(242), (float)dockY, (float)(cw - S(264)), (float)S(56), (float)S(12), Gc(16, 21, 33));
            StrokeRoundRect(g, (float)S(242), (float)dockY, (float)(cw - S(264)), (float)S(56), (float)S(12), Gc(38, 48, 70));

            addBtnPx(BTN_CLIP_PLAY, S(254), dockY + S(11), S(82), S(34), L"▶  İzle",
                Gc(37, 99, 235), Gc(59, 130, 246), Gc(96, 165, 250, 130), hasSel);
            addBtnPx(BTN_CLIP_FAV, S(342), dockY + S(11), S(78), S(34), selFav ? L"★ Fav" : L"☆ Fav",
                selFav ? Gc(68, 52, 16) : Gc(26, 33, 49),
                selFav ? Gc(92, 68, 18) : Gc(36, 46, 68),
                selFav ? Gc(234, 179, 8, 140) : Gc(46, 58, 84), hasSel);
            addBtnPx(BTN_CLIP_SHARE, S(426), dockY + S(11), S(82), S(34), uploadBusy_ ? L"Yükleniyor" : L"Link Al",
                Gc(26, 33, 49), Gc(36, 46, 68), Gc(46, 58, 84), hasSel && !uploadBusy_);
            addBtnPx(BTN_CLIP_RENAME, S(514), dockY + S(11), S(78), S(34), L"Adlandır",
                Gc(26, 33, 49), Gc(36, 46, 68), Gc(46, 58, 84), hasSel);
            addBtnPx(BTN_CLIP_REVEAL, S(598), dockY + S(11), S(74), S(34), L"Klasör",
                Gc(26, 33, 49), Gc(36, 46, 68), Gc(46, 58, 84), hasSel);
            addBtnPx(BTN_CLIP_DELETE, S(678), dockY + S(11), S(48), S(34), L"Sil",
                Gc(42, 22, 28), Gc(127, 29, 29), Gc(185, 28, 28, 110), hasSel);

            std::wstring selUrl = hasSel ? clips_[filteredIndices_[static_cast<size_t>(selectedIdx_)]].shareUrl : L"";
            if (hasSel && !selUrl.empty() && !uploadBusy_) {
                int boxX = S(736);
                int boxW = std::max(S(60), cw - S(112) - boxX);
                RECT linkRc{ boxX, dockY + S(11), boxX + boxW, dockY + S(45) };
                customButtons_.push_back({ BTN_CLIP_OPEN_LINK, linkRc, true });
                bool linkHov = (hoverBtnId_ == BTN_CLIP_OPEN_LINK);
                FillRoundRect(g, (float)boxX, (float)(dockY + S(11)), (float)boxW, (float)S(34), (float)S(8),
                    linkHov ? Gc(22, 32, 52) : Gc(13, 18, 30));
                StrokeRoundRect(g, (float)boxX, (float)(dockY + S(11)), (float)boxW, (float)S(34), (float)S(8),
                    linkHov ? Gc(96, 165, 250, 170) : Gc(44, 58, 84));
                Gdiplus::SolidBrush brLink(linkHov ? Gc(186, 230, 253) : Gc(125, 211, 252));
                g.DrawString(selUrl.c_str(), -1, &fSmall,
                    Gdiplus::RectF((float)(boxX + S(10)), (float)(dockY + S(11)), (float)std::max(S(20), boxW - S(16)), (float)S(34)),
                    &sfLeft, &brLink);
                bool copiedNow = (GetTickCount64() < copyFeedbackUntilMs_);
                addBtnPx(BTN_CLIP_COPY_LINK, cw - S(106), dockY + S(11), S(78), S(34),
                    copiedNow ? L"✓ Kopya" : L"Kopyala",
                    copiedNow ? Gc(22, 101, 52) : Gc(28, 40, 62),
                    copiedNow ? Gc(21, 128, 61) : Gc(37, 99, 235),
                    copiedNow ? Gc(74, 222, 128, 160) : Gc(59, 130, 246, 110), true);
            } else {
                g.DrawString(footerMsg_.c_str(), -1, &fSmall,
                    Gdiplus::RectF((float)S(736), (float)(dockY + S(8)), (float)std::max(S(140), cw - S(762)), (float)S(40)), &sfLeft, &brMuted);
            }
            g.ResetTransform();
            g.ResetClip();
        } else {
            // ================= TAB 1: AYARLAR =================
            const int sox = settingsOffsetX_;

            g.DrawString(L"Ayarlar", -1, &fTitle,
                Gdiplus::RectF((float)(S(242) + sox), (float)S(14), (float)S(220), (float)S(28)), &sfLeft, &brText);
            g.DrawString(L"Video, ses ve kısayol değişiklikleri anında uygulanır.", -1, &fSmall,
                Gdiplus::RectF((float)(S(242) + sox), (float)S(42), (float)S(460), (float)S(18)), &sfLeft, &brMuted);

            // Card 1: Video & Buffer (Blue glow accent)
            FillRoundRect(g, (float)(S(242) + sox), (float)S(68), (float)S(320), (float)S(180), (float)S(12), Gc(16, 21, 33));
            FillRoundRectGrad(g, (float)(S(242) + sox), (float)S(68), (float)S(320), (float)S(32), (float)S(12), Gc(37, 99, 235, 42), Gc(16, 21, 33, 0));
            StrokeRoundRect(g, (float)(S(242) + sox), (float)S(68), (float)S(320), (float)S(180), (float)S(12), Gc(59, 130, 246, 110));
            FillRoundRect(g, (float)(S(248) + sox), (float)S(78), (float)S(3), (float)S(16), 1.5f, Gc(56, 189, 248));
            g.DrawString(L"Video ve Tampon", -1, &fBold,
                Gdiplus::RectF((float)(S(258) + sox), (float)S(76), (float)S(220), (float)S(22)), &sfLeft, &brBlueTxt);
            g.DrawString(L"Hızlı Seçim:", -1, &fSmall,
                Gdiplus::RectF((float)(S(258) + sox), (float)S(211), (float)S(84), (float)S(22)), &sfLeft, &brMuted);
            {
                bool is1080 = (app_.Config().height >= 1080);
                addBtnPx(BTN_SET_720P, S(344) + sox, S(208), S(96), S(28), L"720p HD",
                    !is1080 ? Gc(37, 99, 235) : Gc(24, 31, 48),
                    !is1080 ? Gc(59, 130, 246) : Gc(36, 46, 68),
                    !is1080 ? Gc(96, 165, 250, 180) : Gc(48, 60, 84));
                addBtnPx(BTN_SET_1080P, S(446) + sox, S(208), S(104), S(28), L"1080p Full HD",
                    is1080 ? Gc(37, 99, 235) : Gc(24, 31, 48),
                    is1080 ? Gc(59, 130, 246) : Gc(36, 46, 68),
                    is1080 ? Gc(96, 165, 250, 180) : Gc(48, 60, 84));
            }

            auto drawDarkDropdown = [&](int btnId, int px, int py, int pw, int ph, HWND cmb) {
                RECT rc{ px, py, px + pw, py + ph };
                customButtons_.push_back({ btnId, rc, true });
                bool hov = (hoverBtnId_ == btnId);
                bool prs = (pressedBtnId_ == btnId);
                FillRoundRectGrad(g, (float)rc.left, (float)rc.top, (float)pw, (float)ph, (float)S(7),
                    prs ? Gc(32, 42, 66) : (hov ? Gc(28, 37, 58) : Gc(24, 31, 48)),
                    Gc(19, 25, 40));
                StrokeRoundRect(g, (float)rc.left, (float)rc.top, (float)pw, (float)ph, (float)S(7),
                    hov ? Gc(96, 165, 250) : Gc(54, 70, 102));
                std::wstring txt = L"Seçiniz";
                if (cmb) {
                    int idx = static_cast<int>(SendMessageW(cmb, CB_GETCURSEL, 0, 0));
                    if (idx >= 0) {
                        LRESULT len = SendMessageW(cmb, CB_GETLBTEXTLEN, idx, 0);
                        if (len > 0 && len < 256) {
                            std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
                            SendMessageW(cmb, CB_GETLBTEXT, idx, reinterpret_cast<LPARAM>(&buf[0]));
                            txt = buf.c_str();
                        }
                    }
                }
                g.DrawString(txt.c_str(), -1, &fSmall,
                    Gdiplus::RectF((float)(rc.left + S(10)), (float)rc.top, (float)(pw - S(28)), (float)ph),
                    &sfLeft, &brText);
                g.DrawString(L"▾", -1, &fSmall,
                    Gdiplus::RectF((float)(rc.right - S(22)), (float)rc.top, (float)S(18), (float)ph),
                    &sfCenter, &brMuted);
            };
            drawDarkDropdown(BTN_CMB_DURATION, S(344) + sox, S(101), S(206), S(24), cmbDuration_);
            drawDarkDropdown(BTN_CMB_RESFPS, S(344) + sox, S(136), S(206), S(24), cmbResFps_);
            drawDarkDropdown(BTN_CMB_BITRATE, S(344) + sox, S(171), S(206), S(24), cmbBitrate_);

            // Card 2: Audio (Emerald glow accent)
            FillRoundRect(g, (float)(S(578) + sox), (float)S(68), (float)S(320), (float)S(180), (float)S(12), Gc(16, 21, 33));
            FillRoundRectGrad(g, (float)(S(578) + sox), (float)S(68), (float)S(320), (float)S(32), (float)S(12), Gc(34, 197, 94, 38), Gc(16, 21, 33, 0));
            StrokeRoundRect(g, (float)(S(578) + sox), (float)S(68), (float)S(320), (float)S(180), (float)S(12), Gc(34, 197, 94, 105));
            FillRoundRect(g, (float)(S(584) + sox), (float)S(76), (float)S(3), (float)S(16), 1.5f, Gc(52, 211, 153));
            Gdiplus::SolidBrush brGreenTxt(Gc(134, 239, 172));
            g.DrawString(L"Ses (Oyun + Mikrofon)", -1, &fBold,
                Gdiplus::RectF((float)(S(594) + sox), (float)S(74), (float)S(240), (float)S(22)), &sfLeft, &brGreenTxt);
            drawDarkDropdown(BTN_CMB_MIC_DEV, S(642) + sox, S(160), S(240), S(24), cmbMicDev_);

            {
                float db = static_cast<float>(st.micLevelDb);
                float norm = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));
                int thrDb = trkMicGate_ ? static_cast<int>(SendMessageW(trkMicGate_, TBM_GETPOS, 0, 0)) : app_.Config().micGateThresholdDb;
                float thrNorm = std::max(0.0f, std::min(1.0f, (static_cast<float>(thrDb) + 60.0f) / 60.0f));
                float barX = static_cast<float>(S(594) + sox), barY = static_cast<float>(S(227));
                float barW = static_cast<float>(S(212)), barH = static_cast<float>(S(8));
                FillRoundRect(g, barX, barY, barW, barH, (float)S(4), Gc(26, 33, 48));
                if (norm > 0.01f) {
                    bool openNow = (db >= static_cast<float>(thrDb));
                    FillRoundRectGrad(g, barX, barY, barW * norm, barH, (float)S(4),
                        openNow ? Gc(52, 211, 153) : Gc(251, 191, 36),
                        openNow ? Gc(16, 185, 129) : Gc(217, 119, 6));
                }
                float markerX = barX + barW * thrNorm;
                FillRoundRect(g, markerX - 1.5f, barY - S(2), 3.0f, barH + S(4), 1.0f, Gc(248, 250, 252));

                wchar_t dbTxt[32] = {};
                swprintf(dbTxt, 32, L"Mik: %.0f dB", db);
                g.DrawString(dbTxt, -1, &fSmall,
                    Gdiplus::RectF((float)(S(812) + sox), (float)S(221), (float)S(76), (float)S(20)), &sfLeft, &brMuted);
            }

            // Card 3: Hotkeys, Notifications & Mobile Sync (Violet glow accent)
            FillRoundRect(g, (float)(S(242) + sox), (float)S(258), (float)S(656), (float)S(270), (float)S(12), Gc(16, 21, 33));
            FillRoundRectGrad(g, (float)(S(242) + sox), (float)S(258), (float)S(656), (float)S(34), (float)S(12), Gc(168, 85, 247, 38), Gc(16, 21, 33, 0));
            StrokeRoundRect(g, (float)(S(242) + sox), (float)S(258), (float)S(656), (float)S(270), (float)S(12), Gc(168, 85, 247, 105));
            FillRoundRect(g, (float)(S(248) + sox), (float)S(266), (float)S(3), (float)S(16), 1.5f, Gc(192, 132, 252));
            Gdiplus::SolidBrush brPurpTxt(Gc(216, 180, 254));
            g.DrawString(L"Oyun İçi Kısayol, Bildirim ve Klasör", -1, &fBold,
                Gdiplus::RectF((float)(S(258) + sox), (float)S(264), (float)S(340), (float)S(22)), &sfLeft, &brPurpTxt);

            auto drawInputPill = [&](int ix, int iy, int iw, int ih) {
                FillRoundRect(g, (float)ix, (float)iy, (float)iw, (float)ih, (float)S(7), Gc(24, 30, 44));
                StrokeRoundRect(g, (float)ix, (float)iy, (float)iw, (float)ih, (float)S(7), Gc(54, 70, 102));
            };
            drawInputPill(S(428) + sox, S(291), S(196), S(24));
            drawInputPill(S(428) + sox, S(325), S(196), S(24));
            drawInputPill(S(358) + sox, S(393), S(366), S(24));

            {
                const AppConfig& mc = app_.Config();
                wchar_t mobTxt[160] = {};
                if (app_.IsMobileSyncRunning()) {
                    swprintf(mobTxt, 160, L"Wi-Fi Aktif: %ls:%d  •  PIN: %ls",
                        app_.GetMobileLanIp().c_str(), mc.mobileSyncPort, mc.mobilePin.c_str());
                    Gdiplus::SolidBrush brMobOk(Gc(134, 239, 172));
                    g.DrawString(mobTxt, -1, &fSmall,
                        Gdiplus::RectF((float)(S(560) + sox), (float)S(468), (float)S(326), (float)S(20)), &sfLeft, &brMobOk);
                } else {
                    std::wstring merr = app_.GetMobileError();
                    const wchar_t* msg = !merr.empty() ? merr.c_str() : L"Telefon aktarımı kapalı (0 RAM)";
                    g.DrawString(msg, -1, &fSmall,
                        Gdiplus::RectF((float)(S(560) + sox), (float)S(468), (float)S(326), (float)S(20)), &sfLeft, &brMuted);
                }
            }

            g.DrawString(footerMsg_.c_str(), -1, &fSmall,
                Gdiplus::RectF((float)(S(532) + sox), (float)S(497), (float)S(354), (float)S(20)), &sfLeft, &brMuted);
            g.ResetTransform();
            g.ResetClip();
        }
    }

    BitBlt(targetDc, 0, 0, cw, ch, memDc, 0, 0, SRCCOPY);
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

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            if (mmi && hwnd_) {
                DWORD st = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
                RECT mr{ 0, 0, S(920), S(620) };
                AdjustWindowRectEx(&mr, st, FALSE, 0);
                mmi->ptMinTrackSize.x = mr.right - mr.left;
                mmi->ptMinTrackSize.y = mr.bottom - mr.top;
            }
            return 0;
        }

        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                UpdateLayoutForSize(LOWORD(lParam), HIWORD(lParam));
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;

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
                RECT mrc{ S(590) + settingsOffsetX_, S(220), S(892) + settingsOffsetX_, S(245) };
                InvalidateRect(hwnd_, &mrc, FALSE);
            } else if (wParam == TIMER_UI_ANIM_ID) {
                ULONGLONG now = GetTickCount64();
                bool fastTick = false;

                if (openAnimT_ < 1.0f) {
                    float raw = std::min(1.0f, static_cast<float>(now - openAnimStartMs_) / 280.0f);
                    openAnimT_ = raw;
                    float ease = 1.0f - std::pow(1.0f - raw, 3.0f);
                    openAnimAlpha_ = static_cast<int>(std::round(ease * 255.0f));
                    int curY = openTargetY_ + static_cast<int>(std::round((1.0f - ease) * static_cast<float>(S(18))));
                    if (hwnd_) {
                        SetWindowPos(hwnd_, nullptr, openTargetX_, curY, 0, 0,
                            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                        SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(openAnimAlpha_), LWA_ALPHA);
                    }
                    if (raw >= 1.0f && hwnd_) {
                        LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
                        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
                    }
                    fastTick = true;
                }

                if (tabAnimT_ < 1.0f) {
                    float rawTab = std::min(1.0f, static_cast<float>(now - tabAnimStartMs_) / 140.0f);
                    tabAnimT_ = rawTab;
                    if (rawTab >= 1.0f) {
                        tabIndicatorY_ = (activeTab_ == 1) ? 160.0f : 118.0f;
                    }
                    if (hwnd_) {
                        RECT sideTabsRc{ S(14), S(114), S(208), S(200) };
                        InvalidateRect(hwnd_, &sideTabsRc, FALSE);
                    }
                    fastTick = true;
                }

                if (!fastTick) {
                    if (hwnd_) SetTimer(hwnd_, TIMER_UI_ANIM_ID, 48, nullptr);
                    animPhase_ += 0.16;
                    if (hwnd_ && !playerFullscreen_) {
                        RECT sidePulseRc{ S(14), S(68), S(208), S(104) };
                        InvalidateRect(hwnd_, &sidePulseRc, FALSE);
                    }
                }
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

        case WM_MW_UPLOAD_PROGRESS: {
            wchar_t pbuf[64] = {};
            swprintf(pbuf, 64, L"Link oluşturuluyor... %%%d", static_cast<int>(wParam));
            SetFooterMessage(pbuf);
            return 0;
        }

        case WM_MW_UPLOAD_DONE: {
            auto* res = reinterpret_cast<ClipUploadResult*>(lParam);
            if (uploadThread_.joinable()) uploadThread_.join();
            uploadBusy_ = false;
            if (res) {
                if (res->ok && !res->url.empty()) {
                    for (auto& cc : clips_) {
                        if (_wcsicmp(cc.filePath.c_str(), uploadFilePath_.c_str()) == 0) {
                            cc.shareUrl = res->url;
                            SetClipShareLink(GetClipLinksFilePath(), cc.fileName, res->url);
                            break;
                        }
                    }
                    if (OpenClipboard(hwnd_)) {
                        EmptyClipboard();
                        size_t bytes = (res->url.size() + 1) * sizeof(wchar_t);
                        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
                        if (hg) {
                            void* p = GlobalLock(hg);
                            if (p) {
                                memcpy(p, res->url.c_str(), bytes);
                                GlobalUnlock(hg);
                                SetClipboardData(CF_UNICODETEXT, hg);
                            } else {
                                GlobalFree(hg);
                            }
                        }
                        CloseClipboard();
                    }
                    copyFeedbackUntilMs_ = GetTickCount64() + 2400;
                    SetFooterMessage(L"✓ Link panoya kopyalandı: " + res->url);
                } else {
                    SetFooterMessage(res->error.empty() ? L"Link oluşturulamadı." : res->error);
                    MessageBoxW(hwnd_, res->error.empty() ? L"Klip yüklenemedi." : res->error.c_str(),
                        L"Link Hatası", MB_OK | MB_ICONWARNING);
                }
                delete res;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
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
                        if (hwnd_) {
                            RECT cli{}; GetClientRect(hwnd_, &cli);
                            RECT ctrlRc = playerFullscreen_
                                ? RECT{ 0, std::max<LONG>(0, cli.bottom - S(72)), cli.right, cli.bottom }
                                : RECT{ S(236), std::max<LONG>(0, cli.bottom - S(276)), cli.right, cli.bottom };
                            InvalidateRect(hwnd_, &ctrlRc, FALSE);
                        }
                    } else if (ev->type == MFP_EVENT_TYPE_PLAY) {
                        playerPlaying_ = true;
                        playerEnded_ = false;
                        player_->SetVolume(playerVol_);
                        player_->SetMute(playerMuted_ ? TRUE : FALSE);
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
        case WM_CTLCOLORBTN: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(brCard_);
        }
        case WM_DRAWITEM: {
            const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (!dis) break;
            HDC hdc = dis->hDC;
            RECT rc = dis->rcItem;
            if (dis->CtlType == ODT_COMBOBOX) {
                bool sel = (dis->itemState & ODS_SELECTED) != 0;
                bool isEdit = (dis->itemState & ODS_COMBOBOXEDIT) != 0;
                COLORREF bgCol = isEdit ? kRgbInput : (sel ? RGB(37, 99, 235) : RGB(20, 26, 40));
                HBRUSH br = CreateSolidBrush(bgCol);
                FillRect(hdc, &rc, br);
                DeleteObject(br);
                if (isEdit) {
                    HBRUSH brd = CreateSolidBrush(RGB(58, 76, 108));
                    FrameRect(hdc, &rc, brd);
                    DeleteObject(brd);
                }
                if (dis->itemID != static_cast<UINT>(-1)) {
                    LRESULT len = SendMessageW(dis->hwndItem, CB_GETLBTEXTLEN, dis->itemID, 0);
                    if (len > 0 && len < 512) {
                        std::wstring txt(static_cast<size_t>(len) + 1, L'\0');
                        SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, reinterpret_cast<LPARAM>(&txt[0]));
                        txt.resize(wcslen(txt.c_str()));
                        SetBkMode(hdc, TRANSPARENT);
                        SetTextColor(hdc, kRgbText);
                        HFONT oldF = static_cast<HFONT>(SelectObject(hdc, font_));
                        RECT tr = rc;
                        tr.left += S(8);
                        tr.right -= S(isEdit ? 22 : 6);
                        DrawTextW(hdc, txt.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        SelectObject(hdc, oldF);
                    }
                }
                if (isEdit) {
                    SetBkMode(hdc, TRANSPARENT);
                    SetTextColor(hdc, kRgbMuted);
                    RECT ar = rc;
                    ar.left = ar.right - S(20);
                    DrawTextW(hdc, L"▾", -1, &ar, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                return TRUE;
            }
            bool disabled = (dis->itemState & ODS_DISABLED) != 0;
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            bool isMainAction = (dis->CtlID == ID_BTN_APPLY || dis->CtlID == ID_BTN_EXIT_APP);

            Gdiplus::Graphics bgG(hdc);
            bgG.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            bgG.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            Gdiplus::Color parentBg = isMainAction ? Gc(11, 15, 25) : Gc(16, 21, 33);
            Gdiplus::SolidBrush pbr(parentBg);
            bgG.FillRectangle(&pbr, 0, 0, rc.right - rc.left, rc.bottom - rc.top);

            Gdiplus::Color topCol = Gc(34, 44, 66), botCol = Gc(24, 31, 48), brdCol = Gc(64, 82, 118);
            if (disabled) {
                topCol = Gc(22, 28, 40);
                botCol = Gc(19, 24, 35);
                brdCol = Gc(44, 54, 74);
            } else if (dis->CtlID == ID_BTN_APPLY) {
                topCol = pressed ? Gc(30, 64, 175) : Gc(59, 130, 246);
                botCol = pressed ? Gc(29, 78, 216) : Gc(37, 99, 235);
                brdCol = Gc(147, 197, 253, 180);
            } else if (dis->CtlID == ID_BTN_EXIT_APP) {
                topCol = pressed ? Gc(127, 29, 29) : Gc(185, 28, 28);
                botCol = pressed ? Gc(100, 22, 22) : Gc(139, 24, 24);
                brdCol = Gc(248, 113, 113, 150);
            } else if (dis->CtlID == ID_BTN_MOBILE_QR) {
                topCol = pressed ? Gc(67, 24, 130) : Gc(91, 33, 182, 180);
                botCol = pressed ? Gc(50, 18, 100) : Gc(67, 24, 130, 180);
                brdCol = Gc(168, 85, 247, 170);
            } else if (pressed) {
                topCol = Gc(46, 58, 86);
                botCol = Gc(34, 44, 66);
                brdCol = Gc(96, 165, 250);
            }

            float bw = static_cast<float>(rc.right - rc.left);
            float bh = static_cast<float>(rc.bottom - rc.top);
            float rad = isMainAction ? static_cast<float>(S(10)) : static_cast<float>(S(7));
            FillRoundRectGrad(bgG, 0.5f, 0.5f, bw - 1.0f, bh - 1.0f, rad, topCol, botCol);
            StrokeRoundRect(bgG, 0.5f, 0.5f, bw - 1.0f, bh - 1.0f, rad, brdCol);

            wchar_t txt[128] = {};
            GetWindowTextW(dis->hwndItem, txt, 128);
            Gdiplus::Font fBtn(hdc, isMainAction ? boldFont_ : font_);
            Gdiplus::StringFormat sfBtn;
            sfBtn.SetAlignment(Gdiplus::StringAlignmentCenter);
            sfBtn.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            sfBtn.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            Gdiplus::SolidBrush tbr(disabled ? Gc(110, 124, 148) : Gc(243, 246, 252));
            bgG.DrawString(txt, -1, &fBtn, Gdiplus::RectF(0.0f, 0.0f, bw, bh), &sfBtn, &tbr);
            return TRUE;
        }

        case WM_NOTIFY: {
            const auto* nm = reinterpret_cast<const NMHDR*>(lParam);
            if (nm && nm->code == NM_CUSTOMDRAW &&
                (nm->hwndFrom == trkGameVol_ || nm->hwndFrom == trkMicVol_ || nm->hwndFrom == trkMicGate_)) {
                const auto* cd = reinterpret_cast<const NMCUSTOMDRAW*>(lParam);
                if (cd->dwDrawStage == CDDS_PREPAINT) {
                    return CDRF_NOTIFYITEMDRAW;
                }
                if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                    if (cd->dwItemSpec == TBCD_TICS) {
                        return CDRF_SKIPDEFAULT;
                    }
                    if (cd->dwItemSpec == TBCD_CHANNEL) {
                        Gdiplus::Graphics tg(cd->hdc);
                        tg.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                        RECT chRc = cd->rc;
                        float cx = static_cast<float>(chRc.left);
                        float cw = static_cast<float>(std::max<LONG>(1, chRc.right - chRc.left));
                        float cy = static_cast<float>((chRc.top + chRc.bottom) / 2 - S(2));
                        float ch = static_cast<float>(S(4));
                        FillRoundRect(tg, cx, cy, cw, ch, ch * 0.5f, Gc(32, 42, 62));

                        LRESULT pos = SendMessageW(nm->hwndFrom, TBM_GETPOS, 0, 0);
                        LRESULT mn = SendMessageW(nm->hwndFrom, TBM_GETRANGEMIN, 0, 0);
                        LRESULT mx = SendMessageW(nm->hwndFrom, TBM_GETRANGEMAX, 0, 0);
                        float frac = (mx > mn) ? static_cast<float>(pos - mn) / static_cast<float>(mx - mn) : 0.0f;
                        frac = std::max(0.0f, std::min(1.0f, frac));
                        if (frac > 0.01f) {
                            Gdiplus::Color c1 = (nm->hwndFrom == trkMicGate_) ? Gc(251, 191, 36) : Gc(52, 211, 153);
                            Gdiplus::Color c2 = (nm->hwndFrom == trkMicGate_) ? Gc(245, 158, 11) : Gc(16, 185, 129);
                            FillRoundRectGrad(tg, cx, cy, cw * frac, ch, ch * 0.5f, c1, c2);
                        }
                        return CDRF_SKIPDEFAULT;
                    }
                    if (cd->dwItemSpec == TBCD_THUMB) {
                        Gdiplus::Graphics tg(cd->hdc);
                        tg.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                        RECT tr = cd->rc;
                        float cx = static_cast<float>(tr.left + tr.right) * 0.5f;
                        float cy = static_cast<float>(tr.top + tr.bottom) * 0.5f;
                        float r = static_cast<float>(S(6));
                        Gdiplus::Color accent = (nm->hwndFrom == trkMicGate_) ? Gc(251, 191, 36) : Gc(52, 211, 153);
                        Gdiplus::SolidBrush outerBr(accent);
                        Gdiplus::SolidBrush innerBr(Gc(248, 250, 252));
                        tg.FillEllipse(&outerBr, cx - r, cy - r, r * 2.0f, r * 2.0f);
                        float ir = r * 0.62f;
                        tg.FillEllipse(&innerBr, cx - ir, cy - ir, ir * 2.0f, ir * 2.0f);
                        return CDRF_SKIPDEFAULT;
                    }
                }
            }
            break;
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
                    trimStartSec_ = std::max(0.0, std::min(t, trimEndSec_ - 0.25));
                    if (editCutActive_ && cutStartSec_ < trimStartSec_ + 0.1) cutStartSec_ = trimStartSec_ + 0.1;
                    playerPosSec_ = trimStartSec_;
                } else if (draggingTrim_ == 2) {
                    trimEndSec_ = std::min(dur, std::max(t, trimStartSec_ + 0.25));
                    if (editCutActive_ && cutEndSec_ > trimEndSec_ - 0.1) cutEndSec_ = trimEndSec_ - 0.1;
                    playerPosSec_ = trimEndSec_;
                } else if (draggingTrim_ == 3) {
                    cutStartSec_ = std::max(trimStartSec_ + 0.1, std::min(t, cutEndSec_ - 0.15));
                    playerPosSec_ = cutStartSec_;
                } else if (draggingTrim_ == 4) {
                    cutEndSec_ = std::min(trimEndSec_ - 0.1, std::max(t, cutStartSec_ + 0.15));
                    playerPosSec_ = cutEndSec_;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (draggingEditGameVol_ && playerOpen_ && trimMode_) {
                RECT gb = GetGameVolBarRect();
                double r = static_cast<double>(mx - gb.left) / std::max(1L, gb.right - gb.left);
                editGameVolPct_ = std::max(0, std::min(200, static_cast<int>(std::lround(r * 200.0))));
                editGameMuted_ = (editGameVolPct_ == 0);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (draggingEditMicVol_ && playerOpen_ && trimMode_ && editHasMultiTrack_) {
                RECT mb = GetMicVolBarRect();
                double r = static_cast<double>(mx - mb.left) / std::max(1L, mb.right - mb.left);
                editMicVolPct_ = std::max(0, std::min(200, static_cast<int>(std::lround(r * 200.0))));
                editMicMuted_ = (editMicVolPct_ == 0);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (draggingSeek_ && playerOpen_ && playerReady_) {
                RECT sk = GetSeekBarRect();
                double ratio = static_cast<double>(mx - sk.left) / std::max(1L, sk.right - sk.left);
                ratio = std::max(0.0, std::min(1.0, ratio));
                playerPosSec_ = ratio * playerDurSec_;
                InvalidateRect(hwnd_, nullptr, FALSE);
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
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (trimMode_ && !playerFullscreen_) {
                    RECT gb = GetGameVolBarRect();
                    if (mx >= gb.left - S(4) && mx <= gb.right + S(4) && my >= gb.top && my <= gb.bottom) {
                        draggingEditGameVol_ = true;
                        SetCapture(hwnd_);
                        double r = static_cast<double>(mx - gb.left) / std::max(1L, gb.right - gb.left);
                        editGameVolPct_ = std::max(0, std::min(200, static_cast<int>(std::lround(r * 200.0))));
                        editGameMuted_ = (editGameVolPct_ == 0);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return 0;
                    }
                    RECT mb = GetMicVolBarRect();
                    if (editHasMultiTrack_ && mx >= mb.left - S(4) && mx <= mb.right + S(4) && my >= mb.top && my <= mb.bottom) {
                        draggingEditMicVol_ = true;
                        SetCapture(hwnd_);
                        double r = static_cast<double>(mx - mb.left) / std::max(1L, mb.right - mb.left);
                        editMicVolPct_ = std::max(0, std::min(200, static_cast<int>(std::lround(r * 200.0))));
                        editMicMuted_ = (editMicVolPct_ == 0);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return 0;
                    }
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
                double tgt = (which == 1) ? trimStartSec_ : (which == 2 ? trimEndSec_ : (which == 3 ? cutStartSec_ : cutEndSec_));
                SeekPlayerAbsolute(tgt);
                return 0;
            }
            if (draggingEditGameVol_ || draggingEditMicVol_) {
                draggingEditGameVol_ = false;
                draggingEditMicVol_ = false;
                ReleaseCapture();
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
                        case BTN_CLIP_SHARE: ShareSelected(); break;
                        case BTN_CLIP_COPY_LINK: CopySelectedShareLink(); break;
                        case BTN_CLIP_OPEN_LINK: OpenSelectedShareLink(); break;
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
                        case BTN_CMB_DURATION:
                        case BTN_CMB_RESFPS:
                        case BTN_CMB_BITRATE:
                        case BTN_CMB_MIC_DEV: {
                            RECT anchorRc{};
                            for (const auto& b : customButtons_) {
                                if (b.id == target) { anchorRc = b.rc; break; }
                            }
                            HWND cmb = (target == BTN_CMB_DURATION) ? cmbDuration_
                                     : (target == BTN_CMB_RESFPS)   ? cmbResFps_
                                     : (target == BTN_CMB_BITRATE)  ? cmbBitrate_
                                                                    : cmbMicDev_;
                            ShowDarkComboDropdown(cmb, anchorRc);
                            break;
                        }

                        // Embedded Player & Trim buttons
                        case BTN_PLY_BACK: CloseEmbeddedPlayer(); break;
                        case BTN_PLY_FAV: ToggleFavoriteForViewIdx(); break;
                        case BTN_PLY_SHARE: ShareSelected(); break;
                        case BTN_PLY_RENAME: RenameSelected(); break;
                        case BTN_PLY_FOLDER: RevealSelected(); break;
                        case BTN_PLY_DELETE: DeleteSelected(); break;
                        case BTN_PLY_PLAYPAUSE: TogglePlayerPlayPause(); break;
                        case BTN_PLY_MINUS5: SeekPlayerRelative(-5.0); break;
                        case BTN_PLY_PLUS5: SeekPlayerRelative(+5.0); break;
                        case BTN_PLY_RESTART: SeekPlayerAbsolute(0.0); break;
                        case BTN_PLY_RATE: CyclePlayerRate(); break;
                        case BTN_PLY_TRIM: ToggleTrimMode(); break;
                        case BTN_PLY_FULLSCREEN: TogglePlayerFullscreen(); break;
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
                        case BTN_EDIT_CUT_TOGGLE: {
                            editCutActive_ = !editCutActive_;
                            if (editCutActive_) {
                                double span = trimEndSec_ - trimStartSec_;
                                if (span < 0.6) {
                                    editCutActive_ = false;
                                } else {
                                    double mid = std::max(trimStartSec_ + 0.25, std::min(trimEndSec_ - 0.25, playerPosSec_));
                                    double half = std::min(2.0, span * 0.2);
                                    cutStartSec_ = std::max(trimStartSec_ + 0.1, mid - half);
                                    cutEndSec_ = std::min(trimEndSec_ - 0.1, mid + half);
                                }
                            }
                            InvalidateRect(hwnd_, nullptr, FALSE);
                            break;
                        }
                        case BTN_EDIT_SPEED:
                            if (editSpeed_ < 0.75) editSpeed_ = 1.0;
                            else if (editSpeed_ < 1.25) editSpeed_ = 1.5;
                            else if (editSpeed_ < 1.75) editSpeed_ = 2.0;
                            else editSpeed_ = 0.5;
                            InvalidateRect(hwnd_, nullptr, FALSE);
                            break;
                        case BTN_EDIT_ADD_CLIP: PickInsertClip(); break;
                        case BTN_EDIT_GAME_MUTE:
                            editGameMuted_ = !editGameMuted_;
                            InvalidateRect(hwnd_, nullptr, FALSE);
                            break;
                        case BTN_EDIT_MIC_MUTE:
                            if (editHasMultiTrack_) editMicMuted_ = !editMicMuted_;
                            InvalidateRect(hwnd_, nullptr, FALSE);
                            break;
                        case BTN_EDIT_VERTICAL:
                            editVertical916_ = !editVertical916_;
                            InvalidateRect(hwnd_, nullptr, FALSE);
                            break;
                        case BTN_EDIT_DISCORD10: StartTrimAsync(false, 10); break;
                        case BTN_EDIT_DISCORD25: StartTrimAsync(false, 25); break;
                        case BTN_TRIM_SAVE_COPY: StartTrimAsync(false, 0); break;
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
                case ID_CHK_SPOTIFY_AUDIO:
                case ID_CHK_MIC_AUDIO:
                case ID_CHK_MIC_MUTE:
                case ID_CHK_MIC_GATE: {
                    AppConfig cfg = app_.Config();
                    cfg.captureGameAudio = (SendMessageW(chkGameAudio_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    cfg.captureSpotify = (SendMessageW(chkSpotifyAudio_, BM_GETCHECK, 0, 0) == BST_CHECKED);
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
                case ID_CHK_MOBILE_SYNC: {
                    AppConfig cfg = app_.Config();
                    cfg.mobileSyncEnabled = (SendMessageW(chkMobileSync_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    if (cb_.onConfigApplied) cb_.onConfigApplied(cfg);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    break;
                }
                case ID_BTN_MOBILE_QR:
                    ShowMobileQrModal();
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

        case WM_NCDESTROY:
            FreeBackbuffer();
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void MainWindow::ShowMobileQrModal() {
    if (!app_.IsMobileSyncRunning()) {
        AppConfig cfg = app_.Config();
        cfg.mobileSyncEnabled = true;
        if (chkMobileSync_) SendMessageW(chkMobileSync_, BM_SETCHECK, BST_CHECKED, 0);
        if (cb_.onConfigApplied) cb_.onConfigApplied(cfg);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    struct QrModalState {
        MainWindow* mw = nullptr;
        HWND dlg = nullptr;
        bool done = false;
        QrCode::Matrix qr;
        std::wstring url;
        std::wstring ipPort;
        std::wstring pin;
    } st{ this };

    auto refreshQr = [&]() {
        st.url = app_.GetMobilePairUrl();
        const AppConfig& mc = app_.Config();
        wchar_t buf[96] = {};
        swprintf(buf, 96, L"%ls:%d", app_.GetMobileLanIp().c_str(), mc.mobileSyncPort);
        st.ipPort = buf;
        st.pin = mc.mobilePin;
        int n = WideCharToMultiByte(CP_UTF8, 0, st.url.c_str(), static_cast<int>(st.url.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(n > 0 ? static_cast<size_t>(n) : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, st.url.c_str(), static_cast<int>(st.url.size()), &utf8[0], n, nullptr, nullptr);
        QrCode::Encode(utf8, st.qr);
    };
    refreshQr();

    static const wchar_t* kQrCls = L"AnlikKlipQrModal";
    static bool qrReg = false;
    if (!qrReg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM wp, LPARAM lp) -> LRESULT {
            auto* s = reinterpret_cast<QrModalState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            if (m == WM_NCCREATE) {
                auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                s = reinterpret_cast<QrModalState*>(cs->lpCreateParams);
                SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            }
            if (!s) return DefWindowProcW(h, m, wp, lp);
            switch (m) {
                case WM_ERASEBKGND: return 1;
                case WM_PAINT: {
                    PAINTSTRUCT ps{};
                    HDC hdc = BeginPaint(h, &ps);
                    RECT rc{}; GetClientRect(h, &rc);
                    Gdiplus::Graphics g(hdc);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    Gdiplus::SolidBrush bg(Gdiplus::Color(255, 16, 20, 29));
                    g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

                    int qrBox = s->mw->S(220);
                    int qrX = (rc.right - qrBox) / 2;
                    int qrY = s->mw->S(48);
                    Gdiplus::SolidBrush whiteBr(Gdiplus::Color(255, 255, 255, 255));
                    g.FillRectangle(&whiteBr, qrX, qrY, qrBox, qrBox);
                    if (s->qr.size > 0) {
                        int quiet = 3;
                        int totalMod = s->qr.size + quiet * 2;
                        float cell = static_cast<float>(qrBox) / static_cast<float>(totalMod);
                        Gdiplus::SolidBrush darkBr(Gdiplus::Color(255, 12, 16, 24));
                        for (int y = 0; y < s->qr.size; ++y) {
                            for (int x = 0; x < s->qr.size; ++x) {
                                if (s->qr.Get(x, y)) {
                                    g.FillRectangle(&darkBr,
                                        qrX + (x + quiet) * cell,
                                        qrY + (y + quiet) * cell,
                                        cell + 0.4f, cell + 0.4f);
                                }
                            }
                        }
                    }

                    Gdiplus::FontFamily ff(L"Segoe UI");
                    Gdiplus::Font fTitle(&ff, 12.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
                    Gdiplus::Font fInfo(&ff, 10.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
                    Gdiplus::Font fSub(&ff, 8.8f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                    Gdiplus::StringFormat sfC;
                    sfC.SetAlignment(Gdiplus::StringAlignmentCenter);
                    Gdiplus::SolidBrush txtBr(Gdiplus::Color(255, 238, 242, 255));
                    Gdiplus::SolidBrush pinBr(Gdiplus::Color(255, 134, 239, 172));
                    Gdiplus::SolidBrush mutBr(Gdiplus::Color(255, 148, 163, 184));

                    g.DrawString(L"Telefona Klipleri Aktar (Wi-Fi)", -1, &fTitle,
                        Gdiplus::RectF(0, (float)s->mw->S(14), (float)rc.right, (float)s->mw->S(26)), &sfC, &txtBr);
                    wchar_t line1[160] = {};
                    swprintf(line1, 160, L"Adres: %ls   •   PIN: %ls", s->ipPort.c_str(), s->pin.c_str());
                    g.DrawString(line1, -1, &fInfo,
                        Gdiplus::RectF(0, (float)(qrY + qrBox + s->mw->S(14)), (float)rc.right, (float)s->mw->S(24)), &sfC, &pinBr);
                    g.DrawString(L"Anlık Klip Android uygulamasından QR okutun veya\ntelefon tarayıcısında bu adresi açın.", -1, &fSub,
                        Gdiplus::RectF((float)s->mw->S(16), (float)(qrY + qrBox + s->mw->S(42)),
                            (float)(rc.right - s->mw->S(32)), (float)s->mw->S(38)), &sfC, &mutBr);

                    EndPaint(h, &ps);
                    return 0;
                }
                case WM_COMMAND:
                    if (LOWORD(wp) == 9001) {
                        s->mw->app_.RegenerateMobilePin();
                        // Refresh parent settings & QR
                        const AppConfig& mc = s->mw->app_.Config();
                        s->url = s->mw->app_.GetMobilePairUrl();
                        wchar_t buf[96] = {};
                        swprintf(buf, 96, L"%ls:%d", s->mw->app_.GetMobileLanIp().c_str(), mc.mobileSyncPort);
                        s->ipPort = buf;
                        s->pin = mc.mobilePin;
                        int n = WideCharToMultiByte(CP_UTF8, 0, s->url.c_str(), static_cast<int>(s->url.size()), nullptr, 0, nullptr, nullptr);
                        std::string utf8(n > 0 ? static_cast<size_t>(n) : 0, '\0');
                        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s->url.c_str(), static_cast<int>(s->url.size()), &utf8[0], n, nullptr, nullptr);
                        QrCode::Encode(utf8, s->qr);
                        InvalidateRect(h, nullptr, FALSE);
                        InvalidateRect(s->mw->hwnd_, nullptr, FALSE);
                        return 0;
                    }
                    if (LOWORD(wp) == 9002 || LOWORD(wp) == IDCANCEL) {
                        s->done = true;
                        DestroyWindow(h);
                        return 0;
                    }
                    break;
                case WM_CLOSE:
                    s->done = true;
                    DestroyWindow(h);
                    return 0;
            }
            return DefWindowProcW(h, m, wp, lp);
        };
        wc.hInstance = hInst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kQrCls;
        RegisterClassExW(&wc);
        qrReg = true;
    }

    int dw = S(360), dh = S(415);
    RECT prc{}; GetWindowRect(hwnd_, &prc);
    int dx = prc.left + (prc.right - prc.left - dw) / 2;
    int dy = prc.top + (prc.bottom - prc.top - dh) / 2;
    EnableWindow(hwnd_, FALSE);

    st.dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kQrCls, L"QR Kod ile Bağlan",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        dx, dy, dw, dh, hwnd_, nullptr, hInst_, &st);
    ApplyDarkTitleBarAndControls(st.dlg);

    RECT crc{}; GetClientRect(st.dlg, &crc);
    HWND b1 = CreateWindowExW(0, L"BUTTON", L"Yeni PIN", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        S(62), crc.bottom - S(44), S(108), S(30), st.dlg, reinterpret_cast<HMENU>(9001), hInst_, nullptr);
    HWND b2 = CreateWindowExW(0, L"BUTTON", L"Kapat", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        S(190), crc.bottom - S(44), S(108), S(30), st.dlg, reinterpret_cast<HMENU>(9002), hInst_, nullptr);
    SendMessageW(b1, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(b2, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            st.done = true;
            DestroyWindow(st.dlg);
            break;
        }
        if (!IsDialogMessageW(st.dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(hwnd_, TRUE);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ShowDarkComboDropdown(HWND comboHwnd, RECT anchorClientRc) {
    if (!comboHwnd) return;
    int count = static_cast<int>(SendMessageW(comboHwnd, CB_GETCOUNT, 0, 0));
    if (count <= 0) return;
    int curSel = static_cast<int>(SendMessageW(comboHwnd, CB_GETCURSEL, 0, 0));

    std::vector<std::wstring> items;
    items.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        int len = static_cast<int>(SendMessageW(comboHwnd, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0));
        if (len < 0) len = 0;
        std::wstring s(static_cast<size_t>(len) + 1, L'\0');
        SendMessageW(comboHwnd, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&s[0]));
        s.resize(wcslen(s.c_str()));
        items.push_back(std::move(s));
    }

    RECT scrRc = anchorClientRc;
    MapWindowPoints(hwnd_, HWND_DESKTOP, reinterpret_cast<POINT*>(&scrRc), 2);

    const int padY = S(4);
    const int itemH = S(26);
    const int popW = std::max<int>(scrRc.right - scrRc.left, S(200));
    const int popH = count * itemH + padY * 2;

    int px = scrRc.left;
    int py = scrRc.bottom + S(3);

    HMONITOR mon = MonitorFromRect(&scrRc, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        if (py + popH > mi.rcWork.bottom && scrRc.top - S(3) - popH >= mi.rcWork.top) {
            py = scrRc.top - S(3) - popH;
        }
        if (px + popW > mi.rcWork.right) px = mi.rcWork.right - popW;
        if (px < mi.rcWork.left) px = mi.rcWork.left;
    }

    struct DropState {
        MainWindow* mw = nullptr;
        std::vector<std::wstring>* items = nullptr;
        int curSel = -1;
        int hoverIdx = -1;
        int chosen = -1;
        int itemH = 26;
        int padY = 4;
        bool done = false;
        HWND pop = nullptr;
    } st{ this, &items, curSel, curSel, -1, itemH, padY, false, nullptr };

    static const wchar_t* kDropCls = L"AnlikKlipDarkComboPopup";
    static bool dropReg = false;
    if (!dropReg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM wp, LPARAM lp) -> LRESULT {
            auto* s = reinterpret_cast<DropState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            if (m == WM_NCCREATE) {
                auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                s = reinterpret_cast<DropState*>(cs->lpCreateParams);
                SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            }
            if (!s) return DefWindowProcW(h, m, wp, lp);
            switch (m) {
                case WM_ERASEBKGND: return 1;
                case WM_ACTIVATE:
                    if (LOWORD(wp) == WA_INACTIVE && !s->done) {
                        s->done = true;
                        DestroyWindow(h);
                    }
                    return 0;
                case WM_MOUSEMOVE: {
                    int my = static_cast<short>(HIWORD(lp));
                    int idx = (my - s->padY) / s->itemH;
                    if (my < s->padY || idx < 0 || idx >= static_cast<int>(s->items->size())) idx = -1;
                    if (idx != s->hoverIdx) {
                        s->hoverIdx = idx;
                        InvalidateRect(h, nullptr, FALSE);
                    }
                    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                    TrackMouseEvent(&tme);
                    return 0;
                }
                case WM_MOUSELEAVE:
                    if (s->hoverIdx != -1) {
                        s->hoverIdx = -1;
                        InvalidateRect(h, nullptr, FALSE);
                    }
                    return 0;
                case WM_LBUTTONUP: {
                    int my = static_cast<short>(HIWORD(lp));
                    int idx = (my - s->padY) / s->itemH;
                    if (my >= s->padY && idx >= 0 && idx < static_cast<int>(s->items->size())) {
                        s->chosen = idx;
                    }
                    s->done = true;
                    DestroyWindow(h);
                    return 0;
                }
                case WM_KEYDOWN:
                    if (wp == VK_ESCAPE) {
                        s->done = true;
                        DestroyWindow(h);
                        return 0;
                    } else if (wp == VK_RETURN && s->hoverIdx >= 0) {
                        s->chosen = s->hoverIdx;
                        s->done = true;
                        DestroyWindow(h);
                        return 0;
                    } else if (wp == VK_DOWN && !s->items->empty()) {
                        s->hoverIdx = (s->hoverIdx + 1) % static_cast<int>(s->items->size());
                        InvalidateRect(h, nullptr, FALSE);
                        return 0;
                    } else if (wp == VK_UP && !s->items->empty()) {
                        int n = static_cast<int>(s->items->size());
                        s->hoverIdx = (s->hoverIdx <= 0) ? (n - 1) : (s->hoverIdx - 1);
                        InvalidateRect(h, nullptr, FALSE);
                        return 0;
                    }
                    return 0;
                case WM_PAINT: {
                    PAINTSTRUCT ps{};
                    HDC hdc = BeginPaint(h, &ps);
                    RECT rc{}; GetClientRect(h, &rc);
                    HDC memDC = CreateCompatibleDC(hdc);
                    HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
                    HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

                    Gdiplus::Graphics g(memDC);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

                    Gdiplus::SolidBrush bg(Gdiplus::Color(255, 18, 23, 36));
                    g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

                    Gdiplus::Pen borderPen(Gdiplus::Color(255, 59, 130, 246), 1.0f);
                    g.DrawRectangle(&borderPen, 0, 0, rc.right - 1, rc.bottom - 1);

                    Gdiplus::Font fnt(memDC, s->mw->font_);
                    Gdiplus::StringFormat sf;
                    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
                    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

                    for (int i = 0; i < static_cast<int>(s->items->size()); ++i) {
                        int iy = s->padY + i * s->itemH;
                        float ix = static_cast<float>(s->mw->S(4));
                        float iyf = static_cast<float>(iy + 1);
                        float iw = static_cast<float>(rc.right - s->mw->S(8));
                        float ih = static_cast<float>(s->itemH - 2);

                        if (i == s->hoverIdx) {
                            FillRoundRect(g, ix, iyf, iw, ih, 5.0f, Gdiplus::Color(255, 37, 99, 235));
                        } else if (i == s->curSel) {
                            FillRoundRect(g, ix, iyf, iw, ih, 5.0f, Gdiplus::Color(255, 28, 37, 58));
                        }

                        Gdiplus::Color txtCol = (i == s->hoverIdx) ? Gdiplus::Color(255, 255, 255, 255)
                                              : (i == s->curSel)   ? Gdiplus::Color(255, 147, 197, 253)
                                                                   : Gdiplus::Color(255, 226, 232, 240);
                        Gdiplus::SolidBrush txtBr(txtCol);
                        Gdiplus::RectF textRc(
                            static_cast<Gdiplus::REAL>(s->mw->S(12)),
                            static_cast<Gdiplus::REAL>(iy),
                            static_cast<Gdiplus::REAL>(rc.right - s->mw->S(28)),
                            static_cast<Gdiplus::REAL>(s->itemH));
                        g.DrawString((*s->items)[static_cast<size_t>(i)].c_str(), -1, &fnt, textRc, &sf, &txtBr);

                        if (i == s->curSel) {
                            Gdiplus::SolidBrush dotBr(i == s->hoverIdx ? Gdiplus::Color(255, 255, 255, 255)
                                                                       : Gdiplus::Color(255, 59, 130, 246));
                            float dr = 3.0f;
                            g.FillEllipse(&dotBr,
                                static_cast<Gdiplus::REAL>(rc.right - s->mw->S(16)) - dr,
                                static_cast<Gdiplus::REAL>(iy + s->itemH / 2) - dr,
                                dr * 2.0f, dr * 2.0f);
                        }
                    }

                    BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
                    SelectObject(memDC, oldBmp);
                    DeleteObject(memBmp);
                    DeleteDC(memDC);
                    EndPaint(h, &ps);
                    return 0;
                }
                case WM_DESTROY:
                    s->done = true;
                    return 0;
            }
            return DefWindowProcW(h, m, wp, lp);
        };
        wc.hInstance = hInst_;
        wc.lpszClassName = kDropCls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        dropReg = true;
    }

    st.pop = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kDropCls, L"",
        WS_POPUP | WS_VISIBLE, px, py, popW, popH, hwnd_, nullptr, hInst_, &st);
    SetFocus(st.pop);

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (st.chosen >= 0 && st.chosen < count) {
        SendMessageW(comboHwnd, CB_SETCURSEL, static_cast<WPARAM>(st.chosen), 0);
        SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(comboHwnd), CBN_SELCHANGE),
            reinterpret_cast<LPARAM>(comboHwnd));
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
