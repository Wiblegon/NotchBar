#pragma once
// "Cihaz ekle": the user picks a value a device shares with Windows (a PnP device property between
// 0 and 100, e.g. the battery level) and NotchBar shows it as a battery entry from then on.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nb::sys {

struct PnpDevice {
    std::wstring name;        // friendly name of the physical device
    std::wstring className;   // "Bluetooth", "HIDClass", ...
    std::wstring containerId; // groups the device nodes of one physical device
};

struct PnpValue { // one numeric property (0..100) shared by a device node
    std::wstring label;
    std::wstring ownerInstanceId;
    std::wstring ownerName;
    std::wstring guid; // "{...}"
    uint32_t pid = 0;
    int value = 0;
    bool knownBattery = false; // Windows' own battery property
};

// Present devices that look like peripherals (Bluetooth, HID, USB audio, ...), one entry per physical device.
std::vector<PnpDevice> ListPeripherals();
// Every numeric 0..100 property of the device nodes of that physical device (known battery key first).
std::vector<PnpValue> ListSharedValues(const std::wstring& containerId);
// Reads one property of one device node; nullopt when the device is gone or the value is out of range.
std::optional<int> ReadSharedValue(const std::wstring& instanceId, const std::wstring& guid, uint32_t pid);

} // namespace nb::sys
