#include "pch.h"

#include "ui/Popups.h"

#include "app/App.h"
#include "core/Dispatcher.h"
#include "core/Store.h"
#include "core/Worker.h"
#include "core/Strings.h"
#include "gfx/Draw.h"
#include "gfx/Graphics.h"
#include "gfx/IconCache.h"
#include "gfx/Theme.h"
#include "sys/AudioDevices.h"
#include "sys/AudioMonitor.h"
#include "sys/MediaMonitor.h"
#include "sys/NotificationMonitor.h"
#include "sys/NetworkMonitor.h"
#include "sys/PowerMonitor.h"
#include "sys/QuickSettings.h"
#include "sys/QuickTimer.h"
#include "sys/WifiList.h"
#include "sys/TrayApps.h"
#include "sys/battery/BatteryService.h"
#include "sys/battery/CustomBattery.h"
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

// Sends Win+<key> (e.g. Win+N for the notification center). Our own keyboard hook lets
// combinations pass untouched.
void SendWinCombo(WORD key) {
    INPUT in[4]{};
    for (auto& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN;
    in[1].ki.wVk = key;
    in[2].ki.wVk = key;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_LWIN;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}
} // namespace

// ============================================================================ Sound (output / microphone)
// Two views behind two buttons: output (master volume, bass if the driver has it, output devices,
// per-app volumes) and microphone (level, boost if the driver has it, input devices).
class VolumePopup : public Popup {
public:
    void Refresh() {
        if (IsOpen()) Invalidate();
    }
    void Prepare() { // called right before the box opens
        input_ = false;
        anim_.Clear();
        devicesOpen_ = mixerOpen_ = false; // lists stay folded until their button is pressed
        drag_ = down_ = -1;
        Read();
        Build();
        ui::ClearTimeout(poll_);
        poll_ = ui::SetTimeout(1500, [this] { Poll(); });
    }

protected:
    static constexpr float kW = 300;
    enum Kind { kTabOut, kTabIn, kSettings, kMute, kSlider, kEffect, kDevHeader, kDevice, kMixHeader, kAppMute, kAppSlider };
    struct Item {
        Kind k;
        D2D1_RECT_F r; // relative to the content rect
        int i;
    };

    float ContentWidth() override { return kW; }
    float ContentHeight() override { return height_; }

    void Read() {
        devices_ = sys::ListAudioDevices(input_);
        if (input_) cap_ = sys::GetCapture();
        else if (mixerOpen_) apps_ = sys::ListAppVolumes(); // only needed while the mixer is unfolded
        fx_ = sys::GetEffect(input_);
    }
    void Build() {
        items_.clear();
        seps_.clear();
        float y = 8;
        items_.push_back({kTabOut, {10, y, 50, y + 30}, 0});
        items_.push_back({kTabIn, {54, y, 94, y + 30}, 0});
        items_.push_back({kSettings, {kW - 44, y, kW - 10, y + 30}, 0});
        y += 40;
        items_.push_back({kMute, {10, y, 44, y + 32}, 0});
        items_.push_back({kSlider, {52, y, kW - 62, y + 32}, 0});
        y += 38;
        if (fx_) {
            items_.push_back({kEffect, {52, y, kW - 62, y + 28}, 0});
            y += 30;
        }
        y += 4;
        seps_.push_back(y);
        y += 6;
        // Device list: folded behind a button that shows the current device.
        items_.push_back({kDevHeader, {6, y, kW - 6, y + 40}, 0});
        y += 42;
        if (devicesOpen_) {
            int shown = 0;
            for (size_t i = 0; i < devices_.size() && shown < 6; ++i, ++shown) {
                items_.push_back({kDevice, {6, y, kW - 6, y + 30}, static_cast<int>(i)});
                y += 30;
            }
            y += 2;
        }
        // Per-app mixer (output only): also folded.
        if (!input_) {
            items_.push_back({kMixHeader, {6, y, kW - 6, y + 34}, 0});
            y += 36;
            if (mixerOpen_) {
                if (apps_.empty()) y += 26; // "no app is playing" line
                for (size_t i = 0; i < apps_.size() && i < 8; ++i) {
                    items_.push_back({kAppMute, {10, y + 3, 36, y + 31}, static_cast<int>(i)});
                    items_.push_back({kAppSlider, {150, y, kW - 14, y + 34}, static_cast<int>(i)});
                    y += 34;
                }
            }
        }
        height_ = y + 6;
    }
    void Poll() {
        if (!IsOpen()) return;
        if (drag_ < 0) {
            float before = height_;
            Read();
            Build();
            if (height_ != before) Relayout();
            Invalidate();
        }
        poll_ = ui::SetTimeout(1500, [this] { Poll(); });
    }
    void SwitchView(bool input) {
        if (input_ == input) return;
        input_ = input;
        devicesOpen_ = false;
        Read();
        Build();
        Relayout();
        Invalidate();
    }

    D2D1_RECT_F Abs(const D2D1_RECT_F& r) const {
        auto c = ContentRect();
        return {c.left + r.left, c.top + r.top, c.left + r.right, c.top + r.bottom};
    }
    int Hit(PointF p) const {
        for (size_t i = 0; i < items_.size(); ++i)
            if (PointIn(Abs(items_[i].r), p)) return static_cast<int>(i);
        return -1;
    }
    float MasterLevel(bool* muted) const {
        if (input_) {
            *muted = cap_.muted;
            return cap_.level;
        }
        const auto& s = sys::AudioMonitor::Get().State();
        *muted = s.muted;
        return s.available ? s.level : 0;
    }
    void SetFromX(const Item& it, float x) {
        auto a = Abs(it.r);
        float v = std::clamp((x - (a.left + 6)) / ((a.right - 6) - (a.left + 6)), 0.0f, 1.0f);
        if (it.k == kSlider) {
            if (input_) {
                sys::SetCaptureLevel(v);
                cap_.level = v;
                if (v > 0) cap_.muted = false;
            } else sys::AudioMonitor::Get().SetLevel(v);
        } else if (it.k == kEffect) {
            sys::SetEffect(input_, v);
            if (fx_) fx_->value = v;
        } else if (it.k == kAppSlider && it.i < static_cast<int>(apps_.size())) {
            sys::SetAppVolume(apps_[it.i].id, v);
            apps_[it.i].level = v;
            if (v > 0) apps_[it.i].muted = false;
        }
        Invalidate();
    }
    void DrawSlider(ID2D1DeviceContext* dc, const D2D1_RECT_F& a, float v, bool hot, bool dim) {
        float t0 = a.left + 6, t1 = a.right - 6, cy = (a.top + a.bottom) / 2;
        draw::RoundRect(dc, {t0, cy - 2, t1, cy + 2}, 2, Rgba(255, 255, 255, 0.14f));
        float x = t0 + (t1 - t0) * v;
        draw::RoundRect(dc, {t0, cy - 2, x, cy + 2}, 2, dim ? kTextFaint : kText);
        float rr = hot ? 7.0f : 6.0f;
        dc->FillEllipse(D2D1::Ellipse({x, cy}, rr, rr), draw::Brush(dc, Rgba(245, 245, 247)));
    }

