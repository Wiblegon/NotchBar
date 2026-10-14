#include "model/SettingsModel.h"

#include <algorithm>

namespace nb {

json::Value Settings::ToJson() const {
    json::Object o;
    o["version"] = kVersion;
    o["autostart"] = autostart;
    o["hideTaskbar"] = hideTaskbar;
    o["uiScale"] = uiScale;
    o["reduceMotion"] = reduceMotion;
    o["showToasts"] = showToasts;
    o["windowsNotifications"] = windowsNotifications;
    o["notificationPermissionAsked"] = notificationPermissionAsked;
    o["searchEngine"] = searchEngine;
    o["captureWinKey"] = captureWinKey;
    o["deviceBattery"] = deviceBattery;
    o["firstRunDone"] = firstRunDone;
    json::Object t;
    t["panelOpenDelayMs"] = panelOpenDelayMs;
    t["panelCloseDelayMs"] = panelCloseDelayMs;
    t["edgeExpandDelayMs"] = edgeExpandDelayMs;
    t["edgeCollapseDelayMs"] = edgeCollapseDelayMs;
    o["timings"] = json::Value(std::move(t));
    return json::Value(std::move(o));
}

Settings Settings::FromJson(const json::Value& v, std::vector<std::string>* warnings) {
    Settings s;
    auto b = [&](const char* k, bool& out) {
        if (v[k].isBool()) out = v[k].asBool();
        else if (v.has(k) && warnings) warnings->push_back(std::string("bad type for ") + k);
    };
    auto clampInt = [&](const json::Value& j, int& out, int lo, int hi) {
        if (j.isNumber()) out = std::clamp(j.asInt(), lo, hi);
    };
    b("autostart", s.autostart);
    b("hideTaskbar", s.hideTaskbar);
    b("reduceMotion", s.reduceMotion);
    b("showToasts", s.showToasts);
    b("windowsNotifications", s.windowsNotifications);
    b("notificationPermissionAsked", s.notificationPermissionAsked);
    b("captureWinKey", s.captureWinKey);
    b("deviceBattery", s.deviceBattery);
    b("firstRunDone", s.firstRunDone);
    if (v["uiScale"].isNumber()) s.uiScale = std::clamp(v["uiScale"].asNumber(), 0.8, 1.4);
    if (v["searchEngine"].isString()) s.searchEngine = v["searchEngine"].asString().substr(0, 32);

    const auto& t = v["timings"];
    clampInt(t["panelOpenDelayMs"], s.panelOpenDelayMs, 0, 2000);
    clampInt(t["panelCloseDelayMs"], s.panelCloseDelayMs, 50, 5000);
    clampInt(t["edgeExpandDelayMs"], s.edgeExpandDelayMs, 0, 2000);
    clampInt(t["edgeCollapseDelayMs"], s.edgeCollapseDelayMs, 50, 5000);
    return s;
}

SettingsLoad ParseSettings(const std::string* content) {
    SettingsLoad r;
    if (!content) {
        r.status = LoadStatus::Missing;
        return r;
    }
    json::ParseError err;
    auto v = json::Parse(*content, &err);
    if (!v || !v->isObject()) {
        r.status = LoadStatus::Corrupt;
        r.warnings.push_back("settings unreadable at offset " + std::to_string(err.offset) + ": " + err.message);
        return r;
    }
    int version = (*v)["version"].asInt(0);
    json::Value data = *v;
    if (version < 2) {
        // v1 stored the panel delays at the top level.
        json::Object t;
        if (data["panelOpenDelayMs"].isNumber()) t["panelOpenDelayMs"] = data["panelOpenDelayMs"];
        if (data["panelCloseDelayMs"].isNumber()) t["panelCloseDelayMs"] = data["panelCloseDelayMs"];
        if (!data.has("timings")) data.set("timings", json::Value(std::move(t)));
        r.status = LoadStatus::Migrated;
    }
    r.settings = Settings::FromJson(data, &r.warnings);
    return r;
}

} // namespace nb
