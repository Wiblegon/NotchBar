// Standard Bluetooth LE Battery Service (Bluetooth SIG BAS 1.1):
//   service        0x180F  (GattServiceUuids::Battery)
//   characteristic 0x2A19  Battery Level, uint8 0..100 (GattCharacteristicUuids::BatteryLevel)
// Notifications are used when the characteristic supports them; otherwise the value is read
// on connect, when the battery menu opens (max once a minute) and every 15 minutes otherwise.
// Only *connected, paired* LE devices are touched. Every WinRT call has a timeout.
#include "pch.h"

#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include "core/Log.h"
#include "core/Strings.h"
#include "core/Worker.h"
#include "model/Battery.h"
#include "sys/battery/BatteryService.h"

namespace nb::sys {
namespace {
using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Devices::Enumeration;
using winrt::Windows::Foundation::AsyncStatus;

template <class Op>
bool Wait(Op const& op, int ms) {
    if (op.wait_for(std::chrono::milliseconds(ms)) == AsyncStatus::Completed) return true;
    try {
        op.Cancel();
    } catch (...) {
    }
    return false;
}

struct Session {
    std::wstring deviceId;
    battery::DeviceBattery rec;
    BluetoothLEDevice device{nullptr};
    GattCharacteristic level{nullptr};
    event_token valueToken{};
    bool notifying = false;
    int64_t lastRead = 0;
    int failures = 0;
};

class BleProvider : public BatteryProvider {
public:
    const char* Id() const override { return "gatt-bas"; }

    void Start() override {
        // Paired LE devices, with live connection state.
        auto props = single_threaded_vector<hstring>({L"System.Devices.Aep.IsConnected", L"System.Devices.Aep.ContainerId"});
        watcher_ = DeviceInformation::CreateWatcher(BluetoothLEDevice::GetDeviceSelectorFromPairingState(true), props,
                                                    DeviceInformationKind::AssociationEndpoint);
        added_ = watcher_.Added([this](auto&&, DeviceInformation const& di) { OnSeen(di.Id().c_str(), di.Name().c_str(), di.Properties()); });
        updated_ = watcher_.Updated([this](auto&&, DeviceInformationUpdate const& u) { OnSeen(u.Id().c_str(), L"", u.Properties()); });
        removed_ = watcher_.Removed([this](auto&&, DeviceInformationUpdate const& u) { Drop(u.Id().c_str()); });
        watcher_.Start();
        status_ = L"Bağlı LE cihaz bekleniyor";
    }

    void Stop() override {
        stopped_ = true;
        try {
            if (watcher_) {
                watcher_.Added(added_);
                watcher_.Updated(updated_);
                watcher_.Removed(removed_);
                watcher_.Stop();
            }
        } catch (...) {
        }
        watcher_ = nullptr;
        std::lock_guard lk(mu_);
        for (auto& [id, s] : sessions_) Close(s);
        sessions_.clear();
    }

    void SetPaused(bool p) override {
        paused_ = p;
        if (!p) RequestRefresh();
    }

    void RequestRefresh() override {
        if (paused_) return;
        std::vector<std::wstring> ids;
        {
            std::lock_guard lk(mu_);
            for (auto& [id, s] : sessions_)
                if (s.rec.connected && !s.notifying && UnixNowMs() - s.lastRead > 60000) ids.push_back(id);
        }
        for (auto& id : ids) Schedule(id);
    }

    ProviderInfo Info() const override { return {"gatt-bas", L"Bluetooth LE pil hizmeti", status_, false}; }

private:
    void OnSeen(const std::wstring& id, const std::wstring& name,
                winrt::Windows::Foundation::Collections::IMapView<hstring, winrt::Windows::Foundation::IInspectable> const& props) {
        std::optional<bool> connected;
        if (props.HasKey(L"System.Devices.Aep.IsConnected")) connected = unbox_value_or<bool>(props.Lookup(L"System.Devices.Aep.IsConnected"), false);
        std::wstring container;
        if (props.HasKey(L"System.Devices.Aep.ContainerId"))
            if (auto g = props.Lookup(L"System.Devices.Aep.ContainerId").try_as<winrt::Windows::Foundation::IReference<guid>>()) {
                wchar_t buf[64];
                StringFromGUID2(g.Value(), buf, 64);
                container = buf;
            }
        bool connectNow = false;
        {
            std::lock_guard lk(mu_);
            auto& s = sessions_[id];
            if (s.deviceId.empty()) {
                s.deviceId = id;
                s.rec.id = "gatt:" + ToUtf8(id);
                s.rec.connection = battery::Connection::BluetoothLE;
                s.rec.method = "gatt-bas";
                s.rec.kind = battery::DeviceKind::Other;
            }
            if (!name.empty()) s.rec.name = name;
            if (!container.empty()) s.rec.containerId = container;
            if (connected) {
                bool was = s.rec.connected;
                s.rec.connected = *connected;
                if (*connected && !was) connectNow = true;
                if (!*connected && was) Close(s); // stop any activity for disconnected devices
            }
        }
        if (connectNow) Schedule(id);
        Publish();
    }

    void Drop(const std::wstring& id) {
        {
            std::lock_guard lk(mu_);
            auto it = sessions_.find(id);
            if (it == sessions_.end()) return;
            Close(it->second);
            sessions_.erase(it);
        }
        Publish();
    }

    void Close(Session& s) {
        try {
            if (s.level && s.notifying) s.level.ValueChanged(s.valueToken);
        } catch (...) {
        }
        s.level = nullptr;
        s.notifying = false;
        if (s.device) s.device.Close();
        s.device = nullptr;
    }

