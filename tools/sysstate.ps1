# Dev helper: report taskbar / window state for the real-Windows tests.
param([string]$Find = "")
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class W {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string t);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool SystemParametersInfo(int a, int b, ref RECT r, int c);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out int v, int s);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct APPBARDATA { public int cbSize; public IntPtr hWnd; public uint cb; public uint edge; public RECT rc; public IntPtr lParam; }
  [DllImport("shell32.dll")] public static extern UIntPtr SHAppBarMessage(uint m, ref APPBARDATA d);
  public static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
  public static string Txt(IntPtr h) { var s = new StringBuilder(256); GetWindowText(h, s, 256); return s.ToString(); }
  public static List<string> Visible(string cls) {
    var r = new List<string>();
    EnumWindows((h, l) => { if (IsWindowVisible(h) && Cls(h) == cls) { int c; DwmGetWindowAttribute(h, 14, out c, 4); r.Add(Txt(h) + (c != 0 ? " [cloaked]" : "")); } return true; }, IntPtr.Zero);
    return r;
  }
}
"@
$tray = [W]::FindWindow("Shell_TrayWnd", $null)
$abd = New-Object W+APPBARDATA; $abd.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($abd); $abd.hWnd = $tray
$state = [W]::SHAppBarMessage(4, [ref]$abd).ToUInt32()
$wa = New-Object W+RECT; [W]::SystemParametersInfo(0x30, 0, [ref]$wa, 0) | Out-Null
$fg = [W]::GetForegroundWindow()
[pscustomobject]@{
  TrayExists = $tray -ne [IntPtr]::Zero
  TrayVisible = [W]::IsWindowVisible($tray)
  AutoHide = ($state -band 1) -eq 1
  WorkArea = "$($wa.L),$($wa.T),$($wa.R),$($wa.B)"
  Foreground = "$([W]::Cls($fg)) | $([W]::Txt($fg))"
  NotchBarProcs = (Get-Process NotchBar -ErrorAction SilentlyContinue | Measure-Object).Count
}
if ($Find) { [W]::Visible($Find) }
