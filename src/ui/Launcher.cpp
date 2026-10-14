#include "pch.h"

#include "ui/Launcher.h"

#include "core/Dispatcher.h"
#include "core/Log.h"
#include "core/Store.h"
#include "gfx/Draw.h"
#include "gfx/Theme.h"
#include "model/PanelDirection.h"
#include "sys/AppCatalog.h"
#include "ui/LauncherPanel.h"
#include "ui/TextInput.h"
#include "ui/Widgets.h"

namespace nb::ui {
using namespace theme;

namespace {
constexpr float kM = 12; // shadow margin around the bar
const COLORREF kBarRgb = RGB(24, 25, 28);

bool IsOurs(HWND h) {
    if (!h) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

// Earlier pointer sample (screen px) from the system's mouse-move history, if it moved enough.
std::optional<PointF> PreviousPointer(POINT now) {
    MOUSEMOVEPOINT in{};
    in.x = now.x & 0xFFFF;
    in.y = now.y & 0xFFFF;
    MOUSEMOVEPOINT out[32];
    int n = GetMouseMovePointsEx(sizeof in, &in, out, 32, GMMP_USE_DISPLAY_POINTS);
    for (int i = 1; i < n; ++i) {
        int x = out[i].x > 32767 ? out[i].x - 65536 : out[i].x; // negative coordinates on left/top monitors
        int y = out[i].y > 32767 ? out[i].y - 65536 : out[i].y;
        int dx = x - now.x, dy = y - now.y;
        if (dx * dx + dy * dy >= 36) return PointF{static_cast<float>(x), static_cast<float>(y)};
    }
    return std::nullopt;
}
} // namespace

class BarSurface : public gfx::Surface {
public:
    std::function<void(POINT)> onEnter;
    std::function<void()> onLeave;
    std::function<void(float)> onAppear;
    gfx::Spring appear = gfx::FadeSpring(0);
    bool shown = false;

    D2D1_RECT_F BarRect() const { return {kM, kM, kM + kBarW, kM + kBarH}; }

protected:
    void Paint(ID2D1DeviceContext* dc) override {
        float a = std::clamp(appear.value, 0.0f, 1.0f);
        if (a <= 0.001f) return;
        auto r = BarRect();
        D2D1_MATRIX_3X2_F base;
        dc->GetTransform(&base);
        float s = 0.97f + 0.03f * a;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}) * base);
        draw::Panel(dc, r, kBarRadius, a);
        // Opaque fill: must match the native text field's background exactly.
        draw::RoundRect(dc, {r.left + 1, r.top + 1, r.right - 1, r.bottom - 1}, kBarRadius - 1, Rgba(24, 25, 28, a));
        dc->DrawRoundedRectangle({{r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f}, kBarRadius, kBarRadius},
                                 draw::Brush(dc, Rgba(255, 255, 255, 0.09f * a)), 1);
        draw::Glyph(dc, draw::glyph::Search, {r.left + 22, (r.top + r.bottom) / 2}, 15, Rgba(160, 161, 168, a));
        dc->SetTransform(base);
    }
    bool Animate(float dt) override {
        bool more = appear.Step(dt);
        if (onAppear) onAppear(appear.value);
        if (!shown && appear.value <= 0.01f && !more) Show(false);
        return more;
    }
    void OnMouseMove(PointF p) override {
        if (!inside_) {
            inside_ = true;
            POINT pt;
            GetCursorPos(&pt);
            if (onEnter) onEnter(pt);
        }
    }
    void OnMouseLeave() override {
        inside_ = false;
        if (onLeave) onLeave();
    }

private:
    bool inside_ = false;
};

struct Launcher::Impl {
    BarSurface bar;
    LauncherPanel panel;
    TextInput input;
    HoverPanelModel model;
    TimerId modelTimer = 0;
    bool open = false;
    HWND prevForeground = nullptr;
    RECT barScreen{};
    bool pointerInside = false;

