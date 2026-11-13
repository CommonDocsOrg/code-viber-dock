# tray-look.ps1 (round 24 phase 24C): the tray item's look (the grid, the arrow, the drawer).
#
#   pwsh -File tools\smoke\tray-look.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe] [-OutDir <folder>]
#
# NOT RUN by the agent that wrote it: it quits the user's dock and the test docks take over the
# notification area (Shell_TrayWnd) on the live desktop, which needs the user's go-ahead. Run it
# only when nothing depends on the tray for a minute.
#
# Backs up the user's config and quits the user's dock. Then one test config per variant below, in
# turn: two docks (0 bottom, 1 left, 72 DIPs thick), each holding only a tray item of 1 row x 2
# columns (so three or more registered icons overflow) with the variant's keys, and "tray":
# { "enabled": true }. For each dock it checks (README "Automation (for tests)", Tray):
#   - QueryState 99 (icons drawn) >= 0 and 343 / 344 (the arrow's place / look) as configured;
#   - 345 (the arrow shown) = 1 when there are more icons than the grid shows or with "always", and
#     equal to 101; 350 (the arrow's centre) is not -1 while it is shown;
#   - 347 (the icons' drawn px) > 0 while icons are drawn; 349 (labels beside) on the bottom dock;
#   - ItemCommand 870 opens the drawer the hover way: 100 = 1, 353 = 1, 351 (columns; 1 for the
#     drawer's labels beside), 352 (its icons' px, about drawerIconSize x the dock's scale), 354
#     (title 1 + gear 2; 0 with the title off), 108 (its icons; with "overflow" at most 98 - 99);
#     then the 0.4 s leave grace closes it (100 = 0, 355 = 0) unless the pointer happens to rest
#     on it (reported as a note, not a failure);
#   - default variant only: 870 again, then ItemCommand 871 presses the gear: the drawer closes and
#     a WinDock.Settings window opens (closed again with WM_CLOSE).
# A PNG of each dock (drawer closed) goes to -OutDir. What it looks like is the user's manual check
# (STATUS.md round 24). Always (finally) quits the test dock, restores the config byte for byte and
# restarts the user's dock.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\build\dev\WinDock.exe'),
    [string]$UserExe = (Join-Path $PSScriptRoot '..\..\build\x64-release\WinDock.exe'),
    [string]$OutDir = [IO.Path]::GetTempPath(),
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WinDock.psm1') -Force

$fails = [Collections.Generic.List[string]]::new()
function Check([string]$What, [bool]$Ok) {
    Write-Host ("{0}  {1}" -f ($(if ($Ok) { 'PASS' } else { 'FAIL' })), $What)
    if (-not $Ok) { $fails.Add($What) }
}

# name, the tray item's keys, and what the queries should report (place 343, look 344).
$variants = @(
    @{ name = 'default'; keys = @{}; place = 0; look = 0; gear = $true },
    @{ name = 'arrow start'; keys = @{ expander = 'start' }; place = 1; look = 0 },
    @{ name = 'strip before, count'; keys = @{ expander = 'before'; expanderStyle = 'count' }; place = 2; look = 2 },
    @{ name = 'labels beside'; keys = @{ labels = 'beside'; drawerLabels = 'beside'; iconSize = 28; spacing = 1 }; place = 0; look = 0 },
    @{ name = 'drawer overflow, labels none, title off'; keys = @{ drawerShows = 'overflow'; drawerLabels = 'none'; drawerTitle = $false; drawerIconSize = 24; expander = 'after'; expanderStyle = 'dots' }; place = 3; look = 1 },
    @{ name = 'hover open, always'; keys = @{ drawerOpen = 'hover'; expanderShow = 'always'; colour = 'mono'; drawerColumns = 3 }; place = 0; look = 0 }
)

