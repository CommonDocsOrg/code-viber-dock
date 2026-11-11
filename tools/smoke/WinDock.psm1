# WinDock.psm1: the reusable smoke-test module (round 13 phase 13A). PowerShell 7.
#
# Usage:
#   Import-Module E:\repos\windows-dock\tools\smoke\WinDock.psm1 -Force
#   $bak = Backup-WinDockConfig                       # copy of %LocalAppData%\WinDock\config.json
#   Stop-WinDock                                      # quit the user's dock first (single instance)
#   try {
#       Start-WinDock -Exe build\dev\WinDock.exe -Config @{ docks = @(@{ edge = 'bottom'; items = @(@{ kind = 'clock' }) }) }
#       Get-DockState -Number 0                       # QueryState 0 of dock 0
#       Send-DockCommand -Command 116 -Post           # DockCommand (posted: returns at once)
#   } finally {
#       Stop-WinDock                                  # --quit, then: no WinDock process, Shell_TrayWnd visible
#       Restore-WinDockConfig $bak                    # byte for byte
#       Start-WinDock -Exe build\x64-release\WinDock.exe   # the user's own dock, own config (no -Config)
#   }
#
# Safety rules (CLAUDE.md "Runtime testing", STATUS.md "Testing cautions learned"):
#   - The dock runs on the user's live desktop: it hides the taskbar and may take over the tray.
#     Always end a run with Stop-WinDock (it checks Shell_TrayWnd and falls back to --restore-taskbar).
#   - Back up the config before writing a test config and restore it afterwards; test configs keep
#     "startWithWindows": true and set "reserveSpace": false (Start-WinDock enforces both).
#   - Only one instance can run: quit the user's dock first and restart it from build\x64-release
#     with the user's config restored at the end.
#   - Menus are modal on the dock's UI thread: drive them with POSTED messages (Send-MenuKey) or from
#     a job; a sent message that opens a menu does not return until the menu closes.
#   - Send real keys / clicks only while a WinDock window is the foreground / under the pointer.
#
# Messages (README "Automation (for tests)"): every dock-level message takes the dock in the HIGH
# word of lParam and the old argument (item index, ...) in its low word. They go to the always
# present WinDock.App window. Sent with SendMessageTimeout (5 s) so a stuck dock does not hang a test.

Set-StrictMode -Version Latest

