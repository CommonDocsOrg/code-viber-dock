# WinDock: project status and how to resume

Last updated: 2026-11-02

## What this is
A macOS-style dock for Windows 11 in C++20 / Win32 + Direct2D + DirectComposition, zero third-party dependencies.
Full design: `C:\Users\olson\.claude\plans\need-you-to-come-async-grove.md` (copy it into the repo as `PLAN.md` if that path is gone).

## Progress
| Phase | Status | Notes |
|---|---|---|
| 1 Skeleton, DComp window, clock, tray, config | Done, verified | 272 KB exe, 0% idle CPU |
| 2 Taskbar hide/restore, crash marker, Explorer re-hide | Done, verified | Also fixes Explorer persisting auto-hide to `StuckRects3` (undocumented registry format) |
| 3 Running apps: shell hook tracker, icons, click/minimize/cycle, item menu | Done, verified | Icons come from package manifests / `SHDefExtractIcon`, shell factory only as fallback |
| 4 Pinning: config pins, .lnk resolution, launch + bounce, drag-drop pin, default pins | Done, verified (agent report reviewed; drag-drop path builds but was not exercised, test it by hand once) | 448 KB exe. App-execution-alias targets resolve to package AUMIDs so Store-app shortcuts merge with running windows |
| 5 Magnification, hover label, auto-hide state machine, fullscreen suppression, tray toggles, autostart | Done, verified | 484 KB exe. Also fixed visual z-order bug that dimmed icons. Real DPI change and real D3D fullscreen game untested |
| 5.5 Fix: HWND sized to bar at rest so transparent stage area no longer blocks clicks | Done, verified | Root visual translation; window expands on hover, compacts ~170 ms after leave |
| 6a Acrylic blur background | Done, verified | Second `WinDockBackdrop` HWND with DWM system backdrop; corner radius snaps to system 8 DIP; `theme.acrylic`, `theme.acrylicBackground` |
| 6b Drag to reorder pins + IDropTarget insertion marker | Done, verified | Unpin by dragging a non-running pin away; dragging unpinned running items into the pinned section pins them; `theme.accent` |
| 6c Window previews on hover | Done, verified | `PreviewPopup` on a second DComp target; DWM live thumbnails; `previews`, `previewDelayMs` |
| 7 Settings, themes, animation styles, intelligent hide (plan: `C:\Users\olson\.claude\plans\plan-a-proper-settings-fuzzy-pumpkin.md`) | Done, verified | A1 schema + theme presets + user theme files + tri-state auto-hide: f0cb280. A2 animation styles (easing, speed, hide/launch styles, label fade, reduce motion): cd658bb. A3 intelligent hide (ForegroundWatch): 5c364a2. B1 settings window shell (dark mode, Mica nav, General / Behaviour / About): ef0710a. B2 Appearance page with per-field overrides and Export, live D2D preview strip, Animation and Pinned apps pages: the commit after ef0710a. 760 KB exe. Not tested on real hardware: a move between monitors of different DPI (simulated DPI change passes); dock pixels after a colour change were verified by state only because the display was asleep |
| 8.0 Groundwork for folders / custom icons / weather / icon effects (plan: `C:\Users\olson\.claude\plans\review-this-app-compared-snazzy-nest.md`, Phase 0) | Done, verified | Pin schema `pinned[].icon` / `pinned[].kind`; item kinds App / Folder / Separator / RecycleBin (`DockItem::Kind`); folders detected from the path or a .lnk target (a folder and a shortcut to it dedupe); separators and the Recycle Bin as synthetic keys. IconCache: stale-icon fix (entries match icon file + index), WIC images (.png .jpg .jpeg .bmp .gif), system-image-list folder / Recycle Bin icons, `FromHicon`, `DominantColor`. `PopupWindow` base (hover and click models) with `PreviewPopup` as a hover subclass; `previewHold_` became `popupHolds_` bits. JsonUtil `ReadArray` / `ReadNumbers` / `ReadStrings`. QueryState 24 / 25 / 35. Separators still render as placeholder tiles (Phase 1). A folder or Recycle Bin pin costs ~3 MB private bytes (the shell's system image list). 775 KB exe. The click-popup model has no user yet, so it is untested at runtime |
| 6 Other extras (multi-monitor docks, icon atlas, live preview titles) | Not started | |

Everything through phase A3 is committed; commit after each further phase.

## Decisions made by the user (keep these)
- Renderer default stays `"auto"` (hardware D3D11, WARP fallback). `"software"` is available in config.json for ~8 MB private bytes instead of ~65 MB; the difference is the NVIDIA user-mode driver.
- No working-set trimming hacks (no `SetProcessWorkingSetSize`). Report real numbers.
- Taskbar fully hidden while running; dock floats and auto-hides (macOS behaviour, Phase 5).
- Efficiency over features; no large dependencies.

## Build and run
```
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && cd /d E:\repos\windows-dock && cmake --preset x64-release && cmake --build --preset x64-release'
E:\repos\windows-dock\build\x64-release\WinDock.exe
E:\repos\windows-dock\build\x64-release\WinDock.exe --quit
E:\repos\windows-dock\build\x64-release\WinDock.exe --restore-taskbar
```
Config: `%LocalAppData%\WinDock\config.json`. Crash marker: `%LocalAppData%\WinDock\taskbar-hidden`.

## If the taskbar is missing
The dock hides it. Run `WinDock.exe --restore-taskbar`, or simply launch and quit the dock. If that fails, restart Explorer: `taskkill /f /im explorer.exe && start explorer`.

## Testing cautions learned
- Do not use Notepad as a test app on this machine; it restores unsaved tabs and pops a save dialog.
- `explorer.exe C:\` opens tabs in the existing window on this build, so it cannot be used for a two-window test. Use two `mspaint.exe` windows.
- Listary's `SearchBarWindow` appears next to file dialogs; the tracker ignores `WS_EX_NOACTIVATE` windows without `WS_EX_APPWINDOW` for this reason.
- Always finish a test run with `--quit` and confirm `Shell_TrayWnd` is visible and no `WinDock` process remains.
- Hidden test messages (registered window messages, all documented in README.md): `WinDock.QueryItemPoint`, `WinDock.ItemCommand`, `WinDock.DockCommand` (100 auto-hide toggle, 101 hide taskbar, 102 autostart, 103 open config, 104 show taskbar until re-enabled, 105 quit, 106 reload config, 107 settings window, 108/109/110 auto-hide off/always/intelligent), `WinDock.QueryState` (0 auto-hide state, 1 notification state, 2 hovered item, 3 label visible, 4 magnifying, 5 pending timers, 11/12/13 preview popup visible / cards / card window, 14 auto-hide mode, 15 overlapped, 17 reduce motion, 19/21 foreground-watch counters, 20 theme resolve failed, 24 item kind, 25 item custom icon, 35 item dominant colour; 24/25/35 take the item index in `lParam`).
- QueryState 5 does not count launch bounces. To check that a click did not bounce, watch the dock HWND's height: a bounce keeps it at stage size (128 px at 100 %) for up to 5 s, otherwise it compacts to the bar (64 px) about 200 ms after the mouse leaves.
- Test configs must keep `"startWithWindows": true` (the user's setting): Reload config (DockCommand 106) applies it to the Run key.
- With `autoHide` `"off"`, a window that is maximized and activated (two `mspaint.exe` windows open maximized here) can end up above the topmost dock, so hover tests miss the dock. Restore and shrink the test windows first (`ShowWindow(SW_RESTORE)` + `MoveWindow`). Seen while testing 8.0; z-order code was not changed, so this is most likely an existing gap of the "off" mode (only hide transitions re-assert topmost).
- The machine may be in use during a test run (during the 8.0 run the pins were reordered and the cursor moved without the test script doing it); re-read the item list before asserting on positions or order.
- Folder / Recycle Bin click tests open Explorer views in the user's Explorer window (tabs); leave them, closing a tab window via `Shell.Application` can close the user's other tabs.
- PowerShell P/Invoke: pass `[NullString]::Value` (not `$null`) as the title to `FindWindowW`; `$null` binds as "" and matches only untitled windows. Build from PowerShell: in Git Bash `cmd /c` gets its `/c` path-mangled.

## How to resume
1. Open this repo in Claude Code and say: "Resume WinDock from STATUS.md." v1 plus the user-selected extras (acrylic, drag reorder, window previews) are complete as of 2026-11-01. The next feature round (folder stacks, separators, Recycle Bin, custom icons, weather + calendar, glow / reflection / perspective) is planned in `C:\Users\olson\.claude\plans\review-this-app-compared-snazzy-nest.md`; Phase 0 (groundwork, row 8.0) is done, Phase 1 (folder popup, separators, Recycle Bin) is next.
2. Phase 7 (settings window, themes, animation styles, intelligent hide) is complete. Open follow-ups noted during review: the settings window's keep-dock-shown rule follows the window's own activation, so a dialog closed while WinDock is in the background leaves auto-hide off until the window is activated or closed; `App::RefreshSettings()` after a pin change reloads every page (a half-typed hex value would be overwritten if an icon is dragged on the dock at that moment); the preview strip's clock shows the time of its last repaint; ChooseColor stays light-themed. Not in scope this round, by the user's choice: following the Windows light/dark mode or accent colour for the dock (a `"system"` preset id), a mica/none backdrop choice, click/middle/wheel action settings, display toggles, dock edge/position. Remaining unplanned extras: multi-monitor docks; pack icon surfaces into one atlas to cut ~4 MB; live title updates in the preview popup.
3. Settings window automation: open with `WinDock.DockCommand` 107 or `WinDock.exe --settings`; `WinDock.QueryState` 16 reports it; window class `WinDock.Settings`, control ids in `src/SettingsIds.h`; test scripts from this round live in the session scratchpad (`tools\b1.ps1`, `b2*.ps1`), not in the repo.
4. Manual checks still open: drop a .lnk onto the dock (WM_DROPFILES pinning); a real DPI change (dock and settings window); a real exclusive-fullscreen game with auto-hide (always and intelligent); Steam's label shows "Client" (display-name lookup picks the wrong version-resource string).