    void LayoutBar() {
        auto& sc = Screen::Current();
        int w = sc.Px(kBarW + 2 * kM), h = sc.Px(kBarH + 2 * kM);
        int cx = (sc.monitor.left + sc.monitor.right) / 2;
        int cy = (sc.monitor.top + sc.monitor.bottom) / 2;
        RECT r{cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h};
        bar.Place(r);
        auto br = bar.BarRect();
        bar.SetRegionDip({br.left, br.top, br.right, br.bottom});
        barScreen = {r.left + sc.Px(kM), r.top + sc.Px(kM), r.right - sc.Px(kM), r.bottom - sc.Px(kM)};
        RECT ir{barScreen.left + sc.Px(40), barScreen.top + sc.Px(9), barScreen.right - sc.Px(14), barScreen.bottom - sc.Px(9)};
        input.Place(ir, 15.0f * sc.scale, kBarRgb);
    }

    PanelDir Fit(PanelDir want) {
        auto& sc = Screen::Current();
        float above = sc.Dip(barScreen.top - sc.monitor.top);
        float below = sc.Dip(sc.monitor.bottom - barScreen.bottom);
        return FitPanelDirection(want, above, below, kPanelH + kPanelGap + 8);
    }

    void ApplyPanel() {
        bool hasQuery = !input.Text().empty();
        bool want = open && (hasQuery || model.IsOpen());
        if (want && !panel.Shown()) {
            PanelDir dir = Fit(model.IsOpen() ? model.Dir() : PanelDir::Below);
            panel.PlaceNear(barScreen, dir);
            panel.Appear(true);
        } else if (!want && panel.Shown()) {
            panel.Appear(false);
            if (!hasQuery) panel.ResetToRoot();
        }
    }

    void ArmModel() {
        ClearTimeout(modelTimer);
        if (auto d = model.NextDeadline()) {
            modelTimer = SetTimeout(static_cast<int>(std::max<TimeMs>(1, *d - NowMs())), [this] {
                modelTimer = 0;
                if (model.Tick(NowMs())) ApplyPanel();
                ArmModel();
            });
        }
    }

    void PointerEnterBar(POINT pt) {
        if (!open) return;
        pointerInside = true;
        auto& sc = Screen::Current();
        RectF barR{static_cast<float>(barScreen.left), static_cast<float>(barScreen.top), static_cast<float>(barScreen.right),
                   static_cast<float>(barScreen.bottom)};
        (void)sc;
        PanelDir dir = DeterminePanelDirection({static_cast<float>(pt.x), static_cast<float>(pt.y)}, barR, PreviousPointer(pt));
        model.PointerEnterBar(dir, NowMs());
        ArmModel();
        ApplyPanel();
    }

    void CheckLeave() {
        // Leaving one of our launcher windows into another (bar <-> text field <-> panel/bridge)
        // is not leaving.
        Post([this] {
            POINT pt;
            GetCursorPos(&pt);
            HWND h = WindowFromPoint(pt);
            HWND root = h ? GetAncestor(h, GA_ROOT) : nullptr;
            bool inside = root == bar.Hwnd() || root == panel.Hwnd() || root == input.Host();
            if (inside) return;
            pointerInside = false;
            model.PointerLeaveAll(NowMs());
            ArmModel();
            ApplyPanel();
        });
    }
};

Launcher::Launcher() : impl_(std::make_unique<Impl>()) {}
Launcher::~Launcher() = default;

