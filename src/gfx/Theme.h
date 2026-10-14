#pragma once
// Visual language: matte graphite surfaces, soft corners, faint edge light, small shadows.
// All metrics are in DIPs (scaled by monitor DPI and the user's UI scale).
#include <d2d1.h>

namespace nb::theme {

inline D2D1_COLOR_F Rgba(int r, int g, int b, float a = 1.0f) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a); }

inline const D2D1_COLOR_F kSurface = Rgba(24, 25, 28, 1.0f);        // main graphite, matte (opaque)
inline const D2D1_COLOR_F kSurfaceRaised = Rgba(36, 37, 41, 1.0f);  // tiles, hovered rows
inline const D2D1_COLOR_F kSurfaceHover = Rgba(255, 255, 255, 0.07f);
inline const D2D1_COLOR_F kSurfacePressed = Rgba(255, 255, 255, 0.11f);
inline const D2D1_COLOR_F kEdge = Rgba(255, 255, 255, 0.075f);     // hairline border light
inline const D2D1_COLOR_F kText = Rgba(236, 236, 238);
inline const D2D1_COLOR_F kTextDim = Rgba(160, 161, 168);
inline const D2D1_COLOR_F kTextFaint = Rgba(110, 111, 118);
inline const D2D1_COLOR_F kAccent = Rgba(120, 170, 255);
inline const D2D1_COLOR_F kGood = Rgba(110, 210, 140);
inline const D2D1_COLOR_F kWarn = Rgba(245, 190, 90);
inline const D2D1_COLOR_F kBad = Rgba(240, 110, 100);
inline const D2D1_COLOR_F kFocus = Rgba(140, 180, 255, 0.9f);

// Notch (top)
constexpr float kNotchW = 300, kNotchH = 36;          // normal
constexpr float kNotchCollapsedW = 116, kNotchCollapsedH = 7;
constexpr float kNotchFlare = 9;                      // concave blend into the screen edge
constexpr float kNotchRadius = 16;                    // bottom corners
constexpr float kNotchSlot = 30;                      // button slot width

// Dock (bottom)
constexpr float kDockH = 48, kDockIcon = 30, kDockCell = 42, kDockPad = 7, kDockRadius = 16, kDockMargin = 6;
constexpr float kDockHandleW = 92, kDockHandleH = 5, kDockHandleMargin = 3;
constexpr float kDockMaxWidthFraction = 0.7f;

// Launcher
constexpr float kBarW = 400, kBarH = 44, kBarRadius = 14;
constexpr float kPanelH = 368, kPanelRadius = 16, kPanelGap = 8;

// Popups under the notch
constexpr float kPopupW = 300, kPopupRadius = 16, kPopupGap = 8;

// Shadow: small and controlled
constexpr float kShadowSize = 10;

} // namespace nb::theme
