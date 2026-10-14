#include "model/WebSearch.h"

#include "core/Strings.h"

namespace nb::web {

const std::vector<Engine>& Engines() {
    static const std::vector<Engine> kEngines = {
        {"google", L"Google", L"https://www.google.com/search?q={q}"},
        {"bing", L"Bing", L"https://www.bing.com/search?q={q}"},
        {"duckduckgo", L"DuckDuckGo", L"https://duckduckgo.com/?q={q}"},
        {"brave", L"Brave Search", L"https://search.brave.com/search?q={q}"},
        {"yandex", L"Yandex", L"https://yandex.com.tr/search/?text={q}"},
        {"ecosia", L"Ecosia", L"https://www.ecosia.org/search?q={q}"},
    };
    return kEngines;
}

const Engine& FindEngine(const std::string& id) {
    for (const auto& e : Engines())
        if (id == e.id) return e;
    return Engines().front();
}

std::wstring PercentEncode(const std::wstring& text) {
    static const wchar_t* hex = L"0123456789ABCDEF";
    std::string utf8 = ToUtf8(text);
    std::wstring out;
    out.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                          c == '.' || c == '_' || c == '~';
        if (unreserved) out += static_cast<wchar_t>(c);
        else {
            out += L'%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::wstring BuildSearchUrl(const std::string& engineId, const std::wstring& query) {
    std::wstring url = FindEngine(engineId).urlTemplate;
    auto pos = url.find(L"{q}");
    url.replace(pos, 3, PercentEncode(Trim(query)));
    return url;
}

} // namespace nb::web
