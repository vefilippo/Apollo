# Installs the bundled ViGEmBus driver silently, unless a compatible version (1.17+) is present.
# Dot-sourcing this script only defines the functions below (used by tests).

function Test-ViGEmBusUpToDate {
    param(
        [string]$DriverPath = "$env:SystemRoot\System32\drivers\ViGEmBus.sys",
        [System.Version]$MinimumVersion = [System.Version]"1.17"
    )
    if (-not (Test-Path $DriverPath)) {
        return $false
    }
    $rawVersion = (Get-Item $DriverPath).VersionInfo.FileVersion
    $installedVersion = $null
    if (-not [System.Version]::TryParse((("$rawVersion").Trim() -split '\s+')[0], [ref]$installedVersion)) {
        return $false
    }
    return $installedVersion -ge $MinimumVersion
}

function Install-ViGEmBus {
    param([Parameter(Mandatory)][string]$InstallerPath)
    $process = Start-Process -FilePath $InstallerPath -ArgumentList "/quiet", "/norestart" -Wait -PassThru
    return $process.ExitCode
}

function Test-ViGEmBusInstallSucceeded {
    param([Parameter(Mandatory)][int]$ExitCode)
    # 3010 = installed successfully, reboot required
    return $ExitCode -eq 0 -or $ExitCode -eq 3010
}

function Test-ViGEmBusRunning {
    $service = Get-Service -Name "ViGEmBus" -ErrorAction SilentlyContinue
    return $null -ne $service -and $service.Status -eq "Running"
}

if ($MyInvocation.InvocationName -eq '.') {
    return
}

if (Test-ViGEmBusUpToDate) {
    Write-Output "ViGEmBus 1.17 or later is already installed, skipping."
    exit 0
}

$installerPath = Join-Path $PSScriptRoot "vigembus_installer.exe"
Write-Output "Installing ViGEmBus from $installerPath"
$exitCode = Install-ViGEmBus -InstallerPath $installerPath
Write-Output "ViGEmBus installer exit code: $exitCode"

if (-not (Test-ViGEmBusInstallSucceeded -ExitCode $exitCode)) {
    Write-Output "ViGEmBus installation failed; gamepads will be unavailable until it is installed."
    exit $exitCode
}

if (Test-ViGEmBusRunning) {
    Write-Output "ViGEmBus service is running."
} else {
    Write-Output "ViGEmBus is installed; its service is not running yet (it starts on first use or after a reboot)."
}
exit 0
