# running-widget.ps1 (round 13 phase 13D): the running apps' settings live in the running item.
#
#   pwsh -File tools\smoke\running-widget.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config, quits the user's dock, starts -Exe with a test config: one bottom dock
# with two pinned apps (mspaint / calc by path; nothing needs to run), a running item whose settings
# have gap 2 and padding 12, a clock, and the pre-13D dock-level keys windows / titles / runningFilter
# (the migration must move them into the running item). Asserts QueryState 326 = 2 | (12 << 16),
# then toggles Lock dock twice (DockCommand 116) to force a save, waits for the debounced save and
# reads config.json: no windows / titles / runningFilter at the dock level, the running item has
# gap 2, padding 12 and the moved windows "unstacked" / titles "one" / filter "monitor". Always
# (finally) quits the test dock, restores the config byte for byte and restarts the user's dock.
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
$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    $sys = $env:SystemRoot
    $cfg = @{
        hideTaskbar = $false
        tray        = @{ enabled = $false }
        docks       = @(@{
            id = 'smoke'; edge = 'bottom'; align = 'center'; autoHide = 'off'
            # pre-13D keys: moved into the running item on load (it has none of them)
            windows = 'unstacked'; titles = 'one'; runningFilter = 'monitor'
            items = @(
                @{ kind = 'app'; path = "$sys\System32\mspaint.exe" },
                @{ kind = 'app'; path = "$sys\System32\calc.exe" },
                @{ kind = 'running'; settings = @{ gap = 2; padding = 12 } },
                @{ kind = 'clock' }
            )
        })
    }
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    $v = Get-DockState 326
    Write-Host ("QueryState 326 = 0x{0:X8}" -f $v)
    Check 'QueryState 326 = gap 2 | padding 12 << 16' ($v -eq (2 -bor (12 -shl 16)))

    $before = (Get-Item $configPath).LastWriteTimeUtc
    [void](Send-DockCommand -Command 116)  # Lock dock
    Start-Sleep -Milliseconds 200
    [void](Send-DockCommand -Command 116)  # and back
    $saved = Wait-Until -TimeoutMs 5000 { (Get-Item $configPath).LastWriteTimeUtc -gt $before }
    Check 'config.json written after the lock toggles' $saved
    Start-Sleep -Milliseconds 700  # the debounced save of the second toggle (300 ms)

    $json = Get-Content -Raw $configPath | ConvertFrom-Json -AsHashtable
    $dock = @($json['docks'])[0]
    foreach ($k in 'windows', 'titles', 'runningFilter', 'titlesVertical', 'thumbnails', 'badges') {
        Check "no dock-level '$k' in the saved config" (-not $dock.ContainsKey($k))
    }
    $run = @($dock['items'] | Where-Object { $_['kind'] -eq 'running' })
    Check 'the saved dock has one running item' ($run.Count -eq 1)
    if ($run.Count -eq 1) {
        $s = $run[0]['settings']
        Check 'running item settings exist' ($null -ne $s)
        if ($s) {
            Check 'running gap 2 kept' ($s['gap'] -eq 2)
            Check 'running padding 12 kept' ($s['padding'] -eq 12)
            Check 'dock windows moved into the running item' ($s['windows'] -eq 'unstacked')
            Check 'dock titles moved into the running item' ($s['titles'] -eq 'one')
            Check 'dock runningFilter moved as filter' ($s['filter'] -eq 'monitor')
        }
    }
    Check 'dock unlocked again' (-not [bool]$dock['locked'])
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
