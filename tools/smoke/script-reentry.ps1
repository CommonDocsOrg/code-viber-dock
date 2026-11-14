# script-reentry.ps1 (round 25 phase 25A): a widget cannot re-enter its own script, freeze the UI
# thread with timers, or take another instance's results.
#
#   pwsh -File tools\smoke\script-reentry.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe] [-DismissNotifications]
#
# Installs four throwaway widgets under %LocalAppData%\WinDock\widgets\smoke-25a-*\ (their storage
# goes to widget-data\smoke-25a-*.json), backs up the user's config, quits the user's dock and runs
# -Exe twice with a test config (no taskbar hiding, no tray, one bottom dock, a calendar feed that
# is a small .ics file in the temp folder, so calendar.refresh() reports a change synchronously):
#
#   Run 1, three widgets (slots 0 - 2):
#   (1a) smoke-25a-notif: notifications.onChange listener, then (after the service's first poll)
#        markRead of the first unread notification (dismissAll() with -DismissNotifications), then
#        an endless loop: suspended (QueryState 54 = 2), its listener never ran inside the call.
#        Without an unread notification nothing re-enters (the loop is still stopped): noted.
#   (1b) smoke-25a-cal: calendar.onChange listener, calendar.refresh() (a synchronous change of
#        its own provider: before 25A a nested call that disarmed the watchdog), then an endless
#        loop: suspended (2), the listener did not run inside the call.
#   (2)  smoke-25a-throw: a calendar.onChange listener that throws, then calendar.refresh() (and
#        dismissAll() with -DismissNotifications): the call returns normally ("returned" stored
#        after the refresh), then the listener runs and throws: the widget is in the error state
#        (QueryState 54 = 1), the dock still answers.
#   Run 2, one widget:
#   (3)  smoke-25a-cpu: 64 setInterval(16 ms) timers each burning 30 ms (its timeLimitMs is 100, so
#        the watchdog never fires): the dock keeps answering QueryState (each answer timed, the
#        slowest must stay under 1 s) and the CPU budget suspends the widget (2) within 20 s.
#
# -DismissNotifications really dismisses every notification of the user's notification centre
# (Windows removes them): off by default, the test then uses markRead (the dock's own unread flag).
# Always (finally) quits the test dock, removes the widgets, their storage files and the .ics,
# restores the config byte for byte and restarts the user's dock. Sends no keys or clicks.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\build\dev\WinDock.exe'),
    [string]$UserExe = (Join-Path $PSScriptRoot '..\..\build\x64-release\WinDock.exe'),
    [switch]$DismissNotifications,
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WinDock.psm1') -Force

$fails = [Collections.Generic.List[string]]::new()
function Check([string]$What, [bool]$Ok) {
    Write-Host ("{0}  {1}" -f ($(if ($Ok) { 'PASS' } else { 'FAIL' })), $What)
    if (-not $Ok) { $fails.Add($What) }
}

$root = Join-Path $env:LocalAppData 'WinDock'
$widgetsDir = Join-Path $root 'widgets'
$dataDir = Join-Path $root 'widget-data'
$ids = 'smoke-25a-notif', 'smoke-25a-cal', 'smoke-25a-throw', 'smoke-25a-cpu'
$ics = Join-Path ([IO.Path]::GetTempPath()) 'windock-smoke-25a.ics'

# --- the widgets ---------------------------------------------------------------------------------

$draw = @'
export function draw(canvas, dock) {
  canvas.text(LABEL, canvas.width / 2, canvas.height / 2, { size: 11, align: 'center', baseline: 'middle', color: dock.theme.text });
}
'@

