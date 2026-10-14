#include "model/PanelDirection.h"

#include <cmath>

namespace nb {

PanelDir DeterminePanelDirection(PointF entry, const RectF& bar, std::optional<PointF> previous, PanelDir sideDefault) {
    if (previous) {
        float dx = entry.x - previous->x;
        float dy = entry.y - previous->y;
        // Vertical motion must clearly dominate; a mostly horizontal approach is a side approach.
        if (std::fabs(dy) >= 2.0f && std::fabs(dy) >= std::fabs(dx) * 0.6f) return dy > 0 ? PanelDir::Below : PanelDir::Above;
    }
    float band = bar.height() * 0.3f;
    if (entry.y <= bar.top + band) return PanelDir::Below;
    if (entry.y >= bar.bottom - band) return PanelDir::Above;
    return sideDefault;
}

PanelDir FitPanelDirection(PanelDir preferred, float spaceAbove, float spaceBelow, float panelHeight) {
    if (preferred == PanelDir::Below && spaceBelow < panelHeight && spaceAbove > spaceBelow) return PanelDir::Above;
    if (preferred == PanelDir::Above && spaceAbove < panelHeight && spaceBelow > spaceAbove) return PanelDir::Below;
    return preferred;
}

} // namespace nb
