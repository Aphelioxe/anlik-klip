#include "tray_ui.h"
#include "clip_library.h"
#include "com_ptr.h"
#include "sound_fx.h"

#include <commctrl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr UINT_PTR TIMER_TRAY_STATS = 1;

enum MenuId {
    ID_MENU_SAVE_CFG = 2001,
    ID_MENU_SAVE_30S,
    ID_MENU_SAVE_60S,
    ID_MENU_TOGGLE_REC,
    ID_MENU_TOGGLE_MIC,
    ID_MENU_SHOW_CLIPS,
    ID_MENU_SHOW_SETTINGS,
    ID_MENU_OPEN_FOLDER,
    ID_MENU_EXIT
};

const wchar_t* kTrayWndClass = L"AnlikKlipMainWnd";

struct CornerToastData {
    std::wstring title;
    std::wstring sub;
    int targetX = 0;
    int targetY = 0;
    int w = 332;
    int h = 66;
    ULONGLONG startMs = 0;
};

void ToastAddRoundRect(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    p.AddArc(x, y, d, d, 180.0f, 90.0f);
    p.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    p.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    p.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

LRESULT CALLBACK CornerToastProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<CornerToastData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER: {
            if (!data) {
                DestroyWindow(hwnd);
                return 0;
            }
            ULONGLONG elapsed = GetTickCount64() - data->startMs;
            const float inMs = 180.0f;
            const float holdMs = 2300.0f;
            const float outMs = 240.0f;
            const float totalMs = inMs + holdMs + outMs;

            if (static_cast<float>(elapsed) >= totalMs) {
                KillTimer(hwnd, 1);
                DestroyWindow(hwnd);
                return 0;
            }
            if (static_cast<float>(elapsed) < inMs) {
                float t = static_cast<float>(elapsed) / inMs;
                float ease = 1.0f - std::pow(1.0f - t, 3.0f);
                int curX = data->targetX + static_cast<int>(std::round((1.0f - ease) * 32.0f));
                BYTE alpha = static_cast<BYTE>(std::round(ease * 244.0f));
                SetWindowPos(hwnd, nullptr, curX, data->targetY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
            } else if (static_cast<float>(elapsed) < inMs + holdMs) {
                SetWindowPos(hwnd, nullptr, data->targetX, data->targetY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                SetLayeredWindowAttributes(hwnd, 0, 244, LWA_ALPHA);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                float t = (static_cast<float>(elapsed) - (inMs + holdMs)) / outMs;
                float ease = 1.0f - std::pow(t, 2.0f);
                BYTE alpha = static_cast<BYTE>(std::max(0.0f, std::round(ease * 244.0f)));
                SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{}; GetClientRect(hwnd, &rc);
            int w = std::max<int>(1, rc.right);
            int h = std::max<int>(1, rc.bottom);

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

            {
                Gdiplus::Graphics g(memDC);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

                // Dark glassmorphic card gradient
                Gdiplus::LinearGradientBrush bg(
                    Gdiplus::PointF(0, 0), Gdiplus::PointF(0, static_cast<float>(h)),
                    Gdiplus::Color(255, 20, 27, 43), Gdiplus::Color(255, 13, 18, 30));
                g.FillRectangle(&bg, 0, 0, w, h);

                // Subtle emerald glow tint at left
                Gdiplus::LinearGradientBrush glow(
                    Gdiplus::PointF(0, 0), Gdiplus::PointF(150.0f, 0),
                    Gdiplus::Color(42, 34, 197, 94), Gdiplus::Color(0, 34, 197, 94));
                g.FillRectangle(&glow, 0, 0, 150, h);

                // Rounded inner border
                Gdiplus::GraphicsPath bp;
                ToastAddRoundRect(bp, 0.5f, 0.5f, static_cast<float>(w - 1), static_cast<float>(h - 1), 10.0f);
                Gdiplus::Pen borderPen(Gdiplus::Color(165, 52, 211, 153), 1.2f);
                g.DrawPath(&borderPen, &bp);

                // Circular emerald badge with crisp anti-aliased checkmark
                Gdiplus::LinearGradientBrush badgeBr(
                    Gdiplus::PointF(14, 15), Gdiplus::PointF(48, 49),
                    Gdiplus::Color(255, 52, 211, 153), Gdiplus::Color(255, 16, 185, 129));
                g.FillEllipse(&badgeBr, 14.0f, 15.0f, 34.0f, 34.0f);

                Gdiplus::Pen chkPen(Gdiplus::Color(255, 255, 255, 255), 2.5f);
                chkPen.SetStartCap(Gdiplus::LineCapRound);
                chkPen.SetEndCap(Gdiplus::LineCapRound);
                chkPen.SetLineJoin(Gdiplus::LineJoinRound);
                Gdiplus::PointF pts[3] = {
                    { 24.5f, 32.5f },
                    { 29.2f, 37.2f },
                    { 38.0f, 27.5f }
                };
                g.DrawLines(&chkPen, pts, 3);

                if (data) {
                    Gdiplus::FontFamily ff(L"Segoe UI");
                    Gdiplus::Font fTitle(&ff, 10.2f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
                    Gdiplus::Font fSub(&ff, 8.6f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                    Gdiplus::StringFormat sf;
                    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
                    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

                    Gdiplus::SolidBrush brTitle(Gdiplus::Color(255, 243, 246, 252));
                    Gdiplus::SolidBrush brSub(Gdiplus::Color(255, 156, 168, 190));

                    g.DrawString(data->title.c_str(), -1, &fTitle,
                        Gdiplus::RectF(58.0f, 12.0f, static_cast<float>(w - 70), 22.0f), &sf, &brTitle);
                    g.DrawString(data->sub.c_str(), -1, &fSub,
                        Gdiplus::RectF(58.0f, 34.0f, static_cast<float>(w - 70), 20.0f), &sf, &brSub);

                    // Bottom progress bar showing remaining display time
                    ULONGLONG elapsed = GetTickCount64() - data->startMs;
                    float frac = 1.0f - std::max(0.0f, std::min(1.0f,
                        (static_cast<float>(elapsed) - 180.0f) / 2300.0f));
                    Gdiplus::SolidBrush trackBr(Gdiplus::Color(255, 26, 35, 52));
                    g.FillRectangle(&trackBr, 12.0f, static_cast<float>(h - 4), static_cast<float>(w - 24), 2.5f);
                    if (frac > 0.01f) {
                        Gdiplus::SolidBrush progBr(Gdiplus::Color(255, 52, 211, 153));
                        g.FillRectangle(&progBr, 12.0f, static_cast<float>(h - 4),
                            static_cast<float>(w - 24) * frac, 2.5f);
                    }
                }
            }

            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            delete data;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowCornerToast(HINSTANCE hInst, const std::wstring& title, const std::wstring& sub) {
    static ULONG_PTR toastGdipToken = 0;
    if (!toastGdipToken) {
        Gdiplus::GdiplusStartupInput gsi;
        Gdiplus::GdiplusStartup(&toastGdipToken, &gsi, nullptr);
    }

    static bool reg = false;
    const wchar_t* cls = L"AnlikKlipCornerToast";
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CornerToastProc;
        wc.hInstance = hInst;
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        reg = true;
    }
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int w = 332, h = 66;
    const int x = wa.right - w - 20;
    const int y = wa.top + 20;

    auto* d = new CornerToastData{ title, sub, x, y, w, h, GetTickCount64() };
    HWND hw = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        cls, L"", WS_POPUP, x + 32, y, w, h, nullptr, nullptr, hInst, d);
    if (!hw) {
        delete d;
        return;
    }
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, 20, 20);
    SetWindowRgn(hw, rgn, TRUE);
    SetLayeredWindowAttributes(hw, 0, 0, LWA_ALPHA);
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    SetTimer(hw, 1, 16, nullptr);
}

} // namespace

TrayUI::TrayUI(HINSTANCE hInst, RecorderApp& app)
    : hInst_(hInst), app_(app) {}

TrayUI::~TrayUI() {
    Destroy();
}

HICON TrayUI::CreateStatusIcon(COLORREF dotColor) {
    const int size = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC screenDc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP colorBmp = CreateDIBSection(screenDc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);
    if (!colorBmp || !bits) return nullptr;

    uint32_t* px = reinterpret_cast<uint32_t*>(bits);
    auto setPx = [&](int x, int y, uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || x >= size || y < 0 || y >= size) return;
        px[y * size + x] = (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
    };

    const uint8_t dr = GetRValue(dotColor);
    const uint8_t dg = GetGValue(dotColor);
    const uint8_t db = GetBValue(dotColor);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            bool inside = (x >= 2 && x <= 29 && y >= 2 && y <= 29);
            if ((x == 2 || x == 29) && (y == 2 || y == 29)) inside = false;
            if (!inside) { setPx(x, y, 0, 0, 0, 0); continue; }
            bool border = (x == 2 || x == 29 || y == 2 || y == 29);
            setPx(x, y, 255, border ? 72 : 22, border ? 82 : 26, border ? 98 : 35);
        }
    }

    const float cx = 15.5f, cy = 15.5f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = x - cx, dy = y - cy;
            float d2 = dx * dx + dy * dy;
            if (d2 <= 5.5f * 5.5f) {
                setPx(x, y, 255, dr, dg, db);
            } else if (d2 <= 9.2f * 9.2f && d2 >= 7.2f * 7.2f) {
                setPx(x, y, 255, 232, 238, 248);
            }
        }
    }

    HBITMAP maskBmp = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{ TRUE, 0, 0, maskBmp, colorBmp };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    return icon;
}

