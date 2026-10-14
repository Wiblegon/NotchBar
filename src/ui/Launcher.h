#pragma once
// Center launcher: Windows key opens only the search bar (focused, ready to type).
// Hovering the bar opens the favorites/apps panel on the side the pointer came from.
#include <memory>

namespace nb::ui {

class Launcher {
public:
    Launcher();
    ~Launcher();
    bool Init();
    void Toggle();
    void Open();
    void Close(bool restoreFocus);
    bool IsOpen() const;
    void Layout();
    void OpenSettings();
    void Refresh(); // favorites/apps/settings changed

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nb::ui
