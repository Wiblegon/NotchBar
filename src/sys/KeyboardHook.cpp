#include "pch.h"

#include "sys/KeyboardHook.h"

#include "core/Log.h"

namespace nb::sys {
namespace {
constexpr ULONG_PTR kMagic = 0x4E42484B; // marks our own injected input ("NBHK")
constexpr WORD kMaskKey = 0xE8;          // unassigned virtual key
constexpr UINT WM_REINJECT = WM_APP + 10;

std::atomic<bool> g_enabled{true};
std::atomic<DWORD> g_thread{0};
HWND g_notify = nullptr;
UINT g_msg = 0;
HHOOK g_hook = nullptr;
HANDLE g_threadHandle = nullptr;

// Hook-thread-only state
bool g_winDown = false;
bool g_combo = false;

LRESULT CALLBACK Proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (k->dwExtraInfo != kMagic) {
            bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
            bool isWin = k->vkCode == VK_LWIN || k->vkCode == VK_RWIN;
            if (isWin) {
                if (down) {
                    if (!g_winDown) {
                        g_winDown = true;
                        g_combo = false;
                    }
                } else if (g_winDown) {
                    g_winDown = false;
                    if (!g_combo && g_enabled.load(std::memory_order_relaxed)) {
                        PostThreadMessageW(g_thread.load(), WM_REINJECT, k->vkCode, 0);
                        PostMessageW(g_notify, g_msg, 0, 0);
                        return 1; // hold back this Win-up; re-sent with the mask key
                    }
                }
            } else if (g_winDown && down) {
                g_combo = true; // Win+E, Win+D, Win+Shift+S, Win+Ctrl+arrows ... pass untouched
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void Reinject(WORD winVk) {
    INPUT in[3]{};
    for (auto& i : in) {
        i.type = INPUT_KEYBOARD;
        i.ki.dwExtraInfo = kMagic;
    }
    in[0].ki.wVk = kMaskKey;
    in[1].ki.wVk = kMaskKey;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    in[2].ki.wVk = winVk;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
    SendInput(3, in, sizeof(INPUT));
}

DWORD WINAPI ThreadMain(void*) {
    // Keep the hook responsive even when the UI thread is busy rendering.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, Proc, GetModuleHandleW(nullptr), 0);
    if (!g_hook) {
        LOGE("keyboard hook failed (%lu)", GetLastError());
        return 1;
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_REINJECT) Reinject(static_cast<WORD>(m.wParam));
    }
    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    return 0;
}
} // namespace

KeyboardHook& KeyboardHook::Get() {
    static KeyboardHook k;
    return k;
}

bool KeyboardHook::Start(HWND notify, UINT msg) {
    if (g_threadHandle) return true;
    g_notify = notify;
    g_msg = msg;
    DWORD tid = 0;
    g_threadHandle = CreateThread(nullptr, 0, ThreadMain, nullptr, 0, &tid);
    g_thread = tid;
    LOGI("keyboard hook thread started");
    return g_threadHandle != nullptr;
}

void KeyboardHook::Stop() {
    if (!g_threadHandle) return;
    PostThreadMessageW(g_thread.load(), WM_QUIT, 0, 0);
    WaitForSingleObject(g_threadHandle, 2000);
    CloseHandle(g_threadHandle);
    g_threadHandle = nullptr;
}

void KeyboardHook::SetEnabled(bool e) { g_enabled = e; }
bool KeyboardHook::Enabled() const { return g_enabled; }

} // namespace nb::sys
