#pragma once
// Owns persisted user data. Settings and favorites are separate files so that a damaged
// settings file never costs the user their favorites/folders.
#include <functional>
#include <vector>

#include "model/Favorites.h"
#include "model/SettingsModel.h"

namespace nb {

class Store {
public:
    static Store& Get();

    void Load();
    Settings& settings() { return settings_; }
    Favorites& favorites() { return favorites_; }

    // Writes are coalesced (short delay) and atomic.
    void SaveSettings();
    void SaveFavorites();
    void FlushNow();

    // Observers (UI) are told after any mutation done through the helpers below.
    void Subscribe(std::function<void()> fn) { observers_.push_back(std::move(fn)); }
    void NotifyChanged();

private:
    Settings settings_;
    Favorites favorites_;
    bool settingsDirty_ = false, favoritesDirty_ = false;
    uint64_t timer_ = 0;
    std::vector<std::function<void()>> observers_;
};

} // namespace nb