function TrayItem([hashtable]$Keys) {
    $item = @{ kind = 'tray'; rows = 1; columns = 2 }
    foreach ($k in $Keys.Keys) { $item[$k] = $Keys[$k] }
    $item
}
function TestConfig([hashtable]$Keys) {
    @{ hideTaskbar = $false; tray = @{ enabled = $true }; docks = @(
        @{ id = 'smoke-bottom'; edge = 'bottom'; align = 'center'; autoHide = 'off'; thickness = 72; items = @(TrayItem $Keys) },
        @{ id = 'smoke-left'; edge = 'left'; align = 'center'; autoHide = 'off'; thickness = 72; items = @(TrayItem $Keys) }
    ) }
}

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    foreach ($v in $variants) {
        Write-Host "--- $($v.name)"
        [void](Start-WinDock -Exe $Exe -Config (TestConfig $v.keys))
        # The takeover makes the apps register again (TaskbarCreated): wait for the icons to settle.
        [void](Wait-Until -TimeoutMs 10000 { (Get-DockState 98) -gt 0 })
        Start-Sleep -Milliseconds 1500
        $registered = Get-DockState 98
        Check "$($v.name): takeover running (97 = $(Get-DockState 97))" ((Get-DockState 97) -eq 1)
        $k = $v.keys
        $always = $k.ContainsKey('expanderShow') -and $k.expanderShow -eq 'always'
        foreach ($dock in 0, 1) {
            $tag = "$($v.name) dock $dock"
            $drawn = Get-DockState 99 -Dock $dock -Arg 0
            Check "${tag}: tray slot 0 (99 = $drawn icons of $registered)" ($drawn -ge 0)
            Check "${tag}: arrow place 343 = $($v.place)" ((Get-DockState 343 -Dock $dock -Arg 0) -eq $v.place)
            Check "${tag}: arrow look 344 = $($v.look)" ((Get-DockState 344 -Dock $dock -Arg 0) -eq $v.look)
            $shown = Get-DockState 345 -Dock $dock -Arg 0
            Check "${tag}: 345 ($shown) = 101" ($shown -eq (Get-DockState 101 -Dock $dock -Arg 0))
            if ($always -or $registered -gt $drawn) { Check "${tag}: the arrow is shown" ($shown -eq 1) }
            if ($shown -eq 1) { Check "${tag}: the arrow has a centre (350)" ((Get-DockState 350 -Dock $dock -Arg 0) -ne -1) }
            if ($drawn -gt 0) {
                $px = Get-DockState 347 -Dock $dock -Arg 0
                Check "${tag}: icons drawn at $px px" ($px -gt 0)
            }
            if ($dock -eq 0 -and $k.ContainsKey('labels') -and $drawn -gt 0) {
                Check "${tag}: labels beside (349)" ((Get-DockState 349 -Dock $dock -Arg 0) -eq 1)
            }
            $png = Join-Path $OutDir ("tray-look-{0}-dock{1}.png" -f ($v.name -replace '[^a-z0-9]+', '-'), $dock)
            [void](Save-WindowImage -Hwnd (Get-DockWindow -Dock $dock) -Path $png)
            Write-Host "picture: $png"

            # The drawer, opened the hover way (ItemCommand 870; any drawerOpen).
            Check "${tag}: 870 opens the drawer" ((Send-ItemCommand -Command 870 -Item 0 -Dock $dock) -eq 1)
            [void](Wait-Until -TimeoutMs 2000 { (Get-DockState 100 -Dock $dock) -eq 1 })
            Check "${tag}: drawer open (100), by hover (353)" ((Get-DockState 100 -Dock $dock) -eq 1 -and (Get-DockState 353 -Dock $dock) -eq 1)
            $cols = Get-DockState 351 -Dock $dock
            if ($k.ContainsKey('drawerLabels') -and $k.drawerLabels -eq 'beside') { Check "${tag}: drawer list, 351 = 1 ($cols)" ($cols -eq 1) }
            elseif ($k.ContainsKey('drawerColumns')) { Check "${tag}: drawer columns $cols <= $($k.drawerColumns)" ($cols -ge 1 -and $cols -le $k.drawerColumns) }
            else { Check "${tag}: drawer columns $cols" ($cols -ge 1) }
            $size = if ($k.ContainsKey('drawerIconSize')) { $k.drawerIconSize } else { 40 }
            $want = [int][Math]::Round($size * (Get-DockScale -Dock $dock))
            $got = Get-DockState 352 -Dock $dock
            Check "${tag}: drawer icons $got px (about $want)" ([Math]::Abs($got - $want) -le 2)
            $title = Get-DockState 354 -Dock $dock
            $titleOn = -not ($k.ContainsKey('drawerTitle') -and -not $k.drawerTitle)
            Check "${tag}: title + gear 354 = $title" ($title -eq $(if ($titleOn) { 3 } else { 0 }))
            $listed = Get-DockState 108 -Dock $dock
            if ($k.ContainsKey('drawerShows')) { Check "${tag}: overflow drawer lists $listed <= $registered - $drawn" ($listed -le $registered - $drawn) }
            else { Check "${tag}: drawer lists $listed of $registered" ($listed -ge $drawn) }
            if (Wait-Until -TimeoutMs 3000 { (Get-DockState 100 -Dock $dock) -eq 0 }) {
                Check "${tag}: the leave grace closed it, no hover timers (355)" ((Get-DockState 355 -Dock $dock) -eq 0)
            } else {
                Write-Host "NOTE  ${tag}: the drawer stayed open (the pointer may rest on it); closing it"
                [void](Send-WindowMessage -Hwnd (Get-DockWindow -Dock $dock) -Msg 0x0100 -WParam 0x1B -Post)  # Escape
            }

            if ($v.gear) {
                [void](Send-ItemCommand -Command 870 -Item 0 -Dock $dock)
                [void](Wait-Until -TimeoutMs 2000 { (Get-DockState 100 -Dock $dock) -eq 1 })
                Check "${tag}: 871 presses the gear" ((Send-ItemCommand -Command 871 -Item 0 -Dock $dock) -eq 1)
                $opened = Wait-Until -TimeoutMs 5000 { @(Get-WinDockWindowsOfClass -Class 'WinDock.Settings').Count -gt 0 }
                Check "${tag}: the gear closed the drawer and opened the settings window" ($opened -and (Get-DockState 100 -Dock $dock) -eq 0)
                foreach ($w in @(Get-WinDockWindowsOfClass -Class 'WinDock.Settings')) { [void](Send-WindowMessage -Hwnd $w -Msg 0x0010 -Post) }  # WM_CLOSE
                [void](Wait-Until -TimeoutMs 5000 { @(Get-WinDockWindowsOfClass -Class 'WinDock.Settings').Count -eq 0 })
            }
        }
        Stop-WinDock
    }
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
