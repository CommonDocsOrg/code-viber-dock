# idle-cost.ps1 (round 26 phase 26A): nothing draws or wakes for a dock nobody can see.
#
#   pwsh -File tools\smoke\idle-cost.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config and quits the user's dock. One test dock (bottom, autoHide "always",
# hideDelayMs 300) with the built-in clock showing seconds (slot 0) and the Digital clock script
# widget showing seconds (slot 1). The pointer is parked in the middle of the screen (put back at
# the end) so the dock hides and does not reveal itself.
#   1. QueryState 358 (the session paused) = 0.
#   2. Hidden (QueryState 0 = 2): 361 (content paused) = 1; over 3.5 s the widget's draws
#      (QueryState 58 of slot 1) do not change and the widget timer wake-ups (59) rise by at most 1.
#   3. DockCommand 108 (auto-hide off): shown (0 = 0), 361 = 0; the widget draws once within 1.5 s
#      (the catch-up) and at least twice more over the next 3.5 s (one per second).
#   4. The foreground watch's counters (359 evaluations, 360 skipped) are printed, not asserted.
# Always (finally) quits the test dock, restores the config byte for byte and restarts the user's
# dock.
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

Add-Type -AssemblyName System.Windows.Forms
$cursor = [WinDockSmoke.Native+POINT]::new()
[void][WinDockSmoke.Native]::GetCursorPos([ref]$cursor)
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    [void][WinDockSmoke.Native]::SetCursorPos($screen.Left + $screen.Width / 2, $screen.Top + $screen.Height / 2)
    $cfg = @{
        hideTaskbar = $false
        tray        = @{ enabled = $false }
        docks       = @(@{ id = 'smoke'; edge = 'bottom'; align = 'center'; autoHide = 'always'; hideDelayMs = 300
                           items = @(@{ kind = 'clock'; settings = @{ showSeconds = $true } },
                                     @{ kind = 'widget'; id = 'digital-clock'; settings = @{ showSeconds = $true } }) })
    }
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    Check 'slot 1 is a script widget (QueryState 24 = 6)' ((Get-DockState 24 -Arg 1) -eq 6)
    Check 'the widget drew (QueryState 58 > 0)' (Wait-Until -TimeoutMs 4000 { (Get-DockState 58 -Arg 1) -gt 0 })
    $paused = Get-DockState 358
    Check "QueryState 358 = 0 (the session is not paused; got $paused)" ($paused -eq 0)

    # 2. Hidden: nothing draws, nothing wakes.
    Check 'the dock hid (QueryState 0 = 2)' (Wait-Until -TimeoutMs 5000 { (Get-DockState 0) -eq 2 })
    Start-Sleep -Milliseconds 300  # the hide's last frame
    $content = Get-DockState 361
    Check "hidden: QueryState 361 = 1 (got $content)" ($content -eq 1)
    $d0 = Get-DockState 58 -Arg 1
    $w0 = Get-DockState 59
    Start-Sleep -Milliseconds 3500
    $d1 = Get-DockState 58 -Arg 1
    $w1 = Get-DockState 59
    Write-Host "hidden 3.5 s: draws $d0 -> $d1, wake-ups $w0 -> $w1"
    Check "hidden: the widget's draws stay flat ($d0 -> $d1)" ($d1 -eq $d0)
    Check "hidden: widget timer wake-ups rise by at most 1 ($w0 -> $w1)" (($w1 - $w0) -le 1)

    # 3. Shown again: one catch-up draw, then one a second.
    [void](Send-DockCommand 108)  # auto-hide off
    Check 'the dock is shown (QueryState 0 = 0)' (Wait-Until -TimeoutMs 3000 { (Get-DockState 0) -eq 0 })
    $content = Get-DockState 361
    Check "shown: QueryState 361 = 0 (got $content)" ($content -eq 0)
    Check 'shown: the widget drew at once (the catch-up)' (Wait-Until -TimeoutMs 1500 { (Get-DockState 58 -Arg 1) -gt $d1 })
    $d2 = Get-DockState 58 -Arg 1
    Start-Sleep -Milliseconds 3500
    $d3 = Get-DockState 58 -Arg 1
    Write-Host "shown 3.5 s: draws $d2 -> $d3"
    Check "shown: the widget draws every second ($d2 -> $d3)" (($d3 - $d2) -ge 2)

    Write-Host ("foreground watch: {0} evaluations, {1} skipped (QueryState 359 / 360)" -f (Get-DockState 359), (Get-DockState 360))
} catch {
    $fails.Add("error: $($_.Exception.Message)")
    Write-Host "ERROR: $($_.Exception.Message)"
} finally {
    try { Stop-WinDock } catch { $fails.Add("stop: $($_.Exception.Message)"); Write-Host "STOP: $($_.Exception.Message)" }
    if ($bak) { Restore-WinDockConfig $bak; Write-Host 'config restored' }
    [void][WinDockSmoke.Native]::SetCursorPos($cursor.X, $cursor.Y)
    if ($userRunning -and -not $NoRestart) {
        [void](Start-WinDock -Exe $UserExe)
        Write-Host "user's dock restarted from $UserExe"
    }
}
if ($fails.Count) { Write-Host "$($fails.Count) failed"; exit 1 }
Write-Host 'all passed'
exit 0
