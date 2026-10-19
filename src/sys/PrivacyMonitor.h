#pragma once
// Is a camera or microphone in use right now? Read from Windows' own privacy ledger
// (HKCU\...\CapabilityAccessManager\ConsentStore): an app is using the device while its
// LastUsedTimeStart is set and LastUsedTimeStop is 0. A cheap 2 s registry check, only while visible.
#include <functional>
#include <string>
#include <vector>

namespace nb::sys {

struct PrivacyState {
    bool mic = false, camera = false;
    std::vector<std::wstring> micApps, cameraApps; // display names for the tooltip
};

class PrivacyMonitor {
public:
    static PrivacyMonitor& Get();
    void Start();
    void SetPaused(bool paused); // fullscreen / locked: no polling at all
    const PrivacyState& State() const { return state_; }
    void Subscribe(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }

private:
    void Poll();
    void Arm();
    PrivacyState state_;
    std::vector<std::function<void()>> listeners_;
    uint64_t timer_ = 0;
    bool started_ = false, paused_ = false;
};

} // namespace nb::sys
