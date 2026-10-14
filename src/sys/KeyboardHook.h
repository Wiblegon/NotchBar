#pragma once
// Detects a *lone* Windows key press without breaking Win+<key> shortcuts.
//
// Method: a low-level keyboard hook on its own thread watches Win down/up. Win-down and all
// Win+X combinations always pass through untouched. Only when Win is released with no other key
// in between, the release is held back and re-sent preceded by an unassigned "mask" key
// (VK 0xE8). The shell then sees "Win+<mask>" instead of a lone Win tap and does not open Start;
// we open the launcher instead. The hook callback only flips flags and posts messages
// (no allocation, no I/O, no UI work).
#include <windows.h>

namespace nb::sys {

class KeyboardHook {
public:
    static KeyboardHook& Get();
    // `notify` receives `msg` (posted) whenever the lone Win key was tapped.
    bool Start(HWND notify, UINT msg);
    void Stop();
    void SetEnabled(bool enabled);
    bool Enabled() const;
};

} // namespace nb::sys
