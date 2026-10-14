#pragma once
// Connection state from IP interface + connectivity-hint + WLAN notifications (no polling).
#include <functional>
#include <string>

namespace nb::sys {

enum class NetKind { None, Ethernet, Wifi, Cellular, Other };

struct NetState {
    NetKind kind = NetKind::None;
    bool internet = false;      // NLM reports internet connectivity
    int wifiQuality = -1;       // 0..100, -1 = not available (e.g. location permission on Windows 11 24H2+)
    std::wstring ssid;          // empty when Windows does not expose it to us
};

class NetworkMonitor {
public:
    static NetworkMonitor& Get();
    void Start();
    void Stop();
    const NetState& State() const { return state_; }
    void Subscribe(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }
    void ScheduleEvaluate(); // thread-safe
    static void OpenSystemUi();

private:
    void Apply(const NetState& s);
    NetState state_;
    std::vector<std::function<void()>> listeners_;
    std::atomic<bool> pending_{false};
};

} // namespace nb::sys
