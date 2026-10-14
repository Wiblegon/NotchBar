// Battery levels Windows itself knows for paired devices: the PnP device property
// {104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2 (the value shown in Settings > Bluetooth & devices,
// filled e.g. from HFP battery indicators on Bluetooth Classic headsets or by Windows' own
// HID-over-GATT battery handling). Read through a DeviceWatcher: Added/Updated events, no polling.
#include "pch.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include "core/Log.h"
#include "core/Strings.h"
#include "sys/battery/BatteryService.h"

namespace nb::sys {
namespace {
using namespace winrt;
using namespace winrt::Windows::Devices::Enumeration;
using winrt::Windows::Foundation::IInspectable;

const wchar_t* kBatteryKey = L"{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2";

std::optional<int> ReadLevel(IInspectable const& v) {
    if (!v) return std::nullopt;
    if (auto b = v.try_as<winrt::Windows::Foundation::IReference<uint8_t>>()) return static_cast<int>(b.Value());
    if (auto u = v.try_as<winrt::Windows::Foundation::IReference<uint32_t>>()) return static_cast<int>(u.Value());
    if (auto i = v.try_as<winrt::Windows::Foundation::IReference<int32_t>>()) return static_cast<int>(i.Value());
    return std::nullopt;
}

battery::DeviceKind GuessKind(const std::wstring& name, const std::wstring& category) {
    std::wstring n = ToLowerLinguistic(name + L" " + category);
    if (n.find(L"mouse") != std::wstring::npos || n.find(L"fare") != std::wstring::npos) return battery::DeviceKind::Mouse;
    if (n.find(L"keyboard") != std::wstring::npos || n.find(L"klavye") != std::wstring::npos) return battery::DeviceKind::Keyboard;
    if (n.find(L"buds") != std::wstring::npos || n.find(L"pods") != std::wstring::npos || n.find(L"earbud") != std::wstring::npos)
        return battery::DeviceKind::Earbuds;
    if (n.find(L"headset") != std::wstring::npos) return battery::DeviceKind::Headset;
    if (n.find(L"headphone") != std::wstring::npos || n.find(L"audio") != std::wstring::npos) return battery::DeviceKind::Headphones;
    if (n.find(L"controller") != std::wstring::npos || n.find(L"gamepad") != std::wstring::npos) return battery::DeviceKind::Gamepad;
    if (n.find(L"phone") != std::wstring::npos) return battery::DeviceKind::Phone;
    return battery::DeviceKind::Other;
}

class PnpProvider : public BatteryProvider {
public:
    const char* Id() const override { return "pnp"; }

    void Start() override {
        auto props = single_threaded_vector<hstring>({hstring(kBatteryKey), L"System.Devices.ContainerId"});
        // Only devnodes that carry the battery property; fall back to all devices if the AQS
        // form is not accepted on this Windows build.
        std::wstring aqs = std::wstring(L"\"") + kBatteryKey + L"\":<>[]";
        try {
            watcher_ = DeviceInformation::CreateWatcher(aqs, props, DeviceInformationKind::Device);
        } catch (...) {
            watcher_ = DeviceInformation::CreateWatcher(L"", props, DeviceInformationKind::Device);
        }
        added_ = watcher_.Added([this](auto&&, DeviceInformation const& di) { Upsert(di.Id().c_str(), di.Properties()); });
        updated_ = watcher_.Updated([this](auto&&, DeviceInformationUpdate const& u) { Upsert(u.Id().c_str(), u.Properties()); });
        removed_ = watcher_.Removed([this](auto&&, DeviceInformationUpdate const& u) {
            {
                std::lock_guard lk(mu_);
                devices_.erase(u.Id().c_str());
            }
            Publish();
        });
        completed_ = watcher_.EnumerationCompleted([this](auto&&, auto&&) {
            status_ = devicesCount() ? L"Etkin" : L"Pil bildiren cihaz yok";
            Publish();
        });
        watcher_.Start();
        status_ = L"Taranıyor";
    }

    void Stop() override {
        if (!watcher_) return;
        try {
            watcher_.Added(added_);
            watcher_.Updated(updated_);
            watcher_.Removed(removed_);
            watcher_.EnumerationCompleted(completed_);
            if (watcher_.Status() == DeviceWatcherStatus::Started || watcher_.Status() == DeviceWatcherStatus::EnumerationCompleted)
                watcher_.Stop();
        } catch (...) {
        }
        watcher_ = nullptr;
    }

