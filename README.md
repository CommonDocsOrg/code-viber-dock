# WinDock

A macOS-style dock for Windows 11. C++20 / Win32, Direct2D + DirectComposition, no third-party
dependencies. Single static exe (no VC++ redistributable needed).

Status: Phase 5. A translucent bar with a live clock at the bottom-center of the configured
monitor, plus a tray icon. Icons magnify under the mouse (macOS style), the hovered app's name
shows above it, and the dock auto-hides into the bottom screen edge. The Explorer taskbar is hidden while the dock runs and restored on
exit, crash recovery, or `--restore-taskbar`. Running apps appear on the dock (grouped per app,
with a dot under each). Pinned apps come first, in config order, followed by unpinned running apps
in the order they were first seen. A running app that matches a pin merges into the pinned item.

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
back to the bottom centre of the configured monitor. If that monitor no longer exists, it uses
the primary monitor.

### Dock items

- Left click: launches the app if it has no windows; otherwise activates, minimizes, or cycles
  through its windows. While an app is launching its icon bounces, until a new window shows up or
  5 s pass (`animation.launchStyle` can make it a single hop, an opacity pulse, or nothing).
- Middle click: starts a new instance (and bounces until its window appears).
- Right click: Pin to dock / Unpin from dock, the icon entries for a pinned app (see **Custom
  icons** below), New window (shows as Open when the app is not running), Open file location (for
  path-based items), Close for each window, then the dock menu.
  Folders, separators and the Recycle Bin have their own short menus (below). The dock menu has
  **Add separator** (appends a separator to the pinned items).
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
    nothing there.

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
  Pin one by dropping it (or a shortcut to it) on the dock, with **Add folder…** on the Pinned
  apps page, or in config.json.
  - A left click opens the folder's contents in a popup above the icon; clicking the icon again
    closes it. The popup shows the folder's name, an **Open in Explorer** button and a grid of its
    entries: sub-folders first, then files, in Explorer's name order ("file2" before "file10"),
    hidden and system entries left out, with the shell's icons and two-line names. Up to 48
    entries are shown (fewer on a small monitor); a **+N more** line opens the folder in Explorer.
  - In the popup, click a file to open it (the popup closes), click a sub-folder to show its
    contents in place, the back arrow or Backspace to go up again (never above the pinned folder),
    middle-click a sub-folder to open it in Explorer. Escape, clicking anywhere else (another
    window, the dock), a menu, a drag or the dock hiding close it. While it is open the dock does
    not auto-hide.
  - Middle click on the icon opens the folder in Explorer. Right click: the name, Unpin from
    dock, the icon entries, Open in Explorer, then the dock menu.
- **Recycle Bin** (`{ "kind": "recycleBin" }` in the dock's `items`, or **Add Recycle Bin** on the Pinned
  apps page): shows the bin's empty or full icon, which follows the bin (the dock asks the shell to
  tell it when the bin changes; nothing polls). Its name label reads "Recycle Bin (12 items,
  3.4 MB)", or "Recycle Bin (empty)". A click opens the Recycle Bin; files dropped on it are
  recycled. Right click: **Open**, **Empty Recycle Bin** (the shell's own confirmation; greyed when
  the bin is empty), **Unpin from dock**, the icon entries, then the dock menu.
- **Separators** (`{ "kind": "separator" }` in the dock's `items`, any number; **Add separator** in the dock
  menu or on the Pinned apps page): a thin vertical line in `theme.separator` in a narrow slot
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
  an icon in an `.exe` / `.dll`), saved within 300 ms and drawn at once; the Pinned apps page in
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

### Weather and calendar

**Weather widget** (`weather.show`, off by default; Settings > Weather): left of the clock, after
the separator, the current conditions as a small glyph (sun or moon, partly cloudy, overcast,
fog, drizzle, rain, snow, thunder; drawn as vector shapes, so they stay sharp at any DPI) and the
temperature in the clock's font. It moves with the clock while the bar grows under the mouse.
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
accent-coloured disc. No events. Escape or clicking anywhere else closes it. Turn it off with
`clock.calendarPopup` (General page: "Click the clock to show a month calendar").

## Settings

**Settings…** in the tray / dock menu, `WinDock.exe --settings` (asks the running dock to open it,
or starts the dock and opens it) and `WinDock.DockCommand` 107 open the settings window. There is
only ever one; opening it again brings it to the front. It appears centred on the dock's monitor,
above the dock, and is a normal window (Alt+Tab, minimize; the dock stays the topmost window).

Pages (left column; Up / Down / Home / End switch pages while it has the focus, Tab moves into
the page, Esc closes the window, Enter applies the text in an edit box):

