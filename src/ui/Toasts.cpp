#include "pch.h"

#include "ui/Toasts.h"

#include "app/App.h"
#include "core/Dispatcher.h"
#include "core/Strings.h"
#include "gfx/Anim.h"
#include "gfx/Draw.h"
#include "gfx/IconCache.h"
#include "gfx/Surface.h"
#include "gfx/Theme.h"
#include "ui/Popups.h"
#include "ui/Widgets.h"

namespace nb::ui {
using namespace theme;

namespace {
constexpr float kW = 340, kH = 64, kGapBetween = 6, kM = 10;

class ToastSurface : public gfx::Surface {
public:
    NotificationItem item;
    bool active = false;
    gfx::Spring appear = gfx::FadeSpring(0);
    std::function<void(uint64_t, bool)> onHover;
    std::function<void(uint64_t)> onDismiss;
    std::function<void()> onOpenCenter;

    void SetActive(bool a) {
        if (a == active) return;
        active = a;
        appear.To(a ? 1.0f : 0.0f);
        if (a) Show(true);
        RequestAnimationFrame();
    }

protected:
    D2D1_RECT_F Box() const { return {kM, kM, kM + kW, kM + kH}; }
    D2D1_RECT_F CloseBtn() const {
        auto b = Box();
        return {b.right - 30, b.top + 6, b.right - 6, b.top + 30};
    }
    void Paint(ID2D1DeviceContext* dc) override {
        float a = std::clamp(appear.value, 0.0f, 1.0f);
        if (a <= 0.001f) return;
        auto b = Box();
        D2D1_MATRIX_3X2_F base;
        dc->GetTransform(&base);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0, (1 - a) * -10) * base);
        draw::Panel(dc, b, 14, a);
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a),
                      nullptr);
        float tx = b.left + 14;
        ID2D1Bitmap* icon = item.appId.empty() ? nullptr
                                               : gfx::IconCache::Get().Get(L"app:" + item.appId, static_cast<int>(std::ceil(28 * Scale())));
        if (icon) {
            draw::Bitmap(dc, icon, {b.left + 12, b.top + 18, b.left + 40, b.top + 46});
            tx = b.left + 50;
        } else if (item.source == NotificationSource::Internal) {
            draw::Glyph(dc, draw::glyph::Info, {b.left + 26, b.top + 32}, 18, kTextDim);
            tx = b.left + 50;
        }
        std::wstring src = item.source == NotificationSource::Internal ? L"NotchBar" : (item.appName.empty() ? L"Bildirim" : item.appName);
        draw::Text(dc, src, {tx, b.top + 6, b.right - 34, b.top + 22}, 10.5f, kTextFaint);
        // Long texts are cut in a controlled way: one line each.
        draw::Text(dc, Ellipsize(item.title, 70), {tx, b.top + 21, b.right - 14, b.top + 40}, 13, kText, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        draw::Text(dc, Ellipsize(item.body, 110), {tx, b.top + 39, b.right - 14, b.top + 57}, 11.5f, kTextDim);
        if (hover_) draw::Glyph(dc, draw::glyph::Close, {(CloseBtn().left + CloseBtn().right) / 2, (CloseBtn().top + CloseBtn().bottom) / 2},
                                10, kTextDim);
        dc->PopLayer();
        dc->SetTransform(base);
    }
    bool Animate(float dt) override {
        bool more = appear.Step(dt);
        if (!active && appear.value <= 0.01f && !more) Show(false);
        return more;
    }
    void OnMouseMove(PointF) override {
        if (!hover_) {
            hover_ = true;
            if (onHover) onHover(item.key, true);
            Invalidate();
        }
    }
    void OnMouseLeave() override {
        hover_ = false;
        if (onHover) onHover(item.key, false);
        Invalidate();
    }
    void OnMouseUp(PointF p, int b) override {
        if (b != 0) return;
        if (PointIn(CloseBtn(), p)) {
            if (onDismiss) onDismiss(item.key); // visual only: the Windows notification stays
        } else if (onOpenCenter) onOpenCenter();
    }

private:
    bool hover_ = false;
};
} // namespace

struct ToastHost::Impl {
    ToastPolicy policy{2, 2, 5500};
    ToastSurface slots[2];
    TimerId timer = 0;

    void Sync() {
        const auto& vis = policy.VisibleItems();
        auto& sc = Screen::Current();
        int w = sc.Px(kW + 2 * kM), h = sc.Px(kH + 2 * kM);
        int cx = (sc.monitor.left + sc.monitor.right) / 2;
        int top = sc.monitor.top + sc.Px(kNotchH + kPopupGap) - sc.Px(kM);
        for (int i = 0; i < 2; ++i) {
            auto& s = slots[i];
            if (i < static_cast<int>(vis.size())) {
                bool changed = !s.active || s.item.key != vis[i].item.key;
                s.item = vis[i].item;
                int y = top + i * (sc.Px(kH + kGapBetween));
                s.Place({cx - w / 2, y, cx - w / 2 + w, y + h});
                auto box = RectF{kM, kM, kM + kW, kM + kH};
                s.SetRegionDip(box);
                if (changed) {
                    s.appear.Snap(gfx::ReducedMotion() ? 1.0f : 0.0f);
                    s.active = false;
                }
                s.SetActive(true);
                s.Invalidate();
            } else {
                s.SetActive(false);
            }
        }
        Arm();
    }
    void Arm() {
        ClearTimeout(timer);
        if (auto d = policy.NextDeadline()) {
            timer = SetTimeout(static_cast<int>(std::max<TimeMs>(1, *d - NowMs())), [this] {
                timer = 0;
                if (policy.Tick(NowMs())) Sync();
                else Arm();
            });
        }
    }
};

ToastHost::ToastHost() : impl_(std::make_unique<Impl>()) {}
ToastHost::~ToastHost() = default;

void ToastHost::Init() {
    for (auto& s : impl_->slots) {
        s.Create(L"NotchBar bildirim");
        s.onHover = [this](uint64_t key, bool h) {
            impl_->policy.SetHovered(key, h, NowMs());
            impl_->Arm();
        };
        s.onDismiss = [this](uint64_t key) {
            impl_->policy.Dismiss(key);
            impl_->Sync();
        };
        s.onOpenCenter = [this] {
            Clear();
            auto& sc = Screen::Current();
            POINT anchor{(sc.monitor.left + sc.monitor.right) / 2 + sc.Px(kNotchW / 2 - 22), sc.monitor.top + sc.Px(kNotchH)};
            App::Get().Popups().Toggle(PopupKind::Notifications, anchor);
            App::Get().Notifications().MarkAllSeen();
        };
    }
}

void ToastHost::Push(const NotificationItem& item, bool suppressed) {
    impl_->policy.Push(item, suppressed, NowMs());
    if (!suppressed) impl_->Sync();
}

void ToastHost::Clear() {
    impl_->policy.Clear();
    impl_->Sync();
}

void ToastHost::Layout() { impl_->Sync(); }

} // namespace nb::ui
