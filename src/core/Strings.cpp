#include "core/Strings.h"

#include <windows.h>

namespace nb {

std::string ToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

int FindInsensitive(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty()) return 0;
    if (haystack.empty()) return -1;
    int found = 0;
    int r = FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC,
                            haystack.data(), static_cast<int>(haystack.size()), needle.data(),
                            static_cast<int>(needle.size()), &found, nullptr, nullptr, 0);
    return r;
}

std::wstring ToLowerLinguistic(std::wstring_view s) {
    if (s.empty()) return {};
    int n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING, s.data(),
                          static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING, s.data(), static_cast<int>(s.size()),
                  out.data(), n, nullptr, nullptr, 0);
    return out;
}

std::wstring Ellipsize(std::wstring_view s, size_t maxChars) {
    if (s.size() <= maxChars) return std::wstring(s);
    if (maxChars == 0) return {};
    size_t cut = maxChars - 1;
    if (cut > 0 && IS_HIGH_SURROGATE(s[cut - 1])) --cut;
    std::wstring out(s.substr(0, cut));
    while (!out.empty() && (out.back() == L' ' || out.back() == L'\n')) out.pop_back();
    out += L'…';
    return out;
}

std::wstring Trim(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return std::wstring(s.substr(b, e - b));
}

} // namespace nb
