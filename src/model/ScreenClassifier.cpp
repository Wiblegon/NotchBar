#include "model/ScreenClassifier.h"

namespace nb {

WindowClass ClassifyWindow(const WindowFacts& w, const RectI& m) {
    if (!w.visible || w.minimized || w.cloaked || w.shellWindow || w.toolWindow || w.clickThrough) return WindowClass::Ignore;
    if (w.frame.width() < 32 || w.frame.height() < 32) return WindowClass::Ignore;
    bool intersects = w.frame.left < m.right && w.frame.right > m.left && w.frame.top < m.bottom && w.frame.bottom > m.top;
    if (!intersects) return WindowClass::Ignore;

    const int tol = 1; // rounding of DWM frame bounds
    bool coversMonitor = w.frame.left <= m.left + tol && w.frame.top <= m.top + tol && w.frame.right >= m.right - tol &&
                         w.frame.bottom >= m.bottom - tol;
    bool framed = w.hasCaption || w.hasThickFrame;
    if (coversMonitor) {
        // A maximized, framed window can fill the monitor when the taskbar auto-hides:
        // it is still "maximized", not fullscreen. F11 / borderless / exclusive windows drop the frame
        // or are not maximized at all while covering the whole monitor.
        if (w.zoomed && framed && w.hasCaption) return WindowClass::Maximized;
        return WindowClass::Fullscreen;
    }
    if (w.zoomed) return WindowClass::Maximized;
    return WindowClass::Normal;
}

ScreenMode ModeFromZOrder(const std::vector<WindowClass>& topToBottom, bool exclusiveHint) {
    if (exclusiveHint) return ScreenMode::Fullscreen;
    bool first = true;
    for (WindowClass c : topToBottom) {
        if (c == WindowClass::Ignore) continue;
        if (first && c == WindowClass::Fullscreen) return ScreenMode::Fullscreen;
        first = false;
        // A maximized or fullscreen window below normal windows still covers the monitor.
        if (c == WindowClass::Maximized || c == WindowClass::Fullscreen) return ScreenMode::Maximized;
    }
    return ScreenMode::Desktop;
}

} // namespace nb
