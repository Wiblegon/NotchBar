#include "pch.h"

#include "ui/Widgets.h"

#include "core/Dispatcher.h"
#include "gfx/Draw.h"
#include "gfx/Graphics.h"
#include "gfx/Theme.h"

namespace nb::ui {

Screen& Screen::Current() {
    static Screen s;
    return s;
}

void Screen::Update(float uiScale) {
    handle = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(handle, &mi);
    monitor = mi.rcMonitor;
    work = mi.rcWork;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &dx, &dy);
    scale = dx / 96.0f * uiScale;
}

bool PointIn(const D2D1_RECT_F& r, PointF p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

static bool IsOurWindow(HWND h) {
    if (!h) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

// ------------------------------------------------------------------------------------ Popup
bool Popup::Init(const wchar_t* name) { return Create(name, /*activatable*/ true); }

D2D1_RECT_F Popup::ContentRect() const {
    return {margin_, margin_, WidthDip() - margin_, HeightDip() - margin_};
}

void Popup::OpenAt(int anchorX, int top) {
    anchorX_ = anchorX;
    top_ = top;
    Relayout();
    bool wasOpen = open_;
    open_ = true;
    closing_ = false;
    if (!wasOpen) {
        prevForeground_ = GetForegroundWindow(); // may be one of ours (launcher search field)
    }
    Show(true);
    appear_.To(1);
    RequestAnimationFrame();
    SetForegroundWindow(Hwnd());
    SetFocus(Hwnd());
}

void Popup::Relayout() {
    auto& sc = Screen::Current();
    float s = sc.scale;
    int w = static_cast<int>(std::ceil((ContentWidth() + 2 * margin_) * s));
    int h = static_cast<int>(std::ceil((ContentHeight() + 2 * margin_) * s));
    int left = anchorX_ - w / 2;
    left = std::clamp<int>(left, sc.monitor.left + 4, static_cast<int>(sc.monitor.right) - w - 4);
    int t = top_ - static_cast<int>(margin_ * s);
    if (t + h > sc.monitor.bottom) t = sc.monitor.bottom - h;
    Place({left, t, left + w, t + h});
    Invalidate();
}

void Popup::Close(bool restoreFocus) {
    if (!open_) return;
    open_ = false;
    closing_ = true;
    appear_.To(0);
    RequestAnimationFrame();
    if (restoreFocus && prevForeground_ && IsWindow(prevForeground_)) SetForegroundWindow(prevForeground_);
    prevForeground_ = nullptr;
    if (gfx::ReducedMotion()) {
        closing_ = false;
        Show(false);
    }
    if (onClosed) onClosed();
}

bool Popup::Animate(float dt) {
    bool more = appear_.Step(dt);
    more |= AnimateContent(dt);
    if (closing_ && !more) {
        closing_ = false;
        ui::Post([this] {
            if (!open_) Show(false);
        });
    }
    return more;
}

void Popup::Paint(ID2D1DeviceContext* dc) {
    float a = std::clamp(appear_.value, 0.0f, 1.0f);
    if (a <= 0.001f) return;
    D2D1_RECT_F r = ContentRect();
    float dy = (1 - a) * -6.0f;
    float sc = 0.97f + 0.03f * a;
    D2D1_POINT_2F c{(r.left + r.right) / 2, r.top};
    D2D1_MATRIX_3X2_F base;
    dc->GetTransform(&base);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, c) * D2D1::Matrix3x2F::Translation(0, dy) * base);
    draw::Panel(dc, r, theme::kPopupRadius, a);
    winrt::com_ptr<ID2D1Layer> layer;
    if (a < 0.999f) {
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::IdentityMatrix(), a),
                      nullptr);
    }
    PaintContent(dc, r);
    if (a < 0.999f) dc->PopLayer();
    dc->SetTransform(base);
}

LRESULT Popup::HandleMessage(UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && open_ && !IsOurWindow(reinterpret_cast<HWND>(lp))) Close(false);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            Close(true);
            return 0;
        }
        if (OnKey(static_cast<UINT>(wp))) return 0;
        break;
    }
    return Surface::HandleMessage(m, wp, lp);
}

// ------------------------------------------------------------------------------------ Menu
MenuPopup& MenuPopup::Get() {
    static MenuPopup m;
    static bool init = false;
    if (!init) {
        m.Init(L"NotchBar menÃ¼");
        m.margin_ = 10;
        init = true;
    }
    return m;
}

constexpr float kRowH = 32, kSepH = 9, kMenuPad = 5;

float MenuPopup::ContentWidth() {
    float w = 170;
    for (auto& it : items_) w = std::max(w, draw::TextWidth(it.text, 13) + 60);
    return std::min(w, 340.0f);
}

float MenuPopup::ContentHeight() {
    float h = kMenuPad * 2;
    for (auto& it : items_) h += kRowH + (it.separatorBefore ? kSepH : 0);
    return h;
}

