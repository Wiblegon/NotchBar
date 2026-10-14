# Dev helper: raise a real Windows toast notification (as Windows PowerShell).
param([string]$Title = "NotchBar test bildirimi", [string]$Body = "Windows bildirim altyapısından gelen gerçek bir bildirim.")
[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
[Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime] | Out-Null
$xml = New-Object Windows.Data.Xml.Dom.XmlDocument
$t = [Security.SecurityElement]::Escape($Title); $b = [Security.SecurityElement]::Escape($Body)
$xml.LoadXml("<toast><visual><binding template=`"ToastGeneric`"><text>$t</text><text>$b</text></binding></visual></toast>")
$aumid = '{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\WindowsPowerShell\v1.0\powershell.exe'
[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($aumid).Show((New-Object Windows.UI.Notifications.ToastNotification $xml))
"shown"
