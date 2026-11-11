# photo-frame.ps1 (round 17 phase 17A): the Photo frame widget runs a slideshow on a dock and shows one
# picture standalone, from files anywhere on disk (canvas.image with an absolute path).
#
#   pwsh -File tools\smoke\photo-frame.ps1 [-Exe build\dev\WinDock.exe] [-UserExe build\x64-release\WinDock.exe] [-OutDir <folder>]
#
# Makes five small pictures (and one text file) in a temp folder, backs up the user's config, quits
# the user's dock, starts -Exe with a test config (a bottom dock holding a clock and a photo-frame
# slideshow of that folder, 1 s per picture, a fade; a standalone photo-frame of one portrait picture
# with the frame in the picture's own shape; both with "debugStore": true), waits for a few pictures,
# saves a PNG of each window into -OutDir (default: the temp folder), quits, then asserts from the
# widget's storage (widget-data\photo-frame.json):
#   - the dock's slideshow found the five pictures (not the text file) and showed at least three, in
#     Explorer's name order: 1.png, 2.jpg, 10.png (2 before 10);
#   - the standalone frame took the portrait picture's shape (its picture box is 3 : 4);
#   - (before quitting) the slideshow's Item window (ItemCommand 854) shows the Folder field's box
#     with the folder and its Browse… button, and no button for the hidden Picture field.
# Always (finally) quits the test dock, restores the config and the widget's storage file byte for
# byte, removes the temp pictures and restarts the user's dock. Sends no keys or clicks.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\..\build\dev\WinDock.exe'),
    [string]$UserExe = (Join-Path $PSScriptRoot '..\..\build\x64-release\WinDock.exe'),
    [string]$OutDir,
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WinDock.psm1') -Force
Add-Type -AssemblyName System.Drawing

$fails = [Collections.Generic.List[string]]::new()
function Check([string]$What, [bool]$Ok) {
    Write-Host ("{0}  {1}" -f ($(if ($Ok) { 'PASS' } else { 'FAIL' })), $What)
    if (-not $Ok) { $fails.Add($What) }
}

# The pictures: solid colours with a diagonal, so a cropped or fitted one can be told apart.
$dir = Join-Path $env:TEMP 'windock-photo-smoke'
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
New-Item -ItemType Directory -Path $dir | Out-Null
if (-not $OutDir) { $OutDir = $dir }
function New-Picture([string]$Name, [int]$W, [int]$H, [string]$Color) {
    $bmp = New-Object System.Drawing.Bitmap $W, $H
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.ColorTranslator]::FromHtml($Color))
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), 6
    $g.DrawLine($pen, 0, 0, $W, $H)
    $g.DrawEllipse($pen, $W / 4, $H / 4, $W / 2, $H / 2)
    $g.Dispose()
    $fmt = if ($Name -like '*.jpg') { [System.Drawing.Imaging.ImageFormat]::Jpeg } else { [System.Drawing.Imaging.ImageFormat]::Png }
    $bmp.Save((Join-Path $dir $Name), $fmt)
    $bmp.Dispose()
}
New-Picture '1.png' 400 300 '#C0392B'
Start-Sleep -Milliseconds 30
New-Picture '2.jpg' 300 400 '#27AE60'
Start-Sleep -Milliseconds 30
New-Picture '10.png' 640 360 '#2980B9'
Start-Sleep -Milliseconds 30
New-Picture 'portrait.jpg' 240 320 '#8E44AD'
Start-Sleep -Milliseconds 30
New-Picture 'wide.jpg' 800 200 '#F39C12'
Set-Content -Path (Join-Path $dir 'notes.txt') -Value 'not a picture'

