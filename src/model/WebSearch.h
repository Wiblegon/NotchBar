#pragma once
#include <string>
#include <vector>

namespace nb::web {

struct Engine {
    const char* id;
    const wchar_t* name;
    const wchar_t* urlTemplate; // "{q}" is replaced by the percent-encoded UTF-8 query
};

const std::vector<Engine>& Engines();
const Engine& FindEngine(const std::string& id); // falls back to the first engine

// RFC 3986 percent-encoding of the UTF-8 bytes (unreserved characters kept).
std::wstring PercentEncode(const std::wstring& text);
// Always returns an https:// URL built from a fixed template; never a command line.
std::wstring BuildSearchUrl(const std::string& engineId, const std::wstring& query);

} // namespace nb::web
