# widget-consent.ps1 (round 25 phase 25D): a widget folder runs only with the user's consent, and a
# widget's http requests stay inside its allowlist (redirects re-checked, local addresses refused).
#
#   pwsh -File tools\smoke\widget-consent.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe] [-NoRestart]
#
# Installs throwaway widgets under %LocalAppData%\WinDock\widgets\smoke-25d-*\ (and, only when the
# user has no such folder, widgets\hello\ shadowing the built-in Hello), backs up the user's config,
# quits the user's dock and runs -Exe with a test config (no taskbar hiding, no tray, one bottom
# dock). Each widget's main.js writes storage key "ran" when its module loads, so its
# widget-data\<id>.json exists only if its script ever ran.
#
#   (1) smoke-25d-perm asks for "perf": QueryState 357 = 2 (never allowed), its slot's state
#       QueryState 54 = 3 (needs permission); after a few seconds its storage file was never
#       written (the script did not load).
#   (2) DockCommand 188 grants it: 357 = 1, 54 = 0, and "ran" is stored.
#   (3) its manifest now asks for "perf" and "weather"; Reload widgets (126): 357 = 3 (asks for
#       more), 54 = 3. Granted again (188): it runs.
#   (4) its manifest goes back to "perf" only (fewer than allowed); Reload: 357 = 1, 54 = 0.
#   (5) DockCommand 189 revokes it: 357 = 2, 54 = 3.
#   (6) a folder "hello" (a built-in's id) without permissions: 357 = 2, its slot 54 = 3. Skipped
#       (noted) when the user has a widgets\hello folder of their own.
#   (7) http, against a tiny HTTP server on 127.0.0.1 run by this script (a TcpListener in a
#       thread job: /ok answers 200, /redir answers 302 to http://localhost:<port>/ok):
#       smoke-25d-http lists "http:127.0.0.1" only (granted): GET /ok resolves with status 200 (a
#       listed local address is allowed), GET /redir rejects with the redirect refused (localhost is
#       not listed); smoke-25d-wild lists "*.0.1" (granted): http.get('http://127.0.0.1:<port>/ok')
#       throws at the call (a wildcard never matches an address). The results are read from their
#       storage files after the dock quits.
#
# Always (finally) quits the test dock, stops the server, removes the widgets and their storage
# files, restores the config byte for byte (that removes the consent entries the test made) and
# restarts the user's dock. Sends no keys or clicks; no dialog opens (188 / 189 / 141 never show
# one; nothing here clicks a tile).
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

$root = Join-Path $env:LocalAppData 'WinDock'
$widgetsDir = Join-Path $root 'widgets'
$dataDir = Join-Path $root 'widget-data'
$ids = 'smoke-25d-perm', 'smoke-25d-http', 'smoke-25d-wild'
$helloDir = Join-Path $widgetsDir 'hello'
$helloData = Join-Path $dataDir 'hello.json'
$shadowTest = -not (Test-Path $helloDir)  # never touch a folder the user made
$helloDataExisted = Test-Path $helloData
$port = Get-Random -Minimum 50000 -Maximum 60000

$draw = @'
export function draw(canvas, dock) {
  canvas.text('25D', canvas.width / 2, canvas.height / 2, { size: 11, align: 'center', baseline: 'middle', color: dock.theme.text });
}
'@

function Write-Widget([string]$Id, [string[]]$Perms, [string]$Js = '') {
    $dir = Join-Path $widgetsDir $Id
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $p = ($Perms | ForEach-Object { '"' + $_ + '"' }) -join ', '
    $manifest = @"
{
  "name": "$Id",
  "version": "1.0.0",
  "description": "round 25 phase 25D smoke test (tools\\smoke\\widget-consent.ps1); safe to delete",
  "main": "main.js",
  "size": { "horizontal": { "main": 40, "cross": 40 }, "vertical": { "main": 40, "cross": 40 } },
  "permissions": [$p]
}
"@
    [IO.File]::WriteAllText((Join-Path $dir 'widget.json'), $manifest, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $dir 'main.js'), "storage.set('ran', Date.now());`n" + $Js + "`n" + $draw, [Text.UTF8Encoding]::new($false))
}

function Remove-TestWidgets {
    foreach ($id in $ids) {
        $dir = Join-Path $widgetsDir $id
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
        $data = Join-Path $dataDir "$id.json"
        if (Test-Path $data) { Remove-Item -Force $data }
    }
    if ($shadowTest) {
        if (Test-Path $helloDir) { Remove-Item -Recurse -Force $helloDir }
        if (-not $helloDataExisted -and (Test-Path $helloData)) { Remove-Item -Force $helloData }
    }
}