    void PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) override {
        bool masterMuted = false;
        float master = MasterLevel(&masterMuted);
        for (size_t n = 0; n < items_.size(); ++n) {
            const Item& it = items_[n];
            auto a = Abs(it.r);
            bool hot = hot_ == static_cast<int>(n) || drag_ == static_cast<int>(n);
            float hv = HoverOf(static_cast<int>(n));
            auto HL = [&](const D2D1_RECT_F& rr) {
                if (hv > 0.01f) draw::RoundRect(dc, rr, 8, Rgba(255, 255, 255, 0.07f * hv + 0.05f * PressOf(static_cast<int>(n))));
            };
            D2D1_POINT_2F c{(a.left + a.right) / 2, (a.top + a.bottom) / 2};
            switch (it.k) {
            case kTabOut:
            case kTabIn: {
                bool active = (it.k == kTabIn) == input_;
                if (active) draw::RoundRect(dc, a, 8, kSurfaceRaised);
                else HL(a);
                const auto& s = sys::AudioMonitor::Get().State();
                draw::Glyph(dc, it.k == kTabIn ? g::Microphone : draw::VolumeGlyph(s.level, s.muted), c, 15, active ? kText : kTextDim);
                break;
            }
            case kSettings:
                HL(a);
                draw::Glyph(dc, g::Settings, c, 14, kTextDim);
                break;
            case kMute:
                HL(a);
                draw::Glyph(dc, input_ ? g::Microphone : draw::VolumeGlyph(master, masterMuted), c, 16, masterMuted ? kTextFaint : kText);
                break;
            case kSlider: {
                DrawSlider(dc, a, masterMuted ? 0.0f : master, hot, masterMuted);
                std::wstring pct = masterMuted ? L"—" : std::to_wstring(static_cast<int>(std::lround(master * 100))) + L"%";
                draw::Text(dc, pct, {a.right + 2, a.top, a.right + 52, a.bottom}, 12.5f, kText, DWRITE_FONT_WEIGHT_NORMAL, Align::Trailing, true);
                break;
            }
            case kEffect:
                if (fx_) {
                    DrawSlider(dc, a, fx_->value, hot, false);
                    draw::Text(dc, fx_->name, {r.left + 12, a.top, a.left, a.bottom}, 11.5f, kTextDim);
                }
                break;
            case kDevHeader: {
                std::wstring cur = L"—";
                for (const auto& d : devices_)
                    if (d.isDefault) cur = d.name;
                HL(a);
                draw::Glyph(dc, input_ ? g::Microphone : g::Speaker, {a.left + 18, c.y}, 14, kTextDim);
                draw::Text(dc, input_ ? L"Giriş aygıtı" : L"Çıkış aygıtı", {a.left + 36, a.top + 4, a.right - 30, a.top + 20}, 10.5f, kTextFaint);
                draw::Text(dc, Ellipsize(cur, 34), {a.left + 36, a.top + 19, a.right - 30, a.bottom - 2}, 12.5f, kText);
                draw::Glyph(dc, devicesOpen_ ? g::ChevronUp : g::ChevronDown, {a.right - 16, c.y}, 11, kTextDim);
                break;
            }
            case kMixHeader: {
                HL(a);
                draw::Glyph(dc, g::Speaker, {a.left + 18, c.y}, 14, kTextDim);
                draw::Text(dc, L"Ses karıştırıcısı", {a.left + 36, a.top, a.right - 70, a.bottom}, 12.5f, kText);
                if (mixerOpen_ && !apps_.empty())
                    draw::Text(dc, std::to_wstring(apps_.size()) + L" uygulama", {a.right - 100, a.top, a.right - 30, a.bottom}, 11, kTextFaint,
                               DWRITE_FONT_WEIGHT_NORMAL, Align::Trailing);
                draw::Glyph(dc, mixerOpen_ ? g::ChevronUp : g::ChevronDown, {a.right - 16, c.y}, 11, kTextDim);
                if (mixerOpen_ && apps_.empty())
                    draw::Text(dc, L"Şu anda ses çalan uygulama yok", {a.left + 36, a.bottom + 2, a.right - 8, a.bottom + 26}, 11.5f, kTextFaint);
                break;
            }
            case kDevice: {
                const auto& d = devices_[it.i];
                HL(a);
                if (d.isDefault) draw::Glyph(dc, g::Check, {a.left + 16, c.y}, 12, kAccent);
                draw::Text(dc, Ellipsize(d.name, 36), {a.left + 32, a.top, a.right - 8, a.bottom}, 12.5f, d.isDefault ? kText : kTextDim);
                break;
            }
            case kAppMute: {
                const auto& ap = apps_[it.i];
                float al = ap.muted ? 0.4f : 1.0f;
                HL(a);
                ID2D1Bitmap* bmp = ap.exePath.empty() ? nullptr : gfx::IconCache::Get().Get(L"file:" + ap.exePath, static_cast<int>(std::ceil(18 * Scale())));
                if (bmp) draw::Bitmap(dc, bmp, {c.x - 9, c.y - 9, c.x + 9, c.y + 9}, al);
                else draw::Glyph(dc, g::Speaker, c, 14, kTextDim);
                draw::Text(dc, Ellipsize(ap.name, 16), {a.right + 4, a.top - 3, 150 + r.left - 6, a.bottom + 3}, 12, ap.muted ? kTextFaint : kText);
                break;
            }
            case kAppSlider: {
                const auto& ap = apps_[it.i];
                DrawSlider(dc, a, ap.muted ? 0.0f : ap.level, hot, ap.muted);
                break;
            }
            }
        }
        for (float y : seps_) dc->DrawLine({r.left + 14, r.top + y}, {r.right - 14, r.top + y}, draw::Brush(dc, Rgba(255, 255, 255, 0.08f)), 1);
    }

    void OnMouseMove(PointF p) override {
        if (drag_ >= 0) {
            SetFromX(items_[drag_], p.x);
            return;
        }
        int h = Hit(p);
        if (h != hot_) {
            hot_ = h;
            HotChanged(h);
            Invalidate();
        }
    }
    void OnMouseLeave() override {
        hot_ = -1;
        HotChanged(-1);
        PressedChanged(-1);
        Invalidate();
    }
    void OnMouseDown(PointF p, int b) override {
        if (b != 0) return;
        int h = Hit(p);
        down_ = h;
        if (h >= 0 && items_[h].k != kSlider && items_[h].k != kEffect && items_[h].k != kAppSlider) PressedChanged(h);
        if (h >= 0 && (items_[h].k == kSlider || items_[h].k == kEffect || items_[h].k == kAppSlider)) {
            drag_ = h;
            SetFromX(items_[h], p.x);
        }
    }
    void OnMouseUp(PointF p, int b) override {
        PressedChanged(-1);
        if (drag_ >= 0) {
            drag_ = -1;
            down_ = -1;
            Invalidate();
            return;
        }
        int h = Hit(p);
        int was = down_;
        down_ = -1;
        if (b != 0 || h < 0 || h != was) return;
        const Item it = items_[h];
        switch (it.k) {
        case kTabOut: SwitchView(false); break;
        case kTabIn: SwitchView(true); break;
        case kSettings:
            ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
            Close(false);
            break;
        case kMute:
            if (input_) {
                sys::SetCaptureMuted(!cap_.muted);
                cap_.muted = !cap_.muted;
            } else sys::AudioMonitor::Get().SetMuted(!sys::AudioMonitor::Get().State().muted);
            Invalidate();
            break;
        case kDevHeader:
        case kMixHeader: {
            (it.k == kDevHeader ? devicesOpen_ : mixerOpen_) ^= true;
            if (mixerOpen_ && !input_) apps_ = sys::ListAppVolumes();
            Build();
            Relayout();
            Invalidate();
            break;
        }
        case kDevice:
            sys::SetDefaultAudioDevice(devices_[it.i].id);
            devicesOpen_ = false;
            Read();
            Build();
            Relayout();
            Invalidate();
            break;
        case kAppMute:
            sys::SetAppMuted(apps_[it.i].id, !apps_[it.i].muted);
            apps_[it.i].muted = !apps_[it.i].muted;
            Invalidate();
            break;
        default: break;
        }
    }
    void OnWheel(PointF, int delta) override {
        float step = delta > 0 ? 0.02f : -0.02f;
        if (input_) {
            cap_.level = std::clamp(cap_.level + step, 0.0f, 1.0f);
            sys::SetCaptureLevel(cap_.level);
            Invalidate();
        } else {
            auto& a = sys::AudioMonitor::Get();
            a.SetLevel(a.State().level + step);
        }
    }
    bool OnKey(UINT vk) override {
        if (vk == VK_TAB) SwitchView(!input_);
        else if (vk == VK_RIGHT || vk == VK_UP || vk == VK_LEFT || vk == VK_DOWN) OnWheel({}, (vk == VK_RIGHT || vk == VK_UP) ? 1 : -1);
        else if (vk == 'M' || vk == VK_SPACE) {
            if (input_) {
                sys::SetCaptureMuted(!cap_.muted);
                cap_.muted = !cap_.muted;
                Invalidate();
            } else sys::AudioMonitor::Get().SetMuted(!sys::AudioMonitor::Get().State().muted);
        } else return false;
        return true;
    }

