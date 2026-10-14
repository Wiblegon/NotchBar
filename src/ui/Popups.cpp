#include "pch.h"

#include "ui/Popups.h"

#include "app/App.h"
#include "core/Dispatcher.h"
#include "core/Store.h"
#include "core/Strings.h"
#include "gfx/Draw.h"
#include "gfx/Graphics.h"
#include "gfx/IconCache.h"
#include "gfx/Theme.h"
#include "sys/AudioMonitor.h"
#include "sys/MediaMonitor.h"
#include "sys/NotificationMonitor.h"
#include "sys/PowerMonitor.h"
#include "sys/battery/BatteryService.h"
#include "ui/Launcher.h"

namespace nb::ui {
using namespace theme;
using draw::Align;
namespace g = draw::glyph;

namespace {
std::wstring Duration(double sec) {
    int s = static_cast<int>(sec);
    wchar_t b[32];
    if (s >= 3600) swprintf(b, 32, L"%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else swprintf(b, 32, L"%d:%02d", s / 60, s % 60);
    return b;
}

int64_t WallMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 10000);
}

std::wstring Ago(int64_t thenWallMs) {
    int64_t d = (WallMs() - thenWallMs) / 1000;
    if (d < 60) return L"şimdi";
    if (d < 3600) return std::to_wstring(d / 60) + L" dk önce";
    if (d < 86400) return std::to_wstring(d / 3600) + L" sa önce";
    return std::to_wstring(d / 86400) + L" gün önce";
}

struct Button {
    D2D1_RECT_F r{};
    int id = 0;
    bool enabled = true;
};
} // namespace

