#include "model/Search.h"

#include <windows.h>

namespace nb::search {

std::wstring Fold(const std::wstring& s) {
    if (s.empty()) return {};
    // Decompose so that diacritics become separate combining marks we can drop.
    int n = NormalizeString(NormalizationD, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring d;
    if (n > 0) {
        d.resize(static_cast<size_t>(n));
        n = NormalizeString(NormalizationD, s.c_str(), static_cast<int>(s.size()), d.data(), n);
        d.resize(n > 0 ? static_cast<size_t>(n) : 0);
    }
    if (d.empty()) d = s;
    std::wstring out;
    out.reserve(d.size());
    for (wchar_t c : d) {
        if (c >= 0x0300 && c <= 0x036F) continue; // combining marks
        if (c == L'ı' || c == L'İ') c = L'i'; // dotless/dotted I
        out += c;
    }
    // Invariant lowercasing: deterministic regardless of user locale (Turkish I handled above).
    std::wstring lower(out.size(), L'\0');
    int m = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, out.c_str(), static_cast<int>(out.size()), lower.data(),
                          static_cast<int>(lower.size()), nullptr, nullptr, 0);
    if (m > 0) lower.resize(static_cast<size_t>(m));
    else lower = out;
    return lower;
}

int RankName(const std::wstring& name, const std::wstring& q) {
    if (q.empty() || name.empty()) return 0;
    if (name == q) return 1000;
    if (name.compare(0, q.size(), q) == 0) return 800 - static_cast<int>(std::min<size_t>(name.size(), 100));
    // word-prefix
    for (size_t i = 1; i < name.size(); ++i) {
        wchar_t prev = name[i - 1];
        bool boundary = prev == L' ' || prev == L'-' || prev == L'_' || prev == L'.' || prev == L'(';
        if (boundary && name.compare(i, q.size(), q) == 0) return 600 - static_cast<int>(i);
    }
    auto pos = name.find(q);
    if (pos != std::wstring::npos) return 400 - static_cast<int>(std::min<size_t>(pos, 100));
    // initials ("vsc" -> "visual studio code")
    if (q.size() >= 2) {
        std::wstring initials;
        bool start = true;
        for (wchar_t c : name) {
            if (c == L' ' || c == L'-' || c == L'_') { start = true; continue; }
            if (start) initials += c;
            start = false;
        }
        if (initials.compare(0, q.size(), q) == 0) return 300;
    }
    return 0;
}

} // namespace nb::search
