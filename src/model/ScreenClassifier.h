#pragma once
// Pure classification of top-level windows into desktop / maximized / fullscreen.
// Fullscreen is NOT "window as big as the screen" alone: maximized state, caption/frame
// styles, monitor bounds, visibility and cloaking are combined.
#include <vector>

#include "model/VisibilityModel.h"

namespace nb {

struct RectI {
    int left = 0, top = 0, right = 0, bottom = 0;
    int width() const { return right - left; }
    int height() const { return bottom - top; }
};

struct WindowFacts {
    bool visible = true;
    bool minimized = false;
    bool cloaked = false;       // DWM cloaked: other virtual desktop, suspended UWP, ...
    bool zoomed = false;        // IsZoomed: maximized through the caption button / Win+Up
    bool hasCaption = false;    // (style & WS_CAPTION) == WS_CAPTION
    bool hasThickFrame = false; // WS_THICKFRAME
    bool toolWindow = false;    // WS_EX_TOOLWINDOW without WS_EX_APPWINDOW
    bool clickThrough = false;  // WS_EX_TRANSPARENT or WS_EX_NOACTIVATE overlays (game/GPU overlays)
    bool shellWindow = false;   // desktop (Progman/WorkerW), taskbars, our own windows
    RectI frame;                // DWM extended frame bounds (visible frame), physical px
};

enum class WindowClass { Ignore, Normal, Maximized, Fullscreen };

WindowClass ClassifyWindow(const WindowFacts& w, const RectI& monitor);

// `topToBottom` are the classes of windows intersecting the monitor in z-order.
// `exclusiveHint` = shell reports D3D exclusive fullscreen / presentation mode.
ScreenMode ModeFromZOrder(const std::vector<WindowClass>& topToBottom, bool exclusiveHint);

} // namespace nb