bool TrayUI::Create(bool showWindow) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayUI::TrayWndProc;
    wc.hInstance = hInst_;
    wc.lpszClassName = kTrayWndClass;
    RegisterClassExW(&wc);

    wmTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    // Use the exact embedded desktop/exe icon (resource ID 1 = assets/app.ico) for system tray & window
    iconTray_ = reinterpret_cast<HICON>(LoadImageW(hInst_, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!iconTray_) iconTray_ = CreateStatusIcon(RGB(59, 130, 246));

    iconWindowBig_ = reinterpret_cast<HICON>(LoadImageW(hInst_, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    if (!iconWindowBig_) iconWindowBig_ = iconTray_;

    // Tiny hidden top-level host window (~0 KB UI footprint when MainWindow is closed)
    trayHwnd_ = CreateWindowExW(0, kTrayWndClass, L"AnlikKlipHost", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, hInst_, this);
    if (!trayHwnd_) return false;

    const auto& cfg = app_.Config();
    hotkeys_.Start(trayHwnd_, WM_APP_TRIGGER_CLIP, WM_APP_TRIGGER_MIC);
    hotkeys_.SetHotkeys(cfg.hotkeyMods, cfg.hotkeyVk, cfg.micHotkeyMods, cfg.micHotkeyVk);

    UpdateTrayIcon();
    SetTimer(trayHwnd_, TIMER_TRAY_STATS, 1000, nullptr);

    if (showWindow) {
        ShowSettingsWindow(0);
    }
    return true;
}

void TrayUI::Destroy() {
    mainWin_.reset();
    hotkeys_.Stop();

    if (trayHwnd_) {
        KillTimer(trayHwnd_, TIMER_TRAY_STATS);
        if (trayAdded_) {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = trayHwnd_;
            nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            trayAdded_ = false;
        }
        DestroyWindow(trayHwnd_);
        trayHwnd_ = nullptr;
    }

    if (iconWindowBig_ && iconWindowBig_ != iconTray_) { DestroyIcon(iconWindowBig_); }
    iconWindowBig_ = nullptr;
    if (iconTray_) { DestroyIcon(iconTray_); iconTray_ = nullptr; }
}

void TrayUI::ShowSettingsWindow(int tabIndex) {
    if (!trayHwnd_) return;
    if (mainWin_) {
        mainWin_->BringToFront(tabIndex);
        return;
    }

    MainWindow::Callbacks cb;
    cb.onClosed = [this]() {
        if (trayHwnd_) PostMessageW(trayHwnd_, WM_APP_DESTROY_UI, 0, 0);
    };
    cb.onExitRequested = []() {
        PostQuitMessage(0);
    };
    cb.onSaveClipRequested = [this]() {
        TriggerSaveClip(0);
    };
    cb.onToggleBufferingRequested = [this]() {
        app_.ToggleBuffering();
        UpdateTrayIcon();
        if (mainWin_) mainWin_->RefreshLiveStats();
    };
    cb.onToggleMicMuteRequested = [this]() {
        app_.ToggleMicMute();
        if (mainWin_) mainWin_->RefreshLiveStats();
    };
    cb.onConfigApplied = [this](const AppConfig& newCfg) {
        ApplyConfigFromUi(newCfg);
    };

    mainWin_ = std::make_unique<MainWindow>(hInst_, app_, hotkeys_, std::move(cb));
    if (!mainWin_->Create(tabIndex)) {
        mainWin_.reset();
        return;
    }
    if (mainWin_->Hwnd()) {
        if (iconWindowBig_) SendMessageW(mainWin_->Hwnd(), WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconWindowBig_));
        if (iconTray_) SendMessageW(mainWin_->Hwnd(), WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconTray_));
    }
}

