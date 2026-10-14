#include "pch.h"

#include "sys/PowerMonitor.h"

#include "core/Log.h"

namespace nb::sys {

PowerMonitor& PowerMonitor::Get() {
    static PowerMonitor p;
    return p;
}

void PowerMonitor::Start(HWND w) {
    notifyPct_ = RegisterPowerSettingNotification(w, &GUID_BATTERY_PERCENTAGE_REMAINING, DEVICE_NOTIFY_WINDOW_HANDLE);
    notifyAc_ = RegisterPowerSettingNotification(w, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
    Refresh();
    LOGI("power: system battery %s", system_ ? "present" : "absent");
}

void PowerMonitor::Stop() {
    if (notifyPct_) UnregisterPowerSettingNotification(notifyPct_);
    if (notifyAc_) UnregisterPowerSettingNotification(notifyAc_);
    notifyPct_ = notifyAc_ = nullptr;
}

void PowerMonitor::Refresh() {
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s)) return;
    auto d = battery::FromSystemPower(s.ACLineStatus, s.BatteryFlag, s.BatteryLifePercent);
    if (d) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        d->updatedMs = static_cast<int64_t>(((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000 -
                                            11644473600000ULL);
    }
    bool changed = d.has_value() != system_.has_value() ||
                   (d && system_ && (d->percent != system_->percent || d->charge != system_->charge));
    system_ = d;
    if (changed)
        for (auto& l : listeners_) l();
}

} // namespace nb::sys