- **General**: icon size (slider 16-128, the box goes on to 256), magnification (1.00-2.50),
  magnification range (1-4 icons), display (Primary, then `Display 2 (\\.\DISPLAYn)` ... in the
  dock's order), clock on / off, time format (pick one or type a `strftime` format; a preview shows
  the result), date line, the calendar popup on a clock click, Start with Windows, Hide the Windows
  taskbar, and the renderer (`auto` / `software`; takes effect after WinDock restarts, the page
  shows the one in use).
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
  strength), the reflection (on / off, height, opacity) and the 3D perspective (on / off, angle);
  see [Icon effects](#icon-effects). The preview strip under every page shows them on its
  placeholder icons (the perspective as the shelf's trapezoid with the icons a little shorter).
- **Behaviour**: auto-hide Off / Always / When a window overlaps the dock, hide and reveal delays,
  window previews on / off and their delay.
- **Animation**: speed (Off, then 0.25× to 4× on a logarithmic slider), hide style (slide and
  fade, slide, fade, none), launch feedback (bounce until the app opens, hop once, pulse, none),
  magnification falloff (smooth / linear), easing (ease out, linear, spring), the name label fade
  (0-1000 ms) and reduce motion (follow Windows, on, off; the page shows whether Windows'
  animation effects are on). See [Animation](#animation).
- **Pinned apps**: the pinned list in dock order (name and target; "(missing)" when the file is
  gone; separators show as "Separator"), **Add…** (pick one or more `.lnk` / `.exe` files; a
  shortcut is pinned as the shortcut, apps that are already pinned are skipped), **Remove**,
  **Move up**, **Move down**, **Add folder…** (a folder picker), **Add separator** (appends one),
  **Add Recycle Bin** (greyed while the bin is pinned), and for the selected entry **Choose
  icon…**, **Choose image…** and **Reset icon** (as in the dock's menu, see Custom icons; greyed
  for a separator, Reset only when the entry has an `icon`). The list shows custom icons, images
  included. Changes from the dock itself (drag to reorder, drop to pin, drag off to unpin, the
  icon entries) show up in the list at once.
- **About**: version, the config file path (click it to show the file in Explorer), **Open config
  file**, **Reload config**, and the renderer in use.
- **Weather** (after About): show the weather on the dock; **Find a place** (type a name, then
  Enter or **Search**; the results list shows "Name, Region, Country" and choosing one sets the
  place), the name shown, latitude and longitude (typed values apply on Enter or when the box loses
  the focus; an invalid value beeps and goes back), units (metric: °C and km/h; imperial: °F and
  mph), the refresh interval (5-360 minutes), the status ("Updated 10:15", "(stale)", "Updating…",
  "Not configured", "Error: …"), **Refresh now** and the Open-Meteo credit.

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
(when transparency effects are on; otherwise solid colours), dark buttons, boxes, lists, the pinned
apps list and its header, and scroll bars; sliders have no dark style in Windows, so the window
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
      "edge": "bottom", "locked": false,
      "iconSize": 48, "magnification": 1.5, "magnifyRange": 2,
      "autoHide": "always", "hideDelayMs": 400, "revealDelayMs": 150,
      "previews": true, "previewDelayMs": 600,
      "monitor": 0
    }
  ],
  "hideTaskbar": true,
  "startWithWindows": false,
  "renderer": "auto",
  "clock": { "format": "%H:%M", "showDate": false, "calendarPopup": true },
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

- `docks`: the docks. Only the first one is shown in this version (multiple docks come later); a
  file without any gets one with the defaults. Per dock:
  - `id` (default `"main"`).
  - `items`: everything on the dock, in order. Each item has a `kind`:
    - `"app"`: `{ "kind": "app", "path": "<.lnk or .exe>" }` or
      `{ "kind": "app", "aumid": "<AppUserModelID>" }`. Environment variables in paths are
      expanded. A shortcut to a folder works too (it is shown as a folder).
    - `"folder"`: `{ "kind": "folder", "path": "<folder>" }`, a folder stack (see Dock items).
    - `"separator"` (any number) and `"recycleBin"`: need no `path`.
    - `"running"`: where the running apps that are not pinned go.
    - `"clock"` / `"weather"`: the clock and the weather widget. The widget is shown when its item
      is present; the settings toggles add the item (at the end; the weather goes before the
      clock) or remove it.

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
    kept as written (and not shown). The app, folder, separator and Recycle Bin items form the
    pinned section. **In this version the dock still draws the pinned items (in item order), then
    the running apps, then the weather widget, then the clock, wherever the `running`, `weather`
    and `clock` items stand in the list** (a dock without a `running` item still shows the running
    apps); the order of those three is kept in the file for the coming element layout.
  - `edge` (default `"bottom"`, the only edge implemented) and `locked` (default `false`, not used
    yet): read and written for later versions.
  - The dock settings described below: `iconSize`, `magnification`, `magnifyRange`, `autoHide`,
    `hideDelayMs`, `revealDelayMs`, `previews`, `previewDelayMs`, `monitor`.

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
  above). `previewDelayMs` (default 600, 0 - 10000) is how long the mouse must rest on the icon.

