$ErrorActionPreference = 'Stop'
$serviceName = 'LillipupAuxProbe'
& "$env:WINDIR\System32\sc.exe" stop $serviceName
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1062) { throw "Could not stop driver service (sc.exe exit $LASTEXITCODE)." }
& "$env:WINDIR\System32\sc.exe" delete $serviceName
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1060) { throw "Could not remove driver service (sc.exe exit $LASTEXITCODE)." }
Write-Host 'Removed the Lillipup AUX Probe service. Remove the test certificate and test-signing mode separately if no longer needed.'

