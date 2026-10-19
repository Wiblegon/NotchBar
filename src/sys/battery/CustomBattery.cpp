#include "pch.h"

#include <cfgmgr32.h>
#include <setupapi.h>

#include <map>
#include <set>

#include "core/Dispatcher.h"
#include "core/Log.h"
#include "core/Store.h"
#include "core/Strings.h"
#include "sys/battery/BatteryService.h"
#include "sys/battery/CustomBattery.h"

namespace nb::sys {
namespace {

// DEVPKEY_Device_ContainerId {8C7ED206-3F8A-4827-B3AB-AE9E1FAEFC6C}, 2 (spelled out: no initguid/devpkey.h here)
const DEVPROPKEY kContainerKey = {{0x8C7ED206, 0x3F8A, 0x4827, {0xB3, 0xAB, 0xAE, 0x9E, 0x1F, 0xAE, 0xFC, 0x6C}}, 2};

// {104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2: the battery level Windows shows in Settings > Bluetooth & devices.
const GUID kBatteryGuid = {0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}};

std::wstring GuidToString(const GUID& g) {
    wchar_t buf[64];
    StringFromGUID2(g, buf, 64);
    return buf;
}

bool GuidFromString(const std::wstring& s, GUID* g) { return SUCCEEDED(IIDFromString(s.c_str(), g)); }

std::wstring RegString(HDEVINFO set, SP_DEVINFO_DATA& d, DWORD prop) {
    wchar_t buf[512];
    DWORD type = 0, need = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, &d, prop, &type, reinterpret_cast<BYTE*>(buf), sizeof buf - 2, &need)) return {};
    buf[ARRAYSIZE(buf) - 1] = 0;
    return buf;
}

std::wstring InstanceId(HDEVINFO set, SP_DEVINFO_DATA& d) {
    wchar_t buf[MAX_DEVICE_ID_LEN];
    if (!SetupDiGetDeviceInstanceIdW(set, &d, buf, ARRAYSIZE(buf), nullptr)) return {};
    return buf;
}

std::wstring ContainerOf(HDEVINFO set, SP_DEVINFO_DATA& d) {
    GUID id{};
    DEVPROPTYPE type = 0;
    DWORD need = 0;
    if (!SetupDiGetDevicePropertyW(set, &d, &kContainerKey, &type, reinterpret_cast<PBYTE>(&id), sizeof id, &need, 0) ||
        type != DEVPROP_TYPE_GUID)
        return {};
    return GuidToString(id);
}

std::optional<int> AsPercent(DEVPROPTYPE type, const BYTE* buf, DWORD size) {
    long long v = 0;
    switch (type) {
    case DEVPROP_TYPE_BYTE: if (size >= 1) v = buf[0]; else return std::nullopt; break;
    case DEVPROP_TYPE_SBYTE: if (size >= 1) v = static_cast<int8_t>(buf[0]); else return std::nullopt; break;
    case DEVPROP_TYPE_INT16: if (size >= 2) v = *reinterpret_cast<const int16_t*>(buf); else return std::nullopt; break;
    case DEVPROP_TYPE_UINT16: if (size >= 2) v = *reinterpret_cast<const uint16_t*>(buf); else return std::nullopt; break;
    case DEVPROP_TYPE_INT32: if (size >= 4) v = *reinterpret_cast<const int32_t*>(buf); else return std::nullopt; break;
    case DEVPROP_TYPE_UINT32: if (size >= 4) v = *reinterpret_cast<const uint32_t*>(buf); else return std::nullopt; break;
    default: return std::nullopt;
    }
    if (v < 0 || v > 100) return std::nullopt;
    return static_cast<int>(v);
}

bool IsPeripheralClass(const std::wstring& cls) {
    static const wchar_t* kClasses[] = {L"Bluetooth", L"HIDClass", L"Mouse", L"Keyboard", L"MEDIA", L"USB", L"Battery", L"WPD",
                                        L"Image", L"Camera", L"XboxComposite", L"SoftwareComponent", L"AudioEndpoint"};
    for (auto* c : kClasses)
        if (_wcsicmp(cls.c_str(), c) == 0) return true;
    return false;
}
} // namespace

