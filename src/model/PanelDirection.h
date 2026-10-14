#pragma once
#include <optional>

#include "model/VisibilityModel.h"

namespace nb {

struct PointF {
    float x = 0, y = 0;
};
struct RectF {
    float left = 0, top = 0, right = 0, bottom = 0;
    float width() const { return right - left; }
    float height() const { return bottom - top; }
    bool contains(PointF p) const { return p.x >= left && p.x < right && p.y >= top && p.y < bottom; }
};

// Decides on which side of the search bar the app panel opens, from how the pointer approached.
//  - moving downwards (came from above)  -> Below
//  - moving upwards (came from below)    -> Above
//  - side approach / no movement info    -> entry edge, else `sideDefault`
// `previous` is an earlier pointer sample (screen coords) if one is known.
PanelDir DeterminePanelDirection(PointF entry, const RectF& bar, std::optional<PointF> previous,
                                 PanelDir sideDefault = PanelDir::Below);

// Flips the preferred direction when the panel would not fit on that side.
PanelDir FitPanelDirection(PanelDir preferred, float spaceAbove, float spaceBelow, float panelHeight);

} // namespace nb
