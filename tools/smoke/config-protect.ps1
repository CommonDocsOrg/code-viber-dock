# config-protect.ps1 (round 25 phase 25B): a malformed config.json at startup, and the rotating backups.
#
#   pwsh -File tools\smoke\config-protect.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe]
#
# Backs up the user's config.json and every config.bad-*.json / config.backup-*.json in
# %LocalAppData%\WinDock (bytes), quits the user's dock, then:
#   1. writes a deliberately broken config.json and starts -Exe. Asserts QueryState 356 = 3 (started
#      malformed + the copy was made), exactly one new config.bad-<stamp>.json whose bytes equal the
#      broken file, the notice (a #32770 "ViberDock" window of the dock process) is shown, and no
#      config.backup-* was touched (a malformed start does not rotate).
#      NOTE: this start necessarily runs on the DEFAULTS (the broken file cannot say otherwise): the
#      taskbar is hidden and the dock reserves space for a few seconds. The setting change below is
#      Hide taskbar off (DockCommand 101), so the taskbar comes back at once.
#   2. the save rule: DockCommand 101 is saved (QueryState 298 = 0), config.json is now valid JSON
#      with "hideTaskbar": false, and the .bad copy's bytes are unchanged.
#   3. rotation: quits, writes a valid test config (bytes), starts: config.backup-1.json = those bytes
#      and the old backup-1 (if any) is backup-2; quits and starts again: when config.json did not
#      change in between, backup-1 / -2 are unchanged (no churn).
# Always (finally) quits the test dock, restores config.json byte for byte, puts the .bad / backup
# files back exactly as they were (the test's own deleted, the user's restored if rotated or
# pruned) and restarts the user's dock.
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

$dir = Join-Path $env:LOCALAPPDATA 'WinDock'
$configPath = Join-Path $dir 'config.json'

function Get-CopyFiles {
    # The .bad copies and the rotating backups now in the folder: name -> bytes.
    $map = @{}
    foreach ($pattern in 'config.bad-*.json', 'config.backup-*.json') {
        foreach ($f in @(Get-ChildItem -LiteralPath $dir -Filter $pattern -File -ErrorAction SilentlyContinue)) {
            $map[$f.Name] = [IO.File]::ReadAllBytes($f.FullName)
        }
    }
    $map
}
function Test-SameBytes([byte[]]$A, [byte[]]$B) {
    if ($null -eq $A -or $null -eq $B) { return ($null -eq $A) -and ($null -eq $B) }
    ($A.Length -eq $B.Length) -and ([Convert]::ToBase64String($A) -eq [Convert]::ToBase64String($B))
}
function Read-Bytes([string]$Path) { if (Test-Path -LiteralPath $Path) { [IO.File]::ReadAllBytes($Path) } else { $null } }
function Get-SavedConfig {
    try { Get-Content -Raw $configPath | ConvertFrom-Json -AsHashtable } catch { $null }
}
function Get-Notice {
    @(Get-WinDockWindowsOfClass -Class '#32770' | Where-Object { [WinDockSmoke.Native]::TextOf($_) -eq 'ViberDock' })  # round 27 phase 27A: the title says the product name
}

