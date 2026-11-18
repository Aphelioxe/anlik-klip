#include "com_ptr.h"
#include "config.h"
#include "recorder_app.h"
#include "tray_ui.h"

#include <mfapi.h>
#include <cstring>

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    const bool isRestarted = (cmdLine && wcsstr(cmdLine, L"--restarted") != nullptr);
    HANDLE mutex = nullptr;
    for (int attempt = 0; attempt < (isRestarted ? 25 : 1); ++attempt) {
        mutex = CreateMutexW(nullptr, FALSE, L"Local\\AnlikKlipSingleInstanceMutex");
        if (mutex && GetLastError() != ERROR_ALREADY_EXISTS) break;
        if (isRestarted && attempt < 24) {
            if (mutex) { CloseHandle(mutex); mutex = nullptr; }
            Sleep(100);
            continue;
        }
        HWND existing = FindWindowW(L"AnlikKlipMainWnd", nullptr);
        if (existing) {
            PostMessageW(existing, WM_APP_SHOW_UI, 0, 0);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);

    const std::wstring exeDir = GetExeDirectory();
    Logger::Init(exeDir + L"\\diag.log");
    Logger::Log("AnlikKlip starting");

    const bool startMinimized = (cmdLine && wcsstr(cmdLine, L"--minimized") != nullptr);

    RecorderApp app(exeDir + L"\\settings.ini");
    TrayUI ui(hInst, app);

    if (!ui.Create(!startMinimized)) {
        Logger::Log("TrayUI::Create failed");
        MFShutdown();
        CoUninitialize();
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    app.StartBuffering();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (ui.HandlePreTranslateMessage(msg)) {
            continue;
        }
        if (ui.MainHwnd() && IsWindowVisible(ui.MainHwnd()) && IsDialogMessageW(ui.MainHwnd(), &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ui.Destroy();
    app.StopBuffering();

    Logger::Log("AnlikKlip exiting cleanly");
    MFShutdown();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
