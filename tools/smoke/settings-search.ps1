# settings-search.ps1 (round 18 phase 18D): the settings search dialog (Find a setting) opens at
# once and builds its index afterwards, one settings window per timer tick, so a config with
# many docks[] entries (each needs a hidden settings window for the index) no longer freezes the
# dock for seconds before the dialog appears; the hidden windows stay while a settings window of
# the user's is open, so the next search is instant.
#
#   pwsh -File tools\smoke\settings-search.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe] [-OutDir <folder>]
#
# Backs up the user's config, quits the user's dock, starts -Exe with a test config of one dock (a
# clock, reserveSpace off, no tray) and six standalone widgets (five of them clocks with a time zone
# field: a time zone list each), opens App settings, then asserts through the automation messages:
#   - DockCommand 151 (LOWORD 1: the App settings window's dialog): a WinDock.SettingsSearch window
#     is visible within 1.5 s of the command (the index is not built before it shows), QueryState
#     338 reports the index being built (0) or done (1), and 338 = 1 within 20 s; 283 (rows) > 0;
#     the status line (5013) says "N settings"; a PNG of it into -OutDir;
#   - every WinDock.Settings window but the App settings window is hidden (the index's);
#   - "clock" in the box (5010 + EN_CHANGE) leaves fewer rows, "zzzz" none, cleared: all again;
#   - Go to on a "dock smoke behaviour" row shows the Dock window of dock 0 (223 = 1) with its Contents tab
#     built late (278 > 0), the dialog stays and its index is rebuilt (338 = 1 again);
#   - Esc (IDCANCEL) closes the dialog (282 = 0) and the hidden windows stay (339 = 6: settings
#     windows are open);
#   - 151 again: the dialog is back and its index complete within 1.5 s (the windows exist);
#   - WM_CLOSE to every visible settings window: every WinDock.Settings window is gone within 60 s
#     (the hidden ones go with the last window of the user's, one per posted message; 339 = 0);
#   - settings and the search opened again (hidden windows alive), --quit ends the process in under
#     8 s (they are abandoned at shutdown, not destroyed: over 10 s before).
# Always (finally) quits the test dock, restores the config byte for byte and restarts the user's
# dock. Sends no keys or clicks to other windows (posted messages to the dock's own windows only).
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