void TrayUI::CloseMainWindowToTray() {
    if (!mainWin_) return;
    mainWin_.reset();
    ClearClipMetadataCache();

    // Flush released UI, GDI+, thumbnail, and metadata pages immediately from working set
    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));

    if (!shownHideBalloon_) {
        shownHideBalloon_ = true;
        std::wstring hk = FormatHotkey(app_.Config().hotkeyMods, app_.Config().hotkeyVk);
        wchar_t msg[200] = {};
        swprintf(msg, 200, L"Arka planda düşük RAM modunda çalışıyor. Oyun içinde %ls tuşuna basmanız yeterli.",
            hk.c_str());
        ShowBalloon(L"Anlık Klip Arka Planda", msg, NIIF_INFO);
    }
}

bool TrayUI::HandlePreTranslateMessage(const MSG& msg) {
    return mainWin_ ? mainWin_->HandlePreTranslateMessage(msg) : false;
}

void TrayUI::ApplyConfigFromUi(const AppConfig& newCfg) {
    app_.ApplyConfig(newCfg);
    hotkeys_.SetHotkeys(newCfg.hotkeyMods, newCfg.hotkeyVk, newCfg.micHotkeyMods, newCfg.micHotkeyVk);
    UpdateTrayIcon();
    if (mainWin_) mainWin_->RefreshLiveStats();
}

