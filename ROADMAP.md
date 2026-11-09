# WinDock roadmap: feature round 4 and beyond

Written 2026-11-02 against commit 4a61f6d. Companion to STATUS.md.

## Done (2026-11-03)

Round 4 is complete: every phase below is done except Phase 10 (virtual desktops), which the user
parked, and "Phase 5 round 2" (extra widgets, never scheduled). One commit per sub-phase, all on
master, none pushed by the agents (STATUS.md's "Round 4" table has the commits' notes, what was
verified and how).

| Phase | Status | Commits |
|---|---|---|
| 0 Quick wins | Done (with 1c) | 76cd3f2 |
| 1 Element model | Done | d0713d3 (1a), 8f63bfd (1b), 76cd3f2 (1c) |
| 2 Geometry: edges, alignment, full width, reserve space, peek, drag the dock | Done | 7009e8c (2a), 6c28397 (2b) |
| 3 Multiple docks, monitors, desktop layer, Layout canvas | Done | 49b798e (3a), 6eb2def (3b) |
| 4 Widget runtime (QuickJS-ng), settings, storage, http, shell, popups, Console | Done | 88eabd6 (4a), d6689c6 (4b) |
| 5 Bundled widgets round 1: analog clock + faces, calendar page, audio / media providers, volume, media, start, search, bell | Done | 81b79f8 (5a), 12241cc (5b) |
| 6 Task list: unstacked windows, titles, minimize to dock, thumbnails | Done | 23affaf (6a), 8ce0d03 (6b) |
| 7 Folders and groups: ItemGridView, folder popups, shortcut groups | Done | dda093d (7a), 38e5e50 (7b) |
| 8 System tray takeover, tray grid / drawer, trayApp pins, tray dock | Done | 9381dea (8a), 100edbf (8b) |
| 9 Notifications: service, badges, provider, notification centre, balloons | Done | ed3b7b0 (9a), d415dde (9b) |
| 10 Virtual desktops | Parked (user decision) | |
| 11 Shapes and overflow: scrolling docks, half-circle and circle docks | Done | f6ad5a1 (11a), 6394591 (11b) |
| 12 Desktop folder windows: FolderWindow, rules, appearance, clipboard | Done | fe195d1 (12a), c194d4b (12b) |
| 13 Calendars: ICS feeds, popup events, provider, agenda, reminders | Done | 8a5018c (13a), dd18f42 (13b) |
| 14 Built-ins as widgets (weather script + provider), digital clock faces, examples, validation, settings UI, docs | Done | 8fa0317 (14a), the 14b commit |

### Not verified (needs an unlocked session / a second display / the user)

Collected from STATUS.md's round-4 rows; every item is a code path the scripted tests could not
drive here:

- **Second display**: monitor indexes and `"each"` with two copies, parking by unplugging,
  `WM_DPICHANGED` across monitors of different DPI, the monitor running filter with windows on two
  monitors, reserve space on a second monitor, a canvas drag onto another display (3a, 3b, 11b).
- **A real DPI change** (any phase: widgets, popups, titles, effects, thumbnails, faces).
- **Unlocked-session input**: verified on 2026-11-03 by real input except the drag of a group
  member / tray icon with auto-scroll, an OLE drag out of a folder window to Explorer, the Open
  faces folder button and URL clicks (STATUS.md "Unlocked-session verification"; two bugs fixed,
  open follow-ups listed there). Originally (the session was locked during phases 9 - 14, so input
  was sent messages and pixels PrintWindow pictures): real hover / magnification on scrolling and ring docks,
  the strip / hub reveal by hovering, OLE file drags and drops onto scrolling / ring docks, folder
  windows (drag out to Explorer, drops from Explorer, real keyboard focus), the clipboard (12b:
  `OpenClipboard` is refused while locked), the reminder banner and agenda popup by a real click,
  the notification centre's pixels, the weather script popup by real clicks (14a), the acrylic blur
  of rings and folder windows (not in PrintWindow pictures).
- **Explorer**: a real Explorer restart with the tray takeover or folder windows (the recovery ran
  through test hooks), a real Win+D / Show desktop.
- **Hardware / apps**: a precision touchpad wheel, an exclusive-fullscreen game with auto-hide,
  ABN_FULLSCREENAPP with a real fullscreen app, an elevated tray app, a DRM video window's
  thumbnail, switching the default audio output on this machine, a device loss with live
  thumbnails, a real midnight / DST transition for the widgets (the fake clock path only).
- **Would touch the user's data**: Empty Recycle Bin with the user's items, opening an event URL
  in the user's browser, rules on the real Desktop.

## Why this order

The wish list touches five structural axes of the current code, and almost every feature sits on
one or more of them:

| Axis | Today | Needed by |
|---|---|---|
| **What a dock holds** | `items_` (apps / folders / separators / bin) plus two hard-coded widgets (`clockLayer_`, `weatherLayer_`) with their own tweens and hit tests | start button, search, volume, media, tray grid, analog clock, custom widgets, reorderable contents, lock, thumbnails-as-items |
| **Where and how it lies** | bottom edge, horizontal, centred, one `Layout` struct with x/y baked in everywhere | top / left / right, custom position, full width, peek strip, titles, circle / half circle, scrollable |
| **How many** | `App` owns one `DockWindow`; `Config` is one dock | multiple docks, per-monitor docks, mirrored docks, separate tray dock, desktop-only docks |
| **What runs inside** | C++ only | custom widgets, rewriting the built-ins as widgets, hot reload, dev tools |
| **What the OS tells us** | shell hook window list, icons, flash | tray icons, notifications, badges, virtual desktops, media, volume, calendars |

The first four axes are foundations: each one is a refactor that has to touch most of
`DockWindow.cpp` (3,700 lines). Doing a feature before its foundation means building it twice.
So the plan is: four foundation phases that change no visible behaviour (the existing
automation messages must keep passing), then feature phases that each add one thing on top.

Rules that keep us from redoing work:

1. **One config migration.** Phase 1 introduces the final schema (`docks[]`, each with
   `items[]`), even though only one dock is allowed until Phase 3. `pinned[]` is migrated once.
2. **Elements are axis-aware from day one.** The element interface in Phase 1 takes a layout
   axis even though only "bottom" is implemented until Phase 2, so no element is rewritten for
   vertical docks.
3. **New widgets are written as scripts, not C++.** The widget runtime (Phase 4) comes before
   the volume, media, analog clock, start and search widgets, so they are written once. The
   native parts they need (Core Audio, media sessions, shell) are "providers" exposed to scripts.
4. **Reusable views, not popups.** The folder grid / list (Phase 7) becomes a control used by
   folder popups, shortcut groups, the tray drawer and later the desktop folder docks.
5. **Risky OS integrations get a spike first.** Tray takeover, notification listener access,
   virtual desktops and in-dock thumbnails each start with a one-day throwaway prototype that
   answers the feasibility question before the phase is designed.

## Decisions (2026-11-02)

- Third-party dependencies are allowed when they earn their keep. Script engine: QuickJS-ng
  (vendored, MIT) behind a `ScriptEngine` interface; Chakra.dll as an optional alternative.
- Tray: take over `Shell_TrayWnd` (notify icons only; Explorer's own items are covered by widgets).
- Thumbnails as items: user choice "Live (GPU capture)" vs "Snapshots (every N seconds)", with
  the trade-off explained in the setting.
- Badges: best effort (notification listener counts, overlay icons / progress if they reach us;
  sparse MSIX package if the listener needs identity).
- Virtual desktops: skipped for now (Phase 10 parked).
- Calendars: ICS feeds only (Google secret iCal URL, Outlook published calendar URL).
- Date: a `date` widget (skeuomorphic flip-calendar page / calendar icon with the day), plus
  date on the analog face (day of month, month and day, year as options) and on the digital
  clock. Each is independent: separate widget, in the clock, both or neither.
- Settings window: reshape as needed; drag-and-drop editing of docks, widgets, start button,
  tray icons, everything. Two editing surfaces: on-dock edit mode (Phase 1, cross-dock drags in
  Phase 3) and a monitor canvas in the settings window with a widget / tray palette (Phase 3).
- Feature order after the foundations: 5 widgets round 1, 6 task list, 8 tray, 7 folders and
  groups, 9 notifications and badges, 11 shapes, 13 calendars (ICS), 12 desktop folder docks,
  14 examples and weather port. Phase 0 ships together with Phase 1.
- Multi-monitor: no second display yet; Phase 3 is verified single-monitor (plus a mirrored
  "each" dock) and real two-display checks wait until one is plugged in.
- Full-width mode: "Reserve screen space" is a user setting, default on.
- Reserve screen space on every dock (2026-11-02): `reserveSpace` (default on) applies to any dock
  on any edge with auto-hide "off", full width or not (auto-hide "always" / "intelligent" reserve
  nothing, as in 2b). The band is the resting bar's cross extent and spans the whole edge (an app
  bar cannot reserve part of one); magnification, labels and popups may still overlap windows.
  Each dock instance reserves on its own monitor's edge and releases it when parked, switched to
  auto-hide, removed, on quit and on the crash path.
- Vertical docks: reflection is off on left / right edges, glow and tilt stay. New theme option
  `reflectionClip` (Phase 2): clip the reflection to the bar's normal height instead of growing
  the bar, so part of it stays visible without changing the dock's height.
- Phase 14 scope: the built-in digital clock stays native but gains custom clock faces (the same
  face files the analog widget uses); the weather widget and popup are rewritten on the widget
  runtime; an extra example widget shows how to animate an analog clock with the runtime.

## Phase 0: quick wins (no architecture)

Settings and small behaviour only; all touch existing code paths.

- Clock: "Show seconds" toggle (the format already supports `%S`; the timer already aligns to
  the second). Settings > General.
- Calendar: "Clicking the calendar opens" = popup / app or URL (the existing click popup gets a
  footer button too, so the popup and the launch are not exclusive).
- Previews: "Show window previews for apps with a single window" (hover popup for 1+ windows).
- Middle click action: new instance (today) / minimize / close / nothing. Mouse-wheel over an
  item: nothing / cycle windows (groundwork for the volume widget's wheel later: the element
  interface in Phase 1 routes wheel events).
- Housekeeping: fix the three commits' co-author trailers in STATUS notes, push `origin/master`.

## Phase 1: element model (foundation)

Goal: everything in a dock is a `DockElement`; the dock lays out a sequence of elements and the
magnification / tween machinery runs over the *slots* they produce.

- `DockElement` interface: `Measure(axis, scale) -> size`, `Render(dc)`, `Slots()` (0..n icon
  slots that take part in magnification), `HitTest`, `OnPress/Click/MiddleClick/Wheel/
  RightClick`, `PopupAnchor()`, `Tick()`, dirty flags. Elements that are not icons (clock,
  weather, separators) measure to an arbitrary main-axis length and do not magnify (separators
  already do this via `slotX/slotW`; generalise it).
- Element kinds in this phase: `app`, `folder`, `separator`, `recycleBin` (one element each,
  today's `DockItem` becomes the slot state), `running` (one element that expands into the
  unpinned running apps, keeps `runningOrder_`), `clock`, `weather`. The clock and weather
  layers, `clockX_` / `weatherX_` tweens, `HitClock` / `HitWeather`, `pressedWidget_` all fold
  into their elements.