if (-not ('WinDockSmoke.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
namespace WinDockSmoke {
public static class Native {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessageW(string s);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint msg, IntPtr wp, IntPtr lp, uint flags, uint ms, out IntPtr result);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetTopWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr m);
    [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr m, uint id, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetMenuStringW(IntPtr m, uint id, StringBuilder sb, int n, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr m, int pos);
    [DllImport("user32.dll")] public static extern bool GetMenuItemRect(IntPtr h, IntPtr m, uint pos, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);  // round 14 phase 14A (Get-DockScale)

    public static string ClassOf(IntPtr h) { var sb = new StringBuilder(256); GetClassNameW(h, sb, 256); return sb.ToString(); }
    public static string TextOf(IntPtr h) { var sb = new StringBuilder(512); GetWindowTextW(h, sb, 512); return sb.ToString(); }
    public static uint PidOf(IntPtr h) { uint pid; GetWindowThreadProcessId(h, out pid); return pid; }
    // Top-level windows of a class (EnumWindows: FindWindow(class, null) from PowerShell matches only
    // untitled windows, because $null arrives as ""). pid 0: any process.
    public static IntPtr[] ByClass(string cls, uint pid, bool visibleOnly) {
        var list = new List<IntPtr>();
        EnumWindows((h, lp) => {
            if (ClassOf(h) == cls && (pid == 0 || PidOf(h) == pid) && (!visibleOnly || IsWindowVisible(h))) list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list.ToArray();
    }
    // Position in the top-level z-order (0 = topmost window), -1 not found.
    public static int ZIndex(IntPtr target) {
        int i = 0;
        for (IntPtr h = GetTopWindow(IntPtr.Zero); h != IntPtr.Zero; h = GetWindow(h, 2 /*GW_HWNDNEXT*/), ++i) {
            if (h == target) return i;
        }
        return -1;
    }
    public static string MenuString(IntPtr m, int pos) { var sb = new StringBuilder(256); GetMenuStringW(m, (uint)pos, sb, 256, 0x400); return sb.ToString(); }
}
}
'@
}

$script:N = [WinDockSmoke.Native]
$script:Msg = @{}
foreach ($name in 'QueryState', 'QueryItemPoint', 'DockCommand', 'ItemCommand') {
    $script:Msg[$name] = $script:N::RegisterWindowMessageW("WinDock.$name")
}
$script:ConfigPath = Join-Path $env:LOCALAPPDATA 'WinDock\config.json'

function Get-WinDockProcess { @(Get-Process -Name WinDock -ErrorAction SilentlyContinue) }

function Find-WinDockWindows {
    <# Top-level windows of the running dock by class: App (WinDock.App), Docks (WinDock.DockWindow), Settings (WinDock.Settings), plus the pid. #>
    $p = Get-WinDockProcess | Select-Object -First 1
    $procId = if ($p) { [uint32]$p.Id } else { [uint32]0 }
    [pscustomobject]@{
        Pid      = $procId
        App      = $script:N::ByClass('WinDock.App', $procId, $false) | Select-Object -First 1
        Docks    = @($script:N::ByClass('WinDock.DockWindow', $procId, $false))
        Settings = @($script:N::ByClass('WinDock.Settings', $procId, $false))
    }
}

function Get-WinDockAppWindow {
    $w = (Find-WinDockWindows).App
    if (-not $w -or $w -eq [IntPtr]::Zero) { throw 'WinDock.App window not found (is the dock running?)' }
    $w
}

function ConvertTo-Int32([int64]$Value) {
    # 0xFFFFFFFF is int32 -1 in PowerShell and [int32] of a large int64 throws: take the low 32 bits.
    [BitConverter]::ToInt32([BitConverter]::GetBytes($Value), 0)
}

function Split-Words([int64]$Value) {
    # MAKELONG(lo, hi) of signed 16-bit halves (screen coordinates may be negative).
    $lo = $Value -band 0xFFFF; if ($lo -ge 0x8000) { $lo -= 0x10000 }
    $hi = ($Value -shr 16) -band 0xFFFF; if ($hi -ge 0x8000) { $hi -= 0x10000 }
    , @([int]$lo, [int]$hi)
}

function ConvertTo-LParam([int]$Dock, [int]$Arg) {
    # The dock in the high word, the argument in the low word (README "Which dock").
    [IntPtr]([int64](($Dock -band 0xFFFF) -shl 16) -bor ([int64]$Arg -band 0xFFFF))
}

function Send-WinDockMessage {
    param([Parameter(Mandatory)][string]$Name, [int64]$WParam = 0, [int64]$LParam = 0, [switch]$Post, [IntPtr]$Window = [IntPtr]::Zero)
    $h = if ($Window -ne [IntPtr]::Zero) { $Window } else { Get-WinDockAppWindow }
    $m = $script:Msg[$Name]
    if ($Post) { return [bool]$script:N::PostMessageW($h, $m, [IntPtr]$WParam, [IntPtr]$LParam) }
    $res = [IntPtr]::Zero
    # SMTO_ABORTIFHUNG (2), 5 s
    $ok = $script:N::SendMessageTimeoutW($h, $m, [IntPtr]$WParam, [IntPtr]$LParam, 2, 5000, [ref]$res)
    if ($ok -eq [IntPtr]::Zero) { throw "WinDock.$Name $WParam timed out" }
    [int64]$res
}

function Get-DockState {
    <# QueryState number n of dock i; -Arg goes in the low word of lParam. Returns the value (sign-extended 32-bit). #>
    param([Parameter(Mandatory)][int]$Number, [int]$Dock = 0, [int]$Arg = 0)
    $v = Send-WinDockMessage -Name QueryState -WParam $Number -LParam (ConvertTo-LParam $Dock $Arg)
    ConvertTo-Int32 $v  # the dock answers 32-bit values (-1, INT_MIN, MAKELONG pairs)
}

function Send-DockCommand {
    <# DockCommand (ids in src\ContextMenu.h / README). -Post for commands that open something modal. #>
    param([Parameter(Mandatory)][int]$Command, [int]$Dock = 0, [int]$Arg = 0, [switch]$Post)
    Send-WinDockMessage -Name DockCommand -WParam $Command -LParam (ConvertTo-LParam $Dock $Arg) -Post:$Post
}

function Send-ItemCommand {
    param([Parameter(Mandatory)][int]$Command, [Parameter(Mandatory)][int]$Item, [int]$Dock = 0, [switch]$Post)
    Send-WinDockMessage -Name ItemCommand -WParam $Command -LParam (ConvertTo-LParam $Dock $Item) -Post:$Post
}

function Get-DockItemPoint {
    <# Screen point of slot -Item's icon centre (rest layout) as [pscustomobject]@{X;Y}, or $null. #>
    param([Parameter(Mandatory)][int]$Item, [int]$Dock = 0)
    $v = Send-WinDockMessage -Name QueryItemPoint -WParam 0 -LParam (ConvertTo-LParam $Dock $Item)
    if ((ConvertTo-Int32 $v) -eq -1) { return $null }
    $xy = Split-Words $v
    [pscustomobject]@{ X = $xy[0]; Y = $xy[1] }
}

function Get-DockWindow {
    <# The HWND of dock i's window (QueryState 51). #>
    param([int]$Dock = 0)
    [IntPtr](Send-WinDockMessage -Name QueryState -WParam 51 -LParam (ConvertTo-LParam $Dock 0))
}

function Get-DockScale {
    <# Round 14 phase 14A: dock i's scale (its window's DPI / 96; 1.0 at 100 %), for DIP <-> px checks. #>
    param([int]$Dock = 0)
    $dpi = $script:N::GetDpiForWindow((Get-DockWindow -Dock $Dock))
    if ($dpi -le 0) { return 1.0 }
    [double]$dpi / 96.0
}

function Get-DockBarRect {
    <# The resting bar's screen rect of dock i (QueryState 237 / 238). #>
    param([int]$Dock = 0)
    $lt = Split-Words (Get-DockState 237 -Dock $Dock); $wh = Get-DockState 238 -Dock $Dock
    [pscustomobject]@{ Left = $lt[0]; Top = $lt[1]; Right = $lt[0] + [int]($wh -band 0xFFFF); Bottom = $lt[1] + [int](($wh -shr 16) -band 0xFFFF) }
}

function Wait-Until {
    param([Parameter(Mandatory)][scriptblock]$Condition, [int]$TimeoutMs = 5000, [int]$StepMs = 50)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds $StepMs
    }
    $false
}

function Backup-WinDockConfig {
    <# Copies config.json byte for byte; returns the backup path ($null when there is no config). #>
    param([string]$Path)
    if (-not (Test-Path $script:ConfigPath)) { return $null }
    if (-not $Path) { $Path = Join-Path ([IO.Path]::GetTempPath()) ("windock-config-{0:yyyyMMdd-HHmmss}.bak.json" -f (Get-Date)) }
    [IO.File]::WriteAllBytes($Path, [IO.File]::ReadAllBytes($script:ConfigPath))
    $Path
}

function Restore-WinDockConfig {
    param([Parameter(Mandatory)][string]$Path)
    [IO.File]::WriteAllBytes($script:ConfigPath, [IO.File]::ReadAllBytes($Path))
    $a = (Get-FileHash $Path).Hash; $b = (Get-FileHash $script:ConfigPath).Hash
    if ($a -ne $b) { throw "config restore mismatch ($Path)" }
}

function Test-TaskbarVisible {
    $t = $script:N::ByClass('Shell_TrayWnd', 0, $false)
    @($t | Where-Object { $script:N::IsWindowVisible($_) }).Count -gt 0
}

function Start-WinDock {
    <#
      Starts the dock. -Config: a path to a JSON file or a hashtable; it is written as the config
      (backup first!) with "startWithWindows": true and "reserveSpace": false on every docks[] entry.
      Without -Config the existing config is used (restarting the user's dock).
      -Environment (round 16 phase 16A): variables the dock process gets (e.g. WINDOCK_SENDKEYS_LOG);
      set in this process around Start-Process only (the child inherits them), then put back.
    #>
    param([Parameter(Mandatory)][string]$Exe, $Config, [int]$TimeoutMs = 10000, [hashtable]$Environment)
    $exePath = (Resolve-Path $Exe).Path
    if (Get-WinDockProcess) { throw 'a WinDock process is already running (Stop-WinDock first)' }
    if ($null -ne $Config) {
        $obj = if ($Config -is [string]) { Get-Content -Raw $Config | ConvertFrom-Json -AsHashtable } else { $Config }
        $obj['startWithWindows'] = $true
        foreach ($d in @($obj['docks'])) { if ($d -is [System.Collections.IDictionary]) { $d['reserveSpace'] = $false } }
        $json = $obj | ConvertTo-Json -Depth 20
        [IO.File]::WriteAllText($script:ConfigPath, $json, [Text.UTF8Encoding]::new($false))
    }
    $saved = @{}
    if ($Environment) {
        foreach ($k in $Environment.Keys) {
            $saved[$k] = [Environment]::GetEnvironmentVariable($k)
            [Environment]::SetEnvironmentVariable($k, [string]$Environment[$k])
        }
    }
    try {
        $p = Start-Process -FilePath $exePath -WorkingDirectory (Split-Path $exePath) -PassThru
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
    $ok = Wait-Until -TimeoutMs $TimeoutMs { $w = Find-WinDockWindows; $w.App -and $w.Docks.Count -gt 0 }
    if (-not $ok) { throw "WinDock did not come up ($exePath)" }
    Start-Sleep -Milliseconds 1000  # the UI thread is busy ~0.8 s after a start (icons)
    $p
}

function Stop-WinDock {
    <#
      Quits the running dock with --quit (the exe of the running process, or -Exe), waits, and asserts
      no WinDock process remains and Shell_TrayWnd is visible; on failure runs --restore-taskbar.
    #>
    param([string]$Exe, [int]$TimeoutMs = 10000)
    $procs = @(Get-WinDockProcess)
    if (-not $Exe) { $Exe = if ($procs) { $procs[0].Path } else { $null } }
    if ($procs -and $Exe) {
        Start-Process -FilePath $Exe -ArgumentList '--quit' -Wait | Out-Null
        [void](Wait-Until -TimeoutMs $TimeoutMs { @(Get-WinDockProcess).Count -eq 0 })
    }
    [void](Wait-Until -TimeoutMs 3000 { Test-TaskbarVisible })
    $left = @(Get-WinDockProcess)
    $bar = Test-TaskbarVisible
    if ($left.Count -gt 0 -or -not $bar) {
        if ($Exe) { Start-Process -FilePath $Exe -ArgumentList '--restore-taskbar' -Wait | Out-Null }
        throw "Stop-WinDock: processes left $($left.Count), taskbar visible $bar (ran --restore-taskbar)"
    }
}

# --- popup menus of the dock process (#32768) ---

function Get-WinDockMenus {
    <# The visible popup menu windows of the dock process, each with its HMENU (MN_GETHMENU) and rect. #>
    $procId = (Find-WinDockWindows).Pid
    if (-not $procId) { return @() }
    foreach ($h in $script:N::ByClass('#32768', $procId, $true)) {
        $res = [IntPtr]::Zero
        [void]$script:N::SendMessageTimeoutW($h, 0x01E1, [IntPtr]::Zero, [IntPtr]::Zero, 2, 2000, [ref]$res)  # MN_GETHMENU
        $r = New-Object WinDockSmoke.Native+RECT
        [void]$script:N::GetWindowRect($h, [ref]$r)
        [pscustomobject]@{ Hwnd = $h; Menu = $res; Rect = $r }
    }
}

function Get-MenuEntries {
    param([Parameter(Mandatory)][IntPtr]$Menu)
    $n = $script:N::GetMenuItemCount($Menu)
    for ($i = 0; $i -lt $n; $i++) {
        $st = $script:N::GetMenuState($Menu, [uint32]$i, 0x400)  # MF_BYPOSITION
        [pscustomobject]@{ Pos = $i; Text = $script:N::MenuString($Menu, $i); Popup = [bool]($st -band 0x10); Hilite = [bool]($st -band 0x80); State = $st }
    }
}

function Send-MenuKey {
    <# Posts a key to a menu window (no foreground needed; real keys could reach the user's window). #>
    param([Parameter(Mandatory)][IntPtr]$Hwnd, [Parameter(Mandatory)][int]$Vk)
    [void]$script:N::PostMessageW($Hwnd, 0x0100, [IntPtr]$Vk, [IntPtr]1)  # WM_KEYDOWN
    [void]$script:N::PostMessageW($Hwnd, 0x0101, [IntPtr]$Vk, [IntPtr]([int64]0xC0000001))  # WM_KEYUP
}

function Get-ClassAtPoint {
    param([int]$X, [int]$Y)
    $p = New-Object WinDockSmoke.Native+POINT
    $p.X = $X; $p.Y = $Y
    $script:N::ClassOf($script:N::WindowFromPoint($p))
}

function Get-ZIndex { param([IntPtr]$Hwnd) $script:N::ZIndex($Hwnd) }

# --- any window (round 13 follow-up: the Quick setup look) ---

function Get-WinDockWindowsOfClass {
    <# Top-level windows of the running dock of class -Class (e.g. WinDock.QuickSetup), visible only unless -All. #>
    param([Parameter(Mandatory)][string]$Class, [switch]$All)
    $p = Get-WinDockProcess | Select-Object -First 1
    if (-not $p) { return @() }
    @($script:N::ByClass($Class, [uint32]$p.Id, -not $All))
}

function Send-WindowMessage {
    <# A plain window message to -Hwnd (sent, 5 s timeout; -Post posts). -Msg 0x111 is WM_COMMAND. #>
    param([Parameter(Mandatory)][IntPtr]$Hwnd, [Parameter(Mandatory)][int]$Msg, [int64]$WParam = 0, [int64]$LParam = 0, [switch]$Post)
    if ($Post) { return [bool]$script:N::PostMessageW($Hwnd, [uint32]$Msg, [IntPtr]$WParam, [IntPtr]$LParam) }
    $res = [IntPtr]::Zero
    $ok = $script:N::SendMessageTimeoutW($Hwnd, [uint32]$Msg, [IntPtr]$WParam, [IntPtr]$LParam, 2, 5000, [ref]$res)
    if ($ok -eq [IntPtr]::Zero) { throw "message $Msg to $Hwnd timed out" }
    [int64]$res
}

function Get-WindowRect {
    param([Parameter(Mandatory)][IntPtr]$Hwnd)
    $r = New-Object WinDockSmoke.Native+RECT
    [void]$script:N::GetWindowRect($Hwnd, [ref]$r)
    [pscustomobject]@{ Left = $r.Left; Top = $r.Top; Right = $r.Right; Bottom = $r.Bottom }
}

function Get-StartHostWindow {
    <#
      Round 16 phase 16A: the Start menu's host window (class Windows.UI.Core.CoreWindow, title
      "Start", process StartMenuExperienceHost), as StartMenuPlacer::Host finds it; IntPtr.Zero if none.
      Check its rect (Get-WindowRect) before and after a test that may shift it.
    #>
    # FindWindowEx, not EnumWindows: since Windows 8 EnumWindows skips the shell's immersive windows
    # (measured: ByClass found no CoreWindow at all). Its own type, so a session that loaded the
    # module's Native type before this function existed still works.
    if (-not ('WinDockSmoke.Find' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace WinDockSmoke {
public static class Find {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
}
}
'@
    }
    $h = [IntPtr]::Zero
    while (($h = [WinDockSmoke.Find]::FindWindowExW([IntPtr]::Zero, $h, 'Windows.UI.Core.CoreWindow', 'Start')) -ne [IntPtr]::Zero) {
        $p = Get-Process -Id ($script:N::PidOf($h)) -ErrorAction SilentlyContinue
        if ($p -and $p.ProcessName -eq 'StartMenuExperienceHost') { return $h }
    }
    [IntPtr]::Zero
}

if (-not ('WinDockSmoke.Capture' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace WinDockSmoke {
public static class Capture {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
}
'@
}

function Save-WindowImage {
    <# A PNG of -Hwnd (waits -DelayMs for its paint). Round 13 phase 13H: PrintWindow with
       PW_RENDERFULLCONTENT, so the picture is the window's even when another window covers it (a
       window the script opens does not get the foreground while the terminal has it); the screen
       under its rect only if PrintWindow fails. #>
    param([Parameter(Mandatory)][IntPtr]$Hwnd, [Parameter(Mandatory)][string]$Path, [int]$DelayMs = 300)
    Add-Type -AssemblyName System.Drawing
    Start-Sleep -Milliseconds $DelayMs
    $r = Get-WindowRect $Hwnd
    $w = [Math]::Max(1, $r.Right - $r.Left); $h = [Math]::Max(1, $r.Bottom - $r.Top)
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    $ok = [WinDockSmoke.Capture]::PrintWindow($Hwnd, $dc, 2)  # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc)
    if (-not $ok) { $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size) }
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $Path
}

Export-ModuleMember -Function *
