# Dev helper: move the cursor (physical px) and optionally click, for automated visual checks.
param([int]$X, [int]$Y, [switch]$Click, [switch]$Right, [int]$WaitMs = 0)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class M {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
}
"@
[M]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
[M]::SetCursorPos($X, $Y) | Out-Null
Start-Sleep -Milliseconds 30
# a second small move so the target window gets WM_MOUSEMOVE
[M]::SetCursorPos($X, $Y + 1) | Out-Null
if ($Click) { [M]::mouse_event(0x2, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 40; [M]::mouse_event(0x4, 0, 0, 0, [IntPtr]::Zero) }
if ($Right) { [M]::mouse_event(0x8, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 40; [M]::mouse_event(0x10, 0, 0, 0, [IntPtr]::Zero) }
if ($WaitMs -gt 0) { Start-Sleep -Milliseconds $WaitMs }