    void RequestRefresh() override {
        // Connection state lives on the device container; re-query it (cheap, async).
        int64_t now = UnixNowMs();
        if (now - lastRefresh_ < 20000) return;
        lastRefresh_ = now;
        std::vector<std::wstring> ids;
        {
            std::lock_guard lk(mu_);
            for (auto& [id, d] : devices_) ids.push_back(id);
        }
        for (auto& id : ids) ResolveContainer(id);
    }

    ProviderInfo Info() const override { return {"pnp", L"Windows cihaz özellikleri", status_, false}; }

private:
    size_t devicesCount() {
        std::lock_guard lk(mu_);
        return devices_.size();
    }

    void Upsert(const std::wstring& id, winrt::Windows::Foundation::Collections::IMapView<hstring, IInspectable> const& props) {
        std::optional<int> level;
        std::wstring container;
        if (props.HasKey(kBatteryKey)) level = ReadLevel(props.Lookup(kBatteryKey));
        if (props.HasKey(L"System.Devices.ContainerId")) {
            if (auto g = props.Lookup(L"System.Devices.ContainerId").try_as<winrt::Windows::Foundation::IReference<guid>>()) {
                wchar_t buf[64];
                StringFromGUID2(g.Value(), buf, 64);
                container = buf;
            }
        }
        bool isNew = false;
        {
            std::lock_guard lk(mu_);
            auto it = devices_.find(id);
            if (!level && it == devices_.end()) return; // not a battery device
            auto& d = devices_[id];
            isNew = d.id.empty();
            if (isNew) {
                d.id = "pnp:" + ToUtf8(container.empty() ? id : container);
                d.method = "pnp-property";
                d.connection = id.rfind(L"BTHLE", 0) == 0 ? battery::Connection::BluetoothLE
                               : id.rfind(L"BTHENUM", 0) == 0 ? battery::Connection::BluetoothClassic
                                                               : battery::Connection::Unknown;
                d.containerId = container;
                d.name = L"Cihaz";
                d.connected = true;
            }
            if (level && *level >= 0 && *level <= 100) {
                d.percent = *level;
                d.updatedMs = UnixNowMs();
            }
        }
        if (isNew) ResolveContainer(id);
        Publish();
    }

    // Container: friendly name of the physical device + whether it is connected right now.
    void ResolveContainer(const std::wstring& id) {
        std::wstring container;
        {
            std::lock_guard lk(mu_);
            auto it = devices_.find(id);
            if (it == devices_.end()) return;
            container = it->second.containerId;
        }
        if (container.empty()) return;
        [](PnpProvider* self, std::wstring id, std::wstring container) -> fire_and_forget {
            try {
                co_await winrt::resume_background();
                auto props = single_threaded_vector<hstring>({L"System.Devices.Connected", L"System.Devices.Category"});
                auto info = co_await DeviceInformation::CreateFromIdAsync(container, props, DeviceInformationKind::DeviceContainer);
                bool connected = true;
                if (info.Properties().HasKey(L"System.Devices.Connected"))
                    connected = unbox_value_or<bool>(info.Properties().Lookup(L"System.Devices.Connected"), true);
                std::wstring category;
                if (info.Properties().HasKey(L"System.Devices.Category")) {
                    if (auto arr = info.Properties().Lookup(L"System.Devices.Category").try_as<winrt::Windows::Foundation::IReferenceArray<hstring>>()) {
                        com_array<hstring> vals;
                        arr.GetStringArray(vals);
                        for (auto& v : vals) category += std::wstring(v.c_str()) + L" ";
                    }
                }
                {
                    std::lock_guard lk(self->mu_);
                    auto it = self->devices_.find(id);
                    if (it == self->devices_.end()) co_return;
                    it->second.name = info.Name().c_str();
                    it->second.connected = connected;
                    it->second.kind = GuessKind(it->second.name, category);
                }
                self->Publish();
            } catch (...) {
            }
        }(this, id, container);
    }

    void Publish() {
        std::vector<battery::DeviceBattery> out;
        {
            std::lock_guard lk(mu_);
            for (auto& [id, d] : devices_) out.push_back(d);
        }
        if (!out.empty()) status_ = L"Etkin (" + std::to_wstring(out.size()) + L" cihaz)";
        BatteryService::Get().Report(Id(), std::move(out));
    }

    DeviceWatcher watcher_{nullptr};
    event_token added_{}, updated_{}, removed_{}, completed_{};
    std::mutex mu_;
    std::map<std::wstring, battery::DeviceBattery> devices_;
    std::wstring status_ = L"Başlatılmadı";
    int64_t lastRefresh_ = 0;
};
} // namespace

std::unique_ptr<BatteryProvider> MakePnpProvider() { return std::make_unique<PnpProvider>(); }

} // namespace nb::sys
