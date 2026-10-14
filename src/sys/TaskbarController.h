#pragma once
// Hides the Windows taskbar while our UI is up, and always gives it back.
//
// "Hide" here = (1) switch the taskbar to auto-hide through the documented appbar API so it
// releases its reserved work area (maximized windows reach the bottom edge), then (2) hide the
// taskbar windows. Nothing is disabled, killed, patched or deleted: Explorer keeps running and
// the taskbar comes back with SW_SHOW + restoring the user's original auto-hide setting.
//
// Everything that depends on Explorer's internal window structure (class names) lives in the
// small TaskbarAdapter below so it can be swapped if a Windows release changes it.
#include <windows.h>

#include <string>
#include <vector>

namespace nb::sys {

namespace TaskbarAdapter {
// Windows 10 / 11 (verified on 11 24H2/25H2): primary "Shell_TrayWnd",
// one "Shell_SecondaryTrayWnd" per additional monitor.
std::vector<HWND> FindTaskbars();
bool IsTaskbarClass(const wchar_t* cls);
HWND Primary();
} // namespace TaskbarAdapter

struct TaskbarState {
    bool hidden = false;          // we changed something and have not given it back yet
    bool originalAutoHide = false;
    bool sessionEnding = false;   // set at logoff: the watchdog must not "repair" a normal logoff
    DWORD pid = 0;
};

class TaskbarController {
public:
    static TaskbarController& Get();

    bool Hide();
    void Restore();
    bool IsHidden() const { return hidden_; }

    // Explorer restarted: TaskbarCreated was broadcast. Re-apply once our UI is ready.
    void OnTaskbarCreated();
    // A taskbar window was shown again (Explorer sometimes re-shows it): hide it again.
    void OnTaskbarWindowShown(HWND hwnd);
    void MarkSessionEnding();

    static UINT TaskbarCreatedMessage();

    // Used by the watchdog, startup recovery and "--restore-taskbar".
    static TaskbarState ReadState();
    static bool WriteState(const TaskbarState& s);
    static bool RestoreFromState(const char* reason);
    static bool GetAutoHide();
    static void SetAutoHide(bool on);

private:
    void HideWindows();
    bool hidden_ = false;
    uint64_t reapplyTimer_ = 0;
};

} // namespace nb::sys
