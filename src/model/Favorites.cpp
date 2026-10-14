#include "model/Favorites.h"

#include <algorithm>
#include <set>

#include "core/Strings.h"

namespace nb {

bool Favorites::Contains(const std::wstring& appId) const { return Locate(appId).has_value(); }

std::optional<std::pair<std::wstring, size_t>> Favorites::Locate(const std::wstring& appId) const {
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& it = items_[i];
        if (it.kind == FavItem::Kind::App && it.app.id == appId) return std::make_pair(std::wstring(), i);
        if (it.kind == FavItem::Kind::Folder)
            for (size_t k = 0; k < it.children.size(); ++k)
                if (it.children[k].id == appId) return std::make_pair(it.folderId, k);
    }
    return std::nullopt;
}

bool Favorites::AddApp(const AppRef& app) {
    if (app.id.empty() || Contains(app.id)) return false;
    FavItem f;
    f.kind = FavItem::Kind::App;
    f.app = app;
    items_.push_back(std::move(f));
    return true;
}

std::optional<AppRef> Favorites::Detach(const std::wstring& appId) {
    for (size_t i = 0; i < items_.size(); ++i) {
        auto& it = items_[i];
        if (it.kind == FavItem::Kind::App && it.app.id == appId) {
            AppRef a = it.app;
            items_.erase(items_.begin() + static_cast<ptrdiff_t>(i));
            return a;
        }
        if (it.kind == FavItem::Kind::Folder) {
            auto c = std::find_if(it.children.begin(), it.children.end(), [&](const AppRef& r) { return r.id == appId; });
            if (c != it.children.end()) {
                AppRef a = *c;
                it.children.erase(c);
                return a;
            }
        }
    }
    return std::nullopt;
}

bool Favorites::RemoveApp(const std::wstring& appId) { return Detach(appId).has_value(); }

bool Favorites::MoveRoot(size_t from, size_t to) {
    if (from >= items_.size() || to >= items_.size()) return false;
    if (from == to) return true;
    FavItem tmp = std::move(items_[from]);
    items_.erase(items_.begin() + static_cast<ptrdiff_t>(from));
    items_.insert(items_.begin() + static_cast<ptrdiff_t>(to), std::move(tmp));
    return true;
}

bool Favorites::MoveInFolder(const std::wstring& folderId, size_t from, size_t to) {
    FavItem* f = FindFolderMut(folderId);
    if (!f || from >= f->children.size() || to >= f->children.size()) return false;
    AppRef tmp = f->children[from];
    f->children.erase(f->children.begin() + static_cast<ptrdiff_t>(from));
    f->children.insert(f->children.begin() + static_cast<ptrdiff_t>(to), tmp);
    return true;
}

std::wstring Favorites::NewFolderId() {
    for (;;) {
        std::wstring id = L"folder-" + std::to_wstring(nextFolder_++);
        if (!FindFolder(id)) return id;
    }
}

std::wstring Favorites::CreateFolder(const std::wstring& name, const std::vector<std::wstring>& appIds) {
    FavItem f;
    f.kind = FavItem::Kind::Folder;
    f.folderId = NewFolderId();
    std::wstring trimmed = Trim(name);
    f.folderName = trimmed.empty() ? L"Yeni klasör" : trimmed;
    // Insert where the first moved app was, so creating a folder from two tiles feels in place.
    size_t insertAt = items_.size();
    for (const auto& id : appIds) {
        auto loc = Locate(id);
        if (loc && loc->first.empty()) insertAt = std::min(insertAt, loc->second);
    }
    for (const auto& id : appIds) {
        auto loc = Locate(id);
        if (loc && loc->first.empty() && loc->second < insertAt) --insertAt;
        if (auto a = Detach(id)) f.children.push_back(*a);
    }
    insertAt = std::min(insertAt, items_.size());
    std::wstring id = f.folderId;
    items_.insert(items_.begin() + static_cast<ptrdiff_t>(insertAt), std::move(f));
    return id;
}

bool Favorites::RenameFolder(const std::wstring& folderId, const std::wstring& name) {
    FavItem* f = FindFolderMut(folderId);
    std::wstring trimmed = Trim(name);
    if (!f || trimmed.empty()) return false;
    f->folderName = trimmed.substr(0, 64);
    return true;
}

bool Favorites::MoveAppToFolder(const std::wstring& appId, const std::wstring& folderId) {
    if (!FindFolder(folderId)) return false;
    auto loc = Locate(appId);
    if (loc && loc->first == folderId) return true;
    auto a = Detach(appId);
    if (!a) return false;
    FindFolderMut(folderId)->children.push_back(*a);
    return true;
}

bool Favorites::MoveAppToRoot(const std::wstring& appId) {
    auto loc = Locate(appId);
    if (!loc) return false;
    if (loc->first.empty()) return true;
    auto a = Detach(appId);
    FavItem f;
    f.kind = FavItem::Kind::App;
    f.app = *a;
    items_.push_back(std::move(f));
    return true;
}

