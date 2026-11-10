# tile-size.ps1 (round 14 phase 14D): thumbnail tiles with a thickness across the dock.
#
#   pwsh -File tools\smoke\tile-size.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config, quits the user's dock, starts -Exe with a test config: one right-edge
# dock with a running item (thumbnails "snapshots", thumbnailSize 100, thumbnailAcross 180) and a
# clock. Starts two mspaint.exe only when none is running (CLAUDE.md; closed again at the end), so
# there are windows to tile. Waits for the tiles (QueryState 330 non-zero), then asserts 330 =
# 100 x 180 DIPs at the dock's scale (along | across << 16) and the resting bar (328's high word) at
# least 180 DIPs thick; QueryState 330 for a dock that does not exist is -2. Always (finally) quits
# the test dock, closes the mspaint windows it started, restores the config byte for byte and
# restarts the user's dock.
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
function Near([int]$Px, [double]$Want) { [math]::Abs($Px - [math]::Round($Want)) -le 1 }

$userRunning = @(Get-WinDockProcess).Count -gt 0
$paintBefore = @(Get-Process -Name mspaint -ErrorAction SilentlyContinue).Count
$startedPaint = $false
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    if ($paintBefore -eq 0) {
        # Two windows to tile (mspaint, never Notepad / Explorer: STATUS.md "Testing cautions learned").
        Start-Process mspaint.exe
        Start-Process mspaint.exe
        $startedPaint = $true
        [void](Wait-Until -TimeoutMs 10000 { @(Get-Process -Name mspaint -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }).Count -ge 1 })
    } else {
        Write-Host "mspaint already running ($paintBefore): not starting any; the running windows give the tiles"
    }
    $cfg = @{
        hideTaskbar = $false
        tray        = @{ enabled = $false }
        docks       = @(@{
            id = 'smoke'; edge = 'right'; align = 'center'; autoHide = 'off'
            items = @(
                @{ kind = 'running'; settings = @{ thumbnails = 'snapshots'; thumbnailSize = 100; thumbnailAcross = 180 } },
                @{ kind = 'clock' }
            )
        })
    }
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    $s = Get-DockScale
    Write-Host "dock scale $s"

    $tiled = Wait-Until -TimeoutMs 10000 { (Get-DockState 330) -gt 0 }
    $v = Get-DockState 330
    Write-Host ("QueryState 330 = 0x{0:X8}" -f $v)
    Check 'tiles in the layout (QueryState 330 non-zero)' ($tiled -and $v -gt 0)
    $along = [int]($v -band 0xFFFF); $across = [int](($v -shr 16) -band 0xFFFF)
    Check "330 along = 100 DIP (got $along px)" (Near $along (100 * $s))
    Check "330 across = 180 DIP (got $across px)" (Near $across (180 * $s))
    $b = Get-DockState 328
    $thick = [int](($b -shr 16) -band 0xFFFF)
    Check "328 thickness >= 180 DIP (got $thick px)" ($thick -ge [math]::Round(180 * $s))

    $n = Get-DockState 330 -Dock 9
    Check "QueryState 330 of no such dock = -2 (got $n)" ($n -eq -2)
} catch {
    $fails.Add("error: $($_.Exception.Message)")
    Write-Host "ERROR: $($_.Exception.Message)"
} finally {
    try { Stop-WinDock } catch { $fails.Add("stop: $($_.Exception.Message)"); Write-Host "STOP: $($_.Exception.Message)" }
    if ($startedPaint) {
        # Only the ones this script started (none ran before): fresh, unsaved, empty canvases.
        Get-Process -Name mspaint -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Write-Host 'mspaint windows closed'
    }
    if ($bak) { Restore-WinDockConfig $bak; Write-Host 'config restored' }
    if ($userRunning -and -not $NoRestart) {
        [void](Start-WinDock -Exe $UserExe)
        Write-Host "user's dock restarted from $UserExe"
    }
}
if ($fails.Count) { Write-Host "$($fails.Count) failed"; exit 1 }
Write-Host 'all passed'
exit 0
