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
```

Right-click the tray icon or an empty part of the dock for the menu:

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
  icon under the pointer, falling off with a cosine curve over `magnifyRange` neighbouring slots.
  Magnified icons grow upwards from their bottom edge and push their neighbours outwards. The
  layout is anchored at the pointer, so the icon under the pointer stays under it and the bar
  grows on both sides away from the pointer (like macOS). The bar's width animates with the icons.
  The running-indicator dots keep their size and follow their icons. Everything moves through
  DirectComposition animations (80 ms while the mouse moves, 120 ms back to rest), so no
  surface is redrawn. Magnification updates are capped at about 60 per second.
- **Name label**: the app name appears above the hovered icon after 250 ms, follows it while
  you move along the dock, and disappears when the mouse leaves.
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
  - The popup (class `WinDockPreview`) fades in and out over 120 ms. With acrylic on it uses the
    same blurred backdrop as the bar (with the system's 8 DIP corners), otherwise
    `theme.background`. While it is open and the mouse is still, the dock does no work; DWM draws
    the thumbnails. Thumbnails are released as soon as the popup closes.
- **Auto-hide** (`autoHide`, `"always"` by default): when the mouse leaves the dock, it waits
  `hideDelayMs`, then slides down and fades out (180 ms). The hidden dock is a 1 px tall
  invisible strip along the whole bottom edge of its monitor, so it no longer covers anything
  except that bottom row of pixels. Push the mouse against the bottom edge and keep it there for
  `revealDelayMs` to bring the dock back. Leaving the edge sooner cancels the reveal. Moving
  back onto the dock while it is sliding away reverses the slide. The dock does not hide while
  one of its menus is open.
- The dock is not revealed while a fullscreen app, game, or presentation runs
  (`SHQueryUserNotificationState` reports busy, Direct3D fullscreen, or presentation mode).
- A window that appears or flashes while the dock is hidden does not reveal it. A flashing app's
  dot is orange when you next reveal the dock.
- With `"autoHide": "off"` the dock stays visible. It still floats over windows and reserves no
  screen space.
- `"autoHide": "intelligent"` is meant to hide the dock only while the foreground window overlaps
  it. The overlap test is not implemented yet: for now it behaves like `"always"`.
- At rest the dock window covers exactly the drawn bar, so clicks just above or beside it reach
  the windows underneath. When the mouse enters the bar, the window grows to make room above it
  for magnified icons and the label and beside it for the bar to grow; the cursor is on the bar
  at that moment, so the larger transparent area does not get in the way. Once the mouse has
  left, the label is gone and the icons are back at rest (about 160 ms), it shrinks back to the
  bar. While sliding in or out it covers the bar's column down to the screen edge. The 8 px
  margin below the resting bar belongs to the windows underneath too.
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
  5 s pass.
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
  - A running app that is not pinned can be dragged into the pinned section; dropping it there
    pins it at that position. Dropped anywhere else it goes back to its place. Windows without a
    usable app identity (shown with a class-name fallback) cannot be dragged.
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
  `"intelligent"` (hide only while the foreground window overlaps the dock; for now the same as
  `"always"`). The older `true` / `false` are still read (as `"always"` / `"off"`) and written
  back as strings on the next save. `hideDelayMs` (default 400) and `revealDelayMs` (default 150)
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
acrylic tint), `solid` (Solid, opaque, no acrylic, 12 DIP corners) and `contrast` (High contrast,
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

### Animation

The `animation` object is always written. Its values are read and validated now; they take
effect with the animation work that follows (until then the dock animates as described above,
which matches the defaults except `labelFadeMs`, whose label fade is not implemented yet).

- `speed` (default 1): durations are divided by it, 0.25 - 4. 0 turns animations off.
- `hideStyle`: `"slide"` (default; slide + fade), `"slideOnly"`, `"fade"` or `"none"`.
- `launchStyle`: `"bounce"` (default; repeats until a window appears), `"hop"` (one bounce),
  `"pulse"` (opacity) or `"none"`.
- `magnifyFalloff`: `"cosine"` (default) or `"linear"`.
- `easing`: `"easeOut"` (default, cubic), `"linear"` or `"spring"` (slight overshoot).
- `labelFadeMs` (default 120, 0 - 1000): name label fade in / out; 0 = instant.
- `reduceMotion`: `"system"` (default; follows Settings > Accessibility > Visual effects >
  Animation effects), `"on"` or `"off"`.

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
  always, 110 Auto-hide when a window overlaps). Runs it as if it was chosen from the menu.
  Returns 1 for a known command. 107 (settings window) is reserved and returns 0 for now.
- `WinDock.QueryState`: `wParam` selects the value. 0 = auto-hide state (0 Shown, 1 Hiding,
  2 Hidden, 3 Showing). 1 = current `SHQueryUserNotificationState` value. 2 = hovered item index
  (-1 for none). 3 = 1 if the name label is visible. 4 = 1 while magnified. 5 = number of pending
  interaction timers and running slides (0 when idle). 6 = 1 while the acrylic backdrop is active.
  7 = drag state (0 none, 1 dragging an icon, 2 fading out an icon dragged off the dock, 3 files
  dragged over the dock). 8 = 1 while the dock window has the mouse capture. 9 = insertion index
  of the current drag (-1 for none). 10 = 1 if the OLE drop target is registered. 11 = 1 while
  the window-preview popup is visible. 12 = number of cards in the popup (0 when hidden).
  13 = the window handle shown by popup card `lParam` (0-based; 0 if there is none).
  14 = auto-hide mode (0 off, 1 always, 2 intelligent). 20 = 1 if the theme preset failed to
  resolve on the last load (the dock uses `dark`).
- `WinDock.ItemCommand`: `wParam` = a per-item command (800 pin/unpin toggle, 801 new window,
  802 open file location, 900 close all windows), `lParam` = item index or a window handle of
  the item. Returns 1 if the item was found.
