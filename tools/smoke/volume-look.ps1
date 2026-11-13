# volume-look.ps1 (round 22 phase 22A): the Volume widget's views, icons and size.
#
#   pwsh -File tools\smoke\volume-look.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe] [-OutDir <folder>]
#
# Backs up the user's config and quits the user's dock. One test config with two docks (bottom and
# left, 72 DIPs thick), each holding one Volume widget per variant below (every view, every icon,
# the percentage on and off, a scale and colours, an image). For every slot: QueryState 54 = 0 (the
# script runs without an error) and 58 > 0 (it drew). It reads the volume and never changes it. A
# PNG of each dock goes to -OutDir (default: the temp folder) for a look; what it looks like is the
# user's manual check. Always (finally) quits the test dock, restores the config byte for byte and
# restarts the user's dock.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\build\dev\WinDock.exe'),
    [string]$UserExe = (Join-Path $PSScriptRoot '..\..\build\x64-release\WinDock.exe'),
    [string]$OutDir = [IO.Path]::GetTempPath(),
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WinDock.psm1') -Force

$fails = [Collections.Generic.List[string]]::new()
function Check([string]$What, [bool]$Ok) {
    Write-Host ("{0}  {1}" -f ($(if ($Ok) { 'PASS' } else { 'FAIL' })), $What)
    if (-not $Ok) { $fails.Add($What) }
}

$exePath = (Resolve-Path $Exe).Path
$variants = @(
    @{},
    @{ style = 'ring' },
    @{ style = 'ring'; showPercent = $false },
    @{ style = 'ring'; icon = 'none' },
    @{ style = 'bar-horizontal' },
    @{ style = 'bar-vertical' },
    @{ icon = 'bars' },
    @{ style = 'ring'; icon = 'headphones' },
    @{ icon = 'note'; showPercent = $false },
    @{ icon = 'none' },
    @{ style = 'ring'; scale = 1.5; font = 'Consolas'; iconColor = '#FFFF8800'; textColor = '#FF44AAFF'; levelColor = '#FF44DD66' },
    @{ style = 'bar-horizontal'; icon = 'image'; image = $exePath },
    @{ style = 'bar-vertical'; icon = 'image'; image = 'C:\no-such-file.png'; showPercent = $false }
)
function Items { @($variants | ForEach-Object { @{ kind = 'widget'; id = 'volume'; settings = $_ } }) }
$cfg = @{ hideTaskbar = $false; tray = @{ enabled = $false }; docks = @(
    @{ id = 'smoke-bottom'; edge = 'bottom'; align = 'center'; autoHide = 'off'; thickness = 72; items = (Items) },
    @{ id = 'smoke-left'; edge = 'left'; align = 'center'; autoHide = 'off'; thickness = 72; items = (Items) }
) }

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    foreach ($dock in 0, 1) {
        $n = $variants.Count
        [void](Wait-Until -TimeoutMs 10000 { (Get-DockState 58 -Dock $dock -Arg ($n - 1)) -gt 0 })
        Check "dock ${dock}: $n script widgets (55 = $(Get-DockState 55 -Dock $dock))" ((Get-DockState 55 -Dock $dock) -eq $n)
        for ($i = 0; $i -lt $n; $i++) {
            $state = Get-DockState 54 -Dock $dock -Arg $i
            $draws = Get-DockState 58 -Dock $dock -Arg $i
            $what = ($variants[$i].GetEnumerator() | Sort-Object Key | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '
            Check "dock $dock slot ${i} ($what): state $state, draws $draws" ($state -eq 0 -and $draws -gt 0)
        }
        $png = Join-Path $OutDir "volume-look-dock$dock.png"
        [void](Save-WindowImage -Hwnd (Get-DockWindow -Dock $dock) -Path $png)
        Write-Host "picture: $png"
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