function Read-Store([string]$Id) {
    $p = Join-Path $dataDir "$Id.json"
    if (-not (Test-Path $p)) { return $null }
    Get-Content -Raw $p | ConvertFrom-Json -AsHashtable
}

$httpJs = @"
http.get('http://127.0.0.1:$port/ok').then((r) => storage.set('ok', r.status), (e) => storage.set('ok', 'rejected: ' + e.message));
http.get('http://127.0.0.1:$port/redir').then((r) => storage.set('redir', 'resolved ' + r.status), (e) => storage.set('redir', 'rejected: ' + e.message));
"@
$wildJs = @"
try { http.get('http://127.0.0.1:$port/ok'); storage.set('wild', 'sent'); } catch (e) { storage.set('wild', 'threw: ' + e.message); }
"@

# The server: answers up to 16 connections, then ends (the job is stopped in finally anyway).
$server = {
    param([int]$Port)
    $l = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
    $l.Start()
    try {
        # Accepts in half-second waits so Stop-Job ends it promptly (a blocking accept would not
        # stop); at most 16 connections and 90 s.
        $sw = [Diagnostics.Stopwatch]::StartNew()
        for ($n = 0; $n -lt 16 -and $sw.Elapsed.TotalSeconds -lt 90; ) {
            $task = $l.AcceptTcpClientAsync()
            while (-not $task.Wait(500)) { if ($sw.Elapsed.TotalSeconds -ge 90) { return } }
            $c = $task.Result
            $n++
            try {
                $s = $c.GetStream()
                $r = [IO.StreamReader]::new($s, [Text.Encoding]::ASCII, $false, 1024, $true)
                $line = $r.ReadLine()
                while (($h = $r.ReadLine()) -and $h.Length -gt 0) { }
                $path = ($line -split ' ')[1]
                $resp = if ($path -eq '/redir') {
                    "HTTP/1.1 302 Found`r`nLocation: http://localhost:$Port/ok`r`nContent-Length: 0`r`nConnection: close`r`n`r`n"
                } else {
                    "HTTP/1.1 200 OK`r`nContent-Type: text/plain`r`nContent-Length: 2`r`nConnection: close`r`n`r`nok"
                }
                $b = [Text.Encoding]::ASCII.GetBytes($resp)
                $s.Write($b, 0, $b.Length)
                $s.Flush()
            } finally { $c.Close() }
        }
    } finally { $l.Stop() }
}