std::vector<PnpDevice> ListPeripherals() {
    std::vector<PnpDevice> out;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;
    std::set<std::wstring> seen;
    SP_DEVINFO_DATA d{sizeof d};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        std::wstring cls = RegString(set, d, SPDRP_CLASS);
        if (!IsPeripheralClass(cls)) continue;
        std::wstring name = RegString(set, d, SPDRP_FRIENDLYNAME);
        if (name.empty()) name = RegString(set, d, SPDRP_DEVICEDESC);
        std::wstring container = ContainerOf(set, d);
        if (name.empty() || container.empty() || container == L"{00000000-0000-0000-FFFF-FFFFFFFFFFFF}") continue;
        // one entry per physical device: prefer the first that has a real name
        if (!seen.insert(container).second) continue;
        out.push_back({name, cls, container});
    }
    SetupDiDestroyDeviceInfoList(set);
    std::sort(out.begin(), out.end(), [](const PnpDevice& a, const PnpDevice& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

std::vector<PnpValue> ListSharedValues(const std::wstring& containerId) {
    std::vector<PnpValue> out;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;
    // A battery value is specific to one device. Generic device properties (config flags, bus number,
    // removal policy, ...) are numeric too and look like "15", "3", "0", "1": they exist on dozens of
    // device nodes. So every numeric 0..100 key is counted over all present devices, and keys found on
    // more than a few nodes are dropped (Windows' own battery key is always kept).
    std::map<std::pair<std::wstring, uint32_t>, int> seenOn;
    SP_DEVINFO_DATA d{sizeof d};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        bool mine = ContainerOf(set, d) == containerId;
        DWORD count = 0;
        SetupDiGetDevicePropertyKeys(set, &d, nullptr, 0, &count, 0);
        if (!count || count > 512) continue;
        std::vector<DEVPROPKEY> keys(count);
        if (!SetupDiGetDevicePropertyKeys(set, &d, keys.data(), count, &count, 0)) continue;
        std::wstring inst, name;
        for (const auto& key : keys) {
            BYTE buf[16]{};
            DEVPROPTYPE type = 0;
            DWORD need = 0;
            if (!SetupDiGetDevicePropertyW(set, &d, &key, &type, buf, sizeof buf, &need, 0)) continue;
            auto pct = AsPercent(type, buf, need);
            if (!pct) continue;
            std::wstring guid = GuidToString(key.fmtid);
            ++seenOn[{guid, key.pid}];
            if (!mine) continue;
            if (inst.empty()) {
                inst = InstanceId(set, d);
                name = RegString(set, d, SPDRP_FRIENDLYNAME);
                if (name.empty()) name = RegString(set, d, SPDRP_DEVICEDESC);
            }
            PnpValue v;
            v.ownerInstanceId = inst;
            v.ownerName = name;
            v.guid = guid;
            v.pid = key.pid;
            v.value = *pct;
            v.knownBattery = IsEqualGUID(key.fmtid, kBatteryGuid) && key.pid == 2;
            v.label = v.knownBattery ? L"Pil düzeyi (Windows'un bildirdiği)"
                                     : L"Paylaşılan değer " + guid.substr(1, 8) + L"…/" + std::to_wstring(v.pid);
            out.push_back(std::move(v));
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const PnpValue& v) { return !v.knownBattery && seenOn[{v.guid, v.pid}] > 3; }),
              out.end());
    std::stable_sort(out.begin(), out.end(), [](const PnpValue& a, const PnpValue& b) { return a.knownBattery > b.knownBattery; });
    return out;
}
std::optional<int> ReadSharedValue(const std::wstring& instanceId, const std::wstring& guid, uint32_t pid) {
    GUID g;
    if (!GuidFromString(guid, &g)) return std::nullopt;
    DEVINST inst = 0;
    if (CM_Locate_DevNodeW(&inst, const_cast<DEVINSTID_W>(instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return std::nullopt;
    DEVPROPKEY key{g, pid};
    BYTE buf[16]{};
    ULONG size = sizeof buf;
    DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(inst, &key, &type, buf, &size, 0) != CR_SUCCESS) return std::nullopt;
    return AsPercent(type, buf, size);
}

// ---------------------------------------------------------------- provider for the user's picks
namespace {
class CustomProvider : public BatteryProvider {
public:
    const char* Id() const override { return "custom"; }
    void Start() override {
        started_ = true;
        status_ = L"Etkin";
        Poll();
    }
    void Stop() override {
        started_ = false;
        ui::ClearTimeout(timer_);
    }
    void RequestRefresh() override { Poll(); }
    void SetPaused(bool p) override {
        paused_ = p;
        if (p) ui::ClearTimeout(timer_);
        else Poll();
    }
    ProviderInfo Info() const override {
        return {"custom", L"Eklenen cihazlar", status_ + L" (" + std::to_wstring(Store::Get().settings().customBattery.size()) + L")", false};
    }

private:
    void Poll() {
        if (!started_) return;
        ui::ClearTimeout(timer_);
        std::vector<battery::DeviceBattery> out;
        int64_t now = UnixNowMs();
        for (const auto& s : Store::Get().settings().customBattery) {
            battery::DeviceBattery d;
            d.id = "custom:" + s.id;
            d.name = FromUtf8(s.name);
            d.method = "custom-pnp";
            d.connection = battery::Connection::Unknown;
            auto v = ReadSharedValue(FromUtf8(s.instanceId), FromUtf8(s.guid), static_cast<uint32_t>(s.pid));
            if (v) {
                d.percent = *v;
                d.connected = true;
                d.updatedMs = now;
                last_[s.id] = {*v, now};
            } else if (auto it = last_.find(s.id); it != last_.end()) { // device gone: keep the last value, marked as such
                d.percent = it->second.first;
                d.updatedMs = it->second.second;
                d.connected = false;
            }
            out.push_back(std::move(d));
        }
        BatteryService::Get().Report(Id(), std::move(out));
        if (!paused_) {
            timer_ = ui::SetTimeout(60000, [this] {
                timer_ = 0;
                Poll();
            });
        }
    }
    bool started_ = false, paused_ = false;
    std::wstring status_ = L"Başlatılmadı";
    uint64_t timer_ = 0;
    std::map<std::string, std::pair<int, int64_t>> last_;
};
} // namespace

std::unique_ptr<BatteryProvider> MakeCustomProvider() { return std::make_unique<CustomProvider>(); }

} // namespace nb::sys
