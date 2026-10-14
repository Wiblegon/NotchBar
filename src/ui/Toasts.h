#pragma once
// Transient notification boxes under the notch (separate from it, like the search bar).
// Hiding a box is visual only; the Windows notification is never removed by us.
#include <memory>

#include "model/Notifications.h"

namespace nb::ui {

class ToastHost {
public:
    ToastHost();
    ~ToastHost();
    void Init();
    void Push(const NotificationItem& item, bool suppressed);
    void Clear();
    void Layout();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nb::ui
