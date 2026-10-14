#include "pch.h"

#include "sys/AppCatalog.h"

#include "core/AtomicFile.h"
#include "core/Dispatcher.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "core/Worker.h"
#include "model/Search.h"

namespace nb::sys {
namespace {

std::vector<AppEntry> Enumerate() {
    std::vector<AppEntry> out;
    winrt::com_ptr<IShellItem> folder;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(folder.put())))) return out;
    winrt::com_ptr<IEnumShellItems> items;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(items.put())))) return out;
    for (;;) {
        winrt::com_ptr<IShellItem> it;
        ULONG got = 0;
        if (items->Next(1, it.put(), &got) != S_OK || !got) break;
        AppEntry e;
        PWSTR s = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &s))) e.name = s;
        CoTaskMemFree(s);
        s = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &s))) e.id = s;
        CoTaskMemFree(s);
        if (e.id.empty() || e.name.empty()) continue;
        // Skip uninstallers and documentation links that Start lists as "apps".
        std::wstring f = search::Fold(e.name);
        if (f.find(L"uninstall") != std::wstring::npos || f.find(L"kaldir") == 0) continue;
        e.packaged = e.id.find(L'!') != std::wstring::npos;
        if (auto ps = winrt::com_ptr<IShellItem>(it).try_as<IShellItem2>()) {
            PWSTR target = nullptr;
            if (SUCCEEDED(ps->GetString(PKEY_Link_TargetParsingPath, &target)) && target) {
                e.target = target;
                std::transform(e.target.begin(), e.target.end(), e.target.begin(), ::towlower);
            }
            CoTaskMemFree(target);
        }
        if (e.target.empty() && !e.packaged && e.id.size() > 4 && _wcsicmp(e.id.c_str() + e.id.size() - 4, L".exe") == 0) {
            // ids like "{6D809377-...}\App\app.exe" -> resolve the known folder for window matching
            if (e.id[0] == L'{') {
                auto close = e.id.find(L'}');
                GUID g{};
                if (close != std::wstring::npos && SUCCEEDED(CLSIDFromString(e.id.substr(0, close + 1).c_str(), &g))) {
                    PWSTR base = nullptr;
                    if (SUCCEEDED(SHGetKnownFolderPath(g, 0, nullptr, &base))) {
                        e.target = std::wstring(base) + e.id.substr(close + 1);
                        std::transform(e.target.begin(), e.target.end(), e.target.begin(), ::towlower);
                    }
                    CoTaskMemFree(base);
                }
            } else {
                e.target = e.id;
                std::transform(e.target.begin(), e.target.end(), e.target.begin(), ::towlower);
            }
        }
        e.folded = search::Fold(e.name);
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, a.name.c_str(), -1, b.name.c_str(), -1, nullptr,
                               nullptr, 0) == CSTR_LESS_THAN;
    });
    // AppsFolder can list the same app twice (e.g. per-user and machine shortcuts).
    out.erase(std::unique(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b) { return a.id == b.id; }), out.end());
    return out;
}
} // namespace

AppCatalog& AppCatalog::Get() {
    static AppCatalog c;
    return c;
}

void AppCatalog::LoadCache() {
    auto text = file::Read(paths::AppCacheFile());
    if (!text) return;
    auto v = json::Parse(*text);
    if (!v || (*v)["version"].asInt() != 1) return;
    std::vector<AppEntry> apps;
    for (const auto& a : (*v)["apps"].asArray()) {
        AppEntry e;
        e.id = FromUtf8(a["id"].asString());
        e.name = FromUtf8(a["name"].asString());
        e.target = FromUtf8(a["target"].asString());
        e.packaged = a["packaged"].asBool();
        if (e.id.empty() || e.name.empty()) continue;
        e.folded = search::Fold(e.name);
        apps.push_back(std::move(e));
    }
    LOGI("app cache: %zu entries", apps.size());
    Apply(std::move(apps), false);
}

void AppCatalog::Refresh() {
    ULONGLONG now = GetTickCount64();
    if (refreshing_ || (lastRefresh_ && now - lastRefresh_ < 5 * 60 * 1000)) return;
    refreshing_ = true;
    lastRefresh_ = now;
    WorkerPool::Shell().Submit(
        [this] {
            auto t0 = GetTickCount64();
            auto apps = Enumerate();
            LOGI("apps enumerated: %zu in %llu ms", apps.size(), GetTickCount64() - t0);
            ui::Post([this, apps = std::move(apps)]() mutable {
                refreshing_ = false;
                if (!apps.empty()) Apply(std::move(apps), true);
            });
        },
        "apps-enum");
}

void AppCatalog::Apply(std::vector<AppEntry> apps, bool save) {
    apps_ = std::move(apps);
    if (save) {
        json::Array arr;
        for (const auto& e : apps_) {
            json::Object o;
            o["id"] = ToUtf8(e.id);
            o["name"] = ToUtf8(e.name);
            if (!e.target.empty()) o["target"] = ToUtf8(e.target);
            if (e.packaged) o["packaged"] = true;
            arr.push_back(json::Value(std::move(o)));
        }
        json::Object root;
        root["version"] = 1;
        root["apps"] = json::Value(std::move(arr));
        file::WriteAtomic(paths::AppCacheFile(), json::Serialize(json::Value(std::move(root)), false));
    }
    for (auto& l : listeners_) l();
}

const AppEntry* AppCatalog::Find(const std::wstring& id) const {
    for (const auto& e : apps_)
        if (e.id == id) return &e;
    return nullptr;
}

const AppEntry* AppCatalog::FindByTarget(const std::wstring& exePath) const {
    if (exePath.empty()) return nullptr;
    std::wstring p = exePath;
    std::transform(p.begin(), p.end(), p.begin(), ::towlower);
    for (const auto& e : apps_)
        if (e.target == p) return &e;
    return nullptr;
}

std::vector<AppRef> AppCatalog::Refs() const {
    std::vector<AppRef> r;
    r.reserve(apps_.size());
    for (const auto& e : apps_) r.push_back({e.id, e.name});
    return r;
}

bool AppCatalog::Launch(const std::wstring& id) {
    // shell:AppsFolder\<id> launches both packaged and desktop apps through the shell,
    // exactly like Start does. The id comes from the shell's own enumeration, never from typing.
    std::wstring target = L"shell:AppsFolder\\" + id;
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";
    sei.lpFile = target.c_str();
    sei.nShow = SW_SHOWNORMAL;
    bool ok = ShellExecuteExW(&sei) != FALSE;
    if (!ok) LOGW("launch failed (%lu)", GetLastError());
    return ok;
}

} // namespace nb::sys
