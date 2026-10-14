#include "pch.h"

#include "sys/WindowTracker.h"

#include <appmodel.h>

#include "core/Dispatcher.h"
#include "core/Log.h"
#include "core/Worker.h"

namespace nb::sys {
namespace {

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    WindowTracker::Get().OnWinEvent(event, hwnd, idObject, idChild);
}

std::wstring ClassName(HWND h) {
    wchar_t buf[128];
    int n = GetClassNameW(h, buf, 128);
    return std::wstring(buf, n > 0 ? n : 0);
}

std::wstring Title(HWND h) {
    wchar_t buf[256];
    int n = GetWindowTextW(h, buf, 256); // safe: internal for other processes, no message sent for top-level
    return std::wstring(buf, n > 0 ? n : 0);
}

bool IsCloaked(HWND h) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked != 0;
}

std::wstring PackagedAumid(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t buf[APPLICATION_USER_MODEL_ID_MAX_LENGTH];
    UINT32 len = ARRAYSIZE(buf);
    std::wstring r;
    if (GetApplicationUserModelId(p, &len, buf) == ERROR_SUCCESS) r = buf;
    CloseHandle(p);
    return r;
}

std::wstring PropertyAumid(HWND h) {
    winrt::com_ptr<IPropertyStore> ps;
    if (FAILED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(ps.put())))) return {};
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring r;
    if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) r = pv.pwszVal;
    PropVariantClear(&pv);
    return r;
}

// UWP apps live inside ApplicationFrameHost; the real app is the CoreWindow child.
DWORD HostedAppPid(HWND frame) {
    DWORD framePid = 0;
    GetWindowThreadProcessId(frame, &framePid);
    DWORD result = 0;
    struct Ctx { DWORD framePid; DWORD* out; } ctx{framePid, &result};
    EnumChildWindows(
        frame,
        [](HWND c, LPARAM lp) -> BOOL {
            auto* x = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(c, &pid);
            if (pid != x->framePid) {
                *x->out = pid;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return result;
}

struct Identity {
    std::wstring key, appId, exePath, iconKey;
    bool packaged = false;
};

Identity Identify(HWND h) {
    Identity id;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    id.exePath = WindowTracker::ProcessPath(pid);
    std::wstring exeLower = id.exePath;
    std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), ::towlower);
    if (exeLower.size() >= 24 && exeLower.ends_with(L"applicationframehost.exe")) {
        if (DWORD app = HostedAppPid(h)) {
            pid = app;
            id.exePath = WindowTracker::ProcessPath(app);
        }
    }
    std::wstring packaged = PackagedAumid(pid);
    std::wstring prop = PropertyAumid(h);
    if (!prop.empty()) id.appId = prop;
    else if (!packaged.empty()) id.appId = packaged;
    id.packaged = !packaged.empty();
    id.key = !id.appId.empty() ? id.appId : exeLower;
    if (id.key.empty()) id.key = L"hwnd:" + std::to_wstring(reinterpret_cast<uintptr_t>(h));
    if (id.packaged) id.iconKey = L"app:" + id.appId;
    else if (!id.exePath.empty()) id.iconKey = L"file:" + id.exePath;
    else {
        wchar_t buf[32];
        swprintf(buf, 32, L"hwnd:%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
        id.iconKey = buf;
    }
    return id;
}

std::wstring FileDescription(const std::wstring& path) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path.c_str(), &dummy);
    if (!size) return {};
    std::vector<uint8_t> buf(size);
    if (!GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path.c_str(), 0, size, buf.data())) return {};
    struct Lang { WORD lang, cp; }* langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) || !len) return {};
    wchar_t q[64];
    swprintf(q, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].cp);
    wchar_t* val = nullptr;
    if (VerQueryValueW(buf.data(), q, reinterpret_cast<void**>(&val), &len) && val && len > 1) return val;
    return {};
}

std::map<std::wstring, std::wstring> g_names; // key -> resolved display name (cache)
std::set<std::wstring> g_nameLoading;

} // namespace