- Config schema (final shape, one migration):
  ```json
  { "docks": [ { "id": "main", "items": [ {"kind":"app","path":"..."}, {"kind":"running"},
      {"kind":"separator"}, {"kind":"weather"}, {"kind":"clock"} ],
      "edge":"bottom", "offset":0, "autoHide":"always", "locked":false, ... } ],
    "theme...", "animation...", "weather...": "unchanged (global)" }
  ```
  `pinned[]` migrates to `items[]` with `running`, `weather`, `clock` appended; dock-level keys
  (`iconSize`, `magnification`, `autoHide`, `monitor`, `previews`...) move into `docks[0]` with
  global defaults kept at top level for Phase 3's "mirror" docks.
- Edit mode: drag any element (not just pins) to reorder; `locked` disables drag, reorder and
  (from Phase 2) moving the dock. Dock menu: Lock dock, Add widget >, Add separator.
- Settings: "Pinned apps" page becomes "Dock contents": one list of every element, Add app /
  folder / separator / widget, Move up / down, per-element settings button.
- Automation: `QueryState` additions for element kind at index; `ItemCommand` works on element
  indexes; every existing message keeps its meaning.
- Done when: identical pixels for the user's config, all existing test scripts pass.

## Phase 2: geometry (foundation)

Status: done (2026-11-02) as 2a (axis layout, edges, alignment / offset, popups per edge) and 2b
(full width, reserve space, peek strip, title, reflection clip, drag the dock); see STATUS.md.

Goal: a dock is placed on an edge, at an offset, horizontal or vertical, without any element or
popup knowing which.

- `Layout` gains an axis: main / cross coordinates with helpers that map to screen x/y.
  Magnification, bar growth, slide, reveal strip, hover label placement, insertion marker and
  drag maths are rewritten in axis terms (the one big diff of this phase).
- Edges: bottom / top / left / right. Alignment along the edge: start / centre / end plus a
  pixel `offset`; "custom position" = drag the dock in edit mode (unlocked), snaps to centre and
  corners, saved as alignment + offset.
- Full-width mode (`fullWidth: true`): the bar spans the edge; items start-, centre- or
  end-aligned; optional "reserve screen space" (app bar via `SHAppBarMessage`) so maximized
  windows stop at the dock. Magnification still works; auto-hide still works.
- Peek strip: when hidden, keep `peekPx` of the bar visible at `peekOpacity` (replaces the 1 px
  invisible strip when on). Reveal on hover as today.
- Dock title: optional text drawn outside the bar on the edge-facing side (above a bottom dock).
- Popups (`PopupWindow`) take the dock edge: anchor and slide direction per edge.
- Effects per edge: reflection on horizontal edges only (vertical: off, documented); 3D tilt
  about the edge; acrylic backdrop and shelf follow the bar rect.
- Settings: Dock page gets Edge, Alignment, Offset, Full width, Reserve space, Peek, Title.
- Done when: a bottom dock is pixel-identical; top / left / right docks pass the hover, hide,
  drag, drop, popup and preview scripts (parametrised by edge).

## Phase 3: multiple docks and monitors (foundation)

Status: done (2026-11-02) as 3a (DockManager, N docks, monitors / "each" / parking, running
filter, per-dock theme preset, lock all, dock-indexed automation, Docks page, reserve space on
every dock) and 3b (desktop-only layer, Settings > Layout canvas); see STATUS.md.