private:
    bool input_ = false;
    bool devicesOpen_ = false, mixerOpen_ = false;
    int hot_ = -1, drag_ = -1, down_ = -1;
    float height_ = 160;
    std::vector<Item> items_;
    std::vector<float> seps_;
    std::vector<sys::AudioDevice> devices_;
    std::vector<sys::AppVolume> apps_;
    sys::CaptureState cap_;
    std::optional<sys::DeviceEffect> fx_;
    ui::TimerId poll_ = 0;
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
    // When several apps publish media (e.g. Spotify and a browser), small app icons at the bottom
    // left let the user choose which one this box shows and controls.
    std::vector<std::pair<D2D1_RECT_F, int>> Chips() const {
        std::vector<std::pair<D2D1_RECT_F, int>> out;
        const auto& m = sys::MediaMonitor::Get().State();
        if (m.sources.size() < 2) return out;
        auto r = ContentRect();
        float cy = r.bottom - 22;
        for (size_t i = 0; i < m.sources.size() && i < 3; ++i) {
            float x = r.left + 14 + static_cast<float>(i) * 26;
            out.push_back({{x, cy - 12, x + 24, cy + 12}, static_cast<int>(i)});
        }
        return out;
    }
    void PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) override {
        const auto& m = sys::MediaMonitor::Get().State();
        if (m.hasSession) {
            for (const auto& [cr, idx] : Chips()) {
                const auto& src = m.sources[static_cast<size_t>(idx)];
                if (src.selected) draw::RoundRect(dc, cr, 7, kSurfaceRaised);
                else if (HoverOf(10 + idx) > 0.01f) draw::RoundRect(dc, cr, 7, Rgba(255, 255, 255, 0.07f * HoverOf(10 + idx)));
                float cx = (cr.left + cr.right) / 2, cy = (cr.top + cr.bottom) / 2;
                ID2D1Bitmap* ic = src.iconKey.empty() ? nullptr : gfx::IconCache::Get().Get(src.iconKey, static_cast<int>(std::ceil(16 * Scale())));
                float al = src.selected ? 1.0f : 0.6f;
                if (ic) draw::Bitmap(dc, ic, {cx - 8, cy - 8, cx + 8, cy + 8}, al);
                else draw::Glyph(dc, g::Music, {cx, cy}, 13, kTextDim);
                if (src.playing) dc->FillEllipse(D2D1::Ellipse({cr.right - 4, cr.bottom - 4}, 2.2f, 2.2f), draw::Brush(dc, kGood));
            }
        }
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
            if (b.enabled && HoverOf(b.id) > 0.01f)
                draw::RoundRect(dc, b.r, 16, Rgba(255, 255, 255, 0.07f * HoverOf(b.id) + 0.06f * PressOf(b.id)));
            wchar_t gl = b.id == 1 ? g::Previous : b.id == 3 ? g::Next : (m.playing ? g::Pause : g::Play);
            draw::Glyph(dc, gl, {(b.r.left + b.r.right) / 2, (b.r.top + b.r.bottom) / 2}, b.id == 2 ? 18.0f : 14.0f,
                        b.enabled ? kText : kTextFaint);
        }
    }
    int Hit(PointF p) const {
        for (const auto& b : Buttons())
            if (PointIn(b.r, p)) return b.id;
        for (const auto& [cr, idx] : Chips())
            if (PointIn(cr, p)) return 10 + idx;
        return 0;
    }
    void OnMouseMove(PointF p) override {
        int h = sys::MediaMonitor::Get().State().hasSession ? Hit(p) : 0;
        if (h != hot_) {
            hot_ = h;
            HotChanged(h == 0 ? -1 : h);
            Invalidate();
        }
    }
    void OnMouseLeave() override {
        hot_ = 0;
        HotChanged(-1);
        PressedChanged(-1);
        Invalidate();
    }
    void OnMouseDown(PointF p, int b) override {
        if (b == 0 && sys::MediaMonitor::Get().State().hasSession) PressedChanged(Hit(p) == 0 ? -1 : Hit(p));
    }
    void OnMouseUp(PointF p, int b) override {
        PressedChanged(-1);
        if (b != 0 || !sys::MediaMonitor::Get().State().hasSession) return;
        Press(Hit(p));
    }
    void Press(int id) {
        const auto& m = sys::MediaMonitor::Get().State();
        if (id >= 10 && static_cast<size_t>(id - 10) < m.sources.size()) {
            std::wstring appId = m.sources[static_cast<size_t>(id - 10)].appId;
            sys::MediaMonitor::Get().SelectSource(appId);
            return;
        }
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
        for (const auto& d : devs) h += 50 + (d.parts.empty() ? 0.0f : 20.0f);
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
        rows_.clear();
        for (const auto& d : devs) {
            float rowTop = y;
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
            y += 4;
            rows_.push_back({{r.left + 4, rowTop - 2, r.right - 4, y}, d.id});
        }
        dc->PopAxisAlignedClip();
        auto fa = FootAdd(), fs = FootSettings();
        if (HoverOf(1003) > 0.01f) draw::RoundRect(dc, fa, 8, Rgba(255, 255, 255, 0.07f * HoverOf(1003)));
        if (HoverOf(1004) > 0.01f) draw::RoundRect(dc, fs, 8, Rgba(255, 255, 255, 0.07f * HoverOf(1004)));
        draw::Glyph(dc, g::Add, {fa.left + 16, (fa.top + fa.bottom) / 2}, 11, kTextDim);
        draw::Text(dc, L"Cihaz ekle", {fa.left + 30, fa.top, fa.right - 4, fa.bottom}, 12, kTextDim);
        draw::Text(dc, L"Bluetooth ayarları ›", {fs.left + 4, fs.top, fs.right - 8, fs.bottom}, 12, kTextDim, DWRITE_FONT_WEIGHT_NORMAL,
                   Align::Trailing);
    }
    D2D1_RECT_F FootAdd() const {
        auto f = Footer();
        return {f.left, f.top, f.left + (f.right - f.left) * 0.42f, f.bottom};
    }
    D2D1_RECT_F FootSettings() const {
        auto f = Footer();
        return {f.left + (f.right - f.left) * 0.42f, f.top, f.right, f.bottom};
    }
    int Hit(PointF p) const {
        if (PointIn(FootAdd(), p)) return 1003;
        if (PointIn(FootSettings(), p)) return 1004;
        return -1;
    }
    void OnMouseMove(PointF p) override {
        int h = Hit(p);
        if (h != hot_) {
            hot_ = h;
            HotChanged(h);
            Invalidate();
        }
    }
    void OnMouseLeave() override {
        hot_ = -1;
        HotChanged(-1);
        PressedChanged(-1);
        Invalidate();
    }
    void OnMouseDown(PointF p, int b) override {
        if (b == 0) PressedChanged(Hit(p));
    }
    void OnMouseUp(PointF p, int b) override {
        PressedChanged(-1);
        int h = Hit(p);
        if (b == 0 && h == 1004) {
            ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
            Close(false);
        } else if (b == 0 && h == 1003) {
            // "Cihaz ekle": the picker takes this box's place.
            auto wr = WindowRect();
            auto& sc = Screen::Current();
            POINT anchor{(wr.left + wr.right) / 2, wr.top + sc.Px(margin_) - sc.Px(kPopupGap)};
            Close(false);
            Post([anchor] { App::Get().Popups().Toggle(PopupKind::Picker, anchor); });
        } else if (b == 1) {
            // Right click on a device the user added: offer to remove it.
            for (const auto& [rect, id] : rows_) {
                if (!PointIn(rect, p) || id.rfind("custom:", 0) != 0) continue;
                std::string cid = id.substr(7);
                std::vector<MenuItem> menu;
                menu.push_back({L"Kaldır", g::Delete, [this, cid] {
                                    auto& v = Store::Get().settings().customBattery;
                                    v.erase(std::remove_if(v.begin(), v.end(), [&](const CustomBatterySource& s) { return s.id == cid; }), v.end());
                                    Store::Get().SaveSettings();
                                    sys::BatteryService::Get().RefreshCustom();
                                    Refresh();
                                }});
                POINT at;
                GetCursorPos(&at);
                MenuPopup::Get().Show(std::move(menu), at);
                break;
            }
        }
    }

private:
    int hot_ = -1;
    std::vector<std::pair<D2D1_RECT_F, std::string>> rows_;
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
            if (HoverOf(1002) > 0.01f) draw::RoundRect(dc, ir, 8, Rgba(255, 255, 255, 0.07f * HoverOf(1002)));
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
            float hv = HoverOf(static_cast<int>(rows_.size() - 1));
            bool hot = hv > 0.5f;
            draw::RoundRect(dc, row, 10, Rgba(255, 255, 255, 0.035f + 0.045f * hv + 0.04f * PressOf(static_cast<int>(rows_.size() - 1))));
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
        auto fc = FooterClear(), fo = FooterCenter();
        if (HoverOf(1004) > 0.01f) draw::RoundRect(dc, fc, 8, Rgba(255, 255, 255, 0.07f * HoverOf(1004)));
        if (HoverOf(1003) > 0.01f) draw::RoundRect(dc, fo, 8, Rgba(255, 255, 255, 0.07f * HoverOf(1003)));
        draw::Text(dc, L"Tümünü temizle", {fc.left + 8, fc.top, fc.right - 4, fc.bottom}, 12, items.empty() ? kTextFaint : kTextDim);
        draw::Text(dc, L"Bildirim merkezi ›", {fo.left + 4, fo.top, fo.right - 8, fo.bottom}, 12, kTextDim, DWRITE_FONT_WEIGHT_NORMAL,
                   Align::Trailing);
    }
    D2D1_RECT_F FooterClear() const {
        auto f = Footer();
        return {f.left, f.top, f.left + (f.right - f.left) * 0.45f, f.bottom};
    }
    D2D1_RECT_F FooterCenter() const {
        auto f = Footer();
        return {f.left + (f.right - f.left) * 0.45f, f.top, f.right, f.bottom};
    }
    int Hit(PointF p) const {
        if (PointIn(FooterClear(), p)) return -4;
        if (PointIn(FooterCenter(), p)) return -3;
        if (InfoShown() && PointIn(InfoRow(), p)) return -2;
        for (size_t i = 0; i < rows_.size(); ++i)
            if (PointIn(rows_[i].first, p)) return static_cast<int>(i);
        return -1;
    }
    void OnMouseMove(PointF p) override {
        int h = Hit(p);
        if (h != hot_) {
            hot_ = h;
            HotChanged(h >= 0 ? h : (h == -1 ? -1 : 1000 - h)); // footer / info row ids are negative
            Invalidate();
        }
    }
    void OnMouseLeave() override {
        hot_ = -1;
        HotChanged(-1);
        PressedChanged(-1);
        Invalidate();
    }
    void OnMouseDown(PointF p, int b) override {
        int h = Hit(p);
        if (b == 0) PressedChanged(h >= 0 ? h : (h == -1 ? -1 : 1000 - h));
    }
    void OnMouseUp(PointF p, int b) override {
        PressedChanged(-1);
        int h = Hit(p);
        if (b != 0) return;
        if (h == -4) {
            Post([] { App::Get().ClearNotifications(); });
        } else if (h == -3) {
            // Windows 11 notification center = Win+N (the ms-actioncenter: link opens nothing there).
            Close(false);
            Post([] { SendWinCombo('N'); });
        } else if (h == -2) {
            Close(false);
            Post([] { App::Get().Launcher().OpenSettings(); });
        } else if (h >= 0) {
            auto& row = rows_[h];
            uint64_t key = row.second;
            bool x = p.x > row.first.right - 30 && p.y < row.first.top + 26;
            const NotificationItem* found = nullptr;
            for (const auto& n : App::Get().Notifications().Items())
                if (n.key == key) found = &n;
            if (!found) return;
            NotificationItem n = *found;
            if (x) {
                Post([n] { App::Get().DismissNotification(n); }); // dismissed in Windows as well
            } else {
                // Click on the notification: open its app and clear it.
                Close(false);
                Post([n] { App::Get().ActivateNotification(n); });
            }
        }
    }

private:
    int hot_ = -1;
    std::vector<std::pair<D2D1_RECT_F, uint64_t>> rows_;
};

