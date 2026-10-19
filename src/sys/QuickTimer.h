#pragma once
// One quick countdown at a time: a plain timer or a focus session (no Windows notification
// toasts while it runs). Lives in the UI thread; the end is a single one-shot timeout.
#include <functional>

#include "core/Dispatcher.h"

namespace nb::sys {

enum class TimerKind { None, Timer, Focus };

class QuickTimer {
public:
    static QuickTimer& Get();
    void Start(TimerKind kind, int minutes);
    void Cancel();
    TimerKind Kind() const { return kind_; }
    bool Running() const { return kind_ != TimerKind::None; }
    TimeMs RemainingMs() const;
    TimeMs TotalMs() const { return totalMs_; }
    // Called after the countdown ran out (the timer is already idle then).
    std::function<void(TimerKind)> onFinished;
    std::function<void()> onChanged; // started / cancelled / finished

private:
    TimerKind kind_ = TimerKind::None;
    TimeMs endMs_ = 0, totalMs_ = 0;
    ui::TimerId timeout_ = 0;
};

} // namespace nb::sys