function SearchDialog {
    $all = @(Get-WinDockWindowsOfClass 'WinDock.SettingsSearch')
    if ($all.Count -gt 0) { return $all[0] }
    [IntPtr]::Zero
}
function SettingsWindows { @(Get-WinDockWindowsOfClass 'WinDock.Settings' -All) }
function OpenSearch {
    # DockCommand 151 with LOWORD 1: the App settings window's dialog (posted: it activates the dialog).
    $sw = [Diagnostics.Stopwatch]::StartNew()
    [void](Send-DockCommand -Command 151 -Arg 1 -Post)
    $shown = Wait-Until -TimeoutMs 1500 { (Get-DockState 282 -Arg 1) -eq 1 -and (SearchDialog) -ne [IntPtr]::Zero }
    [pscustomobject]@{ Shown = $shown; Ms = $sw.ElapsedMilliseconds }
}
function Set-SearchText([IntPtr]$Dialog, [string]$Text) {
    $box = Get-ChildControl -Hwnd $Dialog -Id 5010
    [void](Set-ControlText -Hwnd $box -Text $Text)
    [void](Send-WindowMessage -Hwnd $Dialog -Msg 0x0111 -WParam ((0x0300 -shl 16) -bor 5010) -LParam $box)  # EN_CHANGE
    Get-DockState 283 -Arg 1
}

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    $docks = [Collections.ArrayList]::new()
    [void]$docks.Add(@{ id = 'smoke'; edge = 'bottom'; align = 'center'; autoHide = 'off'; items = @(@{ kind = 'clock' }) })
    for ($i = 1; $i -le 5; $i++) {
        [void]$docks.Add(@{ id = "clock$i"; kind = 'widget'; widget = 'analog-clock'; x = 40 + 120 * $i; y = 40 })
    }
    [void]$docks.Add(@{ id = 'notes'; kind = 'widget'; widget = 'notes'; x = 40; y = 240 })
    $cfg = @{ hideTaskbar = $false; tray = @{ enabled = $false }; docks = $docks.ToArray() }
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    Check 'seven docks[] entries (QueryState 47)' ((Get-DockState 47) -eq 7)

    [void](Send-DockCommand -Command 107 -Post)  # App settings
    Check 'the App settings window opened (QueryState 222)' (Wait-Until -TimeoutMs 6000 { (Get-DockState 222) -ge 1 })
    $settings = @(Get-WinDockWindowsOfClass 'WinDock.Settings')[0]
    Check 'no search dialog at first (282 = 0, 338 = -2)' ((Get-DockState 282 -Arg 1) -eq 0 -and (Get-DockState 338 -Arg 1) -eq -2)

    # Open it: the dialog shows before the index is built.
    $open = OpenSearch
    Check "the dialog is visible within 1.5 s of DockCommand 151 ($($open.Ms) ms)" $open.Shown
    $state = Get-DockState 338 -Arg 1
    Check "QueryState 338 answers 0 (building) or 1 (done) while it is open (got $state)" ($state -eq 0 -or $state -eq 1)
    # A picture while the index is being built: the controls must be painted (dark), not white.
    if ((SearchDialog) -ne [IntPtr]::Zero) {
        $png = Save-WindowImage -Hwnd (SearchDialog) -Path (Join-Path $OutDir 'settings-search-indexing.png') -DelayMs 100
        Write-Host "picture while indexing: $png"
    }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Check 'the index completes (338 = 1) within 20 s' (Wait-Until -TimeoutMs 20000 { (Get-DockState 338 -Arg 1) -eq 1 })
    Write-Host "  index built in about $($sw.ElapsedMilliseconds) ms after the dialog showed"
    $rows = Get-DockState 283 -Arg 1
    Check "rows indexed (283 = $rows > 100)" ($rows -gt 100)
    $dialog = SearchDialog
    $status = if ($dialog -ne [IntPtr]::Zero) { Get-ControlText (Get-ChildControl -Hwnd $dialog -Id 5013) } else { '' }
    Check "the status line says '$rows settings' (got '$status')" ($status -eq "$rows settings")
    $hidden = @(SettingsWindows | Where-Object { $_ -ne $settings })
    $hiddenAll = ($hidden.Count -ge 7) -and -not ($hidden | Where-Object { Test-WindowVisible $_ })
    Check "the index's settings windows exist and are hidden ($($hidden.Count) of them)" $hiddenAll
    if ($dialog -ne [IntPtr]::Zero) {
        $png = Save-WindowImage -Hwnd $dialog -Path (Join-Path $OutDir 'settings-search.png') -DelayMs 500
        Write-Host "picture: $png"
    }

    # The box filters.
    if ($dialog -ne [IntPtr]::Zero) {
        $fewer = Set-SearchText $dialog 'clock'
        Check "'clock' leaves fewer rows ($fewer of $rows)" ($fewer -ge 1 -and $fewer -lt $rows)
        Check "'zzzz' leaves none" ((Set-SearchText $dialog 'zzzz') -eq 0)
        Check 'cleared: every row again' ((Set-SearchText $dialog '') -eq $rows)
    }

    # Go to a row of the dock's window: the hidden Dock window is shown, with its Contents tab built
    # late (an index window gets it as an empty page; Activate builds it: QueryState 278 counts the
    # drawer's rows, -2 while there is no Dock window of that dock).
    if ($dialog -ne [IntPtr]::Zero) {
        # Every word must match: "dock smoke behaviour" matches the Where column of that window's
        # Behaviour tab only (the clocks' windows are "Widget clockN").
        $n = Set-SearchText $dialog 'dock smoke behaviour'
        Check "'dock smoke behaviour' finds rows ($n)" ($n -ge 1)
        Check 'no visible Dock window of dock 0 yet (QueryState 223 = 0)' ((Get-DockState 223) -eq 0)
        [void](Send-WindowMessage -Hwnd $dialog -Msg 0x0111 -WParam 5012 -LParam 0 -Post)  # Go to
        Check 'Go to shows the Dock window of dock 0 (223 = 1)' (Wait-Until -TimeoutMs 5000 { (Get-DockState 223) -eq 1 })
        $drawerRows = Get-DockState 278 -Dock 0 -Arg 1
        Check "its Contents tab was built when shown (278 built-in rows = $drawerRows > 0)" ($drawerRows -gt 0)
        Check 'the dialog is still open (282 = 1)' ((Get-DockState 282 -Arg 1) -eq 1)
        Check 'its index is complete again after Go to (338 = 1)' (Wait-Until -TimeoutMs 10000 { (Get-DockState 338 -Arg 1) -eq 1 })
        [void](Set-SearchText $dialog '')
        $rows = Get-DockState 283 -Arg 1
    }

    # Esc closes it; the hidden windows stay while a settings window is open.
    if ($dialog -ne [IntPtr]::Zero) { [void](Send-WindowMessage -Hwnd $dialog -Msg 0x0111 -WParam 2 -LParam 0 -Post) }  # IDCANCEL
    Check 'Esc (IDCANCEL) closes the dialog (282 = 0)' (Wait-Until -TimeoutMs 3000 { (Get-DockState 282 -Arg 1) -eq 0 })
    Start-Sleep -Milliseconds 300
    $kept = Get-DockState 339
    Check "the index's windows are kept while settings windows are open (339 = $kept of $($hidden.Count - 1) hidden)" ($kept -eq $hidden.Count - 1)
    Check 'no search dialog object remains (339 / 1 = 0)' ((Get-DockState 339 -Arg 1) -eq 0)

    # Opened again: instant, the index complete at once.
    $open = OpenSearch
    Check "opened again within 1.5 s ($($open.Ms) ms)" $open.Shown
    Check 'its index is complete within 1.5 s (the windows exist)' (Wait-Until -TimeoutMs 1500 { (Get-DockState 338 -Arg 1) -eq 1 })
    Check "the same rows ($rows)" ((Get-DockState 283 -Arg 1) -eq $rows)
    $dialog = SearchDialog
    if ($dialog -ne [IntPtr]::Zero) { [void](Send-WindowMessage -Hwnd $dialog -Msg 0x0111 -WParam 2 -LParam 0 -Post) }
    Check 'closed again (282 = 0)' (Wait-Until -TimeoutMs 3000 { (Get-DockState 282 -Arg 1) -eq 0 })

    # The last windows of the user's close (the App settings window and the Dock window Go to
    # showed): the hidden ones go with them, one per posted message (destroying a settings window
    # of hundreds of controls takes up to a few seconds on this machine: the dock keeps taking
    # messages between two). The wait polls with EnumWindows only (no message to the dock while it
    # is busy destroying).
    foreach ($h in @(Get-WinDockWindowsOfClass 'WinDock.Settings')) { [void](Send-WindowMessage -Hwnd $h -Msg 0x0010 -Post) }  # WM_CLOSE
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $gone = Wait-Until -TimeoutMs 60000 -StepMs 200 { @(SettingsWindows).Count -eq 0 }
    Check "closed: every WinDock.Settings window is gone within 60 s ($($sw.ElapsedMilliseconds) ms)" $gone
    Check 'closed: no App settings window (222 = 0), nothing kept (339 = 0)' ((Get-DockState 222) -eq 0 -and (Get-DockState 339) -eq 0)

    # Quitting with the hidden windows alive: they are abandoned, not destroyed (SettingsWindow::
    # AbandonForExit), so --quit takes seconds, not tens of seconds, and the taskbar comes back.
    [void](Send-DockCommand -Command 107 -Post)
    Check 'App settings again (222 = 1)' (Wait-Until -TimeoutMs 6000 { (Get-DockState 222) -ge 1 })
    $open = OpenSearch
    Check "the search again ($($open.Ms) ms)" $open.Shown
    Check 'its index complete (338 = 1)' (Wait-Until -TimeoutMs 20000 { (Get-DockState 338 -Arg 1) -eq 1 })
    Check "hidden windows alive (339 = $(Get-DockState 339))" ((Get-DockState 339) -ge 6)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Stop-WinDock -TimeoutMs 30000
    Check "--quit with them alive takes under 8 s ($($sw.ElapsedMilliseconds) ms; was over 10 s when they were destroyed)" ($sw.ElapsedMilliseconds -lt 8000)
} finally {
    try { Stop-WinDock } catch { Write-Warning $_ }
    Restore-WinDockConfig $bak
    if ($userRunning -and -not $NoRestart -and (Test-Path $UserExe)) { [void](Start-WinDock -Exe $UserExe) }
}
if ($fails.Count) { Write-Host "`n$($fails.Count) FAILED"; exit 1 }
Write-Host "`nall checks passed"
