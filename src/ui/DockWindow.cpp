#include "pch.h"

#include "ui/DockWindow.h"

#include "core/Store.h"
#include "gfx/Draw.h"
#include "gfx/IconCache.h"
#include "gfx/Theme.h"
#include "sys/AppCatalog.h"
#include "sys/TrayApps.h"
#include "ui/Widgets.h"

namespace nb::ui {
using namespace theme;
namespace {
constexpr float kMargin = 12; // shadow room
float Smooth(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
float Lerp(float a, float b, float t) { return a + (b - a) * t; }
} // namespace

bool DockWindow::Init() {
    if (!Create(L"NotchBar uygulamalar")) return false;
    edge_.onPresence = [this](EdgePresence p) { ApplyPresence(p, true); };
    Layout();
    Sync();
    width_.Snap(ContentWidth());
    ApplyPresence(edge_.Presence(), false);
    return true;
}

void DockWindow::Layout() {
    auto& sc = Screen::Current();
    float monW = sc.Dip(sc.monitor.right - sc.monitor.left);
    maxContentW_ = std::floor((monW * kDockMaxWidthFraction - 2 * kDockPad) / kDockCell) * kDockCell + 2 * kDockPad;
    winW_ = maxContentW_ + 2 * kMargin;
    winH_ = kDockH + kDockMargin + kMargin;
    int w = sc.Px(winW_), h = sc.Px(winH_);
    int cx = (sc.monitor.left + sc.monitor.right) / 2;
    Place({cx - w / 2, sc.monitor.bottom - h, cx - w / 2 + w, sc.monitor.bottom});
    Retarget(true);
    UpdateRegion();
    Invalidate();
}

float DockWindow::ContentWidth() const {
    int n = 0;
    for (auto& it : items_)
        if (!it.removing) ++n;
    // Last app closed: keep the old width while the dock fades away instead of shrinking to one cell.
    if (n == 0) return std::max(width_.target, kDockCell + 2 * kDockPad);
    float w = n * kDockCell + 2 * kDockPad;
    return std::min(w, maxContentW_);
}

void DockWindow::Sync() {
    const auto& groups = sys::WindowTracker::Get().Groups();
    for (auto& it : items_) it.removing = true;
    std::vector<Item> next;
    for (const auto& g : groups) {
        auto found = std::find_if(items_.begin(), items_.end(), [&](const Item& i) { return i.group.key == g.key; });
        if (found != items_.end()) {
            found->removing = false;
            found->group = g;
            found->alpha.To(1);
            found->scale.To(1);
            next.push_back(*found);
        } else {
            Item n;
            n.group = g;
            n.scale.Snap(0.6f);
            n.alpha.Snap(0);
            n.scale.To(1);
            n.alpha.To(1);
            next.push_back(n);
        }
    }
    // Closed apps stay briefly at their old place while they shrink and fade.
    for (size_t i = 0; i < items_.size(); ++i) {
        if (!items_[i].removing) continue;
        Item r = items_[i];
        r.scale.To(0.4f);
        r.alpha.To(0);
        size_t at = std::min(i, next.size());
        next.insert(next.begin() + static_cast<ptrdiff_t>(at), r);
    }
    bool firstFill = items_.empty();
    items_ = std::move(next);
    if (hot_ >= static_cast<int>(items_.size())) hot_ = -1;
    Retarget(firstFill);
    RequestAnimationFrame();
}

void DockWindow::Retarget(bool snap) {
    float x = kDockPad;
    for (auto& it : items_) {
        if (it.removing) {
            continue; // keeps its x while fading
        }
        if (snap || it.alpha.value < 0.05f) it.x.Snap(x);
        else it.x.To(x);
        x += kDockCell;
    }
    // removing items: align to neighbours so others slide over them
    occ_.To(HasApps() ? 1.0f : 0.0f);
    if (snap) occ_.Snap(HasApps() ? 1.0f : 0.0f);
    width_.To(ContentWidth());
    if (snap) width_.Snap(ContentWidth());
    float total = x + kDockPad;
    float maxScroll = std::max(0.0f, total - maxContentW_);
    scroll_.To(std::clamp(scroll_.target, 0.0f, maxScroll));
}

void DockWindow::ApplyPresence(EdgePresence p, bool animate) {
    if (p == EdgePresence::Hidden) {
        hot_ = -1;
        Tooltip::Get().Hide();
        expand_.Snap(0);
        Show(false);
        return;
    }
    expand_.To(p == EdgePresence::Normal ? 1.0f : 0.0f);
    if (!animate) expand_.Snap(expand_.target);
    if (p == EdgePresence::Collapsed) {
        hot_ = -1;
        Tooltip::Get().Hide();
    }
    if (!Visible()) {
        UpdateRegion();
        Show(true);
    }
    RequestAnimationFrame();
}

bool DockWindow::Animate(float dt) {
    bool more = width_.Step(dt);
    more |= anim_.Step(dt);
    more |= expand_.Step(dt);
    more |= occ_.Step(dt);
    more |= scroll_.Step(dt);
    for (auto& it : items_) {
        more |= it.x.Step(dt);
        more |= it.scale.Step(dt);
        more |= it.alpha.Step(dt);
        more |= it.lift.Step(dt);
    }
    size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(), [](const Item& i) { return i.removing && i.alpha.value < 0.02f; }),
                 items_.end());
    if (items_.size() != before) {
        hot_ = -1;
        anim_.SetHot(gfx::HoverBank::kNone); // indices shifted: drop the stale highlight
        anim_.SetPressed(gfx::HoverBank::kNone);
        Retarget(false);
        more = true;
    }
    UpdateRegion();
    return more;
}

