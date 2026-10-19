#pragma once
// Everything the sound box needs beyond the default-output volume (AudioMonitor): the list of
// output / input devices and switching the default one, per-app volume of the default output,
// the default microphone's level and optional driver effects (bass, microphone boost).
// All calls are synchronous Core Audio calls made on the UI thread when the box is open or clicked.
#include <optional>
#include <string>
#include <vector>

namespace nb::sys {

struct AudioDevice {
    std::wstring id, name;
    bool isDefault = false;
};

struct AppVolume {
    std::wstring id;      // lower-case exe path, or L"system" for system sounds
    std::wstring name;
    std::wstring exePath; // for the icon; empty for system sounds
    float level = 1;
    bool muted = false;
};

struct CaptureState {
    bool available = false;
    float level = 0;
    bool muted = false;
    std::wstring name;
};

// A driver-provided effect slider (bass on outputs, boost on inputs); only present when the
// device's topology exposes it.
struct DeviceEffect {
    std::wstring name;
    float value = 0; // 0..1 of the driver's dB range
};

std::vector<AudioDevice> ListAudioDevices(bool capture);
bool SetDefaultAudioDevice(const std::wstring& id);

CaptureState GetCapture();
void SetCaptureLevel(float level);
void SetCaptureMuted(bool muted);

std::vector<AppVolume> ListAppVolumes();
void SetAppVolume(const std::wstring& id, float level);
void SetAppMuted(const std::wstring& id, bool muted);

std::optional<DeviceEffect> GetEffect(bool capture);
void SetEffect(bool capture, float value);

} // namespace nb::sys
