# widget-to-dock.ps1 (round 16 phase 16G): a standalone widget goes back onto a dock (the automation
# hook of its menu's Move to dock > <dock> and of the drag onto a dock's bar: ItemCommand 868).
#
#   pwsh -File tools\smoke\widget-to-dock.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config, quits the user's dock, starts -Exe with a test config (one bottom dock
# holding a clock, autoHide off, plus one standalone digital-clock widget entry with showSeconds on;
# no tray takeover, taskbar not hidden), then asserts:
#   - ItemCommand 868 on the standalone entry with HIWORD(wParam) = 1 (the entry itself, not a dock)
#     is refused (0) and changes nothing;
#   - ItemCommand 868 with HIWORD(wParam) = 0 (the dock) returns 1 (posted), QueryState 47 (docks)
#     goes 2 -> 1, the dock has one script widget (QueryState 55) in its last slot (QueryState 24 = 6);
#   - the saved config.json has no kind "widget" entry left and the dock's items hold a digital-clock
#     widget item with the entry's settings (showSeconds true).
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
# ItemCommand 868 on the standalone entry (its index in the high word of lParam), the target config
# dock in the high word of wParam (Send-ItemCommand has no wParam argument).
function Send-ToDock([int]$Entry, [int]$Target) {
    Send-WinDockMessage -Name ItemCommand -WParam (868 + ([int64]$Target -shl 16)) -LParam (ConvertTo-LParam $Entry 0)
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
                           items = @(@{ kind = 'clock' }) },
                        @{ id = 'smoke-clock'; kind = 'widget'; widget = 'digital-clock'; x = 40; y = 40
                           settings = @{ showSeconds = $true } })
    }
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    Check 'dock window found (QueryState 51)' ((Get-DockWindow 0) -ne [IntPtr]::Zero)
    Check 'two docks[] entries (QueryState 47)' ((Get-DockState 47) -eq 2)
    Check 'entry 1 is a standalone widget (QueryState 241 = 2)' ((Get-DockState 241 -Dock 1) -eq 2)
    Check 'no script widget on the dock (QueryState 55)' ((Get-DockState 55) -eq 0)

    # Not a dock (the entry itself): refused, nothing changes (a beep).
    $r = Send-ToDock -Entry 1 -Target 1
    Check "868 to entry 1 (not a dock) is refused (got $r)" ($r -eq 0)
    Check 'still two docks[] entries' ((Get-DockState 47) -eq 2)

    # Onto the dock (posted: the entry goes in the app window's next message).
    $r = Send-ToDock -Entry 1 -Target 0
    Check "868 to dock 0 is done (got $r)" ($r -eq 1)
    Check 'one docks[] entry (QueryState 47)' (Wait-Until -TimeoutMs 4000 { (Get-DockState 47) -eq 1 })
    Check 'one script widget on the dock (QueryState 55)' (Wait-Until -TimeoutMs 4000 { (Get-DockState 55) -eq 1 })
    Check 'the last slot is a script widget (QueryState 24 = 6)' ((Get-DockState 24 -Arg 1) -eq 6)

    # The saved file (debounced): no standalone entry, the dock's widget item with the settings.
    $path = Join-Path $env:LOCALAPPDATA 'WinDock\config.json'
    $item = $null
    $ok = Wait-Until -TimeoutMs 8000 {
        try {
            $j = Get-Content -Raw $path | ConvertFrom-Json -AsHashtable
            $standalone = @($j.docks | Where-Object { $_.kind -eq 'widget' })
            $w = @($j.docks[0].items | Where-Object { $_.kind -eq 'widget' -and $_.id -eq 'digital-clock' })
            if ($standalone.Count -eq 0 -and $w.Count -eq 1) { $script:item = $w[0]; $true } else { $false }
        } catch { $false }
    }
    Check 'config.json: no standalone entry, one digital-clock item on the dock' $ok
    if ($item) {
        Check 'its settings carried over (showSeconds true)' ($item.ContainsKey('settings') -and $item.settings.showSeconds -eq $true)
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
