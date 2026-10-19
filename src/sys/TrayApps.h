#pragma once
// Background apps that own a notification-area (tray) icon and are running right now.
// Windows has no API to enumerate tray icons themselves; the per-app list Windows keeps for its
// "Other system tray icons" settings (HKCU\Control Panel\NotifyIconSettings) is matched with the
// running processes instead. Activating an entry brings the app's window up.
#include <string>
#include <vector>

namespace nb::sys {

struct TrayApp {
    std::wstring name;
    std::wstring exePath;
};

std::vector<TrayApp> ListRunningTrayApps();
void ActivateTrayApp(const TrayApp& app);
// Ends every process of the app (matched by executable; by the windows' processes when the path is
// unknown). Explorer is never killed - its windows are closed instead.
void TerminateApp(const std::wstring& exePath, const std::vector<HWND>& windows = {});

} // namespace nb::sys