Desktop-layer spike (3b, Windows 11 build 26300; harness in the session scratchpad
`p3\spike\spike.cpp`): Progman now has two children, `SHELLDLL_DefView` (the icons, on top) and
`WorkerW` (the 24H2 layout: there is no top-level WorkerW any more, `0x052C` creates none).
(a) A DirectComposition window made a child of that WorkerW renders (`PrintWindow` with
`PW_RENDERFULLCONTENT` of WorkerW and of Progman shows it), but `SHELLDLL_DefView` covers the whole
monitor above it, so it never gets the mouse. (a') As a child of Progman above `SHELLDLL_DefView`
it renders and is hit-tested (`ChildWindowFromPointEx`), and stays on the desktop under every
window. (b) A top-level, non-topmost window placed directly above Progman (inserted after the
window that precedes Progman) renders like any dock window and stays directly above Progman when
the desktop takes the foreground. Chosen: (b), in its own class `DesktopLayer` (shown only while
the desktop, or one of WinDock's windows, is the foreground, via the shared ForegroundWatch; placed
above Progman, never topmost): the dock stays a top-level window, so its screen-coordinate
placement, input, popups and drag and drop are unchanged, and it matches "shown only while the
desktop is visible". (a') is the candidate for Phase 12's folder windows if they must stay on the
desktop while windows are open: it needs Progman-relative placement, no topmost handling and no
separate acrylic window.

Goal: N docks, each on a monitor or on every monitor, with filters and a lock.

- `DockManager` (new) owns `std::vector<DockWindow>`; `App` talks to the manager. Shared
  services become singletons the manager owns: `WindowTracker`, `IconCache`, `WeatherService`,
  `ForegroundWatch`, the taskbar controller (hide once).
- `docks[]` with `monitor`: index / `"primary"` / `"each"` (one instance per monitor, mirrored
  config). `running` element filter: all windows / windows on this dock's monitor / (Phase 10)
  on the current virtual desktop. Per-dock `themePreset` override (optional; default global).
- Display changes: `WM_DISPLAYCHANGE` / `WM_DEVICECHANGE` re-enumerate; a dock whose monitor
  is gone is parked (not deleted) until it returns.
- Global "Lock all docks" (tray menu, hotkey later) sets every `locked`.
- Desktop-only docks (`layer: "desktop"`): shown only while the desktop is visible, kept at the
  bottom of the z-order above the wallpaper (spike: `SetParent` to the `WorkerW` behind
  `Progman` vs `HWND_BOTTOM` + hide while any window is foreground). This is the base for
  Phase 12's folder windows.
- Settings: Docks page (list, Add / Duplicate / Remove, pick the dock the other pages edit).
- Automation: dock messages take a dock id in `lParam` (0 = first dock, unchanged behaviour).
- Done when: two docks on two monitors (or two edges of one) run the existing scripts
  independently; one mirrored "each" dock on a single monitor behaves like today.

## Phase 4: widget runtime (foundation)

Status: done (2026-11-02) as 4a (engine, `widget` element, canvas API, hello + scripted clock;
one QuickJS runtime per widget instance) and 4b (generated settings page, storage, http, shell,
popups, press / drop events, watch, Console, Widgets page); see STATUS.md and docs/widgets.md.

Goal: a widget is a folder, loads without compiling, draws through a canvas API, keeps its own
settings and data, and can be hot-reloaded.

- Engine: QuickJS-ng behind a `ScriptEngine` interface so the implementation can be swapped.
  Spike done 2026-11-02 with v0.18.0 (harness: scratchpad `quickjs-spike\harness\main.cpp`):
  - Builds with MSVC 18, no patches; four sources (`quickjs.c dtoa.c libregexp.c
    libunicode.c`), `quickjs-libc.c` not needed. Add them as our own static-library target
    compiled `/W0` (1,500 warnings at /W4) with upstream's defines; do NOT `add_subdirectory`
    the upstream CMake (its `cmake_minimum_required(3.10)` leaves CMP0091 off, so the runtime
    library silently stays /MD and fails to link against the dock's /MT).
  - Cost: +1.1 MB exe at /O2 (about a third less at /O1), ~150 KB per runtime + context,
    ~70 KB per extra context, 0.25 ms to create one, ~6 µs per typical draw call.
  - Verified: `JS_SetInterruptHandler` watchdog (stops `for(;;)`, catastrophic regex, async
    loops within 50 ms; context stays usable), `JS_SetMemoryLimit` per runtime (an exhausted
    heap can throw `null` rather than an Error), C functions / objects / native classes with
    finalizers, custom module loader (dynamic `import()` goes through it too), ES2020+
    features, async via `JS_ExecutePendingJob` pumped by the host. Error stacks carry
    file:line:col.
  - Must do: link with `/STACK:8388608` and call `JS_SetMaxStackSize` with ~512 KB headroom,
    otherwise deep recursion crashes the process (MSVC frames are ~4.8 KB per JS call; the
    default 1 MB thread stack equals QuickJS's default limit). One runtime per thread; call
    `JS_UpdateStackTop` if a runtime ever moves threads. Every native callback is `noexcept`
    with a try/catch converted to a JS throw (a C++ exception crossing QuickJS frames kills the
    process). The module resolver does not sandbox: reject `..`, drive letters, leading
    slashes, and verify the canonical path is inside the widget folder. Provide `setTimeout`,
    `console`, `Intl`-free formatting and local time from the host (QuickJS's Windows
    `getTimezoneOffset` misapplies DST across transitions). Ship widgets as source, never
    bytecode (unsafe for untrusted input). One JS context per widget instance on the UI thread, with an execution-time
  watchdog (a widget that loops is suspended and shows an error tile).
- Widget package: `%LocalAppData%\WinDock\widgets\<id>\widget.json` (name, version, size hints
  per axis, settings schema, refresh interval, permissions such as `http` and `shell`),
  `main.js`, assets. Built-ins ship inside the exe as resources and can be "exported" to the
  widgets folder for editing.
- Host API (v1): `dock` (axis, size, scale, theme colours, edge, hovered / magnified state),
  `canvas` (rect, roundRect, path, arc, text with DirectWrite, image, gradients, clip, opacity),
  `timer`, `settings` (schema-driven; the settings window generates the page), `storage`
  (JSON file per widget), `http` (WinHTTP, allow-listed hosts from the manifest), `system`
  (date / time, locale, units), `shell` (open file / URL, list a folder with icons, the folder
  and file previews), `popup` (open a script-drawn popup through `PopupWindow`, or the built-in
  folder / preview popups), events (press, click, middle, wheel, hover, drop).
- Element kind `widget` with `id` and per-instance settings in `items[]`.
- Dev tools: `watch: true` reloads on file change (`ReadDirectoryChangesW`), error overlay on
  the tile, a small log window (Settings > Widgets > Console) with reload and open-folder
  buttons.
- Settings: Widgets page (installed widgets, add from folder, per-widget settings pages).
- Done when: a "hello" widget and a scripted copy of the digital clock run side by side with the
  native clock, hot reload works, a thrown error is shown and recovered from.

## Phase 5: bundled widgets, round 1 (scripts plus native providers)

Each is one small phase; order by the user's priority.

Status: done (2026-11-02). 5a: `analog-clock` (five faces in `faces/*.json`, the format in
docs/widgets.md "Analog clock faces", reused by Phase 14) and `analog-date` (the calendar page).
5b (the roadmap's 5b - 5e in one commit): the `audio` and `media` providers, `shell.sendKeys`,
and the `volume`, `media`, `start`, `search` and `notification-bell` widgets; see STATUS.md.

- 5a **Analog clock**: faces as data (`faces/*.json`: hands, ticks, numerals, colours), several
  shipped; `face` setting; seconds hand optional. **Analog date** (confirmed: a calendar page
  with the date, skeuomorphic, flip-calendar style; plus the date on the analog face).
- 5b **Volume**: provider `audio` (Core Audio `IAudioEndpointVolume` on the default render and
  capture endpoints, change notifications). Widget shows the level, wheel over it changes
  volume, click opens a popup with slider, mute, mic mute, output picker, "Sound settings".
- 5c **Media**: provider `media` (`GlobalSystemMediaTransportControlsSessionManager`: title,
  artist, album art, playback state, play / pause / next / previous, works for Spotify and
  browser media sessions such as YouTube Music). Widget shows art + title; controls on hover or
  click; optional wheel = volume.
- 5d **Start button** (custom icon; opens Start) and **Search** (opens Windows Search). Both
  are tiny widgets over a `shell` provider call.
- 5e **Notification bell** placeholder slot (filled in Phase 9).

## Phase 6: task list options

Status: done (2026-11-02) as 6a (unstacked windows, titles under icons, minimize to dock, live
window titles) and 6b (thumbnails as items, live capture or snapshots); see STATUS.md and README
"Running apps: windows and titles".

In-dock thumbnails spike (6b, Windows 11 build 26300; harness in the session scratchpad
`p6\spike\spike.cpp`, an animated WinForms window updating every 15 ms, mspaint windows):

- **Windows.Graphics.Capture** (GraphicsCaptureItem from the HWND via
  `IGraphicsCaptureItemInterop::CreateForWindow`, `Direct3D11CaptureFramePool`, frames drawn into a
  D2D bitmap): every `GraphicsCaptureSession` property exists on 26300 (`IsBorderRequired`,
  `IsCursorCaptureEnabled`, `MinUpdateInterval`, `DirtyRegionMode`, `IncludeSecondaryWindows`);
  `GraphicsCaptureAccess::RequestAccessAsync(Borderless)` returns Allowed for the unpackaged exe and
  `IsBorderRequired(false)` really removes the yellow border (screenshot checked). Frames come only
  when the window's content changes: a still mspaint window sent 1 frame in 6 s. The animated window:
  58.7 fps uncapped (0.94 % of one core including the D2D scale, 0.11 ms per frame),
  `MinUpdateInterval` 100 ms gives 9.4 - 9.6 fps (0.13 - 0.31 % CPU), 33 ms 28.3 fps (1.25 %); the
  process's GPU 3D engine stayed at ~0.1 %. A minimized window delivers no frames (the item shrinks
  to the 146 x 28 minimized caption): keep the last frame. **The frame pool refuses a D3D device
  created with `D3D11_CREATE_DEVICE_SINGLETHREADED` (E_NOINTERFACE)**: the dock's device is created
  multithreaded while live thumbnails are on (switching to live rebuilds it once). Capture calls
  made inside another process's `SendMessage` fail with `RPC_E_CANTCALLOUT_ININPUTSYNCCALL`
  (automation): sessions start from a posted message. Each session holds a frame buffer the size of
  the window (GPU memory). Cannot capture DRM content (black).
- **DWM thumbnails** (`DwmRegisterThumbnail`, what the preview popup uses): rejected for tiles.
  DWM draws them over the destination window, outside our DirectComposition tree, so they cannot
  take part in the placement / magnification animations (each frame of a magnification would need
  `DwmUpdateThumbnailProperties` from the CPU), they sit above every visual of the window (no app
  icon over them, no clipping by the auto-hide slide, no perspective / effects), and each needs its
  own top-level destination.
- **PrintWindow(`PW_RENDERFULLCONTENT`)** into a DIB, scaled with `StretchBlt(HALFTONE)`: 7.7 ms
  per call for 640 x 400 (0.5 ms CPU in our process) and 13.8 ms for 1920 x 1080 (5.5 - 15 ms CPU,
  plus 2.9 ms for the scale); works for occluded windows; a minimized window gives its minimized
  caption (skip it, keep the last frame); it waits on the target window, so it runs on a worker
  thread (a hung window is skipped: `IsHungAppWindow`). DRM content is black.
- Chosen: live = Windows.Graphics.Capture (one-buffer pool, not free-threaded, frames on the UI
  thread through a DispatcherQueue, `MinUpdateInterval` = 1 / `thumbnailFps`, sessions only while
  the dock is visible); snapshots = PrintWindow on a worker thread every `thumbnailIntervalS`.

All on the `running` / `app` elements.

- Unstack windows: one slot per window, grouped by app, with the app's order kept.
- Window titles under icons: 1 line (8 chars) or 2 lines (16), ellipsis; raises the bar's cross
  size; on vertical docks shown beside the icon or off (setting).
- Live title updates in the preview popup (open follow-up).
- Thumbnails as items: each window slot shows a thumbnail with the app icon in a corner
  (technique per the questions); magnification scales the tile.
- Minimize-to-dock (minimized windows keep a slot; already true for unstacked windows).

## Phase 7: folders and groups

Status: done (2026-11-02) as 7a (`ItemGridView`: a reusable scrolling grid / list control with
virtualised rows; folder popups rebuilt on it with grid / list / auto views, sub-folder navigation
and file drops) and 7b (shortcut groups: hold-to-group, the group popup, members dragged out,
rename, ungroup, the Settings group editor); see STATUS.md and README "Dock items".

- Shortcut groups: element kind `group` (a label plus its own `items[]` of pins), opened like a
  folder; drag a pin onto a pin to create a group; rename from the menu.
- The folder / group view becomes a reusable control (`ItemGridView`): grid or list mode,
  scrolling (wheel and drag), more than 48 entries, keyboard navigation, drop onto it.
- Folder popup list mode: a scrollable list that pops out above the folder (the user's
  "stack list"); per-item setting grid / list / auto.

## Phase 8: system tray

Status: done (2026-11-02) as 8a (TrayHost takeover, the `tray` grid element, the drawer, Settings >
Tray) and 8b (`trayApp` pins, grid order by drag, the Ctrl + right click tray menu, a separate
tray dock); see STATUS.md and README "System tray".

Tray takeover spike (2026-11-02, Windows 11 build 26300, dock not running, Explorer's taskbar
shown; harness in the session scratchpad `p8\spike\spike.cpp` + `run1.ps1`: `spike host` = our
`Shell_TrayWnd`, `spike app` = a test tray app with a GUID icon and a uID icon, both version 4).
Verdict: **viable**.

- A top-level `WS_POPUP` window of class `Shell_TrayWnd` (`WS_EX_TOOLWINDOW | WS_EX_TOPMOST`,
  never shown, a `TrayNotifyWnd` child; nobody looked into the child) created while Explorer's
  exists is found first by `FindWindow(L"Shell_TrayWnd")` (the newest topmost window is at the top
  of the z-order; `SetWindowPos(HWND_TOPMOST)` makes sure). `ChangeWindowMessageFilterEx(WM_COPYDATA)`
  for elevated senders. `SendNotifyMessage(HWND_BROADCAST, RegisterWindowMessage("TaskbarCreated"))`
  returns at once; within 125 ms ~35 real apps re-registered (Teams, OneDrive, PowerToys, Discord,
  Chrome, Steam, Phone Link, Windows Security, NordVPN, Explorer itself ...), the last ones (Steam,
  Epic) after ~1.2 s. Many apps send `NIM_DELETE` first, some `NIM_MODIFY` (fails: unknown) and
  then `NIM_ADD`; no app needed an implicit add.
- `Shell_NotifyIcon` = `WM_COPYDATA`, `dwData` 1, `wParam` = the sender, `cbData` **1484** for
  every app (shell32 normalises): `{DWORD dwSignature 0x34753423; DWORD dwMessage (NIM_*);
  NOTIFYICONDATA32}` where the 32-bit-compatible NOTIFYICONDATA is `cbSize (956), DWORD hWnd, uID,
  uFlags, uCallbackMessage, DWORD hIcon, WCHAR szTip[128], dwState, dwStateMask, WCHAR szInfo[256],
  uTimeout/uVersion, WCHAR szInfoTitle[64], dwInfoFlags, GUID guidItem, DWORD hBalloonIcon` (packed,
  956 bytes; the 520 bytes after it are unused). Handles are 32-bit values (sign-extend). The
  return value is the call's result. GUID icons are addressed by the GUID alone: WhatsApp sends
  `NIM_DELETE` / `NIM_SETVERSION` / `NIM_MODIFY` with `hWnd` 0, so a GUID icon matches on the GUID
  and an `hWnd` 0 must not overwrite its owner. `NIM_SETVERSION` arrives as its own message
  (`uVersion` 4 for most apps here, 0 for the rest).
- `Shell_NotifyIconGetRect` = `WM_COPYDATA` `dwData` 3, `cbData` 40: `{DWORD dwMagic 0x34753423;
  DWORD dwMessage; DWORD cbSize (32); DWORD padding; DWORD hWnd; UINT uID; GUID guidItem}`, sent
  twice per call: `dwMessage` 2 then 1; the answer is `MAKELONG(left, top)` for 1 and
  **`MAKELONG(width, height)`** for 2 (not right / bottom: the app got `left + width`). GUID queries
  carry `hWnd` 0. 0 = unknown icon (the call fails).
- App bar messages (`SHAppBarMessage`) = `dwData` 0; none came during the spike (no app bar was
  registered), forwarded verbatim they work (the shared-memory handle and the source process id
  are inside the payload, so Explorer can answer through them).
- Other messages to our window: `TaskbarCreated` itself, `WM_DEVICECHANGE`, and once 0x4EF
  (WM_USER + 239, sent while a forwarded menu opened). Forward unknown messages to Explorer's tray.
- Clicks: `PostMessage(owner, uCallbackMessage, ...)`; version 4: `wParam = MAKEWPARAM(x, y)` (the
  anchor in screen pixels, used by the app to place its menu), `lParam = MAKELPARAM(event, uID)`
  (uID 0 for GUID icons); version < 4: `wParam = uID`, `lParam = event`. Left click =
  `WM_LBUTTONDOWN`, `WM_LBUTTONUP`, then `NIN_SELECT` (0x400, version >= 3); right click =
  `WM_RBUTTONDOWN`, `WM_RBUTTONUP`, then `WM_CONTEXTMENU` (version >= 3); hover = `WM_MOUSEMOVE` and
  for version 4 `NIN_POPUPOPEN` (0x406) / `NIN_POPUPCLOSE` (0x407). With
  `AllowSetForegroundWindow(owner pid)` first, the test app's `SetForegroundWindow` succeeded and
  its `TrackPopupMenu` menu opened at the anchor. All arrived with the right semantics.
- Icons: `CopyIcon` of the sender's handle at once works (16 to 256 px icons seen: Chrome sends
  256 x 256); the test app destroying its icon right after `NIM_MODIFY` leaves our copy valid.
  `NIM_MODIFY` icon swaps arrive. One real app (Logitech G HUB) sends `NIM_MODIFY` with the same
  icon handle and tip **every second**: the host must compare and not redraw when nothing changed.
- Balloons (`NIF_INFO`) arrive in `NIM_ADD` / `NIM_MODIFY` (`szInfo`, `szInfoTitle`, `dwInfoFlags`,
  `uTimeout`). Explorer is not running the icons any more, but **every message is mirrored to
  Explorer** (`SendMessageTimeout(WM_COPYDATA)` with the same `COPYDATASTRUCT`, the result
  ignored; Explorer answers 0 for a duplicate `NIM_ADD`): Explorer stays in sync (its hidden tray
  has every icon, deletions and changes included), so it showed the balloon as a toast and sent
  `NIN_BALLOONSHOW` / `NIN_BALLOONTIMEOUT` to the app itself. While the mirror is in flight other
  apps' sends are dispatched (re-entrancy: never hold a reference into the registry across it).
- Z-order: Explorer did not fight back or re-broadcast in 2 x 40 s. Raising Explorer's tray with
  `SetWindowPos(HWND_TOPMOST)` makes `FindWindow` return Explorer's again (apps' messages then go
  to Explorer only) until ours is raised again; every top-level z-order change raises
  `EVENT_OBJECT_REORDER` (hwnd = the desktop window, `OBJID_CLIENT`) in the process that made it,
  so a WinEvent hook scoped to Explorer's process tells when to re-raise ours (no polling).
- Hand-back: `DestroyWindow` + `TaskbarCreated` broadcast: `FindWindow` returns Explorer's at once,
  apps re-send `NIM_ADD` to Explorer (answered 0: Explorer already has them, thanks to the
  mirror), the test app's `Shell_NotifyIconGetRect` succeeds against Explorer afterwards.
- Explorer itself registers notify icons with whoever owns `Shell_TrayWnd`: Safely Remove Hardware
  (GUID 7820AE78-..., with a callback), a "Speakers (USB): Muted" volume icon (GUID 7820AE73-...,
  no callback message) and one more with callback 0x403. Icons without a callback message cannot
  be clicked; the grid skips them.
- Not seen / not tested in the spike: elevated tray apps (none ran elevated; a medium-integrity
  dock cannot post clicks into an elevated app either, Explorer's limitation too), app bar
  messages (first exercised by the dock's own reserve-space app bar in 8a).

Design consequences for 8a: TrayHost mirrors every `dwData` 1 message to Explorer and forwards
app bar (0) and unknown messages to it, answers `dwData` 3 itself, keeps its window above
Explorer's through an `EVENT_OBJECT_REORDER` hook on Explorer's process, and the
TaskbarController / every other `FindWindow(L"Shell_TrayWnd")` in the dock skips windows of its
own process.

Learned while building 8a (same build):

- **Mirror after answering, not inside the call.** Mirrored synchronously, an app's
  `Shell_NotifyIcon` waited for Explorer's answer too; right after the dock hid the taskbar
  Explorer is slow, the apps' calls timed out and a third of the icons never arrived (and those
  apps did not retry). The app gets our answer at once; the messages are copied and sent to
  Explorer from the message loop in order (`FlushMirrors`; also before Stop).
- **Start from the message loop.** The takeover is posted after startup: apps answer the
  broadcast at once and must find the UI thread pumping (an app whose call hit the dock's busy
  startup failed and gave up).
- **Explorer raises its hidden tray** above ours once, ~0.1 - 1 s after the dock hid the taskbar
  (`EVENT_OBJECT_REORDER` from Explorer, then `FindWindow` returns Explorer's for a moment): apps
  answering then registered with Explorer only. The reorder hook re-raises our window and repeats
  the broadcast 1.5 s later (at most 3 times a minute); the takeover broadcast itself is sent to
  every top-level window except ours and Explorer's taskbars. Measured: 40 / 40 icons in 4 runs.
- **App bars from our own process need their shared block re-made.** `SHAppBarMessage` makes its
  shared block (`SHAllocShared`) for the process owning the `Shell_TrayWnd` it found and names
  that process in the payload. `APPBARMSGDATAV3` for 64-bit callers: the 40-byte 32-bit-compatible
  APPBARDATA, `dwMessage` at 40, `hSharedMemory` (8 bytes) at 48, `dwSourceProcessId` at 56,
  `cbData` 64. From another process the block is a handle in our process (relayed as it is, Explorer
  maps it: a third-party app bar works through the relay). From our own process shell32 passes the
  block's address instead (a 16-byte header `{size, 0xBAFF1AFF, pid, pid}` and the APPBARDATA),
  which Explorer cannot map: ABM_QUERYPOS / SETPOS of the docks' reserve-space app bars silently
  did nothing. TrayHost copies the data into a block made for Explorer's process, relays, and
  copies the answer back. (`wParam` and the payload's `hWnd` are not the app bar window for every
  ABM_*; `InSendMessage` reports the outer message.)

- Spike (1 day): create a `Shell_TrayWnd`-class window, broadcast `TaskbarCreated`, confirm
  `Shell_NotifyIcon` `WM_COPYDATA` messages arrive and that clicks forwarded to the icon's owner
  open its menu; confirm Explorer re-collects the icons after we broadcast again on quit.
- `TrayHost` service: registry of icons (GUID or hwnd+id), version 3 / 4 click semantics,
  tooltips, balloon notifications routed to the Phase 9 centre.
- Elements: `tray` (grid: 1 to 2 rows, 1 to 20 columns, overflow button opens a drawer popup
  built on `ItemGridView` with every icon), `trayApp` (one tray icon pinned at full size,
  created by dragging an icon out of the grid or drawer). Options: disabled / in a dock at any
  position / a separate dock (just a `docks[]` entry holding only a `tray` element, bottom
  right, same theme).
- Explorer's own tray items (network, battery, input, Explorer's clock and volume) are not
  notify icons and never arrive; volume is covered by 5b; network and battery become small
  bundled widgets in Phase 5 round 2.

## Phase 9: notifications and badges

Status: done (2026-11-02) as 9a (NotificationService, badges on app slots, theme fields, the
`notifications` provider) and 9b (the `notifications` element and the centre popup, tray balloons
routed to it, Settings > Notifications); see STATUS.md and README "Notifications and badges". Do not
disturb: no public API to read or set it (WNF is not public; `FocusSessionManager` drives focus
sessions, not DND, and was not tried unpackaged), so the centre has no toggle.

- Spike done 2026-11-02 (unpackaged exe, Win11 26300): `UserNotificationListener` access is
  Allowed with no prompt (follows the global "let apps access notifications" consent store;
  call `GetAccessStatus()` on every poll). `GetNotificationsAsync(Toast)` returns every toast
  with display name, AUMID, time and text in ~70 ms for ~40 items. `AppInfo` can throw
  `E_NOTIMPL` for some toasts (wrap per item). AUMIDs arrive as `PFN!App`, plain ids
  (`Chrome`) or `{KnownFolderGUID}/path/exe`; match case-insensitively against the dock's keys.
  **`NotificationChanged` throws `0x80070490` without package identity**, so the design is a
  background MTA poll every 2 to 5 s (1 s while the centre is open) diffing notification ids,
  posted to the UI thread. A sparse MSIX package (`userNotificationListener` capability,
  signed, `Add-AppxPackage -ExternalLocation`) would restore the event but costs signing and
  installer work; not planned. No public API reads another app's badge or overlay icon; the
  per-AUMID toast count is the badge. Harness: scratchpad `notify-spike\harness.cpp`.
- Learned while building 9a (same build): the poll costs about 25 ms of CPU per
  `GetNotificationsAsync` with 46 notifications in the centre (the spike harness alone: 24 ms per
  call), i.e. ~1.3 % of one core at the 2 s cadence and ~0.5 % at 5 s; it grows with the number of
  notifications. The notification database folder (`%LocalAppData%\Microsoft\Windows\Notifications`,
  `wpndatabase.db` + WAL) raised no directory change notification for a toast arriving or going, so
  it is no cheaper change signal. A test toast needs a registered AUMID
  (`HKCU\Software\Classes\AppUserModelId\<id>` with a `DisplayName`, or a Start menu shortcut):
  unregistered, `AppInfo` throws for it; a registered id containing backslashes is reported by its
  last segment only, so the `{KnownFolderGUID}\...` format was verified on a real app's toasts
  (FanControl: `{7C5A40EF-...}/FanControl/FanControl.exe` -> `C:\Program Files (x86)\FanControl\FanControl.exe`).
- `ITaskbarList3` overlay icons and progress (checked with the p8 spike's `Shell_TrayWnd` above
  Explorer's, a test window calling `HrInit`, `SetProgressState` / `SetProgressValue`,
  `SetOverlayIcon`): nothing reaches our window (no message at all); with our window found first,
  `HrInit` fails with `E_NOTIMPL` and `SetOverlayIcon` with `E_FAIL` (all succeed with Explorer's
  tray first). Overlays / progress are therefore not available as badges, and while the tray
  takeover runs apps' taskbar-list calls fail (harmless: the taskbar they target is hidden).
- Notification centre element: unread count, popup listing notifications (app icon, title,
  body, time), click activates the app, dismiss one / all.
- Badges on app slots: per-app unread notification count (the listener groups by AUMID / exe),
  plus `ITaskbarList3` overlay icons and progress if they reach us as the tray owner (spike with
  Phase 8). Theme fields for the bubble.

## Phase 10: virtual desktops

- Provider: `IVirtualDesktopManager` (public: window -> desktop, current desktop from the
  registry with change notification). `running` filter "current desktop only".
- Desktop switcher widget: desktop list and names from the registry; switch via Win+Ctrl+arrow
  simulation (public) or the undocumented internal interface behind a setting (per the
  questions).

## Phase 11: shapes and overflow

Status: done 2026-11-03: 11a (scrollable docks: `overflow` scroll / shrink / clip, the viewport
transform in DockGeometry) and 11b (`shape` arc / circle: the polar mapping in DockGeometry,
`radius`, `rotateIcons`, a circle at `x` / `y`); see STATUS.md.

- Scrollable docks: when the items exceed the edge, the item run scrolls (wheel, drag, edge
  fade); magnification stays correct inside the viewport.
- Circle and half-circle docks: a polar layout mode in the axis abstraction (angle instead of
  main-axis position), background ring, popups anchored radially, magnification along the arc.

## Phase 12: desktop folder docks (Fences-like)

Status: done 2026-11-03 as 12a (`FolderWindow` on the extended `DesktopLayer`, ItemGridView with
multiple selection and double-click activation, SHChangeNotify, the shell menu, drops) and 12b
(Desktop rules, appearance keys, the Folder window settings page, the clipboard); see STATUS.md.
Out of scope: Fences' "Peek" (double-click the desktop to hide every window).

Desktop-layer spike (12a, Windows 11 build 26300; harness in the session scratchpad
`p12\spike\spike.cpp`, `spike.exe T|C`; the session was locked, so foreground changes by script
were refused and only z-order / focus within our own thread could be checked):

- **T, top-level with a z-order lock** (WS_POPUP + WS_EX_TOOLWINDOW + WS_EX_NOREDIRECTIONBITMAP, not
  WS_EX_NOACTIVATE, placed directly above Progman; `WM_WINDOWPOSCHANGING` adds `SWP_NOZORDER` unless
  the layer places it): a DirectComposition target renders (PrintWindow 25 / 25 samples), a normal
  app window stays above it, `SetWindowPos(HWND_TOP)` and the activation's raise are both dropped
  (2 blocked z changes) while `SetActiveWindow` makes it the active window and `SetFocus` gives it
  the keyboard focus, `RegisterDragDrop` succeeds, `DWMWA_SYSTEMBACKDROP_TYPE` succeeds (so the
  Backdrop acrylic window works beneath it). **Progman is pinned to the bottom of the z-order**: a
  window sent to `HWND_BOTTOM` lands directly above it (z index 816 of 818, Progman 817), so a new
  desktop window after an Explorer restart cannot end up above the folder windows; re-placing after
  `TaskbarCreated` / when the desktop takes the foreground covers the rest. A cloaked CoreWindow
  sometimes sits between a window and Progman: the layer test ignores cloaked and invisible
  windows.
- **C, a child of Progman above SHELLDLL_DefView** (Phase 3's candidate a'): renders and is
  hit-tested, `SetFocus` works, `RegisterDragDrop` succeeds, but the child relation **attaches our
  input queue to Explorer's thread** (Explorer's GUI thread reports our window as its focus window:
  a hung Explorer would hang the dock's input and the reverse), `DWMWA_SYSTEMBACKDROP_TYPE` fails
  with `E_HANDLE` (no acrylic on a child), and a child is destroyed with Progman when Explorer
  restarts (would need re-creating).
- Chosen: T, in `DesktopLayer` (Register / FilterPosChanging / Place / Reassert / AboveDesktop):
  the desktop docks of Phase 3 keep `PlaceAboveDesktop`; folder windows register with the layer,
  which keeps each one (and its backdrop window) directly above the desktop window or above the run
  of our own windows there, and re-places them on `TaskbarCreated` and when the desktop takes the
  foreground (one out-of-context `EVENT_SYSTEM_FOREGROUND` hook while a folder window exists).
  Not verified: a real Win+D / Show desktop (it minimizes the user's windows) and a real Explorer
  restart (closes the user's Explorer windows); the recovery path ran through DockCommand 138 (lift
  out of the layer, then the recovery). `SetWindowPos(HWND_TOP)` from the dock's own process was
  refused in the locked session (the hook inserts after another app's window instead).

- Dock kind `folderWindow` on the desktop layer (Phase 3): a titled, resizable panel showing a
  folder through `ItemGridView` (Phase 7), live refresh via `SHChangeNotify`, drag / drop,
  the shell context menu, rename, delete.

## Phase 13: calendar integrations

Status: done 2026-11-03 (ICS feeds only, per the decisions): 13a (CalendarService with web and file
feeds, the own RFC 5545 parser and its tests in `tests/ics`, events and dots in the calendar
popup, the clock's dot, Settings > Calendar) and 13b (the `calendar` provider for widgets, the
Agenda widget, reminder banners); see STATUS.md. Phase 14's script calendar popup can be drawn
from `calendar.eventsOn` / `events` (startDay / endDay, colours) and `system`.

- Provider `calendar`: Google Calendar and Outlook / Microsoft 365 (OAuth device-code or
  loopback flow, tokens in DPAPI) or ICS feeds (per the questions). Events in the calendar
  popup, an agenda widget, and the clock's day shown with a dot on event days.

## Phase 14: built-ins as widgets, dev tooling polish

Status: done 2026-11-03 as 14a (the `weather` provider over the one WeatherService, the "Weather
(script)" widget and its forecast popup; the native weather item stays as the fallback; the script
calendar popup port was skipped: optional, the native popup already shows the feeds) and 14b
(digital faces for the native clock, `clock.face`, three shipped faces + user files in
`clock-faces\`; `examples/` with three annotated widgets; manifest validation in the Console / the
error tile and `WinDock.exe --validate-widget`; Export built-in with sub-folders and a README stub;
the generated settings page's `group`, `unit`, `visibleIf`; the docs pass with
`tools/check_widget_docs.py`). The digital clock itself was not ported to a script (the decision
above: it stays native and gains faces); the scripted `digital-clock` widget of 4a remains.

- Port the digital clock, weather (widget and popup), calendar popup to scripts; the native
  versions stay as fallbacks until the ported ones have run for a release.
- Example widgets folder (`examples/`), `WIDGETS.md` API reference, manifest validation
  messages, "Export built-in to folder" button, widget settings UI generation finalised.

## Phase 5 round 2 (slot in by priority)

Network, battery, CPU / RAM, now-playing variants, desktop switcher skins.

## Round 6 (planned 2026-11-03): desktop layers, floating docks, standalone widgets, notes, performance monitors, drawer previews

### Round 6 done (2026-11-03)

Round 6 is complete: all five phases below are done, one commit each on master, none pushed by the
agents (STATUS.md's "Round 6" table has each phase's notes, what was verified and how, and its "Not
verified" list; "How to resume" collects the open follow-ups).

| Phase | Commit |
|---|---|
| 6A Layers, floating docks, dashboard hotkey | b3d9a8e |
| 6B Standalone widgets | 1e27623 |
| 6C `popup.editText` and the Notes widget | 8e77d58 |
| 6D `perf` provider, System monitor / System overview | 0541b6e |
| 6E Drawer previews with sample data | the commit after 0541b6e ("Preview widgets in the drawer with sample data") |

Plan with the decisions: `C:\Users\olson\.claude\plans\windock-round6.md`. The user's wish list:
a dock that never floats above other windows; floating docks that can be moved or locked and only
appear on a hotkey or on the desktop; standalone widgets on the desktop (a "dashboard" shown by the
same hotkey) that can be moved and resized; a note widget written as a script widget; performance
monitor widgets (CPU, GPU, memory, temperatures ... as graphs, percentages, gauges, dials, numbers;
HWiNFO allowed); live previews with sample data in the widget drawer.

| Phase | Scope | Status |
|---|---|---|
| 6A | `layer: "behind"` (always on the desktop under every window), `floating` bar docks at `x` / `y` (drag, snap, lock), the dashboard hotkey (`dashboard.hotkey`, default Win+Shift+D) and dashboard mode | Done (the commit "Add the behind layer, floating docks and the dashboard hotkey"; STATUS.md "Round 6") |
| 6B | Standalone widgets: `docks[]` kind `"widget"` (`WidgetWindow`), move / resize, `dock.standalone`, manifest `standalone`, settings windows, drawer "Add to the desktop" | Done (the commit "Add standalone widgets on the desktop"; STATUS.md "Round 6") |
| 6C | `popup.editText` (a native text editor window, Promise) and the `notes` widget | Done (the commit "Add popup.editText and the Notes widget"; STATUS.md "Round 6") |
| 6D | `perf` provider (`PerfService`: CPU / memory / GPU / disk / network / battery + HWiNFO shared memory) and the `system-monitor` / `system-overview` widgets | Done (the commit "Add the perf provider and the system monitor widgets"; STATUS.md "Round 6") |
| 6E | Widget previews in the drawer and inspector: preview mode, provider sample data, manifest `preview` | Done (the commit "Preview widgets in the drawer with sample data"; STATUS.md "Round 6") |

## Round 7 (in progress, 2026-11-03): dock fixes, item anchors, widget upgrades

Plan with the decisions: `C:\Users\olson\.claude\plans\windock-round7.md`. The user's wish list: a
Task Manager entry on the dock menu, a crisp Start logo, a maximum dock length after which the dock
scrolls, per-item placement on the dock (start / centre / end, offsets), analog clocks that grow on
hover, a multi-day forecast on the dock, the name label kept with the window previews, popups closer
to the dock (configurable), media controls beside the media widget and a scrolling title, a battery
widget. Testing is light by the user's request (one or two checks per feature). One commit per
phase on master, not pushed; STATUS.md "Round 7" has each phase's notes.

| Phase | Scope | Status |
|---|---|---|
| 7A Dock fixes | Task Manager on the dock / tray menu (DockCommand 149); the hover label stays with the window preview (the preview opens beyond it); `docks[].popupGap` (default 6 DIP from the magnified icon; Behaviour tab); `docks[].maxLength` (Position tab; the overflow mode takes over at the cap); the Start logo snapped to device pixels | Done (the commit "Add Task Manager, popup gap, maximum length; keep the label with previews"; STATUS.md "Round 7") |
| 7B Item anchors | `items[].anchor` (start / centre / end) and `items[].offset`, group layout (an anchored bar is the room's length; one run while it overflows), Sanitize / every edit keeps group order, on-dock drag and strip moves join the group they are dropped into, the inspector's "Position on the dock" / "Offset", QueryState 272 - 274, ItemCommand 859 / 860 | Done (the commit "Add per-item anchors and offsets on the dock"; STATUS.md "Round 7") |
| 7C Widgets | Analog clock hover growth (`hoverScale`, a box thicker than the bar grows outward), the weather widget's dock forecast (`dockForecast` today / days, `dockDays`), media controls beside the art and marquee options (`controls`, `marqueeMode`, `marqueeSpeed`), the battery widget (+ `battery.acLine` / `battery.timeLeft` in `PerfService`, preview samples) | Done (the commit "Grow the analog clock on hover, forecast on the dock, media controls, a battery widget"; STATUS.md "Round 7"); the end-of-round regression is next |

## Round 8 (complete 2026-11-04): settings decluttering

Plan with the decisions: `C:\Users\olson\.claude\plans\windock-round8.md` (the accepted proposal
"WinDock Settings Decluttering"). The user's one explicit constraint: the widget drawer on the Dock
window's Contents tab keeps its live widget previews. Testing is light (one or two checks per
change, a regression at the end). One commit per phase on master, not pushed; STATUS.md "Round 8"
has each phase's notes.

| Phase | Scope | Status |
|---|---|---|
| 8A Foundations | Measured notes and a row reflow (`Collapse`), cards, label hints, tooltips (`Tip`), expanders; resizable settings windows with saved sizes (`settings.windows`); no page titles in Dock / Item windows; the duplicate config buttons, Flash button and Lock check box removed; every paragraph note rewritten as a hint / one line plus a tooltip; QueryState 275 / 276 | Done (the commit "Measure notes, cards and hints, resizable settings windows"; STATUS.md "Round 8") |
| 8B Dock window | Compact header (72 DIP) with the silhouette on the Position tab; Appearance tab (the Theme tab retired, Size moved); Position by placement kind with collapsing rows and a More expander; the preview strip only on Appearance; Contents: one add drawer (previews untouched), the inspector's buttons and More menu; QueryState 277 / 278 | Done (the commit "Compact dock header, Appearance tab, placement kinds, one add drawer"; STATUS.md "Round 8") |
| 8C Item and standalone windows | The header's description line, cards per manifest group, hints / tooltips from field descriptions, the place autocomplete with a summary and an Edit coordinates expander; the standalone widget window's Widget / Window tabs | Done (the commit "Item settings in cards, place autocomplete, one standalone widget window"; STATUS.md "Round 8") |
| 8D Global window | Nav groups; Docks and displays toolbar, Lock all check box and row actions; Themes top menu, collapsible groups and the small reset button; Widget library primary button and More menu; Calendar feeds form as an expander; Tray icons / Notifications hints | Done (the commit "Group the Global nav, menus on Docks and displays, Themes and the Widget library"; STATUS.md "Round 8") |
| 8E Regression | Every window kind at default and minimum size, light / dark if cheap, the drawer previews; the release build; the user's dock restarted | Done (folded into the 2026-11-04 release build the user runs; the Win+D desktop re-placement followed as "Re-place the desktop windows after Win+D raises Progman") |

## Round 9 (complete 2026-11-04): full-screen apps, steady magnification, clearer auto-hide, settings search, presets, help

Plan with the decisions: `C:\Users\olson\.claude\plans\windock-round9.md` (from the user's pasted
wish list of 2026-11-04). Testing is light (one or two checks per change, a regression at the end).
One commit per phase on master, not pushed; STATUS.md "Round 9" has each phase's notes.

| Phase | Scope | Status |
|---|---|---|
| 9A Dock fixes | A full-screen guard (the docks, standalone widgets and the tray dock hide while a full-screen app is in front on their monitor; `settings.fullscreen`); steady bar growth while magnified (`docks[].magnifyBar` "steady" / "follow"); auto-hide controls never greyed (collapsed with a reason and a link for a floating bar; the stale-state bug fixed; every greyed control explained or collapsed); QueryState 280 / 281, DockCommand 150 | Done (the commit "Hide for full-screen apps, steady bar growth, auto-hide never greyed"; STATUS.md "Round 9") |
| 9B Settings search | Ctrl+F / a box in the Global nav / the Dock ⋯ menu opens a search over every setting of every window (name, label, hint, value); Go to opens, scrolls to, focuses and flashes the control; check boxes and combos change in place; QueryState 282 / 283, DockCommand 151 | Done (the commit "Search every setting from Ctrl+F, go to it, change it in place"; most of its code landed one commit earlier in cc20c17, whose subject "Implement late re-placement for desktop windows on foreground change" does not describe it; STATUS.md "Round 9") |
| 9C Presets | Predefined dock setups as RCDATA JSON (basic, minimal, modern, windows and labels, sidebar, floating performance, desktop dashboard) with a chooser dialog (description + rendered preview); Docks and displays > Add ▾ > Dock from a preset…, the Dock ⋯ menu > Apply a preset…, the first-run Welcome dialog; QueryState 284, DockCommand 152 / 153 | Done (the commit "Start from a preset: seven dock setups, a chooser, the welcome dialog"; STATUS.md "Round 9") |
| 9D Help | Human-readable guides in docs/ (quick start, settings, docks, items, troubleshooting), an in-app Help window (Markdown rendered in a RichEdit, F1, a Help nav row, "?" buttons on every settings card), every field labelled with a tooltip (QueryState 285 counts the gaps); QueryState 286 / 287, DockCommand 154 | Done (the commit "Help from every card: user guides, a Help window, F1, a tooltip on every setting"; STATUS.md "Round 9") |
| 9E Regression | Every window kind at default and minimum size, the new dialogs, the guard, the docs link check; the release build; the user's dock restarted | Done (STATUS.md "Round 9 final regression": every suite re-run on the release exe, pictures of every window kind at both sizes; two layout fixes: a collapsed card takes its "?" with it, a slider's value pulls in at a narrow width) |

## Round 10 (complete 2026-11-06): shorter menus, explicit names, one editing workflow, clearer settings

Plan with the decisions: `C:\Users\olson\.claude\plans\windock-round10.md` (from the user's pasted
wish list of 2026-11-06: shorter item menus, a dock menu grouped by task, explicit settings names,
the Contents tab as the main editor, one settings structure for every widget, common controls first,
inherited versus local settings, labels that describe the effect). One commit per phase on master,
not pushed; STATUS.md "Round 10" has each phase's notes.

| Phase | Scope | Status |
|---|---|---|
| 10A Menus and names | Three menus instead of one appended everywhere: the dock menu grouped by task (Add / Position / Auto-hide / Lock, then All docks / App / Tools submenus, Help, Quit), a tray-icon menu of its own for app-wide commands (Docks submenu, Show Windows taskbar temporarily at the top level), item menus with the item's entries first and the dock menu in a Dock submenu; real item menus for the widget kinds with Remove from dock (ItemCommand 861); Customize icon and Contents layout submenus; "Global settings" -> "App settings" everywhere user-visible; settings named for their target; Item window titles "<item> · <dock>" with ordinals; DockCommand 155 | Done (the commit "Menus grouped by task, explicit settings names, App settings"; STATUS.md "Round 10") |
| 10B Contents tab as the editor | Common settings in the inspector, More settings… for the full Item window | Done (the commit "Contents inspector edits common settings in place"; STATUS.md "Round 10") |
| 10C One widget settings structure | Content / Appearance (/ Placement) sections; manifest `section` key | Done (the commit "One settings structure for every widget: Content, Appearance, Placement"; STATUS.md "Round 10") |
| 10D Common first, inherited vs local | Named collapsed expanders; Use app default / Reset to app default / Reset to factory defaults… | Done (the commit "Common settings first, detail folded, app defaults shown"; STATUS.md "Round 10") |
| 10E Regression and release | Suites re-run, pictures, the release build, the user's dock restarted | Done (STATUS.md "Round 10 final regression": the four phases' suites and a cross-phase suite on one dev build, pictures of every settings window kind and the six guides, three naming fixes; the release build; the user's dock restarted on it) |

Design notes for 10A:

- **Why three builders.** `ShowTrayMenu` served the dock background and the notify icon, and
  `ShowItemMenu` appended it whole after an item's entries, so every item menu was 30+ rows with the
  item's own 3 - 8 at the top. Now `AppendDockEntries` builds the dock menu once and is used flat
  (`ShowDockMenu`) or as the item menu's last "Dock" submenu (Quit included, as the plan asks);
  `ShowAppMenu` has only app-wide entries and no dock state. The tray-icon menu is still owned by
  the first dock's window (an activatable, shown window: KB135788's "menu closes on a click
  elsewhere" needs the owner in the foreground; the app window is a hidden `WS_EX_NOACTIVATE`
  popup) and holds that dock shown while open, as before.
- **Widget slots.** Their elements still answer `RightClickAction::DockMenu` (the hit-test path in
  `DockWindow` is unchanged); `App::OnDockMessage` turns a widget slot (`DockItem::IsWidget`) into
  an item menu itself. The tray's icons keep their own behaviour (`OnRightClickAt`: the app's menu,
  Ctrl + right click the tray icon menu); only a right click beside them reaches the new menu.
- **Remove from dock** reuses the Contents tab's removal (erase the config item, reload, save,
  `RefreshSettings`, which closes a removed item's Item window) and only for widget kinds: a pin
  already has Unpin / Remove separator with their own semantics (800). Refused while locked like
  the Add entries.
- **Show forecast / calendar / centre** from an item menu toggle that slot's popup
  (`DockWindow::ToggleSlotPopup`), so the second weather item shows its own forecast; the
  DockCommands 114 / 115 / 133 keep meaning "the kind's first item" (automation unchanged).
- **Stable positions.** Reload widgets / Widget console… are greyed instead of hidden without a
  script widget, so a menu-walking script finds Tools' entries at fixed positions; All docks' lock
  pair still only shows with more than one dock (the plan says so).
- **Names.** Only user-visible strings changed ("App settings"); `SettingsKind::Global`,
  `kHeaderGlobal`, `openGlobal` stay. The ordinal rule compares the full item name (with a clock's
  label / weather place), so it appears only where nothing else tells two items apart, and the
  first keeps no number. The Dock window title keeps its edge / display tail after
  "<id> · Dock settings" (it identifies the dock further; the plan's form is its prefix).

Design notes for 10B:

- **Why not the generated page's own controls.** A `SettingsPage` is a child window of its
  `SettingsWindow`, which creates, colours, lays out and routes notifications only for its
  top-level pages (and the loading guard is the window's). Hosting `WidgetSettingsPage` inside
  the Contents page would mean nested pages in SettingsWindow: the large refactor the plan
  allowed to skip. What both editors share instead is the part that matters for consistency:
  the values (`ItemSettingsValues`), the write (`StoreItemSettings`), `visibleIf`
  (`SettingFieldVisible`) and the scope (`SettingsScope::Pins`); the inspector's controls are
  a small fixed pool.
- **A pool of rows, not a rebuild.** The Contents tab lays itself out; rebuilding the page per
  selection would recreate the strip and the drawer (their previews) and lose a drag. Six rows of
  every control kind are created once; a selection shows one control per row and moves them.
  The pool's ids are stable (`2640 + 10 n + part`), so tests and later phases can drive them by
  row; QueryState 289 / 290 say which rows and controls are live.
- **What counts as common.** Built-in kinds by hand (the clock's first Item-page rows: format,
  zone, date, label; weather: place, name, units), script widgets by the new `common` key, with a
  fallback of the first three simple fields so other authors' widgets get something. Perf choices
  (a sample of the metrics may be taken to list them) and the place search (a geocoder, a typing
  timer, an autocomplete drop-down) stay in the Item window; the weather place is a read-only line.
- **Refresh both ways.** App already refreshes every window but the source after a change, so an
  open Item window followed; it did not rebuild when the change flipped a `visibleIf`, so it now
  compares the visible set on Load. The inspector refreshes its own strip and rows through a
  posted page command: its change may come from an edit losing the focus to the strip in the
  middle of the strip's own mouse handling.
- **Names.** The button is "More settings…" when the inspector shows settings above it (its
  tooltip names the window), else keeps 10A's "<Kind> settings…" (tray). "More ▾" became
  "Customize icon ▾", the name of the item menu's submenu since 10A.

Design notes for 10C:

- **Tabs in the Item window were cheap.** The tab strip (`WinDock.SettingsTabs`) only needed the
  Item kind in its creation condition: layout, painting, keys, Ctrl+Tab and the search's page
  selection were already per page. So no section headings on one page (the plan's fallback): the
  generated page class takes a part (All / Content / Appearance), and `MakeWidgetSettingsPages`
  makes two instances when the schema's `SectionMask` has both sections, else one with every
  field. A section with no fields gets no page, so a schema of one section has one page and no
  strip (the weather item, Hello, Digital clock, Start, Search); its page is named for the section
  it uses (search's Where column for a standalone widget). The tabs are decided when the window is
  made: a manifest that changes while it is open keeps the window's tabs until it is reopened.
- **The two pages stay in step** through `OnPeerChanged` (9A's hook: a change on one page makes
  the window's other pages Load); Reset to defaults in the footer goes to the active page, which
  resets the whole item, and the other tab follows. `VisibleSignature` only covers the page's own
  fields, so a `visibleIf` flip on one tab does not rebuild the other.
- **Standalone widgets keep their page indexes.** Placement stays page 1 (`kTabWidgetWindow`, the
  Window settings… path and automation unchanged) and Appearance is page 2; `NavOrder` lists them
  Content, Appearance, Placement and `TabRect` now lays the tabs out in that order (it used the
  index order).
- **The default rule** (no `section`): colour fields and the groups Colours / Colors / Look / Face
  / Size / Slot / Display / Motion / Motion and size are Appearance, as the plan lists them (font
  fields are not in the rule: the plan names colour only; every built-in widget sets `section`
  anyway). An unknown value is a validation error and, at load, a warning plus the default.
- **The built-in mapping** follows "what it shows and does" versus "how it looks": data choices,
  feeds, places, metrics, time zones, labels, click / wheel actions and refresh intervals are
  Content; faces, styles, colours, sizes, motion and the slot's layout toggles (percent text,
  compact, the weather slot's lines) are Appearance. The seven ungrouped widgets got groups; two
  fields whose group would have been split across the tabs got a group of their own (Battery's low level "Level",
  System overview's refresh "Updates"), the rest keep their cards. The built-in clock has no
  colour / font / size of its own (the theme draws it), so only its face is Appearance; the
  weather item's fields are all Content.
- **Placement**: the plan's "size presets" do not exist (the window's size is the manifest's
  default or what the user dragged), so the tab leads with display, layer and lock; background is
  a Look card; x / y / width / height sit in a heading expander closed per window instance, which
  the search opens (9B) when it goes to one of them.

Design notes for 10D:

- **Where the common controls were.** Theme, icon size and magnification were already on one tab
  (Appearance, since 8B) but in two cards; they are one first card now ("Theme and size"), so the
  plan's "gather those three" was a merge, not a move between tabs. Position already led with Kind,
  Display and Edge; Kind stays first (it decides whether Edge applies at all). Behaviour already
  led with Auto-hide.
- **What folds away.** Only what the plan names, each in a heading expander (a card heading with
  a chevron, closed per window instance, `HeadingExpander(openAtFirst = false)`): x / y, offset,
  peek, peek opacity and maximum length ("Exact offsets"); hide / reveal / preview delays and the
  popup gap ("Delays and popup gap": the plan lists the popup gap with the offsets, but it lives
  on Behaviour and moving it to another tab would move its control to another page window, which
  older automation finds by page; it got the delays' card instead); the magnification range and
  bar mode ("Magnification details": tuning of the common magnification); the Contents
  inspector's anchor and offset ("Anchor and offset"); Animation's falloff / easing / label fade
  ("Motion and timings", now after Accessibility); every Themes group, Bar included. The old
  "More: …" expanders became headings with the same ids (2269, 2272), so QueryState 276 and
  scripts keep working. Reserve screen space and Title were in "More" but are neither offsets nor
  timings: they moved to the Placement and the (renamed) Layer and title cards.
- **"· n changed".** Counted against a value-initialised `DockConfig` / `AnimationConfig` (the
  defaults are the structs', so nothing is duplicated), only for the rows that apply to the
  current placement / auto-hide / previews state, refreshed on every change of a bound number
  (`IntChanged`) and on Load. The Contents inspector's counts the selection's anchor and offset.
- **The Contents tab lays itself out** (NoReflow): `Add` tagged controls with their expander only
  on reflowing pages, so a closed expander there hid nothing; it tags them on every page now, and
  `SettingsPage::OnExpanderToggled` lets the page place its rows again (FlowInspector).
- **Inheritance was mapped, not invented.** The config has three real fallbacks from a dock / item
  to a shared value: `docks[].themePreset` "" (the app theme), a feed's `reminderMin` 0
  (`calendars.defaultReminderMin`) and a script widget without `latitude` / `longitude` (its dock's
  first Weather item, else `config.weather`). Each now says so and has a ↺ while local. Clock /
  weather items copy `config.clock` / `config.weather` when created (EnsureItemSettings) and hold
  every key afterwards, so they are local, not inherited: no "Use app default" there. The theme
  fields are app-wide only (a dock has no per-field overrides), so "Reset to the preset's value"
  is the only theme-row wording that applies. No sentinel was added: "" and 0 are the existing
  ones, and the absent place keys are what the provider already reads.
- **Reset to factory defaults….** A Yes / No MessageBox owned by the settings window (No the
  default), between `host.modal(true / false)`, which holds `App::modalDepth_` as a dock dialog
  does (no Reload config, no dock edits, display changes wait), so the item is still there when
  it returns; the window checks its HWND after it. For a clock / weather item "factory" is the
  settings a new one starts with (WinDock's own unless config.json's top-level objects were
  edited by hand), the behaviour the button had; the text says so.

Notes for 10E (round 10 closed 2026-11-06):

- **What the regression found** was naming left over from before 10A: the folder window page's
  "View" and the folder / group page's "Show the contents as" (now "Contents layout", the menus'
  and the inspector's word), and "Settings > Calendar / Widgets" paths in built-in widget texts and
  the export README. No behaviour regression: every phase suite passed again on the final tree.
- **A confirmation changes automation.** 10D's Reset to factory defaults… turned a button that
  returned at once into a modal one; a test that *sends* the command now blocks until the dialog is
  answered (10C's suite did). Post such commands and answer the `#32770` (README "Automation").
- **Left open** for a later round: real-input checks of the new surfaces, light mode / high
  contrast, other DPIs and displays, a "Folder window settings…" entry in the folder window's own
  menu (STATUS.md "Round 10 final regression", Found and left).

## Round 11 (complete 2026-11-07): the Windows 7 look

Plan with the decisions: `C:\Users\olson\.claude\plans\windock-round11.md` (from the user's request
of 2026-11-06: "need to be able to customize the dock to the point where it looks almost identical
to windows 7"). One commit per phase on master, not pushed; STATUS.md "Round 11" has each phase's
notes.

| Phase | Scope | Status |
|---|---|---|
| 11A Theme: slot buttons, sheen, spacing | Eleven theme fields (`slotStyle` + seven `slot*`, `sheen`, `iconGap`, `barPadding`), taskbar-button frames behind icon slots by state (running / active / attention pulse / hover with hot tracking / pressed / stacked), the sheen, the spacing in the layout, the `aero` preset, the Themes page's Slot buttons card, the preview strip, QueryState 293 / 294 | Done (the commit "Theme slot buttons, sheen and spacing; the aero preset"; STATUS.md "Round 11") |
| 11B Widgets | The Start orb (`icon` `orb`, Start 1.1.0), the new built-in Show desktop widget (strip / glyph, Win+D / Win+M), the digital clock's `dateFormat` and `timeStyle` `taskbar` (1.1.0), three `sendKeys` entries, the `WINDOCK_SENDKEYS_LOG` dry run, ItemCommand 834 hovering widgets | Done (the commit "Start orb, Show desktop widget, taskbar clock layout"; STATUS.md "Round 11") |
| 11C The "Windows 7" preset | `res/presets/win7.json` (after `windows`), presets with a tray turn the takeover on, `measure()`'s `flush` (the Show desktop strip flush with the screen's end and as thick as a full-width bar), guide-docks "Make it look like Windows 7", README; no QueryState 295 | Done (the commit "Windows 7 preset, flush Show desktop strip, the guide"; STATUS.md "Round 11") |
| 11D Disable / enable a dock | `docks[].enabled` (any kind; written only when false), Sync parks a disabled entry (no window, weather, tray takeover or desktop rules for it), Docks and displays' Disable / Enable button (2815), DockCommand 156, QueryState 295; the last enabled dock stays on (also on load). From the user's request of 2026-11-07 ("hide/disable docks ... temporarily ... rather than deleting") | Done (STATUS.md "Round 11") |
| 11E Regression and release | Every round 11 phase script and round 10's suites on one clean dev build of the final tree, the cross-phase checks of the Windows 7 dock (pictures, Themes cards, the drawer, the three Item windows, the chooser and the welcome dialog, the Help sections, QueryState 285), a light check of 11D, the release build | Done (STATUS.md "Round 11 final regression") |

Design notes for 11A:

- **Defaults are the old constants.** `iconGap` 8 is `kGap`; `barPadding` 8 makes the bar
  `iconSize` + 16 (`kBarExtraHeight`). The plan left "today's cross-axis constant" open: the bar's
  extra 16 DIPs were never split evenly (5 above the icons, 11 below with the dots), so `barPadding`
  is half of it and, with the dots, the icon row sits at 5/8 of it from the far edge and the dots'
  centre 5/8 from the near edge: 5 and 5 at 8, exactly the old numbers (the plain dock's picture is
  identical to round 10's exe, pixel for pixel). With buttons there are no dots, so the icons are
  centred (Windows 7: a 32 px icon in a 40 px bar).
- **One layer per slot, drawn on change.** The button is a surface per slot (not a shared one like
  the dots: the stacked count, the hot point and the size differ per slot) behind the icon
  container, moved by the same animation as the dot, so a reorder / magnification moves it for
  free. `RenderButtons` runs at the end of every `ApplyTargets` and compares each slot's state code,
  stacked count, hot point (whole px) and surface size with what it drew: a pass where nothing
  changed draws nothing. The only redraw source while the pointer moves is the existing 16 ms
  magnification throttle (`ApplyMagnify`), and nothing runs at rest (measured: 0 ms of CPU in 3 s
  with a hovered frame). A press / release changes no placement, so they commit `RenderButtons`
  themselves.
- **The pulse is DWM's.** Attention attaches a repeating opacity animation (two smoothsteps,
  1 -> 0.55 -> 1 in 0.8 s, like the launch pulse) to the button layer once and detaches it when the
  attention clears; reduced motion shows it steady. The scrolling run's edge fade leaves a pulsing
  button alone (both drive the layer's opacity).
- **What gets a frame.** The slots that magnify (apps, folders, groups, the Recycle Bin, pinned tray
  icons); separators and widgets never. Running is the dot's rule, except that a minimized-only
  window slot keeps its frame (the plan: minimized apps count as running). Stacked = an app slot
  (not a window slot, not a group) with two or more windows. A ring keeps its dots and gets no frames
  (a rectangle does not follow a sector) and no sheen.
- **Shared drawing.** `Theme::DrawSlotButton` and `Theme::DrawSheen` draw the frame and the sheen
  in canonical pixels for both the dock (`DrawCanonical` maps them to the edge) and the settings
  preview strip, so the two cannot drift. The stacked outlines are clipped to the part beyond the
  frame in front (a translucent fill would otherwise show them through).
- **Ids.** 2422 (the plan's first choice for the group) is `kDockThemeNote`, so the Slot buttons
  heading is 2425; its note is 5500, the first of round 11's range. The theme field rows grew past
  3399 into 3400 - 3476 (that range was unused). QueryState 293 reports the code last drawn (what is
  on screen), not the code the state would give, so a test sees what the user sees.
- **The `aero` colours** are the plan's; they were not tuned against a Windows 7 screenshot in
  this phase (11C's picture with the full preset is the place to judge them).

Design notes for 11B:

- **The orb is canvas calls.** No image asset: gradients and four paths scale to any DPI and size
  and are lit / darkened per state by moving every colour stop (`shade`). The flag's panes share
  one S-curve (`cos(pi u)`) for their horizontal edges, so the gaps stay even while the flag
  waves; eight segments per edge are smooth at 32 px. The plan's "3 concentric rings" for the
  hover glow became one radial gradient (the same soft edge, one fill). At 100 % the orb is 0.92
  of the slot, so the glow has only the box's last 4 % to show in: the lighter sphere and the
  brighter rim carry most of the hover (measured: mean brightness +15.6 on the orb's 32 x 32 px).
- **Pressed is `onPress`.** The widget API has `onPress` (the plan's fallback, a 120 ms flash on
  `onClick`, was not needed). There is no release export, so the pressed look ends on `onClick`,
  on the pointer leaving (`onHover` false) or after a one-shot 2 s timer (a press that turns into a
  drag gets no click and may keep the hover); the timer exists only after a press, so nothing runs
  at rest.
- **"Default 100 % for the orb" without a per-choice default.** A schema default is one value and
  `visibleIf` only tests equality, so `size` became 0 - 100 with 0 = automatic (62 % for the
  logos, as before; 100 % for the orb). A stored size keeps its meaning; 11C's preset can still
  say 100.
- **`sendKeys` stays an allow-list.** It already accepted `win+d`; a general "modifiers + key"
  parser would let any `shell` widget type Ctrl+C or Alt+F4 into the focused app, so only three
  named entries were added (`win+m`, `win+shift+m`, `ctrl+shift+esc`) and a combination may hold
  three keys. The dry-run hook (`WINDOCK_SENDKEYS_LOG`) answers the plan's "a check that the key
  combination reaches SendInput" without pressing anything: it logs the exact VKs the INPUT array
  would hold. The test clicks the Start orb first as the canary (a failed dry run would only open
  Start) before the Show desktop widgets.
- **Show desktop previews as the glyph.** A 10 DIP translucent strip is a faint line in a 72 x 44
  drawer row, so the manifest's `preview.settings` pick the glyph style.
- **The taskbar clock** draws both lines at 12 DIPs regular (Windows 7: 9 pt Segoe UI) in
  `dock.theme.text` (white with `aero`) with 4 DIPs either side; `%#` reaches MSVC's `wcsftime`
  through `system.format` unchanged (verified: `11/6/2026`).
- **Left for 11C**: the strip sits 4 DIPs from the screen edge and `iconGap` (28) after its
  neighbour, and is as tall as the icon row, not the bar; the preset decides whether that is close
  enough or the end padding needs a theme / layout key.

Design notes for 11C:

- **`flush` is a measure() result, not a manifest key.** Show desktop has two styles and only the
  strip should touch the screen's end (a glyph flush with the edge looks cramped), so the flag
  comes with each measure. The layout passes the bar's thickness (`LayoutContext::barCross`, the
  icon row, its padding and titles, no reflections) only on a full-width bar: a flush box takes it
  as its cross size (the canvas grows with it) and, as the first or last slot, drops the end
  padding on that side (one helper, `EndPadding`, shared by the plain lead / trail, the anchored
  groups and the drag layout). On a centred, floating or ring dock it means nothing: a strip
  against a rounded panel's end would cut its corner. Measured: the strip is 10 x 40 px with its
  right edge at the monitor's 5120 px; the clock sits the widget gap (10 px) before it, close to
  Windows 7's spacing, so no gap key was added.
- **The tray in a preset turns the takeover on.** Every other way of adding the tray item (the
  dock menu's Add > Tray, the drawer, Put the tray in its own dock) does, and without it the slot is
  a dim dot; choosing the Windows 7 setup is asking for the notification area. The description
  and the guide say App settings > Tray icons turns it off. (Round 9 left the tray out of the
  Sidebar because presets did not turn it on; Sidebar is unchanged.)
- **The list order moves DockCommand 153's indexes.** `win7` goes after `windows` as the plan
  says, so sidebar / performance / dashboard are 5 - 7 now; 153 is a list index, not an id, and
  README lists the new order.
- **No QueryState 295.** 294 = 28 identifies `aero` among the built-ins and the saved file holds
  `themePreset`; a resolved-preset id would only duplicate them.
- **Start keeps `size` 0** (automatic = 100 % for the orb, 62 % for the logos) rather than the
  plan's 100, so switching the icon back to the logo keeps a sensible size.
- **Left as differences** (guide-docks): titles beside icons, Aero Peek, the orb overhang, jump
  lists, progress bars, the colour names, and the faint line the dock draws between leading
  widgets and icons (after the orb; the theme's Separator opacity 0 hides it).

Notes for 11E (round 11 closed 2026-11-07):

- **Numbering.** The regression was planned as 11D; the user's request of 2026-11-07 (disable a
  dock without removing it) became 11D (b1aeb9a) while the regression's first run was paused, so
  the regression is 11E and was run again, in full, on a clean build of that final tree.
- **What it found.** No behaviour regression: every round 11 phase script and every round 10 suite
  passes on the final tree (one flake each in two runs, both passing on a re-run), and the plain
  dark dock is still pixel-identical to round 10's exe. The only product fix is in README: 11D's
  QueryState 295 sentence had been inserted inside 11C's paragraph. Three scripts had gone stale
  through later phases (the strip's box after 11C's `flush`, the digital clock's tabs after 11B).
- **Left open**: the welcome dialog's list now needs its scroll bar for 8 presets (the chooser's
  fits); the inspector previews an orb Start item with the logo; the phases' "Not verified" lists,
  consolidated in STATUS.md "Round 11 final regression" (real pointer input and a real Win+D above
  all).

## Round 12 (complete 2026-11-09): customize the dock on the dock

Plan with the decisions: `C:\Users\olson\.claude\plans\windock-round12.md` (revision 2, from the
ideas the user kept from the first proposal plus their additions; working mockup linked there). One
commit per phase on master, not pushed; STATUS.md "Round 12" has each phase's notes.

The goals, in the user's words (the plan's list):

1. Customize a dock where it is: an edit mode on the dock, the drawer beside it as a grid with
   previews, drag widgets from the drawer onto the dock, a clear Done.
2. Dock settings that belong to the dock: they fly out from it, and the dock is always in view
   while it changes.
3. Looks without hex: a middle level (Quick look), a gallery, a full look per dock as well as the
   app's, and themes that can be saved, applied, imported and exported.
4. Nothing is a one-way door: undo, a visible Saved state, Keep / Cancel for templates, Keep /
   Revert after moving or hiding the dock.
5. Docks as things you keep: name, rename, duplicate, save as a layout and reapply later; titles
   that can be hidden, shown on hover or drawn inside the bar.
6. Offsets are easy to find: one "Where on the screen" card plus dragging the dock in edit mode.
7. A first run that sets up the docks, their looks, start with Windows and animation, and then
   shows the three gestures.

| Phase | Scope | Status |
|---|---|---|
| 12A One timeline | Undo / redo of every config change (50 entries, per process), the Saved state and actionable save errors, Keep / Revert after changes that move or hide a dock (`settings.confirmPlacement`), Keep / Cancel previews for presets, the banner near the dock, DockCommand 157 - 161, QueryState 296 - 300, ids 5600 - 5615 | Done, run (the commit "Undo timeline, Saved state, Keep / Revert and Keep / Cancel"; STATUS.md "Round 12") |
| 12B Looks | A full look per dock (`docks[].theme` over its `themePreset`; the app's values no longer reach it), Quick look (tint, transparency, corners, icon effects, dots), the gallery of looks, Save as theme / Import / Apply to docks; the Appearance tab became Look | Done, built, not run |
| 12C Docks you keep | `docks[].name` (rename in place, Rename dock…), Duplicate dock, the chooser's Layouts / Looks / Mine with saved layouts in `layouts\` and Keep my current look, `titleShow` / `titlePlace` / `titleUseName` | Done, built, not run |
| 12D Customize mode on the dock | The outline, remove badges, plain-drag reorder, anchor targets, "+" slots, the edit bar, the drawer popup beside the dock as a grid shared with the Contents tab | Done, built, not run |
| 12E The Dock settings panel | The Dock settings window became a borderless panel beside its dock (same class, pages, ids and automation); Position's "Where on the screen" with `edgeGap`; customize mode's dock drag | Done, built, not run |
| 12F Settings on a large display | Sizes from the display, two columns from 1000 DIPs, controls capped at 360 DIPs, the nav's descriptions, one sentence per page, the reasons beside unavailable controls | Done, built, not run |
| 12G Quick setup | Quick setup (docks, looks, behaviour, done) replaced the welcome dialog; three tips on the dock (`settings.tipsShown`) | Done, built, not run |
| 12H Docs pass, regression and release | Part 1: the user guides, README and this note rewritten for 12A - 12G (the guides are compiled into the exe, so they come before the release build). Part 2: the regression on everything at once and the release build | Done (2026-11-09): the first run of 12B - 12G, 6 fixes (the narrow panel's Contents and Position tabs, the edit bar title, the panel beside an outside title, Ctrl+Z / Ctrl+Y in customize mode, the foreground after customize mode, an up-down stretched in two columns), release 5,909,504 bytes; STATUS.md "Round 12 final regression" |
| 12I Dock settings: panel only | The user's decision 2026-11-09: no preview strip on the dock panel (the real dock is the preview; App settings > Themes / Animation keep theirs) and no captioned Dock window at all: a folder window's and a standalone widget's settings are the panel beside that window (the side with the most room; follows its moves through `Host::moved` → `WM_APP + 71`; closes when it is parked or hidden) | Done, built, not run |

Decisions taken (the plan's "Decisions to confirm", as built):

1. A dock's name is separate from its id (`docks[].name`), so a rename never breaks a reference.
2. A dock with its own look ignores the app's values completely: no layering. A dock that had a
   `themePreset` before 12B therefore loads with its own look (that preset, no fields of its own),
   and the app's inline fields stop reaching it (troubleshooting "My dock's colours changed after
   the update").
3. Templates (layouts, looks, gallery picks, Quick setup) are Keep / Cancel previews that cancel
   when left alone; placement changes are Keep / Revert and keep when left alone. A template is a
   try, a move is an edit.
4. The panel beside the dock replaces the Dock settings window (the user's decision, 2026-11-08):
   the same class, pages, ids and automation, no "Open in a window". App, Item, folder-window and
   standalone-widget settings stay windows. 12I superseded the last part (the user's decision
   2026-11-09): folder-window and standalone-widget settings are the panel too, beside their window;
   only App and Item settings are windows.
5. Undo lasts for the session only.
6. The long press that enters customize mode (600 ms, as hold-to-group) works only on an empty part
   of the bar, so it never conflicts with grouping.
7. `edgeGap` is a new setting; `offset` keeps its meaning and got the clearer label "Distance from
   the centre".

Left out on purpose (the plan's "Not in this round"): a simple / advanced switch, a fourth kind of
settings window, a web view or any new dependency; editing widget settings inside customize mode;
sharing layouts online. Also not built: Ctrl+Z on the dock itself in customize mode (the edit bar's
Undo and the dock menu's entry do it), an "Open layouts folder" command (the guide names the
folder), and a preview pane beside the Dock panel (12F: the real dock is the preview).

How it was built: the user cut the per-phase cost after 12A (2026-11-08): only 12A had a runtime
test; 12B - 12G were each verified by a warning-free `build\dev` build, WinDockTests and the two docs
checkers, and were not run on a dock until 12H. 12C / 12D and 12G / 12E were written in parallel
worktrees and merged by hand. A 12B - 12G bug may therefore surface only in the 12H regression.

Design notes for 12A:

- **One mechanism catches every change.** Every path that changes the config already ends in
  `App::RequestSave`, so that is where the timeline hooks in: it posts `kTimelineCommit` (once per
  burst), and the commit compares the config's text (`Config::ToJson(false)`, exactly what a save
  writes minus the settings windows' sizes) with the text recorded last; when they differ it pushes
  `{label, the text before}`. Posting is what makes one gesture one entry: everything a single
  message handler changes (a slider's release, a drag, a preset, a reset with its follow-up
  re-sorts) lands before the posted commit runs. Automation that sends a control message and then
  queries at once would race the post, so QueryState 296 - 300, the menus, Undo / Redo and a new
  settings window flush a pending commit first. Snapshots are a few KB of text; 50 of them are
  nothing next to the icon surfaces.
- **Restore is Reload config.** `ApplySnapshot` parses the snapshot with the same reader as the
  file (`Config::LoadJson`, split out of `LoadFile`) and runs Reload config's path (Sync,
  ApplyConfig + ReloadPins, every settings window refreshed), then saves. Two differences: the
  settings windows' sizes stay as they are (a resize is not a change), and the taskbar and the Run
  key are touched only when the snapshot changes `hideTaskbar` / `startWithWindows` (a "Show
  Windows taskbar temporarily" of this session must survive an unrelated undo). The restored text
  becomes the recorded state, so the posted commit after it finds nothing new.
- **Labels: first one wins.** A gesture's label is the first one noted: call sites that know what
  they do note theirs before changing anything ("Pinned Paint", "Applied preset Modern", "Reset
  Clock to factory defaults"), and the settings window adds a generic one after the page's handler
  returned, from the control that fired: its row's label (or the check box's caption, a radio
  group's first row or card) and its value now; a slider / up-down row also keeps the row's value
  text from its first scroll message, because the edit beside a slider follows the thumb only in
  the page's handler ("Icon size 48 → 64"). A change of a dock's placement with no label is named
  by the Keep / Revert text; anything else is "Change".
- **Keep / Revert is detected, not declared.** The commit compares each dock's edge, floating,
  display, auto-hide, layer and enabled (by id) before and after; any difference starts the bar,
  whatever made it (a settings control, the dock menu, a Ctrl+drag to another edge). Another change
  while it shows keeps the move (its Revert would undo the later change too). Revert is an entry
  of its own, so it can be undone.
- **A preview holds the save, not the timeline's state.** Starting one flushes pending entries and
  the pending save, so the file has everything before it; while it is open RequestSave only marks
  `saveHeld_`, and commits record nothing (the recorded state stays the one before it). Keep is
  then one ordinary commit with the preview's label; Cancel is `ApplySnapshot(before)`. Quitting
  with a preview open needs no special path: nothing ever wrote it.
- **The banner is plain Win32**, not a `PopupWindow` (those are D2D and hover / click models built
  around a dock's slot): one line, two real buttons (automation presses them as in the settings
  windows), `WS_EX_NOACTIVATE` and `MA_NOACTIVATE`, so it never takes the foreground. It shows only
  while no settings window is visible, beside the dock the bar is about.
- **Measured / decided**: `MoveFileEx` onto a read-only config.json fails with
  `ERROR_ACCESS_DENIED` (the test's "Couldn't save: config.json is read-only or locked."); the
  welcome dialog applies at once (12G reworks the first run); Revert this window counts the
  entries newer than the window's mark (an undone-then-redone older change is not counted).

## Dependencies at a glance

```
0 quick wins ──┐
1 elements ────┼─> 2 geometry ─> 3 multi-dock ─┬─> 11 shapes
               │                               ├─> 12 desktop folder docks (needs 7)
               └─> 4 widget runtime ─> 5 widgets ─> 14 port built-ins
                         │                └─> 13 calendars (needs 4)
                         └─> 8 tray ─> 9 notifications / badges
1 elements ─> 6 task list options ─> 10 virtual desktops
1 elements ─> 7 folders / groups ─> 8 tray drawer, 12
```

Phases 5 to 13 can be reordered freely within those edges.