// ============================================================================ Calendar
// Windows-style flyout: today's date big on top, month with up/down arrows, round day cells,
// neighbouring months dimmed (click one to jump there).
class CalendarPopup : public Popup {
public:
    void Prepare() { // opens on the current month
        SYSTEMTIME st;
        GetLocalTime(&st);
        year_ = st.wYear;
        month_ = st.wMonth;
        sel_ = {st.wYear, st.wMonth, st.wDay};
        hot_ = {0, 0};
        tick_ = ui::SetTimeout(50, [this] { Tick(); });
    }

protected:
    static constexpr float kW = 320, kHeadH = 48, kMonthY = 56, kMonthH = 30, kWeekY = 92, kWeekH = 22, kGridY = 116, kRowH = 36,
                           kQuickH = 96;
    float ContentWidth() override { return kW; }
    float ContentHeight() override { return kGridY + 6 * kRowH + 8 + kQuickH; }

    // Quick timer / focus session under the month.
    struct Chip {
        D2D1_RECT_F r;
        sys::TimerKind kind;
        int minutes;
    };
    float QuickTop() const { return ContentRect().top + kGridY + 6 * kRowH + 8; }
    std::vector<Chip> Chips() const {
        std::vector<Chip> v;
        if (sys::QuickTimer::Get().Running()) return v;
        auto r = ContentRect();
        static const int timer[] = {5, 10, 15, 30}, focus[] = {15, 25, 45, 60};
        for (int row = 0; row < 2; ++row) {
            float y = QuickTop() + 12 + row * 38;
            for (int i = 0; i < 4; ++i) {
                float x = r.left + 56 + i * 62;
                v.push_back({{x, y, x + 56, y + 30}, row == 0 ? sys::TimerKind::Timer : sys::TimerKind::Focus, row == 0 ? timer[i] : focus[i]});
            }
        }
        return v;
    }
    D2D1_RECT_F CancelBtn() const { auto r = ContentRect(); float y = QuickTop() + 24; return {r.right - 52, y, r.right - 18, y + 34}; }
    void Tick() { // repaint once a second while a countdown is shown
        ui::ClearTimeout(tick_);
        if (!IsOpen() || !sys::QuickTimer::Get().Running()) return;
        Invalidate();
        tick_ = ui::SetTimeout(1000, [this] { Tick(); });
    }
    ui::TimerId tick_ = 0;
public:
    void Changed() { Invalidate(); Tick(); }
protected:

