# Dev helper: send real key events through SendInput (as a keyboard would).
# Usage: keys.ps1 -Combo "LWIN"            (tap)
#        keys.ps1 -Combo "LWIN+E"          (chord, keys released in reverse order)
#        keys.ps1 -Text "spotify"          (types Unicode text)
param([string]$Combo = "", [string]$Text = "", [int]$WaitMs = 0)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class K {
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit, Size=40)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] i, int size);
  public static void Key(ushort vk, bool up) {
    var i = new INPUT[1]; i[0].type = 1; i[0].ki.wVk = vk;
    uint f = up ? 2u : 0u;
    if (vk == 0x5B || vk == 0x5C || (vk >= 0x21 && vk <= 0x28)) f |= 1u; // extended
    i[0].ki.dwFlags = f; SendInput(1, i, 40);
  }
  public static void Uni(char c) {
    var i = new INPUT[2];
    i[0].type = 1; i[0].ki.wScan = c; i[0].ki.dwFlags = 4;
    i[1].type = 1; i[1].ki.wScan = c; i[1].ki.dwFlags = 4 | 2;
    SendInput(2, i, 40);
  }
}
"@
$map = @{ LWIN=0x5B; RWIN=0x5C; SHIFT=0x10; CTRL=0x11; ALT=0x12; ESC=0x1B; ENTER=0x0D; TAB=0x09; UP=0x26; DOWN=0x28; LEFT=0x25; RIGHT=0x27; BACK=0x08; DEL=0x2E; SPACE=0x20; F4=0x73; F11=0x7A; F5=0x74 }
if ($Combo) {
  $vks = @()
  foreach ($p in $Combo.Split('+')) {
    if ($map.ContainsKey($p.ToUpper())) { $vks += [uint16]$map[$p.ToUpper()] }
    else { $vks += [uint16][char]$p.ToUpper() }
  }
  foreach ($v in $vks) { [K]::Key($v, $false); Start-Sleep -Milliseconds 30 }
  [array]::Reverse($vks)
  foreach ($v in $vks) { [K]::Key($v, $true); Start-Sleep -Milliseconds 30 }
}
if ($Text) { foreach ($c in $Text.ToCharArray()) { [K]::Uni($c); Start-Sleep -Milliseconds 15 } }
if ($WaitMs -gt 0) { Start-Sleep -Milliseconds $WaitMs }
