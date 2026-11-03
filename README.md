# WinDock

A macOS-style dock for Windows 11. C++20 / Win32, Direct2D + DirectComposition; one vendored
third-party library, the QuickJS-ng script engine for widgets (`third_party/quickjs-ng`, MIT,
compiled in). Single static exe (no VC++ redistributable needed).

Status: Phase 5. A translucent bar with a live clock at the bottom-center of the configured
monitor, plus a tray icon. Icons magnify under the mouse (macOS style), the hovered app's name
shows above it, and the dock auto-hides into the bottom screen edge. The Explorer taskbar is hidden while the dock runs and restored on
exit, crash recovery, or `--restore-taskbar`. Running apps appear on the dock (grouped per app,
with a dot under each). Everything is laid out in the order of the dock's `items` in config.json
(pinned apps, folders, separators, the Recycle Bin, the clock, the weather widget, and a `running`
item where the unpinned running apps go, in the order they were first seen). A running app that
matches a pin merges into the pinned item.

## Build

Requires Visual Studio 2022+ (MSVC, CMake, Ninja) and the Windows 10/11 SDK. Run from a
developer environment so `cl.exe` and `ninja` are on `PATH`:

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset x64-release
cmake --build --preset x64-release
```

Output: `build\x64-release\WinDock.exe`. Use the `x64-debug` preset for a debug build. It links
only Windows SDK libraries (Direct3D 11, Direct2D, DirectComposition, DirectWrite, WIC, the shell,
DWM, common controls, `windowsapp` for the JSON parser, `winhttp` for the weather widget, and
`dxguid` for the Direct2D effect ids of the icon glow).

## Run

```bat
build\x64-release\WinDock.exe                     :: start the dock (only one instance runs)
build\x64-release\WinDock.exe --quit              :: ask the running dock to exit
build\x64-release\WinDock.exe --restore-taskbar   :: bring the Explorer taskbar back
build\x64-release\WinDock.exe --settings          :: open the settings window (starts the dock if needed)
```

Right-click the tray icon or an empty part of the dock for the menu:

- **Settings…**: opens the settings window (see Settings), or brings it to the front.
- **Position on screen** submenu (radio items): **Bottom**, **Top**, **Left**, **Right**. Moves the
  dock to that edge of its monitor (`edge`; alignment and offset are kept) and saves it. The
  submenu ends with a reminder that **Ctrl+drag** moves the dock (see Position). See
  [Position](#position-edge-alignment-offset).
- **Auto-hide** submenu (radio items): **Off**, **Always**, **When a window overlaps**. Sets
  `autoHide` (`"off"`, `"always"`, `"intelligent"`) immediately and saves it.
- **Hide taskbar** (checkmark): hides or restores the Explorer taskbar immediately and saves
  `hideTaskbar`.
- **Start with Windows** (checkmark): writes or deletes the `WinDock` value under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` (the quoted path of the running exe). The
  checkmark shows whether that value exists; `startWithWindows` in the config follows it.
- **Refresh weather**, **Weather settings…** (only while the weather widget is on; Refresh is
  greyed until a location is set): fetch the forecast now; open Settings on the Weather page.
- **Open config file**, **Reload config**: see Configuration.
- **Show taskbar until re-enabled**: brings the taskbar back for this session only. "Hide taskbar"
  shows unchecked until you check it again; the config is not changed, so the next start hides
  the taskbar again. `WinDock.exe --restore-taskbar` does the same for a running dock.
- **Quit**.

### Magnification, name label, auto-hide

