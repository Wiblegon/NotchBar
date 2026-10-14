#pragma once
// Aggregates battery readings from independent providers. A provider failing or hanging never
// blocks the others or the UI: each runs on worker threads with timeouts and reports
// asynchronously; results are merged on the UI thread.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "model/Battery.h"

namespace nb::sys {

struct ProviderInfo {
    std::string id;
    std::wstring name;
    std::wstring status;   // e.g. "Etkin", "Cihaz bulunamadı", "Doğrulanmış yöntem yok"
    bool verified = false; // verified on real hardware during development
};

class BatteryProvider {
public:
    virtual ~BatteryProvider() = default;
    virtual const char* Id() const = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    // Hints: the battery menu opened (fresh data wanted) / paused (fullscreen, locked).
    virtual void RequestRefresh() {}
    virtual void SetPaused(bool) {}
    virtual ProviderInfo Info() const = 0;
};

class BatteryService {
public:
    static BatteryService& Get();
    void Start();
    void Stop();
    void SetPaused(bool paused);
    void SetMenuOpen(bool open);

    // Merged view: system battery first, then peripherals by name.
    std::vector<battery::DeviceBattery> Devices() const;
    std::vector<ProviderInfo> Providers() const;
    void Subscribe(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }

    // Providers call this (any thread) with the full current device list of that provider.
    void Report(const std::string& providerId, std::vector<battery::DeviceBattery> devices);

private:
    void Notify();
    void ArmIdle();
    std::vector<std::unique_ptr<BatteryProvider>> providers_;
    std::vector<std::pair<std::string, std::vector<battery::DeviceBattery>>> data_;
    std::vector<std::function<void()>> listeners_;
    bool menuOpen_ = false, paused_ = false, started_ = false;
    uint64_t menuTimer_ = 0, idleTimer_ = 0;
};

std::unique_ptr<BatteryProvider> MakePnpProvider();
std::unique_ptr<BatteryProvider> MakeBleProvider();
std::unique_ptr<BatteryProvider> MakeHyperXProvider();
std::unique_ptr<BatteryProvider> MakeRawmProvider();
int64_t UnixNowMs();

} // namespace nb::sys
