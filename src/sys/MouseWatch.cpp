#include "pch.h"

#include "sys/MouseWatch.h"

#include "core/Dispatcher.h"
#include "core/Log.h"

namespace nb::sys {
namespace {
LRESULT CALLBACK Proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION &&
        (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN || wp == WM_XBUTTONDOWN)) {
        POINT pt = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp)->pt;
        // Never work inside the hook: hand over and return at once.
        ui::Post([pt] {
            auto& w = MouseWatch::Get();
            if (w.onPress) w.onPress(pt);
        });
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
} // namespace

MouseWatch& MouseWatch::Get() {
    static MouseWatch w;
    return w;
}

void MouseWatch::SetActive(bool on) {
    if (on == (hook_ != nullptr)) return;
    if (on) {
        hook_ = SetWindowsHookExW(WH_MOUSE_LL, Proc, GetModuleHandleW(nullptr), 0);
        if (!hook_) LOGW("mouse watch hook failed (%lu)", GetLastError());
    } else {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
}

} // namespace nb::sys
