#pragma once
// Shared UI building blocks: popup base (activatable, closes on outside click / Escape and
// returns focus), dark context menu, tooltip.
#include <functional>
#include <string>
#include <vector>

#include "gfx/Anim.h"
#include "gfx/Surface.h"
#include "model/PanelDirection.h"

namespace nb::ui {

// Monitor + scale used for all our surfaces.
struct Screen {
    RECT monitor{};
    RECT work{};
    float scale = 1; // physical px per DIP including the UI scale setting
    HMONITOR handle = nullptr;
    static Screen& Current();
    void Update(float uiScale);
    float Dip(int px) const { return px / scale; }
    int Px(float dip) const { return static_cast<int>(std::lround(dip * scale)); }
};

bool PointIn(const D2D1_RECT_F& r, PointF p);

class Popup : public gfx::Surface {
public:
    enum class Placement { BelowNotch, Free };

    bool Init(const wchar_t* name);
    // anchorX: screen x (physical) to center under; top: physical y of the popup's top edge.
    void OpenAt(int anchorX, int top);
    void Close(bool restoreFocus);
    bool IsOpen() const { return open_; }
    void Relayout(); // size changed (content), keep anchor
    std::function<void()> onClosed;

protected:
    virtual float ContentWidth() = 0;
    virtual float ContentHeight() = 0;
    virtual void PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) = 0;
    virtual bool OnKey(UINT vk) { return false; }
    virtual bool AnimateContent(float dt) { return false; }

    void Paint(ID2D1DeviceContext* dc) override;
    bool Animate(float dt) override;
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp) override;

    D2D1_RECT_F ContentRect() const; // DIP rect of the panel inside the window
    float margin_ = 14;              // room for the shadow

private:
    bool open_ = false, closing_ = false;
    gfx::Spring appear_ = gfx::FadeSpring(0);
    int anchorX_ = 0, top_ = 0;
    HWND prevForeground_ = nullptr;
};

struct MenuItem {
    std::wstring text;
    wchar_t glyph = 0;
    std::function<void()> action;
    bool enabled = true;
    bool separatorBefore = false;
    bool checked = false;
};

class MenuPopup : public Popup {
public:
    static MenuPopup& Get();
    void Show(std::vector<MenuItem> items, POINT screenPt);

protected:
    float ContentWidth() override;
    float ContentHeight() override;
    void PaintContent(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) override;
    bool OnKey(UINT vk) override;
    void OnMouseMove(PointF p) override;
    void OnMouseLeave() override;
    void OnMouseUp(PointF p, int button) override;

private:
    int HitTest(PointF p) const;
    void Choose(int i);
    std::vector<MenuItem> items_;
    int hot_ = -1;
};

class Tooltip : public gfx::Surface {
public:
    static Tooltip& Get();
    // Shows after a short delay; `below`=true places it under the anchor (screen px).
    void Request(const std::wstring& text, POINT anchor, bool below);
    void Hide();

protected:
    void Paint(ID2D1DeviceContext* dc) override;

private:
    std::wstring text_;
    uint64_t timer_ = 0;
    bool created_ = false;
};

} // namespace nb::ui
