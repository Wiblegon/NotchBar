#pragma once
// Computer battery via GetSystemPowerStatus, refreshed by power-setting notifications
// (battery percentage / AC-DC source) delivered to the message window. No polling.
#include <windows.h>

#include <functional>
#include <optional>

#include "model/Battery.h"

namespace nb::sys {

class PowerMonitor {
public:
    static PowerMonitor& Get();
    void Start(HWND messageWindow);
    void Stop();
    void Refresh(); // call on WM_POWERBROADCAST
    const std::optional<battery::DeviceBattery>& System() const { return system_; }
    bool HasSystemBattery() const { return system_.has_value(); }
    void Subscribe(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }

private:
    std::optional<battery::DeviceBattery> system_;
    HPOWERNOTIFY notifyPct_ = nullptr, notifyAc_ = nullptr;
    std::vector<std::function<void()>> listeners_;
};

} // namespace nb::sys
