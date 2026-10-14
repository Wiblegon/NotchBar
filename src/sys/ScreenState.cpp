#include "pch.h"

#include "sys/ScreenState.h"

#include "core/Dispatcher.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "model/ScreenClassifier.h"
#include "sys/WindowTracker.h"

namespace nb::sys {

ScreenState& ScreenState::Get() {
    static ScreenState s;
    return s;
}

void ScreenState::Start() {
    WindowTracker::Get().Tap([this](DWORD ev, HWND) {
        switch (ev) {
        case EVENT_SYSTEM_FOREGROUND:
        case EVENT_SYSTEM_MINIMIZESTART:
        case EVENT_SYSTEM_MINIMIZEEND:
        case EVENT_SYSTEM_MOVESIZEEND:
        case EVENT_OBJECT_SHOW:
        case EVENT_OBJECT_HIDE:
        case EVENT_OBJECT_DESTROY:
        case EVENT_OBJECT_CLOAKED:
        case EVENT_OBJECT_UNCLOAKED: Schedule(30); break;
        case EVENT_OBJECT_LOCATIONCHANGE: Schedule(60); break;
        }
    });
    EvaluateNow();
}

void ScreenState::SetMonitor(HMONITOR m) {
    monitor_ = m;
    Schedule(0);
}

void ScreenState::Schedule(int delayMs) {
    if (timer_) return; // already pending: coalesce
    timer_ = ui::SetTimeout(delayMs, [this] {
        timer_ = 0;
        EvaluateNow();
    });
}

void ScreenState::EvaluateNow() {
    if (!monitor_) monitor_ = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(monitor_, &mi)) {
        monitor_ = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(monitor_, &mi);
    }
    RectI mon{mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom};

    QUERY_USER_NOTIFICATION_STATE quns{};
    bool exclusive = false;
    if (SUCCEEDED(SHQueryUserNotificationState(&quns)))
        exclusive = quns == QUNS_RUNNING_D3D_FULL_SCREEN || quns == QUNS_PRESENTATION_MODE;
    // The D3D hint is system-wide; only trust it for our monitor when the foreground window is there.
    if (exclusive) {
        HWND fg = GetForegroundWindow();
        if (!fg || MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) != monitor_) exclusive = false;
    }

    std::vector<WindowClass> classes;
    std::wstring decider;
    DWORD self = GetCurrentProcessId();
    int scanned = 0;
    for (HWND h = GetTopWindow(nullptr); h && scanned < 400; h = GetWindow(h, GW_HWNDNEXT), ++scanned) {
        if (!IsWindowVisible(h)) continue;
        WindowFacts f;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        wchar_t cls[64];
        GetClassNameW(h, cls, 64);
        std::wstring_view c = cls;
        f.shellWindow = pid == self || c == L"Progman" || c == L"WorkerW" || c == L"Shell_TrayWnd" ||
                        c == L"Shell_SecondaryTrayWnd";
        LONG style = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
        f.minimized = IsIconic(h);
        f.zoomed = IsZoomed(h);
        f.hasCaption = (style & WS_CAPTION) == WS_CAPTION;
        f.hasThickFrame = (style & WS_THICKFRAME) != 0;
        f.toolWindow = (ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW);
        f.clickThrough = (ex & WS_EX_TRANSPARENT) || ((ex & WS_EX_NOACTIVATE) && !(ex & WS_EX_APPWINDOW));
        DWORD cloaked = 0;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
        f.cloaked = cloaked != 0;
        RECT r{};
        if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(h, &r);
        f.frame = {r.left, r.top, r.right, r.bottom};
        WindowClass wc = ClassifyWindow(f, mon);
        if (wc == WindowClass::Ignore) continue;
        classes.push_back(wc);
        if (wc != WindowClass::Normal) {
            decider = c;
            break; // nothing below can change the result
        }
    }
    ScreenMode m = ModeFromZOrder(classes, exclusive);
    if (m != mode_) {
        mode_ = m;
        reason_ = exclusive ? L"d3d-exclusive" : decider;
        LOGI("screen mode -> %s (%s)", ToString(m), ToUtf8(reason_).c_str());
        if (listener_) listener_(m);
    }
}

} // namespace nb::sys
