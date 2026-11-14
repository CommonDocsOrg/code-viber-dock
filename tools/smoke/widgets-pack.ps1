# widgets-pack.ps1 (round 28): the widgets pack's built-ins, notify.banner and perf.drives().
#
#   pwsh -File tools\smoke\widgets-pack.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config and quits the user's dock. One test config: a bottom dock (auto-hide
# off) with the five round 28 widgets (Timer, World clock, To-do, Moon phase, Quick buttons; slots
# 0 - 4) and a test widget (slot 5, "smoke-pack", written to a temp folder, installed with
# DockCommand 141 and allowed with DockCommand 188). Checks: the five built-ins load without an
# error and draw (QueryState 54 = 0, 58 > 0); the test widget's first notify.banner is taken and
# its second, at once after it, is refused by the rate limit (its storage: a = true, b = false);
# the dock shows the banner (179 = 1, 371 = 1) and ItemCommand 838 dismisses it (371 = 0);
# perf.drives() lists at least one fixed drive with a size; the dock's "indicator": "pill" reads
# back (QueryState 372 = 2) and a widget slot has no indicator (373 = 0). Nothing is clicked and no key is
# pressed (the Quick buttons are never pressed). Always (finally) quits the test dock, removes the
# test widget and its data, restores the config byte for byte and restarts the user's dock.
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

$testId = 'smoke-pack'
$data = Join-Path $env:LOCALAPPDATA 'WinDock'
$installed = Join-Path $data "widgets\$testId"
$store = Join-Path $data "widget-data\$testId.json"
if ((Test-Path $installed) -or (Test-Path $store)) { throw "$installed or $store exists already: remove it first" }
$src = Join-Path ([IO.Path]::GetTempPath()) "windock-$testId"
if (Test-Path $src) { Remove-Item -Recurse -Force $src }
New-Item -ItemType Directory $src | Out-Null
@'
{ "name": "Smoke pack", "version": "1.0.0", "author": "smoke", "permissions": ["notify", "perf"] }
'@ | Set-Content -Encoding utf8 (Join-Path $src 'widget.json')
@'
// The second banner comes inside the 5 s gap: refused (false).
const a = notify.banner({ title: 'Smoke banner', text: 'from tools\\smoke\\widgets-pack.ps1' });
const b = notify.banner({ title: 'Second banner' });
const d = perf.drives();
storage.set('result', { a, b, drives: d.length, sized: d.filter((x) => x.ready && x.total > 0).length });
export function draw(canvas, dock) { canvas.fillRect(4, 4, 8, 8, dock.theme.accent); }
'@ | Set-Content -Encoding utf8 (Join-Path $src 'main.js')

$builtIns = 'timer', 'world-clock', 'todo', 'moon', 'quick-buttons'
$items = @($builtIns | ForEach-Object { @{ kind = 'widget'; id = $_ } }) + @(@{ kind = 'widget'; id = $testId })
$cfg = @{ hideTaskbar = $false; tray = @{ enabled = $false }; docks = @(
    @{ id = 'smoke-pack'; edge = 'bottom'; align = 'center'; autoHide = 'off'; indicator = 'pill'; items = $items }
) }

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    [void](Wait-Until -TimeoutMs 10000 { (Get-DockState 58 -Arg 4) -gt 0 })
    for ($i = 0; $i -lt $builtIns.Count; $i++) {
        $state = Get-DockState 54 -Arg $i
        $draws = Get-DockState 58 -Arg $i
        Check "$($builtIns[$i]) (slot $i): state $state (0 = running), draws $draws" ($state -eq 0 -and $draws -gt 0)
    }
    # Round 28 phase 28D: the dock's indicator style is read from the config; a widget has none.
    Check "indicator style pill (372 = $(Get-DockState 372))" ((Get-DockState 372) -eq 2)
    Check "a widget slot shows no indicator (373 = $(Get-DockState 373 -Arg 0))" ((Get-DockState 373 -Arg 0) -eq 0)

    # The test widget: installed, then allowed (its instance reloads and runs its top level).
    Check 'test widget installed (DockCommand 141)' ((Install-WidgetFolder -Folder $src) -eq 1)
    Check 'test widget allowed (DockCommand 188)' ((Grant-WidgetConsent -Id $testId) -eq 1)
    $shown = Wait-Until -TimeoutMs 4000 { (Get-DockState 371) -eq 1 }
    Check "one widget banner on the dock (371 = $(Get-DockState 371), 179 = $(Get-DockState 179))" ($shown -and (Get-DockState 179) -eq 1)
    $saved = Wait-Until -TimeoutMs 4000 { Test-Path $store }
    if ($saved) {
        $r = (Get-Content -Raw $store | ConvertFrom-Json).result
        Check "first banner taken (a = $($r.a))" ($r.a -eq $true)
        Check "second banner refused by the rate limit (b = $($r.b))" ($r.b -eq $false)
        Check "perf.drives(): $($r.drives) fixed drive(s), $($r.sized) with a size" ($r.drives -ge 1 -and $r.sized -ge 1)
    } else {
        Check "the test widget's storage was written ($store)" $false
    }
    [void](Send-ItemCommand -Command 838 -Item 0)
    $gone = Wait-Until -TimeoutMs 2000 { (Get-DockState 371) -eq 0 }
    Check "the banner dismissed by ItemCommand 838 (371 = $(Get-DockState 371), 179 = $(Get-DockState 179))" ($gone -and (Get-DockState 179) -eq 0)
} catch {
    $fails.Add("error: $($_.Exception.Message)")
    Write-Host "ERROR: $($_.Exception.Message)"
} finally {
    try { Stop-WinDock } catch { $fails.Add("stop: $($_.Exception.Message)"); Write-Host "STOP: $($_.Exception.Message)" }
    foreach ($p in $installed, $store, $src) { if (Test-Path $p) { Remove-Item -Recurse -Force $p } }
    if ($bak) { Restore-WinDockConfig $bak; Write-Host 'config restored' }
    if ($userRunning -and -not $NoRestart) {
        [void](Start-WinDock -Exe $UserExe)
        Write-Host "user's dock restarted from $UserExe"
    }
}
if ($fails.Count) { Write-Host "$($fails.Count) failed"; exit 1 }
Write-Host 'all passed'
exit 0
