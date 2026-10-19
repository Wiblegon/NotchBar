#include "pch.h"

#include "sys/QuickSettings.h"

#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include "core/Dispatcher.h"
#include "core/Log.h"

namespace nb::sys::quick {
namespace {
using namespace winrt;
using namespace winrt::Windows::Devices::Radios;

constexpr wchar_t kNocKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings";
constexpr wchar_t kNocValue[] = L"NOC_GLOBAL_SETTING_TOASTS_ENABLED";

// Reads the radios on a background thread, applies `change` (kind -> desired state, or none) and
// reports the final state.
struct Change {
    bool bluetooth = false, wifi = false;      // which radios to touch
    bool bluetoothOn = false, wifiOn = false;  // desired states
};

fire_and_forget Run(Change change, std::function<void(Radios)> done) {
    Radios out;
    try {
        co_await resume_background();
        auto access = co_await Radio::RequestAccessAsync();
        if (access == RadioAccessStatus::Allowed) {
            auto list = co_await Radio::GetRadiosAsync();
            for (auto const& r : list) {
                bool bt = r.Kind() == RadioKind::Bluetooth;
                bool wf = r.Kind() == RadioKind::WiFi;
                if (!bt && !wf) continue;
                if ((bt && change.bluetooth) || (wf && change.wifi)) {
                    bool want = bt ? change.bluetoothOn : change.wifiOn;
                    co_await r.SetStateAsync(want ? RadioState::On : RadioState::Off);
                }
                bool on = r.State() == RadioState::On;
                RadioInfo& ri = bt ? out.bluetooth : out.wifi;
                ri.present = true;
                ri.on = ri.on || on;
            }
            out.known = true;
        }
    } catch (...) {
        LOGW("quick settings: radio access failed");
    }
    DWORD air = 0, n = sizeof air, type = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\RadioManagement", L"SystemRadioState",
                     RRF_RT_REG_DWORD, &type, &air, &n) == ERROR_SUCCESS) {
        out.airplaneKnown = true;
        out.airplaneReg = air != 0;
    }
    ui::Post([out, done = std::move(done)] {
        if (done) done(out);
    });
}
} // namespace

void ReadRadios(std::function<void(Radios)> done) { Run({}, std::move(done)); }

void SetBluetooth(bool on, std::function<void()> done) {
    Change c;
    c.bluetooth = true;
    c.bluetoothOn = on;
    Run(c, [done = std::move(done)](Radios) {
        if (done) done();
    });
}

void SetWifi(bool on, std::function<void()> done) {
    Change c;
    c.wifi = true;
    c.wifiOn = on;
    Run(c, [done = std::move(done)](Radios) {
        if (done) done();
    });
}

void SetAirplane(bool on, std::function<void()> done) {
    Change c;
    c.bluetooth = c.wifi = true;
    c.bluetoothOn = c.wifiOn = !on;
    Run(c, [done = std::move(done)](Radios) {
        if (done) done();
    });
}

bool DoNotDisturb() {
    DWORD v = 1, n = sizeof v, type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kNocKey, kNocValue, RRF_RT_REG_DWORD, &type, &v, &n) != ERROR_SUCCESS) return false;
    return v == 0;
}

void SetDoNotDisturb(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kNocKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    DWORD v = on ? 0 : 1;
    RegSetValueExW(k, kNocValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
    RegCloseKey(k);
}

} // namespace nb::sys::quick
