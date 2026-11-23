#include "com_ptr.h"
#include "config.h"
#include "recorder_app.h"
#include "tray_ui.h"

#include <mfapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <cstring>

namespace {

void EnsureDesktopShortcutOnce() {
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return;

    HKEY hk = nullptr;
    wchar_t prevExe[MAX_PATH] = {};
    DWORD sz = sizeof(prevExe);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AnlikKlip", 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hk, L"DesktopShortcutExe", nullptr, nullptr, reinterpret_cast<LPBYTE>(prevExe), &sz) == ERROR_SUCCESS &&
            _wcsicmp(prevExe, exePath) == 0) {
            RegCloseKey(hk);
            return;
        }
        RegCloseKey(hk);
    }

    PWSTR deskDir = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &deskDir)) || !deskDir) return;
    std::wstring lnkPath = std::wstring(deskDir) + L"\\Anlık Klip.lnk";
    CoTaskMemFree(deskDir);

    std::wstring exeDir = exePath;
    size_t sl = exeDir.find_last_of(L"\\/");
    if (sl != std::wstring::npos) exeDir.resize(sl);

    ComPtr<IShellLinkW> psl;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, psl.PutVoid())) && psl) {
        psl->SetPath(exePath);
        psl->SetWorkingDirectory(exeDir.c_str());
        psl->SetIconLocation(exePath, 0);
        psl->SetDescription(L"Anlık Klip - Oyun Klip Kaydedici");

        ComPtr<IPersistFile> ppf;
        if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, ppf.PutVoid())) && ppf) {
            if (SUCCEEDED(ppf->Save(lnkPath.c_str(), TRUE))) {
                HKEY hkW = nullptr;
                if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AnlikKlip", 0, nullptr, 0, KEY_WRITE, nullptr, &hkW, nullptr) == ERROR_SUCCESS) {
                    RegSetValueExW(hkW, L"DesktopShortcutExe", 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(exePath),
                                   static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(wchar_t)));
                    RegCloseKey(hkW);
                }
            }
        }
    }
}

} // namespace

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
    EnsureDesktopShortcutOnce();
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
