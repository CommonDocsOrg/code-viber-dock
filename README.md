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

Output: `build\x64-release\WinDock.exe`. Use the `x64-debug` preset for a debug build.

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
- Right click: Pin to dock / Unpin from dock, New window (shows as Open when the app is not
  running), Open file location (for path-based items), Close for each window, then the dock menu.
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
- Drag `.lnk` or `.exe` files onto the dock to pin them. While the files are over the dock, the
  icons open a gap at the insertion point with a thin accent-coloured marker in it (between the
  two icons whose centres the pointer is between, within the pinned section); dropping pins the
  files there. Files whose app is already pinned are skipped. Other file types are not accepted.
  Dragging files to the bottom screen edge reveals a hidden dock immediately. File drops use OLE
  drag and drop, so they work from Explorer and any other drag source running at the same
  integrity level (not from an elevated app).
- Pinning a running app adds it to the end of the pinned list, and the icon moves to that slot.
  Unpinning a running app keeps its icon until its last window closes. The icon moves to the
  running section in first-seen order, as on macOS.
- When a pinned file no longer exists, its entry stays in the config and the dock shows a
  placeholder icon. The dock never deletes pins silently.

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
  the result), date line, Start with Windows, Hide the Windows taskbar, and the renderer
  (`auto` / `software`; takes effect after WinDock restarts, the page shows the one in use).
