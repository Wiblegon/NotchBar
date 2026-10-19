#include "pch.h"

#include "gfx/Surface.h"

#include "core/Dispatcher.h"
#include "core/Log.h"
#include "gfx/Graphics.h"

namespace nb::gfx {
namespace {
std::vector<Surface*> g_surfaces;
bool g_renderPosted = false;
float g_uiScale = 1.0f;
double g_lastStep = 0; // QPC seconds of the last animation step; 0 = animation (re)starting
const wchar_t* kClass = L"NotchBar.Surface";

double QpcSeconds() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

// Seconds per frame of the monitor the UI lives on (re-read at most every 2 s).
double RefreshInterval() {
    static double interval = 1.0 / 60;
    static double checked = -10;
    double now = QpcSeconds();
    if (now - checked < 2.0) return interval;
    checked = now;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        interval = 1.0 / dm.dmDisplayFrequency;
    return interval;
}

void EnsureClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = DefWindowProcW; // replaced per instance in Create via SetWindowLongPtr
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    done = true;
}
} // namespace

Surface::~Surface() { Destroy(); }

const std::vector<Surface*>& Surface::All() { return g_surfaces; }

void Surface::SetGlobalUiScale(float s) {
    g_uiScale = s;
    for (auto* x : g_surfaces) x->UpdateScale();
}

bool Surface::Create(const wchar_t* name, bool activatable, HWND owner) {
    EnsureClass();
    DWORD ex = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    if (!activatable) ex |= WS_EX_NOACTIVATE;
    hwnd_ = CreateWindowExW(ex, kClass, name, WS_POPUP, 0, 0, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Surface::WndProc));
    // Never show in Alt+Tab/Task View peeks and exclude from DWM transitions (no fade-in flashes).
    BOOL disable = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof disable);
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    UpdateScale();
    g_surfaces.push_back(this);
    return true;
}

void Surface::Destroy() {
    g_surfaces.erase(std::remove(g_surfaces.begin(), g_surfaces.end(), this), g_surfaces.end());
    ReleaseSurface();
    visual_ = nullptr;
    target_ = nullptr;
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void Surface::UpdateScale() {
    if (!hwnd_) return;
    float dpiScale = GetDpiForWindow(hwnd_) / 96.0f;
    float s = dpiScale * g_uiScale;
    if (s != scale_) {
        scale_ = s;
        ReleaseSurface();
        dirty_ = true;
    }
}

void Surface::Place(const RECT& r) {
    bool sizeChanged = (r.right - r.left) != (rect_.right - rect_.left) || (r.bottom - r.top) != (rect_.bottom - rect_.top);
    bool moved = r.left != rect_.left || r.top != rect_.top;
    rect_ = r;
    if (moved || sizeChanged)
        SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    UpdateScale();
    if (sizeChanged) {
        ReleaseSurface();
        Invalidate();
    }
}

void Surface::SetRegionDip(const RectF& r) {
    if (!hwnd_) return;
    HRGN rgn = CreateRectRgn(static_cast<int>(std::floor(r.left * scale_)), static_cast<int>(std::floor(r.top * scale_)),
                             static_cast<int>(std::ceil(r.right * scale_)), static_cast<int>(std::ceil(r.bottom * scale_)));
    SetWindowRgn(hwnd_, rgn, FALSE); // system owns rgn afterwards
}

void Surface::ClearRegion() {
    if (hwnd_) SetWindowRgn(hwnd_, nullptr, FALSE);
}

void Surface::Show(bool v) {
    if (v == visible_ || !hwnd_) return;
    visible_ = v;
    if (v) {
        dirty_ = true;
        Render(); // first frame before the window appears: no flash of stale content
        Graphics::Get().Commit();
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(hwnd_, SW_HIDE);
        animating_ = false;
        tracking_ = false;
        ReleaseSurface(); // hidden parts keep no GPU memory and never draw
        Graphics::Get().Commit();
    }
}

void Surface::Raise() {
    if (!visible_ || !hwnd_) return;
    // Only when something else actually sits above us (and is not a menu / tooltip, which must stay on top).
    bool covered = false;
    DWORD self = GetCurrentProcessId();
    for (HWND h = GetWindow(hwnd_, GW_HWNDPREV); h && !covered; h = GetWindow(h, GW_HWNDPREV)) {
        if (!IsWindowVisible(h)) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == self) continue;
        RECT r;
        if (!GetWindowRect(h, &r)) continue;
        RECT inter;
        if (!IntersectRect(&inter, &r, &rect_)) continue;
        wchar_t cls[32];
        GetClassNameW(h, cls, 32);
        if (wcscmp(cls, L"#32768") == 0 || wcscmp(cls, L"tooltips_class32") == 0) continue;
        covered = true;
    }
    if (!covered) return;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
}

void Surface::Invalidate() {
    dirty_ = true;
    if (!g_renderPosted && ui::MessageWindow()) {
        g_renderPosted = true;
        PostMessageW(ui::MessageWindow(), ui::WM_NB_RENDER, 0, 0);
    }
}

void Surface::RequestAnimationFrame() {
    if (!animating_) {
        if (!AnyAnimating()) g_lastStep = 0; // first animated frame advances exactly one refresh interval
        animating_ = true;
    }
    Invalidate();
}

PointF Surface::ScreenToDip(POINT p) const {
    return {(p.x - rect_.left) / scale_, (p.y - rect_.top) / scale_};
}

