# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

WinDock: a macOS-style dock for Windows 11. C++20 / Win32, Direct2D + DirectComposition, one static exe (static CRT). The only vendored dependency is QuickJS-ng (`third_party/quickjs-ng`, compiled in as the `quickjs` static library), which runs the script widgets. User decisions that constrain design: efficiency over features, no large dependencies, renderer default stays `"auto"`, no working-set trimming hacks.

Where the knowledge lives (all three are very large; grep or read by section, never whole):

- `README.md`: the full reference: every setting, the `config.json` schema, and the automation messages ("Automation (for tests)").
- `STATUS.md`: what each phase did and verified, "Testing cautions learned" (read before any runtime test), and "How to resume".
- `ROADMAP.md`: the history and design notes of rounds 4+.
- `docs/`: user guides (also compiled into the exe, see below) and `docs/widgets.md`, the widget developer reference.

## Build, run, test

MSVC only, via CMake presets + Ninja, from a Visual Studio developer environment (`vcvars64.bat`).

```bat
run.bat --norun        :: finds VS with vswhere, configures and builds x64-release (first quits a running dock)
run.bat                :: the same, then starts the dock
run.bat --stop         :: quits the running dock and restores the taskbar
```

By hand: `cmake --preset x64-release` then `cmake --build --preset x64-release` (also `x64-debug`). Output: `build\x64-release\WinDock.exe`.

- From Claude's shells: the Bash tool mangles `cmd /c` and unescapes backslashes. Build from PowerShell: write a small `.cmd` (`call vcvars64.bat`, the two cmake lines) into the scratchpad and run `& cmd /c <path>\build.cmd`.
- The user's own dock runs from `build\x64-release` and the exe is locked while it runs. Build work in progress into a separate tree (`build\dev`, same flags) and rebuild the preset only at the end. Only one instance can run (mutex), so a test dock needs the user's dock quit (`--quit`) and restarted afterwards with their config restored.
- New source files must be added to the `add_executable(WinDock ...)` list in `CMakeLists.txt` (no globbing for sources). `/W4 /permissive-`; keep builds warning-free.

Tests and checks:

```bat
cmake --build --preset x64-release --target WinDockTests
build\x64-release\WinDockTests.exe            :: ICS parser cases in tests\ics (<name>.ics + <name>.expected); exit code = failures
python tools\check_docs.py                    :: guide links, anchors, the help topics the source opens, Help-window Markdown subset
python tools\check_widget_docs.py             :: docs\widgets.md vs the script API bound in QuickJsEngine.cpp / WidgetPackage.cpp
build\x64-release\WinDock.exe --validate-widget examples\minimal   :: manifest check of one widget folder (exit 0 / 1 / 2)
```

`WinDockTests` is the only unit test target (the ICS parser). There is no single-test filter: it runs every `.ics` in the folder given as its argument (default `tests\ics`), so point it at a folder holding one case to run one. Everything else is verified by driving the real exe through the automation messages.

Command line of the exe: `--quit`, `--restore-taskbar`, `--settings`, `--welcome`, `--validate-widget <folder>` (handled in `src/main.cpp`; the first four signal the running instance through registered messages).

## Runtime testing (automation)

The dock exposes registered window messages for tests: `WinDock.QueryState` (read state by number), `WinDock.QueryItemPoint`, `WinDock.DockCommand` (menu commands, ids in `ContextMenu.h`), `WinDock.ItemCommand`. Send them to the always-present app window (class `WinDock.App`) or any dock window (`WinDock.DockWindow`); the target dock is the **high word of `lParam`**. Every number is documented in README "Automation (for tests)"; implementations are `App::QueryState`, `DockWindow::QueryState`, `FolderWindow::QueryState`, `WidgetWindow::QueryState`.

