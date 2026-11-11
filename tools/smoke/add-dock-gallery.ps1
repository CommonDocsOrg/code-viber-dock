# add-dock-gallery.ps1 (round 18 phase 18A): App settings > Docks and displays' Add a dock card adds
# a dock from a layout tile as a Keep / Cancel preview, a second tile replaces the card's own preview,
# Cancel takes it away, Keep keeps it (one undo entry), another source's preview is not cancelled.
#
#   pwsh -File tools\smoke\add-dock-gallery.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe] [-OutDir <folder>]
#
# Backs up the user's config, quits the user's dock, starts -Exe with a test config of two docks (a
# bottom one and a top one, a clock each, reserveSpace off, no tray), opens App settings on Docks and
# displays (a PNG of it into -OutDir, default the temp folder), then asserts through the automation
# messages:
#   - the gallery has a tile per built-in layout at least (QueryState 333 >= 8), the hint and More ▾
#     exist, Copy my pinned apps is checked, the Display drop-down lists Primary ... Every display;
#   - DockCommand 187 tile 0 (Basic): docks[] grows by one, the bar is Keep / Cancel (QueryState 300
#     = 2), the new dock is "basic" and went to the LEFT edge (bottom and top are taken: the next free
#     edge), the gallery's selection is tile 0;
#   - 187 tile 2 (Modern) while that preview is up: still one more than before (the first preview was
#     cancelled), the new dock is "modern";
#   - DockCommand 161 (Cancel): back to two docks, no bar;
#   - 187 tile 1 (Minimal), then 160 (Keep): three docks, saved to config.json, the undo label is
#     "Added dock minimal from preset Minimal";
#   - a look preview from another source (DockCommand 168 on dock 0) up: 187 is refused (0), the dock
#     count stays, the hint says "Keep or cancel the current preview first"; 161 ends that preview.
# Tiles 4 (Windows 7: tray takeover, reserves space) and 5 - 7 are never added. Always (finally)
# quits the test dock, restores the config byte for byte and restarts the user's dock. Sends no
# keys or clicks to other windows (posted nav keys to the settings window only).
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

