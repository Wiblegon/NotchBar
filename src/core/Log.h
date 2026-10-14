#pragma once
// Size-capped log (512 KB, one rotated backup). Do not log search text, notification content,
// file paths of user documents or other personal data.
#include <string>

namespace nb::log {

void Init(const std::wstring& dir, const wchar_t* fileName = L"notchbar.log");
void Write(const char* level, const char* fmt, ...);

} // namespace nb::log

#define LOGI(...) ::nb::log::Write("INFO", __VA_ARGS__)
#define LOGW(...) ::nb::log::Write("WARN", __VA_ARGS__)
#define LOGE(...) ::nb::log::Write("ERR ", __VA_ARGS__)