function New-TestConfig([object[]]$Items) {
    @{
        hideTaskbar = $false
        tray        = @{ enabled = $false }
        docks       = @(@{ id = 'smoke'; edge = 'bottom'; align = 'center'; autoHide = 'off'; items = $Items })
    }
}

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
$job = $null
try {
    Stop-WinDock
    Remove-TestWidgets  # leftovers of an interrupted run
    Write-Widget 'smoke-25d-perm' @('perf')
    Write-Widget 'smoke-25d-http' @('http:127.0.0.1') $httpJs
    Write-Widget 'smoke-25d-wild' @('http:*.0.1') $wildJs
    if ($shadowTest) { Write-Widget 'hello' @() } else { Write-Host 'NOTE  (6) skipped: you have a widgets\hello folder of your own' }
    $job = Start-ThreadJob -ScriptBlock $server -ArgumentList $port

    $items = @(@{ kind = 'widget'; id = 'smoke-25d-perm' }, @{ kind = 'widget'; id = 'smoke-25d-http' }, @{ kind = 'widget'; id = 'smoke-25d-wild' })
    if ($shadowTest) { $items += @{ kind = 'widget'; id = 'hello' } }
    [void](Start-WinDock -Exe $Exe -Config (New-TestConfig $items))
    Check "$($items.Count) script widgets on the dock (QueryState 55)" (Wait-Until -TimeoutMs 4000 { (Get-DockState 55) -eq $items.Count })

    # (1) never allowed: no script
    Check '(1) smoke-25d-perm is not allowed (QueryState 357 = 2)' ((Get-WidgetConsent -Id 'smoke-25d-perm') -eq 2)
    Check '(1) its slot needs permission (QueryState 54 = 3)' ((Get-DockState 54 -Arg 0) -eq 3)
    Start-Sleep -Milliseconds 2500  # longer than the storage's 1 s debounce
    Check '(1) its script never ran (no widget-data file)' ($null -eq (Read-Store 'smoke-25d-perm'))

    # (2) granted: it runs
    Check '(2) DockCommand 188 grants it' ((Grant-WidgetConsent -Id 'smoke-25d-perm') -eq 1)
    Check '(2) allowed (357 = 1)' ((Get-WidgetConsent -Id 'smoke-25d-perm') -eq 1)
    Check '(2) running (54 = 0)' (Wait-Until -TimeoutMs 3000 { (Get-DockState 54 -Arg 0) -eq 0 })
    Check '(2) its script ran (storage written)' (Wait-Until -TimeoutMs 4000 { $null -ne (Read-Store 'smoke-25d-perm') })

    # (3) one more permission: asks again
    Write-Widget 'smoke-25d-perm' @('perf', 'weather')
    [void](Send-DockCommand -Command 126)
    Check '(3) asks for more (357 = 3)' ((Get-WidgetConsent -Id 'smoke-25d-perm') -eq 3)
    Check '(3) needs permission again (54 = 3)' (Wait-Until -TimeoutMs 3000 { (Get-DockState 54 -Arg 0) -eq 3 })
    Check '(3) granted again' ((Grant-WidgetConsent -Id 'smoke-25d-perm') -eq 1)
    Check '(3) running (54 = 0)' (Wait-Until -TimeoutMs 3000 { (Get-DockState 54 -Arg 0) -eq 0 })

    # (4) fewer than allowed: keeps running
    Write-Widget 'smoke-25d-perm' @('perf')
    [void](Send-DockCommand -Command 126)
    Check '(4) fewer permissions stay allowed (357 = 1)' ((Get-WidgetConsent -Id 'smoke-25d-perm') -eq 1)
    Check '(4) still running (54 = 0)' (Wait-Until -TimeoutMs 3000 { (Get-DockState 54 -Arg 0) -eq 0 })

    # (5) revoked: stops
    Check '(5) DockCommand 189 revokes it' ((Revoke-WidgetConsent -Id 'smoke-25d-perm') -eq 1)
    Check '(5) not allowed (357 = 2)' ((Get-WidgetConsent -Id 'smoke-25d-perm') -eq 2)
    Check '(5) needs permission (54 = 3)' (Wait-Until -TimeoutMs 3000 { (Get-DockState 54 -Arg 0) -eq 3 })

    # (6) a folder shadowing a built-in, without permissions
    if ($shadowTest) {
        Check '(6) the hello folder is not allowed (357 = 2)' ((Get-WidgetConsent -Id 'hello') -eq 2)
        Check '(6) its slot needs permission (54 = 3)' ((Get-DockState 54 -Arg 3) -eq 3)
    }

    # (7) http: grant both, let the requests run
    Check '(7) smoke-25d-http waits for consent (54 = 3)' ((Get-DockState 54 -Arg 1) -eq 3)
    Check '(7) grant smoke-25d-http' ((Grant-WidgetConsent -Id 'smoke-25d-http') -eq 1)
    Check '(7) grant smoke-25d-wild' ((Grant-WidgetConsent -Id 'smoke-25d-wild') -eq 1)
    [void](Wait-Until -TimeoutMs 8000 { $s = Read-Store 'smoke-25d-http'; $s -and $s.ContainsKey('ok') -and $s.ContainsKey('redir') })
    Check 'the test dock still answers' ((Get-DockState 55) -eq $items.Count)
    Stop-WinDock  # flushes the storage
    $h = Read-Store 'smoke-25d-http'; $w = Read-Store 'smoke-25d-wild'
    $ok = if ($h) { $h['ok'] } else { $null }
    $redir = if ($h) { [string]$h['redir'] } else { '' }
    $wild = if ($w) { [string]$w['wild'] } else { '' }
    Check "(7) a listed local address answers (ok = '$ok')" ($ok -eq 200)
    Check "(7) the redirect to an unlisted host is refused (redir = '$redir')" ($redir -like 'rejected:*redirect*localhost*')
    Check "(7) a wildcard never matches an address (wild = '$wild')" ($wild -like 'threw:*not in the widget*')
} catch {
    $fails.Add("error: $($_.Exception.Message)")
    Write-Host "ERROR: $($_.Exception.Message)"
} finally {
    try { Stop-WinDock } catch { $fails.Add("stop: $($_.Exception.Message)"); Write-Host "STOP: $($_.Exception.Message)" }
    if ($job) { Stop-Job $job -ErrorAction SilentlyContinue; Remove-Job $job -Force -ErrorAction SilentlyContinue }
    try { Remove-TestWidgets } catch { Write-Warning "could not remove the test widgets: $($_.Exception.Message)" }
    if ($bak) { Restore-WinDockConfig $bak; Write-Host 'config restored (the consent entries of the test are gone with it)' }
    if ($userRunning -and -not $NoRestart -and (Test-Path $UserExe)) {
        [void](Start-WinDock -Exe $UserExe)
        Write-Host "user's dock restarted from $UserExe"
    }
}
if ($fails.Count) { Write-Host "`n$($fails.Count) FAILED"; exit 1 }
Write-Host "`nall checks passed"
exit 0