function DockName([int]$Index) {
    # QueryState 303 writes docks[] entry HIWORD's display name into the app window's static 5690.
    $n = Get-DockState 303 -Dock $Index
    if ($n -lt 0) { return $null }
    Get-ControlText (Get-ChildControl -Hwnd (Get-WinDockAppWindow) -Id 5690)
}
function UndoLabel { Get-ControlText (Get-ChildControl -Hwnd (Get-WinDockAppWindow) -Id 5610) }
function SavedDocks {
    $c = Get-Content -Raw (Join-Path $env:LOCALAPPDATA 'WinDock\config.json') | ConvertFrom-Json -AsHashtable
    @($c['docks']).Count
}

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    $cfg = @{
        hideTaskbar = $false
        tray        = @{ enabled = $false }
        docks       = @(
            @{ id = 'smoke'; edge = 'bottom'; align = 'center'; autoHide = 'off'; items = @(@{ kind = 'clock' }) },
            @{ id = 'two'; edge = 'top'; align = 'center'; autoHide = 'off'; items = @(@{ kind = 'clock' }) }
        )
    }
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    Check 'two docks[] entries (QueryState 47)' ((Get-DockState 47) -eq 2)

    [void](Send-DockCommand -Command 107 -Post)  # App settings
    Check 'the App settings window opened (QueryState 222)' (Wait-Until -TimeoutMs 6000 { (Get-DockState 222) -ge 1 })
    $settings = @(Get-WinDockWindowsOfClass 'WinDock.Settings')[0]
    Check 'Docks and displays shown (nav row 1)' (Show-GlobalPage -Hwnd $settings -Row 1)

    $tiles = Get-DockState 333
    Check "the gallery has the built-in layouts (QueryState 333 = $tiles)" ($tiles -ge 8)
    Check 'no ghost without a hover (QueryState 333 / 2 = -1)' ((Get-DockState 333 -Arg 2) -eq -1)
    $hint = Get-ChildControl -Hwnd $settings -Id 2818
    Check 'the hint exists (2818) and says what a click does' ($hint -ne [IntPtr]::Zero -and (Get-ControlText $hint) -like 'Click a layout*')
    $more = Get-ChildControl -Hwnd $settings -Id 2823
    Check 'More ▾ exists (2823)' ($more -ne [IntPtr]::Zero -and (Get-ControlText $more) -like 'More*')
    $pins = Get-ChildControl -Hwnd $settings -Id 2819
    Check 'Copy my pinned apps is checked (2819)' ($pins -ne [IntPtr]::Zero -and (Send-WindowMessage -Hwnd $pins -Msg 0x00F0) -eq 1)  # BM_GETCHECK
    $display = Get-ChildControl -Hwnd $settings -Id 2822
    $count = if ($display -ne [IntPtr]::Zero) { Send-WindowMessage -Hwnd $display -Msg 0x0146 } else { 0 }  # CB_GETCOUNT
    Check "the Display drop-down lists Primary ... Every display ($count entries)" ($count -ge 2)
    Check 'the Display drop-down is on Primary' ($display -ne [IntPtr]::Zero -and (Send-WindowMessage -Hwnd $display -Msg 0x0147) -eq 0)  # CB_GETCURSEL
    Check 'the old Add ▾ button is gone (2814)' ((Get-ChildControl -Hwnd $settings -Id 2814) -eq [IntPtr]::Zero)
    $png = Save-WindowImage -Hwnd $settings -Path (Join-Path $OutDir 'add-dock-gallery.png') -DelayMs 600
    Write-Host "picture: $png"

    # Tile 0 (Basic) as a preview: bottom and top are taken, so the left edge.
    Check 'DockCommand 187 tile 0 adds (1)' ((Send-DockCommand -Command 187 -Arg 0) -eq 1)
    Check 'three docks[] entries' ((Get-DockState 47) -eq 3)
    Check 'the bar is Keep / Cancel (QueryState 300 = 2)' ((Get-DockState 300) -eq 2)
    Check 'the new dock is "basic"' ((DockName 2) -eq 'basic')
    Check 'it went to the next free edge: left (QueryState 38 = 2)' ((Get-DockState 38 -Dock 2) -eq 2)
    Check 'the gallery selected tile 0 (QueryState 333 / 1)' ((Get-DockState 333 -Arg 1) -eq 0)

    # Tile 2 (Modern) replaces the card's own preview.
    Check 'DockCommand 187 tile 2 adds (1)' ((Send-DockCommand -Command 187 -Arg 2) -eq 1)
    Check 'still three docks[] entries (the first preview cancelled)' ((Get-DockState 47) -eq 3)
    Check 'the new dock is "modern" now' ((DockName 2) -eq 'modern')
    Check 'still a Keep / Cancel preview' ((Get-DockState 300) -eq 2)

    # Cancel.
    Check 'DockCommand 161 cancels (1)' ((Send-DockCommand -Command 161) -eq 1)
    Check 'back to two docks[] entries' ((Get-DockState 47) -eq 2)
    Check 'no bar (QueryState 300 = 0)' ((Get-DockState 300) -eq 0)

    # Keep: tile 1 (Minimal).
    Check 'DockCommand 187 tile 1 adds (1)' ((Send-DockCommand -Command 187 -Arg 1) -eq 1)
    Check 'DockCommand 160 keeps (1)' ((Send-DockCommand -Command 160) -eq 1)
    Check 'three docks[] entries after Keep' ((Get-DockState 47) -eq 3)
    Check 'saved: config.json has three docks' (Wait-Until -TimeoutMs 4000 { (SavedDocks) -eq 3 })
    $label = UndoLabel
    Check "the undo label is 'Added dock minimal from preset Minimal' (got '$label')" ($label -eq 'Added dock minimal from preset Minimal')

    # Another source's preview (a look on dock 0, DockCommand 168) is not the card's to cancel.
    Check 'a look preview starts (DockCommand 168)' ((Send-DockCommand -Command 168 -Arg 1) -eq 1 -and (Get-DockState 300) -eq 2)
    Check 'DockCommand 187 is refused while it is up (0)' ((Send-DockCommand -Command 187 -Arg 0) -eq 0)
    Check 'the dock count stays (3)' ((Get-DockState 47) -eq 3)
    Check 'the hint asks to keep or cancel first' ((Get-ControlText $hint) -like 'Keep or cancel the current preview first*')
    Check 'DockCommand 161 ends the look preview' ((Send-DockCommand -Command 161) -eq 1 -and (Get-DockState 300) -eq 0)

    # A click on the gallery (a POSTED WM_LBUTTONDOWN at tile 0's top-left corner, client px: no real
    # input) focuses it and adds Basic; with the keyboard focus its selection is the "hot" tile, so the
    # canvas draws the ghost and the hint describes the layout (the hover path, without the mouse).
    $gallery = Get-ChildControl -Hwnd $settings -Id 2817
    [void](Send-WindowMessage -Hwnd $gallery -Msg 0x0201 -WParam 1 -LParam ((20 -shl 16) -bor 20) -Post)
    Check 'a posted click on tile 0 adds a preview' (Wait-Until -TimeoutMs 3000 { (Get-DockState 47) -eq 4 -and (Get-DockState 300) -eq 2 })
    Check 'the canvas shows the ghost of tile 0 (QueryState 333 / 2 = 0)' (Wait-Until -TimeoutMs 2000 { (Get-DockState 333 -Arg 2) -eq 0 })
    Check 'the hint describes Basic dock' ((Get-ControlText $hint) -like 'Basic dock:*')
    $png2 = Save-WindowImage -Hwnd $settings -Path (Join-Path $OutDir 'add-dock-gallery-ghost.png') -DelayMs 400
    Write-Host "picture: $png2"
    Check 'DockCommand 161 cancels it' ((Send-DockCommand -Command 161) -eq 1 -and (Get-DockState 47) -eq 3)
} finally {
    try { Stop-WinDock } catch { Write-Warning $_ }
    Restore-WinDockConfig $bak
    if ($userRunning -and -not $NoRestart -and (Test-Path $UserExe)) { [void](Start-WinDock -Exe $UserExe) }
}
if ($fails.Count) { Write-Host "`n$($fails.Count) FAILED"; exit 1 }
Write-Host "`nall checks passed"