- A new feature gets new QueryState / DockCommand / ItemCommand numbers (appended, never renumbered) and a README entry; settings controls get stable ids in `src/SettingsIds.h` ("never renumber; add at the end of a page's range").
- Settings windows (class `WinDock.Settings`) are driven by control id with `WM_COMMAND` etc.; pages are created eagerly so hidden controls can be driven.
- Test scripts live in `tools\smoke\`: `WinDock.psm1` is the reusable P/Invoke module (find the windows, the four registered messages, config backup / restore, start / quit with the taskbar check; its header has the usage and safety rules), plus one short `<feature>.ps1` per feature (e.g. `menu-zorder.ps1`). Add to the module instead of writing new P/Invoke.

Safety rules when running the dock (it runs on the user's live desktop; details in STATUS.md "Testing cautions learned"):

- The dock hides the Explorer taskbar and, with the tray enabled, takes over `Shell_TrayWnd`. Always end a run with `--quit` and confirm `Shell_TrayWnd` is visible and no `WinDock` process remains. Recovery: `WinDock.exe --restore-taskbar`.
- Config is `%LocalAppData%\WinDock\config.json` (the user's real one). Back it up before a test config and restore it after; test configs keep `"startWithWindows": true` (Reload applies it to the Run key) and set `"reserveSpace": false` unless testing it (it resizes the user's maximized windows).
- Test widgets go in `%LocalAppData%\WinDock\widgets\<id>\` and write `widget-data\<id>.json`; remove both after.
- Do not use Notepad or `explorer.exe` as test windows (use two `mspaint.exe`, only if none is already running). Never run Empty Recycle Bin (ItemCommand 807) against the user's bin. Send real keys / clicks only while a WinDock window (or one the script started) is the foreground / under the pointer.
- Repo files are CRLF: multi-line `.Replace()` from PowerShell here-strings silently fails; use the Edit tool.

## Architecture

Single UI thread (STA + OLE); widget scripts also run on it (hence the 8 MB stack in `CMakeLists.txt`). Worker threads are short-lived (HTTP, weather, notifications polling, folder watch) and post results back as `WM_APP + n` messages. Timer ids and `WM_APP` offsets are allocated by hand across classes and cross-referenced in comments (`DockWindow.h` has the main table): pick an unused one and add it to those comments. Idle cost matters: timers are one-shot and nothing ticks while idle.

- **`App`** (`App.cpp`): wires everything; owns the hidden `WinDock.App` window (tray icon, broadcasts, shared services' messages), the `Config`, the `TaskbarController` (hide / restore the taskbar, crash marker `%LocalAppData%\WinDock\taskbar-hidden`), all settings / search / preset / help windows, menus, the debounced config save, and automation dispatch.
- **`DockManager`** (implements `DockShared`): owns one instance per `docks[]` entry (per monitor for `"monitor": "each"`; parked when its monitor is gone) and the process-wide services: `WindowTracker` (shell hook, running apps), `ForegroundWatch` (intelligent auto-hide), `WeatherService` per place, `TrayHost`, `NotificationService`, `CalendarService`, `WidgetRuntime`. A `docks[]` entry is one of three kinds: a dock (`DockWindow`), a desktop folder window (`FolderWindow`), or a standalone widget (`WidgetWindow`), all addressed by the same index. `Sync()` reconciles windows with config + monitors.
- **`DockWindow`** (`DockWindow.cpp`, ~465 KB, plus `DockWindowTray.cpp`): the dock HWND: placement, DPI, layout, input, magnification, auto-hide state machine, drags / drops, popups. Its content comes from **elements** (`DockElement.h`; `WidgetElements.cpp`, `TrayElements.cpp`, `NotificationElements.cpp`): one `DockElement` per `items[]` entry, each contributing 0..n **slots** (`DockItem`). Elements hold no visuals and are rebuilt when the item list changes; the dock owns slots and their composition visuals. Elements measure in main / cross axes (never x / y) and `DockGeometry.h` maps them to the edge / shape (bar, arc, circle).
- **Rendering**: `Renderer` (D3D11 / WARP → D2D → DirectComposition). Animations are `Tween`s handed to DWM as cubic segments, with a CPU-side closed form so followers (the separate acrylic `Backdrop` HWND) stay in step. `IconCache`, `IconEffects` (glow / reflection), `Theme` (a field table with presets and per-field override masks).
- **Popups**: `PopupWindow` base (hover and click models, hold bits keep the dock shown) with `PreviewPopup`, `FolderPopup`, `WeatherPopup`, `CalendarPopup`, `NotificationPopup`, `ScriptPopup`, `ReminderBanner`. `ItemGridView` is shared by folder popups and folder windows.
- **Config** (`Config.h/.cpp`, `JsonUtil.h` over `Windows.Data.Json`): the structs are the schema. The whole file is rewritten atomically on save; unknown top-level keys are kept, unknown keys inside known objects are dropped (an item's `settings` object is kept as is). Older layouts are migrated on load.
- **Widgets**: `WidgetPackage` (manifest, validation, catalog; built-in vs `%LocalAppData%\WinDock\widgets`), `WidgetInstance` (one QuickJS runtime per instance, implements `ScriptHost`), `QuickJsEngine.cpp` (the only file that touches QuickJS: native bindings + the JS prelude), `ScriptEngine.h` (engine-neutral interface), `WidgetCanvas` (the D2D drawing API), `WidgetRuntime` (shared timer scheduler, storage, http, folder watch), providers (`AudioService`, `MediaService`, `PerfService`, weather, calendar, notifications), `WidgetPreview` + `WidgetSamples` (settings previews with sample data), `WidgetConsole`.
- **Settings UI**: hand-built Win32, no dialog resources. `SettingsWindow` hosts three kinds of window (`SettingsKind`: Global with a nav column, Dock with tabs, Item with one generated page). A `SettingsPage` subclass builds a table of `Ctl` rows (DIP rects, cards, expanders, hints, tooltips, help topics); the window does creation, theming (`DarkMode`), reflow and scrolling. Pages live in `SettingsPages*.cpp`; `SettingsForm.h` generates Item pages from a widget manifest's settings schema. A change calls the host with a `SettingsScope` saying what to re-apply. `SettingsSearch` indexes every window's controls.

### Embedded resources (things to keep in sync)

Built-in widgets (`widgets/*/widget.json` + `main.js`), clock faces (`res/clock-faces`), dock presets (`res/presets`) and the user guides (`docs/*.md`) are RCDATA in the exe. Adding one means: an id in `src/resource.h`, a line in `res/app.rc`, and an entry in the table that maps it (`WidgetPackage.cpp`, `ClockFaces.cpp`, `Presets.cpp`, `HelpWindow.cpp`).

- The guides are rendered by the Help window's own Markdown converter: no tables, images, HTML or block quotes in `docs/quick-start.md`, `guide-*.md`, `troubleshooting.md`, `README.md`. Help topics in source are `"guide-x.md#anchor"` literals; `tools/check_docs.py` checks they resolve.
- Changing the script API (bindings or prelude in `QuickJsEngine.cpp`, manifest / permission keys in `WidgetPackage.cpp`) requires the matching edit in `docs/widgets.md`; `tools/check_widget_docs.py` reports drift.
- A new setting touches: `Config.h/.cpp` (struct, read, write), its settings page (with a tooltip or hint, and a help topic for a new card), README (Settings + Configuration + Automation ids), and the user guide.

## Conventions

- Work is organised in numbered rounds and phases; code comments tag the round / phase that introduced something. One commit per phase, with STATUS.md (and ROADMAP.md for the round) updated in the same change, including what was **not** verified.
- Comments explain the why and record measured Windows behaviour (undocumented quirks, things that were tried and failed). Keep that style; do not strip such notes.
- Anything that can leave the desktop broken on a crash (hidden taskbar, app bar band, tray takeover) has an emergency-restore path called from the unhandled-exception filter in `main.cpp`; new such state needs one too.