- `clock.format` uses `strftime` specifiers (`%H:%M`, `%I:%M %p`, `%H:%M:%S`, ...). Formats with
  seconds tick every second; otherwise the clock wakes once a minute. `clock.calendarPopup`
  (default `true`): a click on the clock opens the month calendar (see Weather and calendar).
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
- `monitor`: 0 is the primary monitor, 1..n are the others in enumeration order. An index that no
  longer exists falls back to the primary monitor.
- `hideTaskbar`: hide the Explorer taskbar while the dock runs.
- `startWithWindows`: mirrors the Run-key value. It is applied on Reload config and changed by
  the menu toggle. It is not applied at startup.
- `renderer`: `"auto"` (default; GPU, falling back to software) or `"software"` (always WARP).
  Software rendering avoids the GPU driver's per-process memory; the dock redraws rarely, so it
  is cheap.
- `animation`: see Animation below.

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
| `separator` | `#38FFFFFF` | line between the app icons and the clock, and separator items |
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
| `perspective` | `false` | tilt the bar, the icons and the clock back about the bar's bottom edge (3D); acrylic is not used |
| `perspectiveAngle` | `18` | tilt in degrees, 0 - 35 |

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

The dock window (class `WinDock.DockWindow`) handles these registered messages:

- `WinDock.QueryItemPoint`: `wParam` = a top-level HWND, or 0 with `lParam` = item index.
  Returns the screen point of the item's icon centre as `MAKELONG(x, y)`, or -1. The point is in
  the rest (unmagnified) layout of the shown dock, also while the dock is hidden. Because the
  magnified layout is anchored at the pointer, moving the pointer there hovers that item.
- `WinDock.DockCommand`: `wParam` = a dock menu command (100 Auto-hide toggle between off and the
  last on-mode, 101 Hide taskbar toggle, 102 Start with Windows toggle, 103 Open config file,
  104 Show taskbar until re-enabled, 105 Quit, 106 Reload config, 108 Auto-hide off, 109 Auto-hide
  always, 110 Auto-hide when a window overlaps, 107 open or activate the settings window,
  111 Refresh weather, 112 Add separator, 113 Weather settings… (the settings window on the
  Weather page), 114 show / hide the calendar popup, 115 show / hide the weather popup). Runs it
  as if it was chosen from the menu. Returns 1 for a known command. A popup opened this way
  usually cannot take the foreground (see the folder popup below).
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
  1 folder, 2 separator, 3 Recycle Bin; -1 if there is no such item). 25 = 1 if item `lParam`
  has a custom icon from its `icon` key (0 if not, -1 if there is no such item). 22 = 1 while the
  folder popup is visible. 23 = number of entries (cells) shown in the folder popup (0 when it is
  hidden). 26 = drop mode while files are dragged over the dock (0 none, 1 insertion marker,
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
  average, computed once per icon load; 0 when unknown, -1 if there is no such item).
- `WinDock.OpenSettings` (posted, no parameters): opens or activates the settings window; this is
  what `WinDock.exe --settings` sends to a running dock.
- Settings window (class `WinDock.Settings`, title "WinDock Settings"): the page list is the child
  `WinDock.SettingsNav` (control id 100; its window text is the selected page's name); the pages
  are `WinDock.SettingsPage` children with ids 200 + page index (0 General, 1 Appearance,
  2 Behaviour, 3 Animation, 4 Pinned apps, 5 About, 6 Weather), and every control on them has the stable id
  listed in `src/SettingsIds.h`. Check boxes and radio buttons carry no visible text of their own:
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
  once), 900 close all windows),
  `lParam` = item index or a window handle of the item. Returns 1 if the item was found. A folder
  popup opened this way usually cannot take the foreground (the dock did not get the click), so
  Escape does not reach it; it still closes when another window comes to the foreground, on a dock
  click, or with 806 again.
- Popup windows: the window-preview popup is class `WinDockPreview`, the folder popup
  `WinDockFolder`, the weather popup `WinDockWeather`, the calendar `WinDockCalendar`.
