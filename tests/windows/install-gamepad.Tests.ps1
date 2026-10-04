# Pester 3.4 syntax (ships with Windows PowerShell 5.1)
$script = Join-Path $PSScriptRoot "..\..\src_assets\windows\misc\gamepad\install-gamepad.ps1"
. $script

function New-FakeDriver([string]$version) {
    [pscustomobject]@{ VersionInfo = [pscustomobject]@{ FileVersion = $version } }
}

Describe "Test-ViGEmBusUpToDate" {
    It "accepts the bundled 1.21.442.0" {
        Mock Test-Path { $true }
        Mock Get-Item { New-FakeDriver "1.21.442.0" }
        Test-ViGEmBusUpToDate | Should Be $true
    }
    It "accepts exactly 1.17" {
        Mock Test-Path { $true }
        Mock Get-Item { New-FakeDriver "1.17.0.0" }
        Test-ViGEmBusUpToDate | Should Be $true
    }
    It "rejects 1.16" {
        Mock Test-Path { $true }
        Mock Get-Item { New-FakeDriver "1.16.116.0" }
        Test-ViGEmBusUpToDate | Should Be $false
    }
    It "rejects 1.9 (string comparison would wrongly accept it)" {
        Mock Test-Path { $true }
        Mock Get-Item { New-FakeDriver "1.9.0.0" }
        Test-ViGEmBusUpToDate | Should Be $false
    }
    It "handles FileVersion strings with a trailing description" {
        Mock Test-Path { $true }
        Mock Get-Item { New-FakeDriver "1.21.442.0 built by: WinDDK" }
        Test-ViGEmBusUpToDate | Should Be $true
    }
    It "returns false when the driver file is missing" {
        Mock Test-Path { $false }
        Test-ViGEmBusUpToDate | Should Be $false
    }
    It "returns false for an unparsable version" {
        Mock Test-Path { $true }
        Mock Get-Item { New-FakeDriver "garbage" }
        Test-ViGEmBusUpToDate | Should Be $false
    }
}

Describe "Install-ViGEmBus" {
    It "runs the installer quietly, without restart, and waits for it" {
        Mock Start-Process { [pscustomobject]@{ ExitCode = 0 } }
        Install-ViGEmBus -InstallerPath "C:\fake\vigembus_installer.exe" | Should Be 0
        Assert-MockCalled Start-Process -Times 1 -Exactly -ParameterFilter {
            $Wait -and $PassThru -and
            ($ArgumentList -contains "/quiet") -and
            ($ArgumentList -contains "/norestart") -and
            -not ($ArgumentList -contains "/promptrestart")
        }
    }
    It "returns the installer's exit code" {
        Mock Start-Process { [pscustomobject]@{ ExitCode = 1603 } }
        Install-ViGEmBus -InstallerPath "C:\fake\vigembus_installer.exe" | Should Be 1603
    }
}

Describe "Test-ViGEmBusInstallSucceeded" {
    It "treats 0 as success" { Test-ViGEmBusInstallSucceeded -ExitCode 0 | Should Be $true }
    It "treats 3010 (reboot required) as success" {
        Test-ViGEmBusInstallSucceeded -ExitCode 3010 | Should Be $true
    }
    It "treats 1641 (installed, restart initiated) as success" {
        Test-ViGEmBusInstallSucceeded -ExitCode 1641 | Should Be $true
    }
    It "treats 1638 (another version already installed) as success" {
        Test-ViGEmBusInstallSucceeded -ExitCode 1638 | Should Be $true
    }
    It "treats 1603 as failure" { Test-ViGEmBusInstallSucceeded -ExitCode 1603 | Should Be $false }
}

Describe "Dot-sourcing" {
    It "does not start any installer" {
        Mock Start-Process { throw "must not run" }
        { . $script } | Should Not Throw
    }
}
