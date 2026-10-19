// HyperX Cloud III S Wireless (2.4 GHz dongle 03F0:06BE, also 03F0:02CC).
// Protocol source: HeyOkay/HaloBattery docs/protocols.md (MIT) and LennardKittner/HyperHeadset
// src/devices/cloud_iii_s_wireless.rs (MIT) - reported as verified on real hardware there.
//   request : 64-byte report  0C 02 03 01 00 <cmd> 00..   (byte 3 = 01: READ ONLY)
//   commands: 0x02 headset connected (value 2 = yes), 0x06 battery %, 0x48 charge state
//   answer  : input report 0x0C, byte 5 = cmd, byte 6 = value (0xFF = no value)
//   events  : input report 0x0D, byte 4 = type (1 battery, 10 charge, 12 connection), byte 5 = value
// This reader never sends write/configuration commands (byte 3 = 00), firmware or RGB packets.
// Input reports are received by a blocking overlapped read thread: zero CPU while idle.
#include "pch.h"

#include <cfgmgr32.h>
#include <hidsdi.h>
#include <setupapi.h>
extern "C" {
#include <hidpi.h>
}

#include "core/Log.h"
#include "core/Worker.h"
#include "model/Battery.h"
#include "sys/battery/BatteryService.h"

namespace nb::sys {
namespace {
using namespace battery;

struct Collection {
    std::wstring path;
    HANDLE h = INVALID_HANDLE_VALUE;
    USHORT usagePage = 0, inLen = 0, outLen = 0, featLen = 0;
    bool command = false; // the descriptor defines output report 0x0C: the headset command channel
};

bool IsTarget(const std::wstring& path) {
    std::wstring p = path;
    std::transform(p.begin(), p.end(), p.begin(), ::towlower);
    return p.find(L"vid_03f0&pid_06be") != std::wstring::npos || p.find(L"vid_03f0&pid_02cc") != std::wstring::npos;
}

std::vector<std::wstring> FindPaths() {
    std::vector<std::wstring> out;
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA ifd{sizeof ifd};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        std::vector<uint8_t> buf(need);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr) && IsTarget(det->DevicePath))
            out.push_back(det->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

class HyperXProvider : public BatteryProvider {
public:
    const char* Id() const override { return "hyperx"; }

    void Start() override {
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        CM_NOTIFY_FILTER f{};
        f.cbSize = sizeof f;
        f.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        HidD_GetHidGuid(&f.u.DeviceInterface.ClassGuid);
        CM_Register_Notification(&f, this, &HyperXProvider::OnPnp, &notify_);
        ScheduleRescan(0);
    }

    void Stop() override {
        if (notify_) CM_Unregister_Notification(notify_);
        notify_ = nullptr;
        StopReader();
        if (stop_) CloseHandle(stop_);
        stop_ = nullptr;
    }

    void SetPaused(bool p) override { paused_ = p; }

    void RequestRefresh() override {
        if (paused_ || !active_) return;
        int64_t now = UnixNowMs();
        if (now - lastQuery_ < 30000) return;
        lastQuery_ = now;
        WorkerPool::Background().Submit([this] { QueryAll(); }, "hyperx-query");
    }

    ProviderInfo Info() const override { return {"hyperx", L"HyperX Cloud III S", status_, false}; }

private:
    static DWORD CALLBACK OnPnp(HCMNOTIFICATION, PVOID ctx, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA data, DWORD) {
        auto* self = static_cast<HyperXProvider*>(ctx);
        if ((action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) && data &&
            IsTarget(data->u.DeviceInterface.SymbolicLink))
            self->ScheduleRescan(800); // receiver plugged in / pulled out
        return ERROR_SUCCESS;
    }

    void ScheduleRescan(int delayMs) {
        WorkerPool::Background().Submit(
            [this, delayMs] {
                if (delayMs) Sleep(delayMs);
                Rescan();
            },
            "hyperx-rescan");
    }

    void Rescan() {
        auto paths = FindPaths();
        if (paths.empty()) {
            if (active_) LOGI("hyperx: receiver removed");
            StopReader();
            status_ = L"Alıcı takılı değil";
            rec_ = {};
            Publish(false);
            return;
        }
        if (active_) return;
        std::vector<Collection> cols;
        for (const auto& p : paths) {
            Collection c;
            c.path = p;
            c.h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, nullptr);
            if (c.h == INVALID_HANDLE_VALUE) continue;
            PHIDP_PREPARSED_DATA pp = nullptr;
            if (HidD_GetPreparsedData(c.h, &pp)) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                    c.usagePage = caps.UsagePage;
                    c.inLen = caps.InputReportByteLength;
                    c.outLen = caps.OutputReportByteLength;
                    c.featLen = caps.FeatureReportByteLength;
                    // Diagnostics: report ids the descriptor defines for output / input.
                    USHORT n = caps.NumberOutputValueCaps;
                    std::vector<HIDP_VALUE_CAPS> vc(n ? n : 1);
                    if (n && HidP_GetValueCaps(HidP_Output, vc.data(), &n, pp) == HIDP_STATUS_SUCCESS)
                        for (USHORT k = 0; k < n; ++k) {
                            if (vc[k].ReportID == hyperx::kReportId) c.command = true;
                        }
                    USHORT bn = caps.NumberOutputButtonCaps;
                    std::vector<HIDP_BUTTON_CAPS> bc(bn ? bn : 1);
                    if (bn && HidP_GetButtonCaps(HidP_Output, bc.data(), &bn, pp) == HIDP_STATUS_SUCCESS)
                        for (USHORT k = 0; k < bn; ++k) {
                            if (bc[k].ReportID == hyperx::kReportId) c.command = true;
                        }
                }
                HidD_FreePreparsedData(pp);
            }
            // Only the collection that carries the command channel (output report 0x0C); the keyboard,
            // consumer-control and telephony collections are never opened for I/O.
            if (!c.command) {
                CloseHandle(c.h);
                continue;
            }
            cols.push_back(c);
        }
        if (cols.empty()) {
            status_ = L"Alıcı bulundu, satıcı koleksiyonu açılamadı";
            Publish(false);
            return;
        }
        {
            std::lock_guard lk(mu_);
            cols_ = std::move(cols);
        }
        std::unique_lock rl(recMu_);
        rec_ = {};
        rec_.id = "hyperx:03f0:cloud3s";
        rec_.name = L"HyperX Cloud III S Wireless";
        rec_.kind = DeviceKind::Headset;
        rec_.connection = Connection::Wireless24GHz;
        rec_.method = "hyperx-hid";
        rec_.verifiedOnHardware = true; // answers verified on a real Cloud III S (03F0:06BE) dongle
        rec_.connected = false;
        rl.unlock();
        active_ = true;
        ResetEvent(stop_);
        reader_ = std::thread([this] { ReadLoop(); });
        status_ = L"Alıcı bulundu";
        LOGI("hyperx: receiver found (%zu vendor collections)", cols_.size());
        QueryAll();
    }

    void StopReader() {
        if (!active_) return;
        active_ = false;
        if (stop_) SetEvent(stop_);
        {
            std::lock_guard lk(mu_);
            for (auto& c : cols_) CancelIoEx(c.h, nullptr);
        }
        if (reader_.joinable()) reader_.join();
        std::lock_guard lk(mu_);
        for (auto& c : cols_)
            if (c.h != INVALID_HANDLE_VALUE) CloseHandle(c.h);
        cols_.clear();
    }

    bool Send(uint8_t cmd) {
        std::lock_guard lk(mu_);
        for (auto& c : cols_) {
            if (c.outLen < 7) {
                LOGI("hyperx: skip collection (out report %u bytes)", c.outLen);
                continue;
            }
            auto pkt = hyperx::BuildQuery(cmd, c.outLen);
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            DWORD wrote = 0;
            BOOL ok = WriteFile(c.h, pkt.data(), static_cast<DWORD>(pkt.size()), nullptr, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ov.hEvent, 500) == WAIT_OBJECT_0) ok = GetOverlappedResult(c.h, &ov, &wrote, FALSE);
                else CancelIoEx(c.h, &ov);
            }
            CloseHandle(ov.hEvent);
            if (!ok) {
                DWORD err = GetLastError();
                bool viaSet = HidD_SetOutputReport(c.h, pkt.data(), static_cast<ULONG>(pkt.size())) != FALSE;
                LOGI("hyperx: write failed err=%lu, SetOutputReport=%d (out=%u feat=%u)", err, viaSet ? 1 : 0, c.outLen, c.featLen);
                if (viaSet) return true;
            }
            if (ok) return true;
            // Some dongles accept the same bytes only as a feature report on Windows.
            if (c.featLen >= 7) {
                auto f = hyperx::BuildQuery(cmd, c.featLen);
                if (HidD_SetFeature(c.h, f.data(), static_cast<ULONG>(f.size()))) return true;
            }
        }
        return false;
    }

    void QueryAll() {
        if (paused_ || !active_) return;
        lastQuery_ = UnixNowMs();
        bool ok = Send(hyperx::kCmdConnected);
        Sleep(60);
        ok |= Send(hyperx::kCmdBattery);
        Sleep(60);
        ok |= Send(hyperx::kCmdCharge);
        LOGI("hyperx: query sent=%d", ok ? 1 : 0);
        if (!ok) {
            status_ = L"Sorgu gönderilemedi";
            Publish(true);
        }
    }

    void ReadLoop() {
        std::vector<Collection> cols;
        {
            std::lock_guard lk(mu_);
            cols = cols_;
        }
        size_t n = cols.size();
        std::vector<OVERLAPPED> ov(n);
        std::vector<std::vector<uint8_t>> bufs(n);
        std::vector<HANDLE> waits;
        auto issue = [&](size_t i) {
            ov[i] = {};
            ov[i].hEvent = waits[i];
            ResetEvent(waits[i]);
            bufs[i].assign(std::max<size_t>(cols[i].inLen, 64), 0);
            if (!ReadFile(cols[i].h, bufs[i].data(), static_cast<DWORD>(bufs[i].size()), nullptr, &ov[i]) &&
                GetLastError() != ERROR_IO_PENDING)
                return false;
            return true;
        };
        for (size_t i = 0; i < n; ++i) waits.push_back(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        bool alive = true;
        for (size_t i = 0; i < n; ++i) alive &= issue(i);
        std::vector<HANDLE> all = waits;
        all.push_back(stop_);
        while (alive) {
            DWORD w = WaitForMultipleObjects(static_cast<DWORD>(all.size()), all.data(), FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + n || w == WAIT_FAILED) break;
            size_t i = w - WAIT_OBJECT_0;
            DWORD got = 0;
            if (!GetOverlappedResult(cols[i].h, &ov[i], &got, FALSE)) {
                alive = false; // device gone
                break;
            }
            Handle(bufs[i].data(), got);
            alive = issue(i);
        }
        for (auto h : waits) CloseHandle(h);
        if (!alive && active_) ScheduleRescan(500);
    }

    void Handle(const uint8_t* data, size_t len) {
        auto r = hyperx::ParseReport(data, len);
        if (!r.percent && !r.charge && !r.headsetConnected) return;
        std::unique_lock rl(recMu_);
        if (r.headsetConnected) {
            rec_.connected = *r.headsetConnected;
            if (!rec_.connected) {
                rec_.percent.reset(); // headset off: the receiver has no live value
                rec_.charge = Charge::Unknown;
            }
        }
        if (r.percent) {
            rec_.percent = r.percent;
            rec_.connected = true;
            rec_.updatedMs = UnixNowMs();
        }
        if (r.charge) rec_.charge = *r.charge;
        rl.unlock();
        status_ = L"Etkin";
        Publish(true);
    }

    void Publish(bool present) {
        std::vector<DeviceBattery> out;
        if (present && active_) {
            std::lock_guard rl(recMu_);
            out.push_back(rec_);
        }
        BatteryService::Get().Report(Id(), std::move(out));
    }

    HCMNOTIFICATION notify_ = nullptr;
    HANDLE stop_ = nullptr;
    std::thread reader_;
    std::mutex mu_, recMu_;
    std::vector<Collection> cols_;
    std::atomic<bool> active_{false}, paused_{false};
    std::atomic<int64_t> lastQuery_{0};
    DeviceBattery rec_;
    std::wstring status_ = L"Alıcı takılı değil";
};
} // namespace

std::unique_ptr<BatteryProvider> MakeHyperXProvider() { return std::make_unique<HyperXProvider>(); }

} // namespace nb::sys