void TrayUI::UpdateTrayIcon() {
    if (!trayHwnd_) return;
    AppRuntimeStats st = app_.GetStats();

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = trayHwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = iconTray_;

    std::wstring hk = FormatHotkey(app_.Config().hotkeyMods, app_.Config().hotkeyVk);
    swprintf(nid.szTip, 128, L"Anlık Klip — %ls (%ls)",
        st.running ? L"Kayıt Aktif" : L"Duraklatıldı", hk.c_str());

    if (!trayAdded_) {
        Shell_NotifyIconW(NIM_ADD, &nid);
        trayAdded_ = true;
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void TrayUI::ShowBalloon(const wchar_t* title, const wchar_t* text, DWORD flags) {
    if (!trayHwnd_ || !trayAdded_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = trayHwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = flags | NIIF_NOSOUND;
    wcsncpy(nid.szInfoTitle, title, 63);
    wcsncpy(nid.szInfo, text, 255);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayUI::TriggerSaveClip(int secondsOverride) {
    HWND target = trayHwnd_;
    bool started = app_.SaveClipAsync(secondsOverride, [target](const ClipSaveResult& res) {
        auto* copy = new ClipSaveResult(res);
        if (!PostMessageW(target, WM_APP_CLIP_SAVED, 0, reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
    });
    if (started && app_.Config().soundNotify) {
        PlayClipChime();
    }
    UpdateTrayIcon();
}

void TrayUI::OpenFolder(const std::wstring& path) {
    EnsureDirectoryExists(path);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void TrayUI::ShowTrayMenu() {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();

    std::wstring hk = FormatHotkey(app_.Config().hotkeyMods, app_.Config().hotkeyVk);
    wchar_t saveLabel[128] = {};
    swprintf(saveLabel, 128, L"Son %d Sn Kaydet (%ls)", app_.Config().clipSeconds, hk.c_str());
    AppendMenuW(menu, MF_STRING | (app_.IsRunning() ? 0 : MF_GRAYED), ID_MENU_SAVE_CFG, saveLabel);
    AppendMenuW(menu, MF_STRING | (app_.IsRunning() ? 0 : MF_GRAYED), ID_MENU_SAVE_30S, L"Son 30 Saniyeyi Kaydet");
    AppendMenuW(menu, MF_STRING | (app_.IsRunning() ? 0 : MF_GRAYED), ID_MENU_SAVE_60S, L"Son 1 Dakikayı Kaydet");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, ID_MENU_TOGGLE_REC, app_.IsRunning() ? L"Tamponu Duraklat" : L"Tamponu Başlat");
    AppendMenuW(menu, MF_STRING, ID_MENU_TOGGLE_MIC,
        app_.Config().micMuted ? L"Mikrofon: SUSTURULDU (Aç)" : L"Mikrofon: Açık (Sustur)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_MENU_SHOW_CLIPS, L"Kliplerim / Arayüzü Aç");
    AppendMenuW(menu, MF_STRING, ID_MENU_SHOW_SETTINGS, L"Ayarlar...");
    AppendMenuW(menu, MF_STRING, ID_MENU_OPEN_FOLDER, L"Klipler Klasörünü Aç");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"Çıkış");

    SetForegroundWindow(trayHwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, trayHwnd_, nullptr);
    PostMessageW(trayHwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK TrayUI::TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TrayUI* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<TrayUI*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->trayHwnd_ = hwnd;
    } else {
        self = reinterpret_cast<TrayUI*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->OnMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TrayUI::OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (wmTaskbarCreated_ != 0 && msg == wmTaskbarCreated_) {
        trayAdded_ = false;
        UpdateTrayIcon();
        return 0;
    }

    switch (msg) {
        case WM_TIMER:
            if (wParam == TIMER_TRAY_STATS) {
                UpdateTrayIcon();
                if (mainWin_) mainWin_->RefreshLiveStats();
            }
            return 0;

        case WM_APP_SHOW_UI:
            ShowSettingsWindow(0);
            return 0;

        case WM_APP_DESTROY_UI:
            CloseMainWindowToTray();
            return 0;

        case WM_APP_TRIGGER_CLIP:
            TriggerSaveClip(0);
            return 0;

        case WM_APP_TRIGGER_MIC: {
            bool muted = app_.ToggleMicMute();
            UpdateTrayIcon();
            if (mainWin_) mainWin_->RefreshLiveStats();
            ShowCornerToast(hInst_, muted ? L"Mikrofon Susturuldu" : L"Mikrofon Açıldı",
                            muted ? L"Kayıtlarda mikrofon sesi kapatıldı" : L"Mikrofon sesi yeniden aktif");
            return 0;
        }

        case WM_APP_CLIP_SAVED: {
            auto* res = reinterpret_cast<ClipSaveResult*>(lParam);
            if (res) {
                if (res->ok) {
                    std::wstring durPart;
                    size_t lp = res->message.find(L'(');
                    size_t rp = res->message.find(L')', lp != std::wstring::npos ? lp : 0);
                    if (lp != std::wstring::npos && rp != std::wstring::npos && rp > lp + 1) {
                        durPart = res->message.substr(lp + 1, rp - lp - 1);
                    }
                    std::wstring sub = L"Klasörünüze kaydedildi";
                    if (!durPart.empty()) sub += L" • " + durPart;
                    ShowCornerToast(hInst_, L"Klip Kaydedildi", sub);
                } else {
                    if (app_.Config().soundNotify) PlayErrorTone();
                    ShowBalloon(L"Anlık Klip Hatası", res->message.c_str(), NIIF_WARNING);
                }
                if (mainWin_) mainWin_->NotifyClipSaved(*res);
                delete res;
            }
            UpdateTrayIcon();
            return 0;
        }

        case WM_APP_TRAY:
            if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
                ShowSettingsWindow(0);
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayMenu();
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_MENU_SAVE_CFG: TriggerSaveClip(0); break;
                case ID_MENU_SAVE_30S: TriggerSaveClip(30); break;
                case ID_MENU_SAVE_60S: TriggerSaveClip(60); break;
                case ID_MENU_TOGGLE_REC:
                    app_.ToggleBuffering();
                    UpdateTrayIcon();
                    if (mainWin_) mainWin_->RefreshLiveStats();
                    ShowCornerToast(hInst_, app_.IsRunning() ? L"Tampon Kayıt Aktif" : L"Kayıt Duraklatıldı",
                                    app_.IsRunning() ? L"Arka plan klip kaydı devam ediyor" : L"Klip kaydı geçici olarak durduruldu");
                    break;
                case ID_MENU_TOGGLE_MIC: {
                    bool muted = app_.ToggleMicMute();
                    UpdateTrayIcon();
                    if (mainWin_) mainWin_->RefreshLiveStats();
                    ShowCornerToast(hInst_, muted ? L"Mikrofon Susturuldu" : L"Mikrofon Açıldı",
                                    muted ? L"Kayıtlarda mikrofon sesi kapatıldı" : L"Mikrofon sesi yeniden aktif");
                    break;
                }
                case ID_MENU_SHOW_CLIPS: ShowSettingsWindow(0); break;
                case ID_MENU_SHOW_SETTINGS: ShowSettingsWindow(2); break;
                case ID_MENU_OPEN_FOLDER: OpenFolder(app_.Config().outputDir); break;
                case ID_MENU_EXIT: PostQuitMessage(0); break;
            }
            return 0;
    }
    return DefWindowProcW(trayHwnd_, msg, wParam, lParam);
}