$sources = @{
    'smoke-25a-notif' = @{
        perms = @('notifications'); limit = 50
        settings = '[{ "key": "dismiss", "type": "bool", "label": "Dismiss all", "default": false }]'
        js = @'
const LABEL = '1a';
let inCall = false;
notifications.onChange(() => { storage.set('listener', inCall ? 'nested' : 'after'); });
notifications.get();  // the service starts; its first poll fills the list
setTimeout(() => {
  inCall = true;
  const unread = notifications.get().filter((n) => n.unread);
  storage.set('unread', unread.length);
  if (settings.dismiss) notifications.dismissAll();
  else if (unread.length) notifications.markRead(unread[0].id);
  storage.set('phase', 'looping');
  for (;;) {}
}, 4000);
'@
    }
    'smoke-25a-cal' = @{
        perms = @('calendar'); limit = 50; settings = '[]'
        js = @'
const LABEL = '1b';
let inCall = false;
calendar.onChange(() => { storage.set('listener', inCall ? 'nested' : 'after'); });
setTimeout(() => {
  inCall = true;
  calendar.refresh();
  storage.set('phase', 'looping');
  for (;;) {}
}, 1500);
'@
    }
    'smoke-25a-throw' = @{
        perms = @('calendar', 'notifications'); limit = 50
        settings = '[{ "key": "dismiss", "type": "bool", "label": "Dismiss all", "default": false }]'
        js = @'
const LABEL = '2';
let inCall = false;
calendar.onChange(() => {
  storage.set('listener', inCall ? 'nested' : 'after');
  throw new Error('calendar listener boom');
});
setTimeout(() => {
  inCall = true;
  storage.set('phase', 'calling');
  if (settings.dismiss) {
    notifications.onChange(() => { throw new Error('notifications listener boom'); });
    notifications.dismissAll();
  }
  calendar.refresh();
  storage.set('phase', 'returned');
  inCall = false;
}, 2500);
'@
    }
    'smoke-25a-cpu' = @{
        perms = @(); limit = 100; settings = '[]'
        js = @'
const LABEL = '3';
const burn = () => { const end = Date.now() + 30; while (Date.now() < end) {} };
setTimeout(() => {
  let n = 0;
  try { for (; n < 64; n++) setInterval(burn, 16); } catch (e) { /* the 64-timer limit */ }
  storage.set('timers', n);
}, 500);
'@
    }
}

function Install-TestWidgets {
    foreach ($id in $ids) {
        $s = $sources[$id]
        $dir = Join-Path $widgetsDir $id
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $perms = ($s.perms | ForEach-Object { '"' + $_ + '"' }) -join ', '
        $manifest = @"
{
  "name": "$id",
  "version": "1.0.0",
  "description": "round 25 phase 25A smoke test (tools\\smoke\\script-reentry.ps1); safe to delete",
  "main": "main.js",
  "size": { "horizontal": { "main": 40, "cross": 40 }, "vertical": { "main": 40, "cross": 40 } },
  "timeLimitMs": $($s.limit),
  "permissions": [$perms],
  "settings": $($s.settings)
}
"@
        [IO.File]::WriteAllText((Join-Path $dir 'widget.json'), $manifest, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $dir 'main.js'), $s.js + "`n" + $draw, [Text.UTF8Encoding]::new($false))
    }
}

function Remove-TestWidgets {
    foreach ($id in $ids) {
        $dir = Join-Path $widgetsDir $id
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
        $data = Join-Path $dataDir "$id.json"
        if (Test-Path $data) { Remove-Item -Force $data }
    }
}

function Read-Store([string]$Id) {
    $p = Join-Path $dataDir "$Id.json"
    if (-not (Test-Path $p)) { return @{} }
    Get-Content -Raw $p | ConvertFrom-Json -AsHashtable
}

function Get-TimedState([int]$Number, [int]$Arg = 0) {
    # QueryState with its answer time (ms); a stuck dock throws after 5 s (SendMessageTimeout).
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $v = Get-DockState $Number -Arg $Arg
    [pscustomobject]@{ Value = $v; Ms = $sw.ElapsedMilliseconds }
}

