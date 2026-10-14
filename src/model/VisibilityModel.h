#pragma once
// Pure state machines for "how visible is each edge part" and "is the hover panel open".
// No Win32 here: time is passed in explicitly so the logic is unit-testable.
#include <cstdint>
#include <optional>

namespace nb {

using TimeMs = int64_t;

enum class ScreenMode {
    Desktop,    // desktop or normal-sized windows
    Maximized,  // a maximized (caption-button) window owns the monitor
    Fullscreen, // game / video / F11 / borderless fullscreen
};

enum class EdgePresence {
    Hidden,    // not shown at all, no hit testing, no rendering
    Collapsed, // thin handle (maximized mode)
    Normal,    // regular compact form (also the hover "peek" in maximized mode)
};

const char* ToString(ScreenMode m);
const char* ToString(EdgePresence p);

struct EdgeTimings {
    int expandDelayMs = 150;   // dwell before a collapsed part opens
    int collapseDelayMs = 700; // grace period after the pointer leaves
};

// One instance per edge part (top notch, bottom dock).
class EdgeBarModel {
public:
    explicit EdgeBarModel(EdgeTimings t = {}) : t_(t) {}

    void SetTimings(EdgeTimings t) { t_ = t; }
    void SetMode(ScreenMode m, TimeMs now);
    void PointerEnter(TimeMs now);
    void PointerLeave(TimeMs now);
    // While a popup that belongs to this part is open, it stays Normal.
    void SetPinned(bool pinned, TimeMs now);

    // Applies due timed transitions. Returns true if Presence() changed.
    bool Tick(TimeMs now);

    EdgePresence Presence() const { return presence_; }
    ScreenMode Mode() const { return mode_; }
    bool Hovered() const { return hovered_; }
    std::optional<TimeMs> NextDeadline() const { return deadline_; }

private:
    void Recompute(TimeMs now);

    EdgeTimings t_;
    ScreenMode mode_ = ScreenMode::Desktop;
    EdgePresence presence_ = EdgePresence::Normal;
    bool hovered_ = false;
    bool pinned_ = false;
    std::optional<TimeMs> deadline_;
    EdgePresence pendingTarget_ = EdgePresence::Normal;
};

enum class PanelDir { Below, Above };

struct HoverPanelTimings {
    int openDelayMs = 180;
    int closeDelayMs = 450;
};

// Hover-driven app panel next to the launcher search bar.
// The direction is decided when the pointer enters the bar and stays locked while the
// panel is open or closing, so moving into the panel never flips it.
class HoverPanelModel {
public:
    enum class State { Idle, OpenPending, Open, ClosePending };

    explicit HoverPanelModel(HoverPanelTimings t = {}) : t_(t) {}
    void SetTimings(HoverPanelTimings t) { t_ = t; }

    void PointerEnterBar(PanelDir approachDir, TimeMs now);
    // Pointer is over the panel or the bridge gap between bar and panel.
    void PointerEnterPanel(TimeMs now);
    // Pointer is outside the bar, bridge and panel.
    void PointerLeaveAll(TimeMs now);
    void OpenNow(PanelDir dir); // keyboard
    void CloseNow();

    bool Tick(TimeMs now); // returns true if visibility changed

    bool IsOpen() const { return state_ == State::Open || state_ == State::ClosePending; }
    State GetState() const { return state_; }
    PanelDir Dir() const { return dir_; }
    std::optional<TimeMs> NextDeadline() const { return deadline_; }

private:
    HoverPanelTimings t_;
    State state_ = State::Idle;
    PanelDir dir_ = PanelDir::Below;
    std::optional<TimeMs> deadline_;
};

} // namespace nb
