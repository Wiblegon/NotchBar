#include "pch.h"

#include "sys/battery/BatteryService.h"

#include "core/Dispatcher.h"
#include "core/Log.h"
#include "sys/PowerMonitor.h"

namespace nb::sys {

int64_t UnixNowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 10000) - 11644473600000LL;
}

BatteryService& BatteryService::Get() {
    static BatteryService b;
    return b;
}

void BatteryService::Start() {
    if (started_) return;
    started_ = true;
    providers_.clear();
    providers_.push_back(MakePnpProvider());
    providers_.push_back(MakeBleProvider());
    providers_.push_back(MakeHyperXProvider());
    providers_.push_back(MakeRawmProvider());
    providers_.push_back(MakeCustomProvider());
    for (auto& p : providers_) {
        try {
            p->Start();
            p->SetPaused(paused_);
        } catch (...) {
            LOGE("battery provider %s failed to start", p->Id());
        }
    }
    static bool subscribed = false;
    if (!subscribed) {
        subscribed = true;
        PowerMonitor::Get().Subscribe([this] { Notify(); });
    }
    ArmIdle();
    LOGI("battery service started (%zu providers)", providers_.size());
}

void BatteryService::Stop() {
    if (!started_) return;
    started_ = false;
    ui::ClearTimeout(menuTimer_);
    ui::ClearTimeout(idleTimer_);
    for (auto& p : providers_) p->Stop();
    providers_.clear();
    data_.clear();
    Notify();
}

void BatteryService::ArmIdle() {
    // Menu closed: devices without notifications are re-read rarely (providers rate-limit).
    ui::ClearTimeout(idleTimer_);
    idleTimer_ = ui::SetTimeout(15 * 60 * 1000, [this] {
        idleTimer_ = 0;
        if (!started_) return;
        if (!paused_)
            for (auto& p : providers_) p->RequestRefresh();
        ArmIdle();
    });
}

void BatteryService::SetPaused(bool paused) {
    paused_ = paused;
    for (auto& p : providers_) p->SetPaused(paused);
}

void BatteryService::SetMenuOpen(bool open) {
    menuOpen_ = open;
    ui::ClearTimeout(menuTimer_);
    if (!open || !started_) return;
    for (auto& p : providers_) p->RequestRefresh();
    // While the menu is open: gentle refresh once a minute (providers rate-limit themselves).
    menuTimer_ = ui::SetTimeout(60000, [this] {
        menuTimer_ = 0;
        if (menuOpen_) SetMenuOpen(true);
    });
}

void BatteryService::RefreshCustom() {
    for (auto& p : providers_)
        if (std::string(p->Id()) == "custom") p->RequestRefresh();
}

void BatteryService::Report(const std::string& providerId, std::vector<battery::DeviceBattery> devices) {
    ui::Post([this, providerId, devices = std::move(devices)]() mutable {
        if (!started_) return;
        for (auto& [id, list] : data_)
            if (id == providerId) {
                list = std::move(devices);
                Notify();
                return;
            }
        data_.push_back({providerId, std::move(devices)});
        Notify();
    });
}

std::vector<battery::DeviceBattery> BatteryService::Devices() const {
    std::vector<battery::DeviceBattery> out;
    if (const auto& s = PowerMonitor::Get().System()) out.push_back(*s);
    std::vector<battery::DeviceBattery> peri;
    for (const auto& [id, list] : data_)
        for (const auto& d : list) {
            // De-duplicate the same physical device reported by two providers (same container).
            auto dup = d.containerId.empty() ? peri.end()
                                             : std::find_if(peri.begin(), peri.end(), [&](const auto& x) { return x.containerId == d.containerId; });
            if (dup == peri.end()) peri.push_back(d);
            else if (!dup->percent && d.percent) *dup = d;
        }
    std::sort(peri.begin(), peri.end(), [](const auto& a, const auto& b) {
        if (a.connected != b.connected) return a.connected;
        return a.name < b.name;
    });
    out.insert(out.end(), peri.begin(), peri.end());
    return out;
}

std::vector<ProviderInfo> BatteryService::Providers() const {
    std::vector<ProviderInfo> r;
    for (const auto& p : providers_) r.push_back(p->Info());
    return r;
}

void BatteryService::Notify() {
    for (auto& l : listeners_) l();
}

} // namespace nb::sys
