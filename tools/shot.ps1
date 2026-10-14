# Captures the primary screen (or a region) to a PNG for visual checks during development.
param([string]$Out = "$env:TEMP\nb_shot.png", [int]$X = 0, [int]$Y = 0, [int]$W = 0, [int]$H = 0)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Dpi { [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v); }
"@
[Dpi]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
if ($W -eq 0) { $X = $b.X; $Y = $b.Y; $W = $b.Width; $H = $b.Height }
$bmp = New-Object System.Drawing.Bitmap $W, $H
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($X, $Y, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
"$Out ${W}x${H}"
