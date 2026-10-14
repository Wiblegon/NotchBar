#pragma once
// A top-level, per-pixel-alpha window drawn with Direct2D into a DirectComposition surface.
// Rendering is on demand: Invalidate() schedules one frame; animations request frames only
// while they run. Hidden surfaces release their GPU surface and never render.
#include <windows.h>
#include <dcomp.h>
#include <d2d1_3.h>
#include <winrt/base.h>

#include <vector>

#include "model/PanelDirection.h"
#include "model/VisibilityModel.h"

namespace nb::gfx {

class Surface {
public:
    Surface() = default;
    virtual ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    // activatable=false: clicks never steal focus from the user's app (WS_EX_NOACTIVATE).
    bool Create(const wchar_t* accessibleName, bool activatable = false, HWND owner = nullptr);
    void Destroy();

    // Window rectangle in physical screen pixels (may be negative on secondary monitors).
    void Place(const RECT& screenRect);
    // Interactive/visible region relative to the window, in DIPs. Empty = whole window.
    void SetRegionDip(const RectF& r);
    void ClearRegion();

    void Show(bool visible);
    bool Visible() const { return visible_; }

    void Invalidate();
    void RequestAnimationFrame();

    HWND Hwnd() const { return hwnd_; }
    float Scale() const { return scale_; } // physical px per DIP (DPI scale * UI scale)
    static void SetGlobalUiScale(float s); // user "interface size" setting, applied to all surfaces
    RECT WindowRect() const { return rect_; }
    float WidthDip() const { return (rect_.right - rect_.left) / scale_; }
    float HeightDip() const { return (rect_.bottom - rect_.top) / scale_; }
    PointF ScreenToDip(POINT screen) const;
    POINT DipToScreen(PointF p) const;

    // Frame loop entry points (called by the App).
    static const std::vector<Surface*>& All();
    static bool AnyAnimating();
    static void RenderAll(TimeMs now); // advances animations, renders dirty surfaces, commits once
    static void RecreateAll();         // after device loss

protected:
    virtual void Paint(ID2D1DeviceContext* dc) = 0;
    // Advance animations by dt seconds; return true to keep receiving frames.
    virtual bool Animate(float dt) { return false; }
    virtual LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    virtual void OnMouseMove(PointF) {}
    virtual void OnMouseLeave() {}
    virtual void OnMouseDown(PointF, int /*button 0=L,1=R,2=M*/) {}
    virtual void OnMouseUp(PointF, int) {}
    virtual void OnWheel(PointF, int /*delta*/) {}
    virtual void OnDpiChanged() {}

    bool Hovered() const { return tracking_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    bool Render();
    void ReleaseSurface();
    void UpdateScale();

    HWND hwnd_ = nullptr;
    RECT rect_{};
    float scale_ = 1;
    bool visible_ = false, dirty_ = true, animating_ = false, tracking_ = false;
    winrt::com_ptr<IDCompositionTarget> target_;
    winrt::com_ptr<IDCompositionVisual2> visual_;
    winrt::com_ptr<IDCompositionSurface> surface_;
    SIZE surfaceSize_{};
    uint64_t generation_ = 0;
};

} // namespace nb::gfx
