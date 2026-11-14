# taskbar-gaps.ps1 (round 27 phase 27B): the Network widget and its provider.
#
#   pwsh -File tools\smoke\taskbar-gaps.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config and quits the user's dock. One test config: a bottom dock (auto-hide
# off) with the Network widget (slot 0). Checks: it loads without an error (QueryState 55 = 1,
# 54 = 0, 58 > 0); the provider runs and is not quiet while the widget is on the visible dock
# (367 = 1, 368 = 1 user); removing the widget (ItemCommand 861 on slot 0) stops it (367 = 0). It
# reads the network and changes nothing (no click). Always (finally) quits the test dock, restores
# the config byte for byte and restarts the user's dock.
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

$cfg = @{ hideTaskbar = $false; tray = @{ enabled = $false }; docks = @(
    @{ id = 'smoke-gaps'; edge = 'bottom'; align = 'center'; autoHide = 'off'; items = @(
        @{ kind = 'widget'; id = 'network'; settings = @{ showLabel = $true } }
    ) }
) }

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    [void](Wait-Until -TimeoutMs 10000 { (Get-DockState 58 -Arg 0) -gt 0 })
    Check "one script widget (55 = $(Get-DockState 55))" ((Get-DockState 55) -eq 1)
    $state = Get-DockState 54 -Arg 0
    $draws = Get-DockState 58 -Arg 0
    Check "network widget: state $state (0 = running), draws $draws" ($state -eq 0 -and $draws -gt 0)
    # The provider starts with the widget's first get() (its script's top level).
    [void](Wait-Until -TimeoutMs 3000 { (Get-DockState 367) -ne 0 })
    Check "network provider runs, not quiet (367 = $(Get-DockState 367))" ((Get-DockState 367) -eq 1)
    Check "network provider has one user (368 = $(Get-DockState 368))" ((Get-DockState 368) -eq 1)

    # Remove the Network widget: its provider stops.
    [void](Send-ItemCommand -Command 861 -Item 0)
    $stopped = Wait-Until -TimeoutMs 3000 { (Get-DockState 367) -eq 0 }
    Check "network provider stopped after its widget was removed (367 = $(Get-DockState 367), 368 = $(Get-DockState 368))" ($stopped -and (Get-DockState 368) -eq 0)
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
