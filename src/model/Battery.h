#pragma once
// Battery data model shared by all providers. "Unknown" is never represented as 0%.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nb::battery {

enum class DeviceKind { Computer, Mouse, Keyboard, Headset, Headphones, Earbuds, Gamepad, Phone, Other };
enum class Connection { Internal, Usb, Wireless24GHz, BluetoothClassic, BluetoothLE, Unknown };
enum class Charge { Unknown, Discharging, Charging, Full };

struct Part { // separately reported components (left/right earbud, case)
    std::wstring label;
    std::optional<int> percent;
    Charge charge = Charge::Unknown;
};

struct DeviceBattery {
    std::string id;           // stable: provider prefix + hardware identity
    std::wstring name;
    DeviceKind kind = DeviceKind::Other;
    Connection connection = Connection::Unknown;
    std::optional<int> percent; // nullopt = not reported; 0 = really empty
    Charge charge = Charge::Unknown;
    bool connected = false;
    int64_t updatedMs = 0;      // unix ms of the last successful reading, 0 = never
    std::string method;         // e.g. "system-power", "pnp-property", "gatt-bas", "hyperx-hid"
    std::wstring note;          // honest status text (e.g. "Doğrulanmış okuma yöntemi yok")
    bool verifiedOnHardware = false;
    std::vector<Part> parts;
    std::wstring containerId;   // for de-duplication between providers

    bool IsStale(int64_t nowMs, int64_t maxAgeMs) const { return updatedMs == 0 || nowMs - updatedMs > maxAgeMs; }
};

std::wstring FormatPercent(const std::optional<int>& p); // "—" for unknown, "0%" for real zero
const wchar_t* KindName(DeviceKind k);
const wchar_t* ConnectionName(Connection c);
const wchar_t* ChargeName(Charge c);

// ---- Pure parsers (unit-tested) ----

// Win32 SYSTEM_POWER_STATUS fields -> computer battery. Returns nullopt when no system battery.
std::optional<DeviceBattery> FromSystemPower(uint8_t acLineStatus, uint8_t batteryFlag, uint8_t lifePercent);

// Bluetooth SIG Battery Service (0x180F) Battery Level characteristic (0x2A19): uint8 0..100.
std::optional<int> ParseBasBatteryLevel(const uint8_t* data, size_t len);

// HyperX Cloud III S Wireless (03F0:06BE / 03F0:02CC) vendor protocol, read-only commands.
namespace hyperx {
constexpr uint8_t kReportId = 0x0C;
constexpr uint8_t kNotifyId = 0x0D;
constexpr uint8_t kCmdConnected = 0x02;
constexpr uint8_t kCmdBattery = 0x06;
constexpr uint8_t kCmdCharge = 0x48;
std::vector<uint8_t> BuildQuery(uint8_t command, size_t reportLength = 64);
struct Reading {
    std::optional<int> percent;
    std::optional<Charge> charge;
    std::optional<bool> headsetConnected;
};
// Returns an empty Reading for unrelated or "no value" (0xFF) reports.
Reading ParseReport(const uint8_t* data, size_t len);
} // namespace hyperx

} // namespace nb::battery