$store = Join-Path $env:LocalAppData 'WinDock\widget-data\photo-frame.json'
$storeBak = if (Test-Path $store) { $b = "$store.smoke.bak"; Copy-Item $store $b -Force; $b } else { $null }
$userRunning = @(Get-WinDockProcess).Count -gt 0
$bak = Backup-WinDockConfig
Write-Host "config backup: $bak"
try {
    Stop-WinDock
    if (Test-Path $store) { Remove-Item $store -Force }
    $cfg = @{
        hideTaskbar = $false
        tray        = @{ enabled = $false }
        docks       = @(
            @{ id = 'smoke'; edge = 'bottom'; align = 'center'; autoHide = 'off'
               items = @(@{ kind = 'clock' },
                         @{ kind = 'widget'; id = 'photo-frame'
                            settings = @{ source = 'folder'; folder = $dir; interval = 1; transition = 'fade'; transitionMs = 300
                                          size = 'medium'; aspect = '4:3'; border = 'mat'; shadow = 'soft'; debugStore = $true } }) },
            @{ id = 'frame1'; kind = 'widget'; widget = 'photo-frame'; x = 240; y = 200; w = 320; h = 260
               settings = @{ source = 'image'; image = (Join-Path $dir 'portrait.jpg'); aspect = 'image'; fit = 'cover'
                             border = 'polaroid'; borderWidth = 8; shadow = 'strong'; shadowOffset = 6; debugStore = $true } }
        )
    }
    [void](Start-WinDock -Exe $Exe -Config $cfg)
    Check 'dock window found (QueryState 51)' ((Get-DockWindow 0) -ne [IntPtr]::Zero)
    Check 'two docks[] entries (QueryState 47)' ((Get-DockState 47) -eq 2)
    Check 'slot 1 is a script widget (QueryState 24 = 6)' ((Get-DockState 24 -Arg 1) -eq 6)
    Check 'one script widget on the dock (QueryState 55)' (Wait-Until -TimeoutMs 4000 { (Get-DockState 55) -eq 1 })
    Check 'entry 1 is a standalone widget (QueryState 241 = 2)' ((Get-DockState 241 -Dock 1) -eq 2)
    Check 'the standalone widget runs (QueryState 244 = 0)' (Wait-Until -TimeoutMs 4000 { (Get-DockState 244 -Dock 1) -eq 0 })
    Check 'the dock widget drew (QueryState 58 > 0)' (Wait-Until -TimeoutMs 4000 { (Get-DockState 58 -Arg 1) -gt 0 })

    Start-Sleep -Milliseconds 3600  # three or four pictures at 1 s each
    $dockPng = Save-WindowImage -Hwnd (Get-DockWindow 0) -Path (Join-Path $OutDir 'photo-frame-dock.png')
    $standalone = [IntPtr](Get-DockState 51 -Dock 1)
    $framePng = if ($standalone -ne [IntPtr]::Zero) { Save-WindowImage -Hwnd $standalone -Path (Join-Path $OutDir 'photo-frame-standalone.png') } else { '' }
    Write-Host "pictures: $dockPng  $framePng"
    Check 'the standalone window exists (QueryState 51)' ($standalone -ne [IntPtr]::Zero)
    Check 'the dock widget keeps drawing (QueryState 58 >= 3)' ((Get-DockState 58 -Arg 1) -ge 3)

    # The Item window of the slideshow (ItemCommand 854): the `folder` field (schema index 2:
    # 3700 + 10 x 2) shows the folder in its box (3721) with a Browse… button (3727; not clicked:
    # the picker is modal); the hidden `image` field (index 1, visibleIf source = image) has none.
    [void](Send-ItemCommand -Command 854 -Item 1)
    $settings = [IntPtr]::Zero
    [void](Wait-Until -TimeoutMs 6000 { $w = @(Get-WinDockWindowsOfClass 'WinDock.Settings'); if ($w.Count) { $script:settings = $w[0] }; $w.Count -gt 0 })
    Check 'the Item window opened (class WinDock.Settings)' ($settings -ne [IntPtr]::Zero)
    if ($settings -ne [IntPtr]::Zero) {
        $box = Get-ChildControl -Hwnd $settings -Id 3721
        $browse = Get-ChildControl -Hwnd $settings -Id 3727
        Check 'the Folder field has its box (3721)' ($box -ne [IntPtr]::Zero)
        Check "the Folder box shows the folder" ($box -ne [IntPtr]::Zero -and (Get-ControlText $box) -eq $dir)
        Check 'the Folder field has a Browse… button (3727)' ($browse -ne [IntPtr]::Zero -and (Get-ControlText $browse) -like 'Browse*')
        Check 'the hidden Picture field has no Browse… button (3717)' ((Get-ChildControl -Hwnd $settings -Id 3717) -eq [IntPtr]::Zero)
        $itemPng = Save-WindowImage -Hwnd $settings -Path (Join-Path $OutDir 'photo-frame-item-window.png')
        Write-Host "picture: $itemPng"
    }

    Stop-WinDock  # flushes the widget storage
    Check 'widget storage written' (Test-Path $store)
    if (Test-Path $store) {
        $s = Get-Content -Raw $store | ConvertFrom-Json -AsHashtable
        $shown = @($s['shown:smoke/photo-frame#0'])
        $state = $s['state:smoke/photo-frame#0']
        $names = @($shown | ForEach-Object { Split-Path $_ -Leaf })
        Write-Host ("shown: " + ($names -join ', '))
        Check "the slideshow found 5 pictures (count $($state.count))" ($state.count -eq 5)
        Check "the slideshow showed at least 3 (shown $($shown.Count))" ($shown.Count -ge 3)
        Check 'in Explorer name order: 1.png, 2.jpg, 10.png' (($names.Count -ge 3) -and $names[0] -eq '1.png' -and $names[1] -eq '2.jpg' -and $names[2] -eq '10.png')
        Check 'the dock frame is 4:3 (pic.w / pic.h)' ([Math]::Abs($state.pic.w / $state.pic.h - 4 / 3) -lt 0.05)
        $f = $s['state:frame1']
        $r = if ($f) { $f.pic.w / $f.pic.h } else { 0 }
        Check "the standalone frame took the portrait picture's shape (3:4; got $([Math]::Round($r, 3)))" ([Math]::Abs($r - 0.75) -lt 0.03)
        Check 'the standalone frame fits its window' ($f -and $f.total.w -le $f.box.w + 0.5 -and $f.total.h -le $f.box.h + 0.5)
    }
} finally {
    try { Stop-WinDock } catch { Write-Warning $_ }
    if (Test-Path $store) { Remove-Item $store -Force }
    if ($storeBak) { Move-Item $storeBak $store -Force }
    Restore-WinDockConfig $bak
    if ($userRunning -and -not $NoRestart -and (Test-Path $UserExe)) { [void](Start-WinDock -Exe $UserExe) }
    if ($OutDir -ne $dir) { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
}
if ($fails.Count) { Write-Host "`n$($fails.Count) FAILED"; exit 1 }
Write-Host "`nall checks passed"
