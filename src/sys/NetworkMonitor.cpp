#include "pch.h"

#include "sys/NetworkMonitor.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <netlistmgr.h>
#include <wlanapi.h>

#include "core/Dispatcher.h"
#include "core/Log.h"
#include "core/Worker.h"

namespace nb::sys {
namespace {
HANDLE g_ipNotify = nullptr;
HANDLE g_hintNotify = nullptr;
HANDLE g_wlan = nullptr;

void CALLBACK OnIpChange(PVOID, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) { NetworkMonitor::Get().ScheduleEvaluate(); }
void CALLBACK OnHint(PVOID, NL_NETWORK_CONNECTIVITY_HINT) { NetworkMonitor::Get().ScheduleEvaluate(); }
void WINAPI OnWlan(PWLAN_NOTIFICATION_DATA d, PVOID) {
    if (!d) return;
    if (d->NotificationSource == WLAN_NOTIFICATION_SOURCE_MSM && d->NotificationCode == wlan_notification_msm_signal_quality_change)
        NetworkMonitor::Get().ScheduleEvaluate();
    else if (d->NotificationSource == WLAN_NOTIFICATION_SOURCE_ACM)
        NetworkMonitor::Get().ScheduleEvaluate();
}

using NotifyHintFn = DWORD(WINAPI*)(PNETWORK_CONNECTIVITY_HINT_CHANGE_CALLBACK, PVOID, BOOLEAN, PHANDLE);
using CancelNotifyFn = DWORD(WINAPI*)(HANDLE);

NetState Evaluate() {
    NetState s;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, aa, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, aa, &size);
    }
    ULONG bestMetric = ULONG_MAX;
    if (r == NO_ERROR) {
        for (auto* a = aa; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
            ULONG metric = a->Ipv4Metric ? a->Ipv4Metric : a->Ipv6Metric;
            if (metric >= bestMetric) continue;
            bestMetric = metric;
            if (a->IfType == IF_TYPE_IEEE80211) s.kind = NetKind::Wifi;
            else if (a->IfType == IF_TYPE_ETHERNET_CSMACD) s.kind = NetKind::Ethernet;
            else if (a->IfType == IF_TYPE_WWANPP || a->IfType == IF_TYPE_WWANPP2) s.kind = NetKind::Cellular;
            else s.kind = NetKind::Other;
        }
    }
    winrt::com_ptr<INetworkListManager> nlm;
    if (SUCCEEDED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(nlm.put())))) {
        NLM_CONNECTIVITY c{};
        if (SUCCEEDED(nlm->GetConnectivity(&c)))
            s.internet = (c & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
    }
    if (s.kind == NetKind::Wifi && g_wlan) {
        PWLAN_INTERFACE_INFO_LIST list = nullptr;
        if (WlanEnumInterfaces(g_wlan, nullptr, &list) == ERROR_SUCCESS && list) {
            for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
                auto& itf = list->InterfaceInfo[i];
                if (itf.isState != wlan_interface_state_connected) continue;
                DWORD dataSize = 0;
                PWLAN_CONNECTION_ATTRIBUTES ca = nullptr;
                // Since Windows 11 24H2 this can require location permission; failure is handled.
                if (WlanQueryInterface(g_wlan, &itf.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &dataSize,
                                       reinterpret_cast<void**>(&ca), nullptr) == ERROR_SUCCESS && ca) {
                    s.wifiQuality = static_cast<int>(ca->wlanAssociationAttributes.wlanSignalQuality);
                    auto& ssid = ca->wlanAssociationAttributes.dot11Ssid;
                    int n = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(ssid.ucSSID), ssid.uSSIDLength, nullptr, 0);
                    s.ssid.resize(static_cast<size_t>(n));
                    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(ssid.ucSSID), ssid.uSSIDLength, s.ssid.data(), n);
                    WlanFreeMemory(ca);
                }
                break;
            }
            WlanFreeMemory(list);
        }
    }
    return s;
}
} // namespace

NetworkMonitor& NetworkMonitor::Get() {
    static NetworkMonitor n;
    return n;
}

void NetworkMonitor::Start() {
    NotifyIpInterfaceChange(AF_UNSPEC, OnIpChange, nullptr, FALSE, &g_ipNotify);
    if (auto fn = reinterpret_cast<NotifyHintFn>(GetProcAddress(GetModuleHandleW(L"iphlpapi.dll"), "NotifyNetworkConnectivityHintChange")))
        fn(OnHint, nullptr, FALSE, &g_hintNotify);
    DWORD ver = 0;
    if (WlanOpenHandle(2, nullptr, &ver, &g_wlan) == ERROR_SUCCESS)
        WlanRegisterNotification(g_wlan, WLAN_NOTIFICATION_SOURCE_ACM | WLAN_NOTIFICATION_SOURCE_MSM, TRUE, OnWlan, nullptr,
                                 nullptr, nullptr);
    ScheduleEvaluate();
}

void NetworkMonitor::Stop() {
    if (g_ipNotify) CancelMibChangeNotify2(g_ipNotify);
    g_ipNotify = nullptr;
    if (g_hintNotify) CancelMibChangeNotify2(g_hintNotify);
    g_hintNotify = nullptr;
    if (g_wlan) {
        WlanRegisterNotification(g_wlan, WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
        WlanCloseHandle(g_wlan, nullptr);
    }
    g_wlan = nullptr;
}

void NetworkMonitor::ScheduleEvaluate() {
    if (pending_.exchange(true)) return;
    // Bursts of interface notifications are coalesced into one evaluation.
    ui::Post([this] {
        ui::SetTimeout(250, [this] {
            WorkerPool::Background().Submit(
                [this] {
                    pending_ = false;
                    NetState s = Evaluate();
                    ui::Post([this, s] { Apply(s); });
                },
                "net-eval");
        });
    });
}

void NetworkMonitor::Apply(const NetState& s) {
    bool changed = s.kind != state_.kind || s.internet != state_.internet || s.ssid != state_.ssid ||
                   (s.wifiQuality / 20) != (state_.wifiQuality / 20);
    state_ = s;
    if (changed) {
        LOGI("network: kind=%d internet=%d quality=%d", static_cast<int>(s.kind), s.internet, s.wifiQuality);
        for (auto& l : listeners_) l();
    }
}

void NetworkMonitor::OpenSystemUi() {
    // Windows' own network list flyout; falls back to the Settings page.
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"ms-availablenetworks:", nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        ShellExecuteW(nullptr, L"open", L"ms-settings:network-status", nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace nb::sys
