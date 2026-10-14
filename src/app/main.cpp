#include "pch.h"

#include "app/App.h"
#include "app/Messages.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "sys/Autostart.h"
#include "sys/Install.h"
#include "sys/TaskbarController.h"
#include "sys/Watchdog.h"

using namespace nb;

namespace {
bool HasArg(int argc, wchar_t** argv, const wchar_t* a) {
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], a) == 0) return true;
    return false;
}
const wchar_t* ArgValue(int argc, wchar_t** argv, const wchar_t* a) {
    for (int i = 1; i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], a) == 0) return argv[i + 1];
    return nullptr;
}
HWND FindRunning(bool preview) { return FindWindowW(preview ? kMainClassPreview : kMainClass, nullptr); }
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool preview = HasArg(argc, argv, L"--preview");
    paths::SetPreview(preview);

    // ---- watchdog mode: tiny, no UI, no COM ----
    if (const wchar_t* pid = ArgValue(argc, argv, L"--watchdog")) {
        log::Init(paths::LogDir(), L"watchdog.log");
        return sys::RunWatchdog(static_cast<DWORD>(_wtoi(pid)));
    }

    log::Init(paths::LogDir());
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    if (HasArg(argc, argv, L"--restore-taskbar")) {
        if (HWND w = FindRunning(false)) PostMessageW(w, WM_NB_RESTORE_TASKBAR, 0, 0);
        else sys::TaskbarController::RestoreFromState("command line");
        return 0;
    }
    if (HasArg(argc, argv, L"--exit")) {
        if (HWND w = FindRunning(preview)) PostMessageW(w, WM_CLOSE, 0, 0);
        return 0;
    }
    if (HasArg(argc, argv, L"--enable-autostart")) {
        std::wstring err;
        return sys::autostart::Enable(paths::ExePath(), &err) ? 0 : 1;
    }
    if (HasArg(argc, argv, L"--disable-autostart")) return sys::autostart::Disable() ? 0 : 1;
    if (HasArg(argc, argv, L"--install")) return sys::install::Install(HasArg(argc, argv, L"--quiet"));
    if (HasArg(argc, argv, L"--uninstall")) return sys::install::Uninstall(HasArg(argc, argv, L"--quiet"));

    // ---- single instance per user session (Local\ namespace is per session) ----
    HANDLE mutex = CreateMutexW(nullptr, TRUE, preview ? L"Local\\NotchBar.Instance.Preview" : L"Local\\NotchBar.Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindRunning(preview)) PostMessageW(w, WM_NB_SHOW_LAUNCHER, 0, 0);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    LOGI("NotchBar starting (%s%s)", preview ? "preview" : "normal", HasArg(argc, argv, L"--autostart") ? ", autostart" : "");
    Options o;
    o.preview = preview;
    o.fromAutostart = HasArg(argc, argv, L"--autostart");
    int rc = App::Get().Run(o);
    LOGI("NotchBar exited (%d)", rc);
    LocalFree(argv);
    if (mutex) CloseHandle(mutex);
    return rc;
}