bool Launcher::Init() {
    auto& d = *impl_;
    if (!d.bar.Create(L"NotchBar arama çubuğu")) return false;
    if (!d.panel.Init()) return false;
    auto& st = Store::Get().settings();
    d.model.SetTimings({st.panelOpenDelayMs, st.panelCloseDelayMs});

    TextInput::Callbacks cb;
    cb.onChange = [this] {
        auto& d = *impl_;
        std::wstring q = d.input.Text();
        d.panel.SetQuery(q);
        if (q.empty()) {
            d.panel.ResetToRoot();
            if (!d.model.IsOpen()) d.panel.Appear(false);
        }
        d.ApplyPanel();
    };
    cb.onKey = [this](UINT vk) {
        auto& d = *impl_;
        if (vk == VK_ESCAPE) {
            if (d.panel.Shown() && d.panel.HandleKey(vk)) return true;
            if (!d.input.Text().empty()) {
                d.input.SetText(L"");
                return true;
            }
            Close(true);
            return true;
        }
        if ((vk == VK_DOWN || vk == VK_TAB) && !d.panel.Shown()) {
            d.model.OpenNow(d.Fit(PanelDir::Below));
            d.ApplyPanel();
        }
        if (d.panel.Shown()) return d.panel.HandleKey(vk);
        return vk == VK_TAB;
    };
    cb.onFocusLost = [this](HWND to) {
        if (IsOurs(to)) return; // our menu / rename field / panel
        Post([this] {
            if (impl_->open && !IsOurs(GetForegroundWindow())) Close(false);
        });
    };
    cb.onHover = [this](bool in) {
        if (in) {
            POINT pt;
            GetCursorPos(&pt);
            if (!impl_->pointerInside) impl_->PointerEnterBar(pt);
        } else impl_->CheckLeave();
    };
    d.input.Create(d.bar.Hwnd(), L"Uygulama, dosya veya web'de ara", cb);

    d.bar.onEnter = [this](POINT pt) {
        if (!impl_->pointerInside) impl_->PointerEnterBar(pt);
    };
    d.bar.onLeave = [this] { impl_->CheckLeave(); };
    d.bar.onAppear = [this](float a) { impl_->input.SetAlpha(static_cast<BYTE>(std::clamp(a, 0.0f, 1.0f) * 255)); };
    d.panel.onPointerEnter = [this] {
        auto& d = *impl_;
        d.pointerInside = true;
        d.model.PointerEnterPanel(NowMs());
        d.ArmModel();
    };
    d.panel.onPointerLeave = [this] { impl_->CheckLeave(); };
    d.panel.requestClose = [this](bool restore) { Close(restore); };
    d.panel.refocusSearch = [this] {
        if (impl_->open) impl_->input.Focus();
    };
    d.LayoutBar();
    return true;
}

void Launcher::Layout() {
    impl_->LayoutBar();
    if (impl_->panel.Shown()) Close(false);
}

bool Launcher::IsOpen() const { return impl_->open; }

void Launcher::Toggle() {
    if (impl_->open) Close(true);
    else Open();
}

void Launcher::Open() {
    auto& d = *impl_;
    if (d.open) {
        d.input.Focus();
        return;
    }
    HWND fg = GetForegroundWindow();
    d.prevForeground = IsOurs(fg) ? nullptr : fg;
    d.open = true;
    d.pointerInside = false;
    d.model.CloseNow();
    d.input.SetText(L"");
    d.panel.ResetToRoot();
    d.LayoutBar();
    // First open: only the search bar. No grid, no favorites, no big frame.
    d.bar.shown = true;
    d.bar.appear.Snap(gfx::ReducedMotion() ? 1.0f : 0.0f);
    d.bar.appear.To(1);
    d.input.SetAlpha(gfx::ReducedMotion() ? 255 : 0);
    d.bar.Show(true);
    d.bar.RequestAnimationFrame();
    d.input.Show(true, true);
    sys::AppCatalog::Get().Refresh(); // rate-limited background refresh
    // If the pointer already rests on the bar, treat it as a hover.
    POINT pt;
    GetCursorPos(&pt);
    if (PtInRect(&d.barScreen, pt)) d.PointerEnterBar(pt);
}

void Launcher::Close(bool restoreFocus) {
    auto& d = *impl_;
    if (!d.open) return;
    d.open = false;
    ClearTimeout(d.modelTimer);
    d.model.CloseNow();
    d.panel.CancelSearches();
    d.panel.Appear(false);
    MenuPopup::Get().Close(false);
    Tooltip::Get().Hide();
    d.input.Show(false, false);
    d.bar.shown = false;
    d.bar.appear.To(0);
    d.bar.RequestAnimationFrame();
    if (gfx::ReducedMotion()) d.bar.Show(false);
    if (restoreFocus && d.prevForeground && IsWindow(d.prevForeground)) SetForegroundWindow(d.prevForeground);
    d.prevForeground = nullptr;
}

void Launcher::OpenSettings() {
    Open();
    auto& d = *impl_;
    d.model.OpenNow(d.Fit(PanelDir::Below));
    d.ApplyPanel();
    d.panel.SetView(LauncherPanel::View::Settings);
}

void Launcher::Refresh() {
    auto& st = Store::Get().settings();
    impl_->model.SetTimings({st.panelOpenDelayMs, st.panelCloseDelayMs});
    impl_->panel.Refresh();
}

} // namespace nb::ui