POINT Surface::DipToScreen(PointF p) const {
    return {rect_.left + static_cast<LONG>(std::lround(p.x * scale_)), rect_.top + static_cast<LONG>(std::lround(p.y * scale_))};
}

void Surface::ReleaseSurface() {
    if (surface_) {
        if (visual_) visual_->SetContent(nullptr);
        surface_ = nullptr;
    }
    surfaceSize_ = {};
}

bool Surface::Render() {
    if (!visible_ || !hwnd_) return false;
    auto& g = Graphics::Get();
    if (!g.DComp()) return false;
    if (generation_ != g.Generation() || !target_) {
        ReleaseSurface();
        visual_ = nullptr;
        target_ = nullptr;
        if (FAILED(g.DComp()->CreateTargetForHwnd(hwnd_, TRUE, target_.put()))) return false;
        g.DComp()->CreateVisual(visual_.put());
        target_->SetRoot(visual_.get());
        generation_ = g.Generation();
    }
    int w = std::max<int>(1, rect_.right - rect_.left), h = std::max<int>(1, rect_.bottom - rect_.top);
    if (!surface_ || surfaceSize_.cx != w || surfaceSize_.cy != h) {
        surface_ = nullptr;
        if (FAILED(g.DComp()->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, surface_.put())))
            return false;
        surfaceSize_ = {w, h};
        visual_->SetContent(surface_.get());
    }
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT off{};
    HRESULT hr = surface_->BeginDraw(nullptr, IID_PPV_ARGS(dc.put()), &off);
    if (FAILED(hr)) {
        if (g.RecoverIfLost()) RecreateAll();
        return false;
    }
    float dpi = 96.0f * scale_;
    dc->SetDpi(dpi, dpi);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(off.x / scale_, off.y / scale_));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // transparent target: no ClearType
    dc->PushAxisAlignedClip(D2D1::RectF(0, 0, w / scale_, h / scale_), D2D1_ANTIALIAS_MODE_ALIASED);
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    Paint(dc.get());
    dc->PopAxisAlignedClip();
    hr = surface_->EndDraw();
    dirty_ = false;
    if (FAILED(hr)) {
        if (g.RecoverIfLost()) RecreateAll();
        return false;
    }
    return true;
}

bool Surface::AnyAnimating() {
    for (auto* s : g_surfaces)
        if (s->visible_ && s->animating_) return true;
    return false;
}

void Surface::RenderAll(TimeMs) {
    g_renderPosted = false;
    // Animations advance in whole refresh intervals measured with the high-resolution clock
    // (never GetTickCount's ~15.6 ms steps), so motion is locked to the display's refresh rate.
    // Extra calls between two frames (render requests from input) only repaint, they never advance time.
    double interval = RefreshInterval();
    double t = QpcSeconds();
    float dt = 0;
    if (g_lastStep == 0 || t - g_lastStep > 0.1) {
        dt = static_cast<float>(interval);
        g_lastStep = t;
    } else if (t - g_lastStep >= interval * 0.5) {
        dt = static_cast<float>(std::clamp(std::round((t - g_lastStep) / interval), 1.0, 6.0) * interval);
        g_lastStep = t;
    }
    bool any = false;
    // Copy: Animate/Paint callbacks may show or hide other surfaces.
    auto list = g_surfaces;
    for (auto* s : list) {
        if (std::find(g_surfaces.begin(), g_surfaces.end(), s) == g_surfaces.end()) continue;
        if (!s->visible_) {
            s->animating_ = false;
            continue;
        }
        if (s->animating_ && dt > 0) {
            s->animating_ = s->Animate(dt);
            s->dirty_ = true;
        }
        if (s->dirty_) any |= s->Render();
    }
    if (any) Graphics::Get().Commit();
}

void Surface::RecreateAll() {
    for (auto* s : g_surfaces) {
        s->ReleaseSurface();
        s->visual_ = nullptr;
        s->target_ = nullptr;
        s->generation_ = 0;
        s->dirty_ = true;
    }
    if (ui::MessageWindow()) PostMessageW(ui::MessageWindow(), ui::WM_NB_RENDER, 0, 0);
}

LRESULT CALLBACK Surface::WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Surface*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, wp, lp);
    return self->HandleMessage(m, wp, lp);
}

LRESULT Surface::HandleMessage(UINT m, WPARAM wp, LPARAM lp) {
    auto dip = [&] { return PointF{GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_}; };
    switch (m) {
    case WM_MOUSEACTIVATE:
        if (GetWindowLongW(hwnd_, GWL_EXSTYLE) & WS_EX_NOACTIVATE) return MA_NOACTIVATE;
        break;
    case WM_MOUSEMOVE:
        if (!tracking_) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&t);
            tracking_ = true;
        }
        OnMouseMove(dip());
        return 0;
    case WM_MOUSELEAVE:
        tracking_ = false;
        OnMouseLeave();
        return 0;
    case WM_LBUTTONDOWN: SetCapture(hwnd_); OnMouseDown(dip(), 0); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); OnMouseUp(dip(), 0); return 0;
    case WM_RBUTTONDOWN: OnMouseDown(dip(), 1); return 0;
    case WM_RBUTTONUP: OnMouseUp(dip(), 1); return 0;
    case WM_MBUTTONUP: OnMouseUp(dip(), 2); return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        OnWheel(ScreenToDip(p), GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }
    case WM_DPICHANGED:
        UpdateScale();
        OnDpiChanged();
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, m, wp, lp);
}

} // namespace nb::gfx
