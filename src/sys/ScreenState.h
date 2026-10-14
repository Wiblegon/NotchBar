#pragma once
// Decides Desktop / Maximized / Fullscreen for the monitor that hosts our UI.
// Re-evaluated only on window events (foreground, move/size end, min/max, show/hide,
// location changes of the foreground process), coalesced with a short delay.
#include <windows.h>

#include <functional>

#include "model/VisibilityModel.h"

namespace nb::sys {

class ScreenState {
public:
    static ScreenState& Get();

    void Start();
    void SetMonitor(HMONITOR m);
    void Schedule(int delayMs = 40);
    void EvaluateNow();

    ScreenMode Mode() const { return mode_; }
    void OnChanged(std::function<void(ScreenMode)> fn) { listener_ = std::move(fn); }

    // Diagnostics for tests/logging: class of the deciding window.
    const std::wstring& LastReason() const { return reason_; }

private:
    HMONITOR monitor_ = nullptr;
    ScreenMode mode_ = ScreenMode::Desktop;
    uint64_t timer_ = 0;
    std::function<void(ScreenMode)> listener_;
    std::wstring reason_;
};

} // namespace nb::sys
