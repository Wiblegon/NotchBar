#pragma once
// Default render endpoint volume/mute via Windows Core Audio. Fully notification-driven:
// IAudioEndpointVolumeCallback (level/mute from any app) + IMMNotificationClient (default device).
#include <functional>
#include <string>

namespace nb::sys {

struct AudioState {
    bool available = false; // false: no output device
    float level = 0;        // 0..1 scalar (as shown by Windows)
    bool muted = false;
    std::wstring deviceName;
};

class AudioMonitor {
public:
    static AudioMonitor& Get();
    void Start();
    void Stop();
    const AudioState& State() const { return state_; }
    void SetLevel(float level);
    void SetMuted(bool muted);
    void Subscribe(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }

    // internal (called on the UI thread)
    void Rebind();
    void Apply(float level, bool muted);

private:
    AudioState state_;
    std::vector<std::function<void()>> listeners_;
};

} // namespace nb::sys