WindowTracker& WindowTracker::Get() {
    static WindowTracker t;
    return t;
}

std::wstring WindowTracker::ProcessPath(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ARRAYSIZE(buf);
    std::wstring r;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) r.assign(buf, n);
    CloseHandle(p);
    return r;
}

std::wstring WindowTracker::WindowAppId(HWND h) { return Identify(h).key; }

bool WindowTracker::IsTaskbarWindow(HWND h) {
    if (!IsWindow(h) || !IsWindowVisible(h)) return false;
    if (GetAncestor(h, GA_ROOT) != h) return false;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    bool appWindow = ex & WS_EX_APPWINDOW;
    if ((ex & WS_EX_TOOLWINDOW) && !appWindow) return false;
    if (GetWindow(h, GW_OWNER) && !appWindow) return false;
    if ((ex & WS_EX_NOACTIVATE) && !appWindow) return false;
    if (IsCloaked(h)) return false; // other virtual desktops, hidden UWP frames
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    std::wstring cls = ClassName(h);
    if (cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd" ||
        cls == L"Windows.UI.Core.CoreWindow" || cls == L"XamlExplorerHostIslandWindow")
        return false;
    if (GetWindowTextLengthW(h) == 0 && !appWindow) return false;
    return true;
}

void WindowTracker::Start() {
    auto hook = [&](DWORD a, DWORD b) {
        HWINEVENTHOOK hk = SetWinEventHook(a, b, nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (hk) hooks_.push_back(hk);
    };
    hook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND);
    hook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND);
    hook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
    hook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE); // create, destroy, show, hide
    hook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE);
    hook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);

    groups_.clear();
    EnumWindows(
        [](HWND h, LPARAM) -> BOOL {
            if (IsTaskbarWindow(h)) WindowTracker::Get().Upsert(h);
            return TRUE;
        },
        0);
    // EnumWindows is top-to-bottom; present oldest-ish first like a taskbar.
    std::reverse(groups_.begin(), groups_.end());
    foreground_ = GetForegroundWindow();
    RehookForeground();
    Notify();
    LOGI("window tracker: %zu app groups", groups_.size());
}

void WindowTracker::Stop() {
    for (auto h : hooks_) UnhookWinEvent(h);
    hooks_.clear();
    if (fgLocationHook_) UnhookWinEvent(fgLocationHook_);
    fgLocationHook_ = nullptr;
}

void WindowTracker::RehookForeground() {
    // Location changes are only watched for the foreground process: maximize/restore/F11 of the
    // active app, without subscribing to every window (or cursor) movement on the system.
    DWORD pid = 0;
    if (foreground_) GetWindowThreadProcessId(foreground_, &pid);
    if (pid == fgPid_ && fgLocationHook_) return;
    if (fgLocationHook_) UnhookWinEvent(fgLocationHook_);
    fgLocationHook_ = nullptr;
    fgPid_ = pid;
    if (pid && pid != GetCurrentProcessId())
        fgLocationHook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, WinEventProc, pid, 0,
                                          WINEVENT_OUTOFCONTEXT);
}

void WindowTracker::OnWinEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
    // Child windows (browser render widgets, editors, ...) generate most events but never change
    // the app list or the screen mode: drop them before doing any work.
    if (event != EVENT_OBJECT_DESTROY && event != EVENT_SYSTEM_FOREGROUND && GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        for (auto& t : taps_) t(event, hwnd);
        return;
    }
    if (event == EVENT_OBJECT_DESTROY) {
        Remove(hwnd);
        for (auto& t : taps_) t(event, hwnd);
        return;
    }
    if (event == EVENT_OBJECT_CREATE) return; // wait for SHOW; created windows are usually not ready yet
    if (event == EVENT_SYSTEM_FOREGROUND) {
        foreground_ = hwnd;
        RehookForeground();
    }
    for (auto& t : taps_) t(event, hwnd);
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    pending_.insert(hwnd);
    if (!flushPosted_) {
        flushPosted_ = true;
        ui::Post([this] { Flush(); });
    }
}