bool Favorites::DeleteFolder(const std::wstring& folderId) {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].kind == FavItem::Kind::Folder && items_[i].folderId == folderId) {
            std::vector<AppRef> kids = std::move(items_[i].children);
            items_.erase(items_.begin() + static_cast<ptrdiff_t>(i));
            size_t at = i;
            for (auto& k : kids) {
                FavItem f;
                f.kind = FavItem::Kind::App;
                f.app = std::move(k);
                items_.insert(items_.begin() + static_cast<ptrdiff_t>(at++), std::move(f));
            }
            return true;
        }
    }
    return false;
}

const FavItem* Favorites::FindFolder(const std::wstring& folderId) const {
    for (const auto& it : items_)
        if (it.kind == FavItem::Kind::Folder && it.folderId == folderId) return &it;
    return nullptr;
}

FavItem* Favorites::FindFolderMut(const std::wstring& folderId) {
    return const_cast<FavItem*>(static_cast<const Favorites*>(this)->FindFolder(folderId));
}

void Favorites::UpdateNames(const std::vector<AppRef>& catalog) {
    auto fix = [&](AppRef& r) {
        for (const auto& c : catalog)
            if (c.id == r.id && !c.name.empty()) { r.name = c.name; return; }
    };
    for (auto& it : items_) {
        if (it.kind == FavItem::Kind::App) fix(it.app);
        else for (auto& c : it.children) fix(c);
    }
}

namespace {
json::Value AppToJson(const AppRef& a) {
    json::Object o;
    o["type"] = "app";
    o["id"] = ToUtf8(a.id);
    o["name"] = ToUtf8(a.name);
    return json::Value(std::move(o));
}
std::optional<AppRef> AppFromJson(const json::Value& v) {
    if (!v.isObject() || v["type"].asString() != "app") return std::nullopt;
    AppRef a{FromUtf8(v["id"].asString()), FromUtf8(v["name"].asString())};
    if (a.id.empty() || a.id.size() > 1024) return std::nullopt;
    return a;
}
} // namespace

json::Value Favorites::ToJson() const {
    json::Array arr;
    for (const auto& it : items_) {
        if (it.kind == FavItem::Kind::App) arr.push_back(AppToJson(it.app));
        else {
            json::Object o;
            o["type"] = "folder";
            o["id"] = ToUtf8(it.folderId);
            o["name"] = ToUtf8(it.folderName);
            json::Array kids;
            for (const auto& c : it.children) kids.push_back(AppToJson(c));
            o["items"] = json::Value(std::move(kids));
            arr.push_back(json::Value(std::move(o)));
        }
    }
    json::Object root;
    root["version"] = kVersion;
    root["items"] = json::Value(std::move(arr));
    return json::Value(std::move(root));
}

Favorites Favorites::FromJson(const json::Value& v, std::vector<std::string>* warnings) {
    Favorites f;
    auto warn = [&](const std::string& w) {
        if (warnings) warnings->push_back(w);
    };
    int version = v["version"].asInt(0);
    if (version > kVersion) warn("favorites file is newer than this build; unknown fields ignored");
    std::set<std::wstring> seen, folderIds;
    for (const auto& item : v["items"].asArray()) {
        std::string type = item["type"].asString();
        if (type == "app") {
            auto a = AppFromJson(item);
            if (!a || seen.count(a->id)) { warn("dropped invalid/duplicate app"); continue; }
            seen.insert(a->id);
            FavItem fi;
            fi.app = *a;
            f.items_.push_back(std::move(fi));
        } else if (type == "folder") {
            FavItem fi;
            fi.kind = FavItem::Kind::Folder;
            fi.folderId = FromUtf8(item["id"].asString());
            fi.folderName = FromUtf8(item["name"].asString());
            if (fi.folderId.empty() || folderIds.count(fi.folderId)) fi.folderId = L"folder-import-" + std::to_wstring(f.items_.size());
            if (fi.folderName.empty()) fi.folderName = L"Klasör";
            folderIds.insert(fi.folderId);
            for (const auto& c : item["items"].asArray()) {
                // v1 folders are single-level: nested folders are flattened into this folder.
                if (c["type"].asString() == "folder") {
                    warn("nested folder flattened");
                    for (const auto& cc : c["items"].asArray())
                        if (auto a = AppFromJson(cc); a && !seen.count(a->id)) { seen.insert(a->id); fi.children.push_back(*a); }
                    continue;
                }
                auto a = AppFromJson(c);
                if (!a || seen.count(a->id)) { warn("dropped invalid/duplicate app in folder"); continue; }
                seen.insert(a->id);
                fi.children.push_back(*a);
            }
            f.items_.push_back(std::move(fi));
        } else {
            warn("unknown item type skipped");
        }
    }
    // Continue folder numbering after the highest existing id.
    for (const auto& id : folderIds) {
        if (id.rfind(L"folder-", 0) == 0) {
            int n = _wtoi(id.c_str() + 7);
            if (n >= f.nextFolder_) f.nextFolder_ = n + 1;
        }
    }
    return f;
}

} // namespace nb
