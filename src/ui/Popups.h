#pragma once
// Boxes that open under the notch: volume, media, battery/devices, notifications.
// Each is a separate rounded surface with a visible gap to the notch.
#include <functional>
#include <memory>

#include "ui/Widgets.h"

namespace nb::ui {

enum class PopupKind { Volume, Media, Battery, Notifications };

class PopupHost {
public:
    PopupHost();
    ~PopupHost();
    void Init();
    void Toggle(PopupKind k, POINT anchorBottomCenter);
    void CloseAll();
    bool AnyOpen() const;
    bool IsOpen(PopupKind k) const;
    void Refresh(PopupKind k); // data changed
    std::function<void()> onOpenChanged;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nb::ui
