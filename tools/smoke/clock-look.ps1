# clock-look.ps1 (round 20 phase 20A): the clock item's look (scale, font, weight, colours).
#
#   pwsh -File tools\smoke\clock-look.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config and quits the user's dock. Three test docks in turn (each one bottom
# dock with one clock, the date shown):
#   1. the plain clock: QueryState 340 (the drawn scale, percent) = 100, 341 its block (w | h << 16);
#   2. "scale": 2 on a thick dock (thickness 160 DIPs): 340 = 200 and the block is about twice as
#      tall and wide as in 1 (within 15 %: text metrics are not exactly linear);
#   3. "scale": 2 on a plain dock (no thickness): 340 is below 200 (the fit cap) and the block is no
#      taller than the bar's icon row (QueryState 328's thickness, which includes the padding), with
#      a font, a weight and colours set too (they must not break the layout: 341 > 0).
# A face file with the look (face "large-time-small-date", scale 1.5) draws (341 > 0). What it looks
# like is the user's manual check. Always (finally) quits the test dock, restores the config byte
# for byte and restarts the user's dock.
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
function Dock([hashtable]$Clock, [int]$Thickness = 0) {
    $d = @{ id = 'smoke'; edge = 'bottom'; align = 'center'; autoHide = 'off'; items = @(@{ kind = 'clock'; settings = $Clock }) }
    if ($Thickness -gt 0) { $d['thickness'] = $Thickness }
    @{ hideTaskbar = $false; tray = @{ enabled = $false }; docks = @($d) }
}
function Start-Test([hashtable]$Cfg) {
    [void](Start-WinDock -Exe $Exe -Config $Cfg)
    # The first layout: the block is known once 341 answers with a size.
    [void](Wait-Until -TimeoutMs 10000 { $v = Get-DockState 341; $v -gt 0 })
    $block = Get-DockState 341
    [pscustomobject]@{ Scale = (Get-DockState 340); W = ($block -band 0xFFFF); H = (($block -shr 16) -band 0xFFFF) }
}

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock

    # 1. The plain clock.
    $a = Start-Test (Dock @{ showDate = $true })
    Write-Host "plain: scale $($a.Scale) block $($a.W) x $($a.H)"
    Check "plain clock: 340 = 100 (got $($a.Scale)), 341 non-zero ($($a.W) x $($a.H))" ($a.Scale -eq 100 -and $a.W -gt 0 -and $a.H -gt 0)
    Stop-WinDock

    # 2. Scale 2 on a thick dock: twice the size.
    $b = Start-Test (Dock @{ showDate = $true; scale = 2 } 160)
    Write-Host "scale 2, thick: scale $($b.Scale) block $($b.W) x $($b.H)"
    $ratioH = $b.H / [math]::Max(1, $a.H)
    $ratioW = $b.W / [math]::Max(1, $a.W)
    Check "scale 2 on a thick dock: 340 = 200 (got $($b.Scale))" ($b.Scale -eq 200)
    Check "its block about twice the plain one (h x$([math]::Round($ratioH, 2)), w x$([math]::Round($ratioW, 2)))" ($ratioH -ge 1.7 -and $ratioH -le 2.3 -and $ratioW -ge 1.7 -and $ratioW -le 2.3)
    Stop-WinDock

    # 3. Scale 2 on a plain dock, with a font, a weight and colours: capped to the row.
    $c = Start-Test (Dock @{ showDate = $true; scale = 2; font = 'Consolas'; weight = 'bold'; timeColor = '#FFFF8800'; dateColor = '#FF44AAFF' })
    $bar = Get-DockState 328
    $thick = ($bar -shr 16) -band 0xFFFF
    Write-Host "scale 2, plain: scale $($c.Scale) block $($c.W) x $($c.H); bar thickness $thick"
    Check "scale 2 on a plain dock: 340 below 200 and above 100 (got $($c.Scale))" ($c.Scale -lt 200 -and $c.Scale -gt 100)
    Check "its block no taller than the bar (h $($c.H), bar $thick)" ($c.H -gt 0 -and $c.H -le $thick)
    Check "font / weight / colours drew a block (w $($c.W))" ($c.W -gt 0)
    Stop-WinDock

    # 4. A face with the look.
    $d = Start-Test (Dock @{ showDate = $true; showSeconds = $true; face = 'large-time-small-date'; scale = 1.5; font = 'Bahnschrift'; timeColor = '#FFFFFFFF' } 120)
    Write-Host "face, scale 1.5: scale $($d.Scale) block $($d.W) x $($d.H)"
    Check "face large-time-small-date at scale 1.5: 340 = 150 (got $($d.Scale)), block drawn" ($d.Scale -eq 150 -and $d.W -gt 0 -and $d.H -gt 0)
    $faceState = Get-DockState 216
    Check "QueryState 216 = 1 (the face file draws; got $faceState)" ($faceState -eq 1)
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