D2D1_RECT_F DockWindow::DockRect() const {
    float e = std::clamp(expand_.value, 0.0f, 1.2f) * Occupied();
    float w = Lerp(kDockHandleW, width_.value, std::clamp(e, 0.0f, 1.0f));
    float h = Lerp(kDockHandleH, kDockH, e);
    float bottomGap = Lerp(kDockHandleMargin, kDockMargin, std::clamp(e, 0.0f, 1.0f));
    float cx = winW_ / 2;
    float bottom = winH_ - bottomGap;
    return {cx - w / 2, bottom - h, cx + w / 2, bottom};
}

void DockWindow::UpdateRegion() {
    auto r = DockRect();
    // Collapsed: the handle's hit area reaches the screen edge so it can be found by the mouse.
    if (Occupied() < 0.02f) { // no apps: nothing to see or hit
        SetRegionDip({0, 0, 1, 1});
        return;
    }
    float bottom = expand_.value < 0.5f ? winH_ : r.bottom + 2;
    SetRegionDip({r.left - 2, r.top - 2, r.right + 2, bottom});
}

int DockWindow::HitTest(PointF p) const {
    if (expand_.value * Occupied() < 0.8f) return -1;
    auto r = DockRect();
    if (!PointIn(r, p)) return -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& it = items_[i];
        if (it.removing) continue;
        float x = r.left + it.x.value - scroll_.value;
        if (p.x >= x && p.x < x + kDockCell) return static_cast<int>(i);
    }
    return -1;
}

