# Registers the desktop audio sender as a per-user scheduled task.
#   - runs in your interactive session (WASAPI loopback needs it)
#   - a 1-minute trigger revives the supervisor if it ever dies; the task's
#     Enabled flag is the on/off switch (Enable-/Disable-ScheduledTask)
# Example:
#   .\install-sender-task.ps1 -Target 192.0.2.10:46000 `
#       -GstLaunch 'C:\gstreamer\1.0\msvc_x86_64\bin\gst-launch-1.0.exe'
param(
    [Parameter(Mandatory = $true)][string]$Target,
    [Parameter(Mandatory = $true)][string]$GstLaunch,
    [string]$Python = 'C:\Program Files\Python313\pythonw.exe',
    [string]$TaskName = 'AV Sync Desktop Audio Sender'
)
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$state = Join-Path $dir 'state'
$action = New-ScheduledTaskAction -Execute $Python -WorkingDirectory $dir `
    -Argument "`"$dir\desktop_rtp_sender.py`" --target $Target --gst-launch `"$GstLaunch`" --state-dir `"$state`""
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable
$triggers = @(
    (New-ScheduledTaskTrigger -Once -At (Get-Date).Date -RepetitionInterval (New-TimeSpan -Minutes 1)),
    (New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME")
)
Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal -Settings $settings `
    -Trigger $triggers -Description 'WASAPI loopback -> RTP L24 desktop audio sender (av-sync-bridge/rtp).' -Force | Out-Null
Start-ScheduledTask -TaskName $TaskName
Start-Sleep 8
Get-Content (Join-Path $state 'rtp-status.json')
