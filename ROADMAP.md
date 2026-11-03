# WinDock roadmap: feature round 4 and beyond

Written 2026-11-02 against commit 4a61f6d. Companion to STATUS.md. Nothing here is built yet.

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

- Port the digital clock, weather (widget and popup), calendar popup to scripts; the native
  versions stay as fallbacks until the ported ones have run for a release.
- Example widgets folder (`examples/`), `WIDGETS.md` API reference, manifest validation
  messages, "Export built-in to folder" button, widget settings UI generation finalised.

## Phase 5 round 2 (slot in by priority)

Network, battery, CPU / RAM, now-playing variants, desktop switcher skins.

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
