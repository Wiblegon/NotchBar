#include "core/Store.h"

#include "core/AtomicFile.h"
#include "core/Dispatcher.h"
#include "core/Log.h"
#include "core/Paths.h"

namespace nb {

Store& Store::Get() {
    static Store s;
    return s;
}

void Store::Load() {
    auto text = file::Read(paths::SettingsFile());
    auto r = ParseSettings(text ? &*text : nullptr);
    settings_ = r.settings;
    switch (r.status) {
    case LoadStatus::Ok: LOGI("settings loaded"); break;
    case LoadStatus::Missing: LOGI("settings missing, using defaults"); break;
    case LoadStatus::Migrated:
        LOGI("settings migrated to v%d", Settings::kVersion);
        settingsDirty_ = true;
        break;
    case LoadStatus::Corrupt:
        LOGW("settings corrupt, moved aside: %s", r.warnings.empty() ? "" : r.warnings[0].c_str());
        file::Quarantine(paths::SettingsFile());
        settingsDirty_ = true;
        break;
    }

    auto fav = file::Read(paths::FavoritesFile());
    if (fav) {
        auto v = json::Parse(*fav);
        if (v && v->isObject()) {
            std::vector<std::string> warn;
            favorites_ = Favorites::FromJson(*v, &warn);
            if (!warn.empty()) LOGW("favorites: %zu entries skipped", warn.size());
        } else {
            LOGW("favorites file corrupt, moved aside");
            file::Quarantine(paths::FavoritesFile());
        }
    }
    if (settingsDirty_) FlushNow();
}

void Store::SaveSettings() {
    settingsDirty_ = true;
    if (!timer_) timer_ = ui::SetTimeout(300, [this] { timer_ = 0; FlushNow(); });
}

void Store::SaveFavorites() {
    favoritesDirty_ = true;
    if (!timer_) timer_ = ui::SetTimeout(300, [this] { timer_ = 0; FlushNow(); });
}

void Store::FlushNow() {
    if (timer_) ui::ClearTimeout(timer_);
    if (settingsDirty_) {
        if (!file::WriteAtomic(paths::SettingsFile(), json::Serialize(settings_.ToJson()))) LOGE("settings write failed");
        settingsDirty_ = false;
    }
    if (favoritesDirty_) {
        if (!file::WriteAtomic(paths::FavoritesFile(), json::Serialize(favorites_.ToJson()))) LOGE("favorites write failed");
        favoritesDirty_ = false;
    }
}

void Store::NotifyChanged() {
    for (auto& o : observers_) o();
}

} // namespace nb
