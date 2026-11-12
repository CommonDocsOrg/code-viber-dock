# widget-picker.ps1 (round 18 phase 18B): App settings > Docks and displays > Add a dock >
# Standalone widget… (a button since 18C; More ▾'s entry in 18B) opens the standalone widget picker
# (customize mode's drawer popup with the script widgets only), a tile adds a standalone widget, the
# search filters, Escape closes it. Round 18 phase 18C: also that the App settings window is listed
# among the running apps (QueryState 336).
#
#   pwsh -File tools\smoke\widget-picker.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe] [-OutDir <folder>]
#
# Backs up the user's config, quits the user's dock, starts -Exe with a test config of one dock (a
# clock, reserveSpace off, no tray), opens App settings on Docks and displays, then asserts through
# the automation messages:
#   - WM_COMMAND(2812, 0) to the page opens the picker (QueryState 334 = 1) with the installed script
#     widgets as tiles (335 >= 5), a top-level WinDock.DrawerPopup titled "Add a standalone widget"
#     with no visible chips (a PNG of it into -OutDir);
#   - its search box (5707) filters the tiles ("clock": fewer, "zzzz": none), cleared: all again;
#   - ItemCommand 869 with the first script widget's drawer ref (7: Agenda here): docks[] grows by
#     one, the new entry is a standalone widget (QueryState 241 = 2), the picker is hidden, the undo
#     label says "Added <name> to the desktop", config.json has two entries;
#   - 869 while the picker is hidden is refused (0);
#   - opened again, a posted Escape to its search box closes it (334 = 0).
# Always (finally) quits the test dock, restores the config byte for byte and restarts the user's
# dock. Sends no keys or clicks to other windows (posted nav keys to the settings window and a posted
# Escape to the picker only).
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\build\dev\WinDock.exe'),
    [string]$UserExe = (Join-Path $PSScriptRoot '..\..\build\x64-release\WinDock.exe'),
    [string]$OutDir,
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WinDock.psm1') -Force

$fails = [Collections.Generic.List[string]]::new()
function Check([string]$What, [bool]$Ok) {
    Write-Host ("{0}  {1}" -f ($(if ($Ok) { 'PASS' } else { 'FAIL' })), $What)
    if (-not $Ok) { $fails.Add($What) }
}
if (-not $OutDir) { $OutDir = [IO.Path]::GetTempPath() }

