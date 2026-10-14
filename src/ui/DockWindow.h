#pragma once
// Bottom app area: shows only open applications (no pinned icons), grouped by app identity.
#include <string>
#include <vector>

#include "gfx/Anim.h"
#include "gfx/Surface.h"
#include "sys/WindowTracker.h"
#include "ui/EdgeController.h"

namespace nb::ui {

class DockWindow : public gfx::Surface {
public:
    bool Init();
    void Layout();
    void SetMode(ScreenMode m) { edge_.SetMode(m); }
    void SetPinned(bool p) { edge_.SetPinned(p); }
    void SetTimings(EdgeTimings t) { edge_.SetTimings(t); }
    void Sync(); // window tracker changed

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
    struct Item {
        sys::AppGroup group;
        gfx::Spring x = gfx::SnappySpring(0), scale = gfx::SnappySpring(1), alpha = gfx::FadeSpring(1), lift = gfx::SnappySpring(0);
        bool removing = false;
    };
    void ApplyPresence(EdgePresence p, bool animate);
    void Retarget(bool snap);
    D2D1_RECT_F DockRect() const;
    int HitTest(PointF p) const;
    void ClickItem(int i);
    void MenuForItem(int i, POINT at);
    void UpdateRegion();
    float ContentWidth() const;

    EdgeController edge_;
    std::vector<Item> items_;
    gfx::Spring width_ = gfx::SoftSpring(0), expand_ = gfx::SoftSpring(1), scroll_ = gfx::SnappySpring(0);
    float winW_ = 0, winH_ = 0, maxContentW_ = 0;
    int hot_ = -1, pressed_ = -1;
};

} // namespace nb::ui
