#pragma once
// The panel next to the launcher bar. Distinct states:
//   Favorites / Folder / AllApps / Settings  (empty query, opened by hover or keyboard)
//   Results                                    (typed query: calculation, apps, files, web)
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gfx/Anim.h"
#include "gfx/Surface.h"
#include "model/PanelDirection.h"
#include "model/Search.h"
#include "sys/FileSearch.h"
#include "ui/TextInput.h"

namespace nb::ui {

class LauncherPanel : public gfx::Surface {
public:
    enum class View { Favorites, Folder, AllApps, Settings, Results };

    bool Init();
    // Places the panel next to the bar on the given side. The window also covers the gap between
    // bar and panel, so crossing the gap never counts as leaving.
    void PlaceNear(const RECT& barScreen, PanelDir dir);
    void Appear(bool show);
    bool Shown() const { return shown_; }

    void SetView(View v);
    View CurrentView() const { return view_; }
    void ResetToRoot();
    void SetQuery(const std::wstring& q);
    bool HandleKey(UINT vk); // keys forwarded from the search field
    void Refresh();          // data changed
    void CancelSearches() { gate_.CancelAll(); }

    std::function<void()> onPointerEnter;
    std::function<void()> onPointerLeave;
    std::function<void(bool restoreFocus)> requestClose;
    std::function<void()> refocusSearch;

protected:
    void Paint(ID2D1DeviceContext* dc) override;
    bool Animate(float dt) override;
    void OnMouseMove(PointF p) override;
    void OnMouseLeave() override;
    void OnMouseDown(PointF p, int button) override;
    void OnMouseUp(PointF p, int button) override;
    void OnWheel(PointF p, int delta) override;

public:
    struct Hit;
    struct Impl;

private:
    std::shared_ptr<Impl> d_;
    View view_ = View::Favorites;
    bool shown_ = false;
    search::QueryGate gate_;
};

} // namespace nb::ui
