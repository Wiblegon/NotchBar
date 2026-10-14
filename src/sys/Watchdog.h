#pragma once
// Crash recovery: a tiny second process ("NotchBar.exe --watchdog <pid>") blocks on the main
// process handle (no polling). If the main process ends while the taskbar is still recorded as
// hidden, the watchdog restores it. A normal exit clears the record first, so it does nothing.
#include <windows.h>

namespace nb::sys {

int RunWatchdog(DWORD mainPid); // entry point of the watchdog mode

class Watchdog {
public:
    static Watchdog& Get();
    void Start();   // spawns the helper; respawns it (bounded) if it dies
    void Stop();    // normal exit: lets the helper finish on its own

private:
    static void CALLBACK OnHelperExit(void* ctx, BOOLEAN);
    HANDLE process_ = nullptr;
    HANDLE wait_ = nullptr;
    int respawns_ = 0;
    bool stopping_ = false;
};

} // namespace nb::sys
