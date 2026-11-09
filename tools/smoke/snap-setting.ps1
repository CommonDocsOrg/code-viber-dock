# snap-setting.ps1 (round 14 phase 14B): settings.snapToScreen through the automation.
#
#   pwsh -File tools\smoke\snap-setting.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config, quits the user's dock, starts -Exe with a test config (one bottom dock
# with a clock, no "settings" object). Asserts QueryState 329 (App: settings.snapToScreen) is 1 by
# default; DockCommand 186 with 0 makes it 0 and the saved file has "snapToScreen": false; 186 with 1
# makes it 1 again and the key is gone. A real drag (the snapping itself) is the user's manual check.
# Always (finally) quits the test dock, restores the config byte for byte and restarts the user's dock.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\build\dev\WinDock.exe'),
    [string]$UserExe = (Join-Path $PSScriptRoot '..\..\build\x64-release\WinDock.exe'),
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WinDock.psm1') -Force

$fails = [Collections.Generic.List[string]]::new()
function Check([string]$What, [bool]$Ok) {
    Write-Host ("{0}  {1}" -f ($(if ($Ok) { 'PASS' } else { 'FAIL' })), $What)
    if (-not $Ok) { $fails.Add($What) }
}

$configPath = Join-Path $env:LOCALAPPDATA 'WinDock\config.json'
function Get-SavedSettings {
    # The saved "settings" object (a hashtable; empty when there is none), or $null while the file is
    # being rewritten.
    try {
        $c = Get-Content -Raw $configPath | ConvertFrom-Json -AsHashtable
        if ($c.ContainsKey('settings')) { $c['settings'] } else { @{} }
    } catch { $null }
}

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    $cfg = @{
        hideTaskbar = $false
        tray        = @{ enabled = $false }
        docks       = @(@{
            id = 'smoke'; edge = 'bottom'; align = 'center'; autoHide = 'off'
            items = @(@{ kind = 'clock' })
        })
    }
    [void](Start-WinDock -Exe $Exe -Config $cfg)

    $v = Get-DockState 329
    Check "QueryState 329 = 1 by default (got $v)" ($v -eq 1)

    $r = Send-DockCommand -Command 186 -Arg 0
    $ok = Wait-Until -TimeoutMs 5000 { $s = Get-SavedSettings; $s -and $s.ContainsKey('snapToScreen') -and $s['snapToScreen'] -eq $false }
    $v = Get-DockState 329
    Check "DockCommand 186 0 done ($r), 329 = 0 (got $v), saved snapToScreen false" ($r -eq 1 -and $v -eq 0 -and $ok)

    $r = Send-DockCommand -Command 186 -Arg 1
    $ok = Wait-Until -TimeoutMs 5000 { $s = Get-SavedSettings; $null -ne $s -and -not $s.ContainsKey('snapToScreen') }
    $v = Get-DockState 329
    Check "DockCommand 186 1 done ($r), 329 = 1 (got $v), saved key gone" ($r -eq 1 -and $v -eq 1 -and $ok)
} catch {
    $fails.Add("error: $($_.Exception.Message)")
    Write-Host "ERROR: $($_.Exception.Message)"
} finally {
    try { Stop-WinDock } catch { $fails.Add("stop: $($_.Exception.Message)"); Write-Host "STOP: $($_.Exception.Message)" }
    if ($bak) { Restore-WinDockConfig $bak; Write-Host 'config restored' }
    if ($userRunning -and -not $NoRestart) {
        [void](Start-WinDock -Exe $UserExe)
        Write-Host "user's dock restarted from $UserExe"
    }
}
if ($fails.Count) { Write-Host "$($fails.Count) failed"; exit 1 }
Write-Host 'all passed'
exit 0
