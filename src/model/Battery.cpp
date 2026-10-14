#include "model/Battery.h"

namespace nb::battery {

std::wstring FormatPercent(const std::optional<int>& p) {
    if (!p) return L"—"; // em dash: unknown
    return std::to_wstring(*p) + L"%";
}

const wchar_t* KindName(DeviceKind k) {
    switch (k) {
    case DeviceKind::Computer: return L"Bilgisayar";
    case DeviceKind::Mouse: return L"Fare";
    case DeviceKind::Keyboard: return L"Klavye";
    case DeviceKind::Headset: return L"Oyuncu kulaklığı";
    case DeviceKind::Headphones: return L"Kulaklık";
    case DeviceKind::Earbuds: return L"Kulak içi kulaklık";
    case DeviceKind::Gamepad: return L"Oyun kumandası";
    case DeviceKind::Phone: return L"Telefon";
    default: return L"Cihaz";
    }
}

const wchar_t* ConnectionName(Connection c) {
    switch (c) {
    case Connection::Internal: return L"Dahili";
    case Connection::Usb: return L"USB";
    case Connection::Wireless24GHz: return L"2,4 GHz alıcı";
    case Connection::BluetoothClassic: return L"Bluetooth";
    case Connection::BluetoothLE: return L"Bluetooth LE";
    default: return L"Bilinmiyor";
    }
}

const wchar_t* ChargeName(Charge c) {
    switch (c) {
    case Charge::Discharging: return L"Pilde";
    case Charge::Charging: return L"Şarj oluyor";
    case Charge::Full: return L"Dolu";
    default: return L"";
    }
}

std::optional<DeviceBattery> FromSystemPower(uint8_t ac, uint8_t flag, uint8_t life) {
    // BatteryFlag: 128 = no system battery, 255 = unknown status.
    if (flag == 128) return std::nullopt;
    DeviceBattery d;
    d.id = "system";
    d.name = L"Bu bilgisayar";
    d.kind = DeviceKind::Computer;
    d.connection = Connection::Internal;
    d.connected = true;
    d.method = "system-power";
    d.verifiedOnHardware = true;
    if (life <= 100) d.percent = life; // 255 = unknown
    if (flag != 255 && (flag & 8)) d.charge = Charge::Charging;
    else if (ac == 1 && d.percent && *d.percent >= 100) d.charge = Charge::Full;
    else if (ac == 0) d.charge = Charge::Discharging;
    else if (ac == 1) d.charge = Charge::Full; // plugged in, not charging (e.g. charge limit)
    return d;
}

std::optional<int> ParseBasBatteryLevel(const uint8_t* data, size_t len) {
    if (!data || len < 1) return std::nullopt;
    if (data[0] > 100) return std::nullopt; // values above 100 are reserved by the spec
    return static_cast<int>(data[0]);
}

namespace hyperx {

std::vector<uint8_t> BuildQuery(uint8_t command, size_t reportLength) {
    std::vector<uint8_t> p(reportLength < 6 ? 64 : reportLength, 0);
    p[0] = kReportId;
    p[1] = 0x02;
    p[2] = 0x03;
    p[3] = 0x01; // 0x01 = read; write commands (0x00) are never sent by this app
    p[4] = 0x00;
    p[5] = command;
    return p;
}

Reading ParseReport(const uint8_t* r, size_t len) {
    Reading out;
    if (!r || len < 7) return out;
    if (r[0] == kReportId) {
        uint8_t cmd = r[5], val = r[6];
        if (val == 0xFF) return out;
        if (cmd == kCmdBattery && val <= 100) out.percent = val;
        else if (cmd == kCmdCharge) {
            if (val == 0) out.charge = Charge::Discharging;
            else if (val == 1) out.charge = Charge::Charging;
            else if (val == 2) out.charge = Charge::Full;
        } else if (cmd == kCmdConnected) out.headsetConnected = (val == 2);
    } else if (r[0] == kNotifyId) {
        uint8_t type = r[4], val = r[5];
        if (type == 1 && val <= 100) out.percent = val;
        else if (type == 10) {
            if (val == 0) out.charge = Charge::Discharging;
            else if (val == 1) out.charge = Charge::Charging;
            else if (val == 2) out.charge = Charge::Full;
        } else if (type == 12) out.headsetConnected = (val == 1);
    }
    return out;
}

} // namespace hyperx
} // namespace nb::battery