- **Appearance**: the theme (the built-in presets, then your theme files from the themes folder by
  name; a `preset` that is not found is listed as "<id> (missing)"), **Export current theme…**,
  **Open themes folder**, **Reset all to preset**, and one row per theme field (see
  [Themes](#themes)) grouped as Bar, Name label, Indicator dots, Clock, Placeholder icon and Window
  previews. A colour row has a swatch (click it for the Windows colour picker; it replaces the
  colour and keeps the alpha), the `#AARRGGBB` value (applied on Enter or when the box loses the
  focus; an invalid value beeps and goes back) and the alpha 0-255. Acrylic blur is a check box,
  the two corner radii are number boxes (DIPs).
- **Behaviour**: auto-hide Off / Always / When a window overlaps the dock, hide and reveal delays,
  window previews on / off and their delay.
- **Animation**: speed (Off, then 0.25× to 4× on a logarithmic slider), hide style (slide and
  fade, slide, fade, none), launch feedback (bounce until the app opens, hop once, pulse, none),
  magnification falloff (smooth / linear), easing (ease out, linear, spring), the name label fade
  (0-1000 ms) and reduce motion (follow Windows, on, off; the page shows whether Windows'
  animation effects are on). See [Animation](#animation).
- **Pinned apps**: the pinned list in dock order (name and target; "(missing)" when the file is
  gone), **Add…** (pick one or more `.lnk` / `.exe` files; a shortcut is pinned as the shortcut,
  apps that are already pinned are skipped), **Remove**, **Move up**, **Move down**. Changes from
  the dock itself (drag to reorder, drop to pin, drag off to unpin) show up in the list at once.
- **About**: version, the config file path (click it to show the file in Explorer), **Open config
  file**, **Reload config**, and the renderer in use.

Every change applies to the dock at once (sliders when you release them, number boxes on each
arrow step, on Enter or when the box loses the focus) and is saved within 300 ms, exactly like the
menu toggles. The tray menu and **Reload config** update an open settings window. While the
settings window is the active window the dock stays shown, so you can watch your changes; auto-hide
resumes as soon as another window is activated.

Under every page a **preview strip** draws a small dock from the current settings: the bar with
its border, acrylic tint (over a stand-in wallpaper) or background, corner radius, six placeholder
apps with the three indicator dot states (running, active, attention), the separator and the clock,
the name label above the magnified app, and a sample window-preview popup with a hovered card and
its hovered close button. Move the mouse over it and the magnification follows the pointer, with
the configured magnification, range and falloff. It repaints only when a setting changes or the
mouse moves over it (no timers).

**Theme overrides.** Choosing a theme sets `theme.preset` and clears every override. A theme field
you change on the Appearance page becomes an override: it is saved inline in config.json next to
`"preset"` (only overridden fields are saved) and its **Reset** button appears; Reset copies the
preset's value back and removes the field from config.json. **Reset all to preset** does that for
every field. **Export current theme…** saves all 21 resolved values plus a `name` (the file name)
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
pinned, plus Windows Terminal when it is installed. After editing it by hand, choose **Reload
config** in the dock menu. That applies pins, sizes, magnification, theme, clock, monitor,
auto-hide, `hideTaskbar` and `startWithWindows`. `renderer` needs a restart of the dock. If the file
is malformed, Reload keeps the current settings (and beeps). Pinning, unpinning, and the menu
toggles save the file within 300 ms. The write is atomic (a temp file
replaced with `MoveFileEx`). The dock rewrites the whole file in its own layout. Top-level keys it
does not know are kept, as compact JSON. Unknown keys inside `pinned` entries, `clock`, `theme`
or `animation` are dropped. If the file is malformed, the dock starts with the defaults and leaves
the file untouched.

```json
{
  "pinned": [
    { "path": "C:\\Windows\\explorer.exe" },
    { "aumid": "Microsoft.WindowsTerminal_8wekyb3d8bbwe!App" },
    { "path": "C:\\Users\\me\\Desktop\\Paint.lnk" }
  ],
  "iconSize": 48, "magnification": 1.5, "magnifyRange": 2,
  "autoHide": "always", "hideDelayMs": 400, "revealDelayMs": 150,
  "previews": true, "previewDelayMs": 600,
  "monitor": 0, "hideTaskbar": true, "startWithWindows": false, "renderer": "auto",
  "clock": { "show": true, "format": "%H:%M", "showDate": false },
  "theme": { "preset": "dark",
             "accent": "#FF4CC2FF" },
  "animation": { "speed": 1, "hideStyle": "slide", "launchStyle": "bounce",
                 "magnifyFalloff": "cosine", "easing": "easeOut", "labelFadeMs": 120,
                 "reduceMotion": "system" }
}
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
  seconds tick every second; otherwise the clock wakes once a minute.
- `pinned`: in dock order. Each entry is `{ "path": "<.lnk or .exe>" }` or
  `{ "aumid": "<AppUserModelID>" }`. Environment variables in paths are expanded.
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
sea-green glass shelf in perspective with angled sides, see `shelf`) and `contrast` (High contrast,
black with white borders, square corners). Ids are case-insensitive.

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
| `separator` | `#38FFFFFF` | line between the app icons and the clock |
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
  always, 110 Auto-hide when a window overlaps, 107 open or activate the settings window). Runs it
  as if it was chosen from the menu. Returns 1 for a known command.
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
  filtered ones (cursor, caret, child windows) included.
- `WinDock.OpenSettings` (posted, no parameters): opens or activates the settings window; this is
  what `WinDock.exe --settings` sends to a running dock.
- Settings window (class `WinDock.Settings`, title "WinDock Settings"): the page list is the child
  `WinDock.SettingsNav` (control id 100; its window text is the selected page's name); the pages
  are `WinDock.SettingsPage` children with ids 200 + page index (0 General, 1 Appearance,
  2 Behaviour, 3 Animation, 4 Pinned apps, 5 About), and every control on them has the stable id
  listed in `src/SettingsIds.h`. Check boxes and radio buttons carry no visible text of their own:
  their caption is a separate static (id + 1) that forwards clicks. The Appearance page's theme
  field rows use `3000 + 10 * field index + part` (field index in `Theme::Fields()` order; part 0
  label, 1 swatch / check box, 2 hex box / check box caption, 3 alpha or value box, 4 its up-down,
  5 Reset, 6 unit). The preview strip is the child `WinDock.SettingsPreview` (control id 101).
- `WinDock.ItemCommand`: `wParam` = a per-item command (800 pin/unpin toggle, 801 new window,
  802 open file location, 900 close all windows), `lParam` = item index or a window handle of
  the item. Returns 1 if the item was found.
