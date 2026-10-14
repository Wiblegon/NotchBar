# Dev helper: run a command OUTSIDE the current app-package container (e.g. when this shell was
# started by a packaged app whose %LOCALAPPDATA%/HKCU writes are virtualized). Uses a temporary
# per-user scheduled task that is deleted afterwards. Output is written to $OutFile.
param([Parameter(Mandatory)][string]$Command, [string]$OutFile = "$PSScriptRoot\..\build\outside_out.txt", [int]$TimeoutSec = 60)
$name = "NotchBarDevOutside_" + [guid]::NewGuid().ToString("N").Substring(0, 8)
$OutFile = [IO.Path]::GetFullPath($OutFile)
Remove-Item $OutFile -ErrorAction SilentlyContinue
$ps = "& { $Command } *>&1 | Out-File -Encoding utf8 -FilePath '$OutFile'"
$enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($ps))
$action = New-ScheduledTaskAction -Execute "conhost.exe" -Argument "--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -NonInteractive -WindowStyle Hidden -EncodedCommand $enc"
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings (New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries) | Out-Null
Start-ScheduledTask -TaskName $name
$sw = [Diagnostics.Stopwatch]::StartNew()
do { Start-Sleep -Milliseconds 300; $st = (Get-ScheduledTask -TaskName $name).State } while ($st -eq 'Running' -and $sw.Elapsed.TotalSeconds -lt $TimeoutSec)
Start-Sleep -Milliseconds 300
Unregister-ScheduledTask -TaskName $name -Confirm:$false
if (Test-Path $OutFile) { Get-Content $OutFile -Encoding UTF8 }
