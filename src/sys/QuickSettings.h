#pragma once
// Quick toggles: Bluetooth / Wi-Fi radios (Windows.Devices.Radios), airplane mode (all radios) and
// "do not disturb". Reads and changes are asynchronous; callbacks run on the UI thread.
#include <functional>

namespace nb::sys::quick {

struct RadioInfo {
    bool present = false;
    bool on = false;
};

struct Radios {
    RadioInfo bluetooth, wifi;
    bool known = false; // a read has completed
    bool airplaneKnown = false, airplaneReg = false; // Windows' own airplane-mode flag
    bool Airplane() const {
        if (airplaneKnown && airplaneReg) return true;
        // Our own "airplane" toggle switches the radios off without Windows' flag: all off counts too.
        return bluetooth.present && wifi.present && !bluetooth.on && !wifi.on;
    }
};

void ReadRadios(std::function<void(Radios)> done);
void SetBluetooth(bool on, std::function<void()> done);
void SetWifi(bool on, std::function<void()> done);
void SetAirplane(bool on, std::function<void()> done); // on = both radios off

bool DoNotDisturb();
void SetDoNotDisturb(bool on);

} // namespace nb::sys::quick
