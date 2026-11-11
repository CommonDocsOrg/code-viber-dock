# widget-to-desktop.ps1 (round 16 phase 16E): a script widget dragged off a dock becomes a standalone
# widget (the automation hook of the drag: ItemCommand 867).
#
#   pwsh -File tools\smoke\widget-to-desktop.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config, quits the user's dock, starts -Exe with a test config (one bottom dock
# holding a clock and a digital-clock widget with showSeconds on, autoHide off, no tray takeover,
# taskbar not hidden), then asserts:
#   - ItemCommand 867 on the native clock slot is refused (0) and changes nothing;
#   - ItemCommand 867 on the digital-clock slot returns 1, QueryState 47 (docks) goes 1 -> 2, entry 1
#     is a standalone widget (QueryState 241 = 2), the dock has no script widget left (QueryState 55)
#     and its slot 1 is not a script widget (QueryState 24 != 6);
#   - the saved config.json has the widget entry with the item's settings (showSeconds true).
# Always (finally) quits the test dock, restores the config byte for byte and restarts the user's dock.
# Sends no keys or clicks.
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
try {
    Stop-WinDock
    $cfg = @{
        hideTaskbar = $false
        tray        = @{ enabled = $false }
        docks       = @(@{ id = 'smoke'; edge = 'bottom'; align = 'center'; autoHide = 'off'
                           items = @(@{ kind = 'clock' },
                                     @{ kind = 'widget'; id = 'digital-clock'; settings = @{ showSeconds = $true } }) })
    }
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    Check 'dock window found (QueryState 51)' ((Get-DockWindow 0) -ne [IntPtr]::Zero)
    Check 'one docks[] entry (QueryState 47)' ((Get-DockState 47) -eq 1)
    Check 'slot 0 is the clock (QueryState 24 = 4)' ((Get-DockState 24 -Arg 0) -eq 4)
    Check 'slot 1 is a script widget (QueryState 24 = 6)' ((Get-DockState 24 -Arg 1) -eq 6)
    Check 'one script widget on the dock (QueryState 55)' (Wait-Until -TimeoutMs 4000 { (Get-DockState 55) -eq 1 })

    # The native clock cannot live on the desktop: refused, nothing changes (a beep).
    $r = Send-ItemCommand -Command 867 -Item 0
    Check "867 on the clock slot is refused (got $r)" ($r -eq 0)
    Check 'still one docks[] entry' ((Get-DockState 47) -eq 1)

    # The digital clock moves to the desktop.
    $r = Send-ItemCommand -Command 867 -Item 1
    Check "867 on the digital-clock slot is done (got $r)" ($r -eq 1)
    Check 'two docks[] entries (QueryState 47)' (Wait-Until -TimeoutMs 4000 { (Get-DockState 47) -eq 2 })
    Check 'entry 1 is a standalone widget (QueryState 241 = 2)' ((Get-DockState 241 -Dock 1) -eq 2)
    Check 'no script widget left on the dock (QueryState 55)' (Wait-Until -TimeoutMs 4000 { (Get-DockState 55) -eq 0 })
    Check 'slot 1 is no longer a script widget' ((Get-DockState 24 -Arg 1) -ne 6)
    Check 'its widget runs (QueryState 244 = 0)' (Wait-Until -TimeoutMs 4000 { (Get-DockState 244 -Dock 1) -eq 0 })

    # The saved file (debounced): the widget entry with the item's settings.
    $path = Join-Path $env:LOCALAPPDATA 'WinDock\config.json'
    $saved = $null
    $ok = Wait-Until -TimeoutMs 8000 {
        try {
            $j = Get-Content -Raw $path | ConvertFrom-Json -AsHashtable
            $w = @($j.docks | Where-Object { $_.kind -eq 'widget' })
            if ($w.Count -eq 1) { $script:saved = $w[0]; $true } else { $false }
        } catch { $false }
    }
    Check 'config.json has one kind "widget" entry' $ok
    if ($saved) {
        # A standalone entry saves its widget as "widget" (the id) and "settings" (README "Standalone widgets").
        Check "its widget is digital-clock (got $($saved.widget))" ($saved.widget -eq 'digital-clock')
        Check 'its settings carried over (showSeconds true)' ($saved.ContainsKey('settings') -and $saved.settings.showSeconds -eq $true)
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
