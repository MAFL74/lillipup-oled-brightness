$taskName = 'Lillipup OLED Brightness Bridge'
$exe = Join-Path $PSScriptRoot 'LillipupBrightnessBridge.exe'
Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
Get-Process -Name 'LillipupBrightnessBridge' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force
Write-Host 'Removed the startup task and stopped the brightness bridge.'

