#pragma once
// While a menu, popup or the search bar is open: reports every mouse button press anywhere on
// screen (low-level hook on the UI thread), so "click elsewhere closes it" works even when
// Windows never moved activation (e.g. SetForegroundWindow was refused).
#include <windows.h>

#include <functional>

namespace nb::sys {

class MouseWatch {
public:
    static MouseWatch& Get();
    // Installs/removes the hook. `onPress` runs later on the UI thread with the screen position.
    void SetActive(bool on);
    std::function<void(POINT)> onPress;

private:
    HHOOK hook_ = nullptr;
};

} // namespace nb::sys