// ============================================================================ Volume
class VolumePopup : public Popup {
public:
    void Refresh() {
        if (IsOpen()) Invalidate();
    }

protected:
    float ContentWidth() override { return 280; }
    float ContentHeight() override { return 104; }
    D2D1_RECT_F Track() const {
        auto r = ContentRect();
        return {r.left + 52, r.top + 58, r.right - 56, r.top + 62};
    }
    D2D1_RECT_F MuteBtn() const {
        auto r = ContentRect();
        return {r.left + 10, r.top + 44, r.left + 44, r.top + 76};
    }
    D2D1_RECT_F SettingsLink() const {
        auto r = ContentRect();
        return {r.right - 120, r.top + 8, r.right - 10, r.top + 32};
    }
    void PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) override {
        const auto& s = sys::AudioMonitor::Get().State();
        draw::Text(dc, s.available ? Ellipsize(s.deviceName, 30) : L"Ses çıkış aygıtı yok", {r.left + 14, r.top + 8, r.right - 124, r.top + 32},
                   12, kTextDim);
        auto sl = SettingsLink();
        draw::Text(dc, L"Ses ayarları ›", sl, 12, hot_ == 3 ? kText : kTextFaint, DWRITE_FONT_WEIGHT_NORMAL, Align::Trailing);
        auto mb = MuteBtn();
        if (hot_ == 1) draw::RoundRect(dc, mb, 8, kSurfaceHover);
        draw::Glyph(dc, draw::VolumeGlyph(s.level, s.muted), {(mb.left + mb.right) / 2, (mb.top + mb.bottom) / 2}, 16,
                    s.available ? kText : kTextFaint);
        auto t = Track();
        draw::RoundRect(dc, t, 2, Rgba(255, 255, 255, 0.14f));
        float x = t.left + (t.right - t.left) * (s.muted ? 0.0f : s.level);
        draw::RoundRect(dc, {t.left, t.top, x, t.bottom}, 2, s.muted ? kTextFaint : kText);
        dc->FillEllipse(D2D1::Ellipse({x, (t.top + t.bottom) / 2}, dragging_ || hot_ == 2 ? 8.0f : 7.0f, dragging_ || hot_ == 2 ? 8.0f : 7.0f),
                        draw::Brush(dc, Rgba(245, 245, 247)));
        std::wstring pct = s.muted ? L"Kapalı" : std::to_wstring(static_cast<int>(std::lround(s.level * 100))) + L"%";
        draw::Text(dc, pct, {t.right + 8, r.top + 44, r.right - 8, r.top + 76}, 12.5f, kText, DWRITE_FONT_WEIGHT_NORMAL, Align::Trailing, true);
        draw::Text(dc, L"Tekerlek / ok tuşları: ±2%   M: sessiz", {r.left + 14, r.top + 78, r.right - 10, r.bottom - 4}, 10.5f, kTextFaint);
    }
    void SetFromX(float x) {
        auto t = Track();
        float v = std::clamp((x - t.left) / (t.right - t.left), 0.0f, 1.0f);
        sys::AudioMonitor::Get().SetLevel(v);
        Invalidate();
    }
    int Hit(PointF p) const {
        if (PointIn(MuteBtn(), p)) return 1;
        auto t = Track();
        if (p.x >= t.left - 8 && p.x <= t.right + 8 && p.y >= t.top - 14 && p.y <= t.bottom + 14) return 2;
        if (PointIn(SettingsLink(), p)) return 3;
        return 0;
    }
    void OnMouseMove(PointF p) override {
        if (dragging_) {
            SetFromX(p.x);
            return;
        }
        int h = Hit(p);
        if (h != hot_) {
            hot_ = h;
            Invalidate();
        }
    }
    void OnMouseLeave() override {
        hot_ = 0;
        Invalidate();
    }
    void OnMouseDown(PointF p, int b) override {
        if (b == 0 && Hit(p) == 2) {
            dragging_ = true;
            SetFromX(p.x);
        }
    }
    void OnMouseUp(PointF p, int b) override {
        if (dragging_) {
            dragging_ = false;
            Invalidate();
            return;
        }
        int h = Hit(p);
        if (h == 1) sys::AudioMonitor::Get().SetMuted(!sys::AudioMonitor::Get().State().muted);
        if (h == 3) {
            ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
            Close(false);
        }
    }
    void OnWheel(PointF, int delta) override {
        auto& a = sys::AudioMonitor::Get();
        a.SetLevel(a.State().level + (delta > 0 ? 0.02f : -0.02f));
    }
    bool OnKey(UINT vk) override {
        auto& a = sys::AudioMonitor::Get();
        if (vk == VK_RIGHT || vk == VK_UP) a.SetLevel(a.State().level + 0.02f);
        else if (vk == VK_LEFT || vk == VK_DOWN) a.SetLevel(a.State().level - 0.02f);
        else if (vk == 'M' || vk == VK_SPACE) a.SetMuted(!a.State().muted);
        else return false;
        return true;
    }

private:
    int hot_ = 0;
    bool dragging_ = false;
};