function New-TestConfig([object[]]$Items) {
    @{
        hideTaskbar = $false
        tray        = @{ enabled = $false }
        calendars   = @{ feeds = @(@{ name = 'smoke 25A'; url = $ics; enabled = $true }) }
        docks       = @(@{ id = 'smoke'; edge = 'bottom'; align = 'center'; autoHide = 'off'; items = $Items })
    }
}

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    Remove-TestWidgets  # leftovers of an interrupted run
    Install-TestWidgets
    [IO.File]::WriteAllText($ics, "BEGIN:VCALENDAR`r`nVERSION:2.0`r`nPRODID:-//WinDock smoke 25A//EN`r`nBEGIN:VEVENT`r`nUID:smoke-25a-1`r`nDTSTAMP:20260101T000000Z`r`nDTSTART:20260101T100000Z`r`nDTEND:20260101T110000Z`r`nSUMMARY:smoke 25A`r`nEND:VEVENT`r`nEND:VCALENDAR`r`n")

    # --- run 1: re-entry (1a, 1b, 2) ---
    $dismiss = [bool]$DismissNotifications
    $cfg = New-TestConfig @(
        @{ kind = 'widget'; id = 'smoke-25a-notif'; settings = @{ dismiss = $dismiss } },
        @{ kind = 'widget'; id = 'smoke-25a-cal' },
        @{ kind = 'widget'; id = 'smoke-25a-throw'; settings = @{ dismiss = $dismiss } }
    )
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    # Round 25 phase 25D: widget folders with permissions run only with consent: grant it (the
    # widgets reload and start their timers now; the test config, and with it the consent, is
    # replaced by run 2's and the user's config is restored at the end).
    foreach ($id in 'smoke-25a-notif', 'smoke-25a-cal', 'smoke-25a-throw') {
        Check "consent granted to $id (DockCommand 188)" ((Grant-WidgetConsent -Id $id) -eq 1)
        Check "$id is allowed (QueryState 357 = 1)" ((Get-WidgetConsent -Id $id) -eq 1)
    }
    Check 'three script widgets on the dock (QueryState 55)' (Wait-Until -TimeoutMs 4000 { (Get-DockState 55) -eq 3 })
    foreach ($slot in 0..2) { Check "slot $slot is a script widget (QueryState 24 = 6)" ((Get-DockState 24 -Arg $slot) -eq 6) }

    Check '(1b) the calendar loop widget is suspended (QueryState 54 = 2)' (Wait-Until -TimeoutMs 8000 { (Get-DockState 54 -Arg 1) -eq 2 })
    Check '(2) the throwing-listener widget is in the error state (QueryState 54 = 1)' (Wait-Until -TimeoutMs 8000 { (Get-DockState 54 -Arg 2) -eq 1 })
    Check '(1a) the notifications loop widget is suspended (QueryState 54 = 2)' (Wait-Until -TimeoutMs 10000 { (Get-DockState 54 -Arg 0) -eq 2 })
    $t = Get-TimedState 55
    Check "the dock still answers QueryState ($($t.Ms) ms)" ($t.Value -eq 3 -and $t.Ms -lt 1000)

    Stop-WinDock  # flushes the widgets' storage
    $s1a = Read-Store 'smoke-25a-notif'; $s1b = Read-Store 'smoke-25a-cal'; $s2 = Read-Store 'smoke-25a-throw'
    Check "(1a) reached its loop (phase '$($s1a['phase'])')" ($s1a['phase'] -eq 'looping')
    Check "(1a) its listener never ran inside the call (listener '$($s1a['listener'])')" ($s1a['listener'] -ne 'nested')
    if (-not $dismiss -and [int]($s1a['unread'] ?? 0) -eq 0) {
        Write-Host 'NOTE  (1a) no unread notification: markRead changed nothing, so nothing re-entered (only the watchdog was tested)'
    }
    Check "(1b) reached its loop after calendar.refresh() (phase '$($s1b['phase'])')" ($s1b['phase'] -eq 'looping')
    Check "(1b) its listener never ran inside the call (listener '$($s1b['listener'])')" ($s1b['listener'] -ne 'nested')
    Check "(2) the call returned before the listener ran (phase '$($s2['phase'])')" ($s2['phase'] -eq 'returned')
    Check "(2) the listener ran after the call (listener '$($s2['listener'])')" ($s2['listener'] -eq 'after' -or ($dismiss -and $null -eq $s2['listener']))

    # --- run 2: the CPU budget (3) ---
    [void](Start-WinDock -Exe $Exe -Config (New-TestConfig @(@{ kind = 'widget'; id = 'smoke-25a-cpu' })))
    $slowest = 0; $suspended = $false; $answers = 0
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt 20000) {
        $t = Get-TimedState 54 -Arg 0
        $answers++
        if ($t.Ms -gt $slowest) { $slowest = $t.Ms }
        if ($t.Value -eq 2) { $suspended = $true; break }
        Start-Sleep -Milliseconds 200
    }
    Write-Host ("(3) {0} answers, slowest {1} ms, suspended after {2:N1} s" -f $answers, $slowest, ($sw.ElapsedMilliseconds / 1000.0))
    Check '(3) the 64-timer widget is suspended by the CPU budget within 20 s (QueryState 54 = 2)' $suspended
    Check "(3) the dock kept answering while it ran (slowest QueryState $slowest ms < 1000)" ($slowest -lt 1000)
    Check '(3) more than one answer while it ran' ($answers -gt 1)
    Stop-WinDock
    $s3 = Read-Store 'smoke-25a-cpu'
    Check "(3) it had 64 timers (stored $($s3['timers']))" ([int]($s3['timers'] ?? 0) -eq 64)
} catch {
    $fails.Add("error: $($_.Exception.Message)")
    Write-Host "ERROR: $($_.Exception.Message)"
} finally {
    try { Stop-WinDock } catch { $fails.Add("stop: $($_.Exception.Message)"); Write-Host "STOP: $($_.Exception.Message)" }
    try { Remove-TestWidgets } catch { Write-Warning "could not remove the test widgets: $($_.Exception.Message)" }
    if (Test-Path $ics) { Remove-Item -Force $ics }
    if ($bak) { Restore-WinDockConfig $bak; Write-Host 'config restored' }
    if ($userRunning -and -not $NoRestart -and (Test-Path $UserExe)) {
        [void](Start-WinDock -Exe $UserExe)
        Write-Host "user's dock restarted from $UserExe"
    }
}
if ($fails.Count) { Write-Host "`n$($fails.Count) FAILED"; exit 1 }
Write-Host "`nall checks passed"
exit 0
