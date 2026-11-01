# WinDock

A macOS-style dock for Windows 11. C++20 / Win32, Direct2D + DirectComposition, no third-party
dependencies. Single static exe (no VC++ redistributable needed).

Status: Phase 2. A translucent bar with a live clock at the bottom-center of the primary
monitor, plus a tray icon. The Explorer taskbar is hidden while the dock runs and restored on
exit, crash recovery, or `--restore-taskbar`. Running apps and pinning come in later phases.

## Build

Requires Visual Studio 2022+ (MSVC, CMake, Ninja) and the Windows 10/11 SDK. Run from a
developer environment so `cl.exe` and `ninja` are on `PATH`:

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset x64-release
cmake --build --preset x64-release
```

Output: `build\x64-release\WinDock.exe`. Use the `x64-debug` preset for a debug build.

## Run

```bat
build\x64-release\WinDock.exe                     :: start the dock (only one instance runs)
build\x64-release\WinDock.exe --quit              :: ask the running dock to exit
build\x64-release\WinDock.exe --restore-taskbar   :: bring the Explorer taskbar back
```

Right-click the tray icon or the dock for the menu (Open config file, Restore taskbar, Quit).

While hiding the taskbar the dock writes `%LocalAppData%\WinDock\taskbar-hidden`, holding the
original taskbar state. If the dock is killed or crashes, the next start (or
`WinDock.exe --restore-taskbar`, which works without a running dock) restores from it. The Start
menu still opens with the Win key while the taskbar is hidden.

## Configuration

`%LocalAppData%\WinDock\config.json` is created with defaults on first run. Restart the dock
after editing it. If the file is malformed, the dock starts with the defaults and leaves the file
untouched.

```json
{
  "pinned": [],
  "iconSize": 48, "magnification": 1.5, "magnifyRange": 2,
  "autoHide": true, "hideDelayMs": 400, "revealDelayMs": 150,
  "monitor": 0, "hideTaskbar": true, "startWithWindows": false, "renderer": "auto",
  "clock": { "show": true, "format": "%H:%M", "showDate": false },
  "theme": { "background": "#CC202020", "cornerRadius": 16 }
}
```

- `clock.format` uses `strftime` specifiers (`%H:%M`, `%I:%M %p`, `%H:%M:%S`, ...). Formats with
  seconds tick every second; otherwise the clock wakes once a minute.
- `theme.background` is `#AARRGGBB` (or `#RRGGBB`).
- `monitor`: 0 is the primary monitor, 1..n are the others.
- `hideTaskbar`: hide the Explorer taskbar while the dock runs (read at start).
- `renderer`: `"auto"` (GPU, falling back to software) or `"software"` (always WARP). Software
  rendering avoids the GPU driver's per-process memory; the dock redraws rarely, so it is cheap.
