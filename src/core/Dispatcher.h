#pragma once
// UI-thread dispatcher: everything that touches windows/D2D runs on the single UI thread.
// Background threads and WinRT/COM callbacks hand results over with Post().
#include <windows.h>

#include <cstdint>
#include <functional>

#include "model/VisibilityModel.h"

namespace nb::ui {

constexpr UINT WM_NB_INVOKE = WM_APP + 1;
constexpr UINT WM_NB_RENDER = WM_APP + 2;

void InitDispatcher(HWND messageWindow);
HWND MessageWindow();
bool IsUiThread();

// Thread-safe. Runs fn on the UI thread (dropped silently during shutdown).
void Post(std::function<void()> fn);

// One-shot UI timers. Granularity is the system timer (~15 ms), fine for UI delays.
using TimerId = uint64_t;
TimerId SetTimeout(int ms, std::function<void()> fn);
void ClearTimeout(TimerId& id); // resets id to 0
bool HandleTimer(UINT_PTR id);  // called from the message window's WM_TIMER

TimeMs NowMs(); // monotonic

// Handle WM_NB_INVOKE inside the message window procedure.
void HandleInvoke(LPARAM lp);

} // namespace nb::ui
