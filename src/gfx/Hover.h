#pragma once
// Animated per-element amounts (hover, press, on/off state) so that every highlight, press and
// toggle fades/springs instead of jumping. Channels are independent; all values are 0..1.
#include <map>
#include <utility>

#include "gfx/Anim.h"

namespace nb::gfx {

class HoverBank {
public:
    enum Channel { Hover = 0, Press = 1, State = 2 };

    // Moves a channel of one element to `target`; the first use starts there (no fade-in from 0).
    void To(Channel ch, int id, float target) {
        auto key = std::make_pair(static_cast<int>(ch), id);
        auto it = springs_.find(key);
        if (it == springs_.end()) {
            it = springs_.emplace(key, FastSpring(target)).first;
            it->second.Snap(target);
            return;
        }
        it->second.To(target);
    }
    float Get(Channel ch, int id) const {
        auto it = springs_.find(std::make_pair(static_cast<int>(ch), id));
        if (it == springs_.end()) return 0;
        return it->second.value < 0 ? 0 : (it->second.value > 1.2f ? 1.2f : it->second.value);
    }
    // Exactly one element is hot: the previous one fades out, the new one fades in.
    void SetHot(int id) {
        if (id == hot_) return;
        if (hot_ != kNone) To(Hover, hot_, 0);
        hot_ = id;
        if (hot_ != kNone) To(Hover, hot_, 1);
    }
    void SetPressed(int id) {
        if (id == pressed_) return;
        if (pressed_ != kNone) To(Press, pressed_, 0);
        pressed_ = id;
        if (pressed_ != kNone) To(Press, pressed_, 1);
    }
    int Hot() const { return hot_; }
    // Advances everything; true while something still moves.
    bool Step(float dt) {
        bool more = false;
        for (auto& [k, s] : springs_) more |= s.Step(dt);
        return more;
    }
    void Clear() {
        springs_.clear();
        hot_ = pressed_ = kNone;
    }

    static constexpr int kNone = -1000000;

private:
    static Spring FastSpring(float v) { return Spring(v, 640, 46); } // quick, barely overshooting
    std::map<std::pair<int, int>, Spring> springs_;
    int hot_ = kNone, pressed_ = kNone;
};

} // namespace nb::gfx