    static int DaysIn(int y, int m) {
        static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        return m == 2 && leap ? 29 : d[m - 1];
    }
    static int FirstDow() { // 0 = Monday ... 6 = Sunday (Windows' LOCALE_IFIRSTDAYOFWEEK)
        DWORD v = 0;
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&v), 2);
        return static_cast<int>(v);
    }
    static std::wstring Info(LCTYPE t) {
        wchar_t b[80];
        return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, t, b, 80) > 0 ? b : L"";
    }
    static int DowMonday(int y, int m, int d) { // 0 = Monday
        SYSTEMTIME s{};
        s.wYear = static_cast<WORD>(y);
        s.wMonth = static_cast<WORD>(m);
        s.wDay = static_cast<WORD>(d);
        FILETIME ft;
        SystemTimeToFileTime(&s, &ft);
        SYSTEMTIME s2{};
        FileTimeToSystemTime(&ft, &s2);
        return (s2.wDayOfWeek + 6) % 7;
    }
    void Step(int d) {
        month_ += d;
        if (month_ < 1) { month_ = 12; --year_; }
        if (month_ > 12) { month_ = 1; ++year_; }
        year_ = std::clamp(year_, 1601, 9998);
        Invalidate();
    }

    struct Day { int y, m, d; bool inMonth; };
    Day DayAt(int idx) const { // idx 0..41 of the 6x7 grid
        int off = (DowMonday(year_, month_, 1) - FirstDow() + 7) % 7;
        int pm = month_ == 1 ? 12 : month_ - 1, py = month_ == 1 ? year_ - 1 : year_;
        int nm = month_ == 12 ? 1 : month_ + 1, ny = month_ == 12 ? year_ + 1 : year_;
        int n = DaysIn(year_, month_), d = idx - off + 1;
        if (d < 1) return {py, pm, DaysIn(py, pm) + d, false};
        if (d > n) return {ny, nm, d - n, false};
        return {year_, month_, d, true};
    }

    D2D1_RECT_F UpBtn() const { auto r = ContentRect(); return {r.right - 78, r.top + kMonthY, r.right - 44, r.top + kMonthY + kMonthH}; }
    D2D1_RECT_F DownBtn() const { auto r = ContentRect(); return {r.right - 44, r.top + kMonthY, r.right - 10, r.top + kMonthY + kMonthH}; }
    D2D1_RECT_F Title() const { auto r = ContentRect(); return {r.left + 10, r.top + kMonthY, r.right - 84, r.top + kMonthY + kMonthH}; }
    D2D1_RECT_F Head() const { auto r = ContentRect(); return {r.left + 8, r.top + 6, r.right - 8, r.top + kHeadH - 4}; } // click: date settings
    D2D1_RECT_F Cell(int idx) const {
        auto r = ContentRect();
        float cw = (r.right - r.left - 24) / 7;
        float x = r.left + 12 + (idx % 7) * cw, y = r.top + kGridY + (idx / 7) * kRowH;
        return {x, y, x + cw, y + kRowH};
    }
    static D2D1_RECT_F Circle(const D2D1_RECT_F& c) {
        float cx = (c.left + c.right) / 2, cy = (c.top + c.bottom) / 2, rr = 16;
        return {cx - rr, cy - rr, cx + rr, cy + rr};
    }
    // kinds: 1 up, 2 down, 3 title, 4 footer, 5 day (second = grid index)
    std::pair<int, int> Hit(PointF p) const {
        if (PointIn(UpBtn(), p)) return {1, 0};
        if (PointIn(DownBtn(), p)) return {2, 0};
        if (PointIn(Title(), p)) return {3, 0};
        if (PointIn(Head(), p)) return {4, 0};
        for (int i = 0; i < 42; ++i)
            if (PointIn(Cell(i), p)) return {5, i};
        if (sys::QuickTimer::Get().Running()) {
            if (PointIn(CancelBtn(), p)) return {7, 0};
        } else {
            auto chips = Chips();
            for (size_t i = 0; i < chips.size(); ++i)
                if (PointIn(chips[i].r, p)) return {6, static_cast<int>(i)};
        }
        return {0, 0};
    }

    void PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) override {
        SYSTEMTIME now;
        GetLocalTime(&now);
        // Big date, like the Windows flyout.
        int dow = DowMonday(now.wYear, now.wMonth, now.wDay);
        std::wstring big = Info(LOCALE_SDAYNAME1 + dow) + L", " + std::to_wstring(now.wDay) + L" " + Info(LOCALE_SMONTHNAME1 + now.wMonth - 1);
        if (hot_.first == 4) draw::RoundRect(dc, Head(), 8, kSurfaceHover);
        draw::Text(dc, big, {r.left + 18, r.top + 8, r.right - 16, r.top + 44}, 20, kText, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        dc->DrawLine({r.left + 14, r.top + kHeadH}, {r.right - 14, r.top + kHeadH}, draw::Brush(dc, Rgba(255, 255, 255, 0.08f)), 1);

        // Month row with up / down arrows.
        auto t = Title();
        if (hot_.first == 3) draw::RoundRect(dc, {t.left, t.top + 1, t.left + 150, t.bottom - 1}, 8, kSurfaceHover);
        draw::Text(dc, Info(LOCALE_SMONTHNAME1 + month_ - 1) + L" " + std::to_wstring(year_), {t.left + 8, t.top, t.right, t.bottom}, 14, kText,
                   DWRITE_FONT_WEIGHT_SEMI_BOLD);
        for (int i = 1; i <= 2; ++i) {
            auto b = i == 1 ? UpBtn() : DownBtn();
            if (hot_.first == i) draw::RoundRect(dc, {b.left + 2, b.top, b.right - 2, b.bottom}, 8, kSurfaceHover);
            draw::Glyph(dc, i == 1 ? g::ChevronUp : g::ChevronDown, {(b.left + b.right) / 2, (b.top + b.bottom) / 2}, 11, kTextDim);
        }

        int first = FirstDow();
        for (int c = 0; c < 7; ++c) {
            std::wstring n = Info(LOCALE_SABBREVDAYNAME1 + (first + c) % 7);
            if (n.size() > 2) n.resize(2);
            auto cell = Cell(c);
            draw::Text(dc, n, {cell.left, r.top + kWeekY, cell.right, r.top + kWeekY + kWeekH}, 11.5f, kTextDim, DWRITE_FONT_WEIGHT_NORMAL,
                       Align::Center);
        }
        for (int i = 0; i < 42; ++i) {
            Day d = DayAt(i);
            auto cell = Cell(i);
            auto ci = Circle(cell);
            bool today = d.y == now.wYear && d.m == now.wMonth && d.d == now.wDay;
            bool selected = d.y == sel_[0] && d.m == sel_[1] && d.d == sel_[2];
            bool hover = hot_.first == 5 && hot_.second == i;
            D2D1_COLOR_F text = d.inMonth ? kText : kTextFaint;
            if (today) {
                draw::RoundRect(dc, ci, 16, kAccent);
                text = Rgba(24, 25, 28);
            } else if (hover) {
                draw::RoundRect(dc, ci, 16, kSurfaceHover);
            }
            if (selected && !today)
                dc->DrawRoundedRectangle({{ci.left + 1, ci.top + 1, ci.right - 1, ci.bottom - 1}, 15, 15}, draw::Brush(dc, kAccent), 1.5f);
            draw::Text(dc, std::to_wstring(d.d), cell, 13, text, today ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, Align::Center, true);
        }

        // Quick timer / focus session.
        float qy = QuickTop();
        dc->DrawLine({r.left + 14, qy}, {r.right - 14, qy}, draw::Brush(dc, Rgba(255, 255, 255, 0.08f)), 1);
        auto& qt = sys::QuickTimer::Get();
        if (qt.Running()) {
            bool focus = qt.Kind() == sys::TimerKind::Focus;
            draw::Glyph(dc, focus ? g::Focus : g::Timer, {r.left + 32, qy + 41}, 18, focus ? kAccent : kText);
            int sec = static_cast<int>((qt.RemainingMs() + 999) / 1000);
            wchar_t b[24];
            if (sec >= 3600) swprintf(b, 24, L"%d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
            else swprintf(b, 24, L"%02d:%02d", sec / 60, sec % 60);
            draw::Text(dc, b, {r.left + 56, qy + 20, r.right - 60, qy + 52}, 24, kText, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Leading, true);
            float frac = qt.TotalMs() > 0 ? 1.0f - static_cast<float>(qt.RemainingMs()) / static_cast<float>(qt.TotalMs()) : 0.0f;
            D2D1_RECT_F bar{r.left + 56, qy + 62, r.right - 60, qy + 65};
            draw::RoundRect(dc, bar, 1.5f, Rgba(255, 255, 255, 0.12f));
            draw::RoundRect(dc, {bar.left, bar.top, bar.left + (bar.right - bar.left) * frac, bar.bottom}, 1.5f, focus ? kAccent : kText);
            auto c = CancelBtn();
            if (hot_.first == 7) draw::RoundRect(dc, c, 9, kSurfaceHover);
            draw::Glyph(dc, g::Close, {(c.left + c.right) / 2, (c.top + c.bottom) / 2}, 12, kTextDim);
        } else {
            for (int row = 0; row < 2; ++row)
                draw::Glyph(dc, row == 0 ? g::Timer : g::Focus, {r.left + 32, qy + 12 + row * 38 + 15}, 16, row == 0 ? kTextDim : kAccent);
            auto chips = Chips();
            for (size_t i = 0; i < chips.size(); ++i) {
                bool hot = hot_.first == 6 && hot_.second == static_cast<int>(i);
                draw::RoundRect(dc, chips[i].r, 9, hot ? Rgba(255, 255, 255, 0.14f) : Rgba(255, 255, 255, 0.06f));
                draw::Text(dc, std::to_wstring(chips[i].minutes) + L" dk", chips[i].r, 12, kText, DWRITE_FONT_WEIGHT_NORMAL, Align::Center, true);
            }
        }
    }
    void OnMouseMove(PointF p) override {
        auto h = Hit(p);
        if (h != hot_) {
            hot_ = h;
            Invalidate();
        }
    }
    void OnMouseLeave() override {
        hot_ = {0, 0};
        Invalidate();
    }
    void OnMouseUp(PointF p, int b) override {
        if (b != 0) return;
        auto h = Hit(p);
        switch (h.first) {
        case 1: Step(-1); break;
        case 2: Step(1); break;
        case 3: Prepare(); Invalidate(); break;
        case 4:
            ShellExecuteW(nullptr, L"open", L"ms-settings:dateandtime", nullptr, nullptr, SW_SHOWNORMAL);
            Close(false);
            break;
        case 6: {
            auto chips = Chips();
            if (h.second < static_cast<int>(chips.size())) sys::QuickTimer::Get().Start(chips[h.second].kind, chips[h.second].minutes);
            break;
        }
        case 7: sys::QuickTimer::Get().Cancel(); break;
        case 5: {
            Day d = DayAt(h.second);
            sel_ = {d.y, d.m, d.d};
            if (!d.inMonth) Step(d.m == (month_ % 12) + 1 ? 1 : -1);
            else Invalidate();
            break;
        }
        }
    }
    void OnWheel(PointF, int delta) override { Step(delta > 0 ? -1 : 1); }
    bool OnKey(UINT vk) override {
        if (vk == VK_LEFT || vk == VK_UP || vk == VK_PRIOR) Step(-1);
        else if (vk == VK_RIGHT || vk == VK_DOWN || vk == VK_NEXT) Step(1);
        else if (vk == VK_HOME) { Prepare(); Invalidate(); }
        else return false;
        return true;
    }

private:
    int year_ = 2026, month_ = 1;
    std::array<int, 3> sel_{0, 0, 0};
    std::pair<int, int> hot_{0, 0};
};

// ============================================================================ Network
// Current connection on top, Wi-Fi networks in range below. Known networks connect with one click;
// a new secured network needs its password, which Windows' settings page asks for.
class NetworkPopup : public Popup {
public:
    void Refresh() {
        if (IsOpen()) {
            Build();
            Relayout();
            Invalidate();
        }
    }
    void Prepare() {
        sys::ScanWifi();
        wifi_ = sys::ListWifi();
        polls_ = 0;
        Build();
        ui::ClearTimeout(poll_);
        poll_ = ui::SetTimeout(2500, [this] { Poll(); });
    }

protected:
    static constexpr float kW = 300, kHeadH = 44, kRow = 34;
    enum Kind { kSettings, kDisconnect, kNet };
    struct Item {
        Kind k;
        D2D1_RECT_F r;
        int i;
    };
    float ContentWidth() override { return kW; }
    float ContentHeight() override { return height_; }

    void Build() {
        items_.clear();
        items_.push_back({kSettings, {kW - 44, 7, kW - 10, 37}, 0});
        float y = kHeadH + 2;
        sepY_ = -1;
        size_t shown = std::min<size_t>(wifi_.nets.size(), 8);
        if (shown) {
            sepY_ = kHeadH;
            for (size_t i = 0; i < shown; ++i) {
                items_.push_back({kNet, {6, y, kW - 6, y + kRow}, static_cast<int>(i)});
                if (wifi_.nets[i].connected) items_.push_back({kDisconnect, {kW - 42, y + 3, kW - 10, y + kRow - 3}, static_cast<int>(i)});
                y += kRow;
            }
            y += 2;
        }
        height_ = y + 6;
    }
    void Poll() {
        if (!IsOpen()) return;
        if (++polls_ % 3 == 0) sys::ScanWifi();
        wifi_ = sys::ListWifi();
        Build();
        Relayout();
        Invalidate();
        poll_ = ui::SetTimeout(2500, [this] { Poll(); });
    }
    D2D1_RECT_F Abs(const D2D1_RECT_F& r) const {
        auto c = ContentRect();
        return {c.left + r.left, c.top + r.top, c.left + r.right, c.top + r.bottom};
    }
    int Hit(PointF p) const {
        for (size_t i = items_.size(); i-- > 0;) // later items (disconnect button) sit on top of rows
            if (PointIn(Abs(items_[i].r), p)) return static_cast<int>(i);
        return -1;
    }
    static wchar_t SignalGlyph(int q) { return q < 25 ? g::Wifi1 : q < 50 ? g::Wifi2 : q < 75 ? g::Wifi3 : g::Wifi4; }

    void PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) override {
        const auto& n = sys::NetworkMonitor::Get().State();
        wchar_t glyph = g::NetOffline;
        std::wstring title = L"Bağlı değil";
        if (n.kind == sys::NetKind::Ethernet) { glyph = g::Ethernet; title = L"Kablolu bağlantı"; }
        else if (n.kind == sys::NetKind::Wifi) { glyph = SignalGlyph(n.wifiQuality < 0 ? 100 : n.wifiQuality); title = n.ssid.empty() ? L"Wi‑Fi" : n.ssid; }
        else if (n.kind != sys::NetKind::None) { glyph = g::Globe; title = L"Ağ bağlantısı"; }
        draw::Glyph(dc, glyph, {r.left + 26, r.top + 22}, 17, n.kind == sys::NetKind::None ? kTextFaint : kText);
        draw::Text(dc, Ellipsize(title, 26), {r.left + 50, r.top + 6, r.right - 54, r.top + 38}, 14, kText, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (n.kind != sys::NetKind::None && !n.internet)
            draw::Glyph(dc, g::Warning, {r.right - 62, r.top + 22}, 13, kWarn);
        if (sepY_ >= 0) dc->DrawLine({r.left + 14, r.top + sepY_}, {r.right - 14, r.top + sepY_}, draw::Brush(dc, Rgba(255, 255, 255, 0.08f)), 1);
        for (size_t k = 0; k < items_.size(); ++k) {
            const Item& it = items_[k];
            auto a = Abs(it.r);
            D2D1_POINT_2F c{(a.left + a.right) / 2, (a.top + a.bottom) / 2};
            bool hot = hot_ == static_cast<int>(k);
            if (it.k == kSettings) {
                if (hot) draw::RoundRect(dc, a, 8, kSurfaceHover);
                draw::Glyph(dc, g::Settings, c, 14, kTextDim);
            } else if (it.k == kNet) {
                const auto& w = wifi_.nets[it.i];
                if (hot || w.connected) draw::RoundRect(dc, a, 8, hot ? kSurfaceHover : Rgba(255, 255, 255, 0.04f));
                draw::Glyph(dc, SignalGlyph(w.quality), {a.left + 20, c.y}, 15, w.connected ? kAccent : kText);
                draw::Text(dc, Ellipsize(w.ssid, 26), {a.left + 42, a.top, a.right - 44, a.bottom}, 13, w.connected ? kText : kTextDim);
                if (w.secured && !w.connected) draw::Glyph(dc, g::Lock, {a.right - 18, c.y}, 11, kTextFaint);
            } else if (it.k == kDisconnect) {
                if (hot) draw::RoundRect(dc, a, 8, kSurfaceHover);
                draw::Glyph(dc, g::Close, c, 11, kTextDim);
            }
        }
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
        if (b != 0 || h < 0) return;
        const Item it = items_[h];
        if (it.k == kSettings) {
            ShellExecuteW(nullptr, L"open", L"ms-settings:network", nullptr, nullptr, SW_SHOWNORMAL);
            Close(false);
        } else if (it.k == kDisconnect) {
            sys::DisconnectWifi();
            Close(false);
        } else if (it.k == kNet) {
            const auto net = wifi_.nets[it.i];
            if (net.connected) return;
            if (net.known && sys::ConnectWifi(net)) {
                Close(false);
            } else if (net.secured || !net.known) {
                ShellExecuteW(nullptr, L"open", L"ms-settings:network-wifi", nullptr, nullptr, SW_SHOWNORMAL);
                Close(false);
            }
        }
    }

private:
    sys::WifiList wifi_;
    std::vector<Item> items_;
    float height_ = 60, sepY_ = -1;
    int hot_ = -1, polls_ = 0;
    ui::TimerId poll_ = 0;
};

// ============================================================================ Tray (background apps)
class TrayPopup : public Popup {
public:
    void Prepare() { // reads the running tray apps just before the box opens
        apps_ = sys::ListRunningTrayApps();
        if (apps_.size() > kMax) apps_.resize(kMax);
        hot_ = -1;
    }

protected:
    static constexpr int kCols = 7;
    static constexpr size_t kMax = 28;
    static constexpr float kCell = 38, kIcon = 22, kHead = 6, kPadB = 6;
    int Rows() const { return std::max(1, static_cast<int>((apps_.size() + kCols - 1) / kCols)); }
    float ContentWidth() override { return kCols * kCell + 20; }
    float ContentHeight() override { return kHead + (apps_.empty() ? 36.0f : Rows() * kCell) + kPadB + kFoot; }
    static constexpr float kFoot = 32;
    D2D1_RECT_F Foot() const {
        auto r = ContentRect();
        return {r.left + 8, r.bottom - kFoot - 2, r.right - 8, r.bottom - 4};
    }
    D2D1_RECT_F Cell(size_t i) const {
        auto r = ContentRect();
        float x = r.left + 10 + (i % kCols) * kCell, y = r.top + kHead + (i / kCols) * kCell;
        return {x, y, x + kCell, y + kCell};
    }
    int Hit(PointF p) const {
        if (PointIn(Foot(), p)) return -2;
        for (size_t i = 0; i < apps_.size(); ++i)
            if (PointIn(Cell(i), p)) return static_cast<int>(i);
        return -1;
    }
    void PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) override {
        auto f = Foot();
        if (HoverOf(1002) > 0.01f) draw::RoundRect(dc, f, 8, Rgba(255, 255, 255, 0.07f * HoverOf(1002)));
        draw::Text(dc, L"Windows sistem tepsisini göster ›", {f.left + 10, f.top, f.right - 6, f.bottom}, 11.5f, kTextDim);
        if (apps_.empty()) {
            draw::Text(dc, L"Yok", {r.left + 10, r.top + kHead, r.right - 10, r.top + kHead + 36}, 12, kTextFaint,
                       DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
            return;
        }
        int px = static_cast<int>(std::ceil(kIcon * Scale()));
        for (size_t i = 0; i < apps_.size(); ++i) {
            auto c = Cell(i);
            float hv = HoverOf(static_cast<int>(i));
            if (hv > 0.01f) draw::RoundRect(dc, {c.left + 2, c.top + 2, c.right - 2, c.bottom - 2}, 9, Rgba(255, 255, 255, 0.07f * hv + 0.05f * PressOf(static_cast<int>(i))));
            float cx = (c.left + c.right) / 2, cy = (c.top + c.bottom) / 2;
            D2D1_RECT_F ir{cx - kIcon / 2, cy - kIcon / 2, cx + kIcon / 2, cy + kIcon / 2};
            if (auto* bmp = gfx::IconCache::Get().Get(L"file:" + apps_[i].exePath, px)) draw::Bitmap(dc, bmp, ir);
            else draw::RoundRect(dc, ir, 5, Rgba(255, 255, 255, 0.08f));
        }
    }
    void OnMouseMove(PointF p) override {
        int h = Hit(p);
        if (h == hot_) return;
        hot_ = h;
        HotChanged(h >= 0 ? h : (h == -1 ? -1 : 1000 - h));
        Invalidate();
        if (h >= 0) {
            auto c = Cell(static_cast<size_t>(h));
            Tooltip::Get().Request(apps_[static_cast<size_t>(h)].name, DipToScreen({(c.left + c.right) / 2, c.bottom - 4}), true);
        } else Tooltip::Get().Hide();
    }
    void OnMouseLeave() override {
        hot_ = -1;
        HotChanged(-1);
        PressedChanged(-1);
        Tooltip::Get().Hide();
        Invalidate();
    }
    void OnMouseDown(PointF p, int b) override {
        int h = Hit(p);
        if (b == 0) PressedChanged(h >= 0 ? h : (h == -1 ? -1 : 1000 - h));
    }
    void OnMouseUp(PointF p, int b) override {
        PressedChanged(-1);
        int h = Hit(p);
        if (b == 0 && h == -2) {
            Tooltip::Get().Hide();
            Close(false);
            Post([] { App::Get().PeekWindowsTray(); });
            return;
        }
        if (h < 0 || (b != 0 && b != 1)) return;
        auto app = apps_[static_cast<size_t>(h)];
        Tooltip::Get().Hide();
        POINT at;
        GetCursorPos(&at);
        if (b == 0) {
            Close(false);
            sys::ActivateTrayApp(app);
            return;
        }
        // Right click: this box stays open behind the menu. The menu offers to open the app, to reach
        // its own tray menu (settings of such apps usually live there: the real taskbar is shown for a
        // moment), or to close it.
        std::vector<MenuItem> menu;
        menu.push_back({L"Aç", g::Settings, [this, app] {
                            Close(false);
                            sys::ActivateTrayApp(app);
                        }});
        menu.push_back({L"Kendi menüsü için tepsiyi göster", 0, [this] {
                            Close(false);
                            App::Get().PeekWindowsTray();
                        }});
        menu.push_back({L"Kapat", g::Close, [this, app] {
                            sys::TerminateApp(app.exePath);
                            SetTimeout(600, [this] { // the list loses the app that just ended
                                if (!IsOpen()) return;
                                Prepare();
                                Relayout();
                                Invalidate();
                            });
                        }});
        menu.back().separatorBefore = true;
        MenuPopup::Get().Show(std::move(menu), at);
    }

private:
    std::vector<sys::TrayApp> apps_;
    int hot_ = -1;
};

// ============================================================================ Device picker ("Cihaz ekle")
// Step 1: the user's peripherals. Step 2: the values that device shares with Windows (0..100, e.g.
// the battery level); the chosen one is shown from then on as a battery entry.
class PickerPopup : public Popup {
public:
    void Begin() {
        anim_.Clear();
        step_ = 0;
        scroll_ = 0;
        hot_ = -1;
        values_.clear();
        devices_.clear();
        loading_ = true;
        uint64_t token = ++scanToken_;
        // Walking every device node takes a while: off the UI thread, "Taranıyor…" meanwhile.
        WorkerPool::Background().Submit(
            [this, token] {
                auto list = sys::ListPeripherals();
                Post([this, token, list = std::move(list)]() mutable {
                    if (token != scanToken_) return; // a newer scan / the box was reopened
                    devices_ = std::move(list);
                    loading_ = false;
                    if (IsOpen()) {
                        Relayout();
                        Invalidate();
                    }
                });
            },
            "picker-scan");
    }

protected:
    static constexpr float kW = 340, kRowH = 46, kHead = 44, kMaxRows = 7, kFoot = 30;
    size_t RowCount() const { return step_ == 0 ? devices_.size() : values_.size(); }
    float ListH() const {
        size_t n = RowCount();
        return n == 0 ? 64.0f : std::min(static_cast<float>(n), kMaxRows) * kRowH;
    }
    float ContentWidth() override { return kW; }
    float ContentHeight() override { return kHead + ListH() + kFoot; }
    D2D1_RECT_F ListRect() const {
        auto r = ContentRect();
        return {r.left, r.top + kHead, r.right, r.top + kHead + ListH()};
    }
    D2D1_RECT_F BackRect() const {
        auto r = ContentRect();
        return {r.left + 8, r.top + 8, r.left + 38, r.top + 36};
    }
    D2D1_RECT_F RowRect(size_t i) const {
        auto l = ListRect();
        float y = l.top + static_cast<float>(i) * kRowH - scroll_;
        return {l.left + 6, y, l.right - 6, y + kRowH - 2};
    }
    float MaxScroll() const { return std::max(0.0f, static_cast<float>(RowCount()) * kRowH - ListH()); }
    static wchar_t ClassGlyph(const std::wstring& cls) {
        if (_wcsicmp(cls.c_str(), L"Mouse") == 0) return g::Mouse;
        if (_wcsicmp(cls.c_str(), L"Keyboard") == 0) return g::Keyboard;
        if (_wcsicmp(cls.c_str(), L"Bluetooth") == 0) return g::Bluetooth;
        if (_wcsicmp(cls.c_str(), L"MEDIA") == 0) return g::Headphones;
        if (_wcsicmp(cls.c_str(), L"HIDClass") == 0) return g::Gamepad;
        return g::Devices;
    }
    int Hit(PointF p) const {
        if (step_ > 0 && PointIn(BackRect(), p)) return 2000;
        if (!PointIn(ListRect(), p)) return -1;
        for (size_t i = 0; i < RowCount(); ++i)
            if (PointIn(RowRect(i), p)) return static_cast<int>(i);
        return -1;
    }
    void PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) override {
        std::wstring title = step_ == 0 ? L"Cihaz ekle: cihazı seç" : L"Hangi değer pil olsun?";
        float tx = r.left + 14;
        if (step_ > 0) {
            auto b = BackRect();
            if (HoverOf(2000) > 0.01f) draw::RoundRect(dc, b, 8, Rgba(255, 255, 255, 0.07f * HoverOf(2000)));
            draw::Glyph(dc, g::Back, {(b.left + b.right) / 2, (b.top + b.bottom) / 2}, 13, kText);
            tx = r.left + 46;
        }
        draw::Text(dc, title, {tx, r.top + 10, r.right - 14, r.top + 34}, 14, kText, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        auto l = ListRect();
        if (RowCount() == 0) {
            draw::Text(dc, loading_ ? L"Taranıyor…" : step_ == 0 ? L"Bağlı kablosuz/USB cihaz bulunamadı." : L"Bu cihaz Windows ile pil bilgisi paylaşmıyor.",
                       {l.left + 14, l.top + 8, l.right - 14, l.bottom - 4}, 12, kTextDim);
        }
        dc->PushAxisAlignedClip(l, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (size_t i = 0; i < RowCount(); ++i) {
            auto rr = RowRect(i);
            if (rr.bottom < l.top || rr.top > l.bottom) continue;
            float hv = HoverOf(static_cast<int>(i)), pr = PressOf(static_cast<int>(i));
            if (hv > 0.01f || pr > 0.01f) draw::RoundRect(dc, rr, 9, Rgba(255, 255, 255, 0.07f * hv + 0.05f * pr));
            float cy = (rr.top + rr.bottom) / 2;
            if (step_ == 0) {
                const auto& d = devices_[i];
                draw::Glyph(dc, ClassGlyph(d.className), {rr.left + 22, cy}, 16, kText);
                draw::Text(dc, Ellipsize(d.name, 38), {rr.left + 46, rr.top + 4, rr.right - 28, rr.top + 24}, 13, kText);
                draw::Text(dc, d.className, {rr.left + 46, rr.top + 23, rr.right - 28, rr.bottom}, 10.5f, kTextFaint);
                draw::Glyph(dc, g::ChevronRight, {rr.right - 14, cy}, 11, kTextDim);
            } else {
                const auto& v = values_[i];
                draw::Glyph(dc, v.knownBattery ? g::Battery10 : g::Info, {rr.left + 22, cy}, 15, v.knownBattery ? kGood : kTextDim);
                draw::Text(dc, Ellipsize(v.label, 38), {rr.left + 46, rr.top + 4, rr.right - 70, rr.top + 24}, 12.5f, kText);
                draw::Text(dc, Ellipsize(v.ownerName, 40), {rr.left + 46, rr.top + 23, rr.right - 70, rr.bottom}, 10.5f, kTextFaint);
                draw::Text(dc, std::to_wstring(v.value) + L"%", {rr.right - 66, rr.top, rr.right - 12, rr.bottom}, 14, kText,
                           DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Trailing, true);
            }
        }
        dc->PopAxisAlignedClip();
        draw::Text(dc, step_ == 0 ? L"Cihazın Windows ile paylaştığı bilgilerden seçersin." : (!values_.empty() && values_.front().knownBattery ? L"Seçtiğin değer, pil yüzdesi olarak gösterilir." : L"Windows bu cihaz için pil düzeyi bildirmiyor; bunlar pil olmayabilir."),
                   {r.left + 14, r.bottom - kFoot, r.right - 14, r.bottom - 6}, 10.5f, kTextFaint);
    }
    void OnMouseMove(PointF p) override {
        int h = Hit(p);
        if (h != hot_) {
            hot_ = h;
            HotChanged(h);
            Invalidate();
        }
    }
    void OnMouseLeave() override {
        hot_ = -1;
        HotChanged(-1);
        PressedChanged(-1);
        Invalidate();
    }
    void OnMouseDown(PointF p, int b) override {
        if (b == 0) PressedChanged(Hit(p));
    }
    void OnWheel(PointF, int delta) override {
        scroll_ = std::clamp(scroll_ - delta / 120.0f * kRowH, 0.0f, MaxScroll());
        Invalidate();
    }
    void OnMouseUp(PointF p, int b) override {
        PressedChanged(-1);
        int h = Hit(p);
        if (b != 0 || h < 0) return;
        if (h == 2000) {
            ++scanToken_;
            loading_ = false;
            step_ = 0;
            scroll_ = 0;
            Relayout();
            Invalidate();
            return;
        }
        if (step_ == 0) {
            chosen_ = devices_[static_cast<size_t>(h)];
            values_.clear();
            step_ = 1;
            scroll_ = 0;
            anim_.Clear();
            loading_ = true;
            uint64_t token = ++scanToken_;
            std::wstring container = chosen_.containerId;
            WorkerPool::Background().Submit(
                [this, token, container] {
                    auto vals = sys::ListSharedValues(container);
                    Post([this, token, vals = std::move(vals)]() mutable {
                        if (token != scanToken_) return;
                        values_ = std::move(vals);
                        loading_ = false;
                        if (IsOpen()) {
                            Relayout();
                            Invalidate();
                        }
                    });
                },
                "picker-scan");
            Relayout();
            Invalidate();
            return;
        }
        Add(values_[static_cast<size_t>(h)]);
    }
    bool OnKey(UINT vk) override {
        if (vk == VK_BACK && step_ > 0) {
            ++scanToken_; // drop a scan still running for the device page
            loading_ = false;
            step_ = 0;
            scroll_ = 0;
            Relayout();
            Invalidate();
            return true;
        }
        return false;
    }
    void Add(const sys::PnpValue& v) {
        auto& cfg = Store::Get().settings();
        std::string inst = ToUtf8(v.ownerInstanceId), guid = ToUtf8(v.guid);
        bool dup = false;
        for (const auto& s : cfg.customBattery)
            if (s.instanceId == inst && s.guid == guid && s.pid == static_cast<int>(v.pid)) dup = true;
        if (!dup && cfg.customBattery.size() < 12) {
            CustomBatterySource s;
            s.id = "c" + std::to_string(GetTickCount64() % 100000000);
            s.name = ToUtf8(chosen_.name);
            s.instanceId = inst;
            s.guid = guid;
            s.pid = static_cast<int>(v.pid);
            cfg.customBattery.push_back(std::move(s));
            Store::Get().SaveSettings();
            sys::BatteryService::Get().RefreshCustom();
        }
        std::wstring name = chosen_.name;
        Close(false);
        Post([name, dup] {
            App::Get().PostInternalNotification(dup ? L"Bu değer zaten ekli" : L"Cihaz eklendi", name + L": pil yüzdesi artık Piller kutusunda.");
        });
    }

private:
    int step_ = 0;
    float scroll_ = 0;
    int hot_ = -1;
    bool loading_ = false;
    uint64_t scanToken_ = 0;
    std::vector<sys::PnpDevice> devices_;
    std::vector<sys::PnpValue> values_;
    sys::PnpDevice chosen_;
};
// ============================================================================ Quick settings
// Toggles (Bluetooth, Wi-Fi, airplane mode, do not disturb) and shortcuts to the Windows pages that
// matter most. Every tile has a text label; nothing is hidden behind icons only.
class QuickPopup : public Popup {
public:
    void Prepare() {
        dnd_ = sys::quick::DoNotDisturb();
        anim_.Clear();
        SyncState();
        Sync();
    }
    void Refresh() {
        if (IsOpen()) Invalidate();
    }

protected:
    struct Tile {
        int id;
        const wchar_t* label;
        wchar_t glyph;
    };
    static constexpr int kCols = 3;
    static constexpr float kTileW = 98, kTileH = 62, kGap = 8, kHead = 40;
    const std::array<Tile, 14>& Tiles() const {
        static const std::array<Tile, 14> t{{
            {1, L"Bluetooth", g::Bluetooth},
            {2, L"Wi‑Fi", g::Wifi4},
            {3, L"Kablosuzları kapat", 0xE709},
            {4, L"Rahatsız etme", g::Focus},
            {5, L"Gece ışığı", 0xE706},
            {6, L"Mobil hotspot", g::Globe},
            {7, L"Bluetooth cihazları", g::Devices},
            {8, L"Ekrana yansıt", 0xE7F4},
            {9, L"Cihaza yayınla", 0xEC15},
            {10, L"Ekran alıntısı", 0xE8C6},
            {11, L"Ses ayarları", g::Speaker},
            {12, L"Ekran ayarları", 0xE7F4},
            {13, L"Güç ve uyku", g::Plug},
            {14, L"Tüm ayarlar", g::Settings},
        }};
        return t;
    }
    float ContentWidth() override { return kCols * kTileW + (kCols + 1) * kGap; }
    float ContentHeight() override {
        int rows = (static_cast<int>(Tiles().size()) + kCols - 1) / kCols;
        return kHead + rows * (kTileH + kGap) + 2;
    }
    D2D1_RECT_F TileRect(size_t i) const {
        auto r = ContentRect();
        float x = r.left + kGap + static_cast<float>(i % kCols) * (kTileW + kGap);
        float y = r.top + kHead + static_cast<float>(i / kCols) * (kTileH + kGap);
        return {x, y, x + kTileW, y + kTileH};
    }
    int Hit(PointF p) const {
        for (size_t i = 0; i < Tiles().size(); ++i)
            if (PointIn(TileRect(i), p)) return static_cast<int>(i);
        return -1;
    }
    bool Active(int id) const {
        switch (id) {
        case 1: return radios_.bluetooth.on;
        case 2: return radios_.wifi.on;
        case 3: return radios_.Airplane();
        case 4: return dnd_;
        default: return false;
        }
    }
    bool Enabled(int id) const {
        if (id == 1) return !radios_.known || radios_.bluetooth.present;
        if (id == 2) return !radios_.known || radios_.wifi.present;
        if (id == 3) return !radios_.known || radios_.bluetooth.present || radios_.wifi.present;
        return true;
    }
    static bool IsToggle(int id) { return id >= 1 && id <= 4; }
    void PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) override {
        draw::Text(dc, L"Hızlı ayarlar", {r.left + 14, r.top + 10, r.right - 14, r.top + 34}, 14, kText, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        D2D1_MATRIX_3X2_F base;
        dc->GetTransform(&base);
        for (size_t i = 0; i < Tiles().size(); ++i) {
            const Tile& t = Tiles()[i];
            auto a = TileRect(i);
            bool on = Active(t.id), en = Enabled(t.id);
            int idx = static_cast<int>(i);
            float hv = HoverOf(idx), pr = PressOf(idx);
            float onAmt = std::clamp(anim_.Get(gfx::HoverBank::State, t.id), 0.0f, 1.0f);
            // tile grows slightly under the pointer and dips when pressed
            float s = 1.0f + 0.025f * hv - 0.06f * pr;
            dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {(a.left + a.right) / 2, (a.top + a.bottom) / 2}) * base);
            // background: white tint -> accent tint as the toggle turns on, brighter under the pointer
            float offA = 0.05f + 0.05f * hv, onA = 0.26f + 0.08f * hv;
            D2D1_COLOR_F bg = D2D1::ColorF((255 + (120 - 255) * onAmt) / 255.f, (255 + (170 - 255) * onAmt) / 255.f, 1.0f,
                                           offA + (onA - offA) * onAmt);
            draw::RoundRect(dc, a, 10, bg);
            auto col = !en ? kTextFaint : on ? kAccent : kText;
            draw::Glyph(dc, t.glyph, {(a.left + a.right) / 2, a.top + 22 - 1.5f * hv}, 17 + 1.0f * hv, col);
            draw::Text(dc, t.label, {a.left + 4, a.top + 36, a.right - 4, a.bottom - 2}, 11, !en ? kTextFaint : kText,
                       DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
            if (IsToggle(t.id) && en)
                draw::Text(dc, on ? L"Açık" : L"Kapalı", {a.left + 4, a.top + 4, a.right - 8, a.top + 18}, 9.5f, on ? kAccent : kTextFaint,
                           DWRITE_FONT_WEIGHT_NORMAL, Align::Trailing);
        }
        dc->SetTransform(base);
    }
    void OnMouseMove(PointF p) override {
        int h = Hit(p);
        if (h != hot_) {
            hot_ = h;
            HotChanged(h);
            Invalidate();
        }
    }
    void OnMouseLeave() override {
        hot_ = -1;
        HotChanged(-1);
        PressedChanged(-1);
        Invalidate();
    }
    void OnMouseDown(PointF p, int b) override {
        if (b == 0) PressedChanged(Hit(p));
    }
    // Moves the on/off amounts of the toggles to the current states (animated).
    void SyncState() {
        for (const Tile& t : Tiles())
            if (IsToggle(t.id)) anim_.To(gfx::HoverBank::State, t.id, Active(t.id) ? 1.0f : 0.0f);
        RequestAnimationFrame();
    }
    void Sync() {
        sys::quick::ReadRadios([this](sys::quick::Radios r) {
            radios_ = r;
            SyncState();
            Invalidate();
        });
    }
    void Open(const wchar_t* uri) {
        Close(false);
        std::wstring u = uri;
        Post([u] { ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    }
    void OnMouseUp(PointF p, int b) override {
        PressedChanged(-1);
        int h = Hit(p);
        if (b != 0 || h < 0) return;
        int id = Tiles()[static_cast<size_t>(h)].id;
        if (!Enabled(id)) return;
        auto after = [this] { Sync(); };
        switch (id) {
        case 1:
            radios_.bluetooth.on = !radios_.bluetooth.on; // optimistic; re-read afterwards
            sys::quick::SetBluetooth(radios_.bluetooth.on, after);
            break;
        case 2:
            radios_.wifi.on = !radios_.wifi.on;
            sys::quick::SetWifi(radios_.wifi.on, after);
            break;
        case 3: {
            bool toAirplane = !radios_.Airplane();
            radios_.bluetooth.on = radios_.wifi.on = !toAirplane;
            sys::quick::SetAirplane(toAirplane, after);
            break;
        }
        case 4:
            dnd_ = !dnd_;
            sys::quick::SetDoNotDisturb(dnd_);
            break;
        case 5: Open(L"ms-settings:nightlight"); return;
        case 6: Open(L"ms-settings:network-mobilehotspot"); return;
        case 7: Open(L"ms-settings:bluetooth"); return;
        case 8:
            Close(false);
            Post([] { SendWinCombo('P'); });
            return;
        case 9:
            Close(false);
            Post([] { SendWinCombo('K'); });
            return;
        case 10: Open(L"ms-screenclip:"); return;
        case 11: Open(L"ms-settings:sound"); return;
        case 12: Open(L"ms-settings:display"); return;
        case 13: Open(L"ms-settings:powersleep"); return;
        case 14: Open(L"ms-settings:"); return;
        }
        SyncState();
        Invalidate();
    }

private:
    sys::quick::Radios radios_;
    bool dnd_ = false;
    int hot_ = -1;
};

// ============================================================================ host
struct PopupHost::Impl {
    VolumePopup volume;
    MediaPopup media;
    BatteryPopup battery;
    NotificationsPopup notifications;
    CalendarPopup calendar;
    TrayPopup tray;
    NetworkPopup network;
    QuickPopup quick;
    PickerPopup picker;
    Popup* Of(PopupKind k) {
        switch (k) {
        case PopupKind::Picker: return &picker;
        case PopupKind::Quick: return &quick;
        case PopupKind::Network: return &network;
        case PopupKind::Volume: return &volume;
        case PopupKind::Media: return &media;
        case PopupKind::Battery: return &battery;
        case PopupKind::Calendar: return &calendar;
        case PopupKind::Tray: return &tray;
        default: return &notifications;
        }
    }
};

namespace {
constexpr PopupKind kAllKinds[] = {PopupKind::Volume, PopupKind::Media, PopupKind::Battery, PopupKind::Notifications,
                                   PopupKind::Calendar, PopupKind::Tray, PopupKind::Network, PopupKind::Quick, PopupKind::Picker};
}

PopupHost::PopupHost() : impl_(std::make_unique<Impl>()) {}
PopupHost::~PopupHost() = default;

void PopupHost::Init() {
    auto& d = *impl_;
    d.volume.Init(L"Ses");
    d.media.Init(L"Medya");
    d.battery.Init(L"Piller");
    d.notifications.Init(L"Bildirimler");
    d.calendar.Init(L"Takvim");
    d.tray.Init(L"Arka plan uygulamaları");
    d.network.Init(L"Ağ");
    d.quick.Init(L"Hızlı ayarlar");
    d.picker.Init(L"Cihaz ekle");
    for (auto k : kAllKinds) {
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
    for (auto other : kAllKinds)
        if (other != k && d.Of(other)->IsOpen()) d.Of(other)->Close(false);
    if (k == PopupKind::Battery) sys::BatteryService::Get().SetMenuOpen(true);
    if (k == PopupKind::Volume) d.volume.Prepare();
    if (k == PopupKind::Calendar) d.calendar.Prepare();
    if (k == PopupKind::Tray) d.tray.Prepare();
    if (k == PopupKind::Network) d.network.Prepare();
    if (k == PopupKind::Quick) d.quick.Prepare();
    if (k == PopupKind::Picker) d.picker.Begin();
    auto& sc = Screen::Current();
    // A visible gap separates the box from the notch.
    p->OpenAt(anchor.x, anchor.y + sc.Px(kPopupGap));
    if (k == PopupKind::Media) d.media.Opened();
    if (onOpenChanged) onOpenChanged();
}

void PopupHost::CloseAll() {
    auto& d = *impl_;
    for (auto k : kAllKinds)
        if (d.Of(k)->IsOpen()) d.Of(k)->Close(false);
}

bool PopupHost::AnyOpen() const {
    auto& d = *impl_;
    return d.volume.IsOpen() || d.media.IsOpen() || d.battery.IsOpen() || d.notifications.IsOpen() || d.calendar.IsOpen() ||
           d.tray.IsOpen() || d.network.IsOpen() || d.quick.IsOpen() || d.picker.IsOpen();
}

bool PopupHost::IsOpen(PopupKind k) const { return impl_->Of(k)->IsOpen(); }

void PopupHost::Refresh(PopupKind k) {
    auto& d = *impl_;
    switch (k) {
    case PopupKind::Volume: d.volume.Refresh(); break;
    case PopupKind::Media: d.media.Refresh(); break;
    case PopupKind::Battery: d.battery.Refresh(); break;
    case PopupKind::Notifications: d.notifications.Refresh(); break;
    case PopupKind::Calendar: d.calendar.Changed(); break;
    case PopupKind::Network: d.network.Refresh(); break;
    default: break;
    }
}

} // namespace nb::ui
