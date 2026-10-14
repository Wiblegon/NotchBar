#pragma once
// Tracks the user's open application windows from real system windows, event-driven
// (SetWinEventHook, out-of-context). No per-frame or periodic enumeration.
#include <windows.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace nb::sys {

struct AppWindow {
    HWND hwnd = nullptr;
    std::wstring title;
    bool minimized = false;
};

struct AppGroup {
    std::wstring key;      // AppUserModelID when available, otherwise the executable path
    std::wstring appId;    // AppsFolder id usable for launching/favorites (may be empty)
    std::wstring exePath;
    std::wstring name;     // display name
    std::wstring iconKey;  // IconCache key
    std::vector<AppWindow> windows;
    bool active = false;
};

class WindowTracker {
public:
    static WindowTracker& Get();

    void Start();
    void Stop();

    const std::vector<AppGroup>& Groups() const { return groups_; }
    HWND Foreground() const { return foreground_; }

    void Subscribe(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }
    // Raw event taps for other modules (screen state, taskbar, notifications)
    using EventTap = std::function<void(DWORD event, HWND hwnd)>;
    void Tap(EventTap fn) { taps_.push_back(std::move(fn)); }

    static void Activate(HWND hwnd);
    static void Minimize(HWND hwnd);
    static void Close(HWND hwnd);
    static bool IsTaskbarWindow(HWND hwnd);

    // Resolves app identity for a window (also used by notifications/media icon lookup).
    static std::wstring WindowAppId(HWND hwnd);
    static std::wstring ProcessPath(DWORD pid);

    void OnWinEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild);

private:
    void Flush();
    bool Upsert(HWND hwnd); // true if the visible structure changed
    void Remove(HWND hwnd);
    void Notify();
    void RehookForeground();

    std::vector<AppGroup> groups_;
    std::vector<HWINEVENTHOOK> hooks_;
    HWINEVENTHOOK fgLocationHook_ = nullptr;
    DWORD fgPid_ = 0;
    HWND foreground_ = nullptr;
    std::set<HWND> pending_;
    bool flushPosted_ = false;
    std::vector<std::function<void()>> listeners_;
    std::vector<EventTap> taps_;
};

} // namespace nb::sys
