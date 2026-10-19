#include "pch.h"

#include "sys/PrivacyMonitor.h"

#include <appmodel.h>
#include <tlhelp32.h>

#include <set>

#include "core/Dispatcher.h"

namespace nb::sys {
namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// Windows leaves LastUsedTimeStop at 0 when an app crashes or is killed while using the device, so the
// ledger alone can show a device "in use" forever. An entry only counts while its app is really running
// (exe path for classic apps, package family for packaged ones) and was started after this boot.
struct RunningApps {
    std::set<std::wstring> exes, families;
    ULONGLONG at = 0;
};

const RunningApps& Running() {
    static RunningApps r;
    ULONGLONG now = GetTickCount64();
    if (r.at && now - r.at < 4000) return r;
    r.exes.clear();
    r.families.clear();
    r.at = now;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return r;
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD n = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(h, 0, path, &n)) r.exes.insert(Lower(path));
        wchar_t fam[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
        UINT32 fl = ARRAYSIZE(fam);
        if (GetPackageFamilyName(h, &fl, fam) == ERROR_SUCCESS) r.families.insert(Lower(fam));
        CloseHandle(h);
    }
    CloseHandle(snap);
    return r;
}

bool AppIsRunning(const std::wstring& key, bool packaged) {
    const auto& r = Running();
    if (packaged) return r.families.count(Lower(key)) != 0;
    std::wstring path = key; // "C:#Program Files#App#app.exe"
    std::replace(path.begin(), path.end(), L'#', L'\\');
    return r.exes.count(Lower(path)) != 0;
}

ULONGLONG BootFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG now = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return now - GetTickCount64() * 10000ULL;
}

const wchar_t* kStore = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";

std::wstring DisplayName(std::wstring key) {
    // Non-packaged apps are stored as "C:#Program Files#App#app.exe"; packaged ones as the package family name.
    if (auto p = key.rfind(L'#'); p != std::wstring::npos) key = key.substr(p + 1);
    if (key.size() > 4 && _wcsicmp(key.c_str() + key.size() - 4, L".exe") == 0) key.resize(key.size() - 4);
    if (auto u = key.find(L'_'); u != std::wstring::npos && key.find(L'.') != std::wstring::npos) key.resize(u);
    return key;
}

bool InUse(HKEY k, const wchar_t* sub) {
    HKEY h;
    if (RegOpenKeyExW(k, sub, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return false;
    ULONGLONG start = 0, stop = 0;
    DWORD n = sizeof start, type = 0;
    bool ok = RegQueryValueExW(h, L"LastUsedTimeStart", nullptr, &type, reinterpret_cast<BYTE*>(&start), &n) == ERROR_SUCCESS;
    n = sizeof stop;
    RegQueryValueExW(h, L"LastUsedTimeStop", nullptr, &type, reinterpret_cast<BYTE*>(&stop), &n);
    RegCloseKey(h);
    // started before this boot and never stopped = stale ledger entry
    return ok && start != 0 && stop == 0 && start >= BootFileTime();
}

// Collects the names of apps currently using `capability` ("microphone" / "webcam").
std::vector<std::wstring> Users(const wchar_t* capability) {
    std::vector<std::wstring> out;
    auto scan = [&](const std::wstring& path, bool packaged) {
        HKEY root;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &root) != ERROR_SUCCESS)
            return;
        wchar_t name[512];
        for (DWORD i = 0;; ++i) {
            DWORD n = ARRAYSIZE(name);
            if (RegEnumKeyExW(root, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            if (_wcsicmp(name, L"NonPackaged") == 0) continue;
            if (InUse(root, name) && AppIsRunning(name, packaged)) out.push_back(DisplayName(name));
        }
        RegCloseKey(root);
    };
    std::wstring base = std::wstring(kStore) + capability;
    scan(base, true);
    scan(base + L"\\NonPackaged", false);
    return out;
}
} // namespace

PrivacyMonitor& PrivacyMonitor::Get() {
    static PrivacyMonitor p;
    return p;
}

void PrivacyMonitor::Start() {
    started_ = true;
    Poll();
}

void PrivacyMonitor::SetPaused(bool paused) {
    if (paused == paused_) return;
    paused_ = paused;
    ui::ClearTimeout(timer_);
    if (!paused_ && started_) Poll();
}

void PrivacyMonitor::Arm() {
    ui::ClearTimeout(timer_);
    if (paused_) return;
    timer_ = ui::SetTimeout(2000, [this] {
        timer_ = 0;
        Poll();
    });
}

void PrivacyMonitor::Poll() {
    PrivacyState s;
    s.micApps = Users(L"microphone");
    s.cameraApps = Users(L"webcam");
    s.mic = !s.micApps.empty();
    s.camera = !s.cameraApps.empty();
    bool changed = s.mic != state_.mic || s.camera != state_.camera || s.micApps != state_.micApps || s.cameraApps != state_.cameraApps;
    state_ = std::move(s);
    if (changed)
        for (auto& l : listeners_) l();
    Arm();
}

} // namespace nb::sys
