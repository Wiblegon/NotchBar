#include "core/AtomicFile.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>

namespace nb::file {

bool WriteAtomic(const std::wstring& path, std::string_view data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::optional<std::string> Read(const std::wstring& path, size_t maxBytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
        CloseHandle(h);
        return std::nullopt;
    }
    std::string s(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = s.empty() || ReadFile(h, s.data(), static_cast<DWORD>(s.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok || read != s.size()) return std::nullopt;
    return s;
}

std::wstring Quarantine(const std::wstring& path) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t suffix[64];
    swprintf(suffix, 64, L".corrupt-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring dst = path + suffix;
    MoveFileExW(path.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
    return dst;
}

bool EnsureDir(const std::wstring& dir) {
    int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

} // namespace nb::file
