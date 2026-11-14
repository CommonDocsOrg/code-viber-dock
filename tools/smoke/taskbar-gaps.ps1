# taskbar-gaps.ps1 (round 27 phases 27B / 27C): the Network and Input language widgets and their providers.
#
#   pwsh -File tools\smoke\taskbar-gaps.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config and quits the user's dock. One test config: a bottom dock (auto-hide
# off) with the Network widget (slot 0) and the Input language widget (slot 1, hideSingle off, so it
# takes room even with one layout). Checks: both load without an error (QueryState 55 = 2, 54 = 0,
# 58 > 0 for each); each provider runs and is not quiet while its widget is on the visible dock
# (367 / 369 = 1, 368 / 370 = 1 user); removing the Input language widget (ItemCommand 861 on slot
# 1) stops the input provider (369 = 0, 370 = 0) and leaves the network one running; removing the
# Network widget stops it (367 = 0). It reads the network and the keyboard layout and changes
# neither (no click, no key). Always (finally) quits the test dock, restores the config byte for
# byte and restarts the user's dock.
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
        @{ kind = 'widget'; id = 'network'; settings = @{ showLabel = $true } },
        @{ kind = 'widget'; id = 'input-language'; settings = @{ hideSingle = $false; showName = $true } }
    ) }
) }

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    [void](Wait-Until -TimeoutMs 10000 { (Get-DockState 58 -Arg 1) -gt 0 })
    Check "two script widgets (55 = $(Get-DockState 55))" ((Get-DockState 55) -eq 2)
    foreach ($i in 0, 1) {
        $state = Get-DockState 54 -Arg $i
        $draws = Get-DockState 58 -Arg $i
        Check "widget ${i}: state $state (0 = running), draws $draws" ($state -eq 0 -and $draws -gt 0)
    }
    # The providers start with the widgets' first get() (their scripts' top level).
    [void](Wait-Until -TimeoutMs 3000 { (Get-DockState 367) -ne 0 -and (Get-DockState 369) -ne 0 })
    Check "network provider runs, not quiet (367 = $(Get-DockState 367))" ((Get-DockState 367) -eq 1)
    Check "network provider has one user (368 = $(Get-DockState 368))" ((Get-DockState 368) -eq 1)
    Check "input provider runs, not quiet (369 = $(Get-DockState 369))" ((Get-DockState 369) -eq 1)
    Check "input provider has one user (370 = $(Get-DockState 370))" ((Get-DockState 370) -eq 1)

    # Remove the Input language widget: the input provider stops, the network one keeps running.
    [void](Send-ItemCommand -Command 861 -Item 1)
    $stopped = Wait-Until -TimeoutMs 3000 { (Get-DockState 369) -eq 0 }
    Check "input provider stopped after its widget was removed (369 = $(Get-DockState 369), 370 = $(Get-DockState 370))" ($stopped -and (Get-DockState 370) -eq 0)
    Check "network provider still runs (367 = $(Get-DockState 367))" ((Get-DockState 367) -eq 1)

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
