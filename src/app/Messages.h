#pragma once
#include <windows.h>

namespace nb {

constexpr const wchar_t* kMainClass = L"NotchBar.Main";
constexpr const wchar_t* kMainClassPreview = L"NotchBar.Main.Preview";

constexpr UINT WM_NB_WINKEY = WM_APP + 20;          // lone Windows key tapped
constexpr UINT WM_NB_SHOW_LAUNCHER = WM_APP + 21;   // second instance asked us to show up
constexpr UINT WM_NB_RESTORE_TASKBAR = WM_APP + 22; // "--restore-taskbar" while running

} // namespace nb
