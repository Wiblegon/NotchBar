#include "pch.h"

#include "sys/TrayApps.h"

#include <tlhelp32.h>

#include "sys/WindowTracker.h"

namespace nb::sys {
namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// "{FOLDERID guid}\sub\app.exe" -> real path (Windows stores known-folder ids instead of paths).
std::wstring ExpandKnownFolder(const std::wstring& p) {
    if (p.size() < 40 || p[0] != L'{') return p;
    auto close = p.find(L'}');
    if (close == std::wstring::npos) return p;
    GUID id;
    if (FAILED(IIDFromString(p.substr(0, close + 1).c_str(), &id))) return p;
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) return p;
    std::wstring r = std::wstring(folder) + p.substr(close + 1);
    CoTaskMemFree(folder);
    return r;
}

std::map<std::wstring, std::vector<DWORD>> RunningByPath() {
    std::map<std::wstring, std::vector<DWORD>> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        std::wstring path = WindowTracker::ProcessPath(pe.th32ProcessID);
        if (!path.empty()) out[Lower(path)].push_back(pe.th32ProcessID);
    }
    CloseHandle(snap);
    return out;
}

std::wstring ReadString(HKEY k, const wchar_t* name) {
    wchar_t buf[1024];
    DWORD n = sizeof buf, type = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &n) != ERROR_SUCCESS || type != REG_SZ) return {};
    return buf;
}

std::wstring FileStem(const std::wstring& path) {
    std::wstring n = PathFindFileNameW(path.c_str());
    if (auto dot = n.rfind(L'.'); dot != std::wstring::npos) n.resize(dot);
    return n;
}
} // namespace

std::vector<TrayApp> ListRunningTrayApps() {
    std::vector<TrayApp> apps;
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_ENUMERATE_SUB_KEYS, &root) != ERROR_SUCCESS)
        return apps;
    auto running = RunningByPath();
    std::set<std::wstring> seen;
    DWORD self = GetCurrentProcessId();
    wchar_t sub[256];
    for (DWORD i = 0;; ++i) {
        DWORD n = ARRAYSIZE(sub);
        if (RegEnumKeyExW(root, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) continue;
        std::wstring exe = ExpandKnownFolder(ReadString(k, L"ExecutablePath"));
        std::wstring tip = ReadString(k, L"InitialTooltip");
        RegCloseKey(k);
        if (exe.empty()) continue;
        std::wstring key = Lower(exe);
        auto it = running.find(key);
        if (it == running.end() || seen.count(key)) continue;
        if (std::find(it->second.begin(), it->second.end(), self) != it->second.end()) continue;
        seen.insert(key);
        std::wstring name = FileStem(exe);
        if (!tip.empty() && tip.size() < 40) name = tip; // short tooltips are usually the app name
        apps.push_back({name, exe});
    }
    RegCloseKey(root);
    std::sort(apps.begin(), apps.end(), [](const TrayApp& a, const TrayApp& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return apps;
}

void TerminateApp(const std::wstring& exePath, const std::vector<HWND>& windows) {
    std::set<DWORD> pids;
    std::wstring key = Lower(exePath);
    if (!key.empty()) {
        for (auto& [path, ids] : RunningByPath())
            if (path == key) pids.insert(ids.begin(), ids.end());
    }
    if (pids.empty()) {
        for (HWND h : windows) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid) pids.insert(pid);
        }
    }
    pids.erase(GetCurrentProcessId());
    for (DWORD pid : pids) {
        std::wstring path = Lower(WindowTracker::ProcessPath(pid));
        if (path.ends_with(L"\\explorer.exe")) { // never take the shell down: just close its windows
            for (HWND h : windows) PostMessageW(h, WM_CLOSE, 0, 0);
            return;
        }
    }
    for (DWORD pid : pids) {
        if (HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
            TerminateProcess(p, 1);
            CloseHandle(p);
        }
    }
}

void ActivateTrayApp(const TrayApp& app) {
    auto running = RunningByPath();
    auto it = running.find(Lower(app.exePath));
    if (it != running.end()) {
        struct Ctx {
            std::vector<DWORD>* pids;
            HWND best = nullptr;
            bool bestVisible = false;
        } ctx{&it->second};
        EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<Ctx*>(lp);
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (std::find(c->pids->begin(), c->pids->end(), pid) == c->pids->end()) return TRUE;
                LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
                if ((ex & WS_EX_TOOLWINDOW) || GetWindow(h, GW_OWNER) || GetWindowTextLengthW(h) == 0) return TRUE;
                bool vis = IsWindowVisible(h) != FALSE;
                if (!c->best || (vis && !c->bestVisible)) {
                    c->best = h;
                    c->bestVisible = vis;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        if (ctx.best) {
            if (!ctx.bestVisible) ShowWindow(ctx.best, SW_SHOW);
            WindowTracker::Activate(ctx.best);
            return;
        }
    }
    // No window to show: starting the app again makes single-instance apps surface their window.
    ShellExecuteW(nullptr, L"open", app.exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace nb::sys
