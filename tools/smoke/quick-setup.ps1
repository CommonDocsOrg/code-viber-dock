# quick-setup.ps1 (round 13 follow-up): Quick setup's four steps through the automation, with a
# picture of each step for the visual check.
#
#   pwsh -File tools\smoke\quick-setup.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe] [-ShotDir <folder>]
#
# Backs up the user's config, quits the user's dock, starts -Exe with a test config (one bottom dock
# with a clock), opens Quick setup (DockCommand 180), adds a second dock (WM_COMMAND 5771 to the
# window) and walks the steps with Next (DockCommand 182), asserting QueryState 317 (the step; its
# LOWORD 1 the dock rows) on the way and saving quick-setup-step<n>.png of the window into -ShotDir
# (default: %TEMP%). Cancel (WM_COMMAND 5764) must close it (317 = -2) and put the config back: the
# second dock must be gone from the saved file. Always (finally) quits the test dock, restores the
# config byte for byte and restarts the user's dock.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\build\dev\WinDock.exe'),
    [string]$UserExe = (Join-Path $PSScriptRoot '..\..\build\x64-release\WinDock.exe'),
    [string]$ShotDir = $env:TEMP,
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WinDock.psm1') -Force

$fails = [Collections.Generic.List[string]]::new()
function Check([string]$What, [bool]$Ok) {
    Write-Host ("{0}  {1}" -f ($(if ($Ok) { 'PASS' } else { 'FAIL' })), $What)
    if (-not $Ok) { $fails.Add($What) }
}
$WM_COMMAND = 0x111

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

    $r = Send-DockCommand -Command 180
    Check "DockCommand 180 opened Quick setup (got $r)" ($r -eq 1)
    $ok = Wait-Until { @(Get-WinDockWindowsOfClass 'WinDock.QuickSetup').Count -gt 0 }
    Check 'a WinDock.QuickSetup window is visible' $ok
    $win = @(Get-WinDockWindowsOfClass 'WinDock.QuickSetup')[0]
    Check "step 0 (QueryState 317 = $(Get-DockState 317))" ((Get-DockState 317) -eq 0)
    Check "one dock row (317 arg 1 = $(Get-DockState 317 -Arg 1))" ((Get-DockState 317 -Arg 1) -eq 1)

    [void](Send-WindowMessage -Hwnd $win -Msg $WM_COMMAND -WParam 5771)  # Add a dock
    Check "two dock rows after Add a dock (got $(Get-DockState 317 -Arg 1))" ((Get-DockState 317 -Arg 1) -eq 2)
    Write-Host ("shot: " + (Save-WindowImage -Hwnd $win -Path (Join-Path $ShotDir 'quick-setup-step1.png')))

    foreach ($step in 1..3) {
        $n = Send-DockCommand -Command 182
        Check "Next to step $step (got $n)" ($n -eq 1)
        Check "QueryState 317 = $step" ((Get-DockState 317) -eq $step)
        Write-Host ("shot: " + (Save-WindowImage -Hwnd $win -Path (Join-Path $ShotDir "quick-setup-step$($step + 1).png")))
    }
    $n = Send-DockCommand -Command 182
    Check "Next on the last step refused (got $n)" ($n -eq 0)

    [void](Send-WindowMessage -Hwnd $win -Msg $WM_COMMAND -WParam 5764)  # Cancel
    $closed = Wait-Until { (Get-DockState 317) -eq -2 }
    Check 'Cancel closed Quick setup (317 = -2)' $closed
    $configPath = Join-Path $env:LOCALAPPDATA 'WinDock\config.json'
    $back = Wait-Until -TimeoutMs 5000 {
        try { $j = Get-Content -Raw $configPath | ConvertFrom-Json -AsHashtable; @($j['docks']).Count -eq 1 } catch { $false }
    }
    Check 'the saved config has one dock again after Cancel' $back
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
