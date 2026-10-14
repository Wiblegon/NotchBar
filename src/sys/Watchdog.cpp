#include "pch.h"

#include "sys/Watchdog.h"

#include "core/Dispatcher.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "sys/TaskbarController.h"

namespace nb::sys {

int RunWatchdog(DWORD mainPid) {
    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, mainPid);
    if (!p) {
        // Main process already gone: recover immediately if it left the taskbar hidden.
        TaskbarState s = TaskbarController::ReadState();
        if (s.hidden && s.pid == mainPid && !s.sessionEnding) TaskbarController::RestoreFromState("watchdog: main missing");
        return 0;
    }
    WaitForSingleObject(p, INFINITE); // event-based: zero CPU while waiting
    CloseHandle(p);
    TaskbarState s = TaskbarController::ReadState();
    if (s.hidden && s.pid == mainPid && !s.sessionEnding) {
        LOGW("watchdog: main process %lu ended without restoring the taskbar", mainPid);
        // Explorer may be restarting too; wait briefly for the taskbar window to exist.
        for (int i = 0; i < 20 && !TaskbarAdapter::Primary(); ++i) Sleep(250);
        TaskbarController::RestoreFromState("watchdog");
    }
    return 0;
}

Watchdog& Watchdog::Get() {
    static Watchdog w;
    return w;
}

void Watchdog::Start() {
    stopping_ = false;
    std::wstring exe = paths::ExePath();
    std::wstring cmd = L"\"" + exe + L"\" --watchdog " + std::to_wstring(GetCurrentProcessId());
    if (paths::IsPreview()) cmd += L" --preview";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | IDLE_PRIORITY_CLASS, nullptr,
                        nullptr, &si, &pi)) {
        LOGE("watchdog spawn failed (%lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    RegisterWaitForSingleObject(&wait_, process_, &Watchdog::OnHelperExit, this, INFINITE, WT_EXECUTEONLYONCE);
    LOGI("watchdog started (pid %lu)", pi.dwProcessId);
}

void CALLBACK Watchdog::OnHelperExit(void* ctx, BOOLEAN) {
    auto* self = static_cast<Watchdog*>(ctx);
    ui::Post([self] {
        if (self->stopping_) return;
        if (self->wait_) UnregisterWaitEx(self->wait_, nullptr);
        self->wait_ = nullptr;
        if (self->process_) CloseHandle(self->process_);
        self->process_ = nullptr;
        if (++self->respawns_ > 5) {
            LOGE("watchdog keeps exiting; not respawning again");
            return;
        }
        LOGW("watchdog exited unexpectedly, respawning");
        self->Start();
    });
}

void Watchdog::Stop() {
    stopping_ = true;
    if (wait_) UnregisterWaitEx(wait_, INVALID_HANDLE_VALUE);
    wait_ = nullptr;
    if (process_) CloseHandle(process_);
    process_ = nullptr;
}

} // namespace nb::sys