$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
$before = Get-CopyFiles
Write-Host "copies / backups before: $($before.Keys.Count)"
try {
    Stop-WinDock

    # --- 1. a broken file ---
    $broken = [Text.Encoding]::UTF8.GetBytes("{ `"docks`": [ { `"id`": `"smoke`", `"edge`": `"bottom`", `"items`": [ { `"kind`": `"clock`" ")
    [IO.File]::WriteAllBytes($configPath, $broken)
    [void](Start-WinDock -Exe $Exe)

    [void](Wait-Until -TimeoutMs 5000 { (Get-DockState 356) -ne 0 })
    $v = Get-DockState 356
    Check "QueryState 356 = 3: started malformed, copy made (got $v)" ($v -eq 3)

    $now = Get-CopyFiles
    $newBad = @($now.Keys | Where-Object { $_ -like 'config.bad-*' -and -not $before.ContainsKey($_) })
    Check "one new config.bad-*.json (got $($newBad -join ', '))" ($newBad.Count -eq 1)
    if ($newBad.Count -eq 1) {
        Check "the copy's bytes equal the broken file" (Test-SameBytes $now[$newBad[0]] $broken)
    }
    $backupsSame = $true
    foreach ($k in @(@($before.Keys) + @($now.Keys) | Where-Object { $_ -like 'config.backup-*' } | Select-Object -Unique)) {
        if (-not (Test-SameBytes $before[$k] $now[$k])) { $backupsSame = $false }
    }
    Check 'a malformed start rotates no backup' $backupsSame
    $ok = Wait-Until -TimeoutMs 5000 { (Get-Notice).Count -gt 0 }
    Check 'the notice is shown' $ok
    Write-Host ("INFO  config.json still the broken bytes before any change: {0}" -f (Test-SameBytes (Read-Bytes $configPath) $broken))

    # --- 2. the save rule: with the copy made, a change saves over config.json ---
    $r = Send-DockCommand -Command 101   # Hide taskbar: default on -> off (also brings the taskbar back)
    $ok = Wait-Until -TimeoutMs 5000 { $c = Get-SavedConfig; $c -and $c.ContainsKey('hideTaskbar') -and $c['hideTaskbar'] -eq $false }
    $s = Get-DockState 298
    Check "DockCommand 101 ($r) saved: config.json valid, hideTaskbar false, 298 = 0 (got $s)" ($ok -and $s -eq 0)
    if ($newBad.Count -eq 1) {
        Check 'the copy is unchanged after the save' (Test-SameBytes (Read-Bytes (Join-Path $dir $newBad[0])) $broken)
    }
    $v = Get-DockState 356
    Check "QueryState 356 still 3 after the save (got $v)" ($v -eq 3)
    foreach ($h in Get-Notice) { [void](Send-WindowMessage -Hwnd $h -Msg 0x0010 -Post) }  # WM_CLOSE
    $ok = Wait-Until -TimeoutMs 3000 { (Get-Notice).Count -eq 0 }
    Check 'the notice closes on WM_CLOSE' $ok
    Stop-WinDock

    # --- 3. rotation after a good start ---
    $oldB1 = Read-Bytes (Join-Path $dir 'config.backup-1.json')
    $good = @{
        startWithWindows = $true
        hideTaskbar      = $false
        tray             = @{ enabled = $false }
        docks            = @(@{ id = 'smoke'; edge = 'bottom'; align = 'center'; autoHide = 'off'; reserveSpace = $false; items = @(@{ kind = 'clock' }) })
    }
    $goodBytes = [Text.UTF8Encoding]::new($false).GetBytes(($good | ConvertTo-Json -Depth 20))
    [IO.File]::WriteAllBytes($configPath, $goodBytes)
    [void](Start-WinDock -Exe $Exe)
    $v = Get-DockState 356
    Check "QueryState 356 = 0 after a good start (got $v)" ($v -eq 0)
    $b1 = Read-Bytes (Join-Path $dir 'config.backup-1.json')
    $b2 = Read-Bytes (Join-Path $dir 'config.backup-2.json')
    Check 'backup-1 = the file as it was at the start' (Test-SameBytes $b1 $goodBytes)
    if ($null -ne $oldB1 -and -not (Test-SameBytes $oldB1 $goodBytes)) {
        Check 'the old backup-1 moved to backup-2' (Test-SameBytes $b2 $oldB1)
    }
    Stop-WinDock

    $c3 = Read-Bytes $configPath
    [void](Start-WinDock -Exe $Exe)
    $b1n = Read-Bytes (Join-Path $dir 'config.backup-1.json')
    $b2n = Read-Bytes (Join-Path $dir 'config.backup-2.json')
    if (Test-SameBytes $c3 $goodBytes) {
        Check 'a restart without changes does not rotate' ((Test-SameBytes $b1n $b1) -and (Test-SameBytes $b2n $b2))
    } else {
        Write-Host 'INFO  the test dock rewrote config.json during its run: the restart rotated'
        Check 'backup-1 = the rewritten file, backup-2 = the test config' ((Test-SameBytes $b1n $c3) -and (Test-SameBytes $b2n $goodBytes))
    }
} catch {
    $fails.Add("error: $($_.Exception.Message)")
    Write-Host "ERROR: $($_.Exception.Message)"
} finally {
    try { Stop-WinDock } catch { $fails.Add("stop: $($_.Exception.Message)"); Write-Host "STOP: $($_.Exception.Message)" }
    if ($bak) {
        Restore-WinDockConfig $bak; Write-Host 'config restored'
    } elseif (Test-Path -LiteralPath $configPath) {
        Remove-Item -LiteralPath $configPath -Force; Write-Host 'there was no config before: the test one removed'
    }
    # The copies / backups exactly as before: the test's own go, the user's come back as they were.
    $after = Get-CopyFiles
    foreach ($k in @($after.Keys)) {
        if (-not $before.ContainsKey($k)) { Remove-Item -LiteralPath (Join-Path $dir $k) -Force; Write-Host "removed $k" }
    }
    foreach ($k in @($before.Keys)) {
        if (-not (Test-SameBytes $after[$k] $before[$k])) { [IO.File]::WriteAllBytes((Join-Path $dir $k), $before[$k]); Write-Host "restored $k" }
    }
    if ($userRunning -and -not $NoRestart) {
        [void](Start-WinDock -Exe $UserExe)
        Write-Host "user's dock restarted from $UserExe"
    }
}
if ($fails.Count) { Write-Host "$($fails.Count) failed"; exit 1 }
Write-Host 'all passed'
exit 0
