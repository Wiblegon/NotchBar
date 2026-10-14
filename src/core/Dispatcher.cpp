#include "core/Dispatcher.h"

#include <atomic>
#include <unordered_map>

namespace nb::ui {
namespace {
HWND g_wnd = nullptr;
DWORD g_uiThread = 0;
std::unordered_map<UINT_PTR, std::function<void()>> g_timers;
UINT_PTR g_nextTimer = 1000;
} // namespace

void InitDispatcher(HWND w) {
    g_wnd = w;
    g_uiThread = GetCurrentThreadId();
}

HWND MessageWindow() { return g_wnd; }
bool IsUiThread() { return GetCurrentThreadId() == g_uiThread; }

void Post(std::function<void()> fn) {
    if (!g_wnd) return;
    auto* p = new std::function<void()>(std::move(fn));
    if (!PostMessageW(g_wnd, WM_NB_INVOKE, 0, reinterpret_cast<LPARAM>(p))) delete p;
}

void HandleInvoke(LPARAM lp) {
    std::unique_ptr<std::function<void()>> p(reinterpret_cast<std::function<void()>*>(lp));
    if (p && *p) (*p)();
}

TimerId SetTimeout(int ms, std::function<void()> fn) {
    UINT_PTR id = ++g_nextTimer;
    g_timers[id] = std::move(fn);
    SetTimer(g_wnd, id, static_cast<UINT>(ms < USER_TIMER_MINIMUM ? USER_TIMER_MINIMUM : ms), nullptr);
    return id;
}

void ClearTimeout(TimerId& id) {
    if (!id) return;
    KillTimer(g_wnd, static_cast<UINT_PTR>(id));
    g_timers.erase(static_cast<UINT_PTR>(id));
    id = 0;
}

bool HandleTimer(UINT_PTR id) {
    auto it = g_timers.find(id);
    if (it == g_timers.end()) return false;
    KillTimer(g_wnd, id);
    auto fn = std::move(it->second);
    g_timers.erase(it);
    if (fn) fn();
    return true;
}

TimeMs NowMs() { return static_cast<TimeMs>(GetTickCount64()); }

} // namespace nb::ui
