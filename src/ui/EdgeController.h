#pragma once
// Drives an EdgeBarModel with UI timers and reports presence changes.
#include <functional>

#include "core/Dispatcher.h"
#include "model/VisibilityModel.h"

namespace nb::ui {

class EdgeController {
public:
    std::function<void(EdgePresence)> onPresence;

    EdgeBarModel& Model() { return model_; }
    void SetTimings(EdgeTimings t) { model_.SetTimings(t); }

    void SetMode(ScreenMode m) { Apply([&] { model_.SetMode(m, NowMs()); }); }
    void PointerEnter() { Apply([&] { model_.PointerEnter(NowMs()); }); }
    void PointerLeave() { Apply([&] { model_.PointerLeave(NowMs()); }); }
    void SetPinned(bool p) { Apply([&] { model_.SetPinned(p, NowMs()); }); }
    EdgePresence Presence() const { return model_.Presence(); }

private:
    template <class F>
    void Apply(F&& f) {
        EdgePresence before = model_.Presence();
        f();
        Arm();
        if (model_.Presence() != before && onPresence) onPresence(model_.Presence());
    }
    void Arm() {
        ClearTimeout(timer_);
        if (auto d = model_.NextDeadline()) {
            int ms = static_cast<int>(std::max<TimeMs>(1, *d - NowMs()));
            timer_ = SetTimeout(ms, [this] {
                timer_ = 0;
                Apply([&] { model_.Tick(NowMs()); });
            });
        }
    }
    EdgeBarModel model_;
    TimerId timer_ = 0;
};

} // namespace nb::ui