The descriptions below are for a bottom dock. On the other edges everything turns with the dock
(see [Position](#position-edge-alignment-offset)): "up" means away from the screen edge, "left /
right" means along the dock, the label and the popups open on the side facing the middle of the
screen, and auto-hide slides the dock off its own edge.

- **Magnification**: icons near the mouse grow, up to `magnification` times their size for the
  icon under the pointer, falling off with a cosine curve (or a straight line with
  `animation.magnifyFalloff: "linear"`) over `magnifyRange` neighbouring slots.
  Magnified icons grow upwards from their bottom edge and push their neighbours outwards. The
  layout is anchored at the pointer, so the icon under the pointer stays under it and the bar
  grows on both sides away from the pointer (like macOS). The bar's width animates with the icons.
  The running-indicator dots keep their size and follow their icons. Everything moves through
  DirectComposition animations (80 ms while the mouse moves, 120 ms back to rest), so no
  surface is redrawn. Magnification updates are capped at about 60 per second. The return to rest
  (and every other duration in this section except the 80 ms follow and the delays) is divided by
  `animation.speed`; see [Animation](#animation).
- **Name label**: the app name fades in above the hovered icon after 250 ms, follows it while
  you move along the dock, and fades out when the mouse leaves (`animation.labelFadeMs`, 120 ms;
  0 = appears and disappears at once).
- **Window previews** (`previews`, on by default): rest the mouse on an app with two or more
  windows for `previewDelayMs` (600 ms) and a popup opens above the dock, centred on the icon, with
  one card per window: the window title (shortened with an ellipsis), a live thumbnail rendered by
  DWM, and a close button (x) in the card's top-right corner. The name label is hidden while the
  popup is open. Apps with one window keep just the label.
  - Hover a card to highlight it. Click a card to bring that window to the front (restoring it if
    minimized); the popup closes. Click the x, or middle-click a card, to close that window
    (`WM_CLOSE`, so the app can still ask to save). Its card goes away once the window is gone; the
    popup closes when the app has no windows left. If the popup shrinks while the mouse is in it,
    it shifts so the pointer stays inside.
  - Moving to another app with several windows switches the popup to it; moving to an app with one
    window closes it. Leaving both the icon and the popup closes it after 300 ms (moving between
    them is fine). Clicking the dock, opening a menu, starting a drag, or the dock hiding also
    closes it; after a click or a menu it does not reopen for the same icon until you hover
    another icon or leave the dock.
  - The dock does not auto-hide while the popup is open.
  - At most 6 cards (fewer if they would not fit on the monitor); further windows show as
    "+N more". Thumbnails keep the window's aspect ratio inside a 200 x 130 DIP box. A minimized
    window shows its last frame if DWM kept one, otherwise the app icon.
  - The popup (class `WinDockPreview`) fades in and out over 120 ms (divided by
    `animation.speed`). With acrylic on it uses the same blurred backdrop as the bar (with the
    system's 8 DIP corners), otherwise `theme.background`. While it is open and the mouse is
    still, the dock does no work; DWM draws the thumbnails. Thumbnails are released as soon as
    the popup closes.
- **Auto-hide** (`autoHide`, `"always"` by default): when the mouse leaves the dock, it waits
  `hideDelayMs`, then slides down and fades out (180 ms at `animation.speed` 1;
  `animation.hideStyle` chooses slide only, fade only or instant). The hidden dock is a 1 px tall
  invisible strip along the whole bottom edge of its monitor, so it no longer covers anything
  except that bottom row of pixels. Push the mouse against the bottom edge and keep it there for
  `revealDelayMs` to bring the dock back. Leaving the edge sooner cancels the reveal. Moving
  back onto the dock while it is sliding away reverses the slide. The dock does not hide while
  one of its menus is open.
- The dock is not revealed while a fullscreen app, game, or presentation runs
  (`SHQueryUserNotificationState` reports busy, Direct3D fullscreen, or presentation mode).
- A window that appears or flashes while the dock is hidden does not reveal it (with
  `"intelligent"`, a new foreground window that does not overlap the dock does, see below). A
  flashing app's dot is orange when you next reveal the dock.
- With `"autoHide": "off"` the dock stays visible. It still floats over windows and reserves no
  screen space.
- `"autoHide": "intelligent"` (menu: **When a window overlaps**) hides the dock only while the
  foreground window overlaps it, and shows it again by itself as soon as that window no longer
  does (no need to touch the bottom edge).
  - "Overlaps" means the foreground window's visible frame (without the invisible resize borders)
    intersects the bar's column, from the top of the resting bar down to the bottom of the
    monitor. A maximized window counts (the taskbar is hidden, so it covers the bottom edge), and
    so does a fullscreen app. A window beside the bar, or above it, does not, even if it reaches
    the bottom of the screen elsewhere.
  - Only the foreground window counts. Minimized, hidden or cloaked windows (another virtual
    desktop), the desktop, Start / Search and the other shell surfaces, and the dock's own
    windows never count, so clicking the desktop or opening Start shows the dock.
  - Becoming overlapped starts the usual `hideDelayMs` wait, unless the mouse is on the dock or
    one of its menus / the preview popup / a drag holds it; the dock then hides once the mouse
    leaves. While it is hidden, the bottom-edge strip still reveals it as with `"always"`; once
    the mouse leaves, it hides again if the window still overlaps it.
  - The fullscreen suppression above still applies: the dock does not show by itself, or from
    the strip, while `SHQueryUserNotificationState` reports a fullscreen app or presentation.
  - It reacts to activation, moves and resizes (dragged or programmatic, Win+Arrow snapping),
    maximize, minimize and restore, within about 100 ms, through WinEvent hooks
    (`EVENT_SYSTEM_FOREGROUND`, `EVENT_SYSTEM_MOVESIZEEND`, `EVENT_SYSTEM_MINIMIZESTART/END`, and
    `EVENT_OBJECT_LOCATIONCHANGE` limited to the foreground window's thread). The hooks exist
    only in this mode; `"off"` and `"always"` install none. Nothing runs while no window changes.
    The location hook also reports the mouse cursor while it moves over some apps' windows
    (Chromium-based apps, for example); the first such event turns that hook off for 100 ms,
    after which it is turned back on and the overlap is re-tested once, so a moving mouse costs
    at most about ten cheap wake-ups a second instead of one per mouse move.
- At rest the dock window covers exactly the drawn bar, so clicks just above or beside it reach
  the windows underneath. When the mouse enters the bar, the window grows to make room above it
  for magnified icons and the label and beside it for the bar to grow; the cursor is on the bar
  at that moment, so the larger transparent area does not get in the way. Once the mouse has
  left, the label has faded out and the icons are back at rest (about 160 ms at speed 1), it
  shrinks back to the bar. While sliding in or out it covers the bar's column down to the screen
  edge (plus room above the bar for a `"spring"` overshoot). The 8 px margin below the resting
  bar belongs to the windows underneath too.
- If the mouse reveals the dock at the edge far to its side, the dock hides again after the
  delay, and stays hidden until the mouse leaves the edge and comes back.

### Displays and DPI

The dock follows DPI changes: it redraws every icon at the new pixel size and lays itself out
again. On display changes (resolution, monitors added or removed, work area changes) it moves
back to its place on its edge of its monitor (see Position and Multiple docks).

### Multiple docks and monitors

`docks[]` may hold any number of docks; every one is shown, each with its own items, edge,
position, auto-hide, lock and the other per-dock keys (they all run in the one WinDock process and
share the window list, the weather data and the foreground watch; each has its own window, renderer,
icon cache and popups). Per dock:

- `monitor`: `0` (default) = the primary monitor, `1`, `2` ... = the other monitors in
  `EnumDisplayMonitors` order (Settings > General > Display lists them), `"primary"` (the same as
  `0`, spelled out), or `"each"`: one copy of the dock on every connected monitor, all showing the
  same config (a change on one copy, such as a pin or a move, changes them all; state that is not
  saved, such as the order of the running apps after a drag, is per copy). A dock whose monitor is
  not connected is **parked**: it keeps its place in `docks[]` and in the automation numbering but
  has no window until the monitor is back. Display changes (`WM_DISPLAYCHANGE`,
  `WM_DEVICECHANGE`, a dock's `WM_DPICHANGED`) re-enumerate the monitors and park / unpark docks
  and add / remove the copies of an `"each"` dock (deferred while a dock menu or dialog is open).
- `runningFilter`: `"all"` (default) or `"monitor"`: the dock shows only the windows whose centre is
  on its monitor (a minimized window counts where it was restored); this applies to the running
  apps and to the dots and window lists of pinned apps. (A virtual-desktop filter is not planned.)
- `themePreset`: a theme preset (built-in id or theme file) for this dock only; empty (default) =
  the global `theme.preset`. The global inline theme fields still apply on top of it.

- `layer`: `"normal"` (default: topmost, above windows) or `"desktop"`: the dock is shown only
  while the desktop is the foreground (after Win+D or a click on the wallpaper; also while one of
  WinDock's own windows, such as its popups or the settings window, is in front) and hides as soon
  as another window comes to the foreground; while shown it sits at the bottom of the z-order,
  directly above the desktop window (never topmost), so every window stays above it. It follows
  the same WinEvent hooks as intelligent auto-hide (no polling). A desktop dock reserves no
  screen space and draws its bar without the acrylic blur. `WinDock.QueryState` 52 / 53.

Docks on the same edge of one monitor overlap; move one (Position, Ctrl+drag, or Settings >
Layout).

**Settings > Layout** draws every monitor to scale (its work area shaded) with every dock on its
edge at its alignment / offset (full width across the edge), labelled with its id (and title).
Click a dock to make it the dock the other pages edit; drag it along its edge (it snaps to the
start, centre or end within 24 DIP, like Ctrl+drag on the dock; otherwise the nearest alignment
plus the offset), onto another edge (the edge nearest to the dragged dock's centre) or onto another
monitor (its `monitor` index; an `"each"` dock stays on every monitor, `"primary"` stays
`"primary"`). An outline shows where it will go; the dock moves and the config is saved when the
button is released (Escape or a lost capture cancels). A full-width dock only changes edge /
monitor. Settings > Docks
lists the docks (id, edge, display, whether it is shown or parked), adds one (`id` `"dock"`,
`"dock2"` ..., bottom of the primary monitor, the running apps and the clock), duplicates or removes
the selected one (never the last), and **the selected dock is the one the other pages edit**
(General, Behaviour, Dock contents, the "This dock's theme" combo on Appearance). The dock menu
of each dock (and the tray menu, for the first dock) acts on that dock; with more than one dock
it also has **Lock all docks** / **Unlock all docks** (`WinDock.DockCommand` 124 / 125; Settings >
Docks has the same buttons).

### Position: edge, alignment, offset

Per dock, `edge` puts the dock on the `"bottom"` (default), `"top"`, `"left"` or `"right"` edge of
its monitor, `align` places it along that edge (`"center"`, default; `"start"`: the left end of a
bottom / top dock, the top end of a left / right one; `"end"`: the other end, both 8 DIP from the
monitor's end), and `offset` (DIPs, i.e. pixels at 100 % scaling, default 0, -16000 - 16000) moves
it from there along the edge, toward the right / bottom when positive. The dock never leaves its
monitor: an offset that would push it off is cut at the monitor's end. The dock menu's
**Position on screen**, Settings > General > Position and `WinDock.DockCommand` 120 - 123 set the
edge; missing keys mean the defaults, and every save writes all three.

The layout is computed once for every edge in "main" (along the dock) and "cross" (across it,
toward the screen edge) coordinates and then mapped onto the screen, so an edge changes where
things go, not how they behave:

- **Bottom / top** docks are horizontal; a top dock is a bottom dock mirrored: the icons stand on
  the edge-facing side of the bar (the top), the running dots sit between the icons and the screen
  edge, magnified icons grow downward, the name label, the window previews and the folder /
  weather / calendar popups open below the dock, and auto-hide slides it up off the screen into a
  1 px strip along the top edge.
- **Left / right** docks are vertical: the icons stack along the edge (in config order, top to
  bottom), magnification grows along the edge and away from it, the label is drawn beside the
  magnified icon toward the middle of the screen (upright, vertically centred on the icon), and
  the popups open beside the dock, centred on the item along the edge and kept on the monitor.
  The dots are on the screen-edge side of the icons. While the mouse is on a vertical dock its
  window is wide enough for the longest label (about 400 DIP across), at rest it is just the bar.
- **Widgets on a vertical dock** stay upright and are laid out narrow: the clock puts every word
  on its own line (`14:45` over the date's `Mon` / `02` / `Nov`; a 12-hour format puts `PM` under
  the time), the weather widget puts its glyph above the temperature. Their height is their
  length along the dock.
- **Icon effects**: reflections point toward the screen edge, so they are drawn under the icons of
  a bottom dock and above the icons of a top dock (the icon's top mirrored about its top edge);
  left / right docks have no reflections (Settings says so under Icon effects). The glow is the
  same on every edge. The perspective tilt rotates about the bar's screen-edge side (about the X
  axis on horizontal docks, the Y axis on vertical ones), so the side facing the middle of the
  screen recedes.
- **Placement at the monitor's ends**: with `"start"` / `"end"` (or an offset that brings the dock
  near an end), magnification cannot grow the bar past the monitor's end; it grows away from it
  instead (the icon under the pointer then drifts a little off the pointer at the very end).
- The Explorer taskbar handling is unchanged: it is hidden whatever edge the dock is on.

**Moving the dock by dragging**: hold **Ctrl** and drag the bar (anywhere on it, icons included)
of an unlocked dock. The dock follows the pointer along its edge (it stays on the monitor); on
release it snaps to the centre or to an end when it is within 24 DIP of it, otherwise it keeps the
nearest alignment (centre, start or end) plus the distance from it, saved as `align` and `offset`.
Drag it out of its edge's band (a quarter of the monitor) toward another edge and the dock dims;
releasing there puts it on that edge, at the pointer. Escape cancels; a Ctrl + click without a
move is an ordinary click. A locked dock does not move (`locked`, **Lock dock**). A full-width
dock only changes edges. `WinDock.QueryState` 46 reports the drag.

**Full width** (`fullWidth`, default `false`): the bar spans the whole edge of the monitor, flush
with the screen edge (no margin) and with square corners (also the acrylic blur). The items sit in
it at its start, centre or end per `align`, moved by `offset`, and keep the bar's padding from the
monitor's ends. Magnification grows the run of items inside the bar (the bar's own ends slide past
the monitor's, where the window cuts them off, so the bar always looks full); auto-hide works as
usual (the whole bar slides off the edge).

**Reserve screen space** (`reserveSpace`, default `true`; any edge, full width or not): the dock
registers its window as an app bar (`SHAppBarMessage` `ABM_NEW` / `ABM_QUERYPOS` / `ABM_SETPOS`)
so the shell takes the resting bar's band (from the screen edge to the bar's far side, its margin
included) out of the work area: maximized windows stop at the dock and popups / the settings
window stay clear of it, while the dock stays completely visible. An app bar always spans the
whole edge, so a dock that is not full width still reserves a strip along the whole edge of its
monitor. The band does not grow while icons magnify: magnified icons, the label and popups
overlap windows as before. Each dock reserves its own band on its own monitor (two docks on one
edge stack, like two app bars do). **Only a dock with `autoHide` `"off"` reserves
space**; with `"always"` or `"intelligent"` nothing is reserved (like Explorer's auto-hide
taskbar: an intelligent dock would otherwise never be overlapped, so it would never hide). The
registration follows the bar (edge, monitor, DPI), answers `ABN_POSCHANGED`, and steps out of the
topmost band while the shell reports a fullscreen app (`ABN_FULLSCREENAPP`). It is removed when
any of those conditions goes away, before the Explorer taskbar is restored or hidden again (so
the saved work area never includes it), on quit and when the dock window is destroyed; the crash
filter removes it too, and after a hard kill the shell drops the dead window's band by itself.
With the Explorer taskbar shown on the same edge (hide taskbar off) the shell stacks the two, so
both reserve their height. `WinDock.QueryState` 43 tells whether the dock is registered.

**Peek strip** (`peekPx`, DIPs, default 0 = off, 0 - 64; `peekOpacity`, default 0.5, 0.05 - 1):
while auto-hide keeps the dock hidden, that much of the bar's far side (the top of a bottom dock)
stays on screen at that opacity instead of the invisible 1 px strip, and hovering it for
`revealDelayMs` reveals the dock as before. The dock always slides to the peek position (also with
`hideStyle` `"fade"`); the acrylic blur is not shown on the strip. `WinDock.QueryState` 44.

**Title** (`title`, default empty, up to 128 characters): a label in the name label's font, colours
and pill, drawn outside the bar on the side facing the middle of the screen (above a bottom dock,
beside a vertical one), centred on the items. It takes no part in hit testing (hovering or
clicking it does nothing), hides and slides with the dock, and the resting window includes its
band. `WinDock.QueryState` 45.

### Dock items

- Left click: launches the app if it has no windows; otherwise activates, minimizes, or cycles
  through its windows. While an app is launching its icon bounces, until a new window shows up or
  5 s pass (`animation.launchStyle` can make it a single hop, an opacity pulse, or nothing).
- Middle click on an app (`middleClick`, Settings > Behaviour): starts a new instance (the
  default; it bounces until its window appears), minimizes all its windows, closes all its
  windows (each gets a normal close request, so apps can still ask to save), or does nothing.
  Folders and the Recycle Bin always open in Explorer.
- Mouse wheel over an app (`wheel`, Settings > Behaviour): nothing (the default), or switch
  between its windows: each notch towards you brings the next window to the front (in the order
  the windows appeared, wrapping around), away from you the previous one; with one window it is
  brought to the front.
- Right click: Pin to dock / Unpin from dock, the icon entries for a pinned app (see **Custom
  icons** below), New window (shows as Open when the app is not running), Open file location (for
  path-based items), Close for each window, then the dock menu.
  Folders, separators and the Recycle Bin have their own short menus (below); a widget (the
  clock, the weather) shows the dock menu. The dock menu has **Lock dock** (checked while the
  dock is locked, see below), **Add separator** (appends a separator to the pinned items) and
  **Add widget >** **Clock** (added at the end), **Weather** (before a clock that ends the list,
  else at the end) and **Running apps** (after the last pin; greyed while the dock has a running
  apps item). Several clocks or weather widgets are allowed.
- **Lock dock** (`locked` in the dock's config, also a check box on Settings > Dock contents):
  while locked nothing can be dragged (no reordering, no unpinning by dragging an icon away),
  files dropped between icons are not pinned (the pointer shows "no drop"; dropping a file on an
  app to open it, or on the Recycle Bin, still works) and the dock menu's Add separator and Add
  widget are greyed. The settings window still changes everything.
- Drag an icon to reorder: press on it and move past the system drag threshold. The icon follows
  the pointer (slightly enlarged and translucent), magnification and the name label pause, the
  dock does not auto-hide, and the other icons slide apart to open a gap where it would land (the
  slot under the pointer). Release to drop it there; the new order is saved within 300 ms. Press
  Escape while dragging (or let another window take the mouse) to cancel: everything slides back.
  A press and release without moving past the threshold is still a click.
  - Pinned icons move within the pinned section only.
  - A running app that is not pinned can be moved anywhere among the running apps; that order
    lasts for the session (it is not saved, and newly started apps are added at the end). Dropped
    on a pinned slot, it is pinned at that position. Windows without a usable app identity (shown
    with a class-name fallback) can be reordered among the running apps but not pinned.
  - Drag a pinned icon well above the dock (more than 1.5 bar heights above the bar) and release
    to unpin it: the icon dims while it is in that zone and fades out when released. This only
    applies to pinned apps with no open windows; a running pinned app dropped there just goes back
    to its slot (use the menu's Unpin for those).
  - The clock and the weather widget can be dragged the same way (a press and release without
    moving is still a click). The widget follows the pointer in front of the icons, the other
    items make room, and on release its item moves to the new place in the dock's `items` list
    (saved within 300 ms). It goes in front of the first item whose centre is right of the
    pointer; the running apps count as one block, so a widget released over them lands before or
    after the whole block, whichever side of the block's centre the pointer is on. (Move the
    running apps block itself on Settings > Dock contents.) A widget cannot be dragged off the
    dock; remove it in Settings. When the widget's new place changes the dock's width (the gaps
    around widgets move with it), the dock settles at once instead of animating the landing.
- Drag files onto the dock. What a drop does depends on where the pointer is:
  - Over the middle 70 % of an app's slot: the icon grows a little (no marker) and dropping opens
    the files with that app. When the app is registered for the file type (the first file's
    extension), the files are handed to it the way Explorer's **Open with** does; otherwise the
    app is started with the quoted file paths as arguments (a shortcut pin: its target with the
    shortcut's own arguments first; a Store app: its app execution alias, or the activation
    manager with arguments).
  - Over the middle 70 % of the Recycle Bin: the bin grows the same way and dropping moves the
    files to the Recycle Bin (with undo, and the shell's progress window for a large delete).
  - Anywhere else (between icons, over a folder or a separator): `.lnk`, `.exe` files and folders
    are pinned. The icons open a gap at the insertion point with a thin accent-coloured marker in
    it (between the two icons whose centres the pointer is between, within the pinned section);
    dropping pins them there. Files whose app is already pinned are skipped; other file types do
    nothing there. A locked dock pins nothing there.

  Dragging files to the bottom screen edge reveals a hidden dock immediately. File drops use OLE
  drag and drop, so they work from Explorer and any other drag source running at the same
  integrity level (not from an elevated app).
- Pinning a running app adds it to the end of the pinned list, and the icon moves to that slot.
  Unpinning a running app keeps its icon until its last window closes. The icon moves to the
  running section in first-seen order, as on macOS.
- When a pinned file no longer exists, its entry stays in the config and the dock shows a
  placeholder icon. The dock never deletes pins silently.
- **Folders** ("stacks"): a pin whose path is a folder, or a shortcut to a folder, is a folder
  item. It shows the folder's own icon (custom `desktop.ini` icons included) and the folder's name
  (`C:` for a drive root); there is no launch bounce and no running dot, because Explorer's
  windows belong to File Explorer. A folder and a shortcut to it are the same item (shown once).
  Pin one by dropping it (or a shortcut to it) on the dock, with **Add folder…** on the Dock
  contents page, or in config.json.
  - A left click opens the folder's contents in a popup above the icon (beside it on a left /
    right dock); clicking the icon again closes it. The popup shows the folder's name, an **Open
    in Explorer** button and its entries: sub-folders first, then files, in Explorer's name order
    ("file2" before "file10"), hidden and system entries left out, with the shell's icons. Two
    layouts, chosen per folder item with `"view"` (right click > **View**, or Settings > Folders
    and groups):
    - `"grid"`: cells with the icon and a two-line name, up to 6 columns and 8 rows visible;
    - `"list"`: a scrolling list ("stack list") 340 DIPs wide, up to 14 rows visible: icon, name,
      and the size and date at the right (folders: the date);
    - `"auto"` (the default): a grid up to **48** entries (exactly the round-3 popup), a list for
      more.
    Both scroll when there are more entries than fit: the mouse wheel (a grid row, or three list
    rows, a notch, animated unless reduce motion is on), dragging the contents up / down, or the
    thin scrollbar at the right edge (it fades in while scrolling and on hover, out after a
    second; drag its thumb, click above / below it to page). Only the rows on screen have
    surfaces, so a folder with thousands of entries costs a screenful (icons load as rows scroll
    in). Nothing runs while the popup is still.
  - In the popup, click a file to open it (the popup closes), click a sub-folder to show its
    contents in place (the header then shows the path from the pinned folder, `Downloads › Photos
    › 2026`, leading parts shortened to "…" when it does not fit, and a back arrow), the back arrow,
    Backspace or Alt+Up to go up again (never above the pinned folder; the sub-folder you came
    from is selected), middle-click a sub-folder to open it in Explorer. Keyboard: arrows, Home /
    End, Page Up / Down move the selection (scrolled into view), Enter opens it, typing selects
    the next entry whose name starts with the typed text (keys within a second extend it).
    Right click an entry: **Open**, **Open in Explorer** (a folder) / **Show in Explorer** (a file).
    Escape, clicking anywhere else (another window, the dock), a menu, a drag or the dock hiding
    close it. While it is open the dock does not auto-hide.
  - Drop files onto the popup to move or copy them into the folder it shows, or into the
    sub-folder under the pointer (it is highlighted), as Explorer does: a move on the same volume,
    a copy across volumes; Ctrl copies, Shift moves. The shell's own file operation does it (undo,
    progress and conflict dialogs included) on a thread of its own; the popup then shows the new
    entries. Dropping onto the folder's icon on the dock still pins the files (below).
  - Middle click on the icon opens the folder in Explorer. Right click: the name, Unpin from
    dock, the icon entries, Open in Explorer, View (Automatic / Grid / List), then the dock menu.
- **Shortcut groups** (`{ "kind": "group", "name": "Tools", "items": [ ...apps / folders... ] }`):
  one slot for several shortcuts, like a folder on a phone's home screen. Its icon is a rounded
  tile with the first four members' icons in a 2 x 2 grid (or a custom icon: Choose icon… /
  Choose image…); its label is its name ("Group" when it has none).
  - Make one by dragging an icon onto another pinned app or folder and **holding it there for
    0.6 s**: while the pointer is over the middle half of that icon the others stop making room,
    after the hold the target grows a little, and releasing makes a group of the two at the
    target's place (a running app that is not pinned can be dragged in too). Drag an icon onto a
    group to add it (no hold needed); release before the hold time or outside the middle and it is
    an ordinary reorder.
  - A click opens its popup (the folder popup's look: grid or list per its `view`, scrolling,
    keyboard, type-to-select): click a member to start it, or bring its windows to the front when
    it runs. Right click a member: **Open**, **Remove from group**, **Move to dock** (a pin right
    after the group). Drag a member out of the popup onto the dock to make it a pin there (the
    insertion marker shows where). Drop `.lnk` / `.exe` files or folders onto the group's icon or
    its popup to add them; other files get the no-drop cursor. The last member removed removes
    the group. Middle click on a group does nothing.
  - Running members: the group shows a dot while any member runs (bright while one of them is
    the active app, orange when one wants attention). The running apps themselves also appear as
    usual with the dock's `running` item (they are not pins); the group has no window previews.
  - Right click the group: **Show contents**, **Rename…** (a small dialog), **Ungroup** (the
    members become pins at its place), **Unpin group and its N shortcuts**, the icon entries and
    **View**. Settings > Folders and groups edits the name, the view and the members (Add app…,
    Add folder…, Remove, Move up / down); Dock contents shows a group as "Group" with its members.
  - Older versions (before round 4 phase 7) do not show a group; they keep its `kind` and `icon`
    but drop its name and members if they save the file.
- **Recycle Bin** (`{ "kind": "recycleBin" }` in the dock's `items`, or **Add Recycle Bin** on the Dock
  contents page): shows the bin's empty or full icon, which follows the bin (the dock asks the shell to
  tell it when the bin changes; nothing polls). Its name label reads "Recycle Bin (12 items,
  3.4 MB)", or "Recycle Bin (empty)". A click opens the Recycle Bin; files dropped on it are
  recycled. Right click: **Open**, **Empty Recycle Bin** (the shell's own confirmation; greyed when
  the bin is empty), **Unpin from dock**, the icon entries, then the dock menu.
- **Separators** (`{ "kind": "separator" }` in the dock's `items`, any number; **Add separator** in the dock
  menu or on the Dock contents page): a thin vertical line in `theme.separator` in a narrow slot
  (12 DIPs). It has no name label and no dot, is never magnified (its neighbours are) and does not
  react to clicks. It can be dragged to another place or off the dock like any pin; right click
  shows **Remove**, then the dock menu.
- **Custom icons**: any pin (app, folder, Recycle Bin; not separators) can have its own icon,
  stored as the entry's `"icon"` (see Configuration). The right-click menu of a pinned item has:
  - **Choose icon…**: the Windows Change Icon dialog, opened on the item's current icon (a custom
    icon, a shortcut's own icon, else the app's exe; shell32.dll's folder / Recycle Bin icon for
    those kinds). Browse to any `.exe`, `.dll` or `.ico` and pick an icon.
  - **Choose image…**: an open-file dialog for images and icon files (`.png .jpg .jpeg .bmp .gif
    .ico .exe .dll`). An image or `.ico` is used as it is; for an `.exe` or `.dll` the Change Icon
    dialog follows to pick which icon.
  - **Reset icon** (greyed when the entry has no `icon`): back to the default icon.

  Both dialogs belong to the dock: they open in front of it, and the dock does not auto-hide
  while one is open. What the dialog returned is stored as an absolute path (`"file,index"` for
  an icon in an `.exe` / `.dll`), saved within 300 ms and drawn at once; the Dock contents page in
  Settings has the same three buttons. Editing `"icon"` by hand and choosing **Reload config**
  also redraws the icon at once.

How a pin is matched with running windows:

- `.lnk` pins use the shortcut's own `System.AppUserModel.ID` when it has one. If the target is
  an app execution alias (for example `%LOCALAPPDATA%\Microsoft\WindowsApps\mspaint.exe`), the
  package AUMID stored in the alias is used instead. Otherwise the pin matches on the resolved
  target exe path. Launching a `.lnk` pin starts the shortcut itself, so its arguments and working
  directory are kept. The shortcut's file name is the item's name.
- `.exe` pins are matched the same way as a shortcut target. `aumid` pins match the AUMID.
- Some apps report more than one identity. An alias table in `AppIdentity.cpp` maps those to one
  key. For example, Explorer windows report `Microsoft.Windows.Explorer`, which is treated as the
  same app as `%WINDIR%\explorer.exe`.

While hiding the taskbar the dock writes `%LocalAppData%\WinDock\taskbar-hidden`, holding the
original taskbar state. If the dock is killed or crashes, the next start (or
`WinDock.exe --restore-taskbar`, which works without a running dock) restores from it. The Start
menu still opens with the Win key while the taskbar is hidden.

### Running apps: windows and titles

- **One slot per window** (`windows`: `"stacked"` (default, one slot per app) or `"unstacked"`):
  every window gets a slot of its own with its app's icon, grouped by app in the app order the
  dock already uses (first seen; the windows in the order they appeared). Its hover label shows
  the window title, a click activates that window (minimizes it when it is already in front), the
  preview popup shows that one window (whatever `previewsSingleWindow` says), a middle click acts
  on that window, and the dot marks it: bright while it is the active window, none while it is
  minimized. **Minimize to dock**: a minimized window keeps its slot (its dot goes away) until it
  is closed. **A pinned app keeps its pin slot**, which works as before (a click launches the app or
  activates / cycles its windows, its dot and preview are the app's); its windows follow it as
  extra slots right after it. Window slots cannot be dragged (the order is the app order); their
  item menu is the app's (Close lists that one window).
- **Titles under icons** (`titles`: `"off"` (default), `"one"`, `"two"` lines): the window title
  in a window slot, else the app's (or folder's, or the Recycle Bin's) name, in the label's font
  (Segoe UI) at 10 DIP in `theme.clockText`, centred in the icon's slot and ellipsized (one line is
  about 8 characters at the default 48 DIP icon, two lines about 16). The bar gets taller by the
  text (2 DIP + one or two lines). With reflections the title sits under the reflection, which is
  unchanged. On left / right docks (`titlesVertical`: `"beside"` (default) or `"off"`) the title is
  a 76 DIP column beside the icon on the screen-edge side (right-aligned toward the icon on a left
  dock), which widens the bar. **Magnification**: titles do not scale; each title is a separate
  layer that follows its icon along the bar with the same animation as the dot, so it stays under
  its icon, is never redrawn while the dock magnifies, and the magnified icon grows away from it.
- **Live titles**: when a window's caption changes (the shell's `HSHELL_REDRAW`, coalesced with
  the window-list updates, no polling), its window slot's title, the hover label and the preview
  popup's card follow at once; the preview popup does that for stacked apps too.
- **Window thumbnails** (`thumbnails`: `"off"` (default), `"live"`, `"snapshots"`): each window
  slot (on a stacked dock: each app slot with windows, showing the app's active window) becomes a
  tile with the window's picture and the app icon in its bottom-right corner. A tile is
  `thumbnailSize` DIPs long along the dock (default 0: twice `iconSize`) and an icon deep, so the
  bar keeps its height; on left / right docks tiles are an icon wide and `thumbnailSize` tall. Tiles
  magnify like icons (the picture is kept at the magnified size, so it stays sharp); label, click
  and preview work as for the slot. Pins and folders stay icons; tiles get no glow or reflection.
  A minimized window keeps its last picture, dimmed (minimize to dock).
  - **Live (GPU capture)**: Windows.Graphics.Capture, one session per window, no capture border, no
    cursor. Windows sends a frame only when the window's content changed, and at most
    `thumbnailFps` (default 10, 1 - 60) a second; each frame is scaled into the tile on the GPU.
    Capture runs only while the dock is visible: hidden by auto-hide (or a desktop dock while
    windows are in front) every session is closed. Measured here: one window animating at 60 fps
    costs ~0.3 - 1.4 % of one core and ~0.1 % of the GPU's 3D engine at 10 fps; still windows cost
    nothing; hidden: 0. Each session holds one frame buffer the size of the window in GPU memory.
    Turning live on switches the dock's Direct3D device to multithreaded (capture requires it; the
    dock rebuilds its surfaces once).
  - **Snapshots (every N seconds)**: every `thumbnailIntervalS` seconds (default 5, 1 - 3600) one
    worker thread takes `PrintWindow` (`PW_RENDERFULLCONTENT`) of each window that is not minimized
    (or hung), scaled down; nothing runs in between and nothing at all while the dock is hidden. A
    new window gets its first picture when its tile appears. Measured: 8 ms (640 x 400) to 14 ms
    (1920 x 1080) per window on the worker, a few ms of CPU.
  - Neither shows DRM-protected video (it captures black). `"off"` changes nothing: no session, no
    timer, no thread.

### Weather and calendar

**Weather widget** (a `weather` item, off by default; Settings > Weather): where its item stands
in the dock's list (by default left of the clock, after the separator), the current conditions as
a small glyph (sun or moon, partly cloudy, overcast, fog, drizzle, rain, snow, thunder; drawn as
vector shapes, so they stay sharp at any DPI) and the temperature in the clock's font. At an end
of the dock it moves with that end of the bar while the bar grows under the mouse.
Without a location it shows a map-pin glyph, and a click on it opens Settings on the Weather page.

- A click on the widget opens the **forecast popup** above it (click again, Escape, or clicking
  anywhere else closes it; F5 refreshes): the place, a large glyph and temperature, the condition,
  humidity and wind, when the data was fetched, the **Weather data by Open-Meteo** credit (a link)
  and a refresh button; then 24 hourly columns starting at the current hour (hour, glyph,
  temperature, the chance of precipitation when it is above 0 %, and a thin line through the
  temperatures) and 7 daily rows (weekday, glyph, low and high, and a bar showing the day's range
  on the week's scale, coloured from cool to warm). While the popup is open the dock does not
  auto-hide.
- Data: [Open-Meteo](https://open-meteo.com) (free, no API key; weather data by Open-Meteo.com,
  licensed CC BY 4.0). One HTTPS request per refresh (`refreshMinutes`, default 30) to
  `api.open-meteo.com` with the configured latitude / longitude and units, `timezone=auto` (hours
  and days are the location's local time). The request runs on a short-lived worker thread
  (WinHTTP, the system's automatic proxy settings, 5 s timeouts, TLS only, at most 1 MB); between
  refreshes no thread exists and nothing polls: one one-shot timer waits for the next refresh. After
  a failure (offline, timeout, an HTTP error) the dock retries after 1, 2, 4 ... minutes, never
  later than the refresh interval, and keeps showing the last data, dimmed. The widget and the
  popup are redrawn only when the data, the theme or the DPI changes.
- The last good response is cached in `%LocalAppData%\WinDock\weather-cache.json` (with the time
  it was fetched, the coordinates and the units). At start the dock shows it at once, dimmed as
  stale, when it is for the same place and units, and fetches fresh data straight away.
- **Settings > Weather** finds places by name (Open-Meteo's geocoding service, 10 results, as
  "Name, Region, Country"); picking one fills in the latitude, longitude and the name shown.
- Right-click the dock (the widget included) for **Refresh weather** and **Weather settings…**.

**Calendar**: a click on the clock opens a month calendar above it: today's date in the locale's
long format, the month and year with previous / next arrows (also Left / Right and Page Up / Page
Down; a click on the month name or Home goes back to the current month), the weekday names from
the locale starting on its first day of the week, and six weeks of days with today on an
accent-coloured disc. No events. Escape or clicking anywhere else closes it.

What a click on the clock opens is `clock.click` (General page: "Clicking the clock opens"):
`"calendar"` (the default), `"launch"` (the app, file or URL in `clock.clickTarget`, for example
Outlook or `https://calendar.google.com`) or `"none"`. With a `clickTarget` set, the calendar also
gets an **Open …** button at the bottom ("Open" and the file name, or the URL's host), so the popup and
the app are both a click away (`WinDock.DockCommand` 114 still opens the calendar in "launch"
mode).

### Widgets

A **widget** is a small JavaScript program that draws into a slot of the dock: a folder you can
write and edit without compiling anything. The full reference (every call, every manifest key) is
[docs/widgets.md](docs/widgets.md); the short version:

- **Where**: `%LocalAppData%\WinDock\widgets\<id>\` with `widget.json` (the manifest), `main.js`
  (an ES module), more modules and an `assets` folder if it needs them. Built in (their sources
  are in this repository's `widgets\` folder): **Hello** (`hello`: a label and a click counter),
  **Digital clock (script)** (`digital-clock`: a scripted copy of the dock's own clock, with the
  same format, seconds and date options as its settings), **Analog clock** (`analog-clock`: five
  faces as data files, `faces/*.json`: Classic, Minimal, Modern dark, Roman, Station; an optional
  second hand that ticks or sweeps smoothly, the date on the face, the time under the dial, a
  sweep-in animation) and **Calendar page** (`analog-date`: today's date as a flip-calendar or flat
  page with the month or weekday in a coloured header; a click opens the month calendar; it wakes
  once a day, at midnight), **Volume** (`volume`: the output level with a percentage; the wheel
  changes it in 2 % steps, a middle click mutes, a click opens a popup with a slider, Mute, Mute
  microphone, the output devices and Sound settings), **Now playing** (`media`: the current media
  session's art, title and artist (Spotify, browsers, Media Player ...); hover the art for previous /
  play-pause / next, click the text for a popup; optionally the wheel changes the volume), **Start**
  (`start`: the Windows logo or your icon, opens Start), **Search** (`search`: opens Windows Search)
  and **Notifications** (`notification-bell`: a bell that opens Windows' notification centre (Win+N)
  or, with its "A click opens" setting, WinDock's own centre; with its "Show the unread count"
  setting on (off by default: nothing polls) the badge is the number of unread notifications). A folder with a built-in's id replaces the built-in; your own clock faces
  go into an exported `analog-clock\faces` folder.
- **Adding one**: Settings > Dock contents > **Add widget ▾** lists the installed widgets after
  Clock / Weather / Running apps, or add `{ "kind": "widget", "id": "hello" }` to a dock's `items`
  (with an optional `"settings": { ... }` object). A widget can be dragged on the dock like the
  clock; a right click opens the dock menu, which then has **Reload widgets**.
- **widget.json**: `name`, `version`, `author`, `description`, `size` (the box per axis in DIPs:
  `{ "horizontal": { "main": 64, "cross": 40 }, "vertical": { "main": 44, "cross": 48 } }`),
  `refreshMs` (call `tick()` every so often, aligned to the clock), `timeLimitMs` (watchdog, default
  50 ms), `memoryLimitMB` (default 32), `permissions`, `watch`, and `settings` (a schema: bool,
  int, number, string, choice (also `choiceFrom`: one choice per package file matching a glob),
  colour, font, with defaults; the values live in the item's `settings` object).
- **main.js** exports `draw(canvas, dock)` (required) and optionally `measure(canvas, dock)`
  (returns `{ main, cross }`), `tick(dock)`, `onPress(e)`, `onClick(e)`, `onMiddleClick(e)`,
  `onWheel(e)` (return `true` to use the wheel), `onHover(e)`, `onDrop(e)` (files dropped on it;
  needs the shell permission), `onSettingsChanged(settings)`, and for its popup `drawPopup(canvas,
  dock)`, `onPopupClick(e)`, `onPopupClose()`, and `onVisibilityChange(e)` (the dock was hidden /
  shown). Nothing is drawn per frame: the dock calls `draw()` when the widget asks for it
  (`requestRedraw()`), when a tick redraws it, and when the dock lays it out again; an animation
  asks for frames with a timer only while it runs and only while the dock is visible (docs:
  "Animation").
- **API**: `canvas` (rectangles, rounded rectangles, ellipses, lines, paths with arcs and Béziers,
  solid colours and linear / radial gradients, DirectWrite text and `measureText`, images from the
  widget's folder, clip, opacity, transforms, save / restore; DIPs, upright on every edge, drawn
  with Direct2D straight into the slot), `dock` (axis, edge, box size, scale, icon size, theme
  colours, hovered / magnified, reduce motion, visible), `timer` (`setTimeout`, `setInterval`, `clear`,
  `setRefresh`, `requestRedraw`), `console` (log / warn / error to the widget's log), `system`
  (local date and time computed by Windows, strftime formatting, locale names, 12 / 24 h, units),
  `settings`, `storage` (a JSON store per widget in `%LocalAppData%\WinDock\widget-data\<id>.json`,
  written a second after a change), `http` (GET / POST ... to the hosts its manifest lists, as
  promises), `shell` (open files and URLs, list folders, file icons, `sendKeys` for a fixed list of
  Windows shortcuts: Start, Search, the notification centre ...), `audio` (the default output's
  level / mute, the microphone's mute, the outputs, switching the default output, change events),
  `media` (the current media session: title, artist, album, state, art, play / pause / next /
  previous, change events), `popup` (a popup the widget
  draws, or the dock's folder / window-preview / month calendar popups, anchored on the widget),
  `widget` (the widget's own package files: `files(glob)`, `read`, `readJson`).
- **Permissions** (`"permissions"` in widget.json): `{ "http": ["api.example.com"] }` for
  requests to those hosts, `"shell"` for opening files / URLs, listing folders, file icons, the
  folder popup, file drops and `sendKeys`, `"audio"` and `"media"` for the providers. Without them
  those calls throw. The audio and media providers run only while a widget uses them and never
  poll (Core Audio and media-session events).
- **Settings**: Settings > **Widget settings** is generated from each widget item's schema (pick
  the item at the top, or press Settings… on its Dock contents row); changes apply at once.
  Settings > **Widgets** lists the installed widgets (built-in / folder) with **Add to dock**,
  **Add from folder…**, **Export built-in to folder** (to edit a copy), **Reload widgets**, **Open
  widgets folder** and **Console…**.
- **Developing**: `"watch": true` reloads a folder widget when one of its files changes (no
  polling); the **Console** (Settings > Widgets, the dock menu's Widget console…) shows every
  widget's state and log with Reload / Open folder / Clear.
- **Safety**: every widget has its own script engine (QuickJS-ng) with its own heap limit; a call
  that runs longer than its time limit is stopped and the widget suspended ("Stopped" tile), an
  uncaught exception shows an "Error" tile with the message; **Reload widgets** (dock menu) starts
  them over. Imports and images are read only from the widget's own folder. Widgets run on the
  dock's thread and cost nothing while they do not redraw (each widget's runtime is about 130 KB
  of script heap, about 400 KB of private memory with its surface).

### System tray (notify icons)

Off by default. **Settings > Tray > Show the notification area icons in the dock** (or the dock
menu's **Add widget > Tray**, which also turns it on) makes the dock take over the notification
area: it owns a hidden window of class `Shell_TrayWnd` above Explorer's, so every app's
`Shell_NotifyIcon` (and `Shell_NotifyIconGetRect`, `SHAppBarMessage`) reaches the dock first; a
`TaskbarCreated` broadcast makes the running apps register their icons again. Every message is
passed on to Explorer too, so Explorer keeps every icon: quitting, a crash or turning the tray off
loses nothing (the dock destroys its window and broadcasts `TaskbarCreated` again; after a crash
a marker file `tray-hosted` makes the next start ask the apps once more). App bar messages and the
taskbar's private messages go on to Explorer and are answered by it. Explorer's own items
(network, battery, input language, its clock and volume) are not notify icons and stay with
Explorer, whose taskbar the dock hides anyway; the volume, media and date widgets cover them.

- **The `tray` item** (`{ "kind": "tray", "rows": 1, "columns": 8, "iconSize": 20 }`, one per
  dock; Add widget > Tray, Settings > Dock contents > Add widget): the icons as a grid in one
  widget slot (it does not magnify, like the clock), in registration order (`order`: icon keys
  shown first; 8b lets the grid be reordered by dragging). Columns run along the dock and rows
  across it: on a left / right dock the grid is `rows` wide and `columns` tall. Cells are the icon
  plus 3 DIPs on each side; two rows of large icons shrink to fit the icon row. Icons an app hid
  (`NIS_HIDDEN`), without a picture or without a callback message (they cannot be clicked) are
  not shown; neither are the dock's own tray icon and the keys in `hidden`.
- **Overflow and the drawer**: with more icons than cells, or icons hidden from the grid, the last
  cell is a chevron; a click opens the drawer (the folder / group popup in tray mode, titled
  "Notification area") with every icon, scrolling past eight rows. Its cells behave like the
  grid's.
- **Arranging** (8b): drag an icon within the grid to move it there (the grid's order is saved as
  the item's `order`; a hidden icon dragged from the drawer into the grid is shown again); drag
  it out of the grid or the drawer onto the dock's icon row to pin it there as a **`trayApp`**
  item at the insertion marker. **Ctrl + right click** on an icon (plain right click is its app's)
  opens the tray menu: Hide from / Show in the tray grid, Pin to the dock / Unpin from the dock,
  Tray settings…. Settings > Tray lists every icon with a Show check box too.
- **Pinned tray icons** (`{ "kind": "trayApp", "id": "<icon key>" }`): one icon as a full-size dock
  icon that magnifies like an app (its picture drawn at the icon size: the icon's own resource at
  that size when Windows still knows it, else the app's handle scaled). Its tooltip is the label;
  clicks go to the app like the grid's (no hover popups for version 4 icons here); Ctrl + right
  click > Unpin from the dock puts it back into the grid. While it is pinned the grid leaves the
  icon out (the drawer still lists it). When its app is not running the slot keeps its place
  with the placeholder icon and comes back to life when the app registers the same key again.
  It is not dragged on the dock (Settings > Dock contents moves it).
- **A dock of its own** (8b): Settings > Tray > **Put the tray in its own dock** makes a new dock
  (id `tray`) at the bottom right of the same display holding only this dock's tray item (with its
  rows, columns, hidden icons and order), with the same theme, always shown and reserving no
  screen space; the Docks page moves or removes it (`docks[]` like any other).
- **Input** goes to the icon's app exactly as Explorer delivers it (`uVersion` 3 / 4 semantics:
  version 4 gets the anchor point in `wParam` and `MAKELPARAM(event, uID)`): a left click sends
  button down / up and `NIN_SELECT`, a double click `WM_LBUTTONDBLCLK`, a right click button down
  / up and `WM_CONTEXTMENU` (the app's menu opens at the pointer), a middle click
  `WM_MBUTTONDOWN` / `UP`, moving over an icon `WM_MOUSEMOVE` (at most every 100 ms); the app may
  take the foreground (`AllowSetForegroundWindow`). **Ctrl + right click** on an icon opens the
  tray menu (below) instead; on the chevron or beside the cells a right click is the dock's menu.
  The hover label shows the icon's tooltip; a version 4 icon without `NIF_SHOWTIP` draws
  its own popup instead: it gets `NIN_POPUPOPEN` when the pointer rests on it (the label delay)
  and `NIN_POPUPCLOSE` when it leaves.
- **Where the icon is**: `Shell_NotifyIconGetRect` (apps place their windows by it) answers the
  icon's cell, the drawer's cell while the drawer shows it, or the chevron when it is only in the
  drawer.
- **Balloons** (`NIF_INFO`) are captured (the last 32). Explorer, which gets every message too,
  shows them as Windows toasts, unless `notifications.balloons` is on (see Notifications and
  badges): then the copy Explorer gets has `NIF_INFO` cleared and the balloon goes to the dock's
  notification list only.
- **Icon owners that quit** without removing their icons: each owner process is waited on (a
  thread-pool wait, no polling) and its icons go when it exits; a click that cannot be delivered
  removes the icon as well.
- Cost: nothing runs while the icons do not change. An app that re-sends the same icon (Logitech
  G HUB does every second) wakes the dock briefly (the picture is compared, nothing redraws).
- Limits: an elevated app's icon shows, but a dock that is not elevated cannot click it (Windows
  blocks the message; Explorer's tray has the same restriction for elevated apps, which allow
  it explicitly). Explorer may raise its hidden taskbar above the dock's window (seen once right
  after it is hidden): the dock raises its own again and repeats the broadcast 1.5 s later (at most
  three times a minute), so for a moment some apps may talk to Explorer only.

### Notifications and badges

Off by default (round 4 phase 9). The dock reads Windows' notification centre through
`UserNotificationListener`: every toast with its app, title, text and time. Windows allows an
unpackaged app to read it without a prompt as long as Settings > Privacy & security >
Notifications ("Let apps access your notifications") is on; the access is checked on every poll.

- **The notification centre** (`{ "kind": "notifications" }`, one per dock; dock menu Add widget >
  Notification centre, Settings > Notifications or Dock contents > Add widget): a bell with the
  unread count in a bubble (theme `badgeFill` / `badgeText` / `badgeSize`; dimmed while access is
  denied). A click opens the centre above the bell (beside a vertical dock): one row per
  notification, newest first, with the app's icon, the title, two lines of text and the relative
  time ("now", "5 min", "3 h", "yesterday", then the date), scrolling past six rows. A click on a
  row brings its app to the front from the dock (its windows, or it starts: shell:AppsFolder for
  an AUMID, the exe for a path; Windows does not let another app activate the toast itself) and
  dismisses the notification; the hovered row's cross (or Delete on the selected row) dismisses
  it; **Clear all** dismisses every listed one. Up / Down / Home / End select, Enter opens,
  Escape or another window coming forward closes it. With nothing listed it says "No
  notifications"; without access it shows the reason and **Open Windows settings**
  (ms-settings:privacy-notifications). New notifications appear while it is open (it polls every
  second meanwhile). Older builds skip the item. There is no Do not disturb switch: Windows has no
  public API to read or set it (the quick settings have one). The Notifications widget is the
  scriptable alternative (`notifications.openCentre()` opens this same list over the widget).
- **Badges** (per dock `badges`, default off; DockCommand 132 toggles it): a bubble with the
  number of unread notifications of each app on its slot, at the icon's outer corner (top right
  on bottom and left docks, bottom right on top docks, top left on right docks). It is part of the
  icon: it magnifies and bounces with it. "99+" above 99. A shortcut group shows the sum of its
  members'; on an unstacked dock the badge sits on the app's pin slot (an app without one: its
  first window slot). Theme keys `badgeFill`, `badgeText`, `badgeSize` (see Themes).
- **Which app**: a toast's AppUserModelId is matched against the dock's app keys
  (case-insensitive): `PackageFamilyName!App` (packaged apps) as that AUMID; an explicit id such as
  `Chrome` as that id (it matches windows and pins with the same AppUserModelId: Chrome's windows
  and an `"aumid": "Chrome"` pin); `{KnownFolderGUID}\path\app.exe` (or with `/`) as the exe's
  full path (the folder resolved with `SHGetKnownFolderPath`), which matches a pinned / running
  exe; a full path as that path; `Microsoft.Windows.Explorer` as `explorer.exe` (the dock's alias
  table). Toasts whose app cannot be read (Windows throws for some) count for no slot.
- **Unread / read**: a notification counts until it is dismissed in Windows (the next poll sees it
  gone) or its app is activated from the dock (`notifications.clearOnActivate`, default on: a
  click on the slot marks that app's notifications read; nothing is removed from Windows).
- **Polling**: Windows offers no change event to an unpackaged app (`NotificationChanged` throws
  without package identity), so while the feature is on a worker thread asks for the list every
  `notifications.pollS` seconds (default 2), every max(pollS, 5) s once nothing changed for a
  minute, every second while the notification centre popup is open, and goes back to pollS on any
  change. Only the ids are compared; a new toast's details are read once. The worker exists only
  while something needs it: a dock with `badges`, a widget that uses the `notifications` provider,
  or the open centre; otherwise nothing polls. Cost measured here with 46 notifications in the
  centre: about 25 ms of CPU per poll, almost all of it inside Windows' `GetNotificationsAsync`
  (1.3 % of one core at the 2 s cadence, 0.5 % at 5 s; it grows with the number of notifications).
- **Access denied**: the service stays idle apart from checking the access at the same cadence;
  QueryState 113 reports 2.
- **Tray balloons**: with `notifications.balloons` (and the tray takeover running) a tray icon's
  balloon is a notification of the dock's list (attributed to the icon's exe: it badges that app)
  and Explorer no longer shows it as a toast. Off (default), Explorer shows it as a toast, which the
  dock reads like any other; Explorer files those toasts under an AUMID of its own
  (`NotifyIconGeneratedAumid_<number>`), so they badge no dock slot.
- **Overlay icons and progress** (`ITaskbarList3`) do not reach the dock: they are not window
  messages to `Shell_TrayWnd` (checked with the tray takeover's window above Explorer's, nothing
  arrived). While the takeover runs apps' `ITaskbarList3::HrInit` fails with `E_NOTIMPL` and
  `SetOverlayIcon` with `E_FAIL` (the taskbar they would draw on is hidden anyway).
- **Widgets**: the `notifications` provider (permission `"notifications"`, docs/widgets.md) and
  the Notifications widget's unread count.
- **Settings > Notifications**: Show notification badges on this dock's apps, Show the notification
  centre on this dock (adds / removes the item), Show tray balloons in the centre instead of
  Windows toasts, Clear an app's badge when it is opened from the dock, the poll interval
  (seconds), the access status with **Open Windows settings**, and **Badges per app**: this dock's
  apps and every app with a notification now, with a check box each (unchecked: in
  `notifications.excluded`, no badge on any dock).

## Settings

**Settings…** in the tray / dock menu, `WinDock.exe --settings` (asks the running dock to open it,
or starts the dock and opens it) and `WinDock.DockCommand` 107 open the settings window. There is
only ever one; opening it again brings it to the front. It appears centred on the dock's monitor,
above the dock, and is a normal window (Alt+Tab, minimize; the dock stays the topmost window).

Pages (left column; Up / Down / Home / End switch pages while it has the focus, Tab moves into
the page, Esc closes the window, Enter applies the text in an edit box):

- **General**: icon size (slider 16-128, the box goes on to 256), magnification (1.00-2.50),
  magnification range (1-4 icons), **Position**: screen edge, alignment and offset (pixels at
  100 % scale; applied on Enter or focus loss), Full width, Reserve screen space (enabled with
  full width), Peek when hidden (pixels), Peek opacity (%) and Title (see Position), display (Primary, then `Display 2 (\\.\DISPLAYn)` ... in the
  dock's order), clock on / off, time format (pick one or type a `strftime` format; a preview shows
  the result), **Show seconds**, date line, **Clicking the clock opens** (the month calendar, the
  app or URL below, nothing), **App or URL** (typed, applied on Enter or when the box loses the
  focus, or **Browse…** for an app; a path with spaces is quoted), Start with Windows, Hide the
  Windows taskbar, and the renderer (`auto` / `software`; takes effect after WinDock restarts, the
  page shows the one in use).
- **Appearance**: the theme (the built-in presets, then your theme files from the themes folder by
  name; a `preset` that is not found is listed as "<id> (missing)"), **Export current theme…**,
  **Open themes folder**, **Reset all to preset**, and one row per theme field (see
  [Themes](#themes)) grouped as Bar, Name label, Indicator dots, Clock, Placeholder icon, Window
  previews and Icon effects. A colour row has a swatch (click it for the Windows colour picker; it
  replaces the colour and keeps the alpha), the `#AARRGGBB` value (applied on Enter or when the
  box loses the focus; an invalid value beeps and goes back) and the alpha 0-255. On / off keys
  are check boxes, numbers are number boxes with their unit (DIPs; percent for the 0 - 1 values
  such as the glow strength; degrees for the perspective angle), and the glow mode is a drop-down
  (Off, While hovered, Active app, Always). **Icon effects** holds the glow (mode, colour, radius,
  strength), the reflection (on / off, **Clip the reflection to the bar** right under it, height, opacity)
  and the 3D perspective (on / off, angle);
  see [Icon effects](#icon-effects). The preview strip under every page shows them on its
  placeholder icons (the perspective as the shelf's trapezoid with the icons a little shorter).
- **Behaviour**: auto-hide Off / Always / When a window overlaps the dock, hide and reveal delays,
  window previews on / off, **Show window previews for apps with a single window**, the preview
  delay, and **Mouse**: what a middle click on an app does (start a new instance, minimize its
  windows, close its windows, nothing) and what the mouse wheel over an app does (nothing, switch
  between its windows), and **Running apps**: **Windows** (Stacked / One slot per window),
  **Titles under icons** (Off / One line / Two lines), **Titles on vertical docks** (Off / Beside
  the icon), **Window thumbnails** (Off / Live (GPU capture) / Snapshots (every N seconds), with the
  trade-off explained under it), the snapshot interval, the live frames per second and the tile
  length; see [Running apps: windows and titles](#running-apps-windows-and-titles).
- **Animation**: speed (Off, then 0.25× to 4× on a logarithmic slider), hide style (slide and
  fade, slide, fade, none), launch feedback (bounce until the app opens, hop once, pulse, none),
  magnification falloff (smooth / linear), easing (ease out, linear, spring), the name label fade
  (0-1000 ms) and reduce motion (follow Windows, on, off; the page shows whether Windows'
  animation effects are on). See [Animation](#animation).
- **Dock contents** (was "Pinned apps"): every item of the dock in its order, one row each: the
  name (with the icon for apps, folders and the Recycle Bin; "(missing)" when the file is gone),
  the kind (App, Folder, Group, Separator, Recycle Bin, Running apps, Clock, Weather) and details (the
  target; the clock's format; the weather's place; a second running apps item reads "Not shown:
  only the first running apps item is used"; a kind this version does not know is listed as
  written). Buttons: **Add app…** (pick one or more `.lnk` / `.exe` files; a shortcut is pinned as
  the shortcut, apps that are already pinned are skipped), **Add folder…** (a folder picker),
  **Add separator** (after the last pin), **Add widget ▾** (a menu: Clock, Weather, Running apps,
  placed as the dock menu's Add widget does; Running apps is greyed while the dock has one),
  **Move up** / **Move down** (any item past any other, the running apps block included),
  **Remove** (any item; without a running apps item, running apps that are not pinned are not
  shown, add it back with Add widget > Running apps), **Settings…** for the selected item (the
  clock: the General page; the weather: the Weather page; the running apps: the Behaviour page;
  a folder (or a shortcut to one) or a group: the Folders and groups page; an app or the Recycle Bin:
  Choose icon…; greyed for a separator), **Choose icon…**,
  **Choose image…**, **Reset icon** (as in the dock's menu, see Custom icons; for apps, folders
  and the Recycle Bin, Reset only when the entry has an `icon`), **Add Recycle Bin** (greyed
  while the bin is on the dock) and **Lock the dock**. Every change applies to the dock at once;
  changes from the dock itself (drags, drops, the menus) show up in the list at once.
- **About**: version, the config file path (click it to show the file in Explorer), **Open config
  file**, **Reload config**, and the renderer in use.
- **Weather** (after About): show the weather on the dock; **Find a place** (type a name, then
  Enter or **Search**; the results list shows "Name, Region, Country" and choosing one sets the
  place), the name shown, latitude and longitude (typed values apply on Enter or when the box loses
  the focus; an invalid value beeps and goes back), units (metric: °C and km/h; imperial: °F and
  mph), the refresh interval (5-360 minutes), the status ("Updated 10:15", "(stale)", "Updating…",
  "Not configured", "Error: …"), **Refresh now** and the Open-Meteo credit.
- **Widgets**: the installed script widgets (name, id, built-in / folder, version) with **Add to
  dock**, **Add from folder…**, **Export built-in to folder**, **Reload widgets**, **Open widgets
  folder** and **Console…** (see **Widgets** above).
- **Widget settings**: the settings of one widget item of the dock, generated from its widget.json
  schema (a drop-down at the top picks the item; **Settings…** on a widget row of Dock contents
  opens it), with **Reset to defaults**.
- **Notifications** (round 4 phase 9): badges and the notification centre on the dock it edits, tray
  balloons in the centre, clear on activate, the poll interval, Windows' access status, badges per
  app (see Notifications and badges).
- **Folders and groups**: one folder or shortcut group of the dock (a drop-down picks it;
  **Settings…** on its row of Dock contents opens it): **Show the contents as** Automatic (a grid
  up to 48 entries, a list above) / Grid / List, and the icon buttons (Choose icon…, Choose
  image…, Reset icon); for a group also its **Name** and its shortcuts (Add app…, Add folder…,
  Remove, Move up / down).

Every change applies to the dock at once (sliders when you release them, number boxes on each
arrow step, on Enter or when the box loses the focus) and is saved within 300 ms, exactly like the
menu toggles. The tray menu and **Reload config** update an open settings window. While the
settings window is the active window the dock stays shown, so you can watch your changes; auto-hide
resumes as soon as another window is activated.

Under every page a **preview strip** draws a small dock from the current settings: the bar with
its border, acrylic tint (over a stand-in wallpaper) or background, corner radius, six placeholder
apps with the three indicator dot states (running, active, attention), the separator and the clock,
a sun standing in for the weather widget (when it is on), the name label above the magnified app,
and a sample window-preview popup with a hovered card and
its hovered close button. Move the mouse over it and the magnification follows the pointer, with
the configured magnification, range and falloff. It repaints only when a setting changes or the
mouse moves over it (no timers).

**Theme overrides.** Choosing a theme sets `theme.preset` and clears every override. A theme field
you change on the Appearance page becomes an override: it is saved inline in config.json next to
`"preset"` (only overridden fields are saved) and its **Reset** button appears; Reset copies the
preset's value back and removes the field from config.json. **Reset all to preset** does that for
every field. **Export current theme…** saves all 33 resolved values plus a `name` (the file name)
as a theme file, by default into the themes folder (created on demand); a file saved there becomes
the preset at once, with no overrides left. A theme file named after a built-in id (`dark.json`,
...) is ignored, so for a built-in preset the suggested name is `my-<id>`.

The window follows the Windows app mode (Settings > Personalization > Colors > Choose your app
mode) and switches live: dark or light title bar, Mica behind the title bar and the page list
(when transparency effects are on; otherwise solid colours), dark buttons, boxes, lists, the Dock
contents list and its header, and scroll bars; sliders have no dark style in Windows, so the window
paints their track and thumb itself in dark mode. The colour picker (`ChooseColor`) and the file
dialogs are system dialogs; the colour picker is always light. Dark context menus and drop-down
lists use undocumented Windows functions when they are available; without them those popups are
light. High contrast uses the system colours. Closing the window frees everything it used (the
preview's Direct2D objects included); nothing of it runs while it is closed.

## Configuration

`%LocalAppData%\WinDock\config.json` is created on first run. It starts with File Explorer
pinned, plus Windows Terminal when it is installed, then the running apps and the clock. After
editing it by hand, choose **Reload config** in the dock menu. That applies the dock items, sizes,
magnification, theme, clock, monitor, auto-hide, `hideTaskbar` and `startWithWindows`. `renderer`
needs a restart of the dock. If the file is malformed, Reload keeps the current settings (and
beeps). Pinning, unpinning, and the menu toggles save the file within 300 ms. The write is atomic
(a temp file replaced with `MoveFileEx`). The dock rewrites the whole file in its own layout.
Top-level keys it does not know are kept, as compact JSON. Unknown keys inside a dock, an item,
`clock`, `theme`, `animation` or `weather` are dropped. If the file is malformed, the dock starts
with the defaults and leaves the file untouched.

```json
{
  "docks": [
    {
      "id": "main",
      "items": [
        { "kind": "app", "path": "C:\\Windows\\explorer.exe" },
        { "kind": "app", "aumid": "Microsoft.WindowsTerminal_8wekyb3d8bbwe!App" },
        { "kind": "app", "path": "C:\\Users\\me\\Desktop\\Paint.lnk" },
        { "kind": "running" },
        { "kind": "weather" },
        { "kind": "clock" }
      ],
      "edge": "bottom", "align": "center", "offset": 0, "locked": false,
      "fullWidth": false, "reserveSpace": true, "peekPx": 0, "peekOpacity": 0.5, "title": "",
      "iconSize": 48, "magnification": 1.5, "magnifyRange": 2,
      "autoHide": "always", "hideDelayMs": 400, "revealDelayMs": 150,
      "previews": true, "previewsSingleWindow": false, "previewDelayMs": 600,
      "middleClick": "newInstance", "wheel": "none",
      "monitor": 0, "runningFilter": "all", "themePreset": "", "layer": "normal",
      "windows": "stacked", "titles": "off", "titlesVertical": "beside",
      "thumbnails": "off", "thumbnailIntervalS": 5, "thumbnailSize": 0, "thumbnailFps": 10
    }
  ],
  "hideTaskbar": true,
  "startWithWindows": false,
  "renderer": "auto",
  "clock": { "format": "%H:%M", "showSeconds": false, "showDate": false,
             "click": "calendar", "clickTarget": "" },
  "theme": { "preset": "dark",
             "accent": "#FF4CC2FF" },
  "animation": { "speed": 1, "hideStyle": "slide", "launchStyle": "bounce",
                 "magnifyFalloff": "cosine", "easing": "easeOut", "labelFadeMs": 120,
                 "reduceMotion": "system" },
  "weather": { "latitude": 47.6062, "longitude": -122.3321,
               "location": "Seattle", "units": "metric", "refreshMinutes": 30 }
}
```

**Older files** (with a top-level `pinned` list and the dock settings at the top level) are
migrated once when they are loaded, and the file is rewritten in the format above right away:
the `pinned` entries become `items` in the same order (`separator` / `recycleBin` entries keep
their kind, a path that is a folder becomes `"folder"`, everything else `"app"`), followed by
`running`, then `weather` if `weather.show` was `true`, then `clock` if `clock.show` was `true`
(its default). `iconSize`, `magnification`, `magnifyRange`, `autoHide`, `hideDelayMs`,
`revealDelayMs`, `previews`, `previewDelayMs` and `monitor` move into `docks[0]`. A file that has
`docks` is never migrated: legacy keys in it are ignored and dropped on the next save.

- `docks`: the docks, every one shown (see Multiple docks and monitors); a file without any gets
  one with the defaults. Per dock:
  - `id` (default `"main"`; unique: a repeated id gets a number appended on load).
  - `monitor` (default `0` = primary; an index, `"primary"` or `"each"`), `runningFilter`
    (`"all"` / `"monitor"`), `themePreset` (default `""`), `layer` (`"normal"` / `"desktop"`):
    see Multiple docks and monitors.
  - `items`: everything on the dock, in order. Each item has a `kind`:
    - `"app"`: `{ "kind": "app", "path": "<.lnk or .exe>" }` or
      `{ "kind": "app", "aumid": "<AppUserModelID>" }`. Environment variables in paths are
      expanded. A shortcut to a folder works too (it is shown as a folder).
    - `"folder"`: `{ "kind": "folder", "path": "<folder>", "view": "auto" }`, a folder stack (see
      Dock items). `view` is `"auto"` (default; not written), `"grid"` or `"list"`.
    - `"separator"` (any number) and `"recycleBin"`: need no `path`.
    - `"group"`: `{ "kind": "group", "name": "<name>", "icon": "<optional>", "view": "auto",
      "items": [ { "kind": "app", "path": "..." }, { "kind": "folder", "path": "..." } ] }`, a
      shortcut group (see Dock items). `items` holds app / folder entries only (`path` or
      `aumid`, optional `icon`); `name` "" shows as "Group".
    - `"running"`: where the running apps that are not pinned go (one icon each). **Without a
      `running` item, running apps that are not pinned are not shown**; pinned apps still show
      their windows (dot, click to switch, previews). Only the first `running` item is used.
    - `"clock"` / `"weather"`: the clock and the weather widget. The widget is shown when its item
      is present; the settings toggles add the item (at the end; the weather goes before the
      clock) or remove it.
    - `"widget"`: a script widget, `{ "kind": "widget", "id": "<widget id>", "settings": { ... } }`
      (see **Widgets** above and docs/widgets.md). An item without an `id` is dropped on load;
      `settings` is optional and written back as it is.
    - `"tray"`: the notify icons as a grid (see **System tray** above), `{ "kind": "tray",
      "rows": 1, "columns": 8, "iconSize": 20, "hidden": ["<icon key>", ...], "order": [...] }`;
      `rows` 1 - 2, `columns` 1 - 20, `iconSize` 16 / 20 / 24 DIPs (other values snap to the
      nearest), `hidden` icon keys never shown in the grid (still in the drawer), `order` icon
      keys shown first in that order. Each key is written only when it is not the default. An icon
      key is the icon's GUID in braces, lower case (`"{7820ae78-23e3-4229-82c1-e41cb67d5b9c}"`), or
      `"<exe file name>:<uID>"` lower case (`"steam.exe:1"`). Shows nothing while `tray.enabled`
      is off. Older builds skip the item.
    - `"trayApp"`: one notify icon pinned as a dock icon, `{ "kind": "trayApp", "id": "<icon
      key>" }` (see System tray); an item without an `id` is dropped on load. Older builds skip it.
    - `"notifications"`: the notification centre's bell, `{ "kind": "notifications" }` (see
      Notifications and badges). Older builds skip it.

    App, folder and Recycle Bin items take an optional `icon` (written back only when set): a
    custom icon. An image (`.png`, `.jpg`, `.jpeg`, `.bmp`, `.gif`; scaled to the icon size), an
    `.ico`, or an icon resource as `"file,index"`, for example
    `"%WINDIR%\\system32\\shell32.dll,3"` (a negative index is a resource id, as in shortcuts).
    Environment variables are expanded. If the file does not exist, the default icon is used and
    the item is kept unchanged. The value is kept exactly as written (a `%VAR%` path stays
    unexpanded in the file); **Choose icon…** / **Choose image…** store the absolute path the
    dialog returned, and **Reset icon** removes the key.

    Kinds are case-insensitive. An item without a `kind` but with a `path` / `aumid` is read as
    `"app"`; an app / folder item without either is ignored. A kind this version does not know is
    kept as written (and not shown). The app, folder, separator and Recycle Bin items are the
    pins.

    **The dock shows the items in list order**, left to right: `[clock, app, app, running]` puts
    the clock at the left end. Between an icon and a widget there is a 17 DIP gap, with a hairline
    when the widgets are at an end of the dock (before the widgets at the right end, after the
    ones at the left end; a widget between icons gets the gap but no line); between two widgets a
    10 DIP gap. Widgets never magnify; at an end of the dock they move with that end of the bar
    while icons magnify. Dragging an icon reorders the pins among the pins' places in the list
    (pin k always takes the place of the k-th pin item, so a pin can move past a widget) and the
    running apps among themselves; dragging the clock or the weather widget moves its own item
    (see Dock items; the running apps count as one block). Settings > Dock contents moves any
    item. A file dropped on the dock is pinned before a pin, after the last pin, or (no pins)
    before the running apps, whichever place is nearest.
  - `edge` (default `"bottom"`; `"top"`, `"left"`, `"right"`), `align` (default `"center"`;
    `"start"`, `"end"`) and `offset` (DIPs, default 0): where the dock sits, see
    [Position](#position-edge-alignment-offset). Unknown text reads as the default.
  - `fullWidth` (default `false`), `reserveSpace` (default `true`, used with `autoHide`
    `"off"` only, on any edge), `peekPx` (default 0), `peekOpacity` (default 0.5), `title`
    (default `""`): see Position.
  - `locked` (default `false`): no dragging, no pinning by dropping files, no Add separator / Add
    widget from the dock menu (see Dock items).
  - `windows` (default `"stacked"`; `"unstacked"`: one slot per window), `titles` (default
    `"off"`; `"one"`, `"two"`), `titlesVertical` (default `"beside"`; `"off"`): see
    [Running apps: windows and titles](#running-apps-windows-and-titles). Unknown text keeps the
    default.
  - `thumbnails` (default `"off"`; `"live"`, `"snapshots"`), `thumbnailIntervalS` (default 5,
    1 - 3600), `thumbnailSize` (DIPs along the dock, default 0 = twice `iconSize`, else 16 - 512),
    `thumbnailFps` (default 10, 1 - 60): window thumbnails as dock items, same section.
  - The dock settings described below: `iconSize`, `magnification`, `magnifyRange`, `autoHide`,
    `hideDelayMs`, `revealDelayMs`, `previews`, `previewsSingleWindow`, `previewDelayMs`,
    `middleClick`, `wheel`, `monitor`.

  ```json
  "items": [
    { "kind": "folder", "path": "C:\\Users\\me\\Downloads" },
    { "kind": "separator" },
    { "kind": "app", "path": "C:\\Windows\\explorer.exe", "icon": "%WINDIR%\\system32\\shell32.dll,3" },
    { "kind": "app", "aumid": "Microsoft.WindowsTerminal_8wekyb3d8bbwe!App", "icon": "C:\\Icons\\terminal.png" },
    { "kind": "recycleBin" },
    { "kind": "running" },
    { "kind": "clock" }
  ]
  ```
- `iconSize`: icon size at rest, in DIPs (16 - 256).
- `magnification`: size of the icon under the mouse relative to `iconSize`, 1.0 - 2.5. 1.0 turns
  magnification off. Icons are rendered at the magnified size, so they stay sharp.
- `magnifyRange`: how many neighbouring slots the magnification spreads over, 1 - 4.
- `autoHide`: `"off"`, `"always"` (default; hide when the mouse leaves the dock, see above) or
  `"intelligent"` (hide only while the foreground window overlaps the dock, see above). The older
  `true` / `false` are still read (as `"always"` / `"off"`) and written back as strings on the
  next save. `hideDelayMs` (default 400) and `revealDelayMs` (default 150)
  are the delays before hiding and revealing.
- `previews` (default `true`): the window-thumbnail popup for apps with two or more windows (see
  above); with `previewsSingleWindow` (default `false`) also for apps with one window.
  `previewDelayMs` (default 600, 0 - 10000) is how long the mouse must rest on the icon.
- `middleClick` (default `"newInstance"`): a middle click on an app: `"newInstance"`,
  `"minimize"`, `"close"` or `"none"` (see Dock items). `wheel` (default `"none"`): the mouse wheel
  over an app: `"none"` or `"cycle"`. Unknown values keep the default.

- `clock.format` uses `strftime` specifiers (`%H:%M`, `%I:%M %p`, `%H:%M:%S`, ...). Formats with
  seconds tick every second; otherwise the clock wakes once a minute. `clock.showSeconds`
  (default `false`) adds `:%S` right after the minutes (`%H:%M` shows as `%H:%M:%S`, `%I:%M %p` as
  `%I:%M:%S %p`; a format without minutes gets it at the end); a format that already shows seconds
  is used as it is. `clock.click` (default `"calendar"`): what a click on the clock opens,
  `"calendar"`, `"launch"` or `"none"`; files written before it had `clock.calendarPopup`, and
  without a `click` key `"calendarPopup": false` still reads as `"none"` (the key is not written
  any more). `clock.clickTarget` (default empty): what `"launch"` and the calendar's footer button
  open, through the shell (like Run): an app, a shortcut, a document or a URL; environment
  variables are expanded, and a quoted path may be followed by arguments
  (`"\"C:\\Program Files\\App\\app.exe\" --calendar"`). See Weather and calendar.
- `weather`: the weather widget (see Weather and calendar; shown by a `weather` item);
  `latitude` (-90 - 90) and `longitude` (-180 - 180), in degrees, saved with 4 decimals (both 0:
  no location yet); `location`, the name shown in the popup (it is not looked up); `units`,
  `"metric"` or `"imperial"` (when the key is missing: from the Windows region's measurement
  system); `refreshMinutes`, 5 - 360 (default 30). **Reload config** applies a changed place or
  units at once (the data is fetched again).
- `theme`: see Themes below.
- `theme.acrylic` (default `true`): blur what is behind the bar, like the macOS dock. The blur is
  a Windows 11 system backdrop (acrylic) on a second, bar-sized window (class `WinDockBackdrop`)
  kept directly beneath the dock window; it follows the bar while it grows under magnification
  and while it slides in and out, and it is hidden while the dock is hidden. With acrylic on, the
  bar is tinted with `theme.acrylicBackground` (default `#66202020`, lighter so the blur shows)
  instead of `theme.background`. Caveats:
  - Windows rounds the blur with its own corner radius, so with acrylic the bar's corners snap to
    the closest system radius not above `cornerRadius`: 8 DIPs for `cornerRadius` >= 8, 4 DIPs
    for 4 - 7, square below 4. `cornerRadius` applies as configured only without acrylic.
  - Windows draws its usual soft window shadow around the blurred bar.
  - Windows renders no backdrops when **Settings > Personalization > Colors > Transparency
    effects** is off or a high-contrast theme is on. The dock then falls back to the plain
    `theme.background` bar, and switches back when the setting changes. (Energy saver can also
    suspend transparency without changing that setting; Windows then shows a solid colour in place
    of the blur.) Builds without system backdrops (before Windows 11 22H2) always use the
    plain bar.
- `monitor`: 0 is the primary monitor, 1..n are the others in enumeration order, `"primary"`, or
  `"each"` (a copy on every monitor). A dock whose monitor is not connected is parked (no window)
  until it is.
- `hideTaskbar`: hide the Explorer taskbar while the dock runs.
- `startWithWindows`: mirrors the Run-key value. It is applied on Reload config and changed by
  the menu toggle. It is not applied at startup.
- `renderer`: `"auto"` (default; GPU, falling back to software) or `"software"` (always WARP).
  Software rendering avoids the GPU driver's per-process memory; the dock redraws rarely, so it
  is cheap.
- `animation`: see Animation below.
- `tray`: `{ "enabled": false }` (default; the key is written only while on): the notification
  area takeover (see System tray). It runs while `enabled` is on and some dock has a `tray` item.
- `notifications`: `{ "pollS": 2, "clearOnActivate": true, "balloons": false, "excluded": [] }`
  (the defaults; the object is written only when something differs): `pollS` seconds between
  polls (1 - 60), `clearOnActivate` activating an app from the dock marks its notifications read,
  `balloons` tray balloons go to the dock's notification list instead of Windows toasts,
  `excluded` app keys (lower-case exe paths or AppUserModelIDs) that never show a badge. See
  Notifications and badges. Per dock: `badges` (default `false`; written only when on).

### Themes

`theme.preset` picks a theme; every theme key written next to it in `config.json` overrides that
one value. The layers are: the preset -> a user theme file (if `preset` names one; the file has
its own built-in `base`) -> the keys set inline in `config.json`. The dock saves only `preset`
and the keys you set inline, so editing a theme file keeps affecting the dock. Inline values that
equal the preset's value are dropped when the file is loaded. A config without `preset` (older
versions) loads as `"dark"` with its theme keys as overrides; an untouched older config therefore
becomes just `"theme": { "preset": "dark" }` on the next save. `"preset": ""` means no preset:
the inline keys over the dark defaults.

Built-in presets: `dark` (Dark, the default), `light` (Light), `glass` (Glass, a thinner
acrylic tint), `solid` (Solid, opaque, no acrylic, 12 DIP corners), `shelf` (Glass shelf, a pale
sea-green glass shelf in perspective with angled sides, see `shelf`), `leopard` (3D shelf: a cool
grey glass shelf with icon reflections, a glow on the hovered icon and the 3D perspective tilt) and
`contrast` (High contrast, black with white borders, square corners). Ids are case-insensitive.

User themes live in `%LocalAppData%\WinDock\themes\<id>.json`; the file name without `.json` is
the id used in `preset` (case-insensitive). A file named like a built-in id is ignored.

```json
{ "name": "Nord", "base": "dark", "background": "#CC2E3440", "acrylic": false, "dotRunning": "#FF88C0D0" }
```

`name` is the display name, `base` a built-in id (default `dark`); the other keys are theme keys
(unknown keys are ignored). If the preset is neither a built-in nor a readable, well-formed theme
file, the dock uses `dark` for every key not set inline, keeps the name in the config so you can
fix the file, and **Reload config** beeps once (the rest of the config still applies).

Theme keys (colours are `#AARRGGBB` or `#RRGGBB`; defaults are the `dark` preset):

| Key | Default | Used for |
| --- | --- | --- |
| `background` | `#CC202020` | bar fill without acrylic (also the preview popup) |
| `cornerRadius` | `16` | bar corner radius in DIPs, 0 - 64 (snapped with acrylic, see `acrylic`) |
| `acrylic` | `true` | blur behind the bar (see below) |
| `acrylicBackground` | `#66202020` | bar tint over the blur while acrylic is active |
| `labelBackground` | `#E6303030` | name label fill (above the hovered icon) |
| `labelText` | `#FFFFFFFF` | name label text, preview card titles and close glyph |
| `labelBorder` | `#1FFFFFFF` | name label hairline |
| `labelCornerRadius` | `7` | name label corner radius in DIPs, 0 - 32 |
| `accent` | `#FF4CC2FF` | insertion marker while files are dragged over the dock |
| `barBorder` | `#24FFFFFF` | hairline around the bar and the preview popup |
| `separator` | `#38FFFFFF` | line between the app icons and the widgets, and separator items |
| `dotRunning` | `#80FFFFFF` | dot under a running app |
| `dotActive` | `#FFFFFFFF` | dot under the app owning the foreground window |
| `dotAttention` | `#FFFF9E26` | dot under an app flashing for attention |
| `clockText` | `#F2FFFFFF` | clock time |
| `clockDateText` | `#B6FFFFFF` | clock date line |
| `placeholderFill` | `#FF6B7385` | placeholder icon (missing pin target) |
| `placeholderText` | `#F2FFFFFF` | the placeholder's letter |
| `previewHotBackground` | `#1AFFFFFF` | hovered preview card (half of it: icon box of a card without thumbnail) |
| `previewHotBorder` | `#33FFFFFF` | hovered preview card outline |
| `closeHover` | `#FFC42B1C` | preview close button while hovered |
| `shelf` | `false` | draw the bar as a glass shelf under the icons instead of a panel: `background` is its top surface, `barBorder` its highlights, `cornerRadius` rounds the lip's bottom corners; acrylic is not used |
| `shelfSlant` | `14` | how far the shelf's sides angle in at the back, DIPs, 0 - 64 |
| `shelfEdge` | `#E6A0CDBE` | the shelf's front lip |
| `glow` | `"off"` | when a soft halo shows behind an icon: `"off"`, `"hover"` (the hovered icon; it fades in and out with the label's fade time), `"active"` (the app owning the foreground window), `"always"`; case-insensitive, an unknown value keeps the preset's |
| `glowColor` | `#00000000` | halo colour; alpha 0 = each icon's own dominant colour (the accent for placeholder icons); the colour's alpha scales the strength |
| `glowRadius` | `14` | halo blur radius in DIPs, 4 - 32 |
| `glowStrength` | `0.6` | halo opacity, 0 - 1 |
| `reflection` | `false` | a mirrored copy of each icon under it, fading out downwards (the bar grows by its height) |
| `reflectionHeight` | `0.5` | reflection height as a fraction of the icon, 0.2 - 1 |
| `reflectionOpacity` | `0.35` | reflection opacity at the icon's foot (fading to 0), 0 - 1 |
| `reflectionClip` | `false` | clip the reflection at the bar's edge instead of growing the bar: the dock keeps its height and shows the part of the reflection that fits |
| `perspective` | `false` | tilt the bar, the icons and the clock back about the bar's bottom edge (3D); acrylic is not used |
| `perspectiveAngle` | `18` | tilt in degrees, 0 - 35 |
| `badgeFill` | `#FFFF3B30` | notification badge bubble (light `#FFE5342A`, contrast `#FFFFD400`) |
| `badgeText` | `#FFFFFFFF` | the badge's count (contrast `#FF000000`) |
| `badgeSize` | `0.36` | the bubble's height as a fraction of the icon size, 0.2 - 0.6 (at least 12 DIPs) |

#### Icon effects

The glow and the reflection are drawn once into extra surfaces of each icon (at the magnified
bitmap size, like the magnified icon), whenever the icon itself is drawn: magnification, launch
bounces and drags only move and scale them, so they cost nothing per frame and the dock stays idle.
The glow is a Direct2D Gaussian blur of the icon (sigma = radius / 3) whose coverage is boosted
(x 3, clamped) and recoloured; the effects exist only while the icon is drawn. Separators get
neither. With reflections the bar grows by the reflection height, the running dots move to the
icons' feet, the clock stays centred on the icon row and the glass shelf deepens so the
reflections lie on it. A glow that shows at rest (`"active"`, `"always"`) reaches above the bar:
the dock window at rest is that much taller (the band above the bar takes clicks but does
nothing).

On a top dock the reflections are drawn above the icons (toward the screen edge); left / right
docks have none. With `reflectionClip` the bar does not grow: the icons are clipped at the bar's
screen-edge side, so the part of each reflection between the icon and that edge shows (magnified
icons' reflections are cut there too; so is a glow reaching past that edge). The perspective pivots on the bar's screen-edge side on every edge (see
[Position](#position-edge-alignment-offset)).

The perspective is a DirectComposition 3D transform (a rotation about the bar's bottom edge plus
a perspective projection, viewer distance one dock-stage width) on the bar, the icons and the
clock / weather widget; it is static and does not redraw anything. It is cosmetic: clicks, hover
and drops still use the flat layout, which matches the tilted icons closely because the tilt
pivots on the bar's bottom edge. Acrylic is turned off while it is on (the blur window is a flat
rectangle), like with `shelf`. If DirectComposition refuses the transform the dock stays flat
(`WinDock.QueryState` 32 tells which effects are actually applied).

Memory (measured, 13 items): with the hardware renderer the effects add about 3 MB (reflection)
to 7 MB (`leopard` with an always-on glow) of private bytes; with `"renderer": "software"` (WARP)
the effect and layer pipeline adds about 12 - 14 MB (reflection) to 18 - 22 MB (`leopard` with an
always-on glow) on top of ~24 MB.

### Animation

All animations run in DWM (DirectComposition); the dock only sets one-shot timers to notice when
one has ended, so the settings below cost nothing while the dock is idle. They apply on "Reload
config" (the next animation uses them).

- `speed` (default 1; 0 or 0.25 - 4): every animation duration is divided by it: the return to
  rest (120 ms), the auto-hide slide (180 ms), a launch bounce (300 ms up, 300 ms down), the name
  label fade, the window-preview fade (120 ms), the fade of an icon dragged off the dock (150 ms)
  and an icon landing after a drag (120 ms). The delays (`hideDelayMs`, `revealDelayMs`,
  `previewDelayMs`, the 250 ms label delay) and the 80 ms magnification follow are not scaled.
  0 turns animations off: everything snaps, the magnification follow included, and launching
  shows no feedback.
- `hideStyle`: how auto-hide hides and reveals the dock. `"slide"` (default): slides down and
  fades out. `"slideOnly"`: slides at full opacity. `"fade"`: fades in place; the acrylic blur
  cannot fade, so it goes away when a hide starts and comes back when a reveal has finished.
  `"none"`: hides and reveals at once. Reversing mid-way (the mouse comes back) always continues
  from where the dock is.
- `launchStyle`: feedback while a launched app has not shown a window yet (up to 5 s).
  `"bounce"` (default): bounces repeatedly, then finishes the current bounce. `"hop"`: one bounce;
  the app still counts as launching (a second click does not start another copy). `"pulse"`: the
  icon's opacity pulses (1 -> 0.45 -> 1 every 600 ms), then fades back. `"none"`: no feedback.
- `magnifyFalloff`: `"cosine"` (default) or `"linear"` (scale falls off as 1 - distance / range).
- `easing`: the curve of the settle animations (return to rest, landing, auto-hide slide, label
  position). `"easeOut"` (default, cubic), `"linear"`, or `"spring"`: overshoots by 12 % (60 % of
  the duration), then settles; the bar's edges, the icons and the acrylic blur all spring
  together. The magnification follow while the mouse moves always eases out, and fades
  (opacity) never overshoot (`"spring"` fades ease out).
- `labelFadeMs` (default 120, 0 - 1000): the name label's fade in / out, divided by `speed`;
  0 = instant.
- `reduceMotion`: `"system"` (default) follows Settings > Accessibility > Visual effects >
  Animation effects, live (no restart needed); `"on"` always reduces motion; `"off"` never does.
  Reduced motion makes hide / reveal, the return to rest, landings, the label and the popup
  instant and turns launch feedback off, but keeps the 80 ms magnification follow so the icons
  still track the mouse smoothly. `WinDock.QueryState` 17 reports whether it is in effect.

Unknown values fall back to the default.

## Automation (for tests)

The dock windows (class `WinDock.DockWindow`) and the app's hidden window (class `WinDock.App`,
always there, also when every dock is parked) handle these registered messages:

- **Which dock**: every dock-level message (`QueryItemPoint`, `QueryState`, `DockCommand`,
  `ItemCommand`) takes the dock in the **high word of `lParam`** (bits 16 - 31) and what `lParam`
  carried before (an item index, a popup card) in its low word. Dock 0 is the first dock, so an
  `lParam` below 0x10000 (every script written for one dock) keeps its meaning; a negative
  `lParam` also means dock 0. It does not matter which dock window (or the app window) receives
  the message. Docks are numbered in `docks[]` order, a `"monitor": "each"` dock once per monitor
  (in monitor order); a parked dock keeps its number (`QueryState` 49 = 1; the per-dock values
  return -1 for it, `DockCommand` still applies config commands such as 116 Lock dock or 120 - 123
  to it). `ItemCommand` with a window handle in `lParam` acts on the first dock that shows that
  window (`QueryItemPoint` with an HWND in `wParam` looks in dock `HIWORD(lParam)`).
- **Item indexes** (`QueryItemPoint`, `ItemCommand`, `QueryState` 2 / 9 / 24 / 25 / 33 / 34 / 35)
  count every slot of the dock in its order: icons, separators and the clock / weather widgets
  (one slot each; the `running` item one slot per unpinned running app; with `windows`
  `"unstacked"` one per window, a pinned app's windows right after its pin). `QueryItemPoint` /
  `ItemCommand` with a window handle find that window's own slot on an unstacked dock. Before 1b the widgets
  were not items; with the default order (`[pins..., running, weather, clock]`) the indexes of
  the pins and running apps are unchanged because the widgets come last.
- `WinDock.QueryItemPoint`: `wParam` = a top-level HWND, or 0 with `lParam` = item index.
  Returns the screen point of the item's icon centre (a widget: the centre of its box) as
  `MAKELONG(x, y)`, or -1. The point is in the rest (unmagnified) layout of the shown dock, also
  while the dock is hidden. Because the magnified layout is anchored at the pointer, moving the
  pointer there hovers that item (a widget is not "hovered": `QueryState` 2 stays -1 over it).
  `ItemCommand` on a widget's index does nothing (it is not a pin and has nothing to launch).
- **Tray** (round 4 phase 8): `QueryState` 97 = 1 while the takeover runs (the dock owns
  `Shell_TrayWnd`), 98 = notify icons registered (every app's), 99 = icons drawn in tray slot
  `lParam`'s grid (-1 not a tray slot), 100 = 1 while this dock's tray drawer is open, 101 = 1
  when tray slot `lParam` shows the overflow chevron (-1 not a tray slot), 102 = the tray cell
  under the pointer (-1 none), 103 = `Shell_NotifyIconGetRect` queries answered, 104 = times the
  dock's `Shell_TrayWnd` was raised above Explorer's again, 105 = balloons captured, 106 / 107 =
  the owner window (as a number) / `uID` of the first tray slot's drawer icon `lParam` (0 / -1
  none), 108 = icons in that drawer (-1 no tray slot), 109 = `TaskbarCreated` broadcasts repeated
  after such a raise, 110 / 111 = the `order` / `hidden` keys of tray slot `lParam`'s item (-1 not
  a tray slot), 112 = trayApp slot `lParam`: 1 its icon is registered, 0 the placeholder (-1 not
  a trayApp slot); 24 reports 8 for a tray slot and 9 for a trayApp slot, 93 reports 3 while the
  folder popup is the tray drawer. `DockCommand` 128 Add widget > Tray (also turns `tray.enabled` on), 129 tray on,
  130 tray off (the icons go back to Explorer). `ItemCommand` 822 / 823 = a left / right click on
  tray slot `lParam`'s drawer icon `HIWORD(wParam)` (the drawer's order, 106 / 107 tell which is
  which), forwarded as if clicked at the icon's place; returns 1 if it was delivered; 8b: 824 pins
  that drawer icon as a trayApp item (after the last pin), 825 unpins trayApp slot `lParam`, 826
  hides / shows that drawer icon in the grid (a toggle), 827 moves drawer icon `wParam` bits 16 -
  23 right before drawer icon bits 24 - 31 in the grid order (255: to the end). Settings page
  12 Tray: 4100 the takeover check box, 4104 / 4105 rows, 4107 / 4108 columns, 4110 icon size (0 =
  16, 1 = 20, 2 = 24 px), 4112 the icon list (check boxes: shown in the grid), 4113 Reset to
  Explorer, 4114 Put the tray in its own dock (8b), 4116 Add a tray to this dock. The tray menu
  (Ctrl + right click) is a `#32768` of the WinDock process with the ids 1 Hide / Show, 2 Pin, 3
  Unpin, 4 Tray settings….
- **Notifications** (round 4 phase 9): `QueryState` 113 = the notification service (0 off, 1
  polling, 2 access denied / not available), 114 = unread notifications (every app), 115 = the
  badge count of slot `lParam` (-1 no such slot), 116 = ms since the last poll (-1 none), 117 =
  polls since the service started, 118 = the interval the worker waits now (ms; 0 off), 119 =
  notifications listed (read ones and balloons included), 120 = balloons whose `NIF_INFO` was
  stripped from the copy mirrored to Explorer, 121 = the width in px of slot `lParam`'s badge
  surface (0 none), 122 = the worker thread's own CPU time (µs), 123 = 1 while the notification
  centre is open, 124 = its rows (-1 closed), 125 = what it shows (0 rows, 1 "No notifications", 2
  access denied; -1 closed), 126 = its selected row (-1 none), 127 = the notification id of its row
  `lParam` (0 none); 24 reports 10 for a notifications slot. `DockCommand` 131 = poll now, 132 =
  toggle this dock's `badges` (saved), 133 = show / hide the centre over the dock's first
  notifications item, 134 = Add widget > Notification centre. `ItemCommand` on the dock's open centre
  (the dock in the high word of `lParam`, the row in `HIWORD(wParam)`): 828 dismisses row n (its
  cross), 829 Clear all, 830 opens row n as a click does; each returns 1 when the centre is open and
  the row exists. A click on the bell's slot (ItemCommand 810) opens / closes it. The centre is a
  `WinDockNotifications` window of the WinDock process (Delete / Escape / arrows can be posted to
  it as `WM_KEYDOWN`). Settings page 13 Notifications: 4200 badges, 4202 the centre item, 4205
  balloons, 4208 clear on activate, 4211 / 4212 poll interval, 4217 the status text, 4218 Open
  Windows settings, 4221 the app list (check boxes; column 2 is the app key). Test hooks (environment of the dock process, read when
  the service starts): `WINDOCK_NOTIFY_FILTER=text;text` lists only toasts whose AUMID (balloons:
  whose exe path) contains one of the parts, so tests never touch other notifications;
  `WINDOCK_NOTIFY_DENIED=1` behaves as if access were denied.
- `WinDock.DockCommand`: `wParam` = a dock menu command (100 Auto-hide toggle between off and the
  last on-mode, 101 Hide taskbar toggle, 102 Start with Windows toggle, 103 Open config file,
  104 Show taskbar until re-enabled, 105 Quit, 106 Reload config, 108 Auto-hide off, 109 Auto-hide
  always, 110 Auto-hide when a window overlaps, 107 open or activate the settings window,
  111 Refresh weather, 112 Add separator, 113 Weather settings… (the settings window on the
  Weather page), 114 show / hide the calendar popup, 115 show / hide the weather popup, 116 Lock
  dock toggle, 117 Add widget > Clock, 118 Add widget > Weather, 119 Add widget > Running apps,
  120 / 121 / 122 / 123 Position on screen > Bottom / Top / Left / Right, 124 Lock all docks,
  125 Unlock all docks, 126 Reload widgets (every script widget of every dock reloads its package
  and script), 127 open (or activate) the widget Console; saved like the menu).
  Runs it as if it was chosen from the menu: 112 and 117 - 119 do nothing while the dock is locked
  (greyed in the menu), 119 beeps when the dock already has a running apps item. Returns 1 for a
  known command. A popup opened this way usually cannot take the foreground (see the folder popup
  below).
- `WinDock.QueryState`: `wParam` selects the value. 0 = auto-hide state (0 Shown, 1 Hiding,
  2 Hidden, 3 Showing). 1 = current `SHQueryUserNotificationState` value. 2 = hovered item index
  (-1 for none). 3 = 1 if the name label is shown (0 as soon as it starts fading out). 4 = 1
  while magnified. 5 = number of pending interaction timers (the label fade-out and the
  intelligent-hide re-test included) and running slides (0 when idle). 6 = 1 while the acrylic backdrop is active.
  7 = drag state (0 none, 1 dragging an icon, 2 fading out an icon dragged off the dock, 3 files
  dragged over the dock). 8 = 1 while the dock window has the mouse capture. 9 = insertion index
  of the current drag (-1 for none). 10 = 1 if the OLE drop target is registered. 11 = 1 while
  the window-preview popup is visible. 12 = number of cards in the popup (0 when hidden).
  13 = the window handle shown by popup card `lParam` (0-based; 0 if there is none).
  14 = auto-hide mode (0 off, 1 always, 2 intelligent). 15 = 1 while the foreground window
  overlaps the dock (`"intelligent"` only; always 0 in the other modes). 16 = 1 while the
  settings window exists. 17 = 1 while
  reduce-motion is in effect (`animation.reduceMotion` `"on"`, or `"system"` with Windows'
  animation effects off). 19 = number of window events the intelligent-hide hooks accepted
  since the dock started (foreground changes, moves, minimize / restore of the foreground
  window; it stops counting in the other modes). 20 = 1 if the theme preset failed to resolve on
  the last load (the dock uses `dark`). 21 = number of callbacks those hooks delivered, the
  filtered ones (cursor, caret, child windows) included. 24 = kind of item `lParam` (0 app,
  1 folder, 2 separator, 3 Recycle Bin, 4 clock, 5 weather, 6 script widget, 7 shortcut group; -1 if there is no such item). 25 = 1 if item `lParam`
  has a custom icon from its `icon` key (0 if not, -1 if there is no such item). 22 = 1 while the
  folder popup is visible. 23 = number of entries of the folder the popup shows (all of them,
  scrolled into view or not; 0 when it is hidden). 26 = drop mode while files are dragged over the dock (0 none, 1 insertion marker,
  2 open with the app under the pointer, 3 recycle on the Recycle Bin). 27 = weather state
  (0 none: the widget is off, has no location or has no data yet; 1 stale: cached data, or the
  last refresh failed; 2 fresh; 3 error: the fetch failed and there is no data). 28 = the current
  temperature x 10 in the configured units (`INT_MIN` without data). 29 = 1 while the weather
  popup is visible. 30 = seconds since the weather data was fetched (-1 without data; cached data
  counts from when it was fetched). 31 = 1 while the calendar popup is visible. 32 = the icon
  effects actually applied, a bit mask: 1 glow (some icon has a glow surface), 2 reflections, 4 the
  perspective tilt. 33 = the glow opacity target x 100 of item `lParam` (100 while its halo shows,
  0 when hidden or without a glow surface, -1 if there is no such item). 34 = the height in pixels
  of item `lParam`'s reflection surface (at the magnified bitmap size; 0 when reflections are off
  and for separators, -1 if there is no such item). 35 = the
  dominant colour of item `lParam`'s icon as `0xAARRGGBB` (a saturation- and alpha-weighted
  average, computed once per icon load; 0 when unknown, -1 if there is no such item). 36 = 1
  while the dock is locked. 37 = the index in `docks[0].items` of the config item that shows item
  `lParam` (every running app of the `running` item reports that item's index; -1 if there is no
  such item). 38 = the edge the dock is laid out on (0 bottom, 1 top, 2 left, 3 right). 39 =
  `docks[0].align` (0 center, 1 start, 2 end). 40 = `docks[0].offset` (DIPs). 41 = 1 while laid out
  full width. 42 = `docks[0].reserveSpace`. 43 = 1 while the dock is registered as an app bar
  (reserving screen space). 44 = the peek strip's height in px (0 when off). 45 = 1 while a title
  is drawn. 46 = dock drag (0 none, 1 moving it along its edge, 2 over another edge's zone).
  47 = the number of docks (parked ones included). 48 = the dock's monitor index (0 primary, as
  in `docks[].monitor`; -1 parked; -2 no such dock). 49 = 1 if the dock is parked (its monitor is
  not connected: no window), 0 if shown, -1 no such dock. 50 = its index in `docks[]`. 51 = its
  window handle (0 parked). 52 = `docks[].layer` (0 normal, 1 desktop). 53 = a desktop-layer
  dock's window is shown (1) or hidden because another window is in the foreground (0); -1 for a
  normal dock. Script widgets (24 reports kind 6 for their slots): 54 = the widget state of slot
  `lParam` (0 ok, 1 error, 2 suspended by the watchdog; -1 if the slot is not a script widget).
  55 = the number of script widgets on the dock. 56 = the number of script widgets of every dock
  (live script runtimes). 57 = the script heap of slot `lParam`'s widget in bytes (0 once it
  failed: the runtime is freed). 58 = how often slot `lParam`'s widget has drawn (`draw()` calls).
  59 = how often the widget timer scheduler has woken the process (all widgets; stays put while no
  widget waits for a timer). 60 = the log lines slot `lParam`'s widget has written. 61 / 62 = slot
  `lParam`'s box along / across the dock in px (an icon slot: the icon size). 63 = 1 while the
  widget Console is open (with `lParam` 1: its window handle). 64 = the log lines the Console shows
  now (-1 when it is closed). 65 = 1 while a script widget's popup (`popup.open`) is visible on the
  dock. 66 = that popup's size in px as `MAKELONG(width, height)` (0 when hidden). 67 = the
  widgets' audio provider: change notifications handled since the start (-1 while it is not
  running: no widget uses `audio`). 68 = the media provider: session snapshots received (-1 not
  running). 69 = the media provider's current session: 0 none, 1 stopped, 2 paused, 3 playing (-1
  not running). Task list (phase 6): 70 = the slots were built unstacked (1, `windows`
  `"unstacked"`) or stacked (0). 71 = `titles` (0 off, 1 one line, 2 two lines). 72 = lines of
  title text actually laid out under / beside the icons (0: none, also on a vertical dock with
  `titlesVertical` `"off"`). 73 = the window handle of window slot `lParam` (0 for an app-level
  slot such as a pin, -1 if there is no such slot). 74 = the length in characters of the text the
  hover label shows (-1 while no label is shown). 75 = the length of preview card `lParam`'s
  title (-1 no such card). 76 = how many live title changes this dock has applied (slot titles,
  the label, preview cards). 77 = the length of slot `lParam`'s title under the icon (0: none).
  78 = slot `lParam`'s dot (0 none, also for a minimized window slot; 1 running, 2 the active
  window / app, 3 wants attention; a group: its members'). Thumbnails: 79 = `thumbnails` (0 off, 1 live, 2 snapshots).
  80 = slot `lParam` is not a thumbnail tile (0), a tile without a picture yet (1) or with one (2;
  -1 no such slot). 81 = live frames this dock received. 82 = `PrintWindow` calls of every dock.
  83 = live capture sessions running now. 84 = snapshot rounds this dock started. 85 = the window
  slot `lParam`'s tile shows (0: not a tile). 86 = the HRESULT of the last live capture that could
  not start (0 none). Phase 7, the folder popup: 87 = its layout (0 grid, 1 list; -1 hidden),
  88 = rows that have a visual (surface) now (a screenful; both ends' rows during an animated
  scroll), 89 = scroll offset in px, 90 = the selected entry (-1 none), 91 = characters in the
  path of the folder it shows (grows when navigating into a sub-folder; 0 hidden), 92 = its
  columns (1 in a list; 0 hidden). 5 counts the popup's scroll / scrollbar timers. Phase 7b:
  93 = what the popup shows (0 hidden, 1 a folder, 2 a group), 94 = the slot a dragged icon
  would group with if released now (-1 none; armed after the 0.6 s hold, at once for a group),
  95 = members of group slot `lParam` (-1 not a group), 96 = the slot the dragged icon is held
  over (the hold is running or done; -1 none). 5 also counts the hold timer. 26 reports 4
  while files are dragged over a script widget that takes them (`onDrop`). Per dock: 14 / 36 /
  39 / 40 / 42 and 37 refer to that dock's own config entry. `QueryItemPoint`
  answers for every edge (on a vertical dock the slots' y grows along the dock). While a widget is dragged, 7 is 1 and 9 is the slot index it would land at.
- `WinDock.OpenSettings` (posted, no parameters): opens or activates the settings window; this is
  what `WinDock.exe --settings` sends to a running dock.
- Settings window (class `WinDock.Settings`, title "WinDock Settings"): the page list is the child
  `WinDock.SettingsNav` (control id 100; its window text is the selected page's name); the pages
  are `WinDock.SettingsPage` children with ids 200 + page index (0 General, 1 Appearance,
  2 Behaviour, 3 Animation, 4 Dock contents, 5 About, 6 Weather, 7 Docks, 8 Layout), and every control on them has the stable id
  listed in `src/SettingsIds.h` (the Dock contents page kept the Pinned apps ids 2600 - 2611 and
  added 2612 Add widget, 2613 Settings…, 2614 Lock the dock; its list 2600 has one row per config
  item, columns Name, Kind, Details; General 2130 / 2131 are retired, replaced by 2132 Show
  seconds, 2135 Clicking the clock opens, 2137 App or URL, 2138 Browse…; Position 2140 - 2148: 2142 screen edge and 2144 alignment drop-downs in DockEdge /
  DockAlign order, 2146 / 2147 offset box and up-down, 2149 Full width, 2151 Reserve screen space,
  2154 / 2155 peek, 2157 / 2158 peek opacity (%), 2160 title (applied on Enter / focus loss); Behaviour added 2223
  single-window previews, 2227 middle click, 2229 mouse wheel, and in phase 6 2232 Windows (0
  stacked, 1 one slot per window), 2234 Titles under icons (0 off, 1 one line, 2 two lines), 2236
  Titles on vertical docks (0 off, 1 beside the icon; disabled while titles are off), 2239 Window
  thumbnails (0 off, 1 live, 2 snapshots; 2240 the note on the trade-off), 2242 / 2243 snapshot
  interval (s), 2246 / 2247 live frames per second, 2250 / 2251 tile length (DIPs, 0 = twice the
  icon); phase 3: General 2162 "Show only
  the windows on this dock's display" (`runningFilter`), the Display drop-down 2111 ends with
  "Every display" (`"each"`), Appearance 2420 "This dock's theme" (0 = same as all docks, then the
  theme list), Docks page 2800 list (Dock, Edge, Display, Windows; selecting a row makes it the dock
  the other pages edit), 2801 Add dock, 2802 Duplicate, 2803 Remove, 2805 "The other pages edit:
  (id)", 2806 Lock all docks, 2807 Unlock all docks; phase 3b: General 2165 Layer (0 above
  windows, 1 desktop only), Layout page 2900 the canvas (class `WinDock.LayoutCanvas`; its window
  text lists every dock instance as `index:config index:monitor:left,top,right,bottom;` in canvas
  client pixels, `-` for a parked one), 2902 the selected dock / where a drag would drop it;
  phase 4b: page 9 Widgets (3500 list: Name, Id, Source, Version; 3501 Add from folder…, 3502
  Export built-in to folder, 3503 Reload widgets, 3504 Open widgets folder, 3505 Console…, 3506
  Add to dock, 3508 the last action's result), page 10 Widget settings (3601 the widget item
  drop-down; then per schema field k `3700 + 10 * k + part`: 0 label, 1 the control (check box,
  edit, drop-down), 2 the check box's caption or the int's up-down, 3 the field's note; 3603 Reset
  to defaults; the page's controls are rebuilt when another item is picked; its title 2000 reads
  "Widget: <name>"); Dock contents 2613 Settings… on a widget row opens page 10 for it);
  phase 7: page 11 Folders and groups (4001 the item drop-down: the edited dock's folder items,
  shortcuts to folders included; 4003 Show the contents as: 0 Automatic, 1 Grid, 2 List; 4005
  Choose icon…, 4006 Choose image…, 4007 Reset icon; 4008 the folder's path); Dock contents 2613
  Settings… on a folder row opens page 11 for it; phase 7b: groups on page 11 too (4010 the name,
  committed on `EN_KILLFOCUS`; 4012 the members list (Name, Target); 4013 Add app…, 4014 Add
  folder…, 4015 Remove, 4016 Move up, 4017 Move down; hidden for a folder).
  "Add widget ▾" opens a popup menu
  (items 1 Clock, 2 Weather, 3 Running apps, then the installed widgets 100 + n), so press it with `PostMessage`. Check boxes and radio buttons carry no visible text of their own:
  their caption is a separate static (id + 1) that forwards clicks. The Appearance page's theme
  field rows use `3000 + 10 * field index + part` (ids 3000 - 3399; field index in
  `Theme::Fields()` order; part 0 label, 1 swatch / check box, 2 hex box / check box caption,
  3 alpha or value box (the glow mode's drop-down), 4 its up-down, 5 Reset, 6 unit). The preview strip is the child `WinDock.SettingsPreview` (control id 101).
- `WinDock.ItemCommand`: `wParam` = a per-item command (800 pin/unpin toggle (a separator's
  Remove), 801 new window (the Recycle Bin's Open), 802 open file location, 803 open a folder in
  Explorer, 804 Choose icon… and 808 Choose image… (they open their dialog; sent rather than
  posted, the command is posted back to the dock so the call returns at once), 805 Reset icon
  (removes the pin's `icon`; scriptable), 806 show / hide a folder's popup, 807 empty the Recycle
  Bin (the shell's confirmation appears; it runs on a thread of its own, so the call returns at
  once), 809 reload a script widget (its package and script; nothing for other slots), 810 what a
  left click on the slot does (activates / minimizes a window slot's window, activates / launches
  an app, opens a folder's or widget's popup; by slot index, so it reaches each window slot of an
  unstacked app), 811 / 812 / 813 a folder's or group's popup layout automatic / grid / list
  (saved as its `view`), 817 Rename… a group (a dialog: sent, it is posted back like 804; the
  dialog is a `#32770` of the WinDock process with the edit box 100, OK = `IDOK`), 818 Ungroup,
  819 / 820 / 821 with an argument in the high word of `wParam` (the low word is the command):
  819 slot `lParam` onto slot `HIWORD(wParam)` (a group of the two, or added to that group), 820
  group slot `lParam`'s member `HIWORD(wParam)` out (a pin right after the group), 821 that member
  removed; 800 on a group unpins it with its members, 806 shows / hides its popup, 900 close all windows),
  `lParam` = item index or a window handle of the item. Returns 1 if the item was found. A folder
  popup opened this way usually cannot take the foreground (the dock did not get the click), so
  Escape does not reach it; it still closes when another window comes to the foreground, on a dock
  click, or with 806 again. On the dock's open folder popup (the low word of `lParam` is the
  argument, not an item; the dock in the high word as usual): 814 select entry n (scrolled into
  view), 815 scroll to n px (animated unless reduce motion), 816 open entry n as a left click
  does (a sub-folder navigates); they return 0 while no popup is open. Keys can be posted to the
  popup window (`WM_KEYDOWN` arrows / Home / End / Enter / Backspace, `WM_SYSKEYDOWN` Alt+Up,
  `WM_CHAR` for type-to-select) and `WM_MOUSEWHEEL` scrolls it.
- Popup windows: the window-preview popup is class `WinDockPreview`, the folder popup
  `WinDockFolder`, the weather popup `WinDockWeather`, the calendar `WinDockCalendar`, a script
  widget's popup `WinDockWidgetPopup`.
- Widget Console (class `WinDock.WidgetConsole`, `DockCommand` 127, `QueryState` 63 / 64): 4001 the
  instance list (LISTBOX, one row "dock · name (id#n) · state"; select with `LB_SETCURSEL` +
  `WM_COMMAND(LBN_SELCHANGE)`), 4002 the log (read-only EDIT, `WM_GETTEXT`), 4003 Reload, 4004 Open
  folder, 4005 Clear, 4006 the count.
