#include "pch.h"

#include "sys/WifiList.h"

#include <wlanapi.h>

namespace nb::sys {
namespace {
struct Session {
    HANDLE h = nullptr;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    Session() {
        DWORD ver = 0;
        if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) h = nullptr;
        else if (WlanEnumInterfaces(h, nullptr, &list) != ERROR_SUCCESS) list = nullptr;
    }
    ~Session() {
        if (list) WlanFreeMemory(list);
        if (h) WlanCloseHandle(h, nullptr);
    }
    const GUID* Guid() const { return list && list->dwNumberOfItems ? &list->InterfaceInfo[0].InterfaceGuid : nullptr; }
};

std::wstring Utf8(const DOT11_SSID& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(s.ucSSID), static_cast<int>(s.uSSIDLength), nullptr, 0);
    std::wstring r(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(s.ucSSID), static_cast<int>(s.uSSIDLength), r.data(), n);
    return r;
}
} // namespace

WifiList ListWifi() {
    WifiList out;
    Session s;
    if (!s.Guid()) return out;
    out.hasAdapter = true;
    PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
    if (WlanGetAvailableNetworkList(s.h, s.Guid(), 0, nullptr, &list) != ERROR_SUCCESS || !list) return out;
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const auto& n = list->Network[i];
        WifiNet w;
        w.ssid = Utf8(n.dot11Ssid);
        if (w.ssid.empty()) continue;
        w.quality = static_cast<int>(n.wlanSignalQuality);
        w.secured = n.bSecurityEnabled != FALSE;
        w.connected = (n.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
        w.known = (n.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) != 0;
        w.profile = n.strProfileName;
        auto same = std::find_if(out.nets.begin(), out.nets.end(), [&](const WifiNet& x) { return x.ssid == w.ssid; });
        if (same == out.nets.end()) out.nets.push_back(std::move(w));
        else if (w.connected || w.quality > same->quality) {
            bool keepKnown = same->known || w.known;
            std::wstring profile = w.profile.empty() ? same->profile : w.profile;
            *same = std::move(w);
            same->known = keepKnown;
            same->profile = profile;
        }
    }
    WlanFreeMemory(list);
    std::sort(out.nets.begin(), out.nets.end(), [](const WifiNet& a, const WifiNet& b) {
        if (a.connected != b.connected) return a.connected;
        return a.quality > b.quality;
    });
    return out;
}

void ScanWifi() {
    Session s;
    if (s.Guid()) WlanScan(s.h, s.Guid(), nullptr, nullptr, nullptr);
}

bool ConnectWifi(const WifiNet& net) {
    if (!net.known || net.profile.empty()) return false;
    Session s;
    if (!s.Guid()) return false;
    WLAN_CONNECTION_PARAMETERS p{};
    p.wlanConnectionMode = wlan_connection_mode_profile;
    p.strProfile = net.profile.c_str();
    p.dot11BssType = dot11_BSS_type_any;
    return WlanConnect(s.h, s.Guid(), &p, nullptr) == ERROR_SUCCESS;
}

void DisconnectWifi() {
    Session s;
    if (s.Guid()) WlanDisconnect(s.h, s.Guid(), nullptr);
}

} // namespace nb::sys
