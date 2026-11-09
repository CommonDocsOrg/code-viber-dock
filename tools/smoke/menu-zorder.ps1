# menu-zorder.ps1 (round 13 phase 13A): a menu owned by the dock stays above the dock.
#
#   pwsh -File tools\smoke\menu-zorder.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config, quits the user's dock, starts -Exe with a test config (one bottom dock,
# full width, a clock, autoHide off, no tray takeover, taskbar not hidden), opens the dock menu with a
# POSTED right click at the bar's background (the menu is modal: a sent message would not return),
# waits for QueryState 324 = 1, opens the "Add" submenu with posted keys, moves the pointer across
# the dock (the hover change that made the dock lay out and raise itself above the open menu), then
# asserts: both menu windows are above the dock window in the z-order, and WindowFromPoint inside the
# submenu's first entry is a #32768. Closes the menu (Escape), and always (finally) quits the test
# dock, restores the config byte for byte and restarts the user's dock.
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

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
$saved = New-Object WinDockSmoke.Native+POINT
[void][WinDockSmoke.Native]::GetCursorPos([ref]$saved)
$mainMenu = $null
try {
    Stop-WinDock
    $cfg = @{
        hideTaskbar = $false
        tray        = @{ enabled = $false }
        docks       = @(@{ id = 'smoke'; edge = 'bottom'; align = 'center'; fullWidth = $true; autoHide = 'off'
                           items = @(@{ kind = 'clock' }) })
    }
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    $dock = Get-DockWindow 0
    Check 'dock window found (QueryState 51)' ($dock -ne [IntPtr]::Zero)
    Check 'QueryState 324 is 0 before the menu' ((Get-DockState 324) -eq 0)

    $bar = Get-DockBarRect 0
    $pt = [pscustomobject]@{ X = $bar.Left + 20; Y = [int](($bar.Top + $bar.Bottom) / 2) }
    $cls = Get-ClassAtPoint $pt.X $pt.Y
    Write-Host "bar $($bar.Left),$($bar.Top)-$($bar.Right),$($bar.Bottom); at the press point: $cls"
    if ($cls -ne 'WinDock.DockWindow') {
        throw "the bar's background is not under the point ($cls): locked session or covered; skipping the menu"
    }

    # The posted right click: cursor there first (the dock reads the message position).
    [void][WinDockSmoke.Native]::SetCursorPos($pt.X, $pt.Y)
    Start-Sleep -Milliseconds 300
    $c = New-Object WinDockSmoke.Native+POINT
    $c.X = $pt.X; $c.Y = $pt.Y
    [void][WinDockSmoke.Native]::ScreenToClient($dock, [ref]$c)
    $lp = [IntPtr](([int64]($c.Y -band 0xFFFF) -shl 16) -bor ($c.X -band 0xFFFF))
    [void][WinDockSmoke.Native]::PostMessageW($dock, 0x0204, [IntPtr]2, $lp)  # WM_RBUTTONDOWN, MK_RBUTTON
    [void][WinDockSmoke.Native]::PostMessageW($dock, 0x0205, [IntPtr]0, $lp)  # WM_RBUTTONUP

    Check 'QueryState 324 becomes 1 while the menu is open' (Wait-Until -TimeoutMs 4000 { (Get-DockState 324) -eq 1 })
    Check 'a #32768 of the dock process is shown' (Wait-Until -TimeoutMs 2000 { @(Get-WinDockMenus).Count -ge 1 })
    $mainMenu = @(Get-WinDockMenus)[0]
    $entries = @(Get-MenuEntries $mainMenu.Menu)
    Write-Host ('menu: ' + (($entries | ForEach-Object { if ($_.Text) { $_.Text } else { '-' } }) -join ' | '))
    $add = $entries | Where-Object { $_.Text -eq 'Add' -and $_.Popup } | Select-Object -First 1
    Check 'the dock menu has an "Add" submenu' ($null -ne $add)

    if ($add) {
        # Down until "Add" is highlighted, then Right opens it.
        for ($i = 0; $i -lt 40 -and -not (Get-MenuEntries $mainMenu.Menu)[$add.Pos].Hilite; $i++) {
            Send-MenuKey $mainMenu.Hwnd 0x28  # VK_DOWN
            Start-Sleep -Milliseconds 60
        }
        Check '"Add" highlighted' ((Get-MenuEntries $mainMenu.Menu)[$add.Pos].Hilite)
        Send-MenuKey $mainMenu.Hwnd 0x27  # VK_RIGHT
        Check 'the Add submenu opened' (Wait-Until -TimeoutMs 2000 { @(Get-WinDockMenus).Count -ge 2 })

        # The pointer crosses the dock (hover / magnification change: the dock lays out).
        for ($x = $bar.Left + 20; $x -lt [Math]::Min($bar.Right, $bar.Left + 1200); $x += 60) {
            [void][WinDockSmoke.Native]::SetCursorPos($x, $pt.Y)
            Start-Sleep -Milliseconds 25
        }
        $cp = Get-DockItemPoint 0
        if ($cp) { [void][WinDockSmoke.Native]::SetCursorPos($cp.X, $cp.Y) }
        Start-Sleep -Milliseconds 800

        $menus = @(Get-WinDockMenus)
        $zDock = Get-ZIndex $dock
        foreach ($m in $menus) {
            $z = Get-ZIndex $m.Hwnd
            Check ("menu {0} (z {1}) above the dock (z {2})" -f $m.Hwnd, $z, $zDock) ($z -ge 0 -and $z -lt $zDock)
        }
        $sub = $menus | Where-Object { $_.Hwnd -ne $mainMenu.Hwnd } | Select-Object -First 1
        if ($sub) {
            $r = New-Object WinDockSmoke.Native+RECT
            $got = [WinDockSmoke.Native]::GetMenuItemRect([IntPtr]::Zero, $sub.Menu, 0, [ref]$r)
            if (-not $got) { $r = $sub.Rect; $r.Top += 4; $r.Bottom = $r.Top + 16 }
            $sx = [int](($r.Left + $r.Right) / 2); $sy = [int](($r.Top + $r.Bottom) / 2)
            $at = Get-ClassAtPoint $sx $sy
            Check ("WindowFromPoint in the submenu's first entry ($sx,$sy) is #32768 (got $at)") ($at -eq '#32768')
            Write-Host ("submenu rect {0},{1}-{2},{3}; dock bar top {4}" -f $sub.Rect.Left, $sub.Rect.Top, $sub.Rect.Right, $sub.Rect.Bottom, $bar.Top)
        }
        # Where the main menu overlaps the dock window (if it does), the menu must be the one seen.
        $dr = New-Object WinDockSmoke.Native+RECT
        [void][WinDockSmoke.Native]::GetWindowRect($dock, [ref]$dr)
        $mr = $mainMenu.Rect
        if ($mr.Bottom -gt $dr.Top + 2 -and $mr.Top -lt $dr.Bottom) {
            $oy = [int](([Math]::Max($mr.Top, $dr.Top) + [Math]::Min($mr.Bottom, $dr.Bottom)) / 2)
            $ox = [int](($mr.Left + $mr.Right) / 2)
            $at = Get-ClassAtPoint $ox $oy
            Check "main menu over the dock window at ($ox,$oy) shows #32768 (got $at)" ($at -eq '#32768')
        } else {
            Write-Host 'the main menu does not overlap the dock window (z-order check only)'
        }
    }

    # Close: Escape closes the submenu, the second one the menu.
    foreach ($k in 1..3) {
        $open = @(Get-WinDockMenus)
        if ($open.Count -eq 0) { break }
        Send-MenuKey $open[-1].Hwnd 0x1B
        Start-Sleep -Milliseconds 200
    }
    Check 'QueryState 324 is back to 0 after Escape' (Wait-Until -TimeoutMs 3000 { (Get-DockState 324) -eq 0 })
} catch {
    $fails.Add("error: $($_.Exception.Message)")
    Write-Host "ERROR: $($_.Exception.Message)"
} finally {
    try {
        foreach ($m in @(Get-WinDockMenus)) { Send-MenuKey $m.Hwnd 0x1B }
    } catch { }
    try { Stop-WinDock } catch { $fails.Add("stop: $($_.Exception.Message)"); Write-Host "STOP: $($_.Exception.Message)" }
    if ($bak) { Restore-WinDockConfig $bak; Write-Host 'config restored' }
    [void][WinDockSmoke.Native]::SetCursorPos($saved.X, $saved.Y)
    if ($userRunning -and -not $NoRestart) {
        [void](Start-WinDock -Exe $UserExe)
        Write-Host "user's dock restarted from $UserExe"
    }
}
if ($fails.Count) { Write-Host "$($fails.Count) failed"; exit 1 }
Write-Host 'all passed'
exit 0
