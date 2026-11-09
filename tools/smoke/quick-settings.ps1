# quick-settings.ps1 (round 13 phase 13E): a widget's quick settings (13C) through the automation.
#
#   pwsh -File tools\smoke\quick-settings.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config, quits the user's dock, starts -Exe with a test config: one bottom dock
# with a clock item (showSeconds false, showDate true). Asserts QueryState 325 of the clock's slot
# (slot 0) is 2 (Show seconds, Show date), applies quick entry 0 with ItemCommand 866 (entry in the
# high word of wParam) and waits for the debounced save: the clock item's showSeconds is true, its
# showDate unchanged. Undo (DockCommand 157) must put showSeconds back to false in the saved file.
# Always (finally) quits the test dock, restores the config byte for byte and restarts the user's dock.
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

$configPath = Join-Path $env:LOCALAPPDATA 'WinDock\config.json'
function Get-SavedClock {
    # The saved clock item's settings (a hashtable), or $null while the file is being rewritten.
    try {
        $json = Get-Content -Raw $configPath | ConvertFrom-Json -AsHashtable
        $item = @(@($json['docks'])[0]['items'] | Where-Object { $_['kind'] -eq 'clock' })[0]
        $item['settings']
    } catch { $null }
}

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
            items = @(@{ kind = 'clock'; settings = @{ showSeconds = $false; showDate = $true } })
        })
    }
    [void](Start-WinDock -Exe $Exe -Config $cfg)

    $n = Get-DockState 325 -Arg 0
    Check "QueryState 325 of the clock = 2 (got $n)" ($n -eq 2)

    $r = Send-ItemCommand -Command (866 -bor (0 -shl 16)) -Item 0
    Check "ItemCommand 866 entry 0 done (got $r)" ($r -eq 1)
    $on = Wait-Until -TimeoutMs 5000 { $s = Get-SavedClock; $s -and $s['showSeconds'] -eq $true }
    Check 'saved clock showSeconds true after the quick entry' $on
    $s = Get-SavedClock
    Check 'saved clock showDate unchanged (true)' ($s -and $s['showDate'] -eq $true)

    $u = Send-DockCommand -Command 157
    Check "Undo (DockCommand 157) done (got $u)" ($u -eq 1)
    $off = Wait-Until -TimeoutMs 5000 { $s = Get-SavedClock; $s -and $s['showSeconds'] -eq $false }
    Check 'saved clock showSeconds false again after Undo' $off
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
