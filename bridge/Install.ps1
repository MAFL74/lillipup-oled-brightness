$ErrorActionPreference = 'Stop'
$taskName = 'Lillipup OLED Brightness Bridge'
$exe = Join-Path $PSScriptRoot 'LillipupBrightnessBridge.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Missing executable: $exe" }
$brightness = Get-CimInstance -Namespace root\WMI -ClassName WmiMonitorBrightness -ErrorAction Stop | Select-Object -First 1
if (-not $brightness) { throw 'Windows does not expose WmiMonitorBrightness on this device.' }
$action = New-ScheduledTaskAction -Execute $exe
$trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
Start-Sleep -Seconds 2
$task = Get-ScheduledTask -TaskName $taskName
if ($task.State -ne 'Running') { throw "Task did not start. Current state: $($task.State)" }
Write-Host "Installed and running. Windows brightness is $($brightness.CurrentBrightness)%."

