#pragma once
#include <windows.h>

#include <memory>

#include "model/Notifications.h"
#include "model/VisibilityModel.h"

namespace nb {

namespace ui {
class NotchWindow;
class DockWindow;
class Launcher;
class PopupHost;
class ToastHost;
} // namespace ui

struct Options {
    bool preview = false;     // no taskbar/autostart changes, separate data dir
    bool fromAutostart = false;
};

class App {
public:
    static App& Get();
    int Run(const Options& o);
    void Quit();

    const Options& Opts() const { return opts_; }
    ScreenMode Mode() const { return mode_; }
    bool Paused() const { return paused_; } // locked / suspended / user switched away

    // Notifications (Windows + our own), bounded and in memory only.
    NotificationHistory& Notifications() { return history_; }
    size_t UnseenNotifications() const { return history_.UnseenCount(); }
    void PostInternalNotification(const std::wstring& title, const std::wstring& body);
    void OnWindowsNotifications(const std::vector<NotificationItem>& current, bool silent);

    // Settings side effects
    void ApplySettings();
    void SetTaskbarHidden(bool hide);
    void RestoreTaskbarNow();
    bool SetAutostart(bool on, std::wstring* error);
    void EnableWindowsNotifications(bool on);

    ui::Launcher& Launcher() { return *launcher_; }
    ui::PopupHost& Popups() { return *popups_; }
    HWND MessageWindow() const { return msgWnd_; }

private:
    static LRESULT CALLBACK MainProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMessage(UINT m, WPARAM wp, LPARAM lp);
    bool CreateMessageWindow();
    void Startup();
    void StartDeferred();
    void Loop();
    void Relayout();
    void SetPaused(bool paused, const char* why);
    void OnScreenMode(ScreenMode m);
    void FirstRun();
    void UpdatePins();

    Options opts_;
    HWND msgWnd_ = nullptr;
    ScreenMode mode_ = ScreenMode::Desktop;
    bool paused_ = false;
    bool quitting_ = false;
    uint64_t relayoutTimer_ = 0;
    NotificationHistory history_;
    std::unique_ptr<ui::NotchWindow> notch_;
    std::unique_ptr<ui::DockWindow> dock_;
    std::unique_ptr<ui::Launcher> launcher_;
    std::unique_ptr<ui::PopupHost> popups_;
    std::unique_ptr<ui::ToastHost> toasts_;
};

} // namespace nb
