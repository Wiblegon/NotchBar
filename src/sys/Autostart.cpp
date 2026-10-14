#include "pch.h"

#include "sys/Autostart.h"

#include <comdef.h>
#include <taskschd.h>

#include "core/Log.h"

#pragma comment(lib, "comsuppw.lib")

namespace nb::sys::autostart {
namespace {
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"NotchBar";

std::wstring UserAccount() {
    wchar_t user[256], domain[256];
    DWORD n = 256, d = 256;
    GetUserNameW(user, &n);
    if (!GetEnvironmentVariableW(L"USERDOMAIN", domain, d)) return user;
    return std::wstring(domain) + L"\\" + user;
}

winrt::com_ptr<ITaskFolder> RootFolder() {
    winrt::com_ptr<ITaskService> svc;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(svc.put())))) return nullptr;
    if (FAILED(svc->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t()))) return nullptr;
    winrt::com_ptr<ITaskFolder> root;
    svc->GetFolder(_bstr_t(L"\\"), root.put());
    return root;
}

bool TaskExists() {
    auto root = RootFolder();
    if (!root) return false;
    winrt::com_ptr<IRegisteredTask> t;
    return SUCCEEDED(root->GetTask(_bstr_t(TaskName().c_str()), t.put()));
}

bool RunValueExists() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    bool exists = RegQueryValueExW(k, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return exists;
}

bool RegisterTask(const std::wstring& exe, std::wstring* err) {
    auto fail = [&](const wchar_t* what, HRESULT hr) {
        wchar_t buf[128];
        swprintf(buf, 128, L"%s (0x%08X)", what, static_cast<unsigned>(hr));
        if (err) *err = buf;
        LOGE("autostart task: %ls", buf);
        return false;
    };
    winrt::com_ptr<ITaskService> svc;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(svc.put()));
    if (FAILED(hr)) return fail(L"Görev Zamanlayıcı yok", hr);
    if (FAILED(hr = svc->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t()))) return fail(L"bağlanılamadı", hr);
    winrt::com_ptr<ITaskFolder> root;
    svc->GetFolder(_bstr_t(L"\\"), root.put());
    winrt::com_ptr<ITaskDefinition> def;
    svc->NewTask(0, def.put());

    winrt::com_ptr<IRegistrationInfo> info;
    def->get_RegistrationInfo(info.put());
    info->put_Author(_bstr_t(L"NotchBar"));
    info->put_Description(_bstr_t(L"NotchBar arayüzünü oturum açıldığında başlatır."));

    std::wstring account = UserAccount();
    winrt::com_ptr<IPrincipal> pr;
    def->get_Principal(pr.put());
    pr->put_UserId(_bstr_t(account.c_str()));
    pr->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    pr->put_RunLevel(TASK_RUNLEVEL_LUA); // never elevated in daily use

    winrt::com_ptr<ITaskSettings> st;
    def->get_Settings(st.put());
    st->put_Enabled(VARIANT_TRUE);
    st->put_StartWhenAvailable(VARIANT_FALSE);
    st->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
    st->put_StopIfGoingOnBatteries(VARIANT_FALSE);
    st->put_RunOnlyIfNetworkAvailable(VARIANT_FALSE);
    st->put_RunOnlyIfIdle(VARIANT_FALSE);
    st->put_ExecutionTimeLimit(_bstr_t(L"PT0S")); // no 72h default limit
    st->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
    st->put_Priority(4);                          // normal (scheduler default 7 = below normal)
    st->put_AllowHardTerminate(VARIANT_TRUE);
    st->put_Compatibility(TASK_COMPATIBILITY_V2_1);
    winrt::com_ptr<IIdleSettings> idle;
    st->get_IdleSettings(idle.put());
    if (idle) {
        idle->put_StopOnIdleEnd(VARIANT_FALSE);
        idle->put_RestartOnIdle(VARIANT_FALSE);
    }

    winrt::com_ptr<ITriggerCollection> trigs;
    def->get_Triggers(trigs.put());
    winrt::com_ptr<ITrigger> trig;
    trigs->Create(TASK_TRIGGER_LOGON, trig.put());
    auto logon = trig.as<ILogonTrigger>();
    logon->put_UserId(_bstr_t(account.c_str())); // only this user's logon; no delay set

    winrt::com_ptr<IActionCollection> acts;
    def->get_Actions(acts.put());
    winrt::com_ptr<IAction> act;
    acts->Create(TASK_ACTION_EXEC, act.put());
    auto exec = act.as<IExecAction>();
    exec->put_Path(_bstr_t((L"\"" + exe + L"\"").c_str())); // quoted: paths with spaces
    exec->put_Arguments(_bstr_t(L"--autostart"));
    std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
    exec->put_WorkingDirectory(_bstr_t(dir.c_str()));

    winrt::com_ptr<IRegisteredTask> reg;
    hr = root->RegisterTaskDefinition(_bstr_t(TaskName().c_str()), def.get(), TASK_CREATE_OR_UPDATE, _variant_t(), _variant_t(),
                                      TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(L""), reg.put());
    if (FAILED(hr)) return fail(L"görev kaydedilemedi", hr);
    return true;
}
} // namespace

std::wstring TaskName() {
    wchar_t user[256];
    DWORD n = 256;
    GetUserNameW(user, &n);
    return std::wstring(L"NotchBar (") + user + L")";
}

const wchar_t* MethodName(Method m) {
    switch (m) {
    case Method::ScheduledTask: return L"Görev Zamanlayıcı (oturum açılışı)";
    case Method::RunKey: return L"Başlangıç kaydı (Run)";
    default: return L"Kapalı";
    }
}

Method Current() {
    if (TaskExists()) return Method::ScheduledTask;
    if (RunValueExists()) return Method::RunKey;
    return Method::None;
}

bool Enable(const std::wstring& exe, std::wstring* err) {
    if (RegisterTask(exe, err)) {
        // Make sure an older fallback entry does not start a second copy.
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, kRunValue);
            RegCloseKey(k);
        }
        LOGI("autostart: scheduled task registered");
        return true;
    }
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    std::wstring cmd = L"\"" + exe + L"\" --autostart";
    bool ok = RegSetValueExW(k, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                             static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(k);
    LOGW("autostart: fell back to HKCU Run (%d)", ok);
    return ok;
}

bool Disable() {
    bool ok = true;
    if (auto root = RootFolder()) {
        HRESULT hr = root->DeleteTask(_bstr_t(TaskName().c_str()), 0);
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) ok = false;
    }
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, kRunValue);
        RegCloseKey(k);
    }
    LOGI("autostart disabled (%d)", ok);
    return ok;
}

} // namespace nb::sys::autostart