// ============================================================================ Media
class MediaPopup : public Popup {
public:
    void Refresh() {
        if (!IsOpen()) return;
        ArmProgress();
        Invalidate();
    }
    void Opened() {
        ArmProgress();
    }
    void Closed() {
        ClearTimeout(tick_);
        sys::MediaMonitor::Get().ReleaseArt(); // no album art kept while the box is closed
    }

protected:
    float ContentWidth() override { return 320; }
    float ContentHeight() override { return sys::MediaMonitor::Get().State().hasSession ? 150.0f : 96.0f; }
    void ArmProgress() {
        ClearTimeout(tick_);
        const auto& m = sys::MediaMonitor::Get().State();
        if (IsOpen() && m.playing && m.hasTimeline)
            tick_ = SetTimeout(1000, [this] {
                tick_ = 0;
                Invalidate();
                ArmProgress();
            });
    }
    std::array<Button, 3> Buttons() const {
        auto r = ContentRect();
        const auto& m = sys::MediaMonitor::Get().State();
        float cy = r.bottom - 26, cx = (r.left + r.right) / 2;
        return {Button{{cx - 70, cy - 16, cx - 38, cy + 16}, 1, m.canPrev}, Button{{cx - 18, cy - 18, cx + 18, cy + 18}, 2, m.canPlayPause},
                Button{{cx + 38, cy - 16, cx + 70, cy + 16}, 3, m.canNext}};
    }
    void PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) override {
        const auto& m = sys::MediaMonitor::Get().State();
        if (!m.hasSession) {
            draw::Glyph(dc, g::Music, {r.left + 34, r.top + 48}, 22, kTextFaint);
            draw::Text(dc, L"Şu anda çalan medya yok", {r.left + 64, r.top + 22, r.right - 12, r.top + 46}, 13.5f, kText,
                       DWRITE_FONT_WEIGHT_SEMI_BOLD);
            draw::Text(dc, L"Spotify veya uyumlu bir oynatıcıda başlatın.", {r.left + 64, r.top + 46, r.right - 10, r.top + 70},
                       11, kTextDim);
            return;
        }
        D2D1_RECT_F art{r.left + 14, r.top + 14, r.left + 82, r.top + 82};
        if (auto* bmp = sys::MediaMonitor::Get().Art()) {
            winrt::com_ptr<ID2D1RoundedRectangleGeometry> clip;
            gfx::Graphics::Get().D2D()->CreateRoundedRectangleGeometry({art, 10, 10}, clip.put());
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.get()), nullptr);
            draw::Bitmap(dc, bmp, art);
            dc->PopLayer();
        } else {
            draw::RoundRect(dc, art, 10, kSurfaceRaised);
            ID2D1Bitmap* icon = m.iconKey.empty() ? nullptr : gfx::IconCache::Get().Get(m.iconKey, static_cast<int>(32 * Scale()));
            float cx = (art.left + art.right) / 2, cy = (art.top + art.bottom) / 2;
            if (icon) draw::Bitmap(dc, icon, {cx - 16, cy - 16, cx + 16, cy + 16});
            else draw::Glyph(dc, g::Music, {cx, cy}, 22, kTextFaint);
        }
        float tx = art.right + 14;
        draw::Text(dc, m.title.empty() ? L"Bilinmeyen parça" : m.title, {tx, r.top + 16, r.right - 12, r.top + 38}, 14, kText,
                   DWRITE_FONT_WEIGHT_SEMI_BOLD);
        draw::Text(dc, m.artist, {tx, r.top + 38, r.right - 12, r.top + 58}, 12.5f, kTextDim);
        ID2D1Bitmap* smallIcon = m.iconKey.empty() ? nullptr : gfx::IconCache::Get().Get(m.iconKey, static_cast<int>(std::ceil(14 * Scale())));
        if (smallIcon) draw::Bitmap(dc, smallIcon, {tx, r.top + 63, tx + 14, r.top + 77});
        draw::Text(dc, m.appName, {tx + (smallIcon ? 19.0f : 0.0f), r.top + 60, r.right - 12, r.top + 80}, 11, kTextFaint);
        if (m.hasTimeline) {
            double pos = sys::MediaMonitor::Get().CurrentPosition(NowMs());
            D2D1_RECT_F bar{r.left + 14, r.top + 96, r.right - 14, r.top + 99};
            draw::RoundRect(dc, bar, 1.5f, Rgba(255, 255, 255, 0.14f));
            float f = m.durationSec > 0 ? static_cast<float>(pos / m.durationSec) : 0;
            draw::RoundRect(dc, {bar.left, bar.top, bar.left + (bar.right - bar.left) * f, bar.bottom}, 1.5f, kTextDim);
            draw::Text(dc, Duration(pos), {bar.left, bar.bottom + 1, bar.left + 60, bar.bottom + 15}, 10, kTextFaint, DWRITE_FONT_WEIGHT_NORMAL,
                       Align::Leading, true);
            draw::Text(dc, Duration(m.durationSec), {bar.right - 60, bar.bottom + 1, bar.right, bar.bottom + 15}, 10, kTextFaint,
                       DWRITE_FONT_WEIGHT_NORMAL, Align::Trailing, true);
        }
        for (const auto& b : Buttons()) {
            if (b.id == hot_ && b.enabled) draw::RoundRect(dc, b.r, 16, kSurfaceHover);
            wchar_t gl = b.id == 1 ? g::Previous : b.id == 3 ? g::Next : (m.playing ? g::Pause : g::Play);
            draw::Glyph(dc, gl, {(b.r.left + b.r.right) / 2, (b.r.top + b.r.bottom) / 2}, b.id == 2 ? 18.0f : 14.0f,
                        b.enabled ? kText : kTextFaint);
        }
    }
    int Hit(PointF p) const {
        for (const auto& b : Buttons())
            if (PointIn(b.r, p)) return b.id;
        return 0;
    }
    void OnMouseMove(PointF p) override {
        int h = sys::MediaMonitor::Get().State().hasSession ? Hit(p) : 0;
        if (h != hot_) {
            hot_ = h;
            Invalidate();
        }
    }
    void OnMouseLeave() override {
        hot_ = 0;
        Invalidate();
    }
    void OnMouseUp(PointF p, int b) override {
        if (b != 0 || !sys::MediaMonitor::Get().State().hasSession) return;
        Press(Hit(p));
    }
    void Press(int id) {
        const auto& m = sys::MediaMonitor::Get().State();
        if (id == 1 && m.canPrev) sys::MediaMonitor::Get().Previous();
        if (id == 2 && m.canPlayPause) sys::MediaMonitor::Get().PlayPause();
        if (id == 3 && m.canNext) sys::MediaMonitor::Get().Next();
    }
    bool OnKey(UINT vk) override {
        if (vk == VK_SPACE || vk == VK_RETURN) Press(2);
        else if (vk == VK_LEFT) Press(1);
        else if (vk == VK_RIGHT) Press(3);
        else return false;
        return true;
    }