void MenuPopup::Show(std::vector<MenuItem> items, POINT pt) {
    items_ = std::move(items);
    hot_ = -1;
    auto& sc = Screen::Current();
    int w = static_cast<int>((ContentWidth() + 2 * margin_) * sc.scale);
    int h = static_cast<int>((ContentHeight() + 2 * margin_) * sc.scale);
    int top = pt.y;
    if (pt.y + h > sc.monitor.bottom) top = pt.y - h + static_cast<int>(2 * margin_ * sc.scale);
    OpenAt(pt.x + w / 2 - static_cast<int>(margin_ * sc.scale), top);
}

int MenuPopup::HitTest(PointF p) const {
    D2D1_RECT_F r = ContentRect();
    float y = r.top + kMenuPad;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].separatorBefore) y += kSepH;
        if (p.y >= y && p.y < y + kRowH && p.x >= r.left && p.x < r.right) return static_cast<int>(i);
        y += kRowH;
    }
    return -1;
}

void MenuPopup::PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) {
    float y = r.top + kMenuPad;
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& it = items_[i];
        if (it.separatorBefore) {
            dc->DrawLine({r.left + 10, y + kSepH / 2}, {r.right - 10, y + kSepH / 2}, draw::Brush(dc, theme::kEdge), 1);
            y += kSepH;
        }
        D2D1_RECT_F row{r.left + kMenuPad, y, r.right - kMenuPad, y + kRowH};
        if (static_cast<int>(i) == hot_ && it.enabled) draw::RoundRect(dc, row, 8, theme::kSurfaceHover);
        auto col = it.enabled ? theme::kText : theme::kTextFaint;
        if (it.glyph) draw::Glyph(dc, it.glyph, {row.left + 18, (row.top + row.bottom) / 2}, 14, col);
        draw::Text(dc, it.text, {row.left + 38, row.top, row.right - 26, row.bottom}, 13, col);
        if (it.checked) draw::Glyph(dc, draw::glyph::Check, {row.right - 14, (row.top + row.bottom) / 2}, 12, theme::kAccent);
        y += kRowH;
    }
}

void MenuPopup::OnMouseMove(PointF p) {
    int h = HitTest(p);
    if (h != hot_) {
        hot_ = h;
        Invalidate();
    }
}

void MenuPopup::OnMouseLeave() {
    hot_ = -1;
    Invalidate();
}

void MenuPopup::OnMouseUp(PointF p, int button) {
    int i = HitTest(p);
    if (i >= 0) Choose(i);
}

void MenuPopup::Choose(int i) {
    if (i < 0 || i >= static_cast<int>(items_.size()) || !items_[i].enabled) return;
    auto action = items_[i].action;
    Close(true);
    if (action) ui::Post(action); // run after the menu released focus
}

bool MenuPopup::OnKey(UINT vk) {
    int n = static_cast<int>(items_.size());
    if (!n) return false;
    if (vk == VK_DOWN || vk == VK_UP) {
        int step = vk == VK_DOWN ? 1 : -1;
        for (int k = 0; k < n; ++k) {
            hot_ = (hot_ + step + n) % n;
            if (items_[hot_].enabled) break;
        }
        Invalidate();
        return true;
    }
    if (vk == VK_RETURN || vk == VK_SPACE) {
        Choose(hot_);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------ Tooltip
Tooltip& Tooltip::Get() {
    static Tooltip t;
    return t;
}

void Tooltip::Request(const std::wstring& text, POINT anchor, bool below) {
    if (!created_) {
        Create(L"NotchBar ipucu");
        created_ = true;
    }
    if (text.empty()) {
        Hide();
        return;
    }
    ClearTimeout(timer_);
    bool sameVisible = Visible() && text == text_;
    text_ = text;
    auto place = [this, anchor, below] {
        auto& sc = Screen::Current();
        float wDip = std::min(draw::TextWidth(text_, 12) + 30, 380.0f);
        int w = sc.Px(wDip), h = sc.Px(26);
        int x = std::clamp<int>(anchor.x - w / 2, sc.monitor.left + 4, sc.monitor.right - w - 4);
        int y = below ? anchor.y + sc.Px(6) : anchor.y - h - sc.Px(6);
        Place({x, y, x + w, y + h});
        Invalidate();
        Show(true);
    };
    if (sameVisible) place();
    else timer_ = SetTimeout(Visible() ? 60 : 450, place);
}

void Tooltip::Hide() {
    ClearTimeout(timer_);
    if (created_) Show(false);
}

void Tooltip::Paint(ID2D1DeviceContext* dc) {
    D2D1_RECT_F r{0.5f, 0.5f, WidthDip() - 0.5f, HeightDip() - 0.5f};
    draw::RoundRect(dc, r, 7, theme::Rgba(40, 41, 46, 0.98f));
    dc->DrawRoundedRectangle({r, 7, 7}, draw::Brush(dc, theme::kEdge), 1);
    draw::Text(dc, text_, {r.left + 10, r.top, r.right - 10, r.bottom}, 12, theme::kText, DWRITE_FONT_WEIGHT_NORMAL,
               draw::Align::Center);
}

} // namespace nb::ui