function UndoLabel { Get-ControlText (Get-ChildControl -Hwnd (Get-WinDockAppWindow) -Id 5610) }
function SavedDocks {
    $c = Get-Content -Raw (Join-Path $env:LOCALAPPDATA 'WinDock\config.json') | ConvertFrom-Json -AsHashtable
    @($c['docks']).Count
}
function Picker {
    # The picker is the WinDock.DrawerPopup whose text is "Add a standalone widget" (customize mode's
    # popup is "Add to <dock>").
    foreach ($h in @(Get-WinDockWindowsOfClass 'WinDock.DrawerPopup')) {
        if ((Get-ControlText $h) -eq 'Add a standalone widget') { return $h }
    }
    [IntPtr]::Zero
}
function OpenPicker([IntPtr]$Settings) {
    # The page handles WM_COMMAND(2812, 0) (lParam 0: not from a control) as the Standalone widget…
    # button does (round 18 phase 18C: a button; 18B: More ▾'s entry); posted, since showing the
    # picker activates it.
    $page = Get-ParentWindow (Get-ChildControl -Hwnd $Settings -Id 2812)
    [void](Send-WindowMessage -Hwnd $page -Msg 0x0111 -WParam 2812 -LParam 0 -Post)
    Wait-Until -TimeoutMs 4000 { (Get-DockState 334) -eq 1 }
}

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    $cfg = @{
        hideTaskbar = $false
        tray        = @{ enabled = $false }
        docks       = @(@{ id = 'smoke'; edge = 'bottom'; align = 'center'; autoHide = 'off'; items = @(@{ kind = 'clock' }) })
    }
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    Check 'one docks[] entry (QueryState 47)' ((Get-DockState 47) -eq 1)
    Check 'the picker is not shown at first (QueryState 334 = 0)' ((Get-DockState 334) -eq 0)
    Check 'QueryState 335 is -1 while it is not shown' ((Get-DockState 335) -eq -1)

    [void](Send-DockCommand -Command 107 -Post)  # App settings
    Check 'the App settings window opened (QueryState 222)' (Wait-Until -TimeoutMs 6000 { (Get-DockState 222) -ge 1 })
    $settings = @(Get-WinDockWindowsOfClass 'WinDock.Settings')[0]
    Check 'Docks and displays shown (nav row 1)' (Show-GlobalPage -Hwnd $settings -Row 1)
    $button = Get-ChildControl -Hwnd $settings -Id 2812
    Check 'the Standalone widget… button exists (2812)' ($button -ne [IntPtr]::Zero -and (Get-ControlText $button) -like 'Standalone widget*')
    # Round 18 phase 18C: the App settings window is listed among the running apps (the tracker
    # coalesces shell events over 50 ms).
    Check 'the App settings window is among the running apps (QueryState 336 >= 1)' (Wait-Until -TimeoutMs 3000 { (Get-DockState 336) -ge 1 })

    # Open it.
    Check 'WM_COMMAND 2812 opens the picker (QueryState 334 = 1)' (OpenPicker $settings)
    $tiles = Get-DockState 335
    Check "the installed script widgets are its tiles (QueryState 335 = $tiles)" ($tiles -ge 5)
    $picker = Picker
    Check 'a WinDock.DrawerPopup titled "Add a standalone widget" is visible' ($picker -ne [IntPtr]::Zero)
    $chip = if ($picker -ne [IntPtr]::Zero) { Get-ChildControl -Hwnd $picker -Id 5708 } else { [IntPtr]::Zero }
    Check 'its All chip exists but is hidden (5708)' ($chip -ne [IntPtr]::Zero -and -not (Test-WindowVisible $chip))
    $grid = if ($picker -ne [IntPtr]::Zero) { Get-ChildControl -Hwnd $picker -Id 5712 } else { [IntPtr]::Zero }
    Check 'its grid exists and is shown (5712)' ($grid -ne [IntPtr]::Zero -and (Test-WindowVisible $grid))
    if ($picker -ne [IntPtr]::Zero) {
        $png = Save-WindowImage -Hwnd $picker -Path (Join-Path $OutDir 'widget-picker.png') -DelayMs 800
        Write-Host "picture: $png"
    }

    # The search box filters.
    $search = if ($picker -ne [IntPtr]::Zero) { Get-ChildControl -Hwnd $picker -Id 5707 } else { [IntPtr]::Zero }
    Check 'its search box exists (5707)' ($search -ne [IntPtr]::Zero)
    if ($search -ne [IntPtr]::Zero) {
        [void](Set-ControlText -Hwnd $search -Text 'clock')
        [void](Send-WindowMessage -Hwnd $picker -Msg 0x0111 -WParam ((0x0300 -shl 16) -bor 5707) -LParam $search)  # EN_CHANGE
        $fewer = Get-DockState 335
        Check "'clock' leaves fewer tiles ($fewer of $tiles)" ($fewer -ge 1 -and $fewer -lt $tiles)
        [void](Set-ControlText -Hwnd $search -Text 'zzzz')
        [void](Send-WindowMessage -Hwnd $picker -Msg 0x0111 -WParam ((0x0300 -shl 16) -bor 5707) -LParam $search)
        Check "'zzzz' leaves none" ((Get-DockState 335) -eq 0)
        [void](Set-ControlText -Hwnd $search -Text '')
        [void](Send-WindowMessage -Hwnd $picker -Msg 0x0111 -WParam ((0x0300 -shl 16) -bor 5707) -LParam $search)
        Check 'cleared: every tile again' ((Get-DockState 335) -eq $tiles)
    }

    # A tile: the first script widget that may be standalone (drawer refs: QueryState 230's
    # numbering, 7 + the catalog index; refs are tried from 7 until one is taken).
    $before = Get-DockState 47
    $added = $false
    $ref = -1
    for ($r = 7; $r -lt 7 + 40; $r++) {
        if ((Send-ItemCommand -Command 869 -Item $r) -eq 1) { $ref = $r; $added = $true; break }
    }
    Check "ItemCommand 869 added a widget (ref $ref)" $added
    Check 'docks[] grew by one' ((Get-DockState 47) -eq $before + 1)
    Check 'the new entry is a standalone widget (QueryState 241 = 2)' ((Get-DockState 241 -Dock $before) -eq 2)
    Check 'the picker hid itself (QueryState 334 = 0)' ((Get-DockState 334) -eq 0)
    $label = UndoLabel
    Check "the undo label is 'Added <name> to the desktop' (got '$label')" ($label -like 'Added * to the desktop')
    Check 'saved: config.json has two entries' (Wait-Until -TimeoutMs 4000 { (SavedDocks) -eq 2 })
    Check '869 while the picker is hidden is refused (0)' ((Send-ItemCommand -Command 869 -Item 7) -eq 0)
    Check 'the dock count stays' ((Get-DockState 47) -eq $before + 1)

    # Open again, Escape closes it.
    # (The new widget's panel took the foreground; the picker opens on top of it all the same.)
    Check 'opened again (334 = 1)' (OpenPicker $settings)
    $picker = Picker
    $search = if ($picker -ne [IntPtr]::Zero) { Get-ChildControl -Hwnd $picker -Id 5707 } else { [IntPtr]::Zero }
    if ($search -ne [IntPtr]::Zero) {
        # IDCANCEL as IsDialogMessage sends it for Escape (a posted WM_COMMAND to the popup).
        [void](Send-WindowMessage -Hwnd $picker -Msg 0x0111 -WParam 2 -LParam 0 -Post)
    }
    Check 'Escape (IDCANCEL) closes it (334 = 0)' (Wait-Until -TimeoutMs 3000 { (Get-DockState 334) -eq 0 })
    Check 'nothing was added by closing' ((Get-DockState 47) -eq $before + 1)

    # Round 18 phase 18C: closing the App settings window takes it out of the running apps (the new
    # widget's panel is a tool window and never counted).
    [void](Send-WindowMessage -Hwnd $settings -Msg 0x0010 -Post)  # WM_CLOSE
    Check 'closed: no WinDock window among the running apps (QueryState 336 = 0)' (Wait-Until -TimeoutMs 3000 { (Get-DockState 336) -eq 0 })
} finally {
    try { Stop-WinDock } catch { Write-Warning $_ }
    Restore-WinDockConfig $bak
    if ($userRunning -and -not $NoRestart -and (Test-Path $UserExe)) { [void](Start-WinDock -Exe $UserExe) }
}
if ($fails.Count) { Write-Host "`n$($fails.Count) FAILED"; exit 1 }
Write-Host "`nall checks passed"