private:
    int hot_ = 0;
    TimerId tick_ = 0;
};

// ============================================================================ Battery / devices
class BatteryPopup : public Popup {
public:
    void Refresh() {
        if (!IsOpen()) return;
        float h = ContentHeight();
        if (std::fabs(h - lastH_) > 0.5f) Relayout();
        Invalidate();
    }

protected:
    float ContentWidth() override { return 320; }
    float ContentHeight() override {
        auto devs = sys::BatteryService::Get().Devices();
        float h = 44 + 36; // header + footer
        if (devs.empty()) h += 64;
        for (const auto& d : devs) h += 50 + (d.parts.empty() ? 0.0f : 20.0f) + (d.note.empty() ? 0.0f : 16.0f);
        lastH_ = std::min(h, 460.0f);
        return lastH_;
    }
    static wchar_t KindGlyph(battery::DeviceKind k) {
        switch (k) {
        case battery::DeviceKind::Computer: return g::Laptop;
        case battery::DeviceKind::Mouse: return g::Mouse;
        case battery::DeviceKind::Keyboard: return g::Keyboard;
        case battery::DeviceKind::Headset: return g::Headset;
        case battery::DeviceKind::Headphones: return g::Headphones;
        case battery::DeviceKind::Earbuds: return g::Headphones;
        case battery::DeviceKind::Gamepad: return g::Gamepad;
        case battery::DeviceKind::Phone: return g::Phone;
        default: return g::Bluetooth;
        }
    }
    D2D1_RECT_F Footer() const {
        auto r = ContentRect();
        return {r.left + 8, r.bottom - 34, r.right - 8, r.bottom - 6};
    }
    void PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) override {
        draw::Text(dc, L"Piller", {r.left + 14, r.top + 10, r.right - 14, r.top + 34}, 14, kText, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        auto devs = sys::BatteryService::Get().Devices();
        float y = r.top + 44;
        int64_t now = WallMs() - 11644473600000LL; // unix ms (records use unix time)
        if (devs.empty()) {
            draw::Text(dc, L"Bu bilgisayarın pili yok ve pil bilgisi sunan bağlı bir kablosuz cihaz bulunamadı.",
                       {r.left + 14, y, r.right - 14, y + 52}, 11.5f, kTextDim);
            y += 64;
        }
        dc->PushAxisAlignedClip({r.left, r.top + 40, r.right, r.bottom - 36}, D2D1_ANTIALIAS_MODE_ALIASED);
        for (const auto& d : devs) {
            draw::Glyph(dc, KindGlyph(d.kind), {r.left + 26, y + 18}, 16, d.connected ? kText : kTextFaint);
            draw::Text(dc, d.name, {r.left + 48, y + 4, r.right - 90, y + 24}, 13, d.connected ? kText : kTextDim);
            std::wstring sub = battery::ConnectionName(d.connection);
            if (!d.connected) sub += L" · bağlı değil";
            else if (d.charge != battery::Charge::Unknown) sub += std::wstring(L" · ") + battery::ChargeName(d.charge);
            if (d.updatedMs && d.IsStale(now, 30 * 60 * 1000)) sub += L" · eski veri";
            draw::Text(dc, sub, {r.left + 48, y + 22, r.right - 90, y + 40}, 10.5f, kTextFaint);
            // percentage: unknown is shown as "—", never as 0%
            bool low = d.percent && *d.percent <= 15 && d.charge != battery::Charge::Charging;
            // Disconnected devices: Windows keeps the last value; it is labelled as such, never as live.
            draw::Text(dc, battery::FormatPercent(d.percent), {r.right - 88, y + 2, r.right - 14, y + 26}, 14,
                       !d.percent || !d.connected ? kTextFaint : low ? kBad : kText, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Trailing, true);
            if (d.percent && !d.connected) {
                draw::Text(dc, L"son bilinen", {r.right - 88, y + 24, r.right - 14, y + 40}, 10, kTextFaint, DWRITE_FONT_WEIGHT_NORMAL,
                           Align::Trailing);
            } else if (d.percent) {
                D2D1_RECT_F bar{r.right - 88, y + 30, r.right - 14, y + 33};
                draw::RoundRect(dc, bar, 1.5f, Rgba(255, 255, 255, 0.12f));
                draw::RoundRect(dc, {bar.left, bar.top, bar.left + (bar.right - bar.left) * (*d.percent / 100.0f), bar.bottom}, 1.5f,
                                low ? kBad : (d.charge == battery::Charge::Charging ? kGood : kTextDim));
            } else {
                draw::Text(dc, L"bilinmiyor", {r.right - 88, y + 24, r.right - 14, y + 40}, 10, kTextFaint, DWRITE_FONT_WEIGHT_NORMAL,
                           Align::Trailing);
            }
            y += 46;
            if (!d.parts.empty()) {
                std::wstring parts;
                for (const auto& p : d.parts) {
                    if (!parts.empty()) parts += L"   ";
                    parts += p.label + L" " + battery::FormatPercent(p.percent);
                }
                draw::Text(dc, parts, {r.left + 48, y - 4, r.right - 14, y + 14}, 11, kTextDim);
                y += 20;
            }
            if (!d.note.empty()) {
                draw::Text(dc, d.note, {r.left + 48, y - 4, r.right - 14, y + 12}, 10.5f, kWarn);
                y += 16;
            }
            y += 4;
        }
        dc->PopAxisAlignedClip();
        auto f = Footer();
        if (footHot_) draw::RoundRect(dc, f, 8, kSurfaceHover);
        draw::Text(dc, L"Bluetooth ve cihaz ayarları ›", {f.left + 8, f.top, f.right - 8, f.bottom}, 12, kTextDim);
    }
    void OnMouseMove(PointF p) override {
        bool h = PointIn(Footer(), p);
        if (h != footHot_) {
            footHot_ = h;
            Invalidate();
        }
    }
    void OnMouseUp(PointF p, int b) override {
        if (b == 0 && PointIn(Footer(), p)) {
            ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
            Close(false);
        }
    }

private:
    bool footHot_ = false;
    float lastH_ = 0;
};

