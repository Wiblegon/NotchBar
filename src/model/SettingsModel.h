#pragma once
// User settings: small, versioned, with safe defaults. Favorites live in a separate file so a
// broken settings file can never take the user's favorites/folders with it.
#include <string>
#include <vector>

#include "core/Json.h"

namespace nb {

// A battery value the user picked from the properties a device shares with Windows ("Cihaz ekle").
struct CustomBatterySource {
    std::string id;         // stable id, "c<number>"
    std::string name;       // shown in the battery box
    std::string instanceId; // PnP device instance that owns the property (UTF-8)
    std::string guid;       // property key: "{xxxxxxxx-....}"
    int pid = 0;            // property key: pid
};

struct Settings {
    static constexpr int kVersion = 2;

    bool autostart = true;            // mirrored from the scheduled task; offered on first run
    bool hideTaskbar = true;
    double uiScale = 1.0;             // 0.9 / 1.0 / 1.15
    bool reduceMotion = false;        // in addition to the Windows "show animations" preference
    bool showToasts = true;           // transient boxes under the notch
    bool windowsNotifications = false; // read Windows notifications (asks permission when enabled)
    bool notificationPermissionAsked = false;
    std::string searchEngine = "google";
    bool captureWinKey = true;
    bool deviceBattery = true;
    bool firstRunDone = false;
    std::vector<CustomBatterySource> customBattery;
    bool maximizedClockOnly = true;   // maximized window: the top shows a small clock pill instead of a thin handle

    // Tunable interaction timings (ms).
    int panelOpenDelayMs = 180;
    int panelCloseDelayMs = 450;
    int edgeExpandDelayMs = 150;
    int edgeCollapseDelayMs = 700;

    json::Value ToJson() const;
    static Settings FromJson(const json::Value& v, std::vector<std::string>* warnings = nullptr);
};

enum class LoadStatus { Ok, Missing, Corrupt, Migrated };

struct SettingsLoad {
    Settings settings;
    LoadStatus status = LoadStatus::Ok;
    std::vector<std::string> warnings;
};

// Parses file content. Never throws; corrupt input yields defaults with status Corrupt.
SettingsLoad ParseSettings(const std::string* fileContent);

} // namespace nb
