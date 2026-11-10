# start-near.ps1 (round 16 phase 16A): the Start widget opens the Start menu beside its button.
#
#   pwsh -File tools\smoke\start-near.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config, quits the user's dock, starts -Exe with WINDOCK_SENDKEYS_LOG set (the
# sendKeys dry run: nothing is pressed, Start never opens, the placer computes its shift but moves
# nothing) and a test config: one bottom dock moved 800 DIPs left of the centre (so the shift is not
# 0) holding only the Start widget. Checks: QueryState 331 = 1 (the Start host window is found; a 0
# is reported, not failed); ItemCommand 810 (a left click on the slot) writes a `start-near` line
# within 2 s; its dx = the slot's centre x less the natural panel's centre x (the placer's anchor
# model before any open: centred on the host, kGuessWDip wide; clamped to the monitor) within 2 px,
# and it equals QueryState 332; 331 still 1 and the host's rect unchanged (nothing moved); with
# `menuPosition: 'windows'` the line is plain `win`. The real open / close / restore is the user's
# manual check (STATUS.md "Round 16"). Always (finally) quits the test dock, checks the host is at
# the rect it had before, restores the config byte for byte and restarts the user's dock.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\build\dev\WinDock.exe'),
    [string]$UserExe = (Join-Path $PSScriptRoot '..\..\build\x64-release\WinDock.exe'),
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WinDock.psm1') -Force
Add-Type -AssemblyName System.Windows.Forms

$fails = [Collections.Generic.List[string]]::new()
function Check([string]$What, [bool]$Ok) {
    Write-Host ("{0}  {1}" -f ($(if ($Ok) { 'PASS' } else { 'FAIL' })), $What)
    if (-not $Ok) { $fails.Add($What) }
}

$log = Join-Path ([IO.Path]::GetTempPath()) 'windock-start-near.log'
function Read-LogLines { if (Test-Path $log) { @(Get-Content $log) } else { @() } }
function Start-TestDock([string]$MenuPosition) {
    Remove-Item $log -ErrorAction SilentlyContinue
    $cfg = @{
        hideTaskbar = $false
        tray        = @{ enabled = $false }
        docks       = @(@{
            id = 'smoke'; edge = 'bottom'; align = 'center'; offset = -800; autoHide = 'off'
            items = @(@{ kind = 'widget'; id = 'start'; settings = @{ menuPosition = $MenuPosition } })
        })
    }
    [void](Start-WinDock -Exe $Exe -Config $cfg -Environment @{ WINDOCK_SENDKEYS_LOG = $log })
}
function Click-Start {
    [void](Send-ItemCommand -Command 810 -Item 0)
    [void](Wait-Until -TimeoutMs 2000 { (Read-LogLines).Count -gt 0 })
    Read-LogLines | Select-Object -Last 1
}

$userRunning = @(Get-WinDockProcess).Count -gt 0
$hostWnd = Get-StartHostWindow
$hostBefore = if ($hostWnd -ne [IntPtr]::Zero) { Get-WindowRect $hostWnd } else { $null }
Write-Host ("Start host: {0} rect {1}" -f $hostWnd, ($(if ($hostBefore) { "$($hostBefore.Left),$($hostBefore.Top) - $($hostBefore.Right),$($hostBefore.Bottom)" } else { 'none' })))
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    Start-TestDock 'near'
    $s = Get-DockScale
    Write-Host "dock scale $s"

    $state = Get-DockState 331
    if ($state -eq 0) {
        Write-Host 'NOTE  QueryState 331 = 0: no Start host window on this machine; start-near is plain win here'
    } else {
        Check "QueryState 331 = 1 before the click (got $state)" ($state -eq 1)
    }

    $pt = Get-DockItemPoint -Item 0
    $line = Click-Start
    Write-Host "log: $line"
    $f = if ($line) { $line -split "`t" } else { @() }
    Check "a start-near line within 2 s (got '$line')" ($f.Count -ge 2 -and $f[0] -eq 'start-near' -and $f[1] -eq '5B')

    if ($state -ne 0 -and $f.Count -ge 3 -and $pt -and $hostBefore) {
        $d = $f[2] -split ' '
        $dx = [int]$d[0]; $dy = [int]$d[1]
        # The placer's anchor model (StartMenuPlacer.cpp kGuessWDip; the test dock never opened Start,
        # so no panel rect was read): centred on the host, then the target centred on the slot,
        # clamped into the slot's monitor with an 8 px margin.
        $w = [int][math]::Round(1138 * $s)
        $hostW = $hostBefore.Right - $hostBefore.Left
        $panelLeft = $hostBefore.Left + [math]::Truncate(($hostW - $w) / 2)
        $mon = [System.Windows.Forms.Screen]::FromPoint([System.Drawing.Point]::new($pt.X, $pt.Y)).Bounds
        $target = [math]::Min([math]::Max($pt.X - [math]::Truncate($w / 2), $mon.Left + 8), $mon.Right - 8 - $w)
        $want = $target - $panelLeft
        Write-Host "slot centre x $($pt.X), natural panel $panelLeft + $w, monitor $($mon.Left)..$($mon.Right): want dx $want"
        Check "dx = $want within 2 px (got $dx)" ([math]::Abs($dx - $want) -le 2)
        Check "dy is a small move upward or none for a bottom dock (got $dy)" ($dy -le 60 -and $dy -ge -300)
        $q = Split-Words (Get-DockState 332)
        Check "QueryState 332 = the logged shift (got $($q[0]) $($q[1]))" ($q[0] -eq $dx -and $q[1] -eq $dy)
    }

    $state2 = Get-DockState 331
    Check "QueryState 331 unchanged after the dry run (got $state2)" ($state2 -eq $state)
    if ($hostBefore) {
        $r = Get-WindowRect $hostWnd
        Check "the Start host's rect unchanged ($($r.Left),$($r.Top))" ($r.Left -eq $hostBefore.Left -and $r.Top -eq $hostBefore.Top -and $r.Right -eq $hostBefore.Right -and $r.Bottom -eq $hostBefore.Bottom)
    }

    Stop-WinDock
    Start-TestDock 'windows'
    $line = Click-Start
    Write-Host "log: $line"
    Check "menuPosition 'windows': the line is plain win (got '$line')" ($line -eq "win`t5B")
} catch {
    $fails.Add("error: $($_.Exception.Message)")
    Write-Host "ERROR: $($_.Exception.Message)"
} finally {
    try { Stop-WinDock } catch { $fails.Add("stop: $($_.Exception.Message)"); Write-Host "STOP: $($_.Exception.Message)" }
    if ($hostBefore -and $script:hostWnd -ne [IntPtr]::Zero) {
        $r = Get-WindowRect $hostWnd
        $same = $r.Left -eq $hostBefore.Left -and $r.Top -eq $hostBefore.Top
        Write-Host ("Start host at the end: {0},{1} - {2},{3} ({4})" -f $r.Left, $r.Top, $r.Right, $r.Bottom, ($(if ($same) { 'unchanged' } else { 'MOVED' })))
        if (-not $same) { $fails.Add('the Start host moved') }
    }
    Remove-Item $log -ErrorAction SilentlyContinue
    if ($bak) { Restore-WinDockConfig $bak; Write-Host 'config restored' }
    if ($userRunning -and -not $NoRestart) {
        [void](Start-WinDock -Exe $UserExe)
        Write-Host "user's dock restarted from $UserExe"
    }
}
if ($fails.Count) { Write-Host "$($fails.Count) failed"; exit 1 }
Write-Host 'all passed'
exit 0
