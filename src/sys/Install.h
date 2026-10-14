#pragma once
// Per-user install without admin rights:
//   --install    copy to %LOCALAPPDATA%\Programs\NotchBar, register an "Apps & features"
//                entry (HKCU), autostart task, Start menu shortcut, then launch.
//   --uninstall  stop the running copy (it restores the taskbar), remove the autostart task /
//                Run value, restore the taskbar from the saved state, remove the HKCU entry
//                and the shortcut, delete the program folder. User data is kept unless the
//                user chooses to delete it.
namespace nb::sys::install {

int Install(bool quiet);
int Uninstall(bool quiet);

} // namespace nb::sys::install
