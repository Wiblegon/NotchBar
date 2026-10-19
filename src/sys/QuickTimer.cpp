#include "pch.h"

#include "sys/QuickTimer.h"

namespace nb::sys {

QuickTimer& QuickTimer::Get() {
    static QuickTimer t;
    return t;
}

void QuickTimer::Start(TimerKind kind, int minutes) {
    ui::ClearTimeout(timeout_);
    minutes = std::clamp(minutes, 1, 600);
    kind_ = kind;
    totalMs_ = static_cast<TimeMs>(minutes) * 60 * 1000;
    endMs_ = ui::NowMs() + totalMs_;
    timeout_ = ui::SetTimeout(static_cast<int>(totalMs_), [this] {
        timeout_ = 0;
        TimerKind k = kind_;
        kind_ = TimerKind::None;
        if (onChanged) onChanged();
        if (onFinished) onFinished(k);
    });
    if (onChanged) onChanged();
}

void QuickTimer::Cancel() {
    if (!Running()) return;
    ui::ClearTimeout(timeout_);
    kind_ = TimerKind::None;
    if (onChanged) onChanged();
}

TimeMs QuickTimer::RemainingMs() const { return Running() ? std::max<TimeMs>(0, endMs_ - ui::NowMs()) : 0; }

} // namespace nb::sys
