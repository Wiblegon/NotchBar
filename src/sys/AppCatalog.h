#pragma once
// Launchable applications = the shell's "AppsFolder" (Start menu shortcuts + packaged apps),
// which is what Start itself offers. This is deliberately different from "installed programs"
// (Apps & features / uninstall entries), many of which have nothing to launch.
// Enumeration runs on the shell worker; a small cache file makes the next start instant.
#include <functional>
#include <string>
#include <vector>

#include "model/Favorites.h"

namespace nb::sys {

struct AppEntry {
    std::wstring id;      // AppsFolder parsing name: AUMID or "{KNOWNFOLDER}\path\app.exe"-style id
    std::wstring name;
    std::wstring target;  // lower-case target path for desktop apps (for mapping windows to apps)
    std::wstring folded;  // search key
    bool packaged = false;
    std::wstring IconKey() const { return L"app:" + id; }
};

class AppCatalog {
public:
    static AppCatalog& Get();
    void LoadCache();       // fast, at startup
    void Refresh();         // background re-enumeration (rate-limited)
    bool Ready() const { return !apps_.empty(); }
    const std::vector<AppEntry>& Apps() const { return apps_; }
    const AppEntry* Find(const std::wstring& id) const;
    const AppEntry* FindByTarget(const std::wstring& exePath) const;
    std::vector<AppRef> Refs() const;
    void Subscribe(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }

    static bool Launch(const std::wstring& id);

private:
    void Apply(std::vector<AppEntry> apps, bool save);
    std::vector<AppEntry> apps_;
    std::vector<std::function<void()>> listeners_;
    unsigned long long lastRefresh_ = 0;
    bool refreshing_ = false;
};

} // namespace nb::sys