// ============================================================================ Notifications
class NotificationsPopup : public Popup {
public:
    void Refresh() {
        if (!IsOpen()) return;
        Relayout();
        Invalidate();
    }

protected:
    float ContentWidth() override { return 340; }
    float ContentHeight() override {
        size_t n = App::Get().Notifications().Items().size();
        float h = 44 + 40 + (n == 0 ? 60.0f : std::min(static_cast<float>(n), 6.0f) * 62.0f);
        bool winOn = Store::Get().settings().windowsNotifications;
        if (!winOn || sys::NotificationMonitor::Get().Access() != sys::NotificationAccess::Allowed) h += 34;
        return h;
    }
    D2D1_RECT_F Footer() const {
        auto r = ContentRect();
        return {r.left + 8, r.bottom - 34, r.right - 8, r.bottom - 6};
    }
    D2D1_RECT_F InfoRow() const {
        auto r = ContentRect();
        return {r.left + 8, r.top + 40, r.right - 8, r.top + 70};
    }
    bool InfoShown() const {
        return !Store::Get().settings().windowsNotifications ||
               sys::NotificationMonitor::Get().Access() != sys::NotificationAccess::Allowed;
    }
    void PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) override {
        const auto& items = App::Get().Notifications().Items();
        draw::Text(dc, L"Bildirimler", {r.left + 14, r.top + 10, r.right - 14, r.top + 34}, 14, kText, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float y = r.top + 44;
        if (InfoShown()) {
            auto ir = InfoRow();
            if (hot_ == -2) draw::RoundRect(dc, ir, 8, kSurfaceHover);
            bool on = Store::Get().settings().windowsNotifications;
            std::wstring msg = !on ? L"Windows bildirimleri kapalı. Açmak için Ayarlar ›"
                                   : L"Windows bildirim erişimi yok. Ayarlar ›";
            draw::Glyph(dc, g::Info, {ir.left + 14, (ir.top + ir.bottom) / 2}, 13, kTextDim);
            draw::Text(dc, msg, {ir.left + 30, ir.top, ir.right - 8, ir.bottom}, 11.5f, kTextDim);
            y += 34;
        }
        rows_.clear();
        if (items.empty()) {
            draw::Text(dc, L"Bildirim yok", {r.left + 14, y + 10, r.right - 14, y + 40}, 12.5f, kTextDim, DWRITE_FONT_WEIGHT_NORMAL,
                       Align::Center);
        }
        int shown = 0;
        for (const auto& n : items) {
            if (++shown > 6) break;
            D2D1_RECT_F row{r.left + 8, y, r.right - 8, y + 58};
            rows_.push_back({row, n.key});
            bool hot = hot_ == static_cast<int>(rows_.size() - 1);
            draw::RoundRect(dc, row, 10, hot ? kSurfaceHover : Rgba(255, 255, 255, 0.035f));
            ID2D1Bitmap* icon = n.appId.empty() ? nullptr : gfx::IconCache::Get().Get(L"app:" + n.appId, static_cast<int>(16 * Scale()));
            float tx = row.left + 12;
            if (icon) {
                draw::Bitmap(dc, icon, {tx, row.top + 8, tx + 14, row.top + 22});
                tx += 19;
            }
            std::wstring src = (n.appName.empty() ? L"Uygulama" : n.appName);
            if (n.source == NotificationSource::Internal) src = L"NotchBar";
            draw::Text(dc, src + L" · " + Ago(n.arrivedMs), {tx, row.top + 5, row.right - 30, row.top + 23}, 10.5f, kTextFaint);
            draw::Text(dc, Ellipsize(n.title, 60), {row.left + 12, row.top + 22, row.right - 12, row.top + 40}, 12.5f, kText,
                       DWRITE_FONT_WEIGHT_SEMI_BOLD);
            draw::Text(dc, Ellipsize(n.body, 90), {row.left + 12, row.top + 38, row.right - 12, row.top + 55}, 11.5f, kTextDim);
            if (hot) draw::Glyph(dc, g::Close, {row.right - 16, row.top + 14}, 10, kTextDim);
            y += 62;
        }
        auto f = Footer();
        if (hot_ == -3) draw::RoundRect(dc, f, 8, kSurfaceHover);
        draw::Text(dc, L"Windows bildirim merkezini aç ›", {f.left + 8, f.top, f.right - 8, f.bottom}, 12, kTextDim);
    }
    int Hit(PointF p) const {
        if (PointIn(Footer(), p)) return -3;
        if (InfoShown() && PointIn(InfoRow(), p)) return -2;
        for (size_t i = 0; i < rows_.size(); ++i)
            if (PointIn(rows_[i].first, p)) return static_cast<int>(i);
        return -1;
    }
    void OnMouseMove(PointF p) override {
        int h = Hit(p);
        if (h != hot_) {
            hot_ = h;
            Invalidate();
        }
    }
    void OnMouseLeave() override {
        hot_ = -1;
        Invalidate();
    }
    void OnMouseUp(PointF p, int b) override {
        int h = Hit(p);
        if (h == -3) {
            // Windows 11 notification center; Settings as fallback.
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"ms-actioncenter:", nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                ShellExecuteW(nullptr, L"open", L"ms-settings:notifications", nullptr, nullptr, SW_SHOWNORMAL);
            Close(false);
        } else if (h == -2) {
            Close(false);
            Post([] { App::Get().Launcher().OpenSettings(); });
        } else if (h >= 0) {
            auto& row = rows_[h];
            if (p.x > row.first.right - 30 && p.y < row.first.top + 26) {
                // Hide from our list only; the Windows notification itself is untouched.
                App::Get().Notifications().RemoveLocal(row.second);
                Refresh();
            }
        }
    }

private:
    int hot_ = -1;
    std::vector<std::pair<D2D1_RECT_F, uint64_t>> rows_;
};