    void Schedule(const std::wstring& id) {
        if (paused_ || stopped_) return;
        WorkerPool::Background().Submit([this, id] { ReadOnce(id); }, "ble:" + ToUtf8(id));
    }

    // Worker thread (MTA). Bounded by timeouts; a failing device never affects others.
    void ReadOnce(const std::wstring& id) {
        if (paused_ || stopped_) return;
        try {
            BluetoothLEDevice dev{nullptr};
            GattCharacteristic ch{nullptr};
            {
                std::lock_guard lk(mu_);
                auto it = sessions_.find(id);
                if (it == sessions_.end() || !it->second.rec.connected) return;
                dev = it->second.device;
                ch = it->second.level;
            }
            if (!ch) {
                auto op = BluetoothLEDevice::FromIdAsync(id);
                if (!Wait(op, 5000) || !op.GetResults()) return Fail(id, L"Cihaz açılamadı");
                dev = op.GetResults();
                auto sop = dev.GetGattServicesForUuidAsync(GattServiceUuids::Battery(), BluetoothCacheMode::Cached);
                if (!Wait(sop, 5000)) return Fail(id, L"Zaman aşımı");
                auto sres = sop.GetResults();
                if (sres.Status() == GattCommunicationStatus::AccessDenied)
                    return Fail(id, L"Windows bu cihazın pil hizmetine erişime izin vermiyor");
                if (sres.Status() != GattCommunicationStatus::Success || sres.Services().Size() == 0)
                    return Fail(id, L"Standart pil hizmeti (0x180F) yok");
                auto svc = sres.Services().GetAt(0);
                auto cop = svc.GetCharacteristicsForUuidAsync(GattCharacteristicUuids::BatteryLevel(), BluetoothCacheMode::Cached);
                if (!Wait(cop, 5000)) return Fail(id, L"Zaman aşımı");
                auto cres = cop.GetResults();
                if (cres.Status() != GattCommunicationStatus::Success || cres.Characteristics().Size() == 0)
                    return Fail(id, L"Pil seviyesi özelliği (0x2A19) yok");
                ch = cres.Characteristics().GetAt(0);
                bool canNotify = (ch.CharacteristicProperties() & GattCharacteristicProperties::Notify) == GattCharacteristicProperties::Notify;
                event_token tok{};
                bool notifying = false;
                if (canNotify) {
                    tok = ch.ValueChanged([this, id](GattCharacteristic const&, GattValueChangedEventArgs const& a) {
                        auto buf = a.CharacteristicValue();
                        std::vector<uint8_t> bytes(buf.Length());
                        if (!bytes.empty()) memcpy(bytes.data(), buf.data(), bytes.size());
                        Store(id, battery::ParseBasBatteryLevel(bytes.data(), bytes.size()));
                    });
                    auto wop = ch.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue::Notify);
                    notifying = Wait(wop, 5000) && wop.GetResults() == GattCommunicationStatus::Success;
                    if (!notifying) ch.ValueChanged(tok);
                }
                std::lock_guard lk(mu_);
                auto it = sessions_.find(id);
                if (it == sessions_.end()) return;
                it->second.device = dev;
                it->second.level = ch;
                it->second.notifying = notifying;
                it->second.valueToken = tok;
            }
            auto rop = ch.ReadValueAsync(BluetoothCacheMode::Uncached);
            if (!Wait(rop, 5000)) return Fail(id, L"Okuma zaman aşımı");
            auto r = rop.GetResults();
            if (r.Status() != GattCommunicationStatus::Success) return Fail(id, L"Okunamadı");
            auto buf = r.Value();
            std::vector<uint8_t> bytes(buf.Length());
            if (!bytes.empty()) memcpy(bytes.data(), buf.data(), bytes.size());
            Store(id, battery::ParseBasBatteryLevel(bytes.data(), bytes.size()));
        } catch (...) {
            Fail(id, L"Okuma hatası");
        }
    }

    void Store(const std::wstring& id, std::optional<int> level) {
        {
            std::lock_guard lk(mu_);
            auto it = sessions_.find(id);
            if (it == sessions_.end()) return;
            it->second.lastRead = UnixNowMs();
            it->second.failures = 0;
            it->second.rec.note.clear();
            if (level) {
                it->second.rec.percent = level;
                it->second.rec.updatedMs = UnixNowMs();
            }
        }
        status_ = L"Etkin";
        Publish();
    }

    void Fail(const std::wstring& id, const wchar_t* why) {
        {
            std::lock_guard lk(mu_);
            auto it = sessions_.find(id);
            if (it == sessions_.end()) return;
            it->second.lastRead = UnixNowMs(); // back off; failures are retried only on later requests
            ++it->second.failures;
            it->second.rec.note = why;
        }
        Publish();
    }

    void Publish() {
        std::vector<battery::DeviceBattery> out;
        {
            std::lock_guard lk(mu_);
            for (auto& [id, s] : sessions_)
                // Only devices where the battery service answered (or is being tried while connected).
                if (s.rec.percent || (s.rec.connected && s.failures == 0 && s.lastRead == 0)) {
                    if (!s.rec.percent && s.lastRead == 0) continue; // still probing: do not list yet
                    out.push_back(s.rec);
                }
        }
        BatteryService::Get().Report(Id(), std::move(out));
    }

    DeviceWatcher watcher_{nullptr};
    event_token added_{}, updated_{}, removed_{};
    std::mutex mu_;
    std::map<std::wstring, Session> sessions_;
    std::atomic<bool> paused_{false}, stopped_{false};
    std::wstring status_ = L"Başlatılmadı";
};
} // namespace

std::unique_ptr<BatteryProvider> MakeBleProvider() { return std::make_unique<BleProvider>(); }

} // namespace nb::sys