void DockWindow::Paint(ID2D1DeviceContext* dc) {
    auto r = DockRect();
    if (Occupied() < 0.02f) return; // no apps open: the whole area is gone
    float e = std::clamp(expand_.value, 0.0f, 1.0f) * Occupied();
    float radius = Lerp(kDockHandleH / 2, kDockRadius, e);
    if (e < 0.05f) {
        // thin handle: subtle, does not compete with the content of maximized windows
        draw::RoundRect(dc, r, radius, Rgba(28, 29, 32, 0.92f));
        dc->DrawRoundedRectangle({r, radius, radius}, draw::Brush(dc, Rgba(255, 255, 255, 0.12f)), 1);
        return;
    }
    draw::Panel(dc, r, radius, 1.0f, e > 0.5f);
    float a = Smooth(0.55f, 1.0f, e);
    if (a <= 0.01f) return;
    dc->PushAxisAlignedClip({r.left + 2, r.top, r.right - 2, r.bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    int px = static_cast<int>(std::ceil(kDockIcon * Scale()));
    float cy = (r.top + r.bottom) / 2 - 1;
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& it = items_[i];
        float cx = r.left + it.x.value - scroll_.value + kDockCell / 2;
        float prs = anim_.Get(gfx::HoverBank::Press, static_cast<int>(i));
        float s = it.scale.value * (1 + 0.06f * it.lift.value - 0.12f * prs);
        float alpha = std::clamp(it.alpha.value, 0.0f, 1.0f) * a;
        float hvr = anim_.Get(gfx::HoverBank::Hover, static_cast<int>(i));
        if (hvr > 0.01f || prs > 0.01f) {
            D2D1_RECT_F hr{cx - kDockCell / 2 + 3, r.top + 4, cx + kDockCell / 2 - 3, r.bottom - 4};
            draw::RoundRect(dc, hr, 10, Rgba(255, 255, 255, (0.06f * hvr + 0.05f * prs) * a));
        }
        float half = kDockIcon / 2 * s;
        D2D1_RECT_F ir{cx - half, cy - half - 2 * it.lift.value, cx + half, cy + half - 2 * it.lift.value};
        if (auto* bmp = gfx::IconCache::Get().Get(it.group.iconKey, px)) draw::Bitmap(dc, bmp, ir, alpha);
        else draw::RoundRect(dc, ir, 7, Rgba(255, 255, 255, 0.08f * alpha)); // placeholder until the real icon loads
        bool allMin = std::all_of(it.group.windows.begin(), it.group.windows.end(), [](auto& w) { return w.minimized; });
        float iy = r.bottom - 4.5f;
        if (it.group.active) {
            draw::RoundRect(dc, {cx - 7, iy - 1.5f, cx + 7, iy + 1.5f}, 1.5f, Rgba(236, 236, 238, 0.95f * alpha));
        } else if (it.group.windows.size() > 1) {
            auto b = Rgba(200, 200, 205, 0.55f * alpha);
            dc->FillEllipse(D2D1::Ellipse({cx - 3, iy}, 1.5f, 1.5f), draw::Brush(dc, b));
            dc->FillEllipse(D2D1::Ellipse({cx + 3, iy}, 1.5f, 1.5f), draw::Brush(dc, b));
        } else if (allMin) {
            dc->FillEllipse(D2D1::Ellipse({cx, iy}, 1.5f, 1.5f), draw::Brush(dc, Rgba(200, 200, 205, 0.35f * alpha)));
        }
    }
    dc->PopAxisAlignedClip();
    // overflow hints
    float total = 0;
    for (auto& it : items_)
        if (!it.removing) total += kDockCell;
    if (total + 2 * kDockPad > maxContentW_ + 1) {
        if (scroll_.value > 1) draw::Glyph(dc, draw::glyph::ChevronLeft, {r.left + 7, cy}, 9, Rgba(255, 255, 255, 0.5f * a));
        if (scroll_.value < total + 2 * kDockPad - maxContentW_ - 1)
            draw::Glyph(dc, draw::glyph::ChevronRight, {r.right - 7, cy}, 9, Rgba(255, 255, 255, 0.5f * a));
    }
}

void DockWindow::OnMouseMove(PointF p) {
    if (!edge_.Model().Hovered()) edge_.PointerEnter();
    int h = HitTest(p);
    if (h != hot_) {
        if (hot_ >= 0 && hot_ < static_cast<int>(items_.size())) items_[hot_].lift.To(0);
        hot_ = h;
        anim_.SetHot(h < 0 ? gfx::HoverBank::kNone : h);
        if (hot_ >= 0) {
            items_[hot_].lift.To(1);
            const auto& g = items_[hot_].group;
            std::wstring tip = g.name;
            if (g.windows.size() > 1) tip += L" (" + std::to_wstring(g.windows.size()) + L" pencere)";
            auto r = DockRect();
            float cx = r.left + items_[hot_].x.value - scroll_.value + kDockCell / 2;
            Tooltip::Get().Request(tip, DipToScreen({cx, r.top}), false);
        } else {
            Tooltip::Get().Hide();
        }
        RequestAnimationFrame();
    }
}

void DockWindow::OnMouseLeave() {
    edge_.PointerLeave();
    if (hot_ >= 0 && hot_ < static_cast<int>(items_.size())) items_[hot_].lift.To(0);
    hot_ = pressed_ = -1;
    anim_.SetHot(gfx::HoverBank::kNone);
    anim_.SetPressed(gfx::HoverBank::kNone);
    Tooltip::Get().Hide();
    RequestAnimationFrame();
}

void DockWindow::OnMouseDown(PointF p, int button) {
    pressed_ = HitTest(p);
    anim_.SetPressed(pressed_ < 0 ? gfx::HoverBank::kNone : pressed_);
    RequestAnimationFrame();
    if (pressed_ < 0 && onBlankClick) onBlankClick();
    Invalidate();
}

void DockWindow::OnMouseUp(PointF p, int button) {
    int i = HitTest(p);
    if (button == 2) {
        // Middle click: close the app's window(s); with Ctrl: end the task.
        if (i < 0) return;
        Tooltip::Get().Hide();
        const auto g = items_[i].group;
        std::vector<HWND> hwnds;
        for (auto& w : g.windows) hwnds.push_back(w.hwnd);
        if (GetKeyState(VK_CONTROL) < 0) sys::TerminateApp(g.exePath, hwnds);
        else
            for (HWND h : hwnds) sys::WindowTracker::Close(h);
        return;
    }
    bool same = i == pressed_ && i >= 0;
    pressed_ = -1;
    anim_.SetPressed(gfx::HoverBank::kNone);
    Invalidate();
    if (!same) return;
    Tooltip::Get().Hide();
    if (button == 0) ClickItem(i);
    else if (button == 1) {
        POINT pt;
        GetCursorPos(&pt);
        MenuForItem(i, pt);
    }
}

void DockWindow::OnWheel(PointF, int delta) {
    float total = 0;
    for (auto& it : items_)
        if (!it.removing) total += kDockCell;
    float maxScroll = std::max(0.0f, total + 2 * kDockPad - maxContentW_);
    if (maxScroll <= 0) return;
    scroll_.To(std::clamp(scroll_.target - delta / 120.0f * kDockCell * 2, 0.0f, maxScroll));
    hot_ = -1;
    RequestAnimationFrame();
}

void DockWindow::ClickItem(int i) {
    const auto& g = items_[i].group;
    if (g.windows.empty()) return;
    if (g.windows.size() == 1) {
        HWND h = g.windows[0].hwnd;
        if (g.active && !IsIconic(h)) sys::WindowTracker::Minimize(h);
        else sys::WindowTracker::Activate(h);
        return;
    }
    // Several windows. Second click on the same icon while its list is open: bring all of them up.
    if (MenuPopup::Get().IsOpen() && menuKey_ == g.key) {
        MenuPopup::Get().Close(false);
        menuKey_.clear();
        for (const auto& w : g.windows) sys::WindowTracker::Activate(w.hwnd);
        return;
    }
    menuKey_ = g.key;
    // First click: let the user pick one.
    std::vector<MenuItem> menu;
    for (const auto& w : g.windows) {
        HWND h = w.hwnd;
        MenuItem m;
        m.text = w.title.empty() ? g.name : w.title;
        m.glyph = w.minimized ? draw::glyph::ChevronDown : 0;
        m.checked = h == sys::WindowTracker::Get().Foreground();
        m.action = [h] { sys::WindowTracker::Activate(h); };
        menu.push_back(std::move(m));
    }
    auto r = DockRect();
    float cx = r.left + items_[i].x.value - scroll_.value + kDockCell / 2;
    POINT pt = DipToScreen({cx, r.top - 6});
    auto& sc = Screen::Current();
    pt.x -= sc.Px(100);
    pt.y -= sc.Px(static_cast<float>(menu.size()) * 32 + 30);
    MenuPopup::Get().Show(std::move(menu), pt);
}

void DockWindow::MenuForItem(int i, POINT at) {
    const auto g = items_[i].group;
    std::vector<MenuItem> menu;
    for (const auto& w : g.windows) {
        HWND h = w.hwnd;
        menu.push_back({w.title.empty() ? g.name : w.title, 0, [h] { sys::WindowTracker::Activate(h); }});
    }
    std::vector<HWND> hwnds;
    for (auto& w : g.windows) hwnds.push_back(w.hwnd);
    MenuItem closeItem{hwnds.size() > 1 ? L"Tüm pencereleri kapat" : L"Pencereyi kapat", draw::glyph::Close, [hwnds] {
                           for (HWND h : hwnds) sys::WindowTracker::Close(h);
                       }};
    closeItem.separatorBefore = true;
    menu.push_back(std::move(closeItem));
    std::wstring exe = g.exePath;
    menu.push_back({L"Görevi sonlandır", draw::glyph::Delete, [exe, hwnds] { sys::TerminateApp(exe, hwnds); }});
    auto& sc = Screen::Current();
    at.y -= sc.Px(static_cast<float>(menu.size()) * 32 + 40);
    MenuPopup::Get().Show(std::move(menu), at);
}

} // namespace nb::ui
