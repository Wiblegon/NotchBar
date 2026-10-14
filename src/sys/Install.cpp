#include "pch.h"

#include "sys/Install.h"

#include <filesystem>

#include "app/Messages.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "sys/Autostart.h"
#include "sys/TaskbarController.h"

namespace nb::sys::install {
namespace {
const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NotchBar";

std::wstring LinkIn(REFKNOWNFOLDERID folder, const wchar_t* name) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(folder, 0, nullptr, &p))) r = std::wstring(p) + L"\\" + name;
    CoTaskMemFree(p);
    return r;
}
std::wstring ShortcutPath() { return LinkIn(FOLDERID_Programs, L"NotchBar.lnk"); }
// Desktop: one shortcut to start the app, one safety shortcut that always brings the taskbar back.
std::wstring DesktopLink() { return LinkIn(FOLDERID_Desktop, L"NotchBar.lnk"); }
std::wstring DesktopRestoreLink() { return LinkIn(FOLDERID_Desktop, L"NotchBar - Görev çubuğunu geri getir.lnk"); }

void SetString(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * 2));
}

bool StopRunning() {
    HWND w = FindWindowW(kMainClass, nullptr);
    if (!w) return true;
    PostMessageW(w, WM_CLOSE, 0, 0); // graceful: the app restores the taskbar itself
    for (int i = 0; i < 50; ++i) {
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\NotchBar.Instance");
        if (!m) return true;
        CloseHandle(m);
        Sleep(100);
    }
    return false;
}

bool CreateShortcut(const std::wstring& target, const std::wstring& at, const wchar_t* args = L"", const wchar_t* desc = L"NotchBar") {
    winrt::com_ptr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())))) return false;
    link->SetPath(target.c_str());
    link->SetArguments(args);
    link->SetDescription(desc);
    link->SetIconLocation(target.c_str(), 0);
    auto pf = link.as<IPersistFile>();
    return SUCCEEDED(pf->Save(at.c_str(), TRUE));
}
} // namespace

int Install(bool quiet) {
    std::wstring dir = paths::InstallDir();
    std::wstring target = dir + L"\\NotchBar.exe";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    StopRunning();
    if (_wcsicmp(paths::ExePath().c_str(), target.c_str()) != 0) {
        if (!CopyFileW(paths::ExePath().c_str(), target.c_str(), FALSE)) {
            if (!quiet) MessageBoxW(nullptr, L"Dosya kopyalanamadı.", L"NotchBar kurulumu", MB_ICONERROR);
            return 1;
        }
    }
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        SetString(k, L"DisplayName", L"NotchBar");
        SetString(k, L"DisplayVersion", L"0.1.0");
        SetString(k, L"Publisher", L"NotchBar");
        SetString(k, L"InstallLocation", dir);
        SetString(k, L"DisplayIcon", target);
        SetString(k, L"UninstallString", L"\"" + target + L"\" --uninstall");
        SetString(k, L"QuietUninstallString", L"\"" + target + L"\" --uninstall --quiet");
        DWORD one = 1;
        RegSetValueExW(k, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), 4);
        RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), 4);
        RegCloseKey(k);
    }
    CreateShortcut(target, ShortcutPath());
    CreateShortcut(target, DesktopLink());
    CreateShortcut(target, DesktopRestoreLink(), L"--restore-taskbar", L"Windows görev çubuğunu geri getirir");
    LOGI("installed to program folder");
    if (quiet) return 0; // scripted install: do not start the UI
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + target + L"\"";
    if (CreateProcessW(target.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}

int Uninstall(bool quiet) {
    BOOL deleteData = FALSE;
    if (!quiet) {
        TASKDIALOGCONFIG tc{sizeof tc};
        tc.pszWindowTitle = L"NotchBar'ı kaldır";
        tc.pszMainInstruction = L"NotchBar kaldırılsın mı?";
        tc.pszContent = L"Windows görev çubuğu eski hâline getirilir ve otomatik başlatma kaydı silinir.";
        tc.pszVerificationText = L"Ayarlarımı ve favorilerimi de sil";
        tc.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
        int button = 0;
        TaskDialogIndirect(&tc, &button, nullptr, &deleteData);
        if (button != IDYES) return 1;
    }
    StopRunning();
    autostart::Disable();
    TaskbarController::RestoreFromState("uninstall");
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    DeleteFileW(ShortcutPath().c_str());
    DeleteFileW(DesktopLink().c_str());
    DeleteFileW(DesktopRestoreLink().c_str());
    if (deleteData) {
        std::error_code ec;
        std::filesystem::remove_all(paths::SharedDataDir(), ec);
    }
    std::wstring dir = paths::InstallDir();
    std::wstring exe = paths::ExePath();
    if (_wcsnicmp(exe.c_str(), dir.c_str(), dir.size()) == 0) {
        // We are running from the folder being removed: delete it after this process exits.
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring cmdExe = std::wstring(sys) + L"\\cmd.exe";
        std::wstring cmd = L"\"" + cmdExe + L"\" /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + dir + L"\"";
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(cmdExe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    } else {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    if (!quiet) MessageBoxW(nullptr, L"NotchBar kaldırıldı.", L"NotchBar", MB_ICONINFORMATION);
    return 0;
}

} // namespace nb::sys::install
