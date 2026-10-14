// RAWM ES21 Pro: the official RAWM HUB supports this model, but no documented or independently
// verified battery protocol was found (see docs/HARDWARE.md). This provider therefore only
// recognises the device by its HID product string and reports "battery unknown" honestly.
// It never sends any report to the device: probing unknown HID packets is deliberately avoided.
#include "pch.h"

#include <hidsdi.h>
#include <setupapi.h>

#include "core/Log.h"
#include "core/Strings.h"
#include "core/Worker.h"
#include "sys/battery/BatteryService.h"

namespace nb::sys {
namespace {

class RawmProvider : public BatteryProvider {
public:
    const char* Id() const override { return "rawm"; }
    void Start() override { WorkerPool::Background().Submit([this] { Scan(); }, "rawm-scan"); }
    void Stop() override {}
    void RequestRefresh() override {
        int64_t now = UnixNowMs();
        if (now - last_ < 60000) return;
        last_ = now;
        WorkerPool::Background().Submit([this] { Scan(); }, "rawm-scan");
    }
    ProviderInfo Info() const override { return {"rawm", L"RAWM ES21 Pro", status_, false}; }

private:
    void Scan() {
        GUID hid;
        HidD_GetHidGuid(&hid);
        HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        bool found = false;
        std::wstring product;
        if (set != INVALID_HANDLE_VALUE) {
            SP_DEVICE_INTERFACE_DATA ifd{sizeof ifd};
            for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &ifd); ++i) {
                DWORD need = 0;
                SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
                std::vector<uint8_t> buf(need);
                auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
                det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) continue;
                // Open without read/write access: only attribute/string queries, no I/O.
                HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (h == INVALID_HANDLE_VALUE) continue;
                wchar_t name[128] = {};
                if (HidD_GetProductString(h, name, sizeof name)) {
                    std::wstring n = ToLowerLinguistic(name);
                    if (n.find(L"rawm") != std::wstring::npos || n.find(L"es21") != std::wstring::npos) {
                        found = true;
                        product = name;
                    }
                }
                CloseHandle(h);
            }
            SetupDiDestroyDeviceInfoList(set);
        }
        std::vector<battery::DeviceBattery> out;
        if (found) {
            battery::DeviceBattery d;
            d.id = "rawm:es21";
            d.name = product.empty() ? L"RAWM ES21 Pro" : product;
            d.kind = battery::DeviceKind::Mouse;
            d.connection = battery::Connection::Unknown;
            d.connected = true;
            d.method = "none";
            d.note = L"Doğrulanmış pil okuma yöntemi yok (RAWM HUB'da görüntülenir)";
            out.push_back(d);
            status_ = L"Cihaz bulundu, pil okunamıyor";
        } else {
            status_ = L"Cihaz bulunamadı; pil yöntemi doğrulanmadı";
        }
        BatteryService::Get().Report(Id(), std::move(out));
    }
    std::atomic<int64_t> last_{0};
    std::wstring status_ = L"Taranıyor";
};
} // namespace

std::unique_ptr<BatteryProvider> MakeRawmProvider() { return std::make_unique<RawmProvider>(); }

} // namespace nb::sys
