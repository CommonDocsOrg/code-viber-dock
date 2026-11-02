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

Goal: a widget is a folder, loads without compiling, draws through a canvas API, keeps its own
settings and data, and can be hot-reloaded.

- Engine: behind a `ScriptEngine` interface so the implementation can be swapped (see
  questions). One JS context per widget instance on the UI thread, with an execution-time
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

- 5a **Analog clock**: faces as data (`faces/*.json`: hands, ticks, numerals, colours), several
  shipped; `face` setting; seconds hand optional. **Analog date** (meaning to confirm: a date
  dial / flip calendar).
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

All on the `running` / `app` elements.

- Unstack windows: one slot per window, grouped by app, with the app's order kept.
- Window titles under icons: 1 line (8 chars) or 2 lines (16), ellipsis; raises the bar's cross
  size; on vertical docks shown beside the icon or off (setting).
- Live title updates in the preview popup (open follow-up).
- Thumbnails as items: each window slot shows a thumbnail with the app icon in a corner
  (technique per the questions); magnification scales the tile.
- Minimize-to-dock (minimized windows keep a slot; already true for unstacked windows).

## Phase 7: folders and groups

- Shortcut groups: element kind `group` (a label plus its own `items[]` of pins), opened like a
  folder; drag a pin onto a pin to create a group; rename from the menu.
- The folder / group view becomes a reusable control (`ItemGridView`): grid or list mode,
  scrolling (wheel and drag), more than 48 entries, keyboard navigation, drop onto it.
- Folder popup list mode: a scrollable list that pops out above the folder (the user's
  "stack list"); per-item setting grid / list / auto.

## Phase 8: system tray

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

- Spike: `UserNotificationListener::RequestAccessAsync` from an unpackaged exe on this build.
  If it needs package identity, decide between a sparse MSIX package (keeps the plain exe) and
  dropping the feature.
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

- Scrollable docks: when the items exceed the edge, the item run scrolls (wheel, drag, edge
  fade); magnification stays correct inside the viewport.
- Circle and half-circle docks: a polar layout mode in the axis abstraction (angle instead of
  main-axis position), background ring, popups anchored radially, magnification along the arc.

## Phase 12: desktop folder docks (Fences-like)

- Dock kind `folderWindow` on the desktop layer (Phase 3): a titled, resizable panel showing a
  folder through `ItemGridView` (Phase 7), live refresh via `SHChangeNotify`, drag / drop,
  the shell context menu, rename, delete.

## Phase 13: calendar integrations

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
