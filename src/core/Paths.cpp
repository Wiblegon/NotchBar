#include "core/Paths.h"

#include <windows.h>
#include <shlobj.h>

#include "core/AtomicFile.h"

namespace nb::paths {
namespace {
bool g_preview = false;

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}
} // namespace

void SetPreview(bool preview) { g_preview = preview; }
bool IsPreview() { return g_preview; }

std::wstring SharedDataDir() {
    static std::wstring dir = [] {
        std::wstring d = KnownFolder(FOLDERID_LocalAppData) + L"\\NotchBar";
        file::EnsureDir(d);
        return d;
    }();
    return dir;
}

std::wstring DataDir() {
    if (!g_preview) return SharedDataDir();
    std::wstring d = SharedDataDir() + L"\\Preview";
    file::EnsureDir(d);
    return d;
}

std::wstring SettingsFile() { return DataDir() + L"\\settings.json"; }
std::wstring FavoritesFile() { return DataDir() + L"\\favorites.json"; }
std::wstring AppCacheFile() { return DataDir() + L"\\apps-cache.json"; }
std::wstring TaskbarStateFile() { return SharedDataDir() + L"\\taskbar-state.json"; }

std::wstring LogDir() {
    std::wstring d = DataDir() + L"\\logs";
    file::EnsureDir(d);
    return d;
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n);
}

std::wstring InstallDir() { return KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\NotchBar"; }

} // namespace nb::paths
