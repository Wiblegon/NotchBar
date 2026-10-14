#pragma once
#include <string>

namespace nb::paths {

// Preview mode uses a separate data directory and never touches autostart/taskbar.
void SetPreview(bool preview);
bool IsPreview();

std::wstring DataDir();            // %LOCALAPPDATA%\NotchBar[\Preview]
std::wstring SharedDataDir();      // %LOCALAPPDATA%\NotchBar (taskbar recovery state is never per-mode)
std::wstring SettingsFile();
std::wstring FavoritesFile();
std::wstring AppCacheFile();
std::wstring TaskbarStateFile();
std::wstring LogDir();
std::wstring ExePath();
std::wstring InstallDir();         // %LOCALAPPDATA%\Programs\NotchBar

} // namespace nb::paths
