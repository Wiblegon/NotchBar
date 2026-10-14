#include "pch.h"

#include "ui/NotchWindow.h"

#include "app/App.h"
#include "gfx/Draw.h"
#include "gfx/IconCache.h"
#include "gfx/Theme.h"
#include "sys/AudioMonitor.h"
#include "sys/MediaMonitor.h"
#include "sys/NetworkMonitor.h"
#include "sys/PowerMonitor.h"
#include "ui/Widgets.h"

namespace nb::ui {
using namespace theme;
namespace {
constexpr float kMargin = 8; // room for the flares and a hint of shadow
float Smooth(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
} // namespace

bool NotchWindow::Init() {
    if (!Create(L"NotchBar üst çentik")) return false;
    w_ = gfx::SoftSpring(kNotchW);
    h_ = gfx::SoftSpring(kNotchH);
    edge_.onPresence = [this](EdgePresence p) { ApplyPresence(p, true); };
    Layout();
    RefreshClock();
    ApplyPresence(edge_.Presence(), false);
    return true;
}

void NotchWindow::Layout() {
    auto& sc = Screen::Current();
    winW_ = kNotchW + 2 * kNotchFlare + 2 * kMargin;
    winH_ = kNotchH + kMargin;
    int w = sc.Px(winW_), h = sc.Px(winH_);
    int cx = (sc.monitor.left + sc.monitor.right) / 2;
    Place({cx - w / 2, sc.monitor.top, cx - w / 2 + w, sc.monitor.top + h});
    UpdateRegion();
    Invalidate();
}

float NotchWindow::BodyX() const { return (winW_ - w_.value) / 2; }

void NotchWindow::ApplyPresence(EdgePresence p, bool animate) {
    switch (p) {
    case EdgePresence::Normal:
        w_.To(kNotchW);
        h_.To(kNotchH);
        break;
    case EdgePresence::Collapsed:
        w_.To(kNotchCollapsedW);
        h_.To(kNotchCollapsedH);
        hot_ = NotchButton::None;
        Tooltip::Get().Hide();
        break;
    case EdgePresence::Hidden:
        // Fullscreen: disappear at once, no animation over the game/video.
        w_.Snap(kNotchCollapsedW);
        h_.Snap(0);
        hot_ = NotchButton::None;
        Tooltip::Get().Hide();
        Show(false);
        return;
    }
    if (!animate) {
        w_.Snap(w_.target);
        h_.Snap(h_.target);
    }
    if (!Visible()) {
        if (animate && h_.value <= 0.5f) {
            w_.Snap(kNotchCollapsedW);
            h_.Snap(0.5f);
        }
        UpdateRegion();
        Show(true);
    }
    RequestAnimationFrame();
}

bool NotchWindow::Animate(float dt) {
    bool more = w_.Step(dt);
    more |= h_.Step(dt);
    UpdateRegion();
    return more;
}

void NotchWindow::UpdateRegion() {
    float x0 = BodyX() - kNotchFlare - 1;
    float x1 = BodyX() + w_.value + kNotchFlare + 1;
    float h = std::max(h_.value, kNotchCollapsedH) + 2;
    SetRegionDip({x0, 0, x1, h});
}

int NotchWindow::BottomPx() const { return WindowRect().top + static_cast<int>(std::lround(kNotchH * Scale())); }

D2D1_RECT_F NotchWindow::SlotRect(NotchButton b) const {
    float x = BodyX(), w = w_.value, h = h_.value;
    float s = kNotchSlot;
    float pad = 7;
    switch (b) {
    case NotchButton::Media: return {x + pad, 0, x + pad + s, h};
    case NotchButton::Volume: return {x + pad + s, 0, x + pad + 2 * s, h};
    case NotchButton::Network: return {x + pad + 2 * s, 0, x + pad + 3 * s, h};
    case NotchButton::Clock: return {x + w / 2 - 34, 0, x + w / 2 + 34, h};
    case NotchButton::Battery: return {x + w - pad - 2 * s, 0, x + w - pad - s, h};
    case NotchButton::Notifications: return {x + w - pad - s, 0, x + w - pad, h};
    default: return {};
    }
}

NotchButton NotchWindow::HitTest(PointF p) const {
    if (Smooth(0.7f, 1.0f, h_.value / kNotchH) < 0.5f) return NotchButton::None;
    for (auto b : {NotchButton::Media, NotchButton::Volume, NotchButton::Network, NotchButton::Clock, NotchButton::Battery,
                   NotchButton::Notifications})
        if (PointIn(SlotRect(b), p)) return b;
    return NotchButton::None;
}

POINT NotchWindow::AnchorOf(NotchButton b) const {
    auto r = SlotRect(b);
    return DipToScreen({(r.left + r.right) / 2, kNotchH});
}

void NotchWindow::Paint(ID2D1DeviceContext* dc) {
    float w = w_.value, h = h_.value;
    if (h < 0.5f) return;
    float x = BodyX();
    float radius = std::min(kNotchRadius, h * 0.45f);
    float flare = std::min(kNotchFlare, h);
    auto path = draw::NotchPath(x, w, h, flare, radius);
    // faint contact shadow under the body
    if (h > 10) {
        D2D1_RECT_F sh{x + 6, h - 6, x + w - 6, h + 2.5f};
        draw::RoundRect(dc, sh, 6, D2D1::ColorF(0, 0, 0, 0.18f));
    }
    dc->FillGeometry(path.get(), draw::Brush(dc, kSurface));
    dc->DrawGeometry(path.get(), draw::Brush(dc, kEdge), 1.0f);

    float a = Smooth(0.72f, 1.0f, h / kNotchH);
    if (a <= 0.01f) return;
    dc->PushAxisAlignedClip({x, 0, x + w, h}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    auto col = [&](D2D1_COLOR_F c, float mul = 1) {
        c.a *= a * mul;
        return c;
    };
    auto center = [&](NotchButton b) {
        auto r = SlotRect(b);
        return D2D1_POINT_2F{(r.left + r.right) / 2, h / 2};
    };
    // hover/press feedback
    if (hot_ != NotchButton::None) {
        auto r = SlotRect(hot_);
        D2D1_RECT_F hr{r.left + 2, h / 2 - 12, r.right - 2, h / 2 + 12};
        draw::RoundRect(dc, hr, 8, col(pressed_ == hot_ ? kSurfacePressed : kSurfaceHover));
    }

    // Media: source app icon when a session exists, otherwise a dim music note.
    {
        const auto& m = sys::MediaMonitor::Get().State();
        auto c = center(NotchButton::Media);
        ID2D1Bitmap* icon = m.hasSession && !m.iconKey.empty()
                                ? gfx::IconCache::Get().Get(m.iconKey, static_cast<int>(std::ceil(16 * Scale())))
                                : nullptr;
        if (icon) draw::Bitmap(dc, icon, {c.x - 8, c.y - 8, c.x + 8, c.y + 8}, a * (m.playing ? 1.0f : 0.55f));
        else draw::Glyph(dc, draw::glyph::Music, c, 14, col(m.hasSession ? kText : kTextFaint));
        if (m.playing) // small "now playing" dot
            dc->FillEllipse(D2D1::Ellipse({c.x + 8, c.y + 8}, 1.8f, 1.8f), draw::Brush(dc, col(kGood)));
    }
    // Volume
    {
        const auto& v = sys::AudioMonitor::Get().State();
        draw::Glyph(dc, v.available ? draw::VolumeGlyph(v.level, v.muted) : draw::glyph::Speaker, center(NotchButton::Volume), 14,
                    col(v.available ? kText : kTextFaint));
    }
    // Network
    {
        const auto& n = sys::NetworkMonitor::Get().State();
        wchar_t g = draw::glyph::NetOffline;
        if (n.kind == sys::NetKind::Ethernet) g = draw::glyph::Ethernet;
        else if (n.kind == sys::NetKind::Wifi) {
            int q = n.wifiQuality;
            g = q < 0 ? draw::glyph::Wifi4 : q < 25 ? draw::glyph::Wifi1 : q < 50 ? draw::glyph::Wifi2 : q < 75 ? draw::glyph::Wifi3 : draw::glyph::Wifi4;
        } else if (n.kind != sys::NetKind::None) g = draw::glyph::Globe;
        bool good = n.kind != sys::NetKind::None && n.internet;
        draw::Glyph(dc, g, center(NotchButton::Network), 14, col(good ? kText : kTextFaint));
    }
    // Clock: fixed position at the center of the notch.
    {
        auto r = SlotRect(NotchButton::Clock);
        draw::Text(dc, clock_, {r.left, 0, r.right, h}, 13.5f, col(kText), DWRITE_FONT_WEIGHT_SEMI_BOLD, draw::Align::Center, true);
    }
    // Battery
    {
        const auto& sb = sys::PowerMonitor::Get().System();
        auto c = center(NotchButton::Battery);
        if (sb && sb->percent) {
            bool charging = sb->charge == battery::Charge::Charging;
            auto color = (*sb->percent <= 15 && !charging) ? kBad : kText;
            draw::Glyph(dc, draw::BatteryGlyph(*sb->percent, charging), c, 15, col(color));
        } else if (sb) {
            draw::Glyph(dc, draw::glyph::BatteryUnknown, c, 15, col(kTextDim));
        } else {
            draw::Glyph(dc, draw::glyph::Devices, c, 14, col(kText)); // desktop: peripherals menu
        }
    }
    // Notifications
    {
        auto c = center(NotchButton::Notifications);
        draw::Glyph(dc, draw::glyph::Bell, c, 14, col(kText));
        if (App::Get().UnseenNotifications() > 0)
            dc->FillEllipse(D2D1::Ellipse({c.x + 6, c.y - 6}, 3.0f, 3.0f), draw::Brush(dc, col(kAccent)));
    }
    dc->PopAxisAlignedClip();
}

std::wstring NotchWindow::TooltipFor(NotchButton b) const {
    switch (b) {
    case NotchButton::Media: {
        const auto& m = sys::MediaMonitor::Get().State();
        if (!m.hasSession) return L"Çalan medya yok";
        std::wstring t = m.title.empty() ? m.appName : m.title;
        if (!m.artist.empty()) t += L" — " + m.artist;
        return t;
    }
    case NotchButton::Volume: {
        const auto& v = sys::AudioMonitor::Get().State();
        if (!v.available) return L"Ses çıkışı yok";
        return v.muted ? L"Ses kapalı" : L"Ses: " + std::to_wstring(static_cast<int>(std::lround(v.level * 100))) + L"%";
    }
    case NotchButton::Network: {
        const auto& n = sys::NetworkMonitor::Get().State();
        std::wstring t;
        switch (n.kind) {
        case sys::NetKind::None: return L"Bağlantı yok";
        case sys::NetKind::Ethernet: t = L"Kablolu bağlantı"; break;
        case sys::NetKind::Wifi: t = n.ssid.empty() ? L"Wi‑Fi" : L"Wi‑Fi: " + n.ssid; break;
        case sys::NetKind::Cellular: t = L"Hücresel"; break;
        default: t = L"Ağ bağlantısı"; break;
        }
        if (!n.internet) t += L" (internet yok)";
        return t;
    }
    case NotchButton::Clock: {
        wchar_t buf[128];
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, nullptr, nullptr, buf, 128, nullptr);
        return buf;
    }
    case NotchButton::Battery: {
        const auto& sb = sys::PowerMonitor::Get().System();
        if (!sb) return L"Cihaz pilleri";
        std::wstring t = L"Pil: " + battery::FormatPercent(sb->percent);
        if (sb->charge == battery::Charge::Charging) t += L" (şarj oluyor)";
        return t;
    }
    case NotchButton::Notifications: {
        size_t n = App::Get().UnseenNotifications();
        return n ? std::to_wstring(n) + L" yeni bildirim" : L"Bildirimler";
    }
    default: return {};
    }
}

void NotchWindow::OnMouseMove(PointF p) {
    if (!edge_.Model().Hovered()) edge_.PointerEnter();
    NotchButton b = HitTest(p);
    if (b != hot_) {
        hot_ = b;
        Invalidate();
        if (b != NotchButton::None) Tooltip::Get().Request(TooltipFor(b), AnchorOf(b), true);
        else Tooltip::Get().Hide();
    }
}

void NotchWindow::OnMouseLeave() {
    edge_.PointerLeave();
    hot_ = pressed_ = NotchButton::None;
    Tooltip::Get().Hide();
    Invalidate();
}

void NotchWindow::OnMouseDown(PointF p, int button) {
    if (button != 0) return;
    pressed_ = HitTest(p);
    Invalidate();
}

void NotchWindow::OnMouseUp(PointF p, int button) {
    NotchButton b = HitTest(p);
    bool click = button == 0 && b == pressed_ && b != NotchButton::None;
    pressed_ = NotchButton::None;
    Invalidate();
    if (click) {
        Tooltip::Get().Hide();
        if (onClick) onClick(b);
    }
}

void NotchWindow::OnWheel(PointF p, int delta) {
    if (HitTest(p) != NotchButton::Volume) return;
    auto& a = sys::AudioMonitor::Get();
    a.SetLevel(a.State().level + (delta > 0 ? 0.02f : -0.02f));
    Tooltip::Get().Request(TooltipFor(NotchButton::Volume), AnchorOf(NotchButton::Volume), true);
}

void NotchWindow::RefreshClock() {
    wchar_t buf[64];
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, buf, 64);
    if (clock_ != buf) {
        clock_ = buf;
        Invalidate(); // hidden surfaces just stay dirty; nothing is drawn until visible
    }
    ScheduleClock();
}

void NotchWindow::ScheduleClock() {
    ClearTimeout(clockTimer_);
    SYSTEMTIME st;
    GetLocalTime(&st);
    // Wake exactly once per minute, just after the minute boundary.
    int ms = (60 - st.wSecond) * 1000 - st.wMilliseconds + 40;
    clockTimer_ = SetTimeout(ms, [this] {
        clockTimer_ = 0;
        RefreshClock();
    });
}

} // namespace nb::ui
