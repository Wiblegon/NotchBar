#pragma once
// Favorites and virtual app folders. Folders are in-app groupings only: they never touch the
// file system and deleting one never uninstalls or deletes an app.
// Data model is versioned; folders are single-level in v1 but stored as a generic item list so
// nested folders can be added later without a format break.
#include <optional>
#include <string>
#include <vector>

#include "core/Json.h"

namespace nb {

struct AppRef {
    std::wstring id;   // AppsFolder parsing name (AUMID for packaged apps, path-based id for desktop apps)
    std::wstring name; // display name cached for offline rendering
    bool operator==(const AppRef& o) const { return id == o.id; }
};

struct FavItem {
    enum class Kind { App, Folder };
    Kind kind = Kind::App;
    AppRef app;                  // Kind::App
    std::wstring folderId;       // Kind::Folder
    std::wstring folderName;     // Kind::Folder
    std::vector<AppRef> children; // Kind::Folder
};

class Favorites {
public:
    static constexpr int kVersion = 1;

    const std::vector<FavItem>& Items() const { return items_; }

    bool Contains(const std::wstring& appId) const;
    // Location of an app: folder id (empty for root) and index.
    std::optional<std::pair<std::wstring, size_t>> Locate(const std::wstring& appId) const;

    bool AddApp(const AppRef& app);
    bool RemoveApp(const std::wstring& appId);
    bool MoveRoot(size_t from, size_t to);
    bool MoveInFolder(const std::wstring& folderId, size_t from, size_t to);

    std::wstring CreateFolder(const std::wstring& name, const std::vector<std::wstring>& appIds = {});
    bool RenameFolder(const std::wstring& folderId, const std::wstring& name);
    bool MoveAppToFolder(const std::wstring& appId, const std::wstring& folderId);
    bool MoveAppToRoot(const std::wstring& appId);
    // Children go back to the root list at the folder's position. Nothing is uninstalled.
    bool DeleteFolder(const std::wstring& folderId);

    const FavItem* FindFolder(const std::wstring& folderId) const;
    void UpdateNames(const std::vector<AppRef>& catalog);

    json::Value ToJson() const;
    // Tolerant loader: skips malformed entries and duplicates, reports what was dropped.
    static Favorites FromJson(const json::Value& v, std::vector<std::string>* warnings = nullptr);

private:
    FavItem* FindFolderMut(const std::wstring& folderId);
    std::optional<AppRef> Detach(const std::wstring& appId);
    std::wstring NewFolderId();

    std::vector<FavItem> items_;
    int nextFolder_ = 1;
};

} // namespace nb
