#pragma once
#include <string>
#include <string_view>

namespace nb {

std::string ToUtf8(std::wstring_view w);
std::wstring FromUtf8(std::string_view s);

// Locale-aware, case/diacritic-insensitive "contains" (Turkish İ/ı handled by the NLS linguistic flags).
// Returns match position or -1. Implemented with Win32 NLS in the app; tests use the same code.
int FindInsensitive(std::wstring_view haystack, std::wstring_view needle);
std::wstring ToLowerLinguistic(std::wstring_view s);

// Shortens text to maxChars, adding an ellipsis. Avoids splitting surrogate pairs.
std::wstring Ellipsize(std::wstring_view s, size_t maxChars);
std::wstring Trim(std::wstring_view s);

} // namespace nb
