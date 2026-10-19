#pragma once
#include <functional>
#include <string>

#include "gfx/Anim.h"
#include "gfx/Hover.h"
#include "gfx/Surface.h"
#include "ui/EdgeController.h"

namespace nb::ui {

enum class NotchButton { None, Media, Volume, Network, Clock, Battery, Notifications, Power, Mic, Camera, Tray, Quick };

class NotchWindow : public gfx::Surface {
public:
    bool Init();
    void Layout();                      // monitor/DPI/scale changed
    void SetMode(ScreenMode m) { edge_.SetMode(m); }
    void SetPinned(bool pinned) { edge_.SetPinned(pinned); }
    void SetTimings(EdgeTimings t) { edge_.SetTimings(t); }
    void SetClockPill(bool on);         // collapsed form = small clock instead of a thin handle
    void RefreshClock();                // time/locale changed, resume
    void Changed();                     // a data source changed (may add/remove items and resize)

    // Screen-space anchor (bottom center, physical px) of a button, for popups.
    POINT AnchorOf(NotchButton b) const;
    int BottomPx() const;               // bottom of the normal notch
    std::function<void(NotchButton)> onClick;
    std::function<void()> onBlankClick;   // pressed on the notch but not on a button
    std::function<void(POINT)> onContext; // right click anywhere on the notch (screen px)

protected:
    void Paint(ID2D1DeviceContext* dc) override;
    bool Animate(float dt) override;
    void OnMouseMove(PointF p) override;
    void OnMouseLeave() override;
    void OnMouseDown(PointF p, int button) override;
    void OnMouseUp(PointF p, int button) override;
    void OnWheel(PointF p, int delta) override;
    void OnDpiChanged() override { Layout(); }

private:
    void ApplyPresence(EdgePresence p, bool animate);
    void UpdateRegion();
    float BodyX() const;
    float TargetWidth() const;          // depends on which items are shown right now
    bool MediaShown() const;
    int PrivacyCount() const;
    D2D1_RECT_F SlotRect(NotchButton b) const;
    NotchButton HitTest(PointF p) const;
    std::wstring TooltipFor(NotchButton b) const;
    void ScheduleClock();

    EdgeController edge_;
    gfx::HoverBank anim_; // hover / press amounts per button
    gfx::Spring w_{0}, h_{0}, content_{1};
    float winW_ = 0, winH_ = 0;
    bool pill_ = true;
    NotchButton hot_ = NotchButton::None, pressed_ = NotchButton::None;
    std::wstring clock_, date_;
    TimerId clockTimer_ = 0;
};

} // namespace nb::ui
