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
#include "sys/PrivacyMonitor.h"
#include "ui/Widgets.h"

namespace nb::ui {
using namespace theme;
namespace {
constexpr float kMargin = 8; // room for the flares and a hint of shadow
constexpr float kPad = 7;    // inner padding at both ends
constexpr float kClockZone = 68;
constexpr float kPrivW = 13; // one camera/microphone indicator (small on purpose)
constexpr float kNetW = 14;  // connection indicator next to the clock (same size class as camera/microphone)
float Smooth(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
} // namespace

bool NotchWindow::Init() {
    if (!Create(L"NotchBar üst çentik")) return false;
    w_ = gfx::SoftSpring(TargetWidth());
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

bool NotchWindow::MediaShown() const { return sys::MediaMonitor::Get().State().hasSession; }

int NotchWindow::PrivacyCount() const {
    const auto& p = sys::PrivacyMonitor::Get().State();
    return (p.mic ? 1 : 0) + (p.camera ? 1 : 0);
}

// The body is as wide as what it shows: unused parts take no room, the clock stays centered.
float NotchWindow::TargetWidth() const {
    int left = MediaShown() ? 4 : 3, right = 3; // left: [media] volume quick tray; right: battery notifications power
    float side = kPad + std::max(left, right) * kNotchSlot;
    // Next to the clock: connection state on the left, camera/microphone indicators on the right.
    float beside = std::max(kNetW, PrivacyCount() * kPrivW);
    return std::min(kNotchW, 2 * side + kClockZone + 2 * beside);
}

void NotchWindow::Changed() {
    if (edge_.Presence() == EdgePresence::Normal && std::fabs(w_.target - TargetWidth()) > 0.5f) {
        w_.To(TargetWidth());
        if (Visible()) RequestAnimationFrame();
    }
    Invalidate();
}

void NotchWindow::ApplyPresence(EdgePresence p, bool animate) {
    switch (p) {
    case EdgePresence::Normal:
        w_.To(TargetWidth());
        h_.To(kNotchH);
        break;
    case EdgePresence::Collapsed:
        w_.To(pill_ ? kNotchPillW : kNotchCollapsedW);
        h_.To(pill_ ? kNotchPillH : kNotchCollapsedH);
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
            // Grow out of the edge. Snap() also overwrites the spring targets, so restore them,
            // otherwise the notch stays 0.5 px tall after coming back from fullscreen.
            float tw = w_.target, th = h_.target;
            w_.Snap(kNotchCollapsedW);
            h_.Snap(0.5f);
            w_.To(tw);
            h_.To(th);
        }
        UpdateRegion();
        Show(true);
    }
    RequestAnimationFrame();
}

void NotchWindow::SetClockPill(bool on) {
    if (on == pill_) return;
    pill_ = on;
    if (edge_.Presence() == EdgePresence::Collapsed) ApplyPresence(EdgePresence::Collapsed, true);
    Invalidate();
}

bool NotchWindow::Animate(float dt) {
    bool more = w_.Step(dt);
    more |= h_.Step(dt);
    more |= anim_.Step(dt);
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
    float pad = kPad;
    // Unused parts disappear completely: no media session -> no media slot, the rest moves up.
    bool media = MediaShown();
    float left = x + pad;
    const auto& pv = sys::PrivacyMonitor::Get().State();
    switch (b) {
    case NotchButton::Media: return media ? D2D1_RECT_F{left, 0, left + s, h} : D2D1_RECT_F{};
    case NotchButton::Volume: left += media ? s : 0; return {left, 0, left + s, h};
    case NotchButton::Network: { // small, left of the clock (like the camera/microphone indicators on the right)
        float r = x + w / 2 - kClockZone / 2;
        return {r - kNetW, 0, r, h};
    }
    case NotchButton::Quick: left += (media ? 2 : 1) * s; return {left, 0, left + s, h};
    case NotchButton::Tray: left += (media ? 3 : 2) * s; return {left, 0, left + s, h};
    case NotchButton::Clock: return {x + w / 2 - kClockZone / 2, 0, x + w / 2 + kClockZone / 2, h};
    case NotchButton::Mic: {
        if (!pv.mic) return {};
        float l = x + w / 2 + kClockZone / 2;
        return {l, 0, l + kPrivW, h};
    }
    case NotchButton::Camera: {
        if (!pv.camera) return {};
        float l = x + w / 2 + kClockZone / 2 + (pv.mic ? kPrivW : 0);
        return {l, 0, l + kPrivW, h};
    }
    case NotchButton::Battery: return {x + w - pad - 3 * s, 0, x + w - pad - 2 * s, h};
    case NotchButton::Notifications: return {x + w - pad - 2 * s, 0, x + w - pad - s, h};
    case NotchButton::Power: return {x + w - pad - s, 0, x + w - pad, h};
    default: return {};
    }
}

NotchButton NotchWindow::HitTest(PointF p) const {
    if (Smooth(0.7f, 1.0f, h_.value / kNotchH) < 0.5f) return NotchButton::None;
    for (auto b : {NotchButton::Media, NotchButton::Volume, NotchButton::Network, NotchButton::Clock, NotchButton::Battery,
                   NotchButton::Notifications, NotchButton::Power, NotchButton::Mic, NotchButton::Camera,
                   NotchButton::Tray, NotchButton::Quick}) {
        auto r = SlotRect(b);
        if (r.right > r.left && PointIn(r, p)) return b;
    }
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
    if (pill_ && a < 1.0f) {
        // Small collapsed form: just the time, fading out as the full notch grows in.
        float pa = (1.0f - a) * Smooth(0.6f, 1.0f, h / kNotchPillH);
        if (pa > 0.01f) {
            D2D1_COLOR_F c = kText;
            c.a *= pa;
            draw::Text(dc, clock_, {x, 0, x + w, std::min(h, kNotchPillH) - 1}, 11.5f, c, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                       draw::Align::Center, true);
        }
    }
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
    // hover/press feedback: fades in and out; icons grow a little under the pointer and shrink when pressed
    auto hov = [&](NotchButton b) { return anim_.Get(gfx::HoverBank::Hover, static_cast<int>(b)); };
    auto prs = [&](NotchButton b) { return anim_.Get(gfx::HoverBank::Press, static_cast<int>(b)); };
    for (auto b : {NotchButton::Media, NotchButton::Volume, NotchButton::Network, NotchButton::Clock, NotchButton::Battery,
                   NotchButton::Notifications, NotchButton::Power, NotchButton::Mic, NotchButton::Camera, NotchButton::Tray,
                   NotchButton::Quick}) {
        float hv = hov(b), pv = prs(b);
        if (hv < 0.01f && pv < 0.01f) continue;
        auto r = SlotRect(b);
        if (r.right <= r.left) continue;
        D2D1_RECT_F hr{r.left + 2, h / 2 - 12, r.right - 2, h / 2 + 12};
        D2D1_COLOR_F c = Rgba(255, 255, 255, 0.07f * hv + 0.06f * pv);
        draw::RoundRect(dc, hr, 8, col(c));
    }
    auto gs = [&](NotchButton b, float size) { return size * (1.0f + 0.12f * hov(b) - 0.16f * prs(b)); };

    // Media: only while a session exists.
    if (sys::MediaMonitor::Get().State().hasSession) {
        const auto& m = sys::MediaMonitor::Get().State();
        auto c = center(NotchButton::Media);
        ID2D1Bitmap* icon = m.hasSession && !m.iconKey.empty()
                                ? gfx::IconCache::Get().Get(m.iconKey, static_cast<int>(std::ceil(16 * Scale())))
                                : nullptr;
        float ih = gs(NotchButton::Media, 16) / 2;
        if (icon) draw::BitmapMono(dc, icon, {c.x - ih, c.y - ih, c.x + ih, c.y + ih}, a * (m.playing ? 1.0f : 0.55f));
        else draw::Glyph(dc, draw::glyph::Music, c, gs(NotchButton::Media, 14), col(m.hasSession ? kText : kTextFaint));
        if (m.playing) // small "now playing" dot
            dc->FillEllipse(D2D1::Ellipse({c.x + 8, c.y + 8}, 1.8f, 1.8f), draw::Brush(dc, col(kGood)));
    }
    // Volume
    {
        const auto& v = sys::AudioMonitor::Get().State();
        draw::Glyph(dc, v.available ? draw::VolumeGlyph(v.level, v.muted) : draw::glyph::Speaker, center(NotchButton::Volume),
                    gs(NotchButton::Volume, 14), col(v.available ? kText : kTextFaint));
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
        // Small indicator: cable / Wi-Fi strength normally; amber when connected without internet,
        // red when there is no connection at all.
        bool connected = n.kind != sys::NetKind::None;
        auto color = !connected ? kBad : n.internet ? kText : kWarn;
        draw::Glyph(dc, g, center(NotchButton::Network), gs(NotchButton::Network, 9), col(color)); // as small as the microphone icon
        if (connected && !n.internet) { // small "!" dot
            auto c = center(NotchButton::Network);
            dc->FillEllipse(D2D1::Ellipse({c.x + 5, c.y - 5}, 2.0f, 2.0f), draw::Brush(dc, col(kWarn)));
        }
    }
    // Clock: fixed position at the center of the notch.
    {
        auto r = SlotRect(NotchButton::Clock);
        draw::Text(dc, clock_, {r.left, h / 2 - 15, r.right, h / 2 + 3}, 13, col(kText), DWRITE_FONT_WEIGHT_SEMI_BOLD, draw::Align::Center, true);
        draw::Text(dc, date_, {r.left, h / 2 + 1, r.right, h / 2 + 13}, 9.5f, col(kTextDim), DWRITE_FONT_WEIGHT_NORMAL, draw::Align::Center);
    }
    // Battery
    {
        const auto& sb = sys::PowerMonitor::Get().System();
        auto c = center(NotchButton::Battery);
        if (sb && sb->percent) {
            bool charging = sb->charge == battery::Charge::Charging;
            auto color = (*sb->percent <= 15 && !charging) ? kBad : kText;
            draw::Glyph(dc, draw::BatteryGlyph(*sb->percent, charging), c, gs(NotchButton::Battery, 15), col(color));
        } else if (sb) {
            draw::Glyph(dc, draw::glyph::BatteryUnknown, c, gs(NotchButton::Battery, 15), col(kTextDim));
        } else {
            draw::Glyph(dc, draw::glyph::Devices, c, gs(NotchButton::Battery, 14), col(kText)); // desktop: peripherals menu
        }
    }
    // Notifications
    {
        auto c = center(NotchButton::Notifications);
        draw::Glyph(dc, draw::glyph::Bell, c, gs(NotchButton::Notifications, 14), col(kText));
        if (App::Get().UnseenNotifications() > 0)
            dc->FillEllipse(D2D1::Ellipse({c.x + 6, c.y - 6}, 3.0f, 3.0f), draw::Brush(dc, col(kAccent)));
    }
    // Background apps (the "hidden icons" of the system tray)
    draw::Glyph(dc, draw::glyph::ChevronUp, center(NotchButton::Tray), gs(NotchButton::Tray, 12), col(kText));
    // Quick settings (Bluetooth, Wi-Fi, ...)
    draw::Glyph(dc, 0xE9E9, center(NotchButton::Quick), gs(NotchButton::Quick, 14), col(kText));
    // Camera / microphone in use: tiny indicators next to the clock.
    {
        const auto& pv = sys::PrivacyMonitor::Get().State();
        if (pv.mic) draw::Glyph(dc, draw::glyph::Microphone, center(NotchButton::Mic), gs(NotchButton::Mic, 9), col(kWarn));
        if (pv.camera) draw::Glyph(dc, draw::glyph::Camera, center(NotchButton::Camera), gs(NotchButton::Camera, 9), col(kGood));
    }
    // Power menu
    draw::Glyph(dc, draw::glyph::Power, center(NotchButton::Power), gs(NotchButton::Power, 14), col(kText));
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
    case NotchButton::Power: return L"Güç";
    case NotchButton::Tray: return L"Arka plan uygulamaları";
    case NotchButton::Quick: return L"Hızlı ayarlar (Bluetooth, Wi‑Fi, ...)";
    case NotchButton::Mic:
    case NotchButton::Camera: {
        const auto& pv = sys::PrivacyMonitor::Get().State();
        bool mic = b == NotchButton::Mic;
        std::wstring t = mic ? L"Mikrofon kullanılıyor" : L"Kamera kullanılıyor";
        const auto& apps = mic ? pv.micApps : pv.cameraApps;
        for (size_t i = 0; i < apps.size(); ++i) t += (i ? L", " : L": ") + apps[i];
        return t;
    }
    default: return {};
    }
}

void NotchWindow::OnMouseMove(PointF p) {
    if (!edge_.Model().Hovered()) edge_.PointerEnter();
    NotchButton b = HitTest(p);
    if (b != hot_) {
        hot_ = b;
        anim_.SetHot(b == NotchButton::None ? gfx::HoverBank::kNone : static_cast<int>(b));
        RequestAnimationFrame();
        Invalidate();
        if (b != NotchButton::None) Tooltip::Get().Request(TooltipFor(b), AnchorOf(b), true);
        else Tooltip::Get().Hide();
    }
}

void NotchWindow::OnMouseLeave() {
    edge_.PointerLeave();
    hot_ = pressed_ = NotchButton::None;
    anim_.SetHot(gfx::HoverBank::kNone);
    anim_.SetPressed(gfx::HoverBank::kNone);
    Tooltip::Get().Hide();
    RequestAnimationFrame();
    Invalidate();
}

void NotchWindow::OnMouseDown(PointF p, int button) {
    if (button != 0) return;
    pressed_ = HitTest(p);
    anim_.SetPressed(pressed_ == NotchButton::None ? gfx::HoverBank::kNone : static_cast<int>(pressed_));
    RequestAnimationFrame();
    if (pressed_ == NotchButton::None && onBlankClick) onBlankClick();
    Invalidate();
}

void NotchWindow::OnMouseUp(PointF p, int button) {
    NotchButton b = HitTest(p);
    bool click = button == 0 && b == pressed_ && b != NotchButton::None;
    pressed_ = NotchButton::None;
    anim_.SetPressed(gfx::HoverBank::kNone);
    RequestAnimationFrame();
    Invalidate();
    if (click) {
        Tooltip::Get().Hide();
        if (onClick) onClick(b);
    } else if (button == 1 && onContext) {
        Tooltip::Get().Hide();
        POINT pt;
        GetCursorPos(&pt);
        onContext(pt);
    }
}

void NotchWindow::OnWheel(PointF p, int delta) {
    if (HitTest(p) != NotchButton::Volume) return;
    auto& a = sys::AudioMonitor::Get();
    a.SetLevel(a.State().level + (delta > 0 ? 0.02f : -0.02f));
    Tooltip::Get().Request(TooltipFor(NotchButton::Volume), AnchorOf(NotchButton::Volume), true);
}

void NotchWindow::RefreshClock() {
    wchar_t buf[64], dbuf[64];
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, buf, 64);
    // Short date under the time, e.g. "15 Ekim" (day + month name in the user's language).
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"d MMMM", dbuf, 64, nullptr)) dbuf[0] = 0;
    if (clock_ != buf || date_ != dbuf) {
        clock_ = buf;
        date_ = dbuf;
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
