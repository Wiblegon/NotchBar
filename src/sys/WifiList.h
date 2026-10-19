#pragma once
// Wi-Fi networks in range, and connecting to the ones Windows already has a profile for.
#include <string>
#include <vector>

namespace nb::sys {

struct WifiNet {
    std::wstring ssid, profile;
    int quality = 0; // 0..100
    bool secured = false, connected = false, known = false;
};

struct WifiList {
    bool hasAdapter = false;
    std::vector<WifiNet> nets; // strongest first, connected one on top
};

WifiList ListWifi();
void ScanWifi();                         // asks the adapter for a fresh scan (results arrive a few seconds later)
bool ConnectWifi(const WifiNet& net);    // only for known networks; false when it cannot be done silently
void DisconnectWifi();

} // namespace nb::sys