// ============================================================================ host
struct PopupHost::Impl {
    VolumePopup volume;
    MediaPopup media;
    BatteryPopup battery;
    NotificationsPopup notifications;
    Popup* Of(PopupKind k) {
        switch (k) {
        case PopupKind::Volume: return &volume;
        case PopupKind::Media: return &media;
        case PopupKind::Battery: return &battery;
        default: return &notifications;
        }
    }
};

PopupHost::PopupHost() : impl_(std::make_unique<Impl>()) {}
PopupHost::~PopupHost() = default;

void PopupHost::Init() {
    auto& d = *impl_;
    d.volume.Init(L"Ses");
    d.media.Init(L"Medya");
    d.battery.Init(L"Piller");
    d.notifications.Init(L"Bildirimler");
    for (auto k : {PopupKind::Volume, PopupKind::Media, PopupKind::Battery, PopupKind::Notifications}) {
        d.Of(k)->onClosed = [this, k] {
            if (k == PopupKind::Media) impl_->media.Closed();
            if (k == PopupKind::Battery) sys::BatteryService::Get().SetMenuOpen(false);
            if (onOpenChanged) onOpenChanged();
        };
    }
}

void PopupHost::Toggle(PopupKind k, POINT anchor) {
    auto& d = *impl_;
    Popup* p = d.Of(k);
    if (p->IsOpen()) {
        p->Close(true);
        return;
    }
    for (auto other : {PopupKind::Volume, PopupKind::Media, PopupKind::Battery, PopupKind::Notifications})
        if (other != k && d.Of(other)->IsOpen()) d.Of(other)->Close(false);
    if (k == PopupKind::Battery) sys::BatteryService::Get().SetMenuOpen(true);
    auto& sc = Screen::Current();
    // A visible gap separates the box from the notch.
    p->OpenAt(anchor.x, anchor.y + sc.Px(kPopupGap));
    if (k == PopupKind::Media) d.media.Opened();
    if (onOpenChanged) onOpenChanged();
}

void PopupHost::CloseAll() {
    auto& d = *impl_;
    for (auto k : {PopupKind::Volume, PopupKind::Media, PopupKind::Battery, PopupKind::Notifications})
        if (d.Of(k)->IsOpen()) d.Of(k)->Close(false);
}

bool PopupHost::AnyOpen() const {
    auto& d = *impl_;
    return d.volume.IsOpen() || d.media.IsOpen() || d.battery.IsOpen() || d.notifications.IsOpen();
}

bool PopupHost::IsOpen(PopupKind k) const { return impl_->Of(k)->IsOpen(); }

void PopupHost::Refresh(PopupKind k) {
    auto& d = *impl_;
    switch (k) {
    case PopupKind::Volume: d.volume.Refresh(); break;
    case PopupKind::Media: d.media.Refresh(); break;
    case PopupKind::Battery: d.battery.Refresh(); break;
    case PopupKind::Notifications: d.notifications.Refresh(); break;
    }
}

} // namespace nb::ui
