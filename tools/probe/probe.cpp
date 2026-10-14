// Technical risk probe: checks APIs whose availability decides the architecture.
// Not part of the product. Build: see tools\probe\build_probe.cmd
#include <windows.h>
#include <taskschd.h>
#include <comdef.h>
#include <sddl.h>
#include <cstdio>
#include <string>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.Media.Control.h>
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsupp.lib")
#pragma comment(lib, "windowsapp.lib")

using namespace winrt;

static void ProbeNotifications() {
    using namespace Windows::UI::Notifications::Management;
    try {
        auto listener = UserNotificationListener::Current();
        auto st = listener.GetAccessStatus();
        printf("[notif] GetAccessStatus = %d (0=Unspecified,1=Allowed,2=Denied)\n", (int)st);
        if (st == UserNotificationListenerAccessStatus::Allowed) {
            auto list = listener.GetNotificationsAsync(Windows::UI::Notifications::NotificationKinds::Toast).get();
            printf("[notif] notifications available: %u\n", list.Size());
            try {
                auto tok = listener.NotificationChanged([](auto&&, auto&&) {});
                printf("[notif] NotificationChanged subscribe OK\n");
                listener.NotificationChanged(tok);
            } catch (hresult_error const& e) {
                printf("[notif] NotificationChanged subscribe failed 0x%08X %ls\n", (unsigned)e.code(), e.message().c_str());
            }
        }
    } catch (hresult_error const& e) {
        printf("[notif] exception 0x%08X %ls\n", (unsigned)e.code(), e.message().c_str());
    }
}

static void ProbeMedia() {
    using namespace Windows::Media::Control;
    try {
        auto mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto s = mgr.GetCurrentSession();
        printf("[media] manager OK, sessions=%u current=%ls\n", mgr.GetSessions().Size(),
               s ? s.SourceAppUserModelId().c_str() : L"(none)");
    } catch (hresult_error const& e) {
        printf("[media] exception 0x%08X\n", (unsigned)e.code());
    }
}

static std::wstring CurrentUserSid() {
    HANDLE tok{};
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok);
    BYTE buf[256]; DWORD len = 0;
    GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len);
    CloseHandle(tok);
    LPWSTR s{}; ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &s);
    std::wstring r = s; LocalFree(s); return r;
}

static void ProbeTask() {
    ITaskService* svc{};
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&svc));
    if (FAILED(hr)) { printf("[task] CoCreate 0x%08X\n", (unsigned)hr); return; }
    hr = svc->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    ITaskFolder* root{}; svc->GetFolder(_bstr_t(L"\\"), &root);
    ITaskDefinition* def{}; svc->NewTask(0, &def);
    ITriggerCollection* trigs{}; def->get_Triggers(&trigs);
    ITrigger* trig{}; trigs->Create(TASK_TRIGGER_LOGON, &trig);
    ILogonTrigger* lt{}; trig->QueryInterface(IID_PPV_ARGS(&lt));
    wchar_t user[256]; DWORD n = 256; GetUserNameW(user, &n);
    wchar_t dom[256]; DWORD dn = 256; GetEnvironmentVariableW(L"USERDOMAIN", dom, dn);
    std::wstring full = std::wstring(dom) + L"\\" + user;
    lt->put_UserId(_bstr_t(full.c_str()));
    IActionCollection* acts{}; def->get_Actions(&acts);
    IAction* act{}; acts->Create(TASK_ACTION_EXEC, &act);
    IExecAction* ex{}; act->QueryInterface(IID_PPV_ARGS(&ex));
    ex->put_Path(_bstr_t(L"C:\\Windows\\System32\\cmd.exe"));
    ex->put_Arguments(_bstr_t(L"/c exit"));
    IPrincipal* pr{}; def->get_Principal(&pr);
    pr->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    pr->put_UserId(_bstr_t(full.c_str()));
    pr->put_RunLevel(TASK_RUNLEVEL_LUA);
    IRegisteredTask* reg{};
    hr = root->RegisterTaskDefinition(_bstr_t(L"NotchBarProbeTask"), def, TASK_CREATE_OR_UPDATE,
                                      _variant_t(), _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(L""), &reg);
    printf("[task] Register logon task as %ls (non-elevated) hr=0x%08X\n", full.c_str(), (unsigned)hr);
    if (SUCCEEDED(hr)) {
        hr = root->DeleteTask(_bstr_t(L"NotchBarProbeTask"), 0);
        printf("[task] Delete hr=0x%08X\n", (unsigned)hr);
    }
}

int main() {
    init_apartment(apartment_type::multi_threaded);
    BOOL elevated = FALSE; HANDLE t{}; TOKEN_ELEVATION e{}; DWORD l{};
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t);
    GetTokenInformation(t, TokenElevation, &e, sizeof e, &l); CloseHandle(t);
    printf("[env] elevated=%lu sid=%ls\n", e.TokenIsElevated, CurrentUserSid().c_str());
    ProbeNotifications();
    ProbeMedia();
    ProbeTask();
    return 0;
}
