#include "pch.h"

#include "app/Startup.h"

#include <wtsapi32.h>

#include "core/Log.h"
#include "core/Paths.h"

namespace nb::startup {
namespace {
LARGE_INTEGER g_freq{}, g_t0{};
struct Stage {
    const char* name;
    double ms;
};
std::vector<Stage> g_stages;

double SinceT0() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - g_t0.QuadPart) * 1000.0 / g_freq.QuadPart;
}

int64_t FileTimeMs(const FILETIME& ft) {
    return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 10000);
}
int64_t NowFileMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return FileTimeMs(ft);
}
} // namespace

void Begin() {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    g_stages.clear();
}

void Mark(const char* stage) { g_stages.push_back({stage, SinceT0()}); }

void Report(bool fromAutostart) {
    int64_t now = NowFileMs();
    double inProcess = SinceT0();
    FILETIME c, e, k, u;
    int64_t created = 0;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) created = FileTimeMs(c);
    int64_t logon = 0;
    WTSINFOW* info = nullptr;
    DWORD bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfo,
                                    reinterpret_cast<LPWSTR*>(&info), &bytes) && info) {
        FILETIME lf{static_cast<DWORD>(info->LogonTime.LowPart), static_cast<DWORD>(info->LogonTime.HighPart)};
        logon = FileTimeMs(lf);
        WTSFreeMemory(info);
    }
    char line[2048];
    int n = snprintf(line, sizeof line, "launch=%s process_start_to_ready_ms=%.1f creation_to_ready_ms=%lld",
                     fromAutostart ? "autostart" : "manual", inProcess, created ? static_cast<long long>(now - created) : -1);
    std::string s(line, n > 0 ? n : 0);
    if (logon > 0 && fromAutostart) {
        snprintf(line, sizeof line, " logon_to_process_ms=%lld logon_to_ready_ms=%lld", static_cast<long long>(created - logon),
                 static_cast<long long>(now - logon));
        s += line;
    }
    s += " stages:";
    double prev = 0;
    for (auto& st : g_stages) {
        snprintf(line, sizeof line, " %s=+%.1f(@%.1f)", st.name, st.ms - prev, st.ms);
        s += line;
        prev = st.ms;
    }
    LOGI("startup %s", s.c_str());
    // Separate small file that keeps the last 20 measurements for the report.
    std::wstring path = paths::LogDir() + L"\\startup.log";
    std::vector<std::string> lines;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[4096];
        while (fgets(buf, sizeof buf, f)) lines.emplace_back(buf);
        fclose(f);
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    lines.push_back(std::string(line) + s + "\n");
    while (lines.size() > 20) lines.erase(lines.begin());
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        for (auto& l : lines) fputs(l.c_str(), f);
        fclose(f);
    }
}

} // namespace nb::startup
