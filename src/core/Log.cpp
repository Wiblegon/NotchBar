#include "core/Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace nb::log {
namespace {
std::mutex g_mu;
std::wstring g_path;
constexpr LONGLONG kMaxBytes = 512 * 1024;

void RotateIfNeeded() {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &fa)) return;
    LONGLONG size = (static_cast<LONGLONG>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    if (size < kMaxBytes) return;
    std::wstring old = g_path + L".1";
    MoveFileExW(g_path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
}
} // namespace

void Init(const std::wstring& dir, const wchar_t* fileName) {
    std::lock_guard lk(g_mu);
    g_path = dir + L"\\" + fileName;
}

void Write(const char* level, const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1200];
    int n = snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] [%lu] %s\r\n", st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level, GetCurrentThreadId(), msg);
#ifdef _DEBUG
    OutputDebugStringA(line);
#endif
    std::lock_guard lk(g_mu);
    if (g_path.empty()) return;
    RotateIfNeeded();
    HANDLE h = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, line, static_cast<DWORD>(n > 0 ? std::min<int>(n, sizeof line - 1) : 0), &w, nullptr);
    CloseHandle(h);
}

} // namespace nb::log