void WindowTracker::Flush() {
    flushPosted_ = false;
    auto pending = std::move(pending_);
    pending_.clear();
    bool changed = false;
    for (HWND h : pending) {
        bool eligible = IsTaskbarWindow(h);
        bool tracked = false;
        for (auto& g : groups_)
            for (auto& w : g.windows)
                if (w.hwnd == h) tracked = true;
        if (eligible) {
            changed |= Upsert(h);
        } else if (tracked) {
            Remove(h);
            changed = true;
        }
    }
    // active flag
    HWND fg = foreground_;
    for (auto& g : groups_) {
        bool a = std::any_of(g.windows.begin(), g.windows.end(), [&](const AppWindow& w) { return w.hwnd == fg; });
        if (a != g.active) {
            g.active = a;
            changed = true;
        }
    }
    if (changed) Notify();
}

bool WindowTracker::Upsert(HWND h) {
    // Update existing entry in place.
    for (auto& g : groups_)
        for (auto& w : g.windows)
            if (w.hwnd == h) {
                w.title = Title(h);
                bool mini = IsIconic(h) != FALSE;
                bool changed = mini != w.minimized;
                w.minimized = mini;
                return changed;
            }
    Identity id = Identify(h);
    AppWindow w{h, Title(h), IsIconic(h) != FALSE};
    for (auto& g : groups_)
        if (g.key == id.key) {
            g.windows.push_back(w);
            return true;
        }
    AppGroup g;
    g.key = id.key;
    g.appId = id.appId;
    g.exePath = id.exePath;
    g.iconKey = id.iconKey;
    auto cached = g_names.find(id.key);
    g.name = cached != g_names.end() ? cached->second : w.title;
    g.windows.push_back(w);
    groups_.push_back(std::move(g));

    if (cached == g_names.end() && !g_nameLoading.count(id.key)) {
        g_nameLoading.insert(id.key);
        std::wstring key = id.key, appId = id.packaged ? id.appId : L"", exe = id.exePath;
        WorkerPool::Shell().Submit([this, key, appId, exe] {
            std::wstring name;
            if (!appId.empty()) {
                winrt::com_ptr<IShellItem> item;
                if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, appId.c_str(), IID_PPV_ARGS(item.put())))) {
                    PWSTR dn = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &dn))) name = dn;
                    CoTaskMemFree(dn);
                }
            }
            if (name.empty() && !exe.empty()) name = FileDescription(exe);
            if (name.empty() && !exe.empty()) {
                name = PathFindFileNameW(exe.c_str());
                if (auto dot = name.rfind(L'.'); dot != std::wstring::npos) name.resize(dot);
            }
            ui::Post([this, key, name] {
                g_nameLoading.erase(key);
                if (name.empty()) return;
                g_names[key] = name;
                for (auto& g : groups_)
                    if (g.key == key) g.name = name;
                Notify();
            });
        });
    }
    return true;
}

void WindowTracker::Remove(HWND h) {
    for (auto it = groups_.begin(); it != groups_.end(); ++it) {
        auto& ws = it->windows;
        auto w = std::find_if(ws.begin(), ws.end(), [&](const AppWindow& x) { return x.hwnd == h; });
        if (w != ws.end()) {
            ws.erase(w);
            if (ws.empty()) groups_.erase(it);
            Notify();
            return;
        }
    }
}

void WindowTracker::Notify() {
    for (auto& l : listeners_) l();
}

void WindowTracker::Activate(HWND h) {
    if (!IsWindow(h)) return;
    if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    if (!SetForegroundWindow(h)) {
        // Foreground lock: fall back to the shell's documented-as-legacy switch helper.
        SwitchToThisWindow(h, TRUE);
    }
}

void WindowTracker::Minimize(HWND h) {
    if (IsWindow(h)) ShowWindowAsync(h, SW_MINIMIZE);
}

void WindowTracker::Close(HWND h) {
    if (IsWindow(h)) PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0);
}

} // namespace nb::sys
