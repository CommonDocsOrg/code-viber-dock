# fixed-size.ps1 (round 14 phase 14A): a bar of a fixed length and thickness through the automation.
#
#   pwsh -File tools\smoke\fixed-size.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config, quits the user's dock, starts -Exe with a test config: one bottom dock
# with a clock, "length": 600 and "thickness": 120. Asserts QueryState 328 (the resting bar's length |
# thickness << 16, px) is 600 x 120 DIPs at the dock's scale; DockCommand 184 with 800 makes it 800
# long and the saved file has "length": 800; 184 with 0 drops the key and the bar hugs the clock
# again (shorter than 800); 185 with 0 drops "thickness" and the bar is thinner than 120; 328 for a
# dock that does not exist is -2. Always (finally) quits the test dock, restores the config byte for
# byte and restarts the user's dock.
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
function Get-SavedDock {
    # The saved first dock (a hashtable), or $null while the file is being rewritten.
    try { @((Get-Content -Raw $configPath | ConvertFrom-Json -AsHashtable)['docks'])[0] } catch { $null }
}
function Get-BarSize {
    # QueryState 328 of dock 0 as [length, thickness] px.
    $v = Get-DockState 328
    , @([int]($v -band 0xFFFF), [int](($v -shr 16) -band 0xFFFF))
}
function Near([int]$Px, [double]$Want) { [math]::Abs($Px - [math]::Round($Want)) -le 1 }

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    $cfg = @{
        hideTaskbar = $false
        tray        = @{ enabled = $false }
        docks       = @(@{
            id = 'smoke'; edge = 'bottom'; align = 'center'; autoHide = 'off'; length = 600; thickness = 120
            items = @(@{ kind = 'clock' })
        })
    }
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    $s = Get-DockScale
    Write-Host "dock scale $s"

    $b = Get-BarSize
    Check "QueryState 328 length = 600 DIP (got $($b[0]) px)" (Near $b[0] (600 * $s))
    Check "QueryState 328 thickness = 120 DIP (got $($b[1]) px)" (Near $b[1] (120 * $s))

    $r = Send-DockCommand -Command 184 -Arg 800
    $ok = Wait-Until -TimeoutMs 5000 { $d = Get-SavedDock; $d -and $d['length'] -eq 800 }
    $b = Get-BarSize
    Check "DockCommand 184 800 done ($r), 328 length = 800 DIP (got $($b[0]) px), saved length 800" ($r -eq 1 -and (Near $b[0] (800 * $s)) -and $ok)

    $r = Send-DockCommand -Command 184 -Arg 0
    $ok = Wait-Until -TimeoutMs 5000 { $d = Get-SavedDock; $d -and -not $d.ContainsKey('length') }
    $b = Get-BarSize
    Check "DockCommand 184 0 done ($r), saved length gone, bar shorter than 800 DIP (got $($b[0]) px)" ($r -eq 1 -and $ok -and $b[0] -lt [math]::Round(800 * $s))

    $r = Send-DockCommand -Command 185 -Arg 0
    $ok = Wait-Until -TimeoutMs 5000 { $d = Get-SavedDock; $d -and -not $d.ContainsKey('thickness') }
    $b = Get-BarSize
    Check "DockCommand 185 0 done ($r), saved thickness gone, bar thinner than 120 DIP (got $($b[1]) px)" ($r -eq 1 -and $ok -and $b[1] -lt [math]::Round(120 * $s))

    $n = Get-DockState 328 -Dock 9
    Check "QueryState 328 of no such dock = -2 (got $n)" ($n -eq -2)
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
