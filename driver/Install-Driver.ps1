$ErrorActionPreference = 'Stop'
$serviceName = 'LillipupAuxProbe'
$driverPath = Join-Path $PSScriptRoot 'LillipupAuxProbe.sys'
$certificatePath = Join-Path $PSScriptRoot 'LillipupAuxProbe-test.cer'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell window.'
}

$gpu = Get-PnpDevice -PresentOnly -Class Display | Where-Object {
    $_.InstanceId -like 'PCI\VEN_8086&DEV_9A78*'
} | Select-Object -First 1
if (-not $gpu) {
    throw 'Expected Intel GPU PCI ID 8086:9A78 was not found. Refusing to install this machine-specific driver.'
}

$certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
$signature = Get-AuthenticodeSignature -FilePath $driverPath
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
    throw 'Driver signature is invalid or does not match the included public certificate.'
}

$bcd = (& "$env:WINDIR\System32\bcdedit.exe" /enum '{current}') -join "`n"
if ($bcd -notmatch '(?im)^\s*testsigning\s+Yes\s*$') {
    throw 'Windows test-signing mode is not enabled. This script will not change boot security settings.'
}

$service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if (-not $service) {
    $quotedDriverPath = '"' + $driverPath + '"'
    & "$env:WINDIR\System32\sc.exe" create $serviceName type= kernel start= demand binPath= $quotedDriverPath DisplayName= 'Lillipup AUX Probe'
    if ($LASTEXITCODE -ne 0) { throw "Could not create kernel service (sc.exe exit $LASTEXITCODE)." }
} else {
    $serviceCim = Get-CimInstance Win32_SystemDriver -Filter "Name='$serviceName'"
    if ($serviceCim.PathName.Trim('"') -ne $driverPath) {
        throw "Service '$serviceName' exists with a different driver path. Remove or inspect it before continuing."
    }
}

if ((Get-Service -Name $serviceName).Status -eq 'Running') {
    Write-Host "Driver service '$serviceName' is already running."
    return
}
& "$env:WINDIR\System32\sc.exe" start $serviceName
if ($LASTEXITCODE -ne 0) { throw "Could not start kernel driver (sc.exe exit $LASTEXITCODE). Check test-signing, certificate trust, and Code Integrity logs." }
Write-Host "Installed and started $serviceName for detected GPU $($gpu.InstanceId)."

