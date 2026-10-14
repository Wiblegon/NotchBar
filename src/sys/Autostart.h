#pragma once
// Start at user logon.
// Primary: a per-user Task Scheduler task with a logon trigger (no delay), interactive token,
// least privilege, normal priority (4 - the scheduler default 7 would be *below normal*), no
// battery/network/idle conditions and no execution time limit. Created without elevation.
// Fallback: HKCU\...\Run (subject to Explorer's startup-app delay; documented limitation).
#include <string>

namespace nb::sys::autostart {

enum class Method { None, ScheduledTask, RunKey };

Method Current();
bool Enable(const std::wstring& exePath, std::wstring* error = nullptr);
bool Disable();             // removes only this app's task / Run value
std::wstring TaskName();    // "NotchBar (<user>)"
const wchar_t* MethodName(Method m);

} // namespace nb::sys::autostart
