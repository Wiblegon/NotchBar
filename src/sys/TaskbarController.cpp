#include "pch.h"

#include "sys/TaskbarController.h"

#include "core/AtomicFile.h"
#include "core/Dispatcher.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Paths.h"

namespace nb::sys {

// ---------------------------------------------------------------- adapter (Explorer internals)
namespace TaskbarAdapter {
bool IsTaskbarClass(const wchar_t* cls) {
    return wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

HWND Primary() { return FindWindowW(L"Shell_TrayWnd", nullptr); }

std::vector<HWND> FindTaskbars() {
    std::vector<HWND> out;
    if (HWND p = Primary()) out.push_back(p);
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) out.push_back(h);
    return out;
}
} // namespace TaskbarAdapter

// ---------------------------------------------------------------- state file
TaskbarState TaskbarController::ReadState() {
    TaskbarState s;
    auto text = file::Read(paths::TaskbarStateFile());
    if (!text) return s;
    auto v = json::Parse(*text);
    if (!v) return s;
    s.hidden = (*v)["hidden"].asBool();
    s.originalAutoHide = (*v)["originalAutoHide"].asBool();
    s.sessionEnding = (*v)["sessionEnding"].asBool();
    s.pid = static_cast<DWORD>((*v)["pid"].asNumber());
    return s;
}

bool TaskbarController::WriteState(const TaskbarState& s) {
    json::Object o;
    o["version"] = 1;
    o["hidden"] = s.hidden;
    o["originalAutoHide"] = s.originalAutoHide;
    o["sessionEnding"] = s.sessionEnding;
    o["pid"] = static_cast<double>(s.pid);
    return file::WriteAtomic(paths::TaskbarStateFile(), json::Serialize(json::Value(std::move(o))));
}

bool TaskbarController::GetAutoHide() {
    APPBARDATA abd{sizeof abd};
    abd.hWnd = TaskbarAdapter::Primary();
    return (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != 0;
}

void TaskbarController::SetAutoHide(bool on) {
    APPBARDATA abd{sizeof abd};
    abd.hWnd = TaskbarAdapter::Primary();
    abd.lParam = on ? ABS_AUTOHIDE : ABS_ALWAYSONTOP;
    SHAppBarMessage(ABM_SETSTATE, &abd);
}

bool TaskbarController::RestoreFromState(const char* reason) {
    TaskbarState s = ReadState();
    for (HWND h : TaskbarAdapter::FindTaskbars()) ShowWindow(h, SW_SHOWNA);
    if (s.hidden) {
        SetAutoHide(s.originalAutoHide);
        s.hidden = false;
        s.sessionEnding = false;
        WriteState(s);
        LOGI("taskbar restored (%s), auto-hide=%d", reason, s.originalAutoHide);
        return true;
    }
    LOGI("taskbar shown (%s), no saved change to undo", reason);
    return false;
}

// ---------------------------------------------------------------- controller
TaskbarController& TaskbarController::Get() {
    static TaskbarController c;
    return c;
}

UINT TaskbarController::TaskbarCreatedMessage() {
    static UINT msg = RegisterWindowMessageW(L"TaskbarCreated");
    return msg;
}

bool TaskbarController::Hide() {
    if (!TaskbarAdapter::Primary()) {
        LOGW("taskbar not found (Explorer not running?), nothing to hide");
        return false;
    }
    TaskbarState s = ReadState();
    if (!s.hidden) s.originalAutoHide = GetAutoHide(); // remember the user's setting once
    s.hidden = true;
    s.sessionEnding = false;
    s.pid = GetCurrentProcessId();
    // Persist BEFORE changing anything, so the watchdog/next start can always undo it.
    if (!WriteState(s)) {
        LOGE("cannot write taskbar state; refusing to hide the taskbar");
        return false;
    }
    if (!GetAutoHide()) SetAutoHide(true);
    HideWindows();
    hidden_ = true;
    LOGI("taskbar hidden (original auto-hide=%d)", s.originalAutoHide);
    return true;
}

void TaskbarController::HideWindows() {
    for (HWND h : TaskbarAdapter::FindTaskbars())
        if (IsWindowVisible(h)) ShowWindow(h, SW_HIDE);
}

void TaskbarController::Restore() {
    if (reapplyTimer_) ui::ClearTimeout(reapplyTimer_);
    RestoreFromState("restore");
    hidden_ = false;
}

void TaskbarController::OnTaskbarCreated() {
    if (!hidden_) return;
    LOGI("Explorer restarted (TaskbarCreated); re-applying taskbar hiding");
    if (reapplyTimer_) ui::ClearTimeout(reapplyTimer_);
    // Explorer shows the new taskbar shortly after broadcasting; re-apply a few times briefly.
    reapplyTimer_ = ui::SetTimeout(300, [this] {
        reapplyTimer_ = 0;
        if (!hidden_) return;
        if (!GetAutoHide()) SetAutoHide(true);
        HideWindows();
        reapplyTimer_ = ui::SetTimeout(1500, [this] {
            reapplyTimer_ = 0;
            if (hidden_) HideWindows();
        });
    });
}

void TaskbarController::OnTaskbarWindowShown(HWND h) {
    if (!hidden_) return;
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64) || !TaskbarAdapter::IsTaskbarClass(cls)) return;
    ui::Post([this, h] {
        if (hidden_ && IsWindowVisible(h)) ShowWindow(h, SW_HIDE);
    });
}

void TaskbarController::MarkSessionEnding() {
    if (!hidden_) return;
    TaskbarState s = ReadState();
    s.sessionEnding = true;
    WriteState(s);
}

} // namespace nb::sys
