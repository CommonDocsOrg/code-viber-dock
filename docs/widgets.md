# ViberDock widgets: API reference

A widget is a small JavaScript program that draws into one slot of a dock. It is a folder (or a
built-in shipped inside `WinDock.exe`), loads without compiling, and runs in its own sandboxed
script engine ([QuickJS-ng](../third_party/quickjs-ng/VERSION.md) 0.18, ES2023) on the dock's UI
thread. This file is the full reference; README.md has the short version.

- [Packages](#packages): where widgets live, the folder layout
- [widget.json](#widgetjson): the manifest
- [main.js](#mainjs): the module and its exports, when they are called
- [The host API](#the-host-api): `canvas`, `dock`, `timer`, `console`, `system`, `settings`,
  `storage`, `http`, `shell`, `audio`, `media`, `notifications`, `calendar`, `weather`, `perf`, `network`,
  `input`, `notify`, `links`, `popup`, `widget`
- [Animation](#animation): frames with timers, `dock.visible`, `dock.reduceMotion`, the cost rules
- [Clock faces](#clock-faces): the face formats: analog (`faces/*.json` of the Analog clock widget)
  and digital (the dock's own clock, `clock.face`)
- [Limits and errors](#limits-and-errors): the watchdog, memory, the error tile, reloading
- [Sandbox](#sandbox): what a widget can and cannot reach, permissions
- [Developing a widget](#developing-a-widget): `watch`, the Console, the Settings pages, `examples/`,
  editor help with [`windock.d.ts`](windock.d.ts)
- [Validating a widget](#validating-a-widget): the manifest and syntax check, `WinDock.exe --validate-widget`
- [Publishing a widget](#publishing-a-widget): zip it; what Install from zip… checks; uninstalling
- [Standalone widgets](#standalone-widgets): a widget in a window of its own on the desktop
- [Preview mode](#preview-mode): the widget drawer's live previews, `dock.preview`, the sample data,
  the manifest's `preview`
- [Adding a widget to a dock](#adding-a-widget-to-a-dock)
- [Examples](#examples)

## Packages

```
%LocalAppData%\WinDock\widgets\
  <id>\                 the widget's id: 1 - 64 of a-z 0-9 - _ . (not starting with .)
    widget.json         the manifest (required)
    main.js             the ES module the dock loads (the manifest's "main" can name another)
    lib\util.js         any modules main.js imports (relative imports)
    assets\icon.png     images for canvas.image()
```

Built-in widgets (`hello`, `digital-clock`, `analog-clock`, `analog-date`, `volume`, `media`,
`start`, `search`, `notification-bell`, `agenda`, `weather`, `notes`, `system-monitor`,
`system-overview`, `battery`, `show-desktop`, `stocks`, `photo-frame`, `network`, `input-language`,
`timer`, `world-clock`, `todo`, `moon`, `quick-buttons`, `feed`) are compiled into the
exe as resources (their sources are in the repository's `widgets\` folder). A folder with the same id as a built-in replaces it:
the folder wins, so copying a built-in's files into `widgets\<id>\` and editing them changes the
widget. Round 25: such a folder takes over the built-in's places, settings and `widget-data` file,
so it runs only after the user allowed it (see [Consent](#consent)), even when it asks for no
permissions; **Export built-in to folder** makes a copy that is allowed at once. Widgets are always
loaded as source; bytecode is never loaded.

## widget.json

```json
{
  "name": "Hello",
  "version": "1.0.0",
  "author": "ViberDock",
  "description": "A label and a click counter.",
  "main": "main.js",
  "size": {
    "horizontal": { "main": 64, "cross": 40 },
    "vertical": { "main": 44, "cross": 48 }
  },
  "refreshMs": 0,
  "timeLimitMs": 50,
  "memoryLimitMB": 32,
  "permissions": [],
  "settings": [
    { "key": "label", "type": "string", "label": "Label", "default": "Hello" }
  ]
}
```

| Key | Default | Meaning |
|---|---|---|
| `name` | the id | Shown in Settings (the Contents tab's strip and widget drawer, the Widget library). |
| `api` | 1 | The widget API version it was written for (informational; 1 is the only one so far). |
| `version`, `author`, `description` | "" | Informational. |
| `main` | `"main.js"` | The module to load, a path inside the package. |
| `size.horizontal`, `size.vertical` | 64 x 40, 48 x 48 | The box in DIPs (pixels at 100 % scale) on a horizontal (bottom / top) and a vertical (left / right) dock: `main` along the dock, `cross` across it. Used unless the module exports `measure()`. Clamped to 4 - 1024 (main) and 4 - 512 (cross). |
| `refreshMs` | 0 | Above 0: the dock calls `tick()` every `refreshMs` ms (at least 100), aligned to multiples of it in local time (60000 = on the minute), then redraws (only while the widget can be seen, see `timer.setRefresh`). 0: no ticking. `timer.setRefresh(ms)` overrides it at run time (also for another time zone: `timer.setRefresh(ms, { timeZone })`). |
| `timeLimitMs` | 50 | The watchdog: the longest one call into the script may run (10 - 1000; loading the module gets four times as long, at least 200 ms). |
| `memoryLimitMB` | 32 | The script heap (4 - 256 MB). |
| `permissions` | [] | What the widget may do beyond drawing (see [Sandbox](#sandbox)): `"shell"` (open files / URLs, list folders, file icons, the folder popup, file drops, `shell.sendKeys`), `"audio"` (the [audio](#audio) provider), `"media"` (the [media](#media) provider), `"notifications"` (the [notifications](#notifications) provider), `"calendar"` (the [calendar](#calendar) provider), `"weather"` (the [weather](#weather) provider), `"perf"` (the [perf](#perf) provider: performance counters and HWiNFO's sensors), `"network"` (round 27: the [network](#network) provider), `"input"` (round 27: the [input](#input) provider: the keyboard language), `"notify"` (round 28: [notify](#notify), a banner on screen), `"links"` (round 29: [links](#links), a web page in the browser on a click), `{ "http": ["api.example.com", "*.example.org"] }` (also written `"http:api.example.com"`: requests to those hosts only; `*.example.org` is any sub-domain of it, not example.org itself; an IP address is matched only by an entry that is that address), and in that object (round 29) `"hostSettings": ["feedUrl"]`: `url` settings whose host the user may allow per item ([http](#http), user hosts; `"http": []` with it: only the user's hosts). An unknown entry is an error: the widget does not load (see [Validating a widget](#validating-a-widget)). Round 25: a widget from the widgets folder runs only after the user allowed its permissions (see [Consent](#consent)); ask for the fewest you need. |
| `watch` | false | Folder widgets: reload when a file of the widget's folder changes (see [Developing a widget](#developing-a-widget)). |
| `preview` | none | Round 6 phase 6E: the widget's live preview in the settings window's widget drawer and Widget library ([Preview mode](#preview-mode)): `{ "settings": { "face": "classic" }, "width": 64, "height": 48 }`. `settings`: an object of setting values merged over the schema's defaults for the preview only (a nice face, a sample label); `width` / `height`: the preview's box in DIPs (8 - 1024 / 8 - 512; left out: the box the widget measures on a 48 DIP bottom dock). The manifest check reports a non-object, a setting the schema does not name (a warning) and sizes out of range. |
| `standalone` | none | How the widget behaves as a [standalone widget](#standalone-widgets) (a window of its own on the desktop): `{ "minWidth": 96, "minHeight": 96, "aspect": "keep", "defaultWidth": 200, "defaultHeight": 200 }`. `minWidth` / `minHeight`: the smallest window in DIPs (16 - 4000; default 48, and never below 48); `aspect`: `"free"` (default: any shape), `"keep"` (resizing keeps the width : height of the window as it was made or last set in its settings) or a number (width / height, e.g. `1` for a square); `defaultWidth` / `defaultHeight`: the size **Add to the desktop** gives a new one (default: 200 x 200 when the horizontal size hint is about square, else twice the horizontal hint; never below the minimum); `supported`: `false` when the widget needs a dock (round 16 phase 16E; default `true`): it is never made a standalone widget (dragging it off the dock only reorders it, the drawer does not drop it on the desktop, **Add to the desktop** refuses it). |
| `settings` | [] | The widget's settings schema, see below. |

### Settings schema

Each entry: `key` (required), `type`, `label`, `description`, `default`, the page's layout keys
`section`, `group`, `unit`, `visibleIf`, `common` and `quick` (below), and per type:

| `type` | Value | Extra keys |
|---|---|---|
| `bool` | true / false | |
| `int` | a whole number | `min`, `max`, `step` |
| `number` | a number | `min`, `max`, `step` |
| `string` | text | |
| `choice` | one of `choices` | `choices`: `["a", "b"]` or `[{ "value": "a", "label": "A" }]`; `choiceFrom`: a glob of package files, or `"perf"` (below) |
| `colour` (`color`) | `"#AARRGGBB"` / `"#RRGGBB"` | |
| `font` | a font family name | |
| `place` | a place (round 5): no key of its own; it edits the item's `latitude`, `longitude` (numbers, 4 decimals) and `location` (the name shown) | none (no `default`) |
| `timezone` | a Windows time zone key name (`"Tokyo Standard Time"`); `""` = the local time | none |
| `file` | an absolute path to a file (`""` = none; round 17 phase 17A) | `filter`: the picker's patterns, `"*.png;*.jpg"` (left out: every file) |
| `folder` | an absolute path to a folder (`""` = none; round 17 phase 17A) | |
| `secret` | text the user keeps private: an API key, a token, a password (round 29) | none: no `default` (it would be in the manifest for everyone), never `common` or `quick` |
| `url` | `""` or an `https://` address; `http://` only for a device on the local network; never this PC (round 29) | none (a `default` must be a valid url value, else `""`); name it in `hostSettings` to let the user's host be reached |

`secret` (round 29): the settings page shows a password box (dots) with **Show** / **Hide** beside
it. The value is stored in config.json **protected with Windows' DPAPI for the current user**
(`"apiKey": "dpapi:AQAAANCMnd8B..."`), so the file, its backups and copies of it do not give the
value away on another account or machine; **Save layout** and dock exports leave secret values out
altogether. The script gets the plain value in `settings`. The widget's log and its error tile never
show it: a line that contains the value (4 characters or more) shows `[secret]` in its place. A
plain value found in config.json for a secret field (a widget updated to make a field secret, or a
value typed into the file) is protected the next time the dock starts or reloads its config. If
Windows refuses to protect a value, it is not saved at all and the dock says so once. What it does
not do: hide the value from other programs running as the same Windows user (they could ask DPAPI
too), or from the widget itself. The built-in Stocks widget's **API key** is a secret since 1.1.0.

`url` (round 29): an edit box; what the user types is checked when the box loses the focus (a
message says why an address is refused, and the box goes back to the value before): `https://`
only, unless the host names a device on the local network (a private address, `.local`, `.lan`,
`.home`, `.home.arpa`, `.internal`, `.localdomain` or one label: then `http://` works too); never
`localhost` or a loopback address; no user name or password in it; at most 2048 characters. A
`url` field named in the manifest's `hostSettings` ([http](#http)) asks, right after the user types
a new host, `"<Widget> wants to connect to <host>"` with **Allow** / **Don't allow** (the default);
the address is kept either way, and the widget can reach the host only after Allow. Like `secret`,
a `url` cannot be `common` (the Contents inspector would skip the check and the prompt).

`file` / `folder` (round 17 phase 17A): the settings page shows the path in a box (typed or pasted
paths work, `%VAR%` included) with a **Browse…** button that opens the Windows file or folder
picker (started in the current value's folder when it exists). The value is any string: the widget
checks the path when it uses it (`shell.list`, `canvas.image` with the `shell` permission). The
Contents inspector edits a `common` one as a plain box.

`place` (round 5): the settings page shows the weather place block for it: **Find a place** (round
8 phase 8C: an autocomplete box; typing searches Open-Meteo's geocoding service after a short pause
or on Enter, the places found fill its list, choosing one sets the place), the place as one line
("Colorado Springs · 38.83, −104.82"), **Edit coordinates** (an expander with the name shown,
latitude and longitude), the status of that place's forecast service with **Refresh now**. Its value
is not a key of its own: the script sees `settings.latitude`, `settings.longitude` and
`settings.location` (only when the item has a valid latitude and longitude). Only the first
`place` field of a schema is shown; it takes no `default`, `min`, `max`, `step`, `choices`,
`choiceFrom` or `unit` (the manifest check warns). A widget with the `weather` permission and a
`place` field gets its own forecast from the `weather` provider once its item has a place (see
weather below).

`timezone` (round 5): a drop-down with "Local time" and then every time zone of this Windows by its
display name, sorted by UTC offset; the value is the zone's key name (`""` for local). A `default`
that names no zone of this machine is warned about (the local time is used). The script passes the
value to the `system` time calls and to `timer.setRefresh` (see [system](#system)); the Digital
clock (script), Analog clock and Calendar page widgets have a `timeZone` field of this type.

`choiceFrom` (choice fields): `"faces/*.json"` adds one choice per package file matching the glob
(`*` and `?` stay inside one folder level), after any fixed `choices`: the value is the file name
without its folder and extension (`classic`), the label the file's JSON `"name"` (else the value).
The list is read whenever the package is loaded (a widget reload, opening its settings page), so a
file added to an exported widget's folder shows up after **Reload widgets**. A default that names no
file falls back to the first one.

`"choiceFrom": "perf"` (round 6 phase 6D): the choices are the metrics of the [perf](#perf)
provider, listed by the host when the settings page is built: every key of `perf.metrics()` by its
name, the built-in ones first (CPU, memory, GPU, disks, network, battery, uptime), then HWiNFO's
readings as "HWiNFO: <sensor> · <reading>" (while HWiNFO shares them). The value is the metric's key
(`"cpu.total"`, `"hwinfo.3.16777216"`). Any key is accepted and kept: a stored key that is not
available now (HWiNFO closed) is listed last as "<key> (not available now)" and stays selected, so
opening the page never changes it. The list comes from the running sampler, or, when no widget
uses `perf` at that moment, from a short sample taken for the page (kept 30 s). Give the field a
`default` key (the manifest check warns without one). The settings page needs no permission for
this; the widget needs `"perf"` to read the values.

The values live in the dock item's `settings` object in config.json. The script sees the schema's
defaults overridden by the item's values (a value of the wrong type or out of range falls back to
the default; ints are rounded, numbers clamped); keys the schema does not name are passed through
unchanged. Each widget item has its own settings window (an Item window, round 5), generated from
the schema: **Widget settings…** in the widget's own right-click menu, or Dock settings >
Contents > **More settings…** in the inspector of the widget's tile (a double click or Enter on it
does the same; round 10 phase 10B: the inspector itself edits the fields marked `common`, below). It has a check box per `bool`, an up-down (stepper, `step` per click, `min` -
`max`) per `int`, a drop-down per `choice` and `timezone`, the place block for a `place`, a colour
swatch (round 16 phase 16D: a click opens the Windows colour dialog, as the theme pages' swatches
do; the RGB is replaced, the alpha kept, and a fully transparent value such as `#00000000` "the
theme's colour" becomes opaque) beside an `#AARRGGBB` box for a `colour` (the Contents inspector
shows the same pair for a `common` colour), an edit box for the others, and **Reset to defaults** in its footer (removes the
item's `settings`). A change is saved and reaches the widget at once (`onSettingsChanged`). Round 8
phase 8C: the manifest's `description` is the window header's third line (one line; its tooltip
has all of it); a field's `description` is a one-line hint under its label (a check box: under its
caption) when it fits that line at the window's default width (600 DIPs), else its first sentence
(up to the first ". ") is the hint; the whole description is always the tooltip of the label, the
hint and the control. So write a first sentence that stands on its own and keep it short (about 30
characters fit under a label, about 60 under a check box). Round 10 phase 10C: when the schema has
fields of both sections (`section`, below) the window shows them on two tabs, **Content** (what the
widget shows and does: places, feeds, metrics and data choices, time zones, labels, click and wheel
actions, refresh intervals) and **Appearance** (how it looks: styles, faces, colours, fonts, sizes,
motion, the layout of its slot); a schema whose fields are all of one section gets one page and no
tab strip. Reset to defaults and Console… stay in the footer under both tabs. A
[standalone widget](#standalone-widgets) shows the same pages as the **Content** (and
**Appearance**) tabs of its window, before its **Placement** tab (with the description as the
Content tab's first line, and Reset to defaults / Console… at the end of each).

The page's layout keys (round 4 phase 14):

| Key | Example | Effect |
|---|---|---|
| `section` | `"section": "appearance"` | Round 10 phase 10C: the tab the field is on, `"content"` or `"appearance"` (any other value, or not a string: an error). Without it the field goes to **Appearance** when its `type` is `colour` or its `group` is one of Colours, Colors, Look, Face, Size, Slot, Display, Motion, Motion and size (case ignored), else to **Content**, so a schema written before the key keeps working. Groups stay the cards inside a tab (a group whose fields are on both tabs gets a card on each). `common` (the Contents inspector) does not depend on it. Every built-in widget sets it on every field. |
| `group` | `"group": "Second hand"` | Round 8 phase 8C: a card titled with the group around the field and the ones after it with the same group; a field whose group differs from the field above it starts a new card (so fields of one group should be next to each other: a group that comes back later gets a second card). Fields without a group: a card titled with the widget's `name`. |
| `unit` | `"unit": "DIPs"` | A unit after an `int`'s stepper or a `number`'s edit box ("%", "minutes"). |
| `visibleIf` | `"visibleIf": { "seconds": true }` | The field (and a group header that would be left empty) is shown only while every named field has that value; the page is generated again when one of them changes. Round 23: an array means any of its values (`"visibleIf": { "layout": ["deck", "cover"] }`). A key that names no other field is an error. |
| `common` | `"common": true` | Round 10 phase 10B: one of the widget's few everyday settings. Dock settings > Contents shows the fields marked `common` in the inspector of the selected widget, editable in place (in schema order, at most six, each only while its `visibleIf` holds), above **More settings…**, which opens the whole page. Without any `common` field the inspector shows the first three fields of a simple type (`bool`, `int`, `number`, `string`, a `choice` not from `"perf"`). The inspector edits `bool`, `int`, `number`, `string`, `colour`, `font`, `choice` and `timezone` fields; a `place` shows as a read-only line, a `"choiceFrom": "perf"` choice is left to the page. Not a boolean: an error. Mark two to four fields. |
| `quick` | `"quick": true` | Round 13 phase 13C: the field is a quick setting in the widget's right-click menu, between its own entries and **Widget settings…**. A `bool` is one checkable entry (its `label`, checked while the value is true; a click toggles it); a `choice` is a submenu titled with its `label`, holding one radio entry per choice (the current value checked; a click chooses it). Fields are listed in schema order. Only `bool` and `choice` fields can be quick: on another type the key is ignored and the manifest check warns. At most 8 quick fields per widget: the ones after the eighth are left out of the menu, with a warning. Not a boolean: an error. A change from the menu is written and applied as the settings window does (saved, `onSettingsChanged`, one Undo step named after the field and the widget, "Changed Units of Weather"). A `"choiceFrom": "perf"` choice lists only its fixed `choices` there. |

```json
{ "key": "seconds", "type": "bool", "label": "Second hand", "default": false, "group": "Second hand", "section": "appearance" },
{ "key": "secondsMotion", "type": "choice", "label": "Second hand motion", "group": "Second hand", "section": "appearance",
  "visibleIf": { "seconds": true }, "choices": ["ticking", "smooth"], "default": "ticking" },
{ "key": "size", "type": "int", "label": "Size", "min": 0, "max": 128, "default": 0, "unit": "DIPs", "section": "appearance" },
{ "key": "timeZone", "type": "timezone", "label": "Time zone", "default": "", "group": "Time zone", "section": "content" }
```

## main.js

An ES module. The dock imports it once, evaluates its top-level code (with the whole host API
available), then calls its exports:

| Export | Called | Arguments | Returns |
|---|---|---|---|
| `draw(canvas, dock)` | **required.** When the widget asked for a redraw (`requestRedraw()`, a tick), when its settings changed, and when the dock lays it out again (its size, scale, theme or edge changed). Never per frame while idle. | the canvas, the dock | |
| `measure(canvas, dock)` | Before the dock lays the widget out (and after every redraw request). Only `canvas.measureText` works here. | | `{ main, cross }` in DIPs; without this export the manifest's size hint for the dock's axis is used. `main` 0: the widget takes no room on the dock (it is not drawn; the Battery widget without a battery). A box thicker than the bar stands on the bar's near (screen-edge) side and sticks out on the far side only, like a magnified icon (round 7; thinner boxes are centred on the icon row). A changed size lays the dock out again at once: a widget can grow and shrink by frames (the Analog clock's hover growth). Round 11 phase 11C: `flush: true` in the result asks for the Windows 7 "Show desktop" placement: on a full-width dock the box is the bar's whole thickness (`cross` is ignored; `canvas.height` / `dock.cross` are that thickness in `draw()`) and, as the dock's first or last item, it takes no end padding, so it touches the screen's end; anywhere else (a centred, floating or ring dock, standalone, previews) it is ignored |
| `tick(dock)` | Every `refreshMs` (see the manifest, `timer.setRefresh`), only while the widget can be seen (round 26: one catch-up tick when it is seen again). | the dock | `false` to skip the redraw that follows |
| `onPress(e)` | The left button went down on the widget. | `{ x, y, button }`: DIPs from the box's top-left | Round 23: `true` **claims** the press: the widget follows the held button (`onDrag`, `onRelease`); the dock starts no item drag (nor a Ctrl + drag of the dock, nor a scroll of its run), a standalone window does not move, and no `onClick` follows. Anything else (nothing): the press goes on as before. Claim only presses on the widget's own controls (a slider, a seek bar, a button that acts on release), so the rest of it can still be dragged |
| `onDrag(e)` | Round 23: the pointer moved while a press this widget claimed is held, every move (no drag threshold), also outside its box (the dock / the window holds the mouse capture). Never called otherwise: no pointer-move events reach a widget outside a claimed press. | `{ x, y, button }`: DIPs from the box's top-left (may be outside it, negative) | |
| `onRelease(e)` | Round 23: a claimed press ended: the button went up, or the capture was lost (a menu, Alt+Tab, the window went away: `cancelled` true; do not act then). Round 25: the dock's slots changing during the press (a window opened or closed) no longer ends it: `onDrag` and `onRelease` keep coming to this widget; when its slot is gone from the dock it gets `cancelled` true with `x` and `y` 0. | `{ x, y, button, cancelled }` | |
| `onClick(e)` | A left click on the widget (press and release on it). | `{ x, y, button, closedPopup }`: `closedPopup` is true when that press closed this widget's popup (so a click toggles it: `if (!e.closedPopup) popup.open(...)`) | |
| `onMiddleClick(e)` | A middle click. | `{ x, y, button }` | |
| `onWheel(e)` | The mouse wheel over the widget. | `{ x, y, delta, notches, horizontal }`: `delta` in WHEEL_DELTA units (120 per notch; positive: away from the user) | `true` if the widget used it |
| `onHover(e)` | The pointer entered / left the widget. | `{ x, y, inside }` | |
| `onDrop(e)` | Files were dropped on the widget (only with the `shell` permission and this export; the dock then shows the drop as allowed over the whole widget instead of pinning). | `{ x, y, files: [paths] }` | |
| `onSettingsChanged(settings)` | The item's settings changed (Settings, config.json reload). The global `settings` is the new object already. | the settings | |
| `drawPopup(canvas, dock)` | The widget's popup (`popup.open`) is shown or redrawn (`popup.redraw()`). The canvas is the popup's (`canvas.width` / `height`: its size in DIPs), over the theme's panel. It draws both kinds, the ordinary popup and a passive one: the module tells them apart by what it opened (e.g. `canvas.width`). | the canvas, the dock | |
| `onPopupClick(e)` | A click in the popup (never in a passive one). | `{ x, y, button }`: DIPs from the popup's top-left; `button` `"left"`, `"middle"`, `"right"` | |
| `onPopupClose(e)` | The popup closed (Escape, another window came forward, a click on the dock, `popup.close()`, a reload; a passive one: see `popup.open`). | `{ passive }`: true when the passive popup closed | |
| `onVisibilityChange(e)` | The dock went off screen (auto-hide hid it, a desktop dock lost the desktop; round 26: also the full-screen guard hid it, the session was locked, the display went off) or came back. Stop animations while hidden (see [Animation](#animation)). | `{ visible }` | |

Every export also gets `dock` as its last argument; it is the same object as the global `dock`.
Nothing redraws by itself after an event: call `requestRedraw()` when the content changed. A right
click on a widget opens the dock menu. The dock owns the widget across item-list changes (pins
added, other widgets moved): its module stays loaded and keeps its state until it is reloaded or
removed.

## The host API

All of it is available as globals; `canvas` and `dock` are also passed to the exports.

### canvas

Coordinates are DIPs from the top-left of the widget's box. The box is upright on every edge (on a
vertical dock it is `cross` wide and `main` tall); the canvas scales everything by the dock's DPI
scale. Drawing calls draw straight into the slot's surface through Direct2D (no intermediate
bitmap) and only work inside `draw()`; in `measure()` they do nothing, and outside both they throw.
The surface starts transparent; drawing is clipped to the box.

Paint (`paint` below): a colour string `"#RGB"`, `"#RRGGBB"`, `"#AARRGGBB"` (alpha first, as in
config.json), `"rgb(r, g, b)"`, `"rgba(r, g, b, a)"` (a 0 - 1), `"transparent"`, a number
`0xAARRGGBB`, or a gradient object (below). Omitted: the theme's text colour (`dock.theme.text`).

Stroke (`stroke` below): a width in DIPs (default 1), or `{ width, cap: "butt" | "round" |
"square", join: "miter" | "round" | "bevel", dash: [on, off, ...] }` (dash lengths in DIPs).

| Call | |
|---|---|
| `canvas.width`, `canvas.height`, `canvas.scale` | The box in DIPs; px per DIP. `canvas.drawing`: false in `measure()`. |
| `fillRect(x, y, w, h, paint)`, `strokeRect(x, y, w, h, paint, stroke)` | |
| `fillRoundRect(x, y, w, h, r, paint)`, `strokeRoundRect(x, y, w, h, r, paint, stroke)` | |
| `fillEllipse(cx, cy, rx, ry, paint)`, `strokeEllipse(cx, cy, rx, ry, paint, stroke)` | |
| `fillCircle(cx, cy, r, paint)`, `strokeCircle(cx, cy, r, paint, stroke)` | |
| `line(x0, y0, x1, y1, paint, stroke)` | |
| `path()` | A new path: `moveTo(x, y)`, `lineTo(x, y)`, `quadTo(cx, cy, x, y)`, `bezierTo(c1x, c1y, c2x, c2y, x, y)`, `arc(cx, cy, r, start, end, counterClockwise)` (radians, 0 = +x, clockwise as y points down; a line joins the current point to the arc's start), `rect(x, y, w, h)`, `close()`. Each returns the path, so calls chain. A path can be kept and drawn again; its geometry is built once. |
| `fill(path, paint, rule)` | `rule`: `"nonzero"` (default) or `"evenodd"`. |
| `stroke(path, paint, stroke)` | |
| `text(str, x, y, style)` | DirectWrite text. `style`: `{ font: "Segoe UI", size: 12, weight: 400 \| "thin" \| "light" \| "normal" \| "medium" \| "semibold" \| "bold" \| "black", italic: false, align: "left" \| "center" \| "right", baseline: "top" \| "middle" \| "bottom" \| "alphabetic", maxWidth: 0, color: paint }`. `x` is the left / centre / right of the text per `align`, `y` its top / middle / bottom / baseline. `"\n"` breaks lines; `maxWidth` > 0 wraps the text at that width (and `align` places the lines inside `[x - ..., x + maxWidth]` the same way). |
| `measureText(str, style)` | `{ width, height, baseline, lines }` in DIPs (also in `measure()`). |
| `image(path, x, y, w, h, opacity)` | An image file of the package (`"assets/icon.png"`; PNG, JPEG, BMP, GIF, ICO, TIFF via WIC; up to 8 MB and 4096 x 4096). `w` / `h` omitted or 0: its pixel size in DIPs. Decoded once and cached. Throws if it cannot be read. Round 17 phase 17A: with the `shell` permission also **a file anywhere on disk** by its absolute path (`"C:\\Users\\me\\Pictures\\a.jpg"`, a UNC path, or one starting with `%VAR%`; also WebP, HEIC and AVIF where Windows has their codecs): up to 64 MB, decoded at most 2048 px on its long side (larger ones are scaled down while decoding) after its EXIF orientation, so a phone photo stands upright. The decoded photos are kept within 64 MB per widget, the least recently used going first: a slideshow can decode its next picture ahead with `imageSize` (also in `measure()`) and change to it without a pause. |
| `imageSize(path)` | `{ width, height }` in pixels (a file from disk: as decoded, at most 2048 on the long side, after its orientation). |
| `fileIcon(path, x, y, size)` | The shell's icon of a file or folder (an absolute path; `%VAR%` expanded), `size` DIPs square. Needs the `shell` permission. Cached like images. |
| `save()`, `restore()` | The transform, opacity and clips (up to 64 levels). Whatever is left pushed at the end of `draw()` is popped. |
| `translate(x, y)`, `rotate(radians, cx, cy)`, `scaleBy(sx, sy)` | Change the transform of what follows (`rotate`: clockwise for positive angles, about (cx, cy)). The scale call is `scaleBy` because `canvas.scale` is the DPI property (before 5a the method was listed as `scale(sx, sy)`, which the property shadowed). |
| `opacity(a)` | Multiplies the opacity of everything drawn from now on (0 - 1). |
| `clip(x, y, w, h)` | Clips to a rectangle (in the current transform; rotated: its bounds). |
| `clipPath(path)` | Clips to a path (anti-aliased). |
| `layer(id, x, y, w, h, options)` | Round 16 phase 16F: starts a **composition layer**, a child visual of the widget's slot with a surface of its own: the drawing calls up to `endLayer()` draw its content (its own coordinates, `w` x `h` DIPs, clipped to the box like the slot) and the layer sits at (`x`, `y`) in the slot (or in its `parent` layer). `options`: `parent` (the id of a layer declared earlier in this draw: a group), `clip` (clip the layer and its children to its box), `opacity` (0 - 1, also over its children), `tracks` (up to 32 loops DirectComposition runs on the visual, see [Animation](#animation): `{ prop: "x" \| "y" \| "rotate" \| "opacity", duration (seconds, > 0), begin (seconds: the loop's phase), once (true: ends at the last key), keys: [[seconds, value], ...] (2 - 64, in order, within the duration; x / y in DIPs, rotate in degrees about center: [cx, cy]), easing: [[x1, y1, x2, y2], ...] (a cubic bezier per key span, as CSS; left out: linear) }`). Returns **true** when the layer is live (the dock's slot or a standalone widget, no reduce motion) and **false** when the host drew the content inline instead (a popup, a preview, `measure()`, reduce motion): at the tracks' first keys, inside the parent's box and opacity. The same `id` across redraws keeps the layer and, with unchanged options, its running loops; layers not declared again are removed. Layers do not nest: `endLayer()` first, then declare the child with `parent`. |
| `endLayer()` | Ends the open layer (throws without one). `draw()` returning with one open ends it. |
| `linearGradient(x0, y0, x1, y1, stops)` | A paint. `stops`: `[[offset, colour], ...]` (1 - 64 stops, offsets 0 - 1). |
| `radialGradient(cx, cy, rx, ry, stops, fx, fy)` | A paint centred on (cx, cy) with radii rx / ry (ry defaults to rx); (fx, fy) moves the gradient's origin from the centre. |

### dock

Read-only, always current:

| Property | |
|---|---|
| `axis` | `"horizontal"` (bottom / top docks) or `"vertical"` (left / right). |
| `edge` | `"bottom"`, `"top"`, `"left"`, `"right"`. |
| `width`, `height` | The widget's box in DIPs (upright), as drawn. |
| `main`, `cross` | The same box along / across the dock. |
| `scale` | Physical pixels per DIP (1.25 at 125 %). |
| `iconSize` | The dock's icon size in DIPs. |
| `hovered` | The pointer is over this widget. |
| `magnified` | The dock is magnifying (the pointer is on the dock). |
| `reduceMotion` | Reduce motion is in effect (`animation.reduceMotion`, or Windows' animation effects are off). |
| `locked` | The dock is locked. |
| `visible` | The dock is on screen: shown or sliding in, not auto-hidden or hiding, not a desktop dock hidden behind another window, not hidden by the full-screen guard, and (round 26) the session is not locked, disconnected or on a display that is off. `onVisibilityChange` reports changes. While it is false (and no popup of the widget is open) `tick()` does not run and redraws wait for the show. |
| `id`, `widgetId` | The dock's id (`docks[].id`; a standalone widget: its entry's id), the widget's id. |
| `itemKey` | Round 6: a key for this item, for per-item data in `storage` (the widget's storage is shared by every item of the widget). On a dock `"<dock id>/<widget id>#<n>"`: the n-th item of this widget on that dock (`"main/notes#0"`), so it stays the same when the item is moved past other items, and changes when it is moved past another item of the same widget or when one of those before it is removed (the items then trade keys: the data follows the position among them). A standalone widget: its entry's id (`"note1"`), which never changes. A widget that must keep its data across such moves can offer a setting for the key as well (the Notes widget's **Note id**). |
| `standalone` | `true` for a [standalone widget](#standalone-widgets) (a window of its own on the desktop): `width` / `height` (and `main` / `cross`) are the window's client size, `iconSize` is the shorter side / 1.5, `axis` is `"horizontal"`, `edge` `"bottom"`, `magnified` and `clockDate` are false, `locked` is the entry's lock, `visible` false while a "desktop" layer hides it (also while the full-screen guard hides it and, round 26, while the session is paused). |
| `preview` | Round 6 phase 6E: `true` while the widget draws its live preview in a settings window ([Preview mode](#preview-mode)): drawn once on a virtual dock, every side effect a no-op, the providers answering with sample data. A widget with content of its own (a note, a list) can show a sample instead (the Notes widget's "Shopping list"). |
| `clockDate` | A built-in clock item of the dock draws a second line (its date or, since round 5, its label): the dock's own widgets then use their smaller size (the weather element: a 24 instead of 26 DIP glyph, 14 instead of 16 DIP text). |
| `theme` | The dock's theme colours as `"#AARRGGBB"`: `text`, `dateText`, `accent`, `background`, `labelBackground`, `labelText`, `separator`, `border`. |
| `requestRedraw()` | Same as the global. |

### timer and redraws

| Call | |
|---|---|
| `requestRedraw()` (also `timer.requestRedraw()`, `dock.requestRedraw()`) | Redraw (and re-measure) the widget soon: once, after the current call returns, however often it is called. Round 26: asked while the widget cannot be seen (see `timer.setRefresh`), the redraw waits and happens once when it is seen again. |
| `setTimeout(fn, ms)`, `setInterval(fn, ms)` (also `timer.setTimeout` / `timer.setInterval`) | Returns an id. One shared host scheduler runs every widget's timers (one Windows timer for the earliest deadline; none while nothing waits). Intervals are at least 16 ms. At most 64 timers per widget (more throws a RangeError). No extra arguments are passed to `fn`. They keep running while the widget cannot be seen (round 26: only the refresh pauses; stop frame timers yourself in `onVisibilityChange`). |
| `clearTimeout(id)`, `clearInterval(id)`, `timer.clear(id)` | |
| `timer.setRefresh(ms[, { timeZone }])` | `tick()` every `ms` (at least 100, at most 86400000) aligned to local time, replacing the manifest's `refreshMs`; 0 stops it. A clock that shows seconds calls `timer.setRefresh(1000)`, one without `60000`; `86400000` wakes at local midnight. With `timeZone` (round 5: a Windows time zone key name, see [system](#system); the key alone as the second argument works too) the multiples are of that zone's time of day: `timer.setRefresh(86400000, { timeZone: 'Tokyo Standard Time' })` wakes at Tokyo's midnight, `3600000` on its hours (zones 5:45 or 12:45 from UTC included). Every Windows zone is a whole number of minutes from UTC, so periods up to a minute fall on the same instants in every zone. When the system time or time zone changes, or the machine resumes from sleep, the refresh is re-aligned and `tick()` runs at once; a daylight-saving change of the zone itself is picked up at the next wake-up (each wake-up aligns the next one again). Round 26: while the widget cannot be seen (`dock.visible` false and no popup of it open: an auto-hidden dock, a desktop dock behind another window, the full-screen guard, a locked session or a display that is off) the refresh does not run at all; when it is seen again and a boundary passed meanwhile, `tick()` runs once at once, then the aligned schedule continues (shown again before the next boundary: nothing extra runs). |

Prefer `setRefresh` / `refreshMs` for anything tied to the clock (it wakes at the boundary itself)
and redraw only what changed: a widget that does nothing costs no CPU at all.

### console

`console.log`, `console.info`, `console.debug`, `console.warn`, `console.error`: the arguments,
joined with spaces (objects as JSON, errors with their stack), go to the widget's log (the last 500
lines; also sent to the debugger output as `WinDock widget <id>#<n> [log] ...`).

### system

Computed by the host in the user's locale (QuickJS's own `Date` applies the current daylight-saving
offset to every date on Windows: use these for local time). Since round 5 the time calls take an
optional Windows time zone key name; a clock in another zone:

```js
const zone = () => settings.timeZone || '';        // a "timezone" setting ("" = local)
timer.setRefresh(60000, { timeZone: zone() });      // on the minute (the same instant in every zone)
export function draw(canvas, dock) {
  const t = system.localTime(zone());               // { hour, minute, ..., utcOffset, timeZone }
  canvas.text(system.format('%H:%M', zone()), canvas.width / 2, 0, { align: 'center', color: dock.theme.text });
}
```

| Call | |
|---|---|
| `system.now()` | `{ year, month (1 - 12), day, hour, minute, second, millisecond, weekday (0 = Sunday), yearDay, dst, utcOffset (minutes east of UTC), timeZone }` (`timeZone` `""`: the local time). |
| `system.localTime(timeZone)` | Round 5: the same object for the time now in a Windows time zone given by its key name (`"Tokyo Standard Time"`, `"GMT Standard Time"`: what a `timezone` setting holds; any case), converted by Windows with the zone's own daylight-saving rules (`SystemTimeToTzSpecificLocalTimeEx`): `utcOffset` is the zone's offset now, `dst` whether its daylight time is in effect, `timeZone` the key as Windows spells it. `""`, `null`, no argument or a key this Windows does not know: the local time with `timeZone` `""` (so a script can tell an unknown zone apart). |
| `system.timeZones()` | Round 5: every zone of this Windows as `[{ key, name, standardOffset }]`, sorted as Windows lists them (by UTC offset, then name): `name` the display name (`"(UTC+09:00) Osaka, Sapporo, Tokyo"`), `standardOffset` minutes east of UTC without daylight saving (the offset now is `system.localTime(key).utcOffset`). About 140 entries, read from the registry on each call: call it once. |
| `system.format(fmt[, timeZone])` | The time now (local, or in `timeZone`) through strftime (`wcsftime`, as `clock.format`): `"%H:%M"`, `"%a %d %b"`, `"%#I:%M %p"`. A bad specifier falls back to `"%H:%M"`. `%Z` / `%z` always name the local zone. |
| `system.formatDate("long" \| "short"[, timeZone])`, `system.formatTime(seconds[, timeZone])` | The user's own date / time formats (Windows region settings), for the local time or the time in `timeZone`. |
| `system.locale` | `"en-US"`. |
| `system.hour12` | The user's time format is a 12-hour clock. |
| `system.firstDayOfWeek` | 0 = Sunday. |
| `system.dayNames`, `system.shortDayNames` | Sunday first. |
| `system.monthNames`, `system.shortMonthNames` | January first. |
| `system.units`, `system.temperatureUnit` | `"metric"` / `"imperial"` and `"C"` / `"F"`, from `weather.units` in config.json. |

### settings

The global `settings` is the merged settings object (see [Settings schema](#settings-schema)). It
is replaced (and `onSettingsChanged` called) when the item's settings change. A `secret` setting's
value is there in plain text (the widget needs it); a `url` setting's value is one the settings
page would accept (else its default).

Round 29: the settings object also has two methods (not enumerable: `JSON.stringify(settings)` and
`Object.keys(settings)` see the values only; no setting may be named `set` or `onChange`):

| Call | |
|---|---|
| `settings.set(key, value)` | Changes one of this item's own settings: `key` must be a key of the manifest's schema (another key throws a TypeError), and `value` is checked exactly as the settings page checks what the user types: a `bool` a boolean; an `int` a whole number within `min` - `max` (outside: refused); a `number` finite, clamped to `min` - `max`; a `choice` one of its values (a `"perf"` choice any metric key); a `colour` `#AARRGGBB` / `#RRGGBB`; a `timezone` `""` or a zone of this Windows; a `url` a valid url value (below); `string`, `font`, `file`, `folder` and `secret` one line of at most 1024 characters; a `place` cannot be set (it throws: the user picks a place). A refused value throws a TypeError naming the reason. The change is written after the calling function returns (with the other changes of the same moment, in one write of config.json through its usual debounced save, as one Undo step `"Changed <setting> of <widget> (by the widget)"`); then the dock re-reads the item, so `onSettingsChanged` and `settings.onChange` run (never inside the call that set it) and an open settings window shows the new value. A `secret` is stored protected, as when the user types it. Returns `true` when the change was taken (also when the value is the one it has), **`false`** when the rate limit refused it: 5 changes at once, then one more every 10 seconds (a warning in the log, at most every 30 s). config.json is rewritten whole on every save: keep state that changes often in `storage`, not in settings. **A `url` set this way does not make its host reachable**: only an address the user types in the settings page, followed by the prompt, does (see [http](#http), user hosts). In a preview it changes nothing and returns `false`. |
| `settings.onChange(fn)` | `fn(settings)` after the item's settings changed (the settings window, a quick setting, Reload config, the widget's own `settings.set`), right after `onSettingsChanged`. Returns a remover. Register it at the top level: the listeners belong to the script, not to one settings object, so they keep working after `settings` is replaced. |

```js
// A widget that remembers the view the user picked in its popup (a choice setting "view").
export function onPopupClick(e) {
  const view = e.x < POPUP_W / 2 ? 'day' : 'week';        // the popup's width, as opened
  if (!settings.set('view', view)) console.warn('too many changes; try again in a few seconds');
}
settings.onChange(() => requestRedraw());
```

### storage

A small persistent JSON store per widget **id** (every item with that id shares it), in
`%LocalAppData%\WinDock\widget-data\<id>.json` (outside the widget's folder, so exporting,
replacing or deleting the folder keeps the data, and `watch` does not see the writes). Changes are
written one second after the last change (and when the dock quits); nothing is written while
nothing changes. At most 1 MB per widget. Round 25: a store file larger than that on disk (edited
by hand, written by another program) is not loaded: the widget sees an empty store, every
`storage.set` throws an Error naming the file, and the file is left as it is (never rewritten).

| Call | |
|---|---|
| `storage.get(key, fallback)` | The stored value (any JSON value), or `fallback` (default `undefined`). |
| `storage.set(key, value)` | Stores `JSON.stringify(value)`; `undefined` removes the key. Keys are 1 - 256 characters. Throws a RangeError when the store would pass 1 MB. |
| `storage.remove(key)`, `storage.keys()`, `storage.clear()` | |
| `storage.onChange(fn)` | Round 6: `fn({ key })` when **another item** of the same widget (on any dock or the desktop) set or removed `key` (a value that did not change is not reported, nor are the item's own writes). Delivered shortly after the write (posted, once per key and burst). Returns a remover. Use it to redraw what several items show from one key (the Notes widget's shared notes). |

### http

Needs `{ "http": [hosts] }` in `permissions`; any other host, or no permission, throws a
TypeError at the call. Requests run on a worker thread (WinHTTP, the system's proxy settings) and
come back as a promise resolved on the dock's thread. Round 25:

- **Redirects** are followed by the host, at most 5, and each one is checked like the first URL:
  its host must be in the permission (a wildcard never matches an IP address), and an https URL
  never redirects to http. A refused redirect rejects the promise ("http: the redirect to host ...
  is not in the widget's http permission").
- **Local addresses are refused**: this PC (`localhost`, 127.0.0.0/8, ::1), the local network
  (10/8, 172.16/12, 192.168/16, link-local 169.254/16 and fe80::/10, unique-local fc00::/7) and
  the unspecified address, in their IPv4-mapped forms too. The address checked is the one the
  request actually connected to, before anything of the response reaches the script, so a public
  name that resolves to a local address is refused as well. To reach a device on your network or a
  server on this PC, list that address itself (`"http:192.168.1.20"`) or `"http:localhost"`; the
  consent dialog then says "this PC" / "your local network" for it. Behind a proxy the address
  WinHTTP connects to is the proxy's, and the check does not apply.
- **The timeout** (`timeout`, 1000 - 30000 ms, default 10000) is for the whole request, redirects
  and body included.
- **Headers**: the name must be a token and neither name nor value may hold a line break (checked
  by the host, not only by the API's own code); `Host`, `Content-Length`, `Transfer-Encoding`,
  `Connection`, `Keep-Alive`, `Upgrade`, `TE`, `Trailer`, `Expect` and every `Proxy-*` header are
  refused (a TypeError at the call), and all headers together are at most 16 KB. On a redirect to
  another host `Authorization` is dropped.
- Windows credentials are never sent (no automatic logon to intranet servers or proxies).
- **User hosts** (round 29): a manifest can let the user choose the server, for a widget that reads
  "any RSS feed" or "your Home Assistant": `{ "http": ["api.example.com"], "hostSettings": ["feedUrl"] }`
  names `url` settings ([Settings schema](#settings-schema)) whose host becomes reachable for that
  item once the user typed the address in the widget's settings page and allowed it in the prompt
  that follows (`"<Widget> wants to connect to <host>"`). The consent is per item (`dock.itemKey`) and
  setting, for exactly that host; the URL changed to another host asks again, and until then a
  request to it throws ("host ... was not allowed: enter it in the widget's settings and allow it
  when asked"). A value the script sets with `settings.set`, or one written into config.json, is
  never allowed by itself. Every redirect is checked against the manifest's hosts plus that host.
  Such a host must use **https**; plain `http://` and **local addresses** are allowed only when the
  host's own text names a device on the local network: a private, link-local or unique-local
  address (`192.168.1.20`, `fd00::5`), a name under `.local`, `.lan`, `.home`, `.home.arpa`,
  `.internal` or `.localdomain`, or a single-label name (`homeassistant`); the prompt then says the
  address is on the local network. Such a host may connect to a private address, never to this PC
  (`localhost`, loopback). A public-looking name must connect to a public address: a name the user
  typed that resolves (or is rebound) to an address on the local network is refused after connect,
  before the script sees anything (use the device's own `.local` name or its address instead). The
  idea behind the rules: a widget must not be able to steer itself to an address inside the user's
  network (a router's admin page, a NAS) through a host the user allowed for something else.

| Call | |
|---|---|
| `http.get(url, options)` | `options`: `{ headers: { name: value }, timeout: ms (1000 - 30000, default 10000; the whole request), type: "json" \| "text" }`. |
| `http.post(url, body, options)` | A string body is sent as text (`Content-Type: text/plain; charset=utf-8`), anything else as JSON (`application/json`), unless `headers` names a content type. |
| `http.request({ method, url, body, headers, timeout, type })` | `GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `HEAD`. |

The promise resolves to `{ status, ok, contentType, text, json }` for every HTTP response (`ok`:
status 200 - 299; `json`: the parsed body when `type: "json"` was asked for or the content type is
JSON). It rejects with an Error on a network failure, a timeout, a body over 2 MB, or (with
`type: "json"`) a body that is not JSON. At most 4 requests per widget are in flight; a reload
drops the answers of the old script.

```js
const r = await http.get('https://api.open-meteo.com/v1/forecast?latitude=52.5&longitude=13.4&current=temperature_2m', { type: 'json' });
if (r.ok) temperature = r.json.current.temperature_2m;
```

### shell

Needs `"shell"` in `permissions` (else every call throws).

| Call | |
|---|---|
| `shell.open(target, args)` | Opens a file, folder, app or URL the way Explorer does (ShellExecute; `%VAR%` expanded). Round 25: it opens right **after** the calling function returns (ShellExecute can wait on other programs and show its own message box, which must not happen in the middle of a script call), so `shell.open` returns `true` at once and a target that cannot be opened is reported in the widget's log ("shell.open: could not open ..."), not thrown. A missing permission or an empty target still throws. At most 16 per call. |
| `shell.list(folder)` | The folder's entries, folders first, in Explorer's name order, hidden and system entries left out (up to 1000): `[{ name, path, folder, size, modified }]` (`modified`: ms since 1970, UTC). Draw their icons with `canvas.fileIcon`. |
| `shell.sendKeys(name)` | Presses one key combination of a fixed allow-list with `SendInput` (as if typed: the keys go to whatever has the focus, the shell handles the Windows-key ones). Anything else throws, and so does input Windows refuses (a window of higher integrity has the focus). Returns `true`. |

The `sendKeys` allow-list (names are case-insensitive). It is a fixed list on purpose, not a parser
of `"modifier+key"` strings: a widget may open Windows' own UI and press the media keys, never type
shortcuts (Ctrl+C, Alt+F4) into the app that has the focus. Round 11 phase 11B added `win+m`,
`win+shift+m` and `ctrl+shift+esc`; a combination's modifiers are held while its last key is pressed.

| Name | Keys | Opens |
|---|---|---|
| `win` | Windows | Start |
| `start-near` | Windows | Start, after moving the Start menu's window beside the widget (the Start widget's Menu position; round 16 phase 16A): above the widget's slot on a bottom dock, beside it on a side dock, above a standalone widget, clamped to the monitor; the window goes back when the menu closes. Where the menu's window is not found it is `win` |
| `win+a` | Windows + A | Quick settings |
| `win+b` | Windows + B | The notification area (keyboard focus) |
| `win+d` | Windows + D | Show / hide the desktop |
| `win+e` | Windows + E | File Explorer |
| `win+i` | Windows + I | Settings |
| `win+m` | Windows + M | Minimizes every window (the Show desktop widget's "minimize") |
| `win+shift+m` | Windows + Shift + M | Restores what Windows + M minimized |
| `win+n` | Windows + N | The notification centre and calendar |
| `win+s` | Windows + S | Search |
| `win+tab` | Windows + Tab | Task view |
| `win+v` | Windows + V | Clipboard history |
| `win+w` | Windows + W | Widgets |
| `win+x` | Windows + X | The Start button's menu |
| `win+space` | Windows + Space | The next input language (round 27; Windows 11 shows its switcher for a moment; `input.next()` presses it without the `shell` permission) |
| `win+shift+s` | Windows + Shift + S | A screen snip (Snipping Tool; round 28) |
| `win+k` | Windows + K | Cast (round 28) |
| `win+.` | Windows + . | The emoji panel (round 28; it types into the window that has the focus, which is the user's app, not the dock) |
| `win+ctrl+left`, `win+ctrl+right` | Windows + Ctrl + Left / Right | The previous / next virtual desktop (round 28) |
| `ctrl+shift+esc` | Ctrl + Shift + Esc | Task Manager |
| `volume-up`, `volume-down`, `volume-mute` | the volume keys | (the system volume; prefer `audio`) |
| `media-play-pause`, `media-next`, `media-previous`, `media-stop` | the media keys | (the focused media app; prefer `media`) |

### audio

Needs `"audio"` in `permissions`. Core Audio provides the default output and input device controls
and the per-app audio mixer. The provider starts with the first call of a widget that has the
permission and stops when the last such widget goes. Device and master-volume changes are event-driven.

| Call | |
|---|---|
| `audio.get()` | `{ available, volume, muted, device, deviceId, mic, micVolume, micMuted, micName, devices }`: `available` false when there is no output; `volume` 0 - 1 (the number Windows' volume slider shows / 100); `device` the output's name; `mic` true when there is an input device; `devices`: the active outputs `[{ id, name, default }]`. |
| `audio.setVolume(v)` | The output level, 0 - 1 (clamped). |
| `audio.setMuted(b)`, `audio.setMicMuted(b)` | Mutes / unmutes the output / the microphone. |
| `audio.setDevice(id)` | Makes the output with this id (from `devices`) the default (the console and multimedia roles, as the Sound settings do). This uses `IPolicyConfig`, the undocumented interface Windows' own settings use (stable since Windows 7). An id that is not an active output throws. |
| `audio.getApps()` | Reads the current app sessions on active output devices and returns one `{ id, name, volume, muted }` per app. `volume` is its highest session level (0–1) and `muted` is true when all its sessions are muted. Returns an empty array in previews. |
| `audio.setAppVolume(id, v)`, `audio.setAppMuted(id, b)` | Sets the level (clamped to 0–1) or mute state of an app's current sessions. Use an id from `getApps()`. These are separate controls: setting the level preserves mute. |
| `audio.onChange(fn)` | `fn(state)` (the `get()` object) whenever the level, a mute state, the default devices or the device list change, by this widget, another program, the volume keys or the quick settings. Returns a function that removes the listener. Changes are coalesced (one call per burst). A mute change can take most of a second to be reported by Windows: a widget that changes something itself should update its own view at once. |

### media

Needs `"media"` in `permissions`. The system's current media session (what the media keys and the
quick settings' media controls act on: Spotify, browsers playing media, the Media Player app ...),
through `GlobalSystemMediaTransportControlsSessionManager`. The provider starts with the first call
and stops when the last widget using it goes; all its WinRT calls run on a worker thread of its own,
woken by the session manager's events (sessions, current session, media properties, playback
state, and since round 23 the timeline); nothing polls. A timeline report alone only updates the
position when it jumped (more than 1.5 s from where the last report, carried forward, puts it: a
seek, the app moved), so apps that report their timeline often cost no redraws; between reports a
widget carries `position` forward itself while `status` is `"playing"` (as the Now playing mini
players do, redrawing once a second only while they show the progress).

| Call | |
|---|---|
| `media.get()` | `{ available, session, title, artist, album, app, status, hasArt, position, duration, canPlay, canPause, canToggle, canNext, canPrevious, canSeek }`: `canSeek` (round 23) true when the session lets its position be changed (`media.seek`) and reports a duration; `session` false when nothing is current; `status` `"playing"`, `"paused"`, `"stopped"` or `"none"`; `app` the session's AppUserModelId (`"Spotify.exe"`, `"Chrome"`, `"Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic"`); `position` / `duration` in seconds or `null` (the position is carried forward while playing, from the app's last report). The first call returns `available: false` until the worker's first snapshot (a moment later, `onChange` follows). |
| `media.art()` | The current art as an image token (`"media:art/3"`) for `canvas.image(token, x, y, w, h)` / `imageSize`, or `null`. The thumbnail is read once per track (app + title + artist + album) and decoded by the canvas like a package image; a token of an older track throws in `canvas.image`. |
| `media.play()`, `pause()`, `toggle()`, `next()`, `previous()` | Sent to the current session (asynchronously; `onChange` reports the result). |
| `media.seek(seconds)` | Round 23: the current session to `seconds` from the track's start (clamped to the duration; `TryChangePlaybackPositionAsync`). Ignored unless `canSeek`. `get()` reports the target at once (no `onChange` for it: redraw yourself), and keeps reporting it until the app reports a position newer than the seek; the seeks of a burst (a drag) are coalesced, the latest wins. Seek on release rather than on every `onDrag` step. |
| `media.onChange(fn)` | `fn(state)` (the `get()` object) when the session, the track, the playback state or the art change. Returns a remover. |

### notifications

Needs `"notifications"` in `permissions`. Windows' notification centre as the dock reads it
(README "Notifications and badges"): the toasts of every app, newest first, plus captured tray
balloons when `notifications.balloons` is on. The first call of a widget makes the dock's
notification service run (it polls the list every few seconds: Windows has no change event for
the dock) until the widget goes or reloads; while it runs every change calls `onChange`.

| Call | |
|---|---|
| `notifications.get()` | `[{ id, app, appKey, aumid, title, body, time, unread, balloon }]`: `app` the app's display name, `appKey` the dock key it maps to (a lower-case AUMID or exe path; `""` unknown), `title` the first text line, `body` the others joined with `\n`, `time` ms since 1970 (UTC), `unread` false once its app was activated from the dock. Empty while the service is off or denied. |
| `notifications.state()` | `{ state, reason }`: `state` `"polling"`, `"denied"` (no access: `reason` says why) or `"off"`. |
| `notifications.count()` | The unread notifications (every app). |
| `notifications.countFor(appKey)` | The unread ones of one app key (case-insensitive). |
| `notifications.dismiss(id)` | Removes it from Windows' notification centre (a balloon: from the list). |
| `notifications.dismissAll()` | Every notification of the dock's list. |
| `notifications.markRead(id)` | Marks it read (its badge goes; it stays in Windows). |
| `notifications.activate(id)` | Brings its app to the front from the dock (its slot's windows, or starts it: shell:AppsFolder for an AUMID, the exe for a path) and marks its notifications read. Windows does not let another app activate the toast itself. Round 25: an unknown `id` throws at once; the activation itself runs right after the calling function returns (starting an app can wait on other programs). |
| `notifications.openCentre()` | Toggles the dock's notification centre (README "Notifications and badges") over the widget: the same list as the `notifications` element's, opened after the call returns. Throws when the widget has no slot. |
| `notifications.onChange(fn)` | `fn(list)` (the `get()` array) when notifications come, go or change state. Returns a remover. A change the widget makes itself (`dismiss`, `dismissAll`, `markRead`, `activate`) reaches its own listeners **after** the function that made it returns, not inside the call (see [Limits and errors](#limits-and-errors), re-entry). |

### calendar

Needs `"calendar"` in `permissions`. The events of the dock's calendar feeds (README "Calendar
feeds", App settings > Calendar feeds: ICS web feeds and files). The feeds belong to the dock, not to the
widget: they are fetched and expanded whether or not a widget asks (and not at all while no feed is
enabled; every call then returns empty lists). Calls answer from the dock's expanded occurrences
at once (no I/O); nothing polls, changes arrive through `onChange`.

Dates in arguments: a `Date`, a number (ms since 1970, UTC; its local day where a day is meant) or
`"YYYY-MM-DD"` (a local day; its local midnight where an instant is meant). A malformed one throws.

| Call | |
|---|---|
| `calendar.feeds()` | `[{ name, color, enabled, file, status, error, lastFetch, events }]`: `status` `"off"`, `"loading"`, `"ok"` or `"error"` (`error` says why), `lastFetch` ms since 1970 (0 never), `events` the feed's occurrences in the expansion window. |
| `calendar.events({ from, to })` | The occurrences overlapping `[from, to)`, sorted by start (all-day first on the same start; at most 2000). `from` defaults to now, `to` to the end of the expansion window (92 days ahead); at most about three years at a time. Ranges outside the window extend it. |
| `calendar.eventsOn(date)` | The events touching that local day (default today): all-day and continuing ones first, then by start (what the month calendar lists for the day). |
| `calendar.today()` | `eventsOn` today. |
| `calendar.next(n)` | The next `n` (1 - 100, default 5) events that have not ended (ongoing first, an all-day event of today included), sorted by start. |
| `calendar.refresh()` | Every feed fetches / is read now. |
| `calendar.openPopup(date)` | The dock's month calendar over the widget, showing that day's month with the day selected (default today); after the current call returns. |
| `calendar.onChange(fn)` | `fn(feeds)` (the `feeds()` array) after the feeds' data or status changed. Returns a remover. |
| `calendar.onReminder(fn)` | `fn(event)` when a reminder fires (feeds with reminders, App settings > Calendar feeds; the dock also shows its banner). Returns a remover. |

An event: `{ id, feed, color, title, location, description, start, end, allDay, url, recurring,
startDay, endDay, startTime, endTime, reminder }`: `id` stable across refreshes (the feed, the
event's UID and its original start), `feed` / `color` the feed's name and `"#AARRGGBB"`, `start` /
`end` ms since 1970 (UTC; an all-day event: the local midnights), `startDay` / `endDay` its first /
last local day as `"YYYY-MM-DD"`, `startTime` / `endTime` the locale's short time (`""` for
all-day events), `recurring` it is an instance of a repeating event, `reminder` the event's own
alarm in minutes before the start (-1 none; informational, the dock's reminders use the feed's
setting). Format days with these fields rather than `new Date(start)`: QuickJS's local `Date`
applies today's daylight-saving offset to every date on Windows.

### weather

Needs `"weather"` in `permissions`. The dock's forecast (README "Weather and calendar",
[Open-Meteo](https://open-meteo.com)). Since round 5 the dock runs **one WeatherService per
place** (latitude, longitude, units, refresh interval): every item showing a place (built-in
`weather` items and widgets) reads that place's data, and a refresh is one request per place
whatever shows it. **Which place a widget gets**: its item's own `latitude` / `longitude` (with its
`units`, `refreshMinutes`, `location` and `label` settings over the defaults: give the widget a
`place` field, see Settings schema) when it has them; else the place of the first `weather` item
of its dock; else the defaults (the top-level `weather` object). A place's service runs while a
shown dock's weather item or some widget that called the provider needs it (the first call
creates it and starts it shortly after the call returns: `onChange` follows; a new place moves the
widget to that place's service); it fetches every `refreshMinutes` and after a failure 1, 2, 4 ...
minutes later. Nothing polls; calls answer from the service's data at once.

| Call | |
|---|---|
| `weather.get()` | `{ state, configured, place, latitude, longitude, units, temperatureUnit, windUnit, refreshMinutes, busy, error, status, age, fetchedAt, updated, current, hourly, daily }`: `state` `"none"` (no data yet / not configured / not running), `"stale"` (the cached or last data; a fetch failed or has not finished), `"fresh"` or `"error"` (no data and the last fetch failed: `error` says why, e.g. `"cannot connect"`); `configured` false while no place is set; `place` the name shown (the widget's own `location`, else its place's); `label` the widget's own `label` setting (`""` none); `units` `"metric"` / `"imperial"`, `temperatureUnit` `"C"` / `"F"`, `windUnit` `"km/h"` / `"mph"`; `busy` a fetch is in flight; `status` the Settings page's status line; `age` seconds since the data was fetched (-1 none); `fetchedAt` ms since 1970 (0 none); `updated` its local short time (`"10:15"`); `current` `{ temp, code, isDay, description, glyph, wind, humidity, feelsLike, uv }` or `null` (`code` the WMO weather code, `description` "Partly cloudy", `glyph` the glyph's name; round 28: `feelsLike` the apparent temperature in the same unit, `uv` the UV index now); `hourly` up to 48 hours from the current one: `[{ time (ms since 1970), hour (0 - 23, the place's local time), temp, code, isDay, precip (% chance) }]`; `daily` up to 7 days from today: `[{ day ("YYYY-MM-DD"), weekday (0 = Sunday), code, description, min, max, uvMax, sunrise, sunset }]` (round 28: `uvMax` the day's highest UV index; `sunrise` / `sunset` ms since 1970, `null` when the sun does not rise or set that day; all from the same request as before). Missing numbers are `null`. |
| `weather.refresh()` | Fetches the widget's place now (its service: every item of that place gets the result). After the call returns. |
| `weather.openSettings()` | Opens this widget item's own settings window (its Item window, round 5, where its `place` field is; after the call returns; the widget's popup closes). |
| `weather.icon(code, isDay, colours)` | An image token for `canvas.image(token, x, y, w, h, opacity)`: the dock's own vector condition glyph for a WMO `code` (sun / moon with `isDay` false, partly cloudy, overcast, fog, drizzle, rain, snow, thunder), `"setLocation"` (the map pin) or `"refresh"` (the circular arrow). `colours`: `{ ink, water }` (default `dock.theme.text` and `dock.theme.accent`; `refresh` uses `ink`). Drawn as geometry at any size (crisp at any scale; `w` / `h` omitted: 24 DIPs); `opacity` dims it as the native element does for stale data (0.6). |
| `weather.onChange(fn)` | `fn(state)` (the `get()` object) whenever the data or the status of the widget's place changes: a fetch started, finished or failed, the place or units changed. Returns a remover. |

The built-in `weather` widget ("Weather", `widgets/weather`) is the worked example: its
slot draws what the native element draws (pixel for pixel at 100 %), its popup the native
forecast popup's layout; its schema has `place`, `label`, `units` and `refreshMinutes` (round 5),
and it draws its `label` under the temperature.

### perf

Needs `"perf"` in `permissions`. The machine's performance (round 6 phase 6D): CPU, memory, GPU,
disks, network, battery, uptime, and the sensors of [HWiNFO](https://www.hwinfo.com) (temperatures,
fans, voltages, clocks, power ...) when it runs. **One sampler** serves every widget: a worker
thread of the dock that starts with the first widget that calls the provider (any call, `onChange`
included) and stops when the last one goes or reloads; while no widget uses `perf` nothing runs.
It samples every `interval` ms (250 - 10000, default 1000: the fastest any live widget asked for
with `perf.setInterval`), and each snapshot is handed to the dock's thread, which keeps it and calls
every user's `onChange`. The dock also keeps the **last 600 samples of every metric**, so a graph
needs no history of its own (`perf.series`). Calls answer from the dock's copy at once.

| Call | |
|---|---|
| `perf.get()` | The latest snapshot: `{ time, interval, serial, metrics: { key: value, ... } }`: `time` ms since 1970 (UTC), `interval` the sampler's period in ms, `serial` the snapshot's number (0: none yet: right after the first call `metrics` is `{}` for up to half a second; draw a placeholder), `metrics` every available key with its number (bools are 0 / 1). The object is frozen and the same object until the next snapshot (cheap to call in every `draw()`); a key that is not available is absent, never 0. |
| `perf.metrics()` | Every available metric: `[{ key, name, unit, min, max, kind, group, hwinfo }]`: `name` for people ("CPU total", "GPU (3D)", "CPU [#0]: AMD Ryzen 7 · Core Temperatures"), `unit` (`"%"`, `"B"`, `"B/s"`, `"MHz"`, `"s"`, HWiNFO's own such as `"°C"`, `"RPM"`, `"V"`, `"W"`; `""` none), `min` / `max` a sensible range for gauges (percent 0 - 100, memory 0 - the total, a network link's speed, a HWiNFO reading's own lowest / highest rounded outwards; counts, frequency and HWiNFO ranges widen when a value leaves them), `kind` (below), `group` (`"CPU"`, `"Memory"`, `"GPU"`, `"Disk"`, `"Network"`, `"Battery"`, `"System"`, or the HWiNFO sensor's name), `hwinfo` true for HWiNFO readings. Frozen; the same array until the key set or a range changes. |
| `perf.series(key, n)` | Up to `n` (0 - 600, default 600) newest values of `key`, **oldest first**, as numbers; `NaN` for a sample in which the key was missing. Fewer while the sampler has not taken that many since it started; `[]` for a key it never saw. |
| `perf.setInterval(ms)` | This widget's sampling period, clamped to 250 - 10000 ms (a widget that never calls it counts as 1000). The sampler uses the fastest of all its widgets'; returns the period it uses now. `onChange` then comes at that rate for every widget: a widget that wants a slow display can still redraw less often. |
| `perf.hwinfo()` | `{ available, reason, version, pollTime, sensors: [{ index, name }], readingCount }`: `available` true while HWiNFO's shared memory is readable and fresh (updated in the last 10 s), else `reason` says why (not running / sharing off, stopped sharing, stale data: the free version stops after 12 hours); `version` "2.1" (its layout), `pollTime` ms since 1970 of HWiNFO's last update (refreshed every 5 s here), `sensors` its sensors by index (the `<sensor>` part of the keys), `readingCount` its readings. |
| `perf.drives()` | Round 28: the space of the **fixed** drives: `[{ drive, label, ready, total, free, used }]` (`drive` `"C:"`, `label` the volume's name, `ready` false for a drive that cannot be read, such as a locked BitLocker volume, which then has no numbers; `total` / `free` / `used` in bytes, `free` what this user may still write). Network, removable and optical drives are never asked (they can hang or ask for a disc). Read when a script asks, at most every 5 s (calls in between get the same answer); it does not start the sampler. Call it from a timer of a few seconds or more, not from every `draw()`. |
| `perf.onChange(fn)` | `fn(snapshot)` (the `get()` object) after every snapshot. Returns a remover. Starts the sampler like `get()`. |

**Keys** (`perf.metrics()` lists exactly the available ones; values in the unit shown):

| Key | Unit, kind | |
|---|---|---|
| `cpu.total`, `cpu.core.N` | %, percent | CPU busy time over the last interval, total and per logical processor (N from 0; at most 64 keys; one processor group). |
| `cpu.frequency` | MHz, clock | The effective clock as Task Manager shows it ("% Processor Performance" x "Processor Frequency"); else what Windows reports (often the base clock). |
| `cpu.processes`, `cpu.threads` | count, number | |
| `memory.used`, `memory.available`, `memory.total` | B, bytes | Physical memory (`memory.total` = Windows' usable memory). |
| `memory.percent` | %, percent | Used / total. |
| `memory.committed`, `memory.commitLimit` | B, bytes | The commit charge and its limit (RAM + page files). |
| `gpu.total` | %, percent | The 3D engine's busy time (each engine is summed over the processes using it). |
| `gpu.max` | %, percent | The busiest engine of any kind (Task Manager's GPU %). |
| `gpu.engine.3D`, `.Copy`, `.VideoDecode`, `.VideoEncode`, `.Compute` | %, percent | Each kind's busiest engine (only the kinds the GPU has). |
| `gpu.memory.used`, `gpu.memory.total` | B, bytes | Dedicated GPU memory in use (every adapter) and the first hardware adapter's size. |
| `disk.total` | %, percent | Physical disks' "% Disk Time" (all disks, at most 100). |
| `disk.read`, `disk.write` | B/s, bytesPerSecond | All physical disks. |
| `disk.N.active`, `disk.N.read`, `disk.N.write` | %, B/s | Per physical disk (N as Windows numbers them; at most 16 disks). |
| `network.down`, `network.up` | B/s, bytesPerSecond | Received / sent over the hardware interfaces that are up (filter, loopback and tunnel interfaces left out: their traffic crosses the hardware one too). |
| `network.<alias>.down`, `network.<alias>.up` | B/s, bytesPerSecond | Per interface (`<alias>` its name in lower case, `wi-fi`, `ethernet-2`; at most 8 interfaces). |
| `battery.percent`, `battery.charging` | %, percent; bool | Only on a machine with a battery. |
| `battery.acLine` | bool | Round 7: 1 on mains power, 0 on battery (`ACLineStatus`; absent when Windows does not know, and without a battery). |
| `battery.timeLeft` | s, seconds | Round 7: the time left on battery as Windows estimates it (`BatteryLifeTime`); absent while unknown (charging, on mains power, right after a change) and without a battery. |
| `system.uptime` | s, seconds | Since Windows started. |
| `hwinfo.<sensor>.<reading>` | HWiNFO's unit and kind | Each HWiNFO reading (at most 512), named "<sensor> · <reading>" with HWiNFO's own (user-edited) names. |

`kind` is one of `"percent"`, `"bytes"`, `"bytesPerSecond"`, `"temperature"`, `"voltage"`, `"fan"`,
`"current"`, `"power"`, `"clock"`, `"number"`, `"bool"`, `"seconds"` (HWiNFO's reading types map to
temperature, voltage, fan, current, power, clock, percent (usage) and number (other; a `Yes/No`
unit: bool)). Sources: `NtQuerySystemInformation` (CPU), `GetPerformanceInfo` and
`GlobalMemoryStatusEx` (memory, processes), PDH counters by their English names (frequency, GPU
engines and memory, disks: any Windows language), DXGI (the GPU's size), `GetIfTable2` (network),
`GetSystemPowerStatus` (battery). A source that fails leaves its keys out.

**HWiNFO**: install [HWiNFO64](https://www.hwinfo.com), open its Settings, turn on **Shared Memory
Support** (Settings > General / User Interface), and run it (sensors-only mode is enough; it may
stay minimized to the tray). The provider reads HWiNFO's shared memory (`Global\HWiNFO_SENS_SM2`,
the documented layout of HWiNFO's SDK; nothing is installed or linked) and looks for it again every
5 s while it is absent, so starting HWiNFO later adds its readings within seconds (and
`perf.metrics()` changes). **The free version shares its sensors for 12 hours per start**; after
that the data goes stale (`perf.hwinfo().available` false, its keys disappear) until HWiNFO is
restarted. HWiNFO Pro has no limit. Its readings change at HWiNFO's own polling rate (2 s by
default), whatever the sampler's interval.

**Memory**: 600 doubles (4.8 KB) per key, for at most 1024 keys ever seen while the sampler runs
(this machine has 68 keys without HWiNFO; a few hundred with it: under 2.5 MB); the history goes
when the sampler stops. **Cost**: STATUS.md 6D has the measurement (the sampler plus one System
monitor redrawing per snapshot); nothing at all while no widget uses `perf`.

```js
// A tiny CPU graph: no history of its own, one redraw per snapshot, no timers.
perf.setInterval(1000);
perf.onChange(() => requestRedraw());
export function draw(canvas, dock) {
  const v = perf.get().metrics['cpu.total'];
  const s = perf.series('cpu.total', 60);
  const path = canvas.path();
  s.forEach((x, i) => { if (!Number.isNaN(x)) path.lineTo(canvas.width * i / 59, canvas.height * (1 - x / 100)); });
  canvas.stroke(path, dock.theme.accent, 1.5);
  canvas.text(v === undefined ? '—' : Math.round(v) + '%', 2, 2, { size: 11, color: dock.theme.text });
}
```

The built-in `system-monitor` and `system-overview` widgets are the worked examples (see
[Examples](#examples)).

### network

Round 27. Needs `"network"` in `permissions`. The connection Windows uses for the internet, what the
taskbar's network icon showed (Windows.Networking.Connectivity: `GetInternetConnectionProfile`).
The provider starts with the first call (`get()` or `onChange`) of a widget that has the
permission and stops when the last such widget goes. It is event-driven (Windows'
`NetworkStatusChanged`: connected / disconnected, internet gained or lost, a captive portal, the
cost, another connection taking over); nothing polls, and it is quiet (reads nothing, calls no
listener) while none of its widgets can be seen (`dock.visible` false and no popup open) or the
session is locked; when one is seen again it reads once if something changed meanwhile.

| Call | |
|---|---|
| `network.get()` | `{ available, connected, internet, state, kind, name, bars, signal, metered }`: `available` false when Windows could not say; `connected` there is a connection for the internet; `state` `"internet"`, `"local"` (connected, no internet), `"constrained"` (a captive portal: sign-in needed) or `"none"`; `internet` the same as `state === "internet"`; `kind` `"wifi"`, `"ethernet"`, `"cellular"`, `"vpn"` (a PPP, tunnel or virtual adapter carries it), `"other"` or `"none"`; `name` the Wi-Fi network's name (SSID), for other kinds the connection's profile name ("Network 2"), `""` without one; `bars` Windows' signal bars 0 - 5 for Wi-Fi and cellular, else `null`; `signal` `bars` × 20 (0 - 100) or `null`; `metered` the connection is set as metered (a fixed or variable cost). |
| `network.onChange(fn)` | `fn(state)` (the `get()` object) when anything in it changed (coalesced: one call per change, not per Windows event). Returns a remover. |

Limits (from Microsoft's documentation; not measured): the signal is the 0 - 5 bars Windows shows,
not a percentage, and a change of the bars alone is not an event: they are read again at every
network event and whenever the provider wakes, so they can lag while nothing else changes. The
Wi-Fi APIs that give a 0 - 100 quality and signal events (`wlanapi`'s current connection and its
MSM notifications) need the user's precise-location consent since Windows 11 24H2 and would show a
location prompt; they are not used. The SSID comes from `WlanConnectionProfileDetails.
GetConnectedSsid`, which Microsoft's page on that change ("Changes to API behavior for Wi-Fi access and location") recommends instead. A VPN is recognised by its adapter's
type only (the built-in Windows VPN, WireGuard, OpenVPN's TAP / wintun); one that presents an
ethernet adapter shows as ethernet. Airplane mode shows as no connection.

### input

Round 27. Needs `"input"` in `permissions`. The keyboard layout (input language) of the window the
user works in, and the installed layouts: what the taskbar's language indicator ("ENG") showed.
The provider starts with the first call (`get()` or `onChange`) and stops with the last widget
using it; it adds no hook of its own: it listens to the shell notices the dock already receives (a
language change, a window activated) and is quiet while none of its widgets can be seen or the
session is locked. Each notice costs a `GetKeyboardLayout` and a `GetKeyboardLayoutList`; the names
are read once per layout.

| Call | |
|---|---|
| `input.get()` | `{ available, id, tag, code, region, language, layout, current, layouts }`: the active layout's `id` (the HKL as 8 hex digits, `"04090409"`), `tag` (`"en-US"`), `code` (the three letters the taskbar shows, `"ENG"`, `"DEU"`: the ISO 639-2 name, upper case), `region` (`"US"`), `language` (`"English (United States)"`, in Windows' display language), `layout` (the keyboard's name, `"US"`, `"German"`, an IME's name); `current` its index in `layouts` (-1 when it is not listed); `layouts` every installed layout in the switcher's order, `[{ id, tag, code, region, language, layout, current }]`. Read fresh at every call. |
| `input.set(idOrTag)` | Asks the window the user works in (the foreground window; while a popup of the dock is in front, the window before it) to switch to that layout (an `id` from `layouts`, or a `tag`: its first layout) by posting `WM_INPUTLANGCHANGEREQUEST`, as the old language bar did. Returns `true` when the request was posted; an unknown id throws, and so does a window Windows does not let the dock post to (an app running as administrator). The switch itself happens when the app handles the message: `onChange` (or the next `get()`) shows it. |
| `input.next()` | Presses Windows + Space (as the user would: every app follows it, and Windows 11 shows its switcher for a moment). Needs only `"input"` (the key is fixed). Returns `true`; input Windows refuses throws. |
| `input.onChange(fn)` | `fn(state)` (the `get()` object) when the active layout or the list changed. Returns a remover. |

Limits: Windows reports a switch inside a window only to that window (`WM_INPUTLANGCHANGE`). The
provider relies on the shell's `HSHELL_LANGUAGE` notice, which is documented for shell hooks
(`WH_SHELL`); that it also reaches a shell hook window (`RegisterShellHookWindow`, what the dock
uses) is assumed, not documented. Without it the indicator follows at the next window activation
and at every `get()` (the built-in widget reads again shortly after its own switch).
`input.set` is a request: an app that handles `WM_INPUTLANGCHANGEREQUEST` itself, console windows
and some UWP / WinUI apps may ignore it (`input.next()` works there). With Windows' default "one
input method for all apps" the layout is the same everywhere; with "Let me use a different input
method for each app window" the indicator follows the foreground app. Out of scope: an IME's
conversion mode (あ / A, half / full width) and Text Services Framework profiles beyond the layout
list.

### notify

Round 28. Needs `"notify"` in `permissions`. A banner on screen: the one the dock shows for calendar
reminders (beside the dock, over the widget's slot; above a standalone widget's window), with the
widget's title, one line of text and a **Dismiss** button. It never takes the focus. For a timer
that ended, a reminder of the widget's own, a state the user should see now.

| Call | |
|---|---|
| `notify.banner({ title, text, sound })` | Shows the banner after the current call returns. `title` (required, not empty) and `text` are plain text, one line each: line breaks and other control characters become spaces, `title` is cut at 80 characters and `text` at 160 (with "…"); nothing in them is a link or markup. `sound: true` plays Windows' "Asterisk" sound (the sound scheme's, off when the user turned system sounds off). Returns `true` when the banner was taken, `false` when the rate limit refused it (and in a preview, which shows nothing); a missing permission or an empty title throws. |

The limits, per widget item: **one banner at a time** (a newer one replaces the widget's banner on
screen or waiting), and **at least 5 seconds between two calls** (a call inside that gap shows
nothing, returns `false` and writes a warning to the widget's log). The banner stays until the user
dismisses it (Dismiss, or a click on it); the widget cannot close it. Like the calendar reminders
it also shows while the dock is auto-hidden (the dock stays hidden), it waits while a full-screen
app is in front (the full-screen guard) and shows when that app is gone, and on a dock it queues
behind a calendar reminder already shown. Script timers keep running while the dock is hidden, so a
`setTimeout` for the end of a countdown raises its banner then too (the Timer widget does this).

### links

Round 29. Needs `"links"` in `permissions`. Opens a web page in the user's browser, for a widget
that lists things to read (the built-in Headlines widget opens a headline's article) without asking
for `"shell"` (which could open anything and run programs).

| Call | |
|---|---|
| `links.open(url)` | Opens `url` (an `http://` or `https://` address, at most 2048 characters, no spaces or control characters; anything else throws a TypeError) in the default browser, right after the calling function returns. Only **as the answer to a click**: within 1.5 s of a click on the widget (`onClick`, `onMiddleClick`) or in its popup (`onPopupClick`), and once per click; otherwise it opens nothing, returns `false` and writes a warning to the log. Returns `true` when the page will open (`false` in a preview). |

A script cannot open pages by itself (a timer, a provider's `onChange`, an http answer): the user's
click is the consent for each page. The address can be any host: it is shown by the browser, not
fetched by ViberDock.

### popup

A widget can open one popup at a time, anchored on its slot beside the dock (above a bottom dock,
right of a left dock ...), like the dock's own popups. It holds the dock shown while it is open
and closes on Escape, when another window comes forward, on a click on the dock, and when the
widget reloads.

| Call | |
|---|---|
| `popup.open(width, height[, { passive }])` (or `popup.open({ width, height, passive })`) | A popup of that size in DIPs (16 - 1600 x 16 - 1200) that the module draws in `drawPopup(canvas, dock)` over the theme's panel; clicks in it go to `onPopupClick`. Opening it again resizes and redraws it. `passive: true` (default false): a passive popup: it never takes the focus, the mouse passes through it, clicks never reach `onPopupClick`, it does not keep the dock shown and it does not count for `popup.isOpen` or `closedPopup`. It closes on `popup.close()`, when the widget opens its ordinary popup, when the dock hides and when the widget goes. Opening it again resizes and redraws it in place: a wheel handler can open it on every notch. One passive popup per dock at a time; while the widget's ordinary popup is open, a passive open redraws that popup instead. |
| `popup.redraw()` | Calls `drawPopup` again (for whichever of the two is on screen). |
| `popup.close()` | Closes the ordinary popup and the passive one. |
| `popup.isOpen` | The widget's ordinary popup (of any kind, not a passive one) is on screen. |
| `popup.folder(path[, view])` | The dock's own folder popup (the folder's contents, scrolling, keyboard, navigation into sub-folders, file drops, Open in Explorer) for that folder, anchored on the widget. `view`: `"auto"` (default: a grid up to 48 entries, a list above), `"grid"` or `"list"`; anything else throws. Needs `"shell"`. |
| `popup.calendar()` | The dock's own month calendar (the clock's popup: locale weekday names, first day of the week, today marked), anchored on the widget. A second call while it is open does nothing; a click on the widget closes it (`closedPopup`). |
| `popup.previews(app)` | The window previews of an app the dock shows (its exe path, e.g. `"C:\\Windows\\explorer.exe"`, or its AppUserModelID; any case), anchored on the widget; nothing (a log line) when the app has no window on the dock. |
| `popup.editText(options)` | Round 6: a **text editor window** for the user to type into; returns a promise of the text after **OK** (Ctrl+Enter; Enter too when single-line), or `null` after **Cancel** (Escape, the close box, the widget reloading or going away). `options`: `{ title = "Edit", text = "", multiline = true, maxLength = 100000, placeholder = "" }`; `maxLength` 1 - 1000000 characters (a line break counts as one; the editor stops typing there and its status line says so); a `text` longer than `maxLength` is refused; line breaks come back as `"\n"` (a single-line editor turns those of `text` into spaces). The promise **rejects** (it never throws) on a bad argument (a `TypeError` / `RangeError`), when the widget already has an editor open (an `Error`, "an editor is already open": one per widget at a time), or when there is no editor for it there. No permission is needed. |

The calls are carried out right after the current call returns.

`popup.editText` opens a real window (class `WinDock.TextEditor`, in the Windows app mode's light or
dark colours): resizable, not modal, owned by the dock (or the standalone widget's window), placed
next to the widget on the dock's popup side (above a bottom dock's slot, beside a left / right
one's; above a standalone widget, below it without room) and kept on the monitor, 420 x 320 DIPs
(single line: 420 x 140), its size remembered per widget for the session. It takes the keyboard
focus; Ctrl+A selects all, Ctrl+Z undoes, Ctrl+Backspace deletes a word, Tab moves between the
text, OK and Cancel, a status line counts the characters. The dock stays shown while it is open,
and the widget keeps running meanwhile (its timers, providers and redraws go on; the editor itself
runs nothing while it waits). It is topmost only when its owner is (a dock above the windows), so a
desktop-layer dock's or widget's editor is an ordinary window.

```js
let editing = false;
export async function onClick() {
  if (editing) return;                         // one editor per widget at a time
  editing = true;
  try {
    const text = await popup.editText({ title: 'Note', text: storage.get('note', ''), maxLength: 2000 });
    if (text !== null) { storage.set('note', text); requestRedraw(); }  // null: cancelled
  } catch (e) {
    console.warn(e.message);                   // a bad argument, or no editor here
  } finally {
    editing = false;
  }
}
```

### widget

The widget's own package files (no permission needed: modules and images come from the package
anyway). Paths are package paths as for `import` (`"faces/classic.json"`).

| Call | |
|---|---|
| `widget.files(pattern)` | The package's files, sorted (`"main.js"`, `"faces/classic.json"` ...), only those matching the glob when one is given (`"faces/*.json"`; `*` and `?` stay inside one folder level, case-insensitive). A folder widget lists up to 512 files. |
| `widget.read(path)` | A file as text (UTF-8, at most 8 MB). Throws if it is not in the package. |
| `widget.readJson(path)` | The same, parsed (a SyntaxError names the file). |
| `widget.resize(w, h)` | Round 23: a [standalone widget](#standalone-widgets) asks for its window's size in DIPs (its client area). Applied after the current call returns: the top-left stays (a centred window stays centred), clamped to the manifest's `standalone.minWidth` / `minHeight` (a fixed `aspect` sets the height from the width) and to 4000, kept reachable on its monitor, and saved like a resize by hand; the widget is measured and drawn again at the new size. It works on a locked window too (the lock is against the pointer). Returns `true` when the request was taken, `false` on a dock (the dock sizes slots from `measure()`) or in a preview. Call it on a user's choice (`onSettingsChanged`), not from `draw()`. |

## Animation

Nothing on the dock animates by itself and nothing redraws per frame: a widget animates by asking
for frames with a timer and drawing the state for the current time in `draw()`. The analog clock
(`widgets/analog-clock/main.js`) is the worked example: its smooth second hand and the sweep of its
hands from 12 o'clock to the time when it appears or its face changes.

```js
let anim = null;      // { start, last } while it runs
let frames = 0;       // the setInterval of the frames

function startAnimation() {
  if (dock.reduceMotion || !dock.visible) return;  // never with reduce motion, never off screen
  anim = { start: 0, last: 0 };
  frames = setInterval(frame, 16);                  // ~60 frames a second, only while it runs
}
function frame() {
  const now = Date.now();
  if (!anim.start) anim.start = now;                // the clock starts with the first frame ...
  else if (now - anim.last > 50) anim.start += now - anim.last - 16;  // ... and pauses on a stall
  anim.last = now;
  if (now - anim.start >= 700) { anim = null; clearInterval(frames); frames = 0; }
  requestRedraw();
}
export function draw(canvas) {
  const p = anim && anim.start ? Math.min(1, (Date.now() - anim.start) / 700) : 1;
  const eased = 1 - Math.pow(1 - p, 3);             // ease-out cubic
  // ... draw with `eased` ...
}
export function onVisibilityChange(e) {
  // Off screen: stop every frame timer (and refreshes nobody sees); back: redraw once.
}
```

The rules (the dock's "efficiency first" budget):

- **An idle widget costs nothing**: no timer, no wake-up, no draw. Use `timer.setRefresh` for
  anything tied to the clock (it wakes at the boundary itself, aligned to local time) and
  `setTimeout` for one-off delays; never poll.
- **Frames only while something moves**: start a `setInterval` (16 ms ≈ 60 frames a second, 33 ms
  ≈ 30) when an animation starts and clear it when it ends. Each frame is one wake-up of the dock
  and one `draw()` of the widget (QueryState 59 / 58 count them).
- **Frames only while visible**: `dock.visible` is false while the dock is auto-hidden (or a
  desktop dock is behind another window); `onVisibilityChange({ visible })` tells the widget, which
  stops its frame timers then (the analog clock also stops its minute / second refresh while
  hidden and redraws once when it comes back). Round 26: the host does part of this for every
  widget: while hidden the refresh (`tick()`) does not run and `requestRedraw` waits; on the show
  one catch-up `tick()` (when a boundary passed) and one redraw. `setTimeout` / `setInterval` are
  left running (a widget may poll data with them), so frame timers are still the widget's to stop.
- **Reduce motion**: `dock.reduceMotion` (the dock's `animation.reduceMotion`, or Windows' "Animation
  effects" off) means no decorative animation: jump to the end state (the clock's smooth second
  hand ticks instead, its sweep is skipped).
- **Short and bounded**: a transition should end by itself (the clock's sweep takes 0.7 s, the
  calendar page's flip 0.45 s); measure the time with `Date.now()` and draw the state for "now",
  so a late frame jumps ahead instead of slowing the animation down. The dock's UI thread can be
  busy for a moment (loading icons at startup): pausing the animation's clock across such a stall
  (the `anim.last` check above) keeps it from being skipped.

**Loops that never cost a frame** (round 16 phase 16F): a picture whose parts move rigidly, as the
Weather widget's Meteocons do (a sun's rays turn, a cloud drifts, raindrops fall and fade), is drawn
as [composition layers](#canvas) (`canvas.layer` with `tracks`): each moving part gets a child
visual of the slot, drawn once, and its loop is handed to DirectComposition as cubic segments (the
same way the dock's own tweens go to DWM). The compositor animates them on the GPU; the widget, the
dock and the CPU stay idle (QueryState 58 / 59 do not move). Tracks are translate (`x`, `y`),
`rotate` and `opacity` keyframes with optional cubic-bezier easing and a phase (`begin`) to stagger
copies of one loop. Under reduce motion, in popups and previews the layers are drawn inline as a
still picture at their first keys, so a script needs no second code path. A redraw with the same
layer ids and options leaves the loops running (no restart jump); changed options restart them.
Keep the parts few and small: each is a surface and a visual (a Meteocons icon is 3 - 8).

Measured (5a, this machine, user config): the analog clock with a ticking second hand wakes 10
times in 10 s; with the smooth second hand ~300 times in 10 s while the dock is shown and 0 while
it is auto-hidden; without a second hand 0 in 10 s (once a minute); the sweep is ~45 frames.
Round 7: the hover growth (48 -> 77 px) is ~9 wake-ups per enter or leave (the box went 48, 56,
67, 70, 73, 75, 77 px in 132 ms, one dock layout each), none at rest.

## Clock faces

Two clocks draw faces from JSON files: the **Analog clock** widget (format 1, below) and the dock's
own **digital clock** (format 2, `clock.face`). They share these keys:

| Key | |
|---|---|
| `name` | Shown in the face drop-down (the widget's Face setting; a clock item's Face on its settings page). |
| `description` | Informational. |
| `format` | 1: an analog face; 2: a digital face. |
| `type` | `"digital"` for a digital face (required there); analog faces have none. |
| colours | `"#AARRGGBB"` / `"#RRGGBB"` (as in config.json); digital faces also take theme references (below). |
| fonts | `font` (a family name), `weight` (100 - 950), `size`: a ratio (analog: of the dial's radius; digital: of the built-in clock's size). |

A file of the other kind is not offered: the Analog clock widget lists `faces/*.json` of its own
package, the dock's clock only files with `"type": "digital"`.

### Analog clock faces (format 1)

The analog clock widget draws a face file: `faces/<name>.json` in its package (the built-ins:
`classic`, `minimal`, `modern-dark`, `roman`, `station`). To add one, export the widget (App
settings > Widget library > **Export built-in to folder**), put a new `.json` next to the others in
`%LocalAppData%\WinDock\widgets\analog-clock\faces\`, and press **Reload widgets**: the Face
drop-down lists it (`choiceFrom`). A face only needs the keys it changes; everything else has a
default. The dock's own digital clock has a face format of its own (format 2, below).

All lengths and widths are **fractions of the dial's radius** (inside the rim), so a face scales to
any size. Colours are `"#AARRGGBB"` / `"#RRGGBB"` (as in config.json). A *paint* is a colour or a
gradient: `{ "radial": [[0, "#FFFFFFFF"], [1, "#FFDDDDDD"]], "focusX": 0, "focusY": -0.3 }` (centred
on the dial, the focus moved by fractions of the radius) or `{ "linear": [[0, "#..."], [1, "#..."]],
"angle": 90 }` (degrees; 90 = top to bottom).

```jsonc
{
  "name": "Classic",                       // shown in the Face drop-down
  "description": "...",
  "format": 1,
  "dial": {
    "fill": paint,                          // the face
    "radius": 1,                            // of the half box (room for the shadow is kept)
    "rim": { "width": 0.08, "color": paint, "highlight": "#50FFFFFF" },   // a case ring
    "border": { "width": 0.02, "color": "#26000000" },                    // a line inside it
    "shadow": { "color": "#66000000", "size": 0.08, "offsetY": 0.04 },    // a soft drop shadow
    "gloss": "#14FFFFFF"                    // a highlight over the top half
  },
  "ticks": {
    "major": { "count": 12, "length": 0.13, "width": 0.045, "color": "#FF2B241C", "inset": 0.05, "shape": "line" },
    "minor": { "count": 60, "length": 0.05, "width": 0.016, "color": "#FF6B5E4E", "inset": 0.05, "shape": "line" }
  },                                        // shape: line | block (square ends) | dot; a minor tick under a major one is skipped
  "numerals": { "style": "arabic", "font": "Georgia", "size": 0.2, "weight": 600, "color": "#FF2B241C", "radius": 0.66 },
                                            // style: none | arabic | roman | cardinal (12, 3, 6, 9);
                                            // on a dial under 26 DIPs of radius arabic / roman show 12, 3, 6, 9 only
  "hands": {
    "hour":   { "length": 0.5,  "width": 0.11, "tail": 0.1,  "color": "#FF1C1814", "shape": "leaf" },
    "minute": { "length": 0.76, "width": 0.08, "tail": 0.12, "color": "#FF1C1814", "shape": "leaf" },
    "second": { "length": 0.84, "width": 0.018, "tail": 0.2, "color": "#FFB3261E", "shape": "line",
                "counterweight": 0.055, "tipDot": 0, "tipDotAt": 1 },
                                            // shape: taper | line | block | leaf; counterweight: a disc on
                                            // the tail; tipDot: a disc near the tip (station clocks), its
                                            // centre at tipDotAt of the length (1 = on the tip; less lets
                                            // the rod poke past it)
    "cap": { "radius": 0.055, "color": "#FF1C1814", "ring": "#FFB3261E", "ringWidth": 0.02 },
    "shadow": "#38000000"                   // a shadow under every hand (omit: none)
  },
  "date": { "fill": "#FFFFFFFF", "border": "#FF9A8A70", "color": "#FF2B241C", "font": "Segoe UI",
            "weight": 600, "size": 0.17, "radius": 0.52 },
                                            // the date windows (widget setting dateOnFace): at 3 o'clock
                                            // (day), 6 (month and day, year, month and year) and 12
                                            // (weekday and day); radius: their distance from the centre;
                                            // the numerals they cover are left out
  "text": { "value": "QUARTZ", "font": "Segoe UI", "size": 0.08, "weight": 600, "color": "#FF666666", "y": -0.35 }
                                            // optional: a line of text on the dial (y: from the centre)
}
```

(The comments are for this page only: face files are plain JSON.) A face that cannot be read is
logged (Console) and the default face is drawn; a face named in the settings that does not exist
falls back to the first file.

### Digital clock faces (format 2: the dock's clock)

The dock's built-in clock (a `clock` item) draws its `face` (round 5: each clock item's own
setting; `clock.face` is the default for new ones): `"digital"` (the default: its own look,
unchanged) or the id of a face file. Shipped faces: `default` (the built-in look as a face file,
pixel for pixel the same: a starting point), `large-time-small-date`, `mono-pill`, and (round 28) `seconds-line`, `compact`, `time-weekday`, `big-date`. Your own:
`%LocalAppData%\WinDock\clock-faces\<id>.json` (the clock item's settings page: **Open faces
folder**); a file with a shipped face's id replaces it. The **Face** drop-down of a clock item's
settings page lists them (the list is read again whenever the page is generated); a face that
cannot be read is not offered, and a configured face that is missing draws `"digital"`. The clock's
settings stay in force: its `format` (strftime), `showSeconds`, `showDate`, `label` (on the date's
line), `timeZone`, the click and the calendar
dot. Sizes are ratios of the built-in clock's: the time 16 DIPs (14 while the date is shown), the
date 11 DIPs; `seconds.size` / `suffix.size` are ratios of the time's size.

```jsonc
{
  "name": "Large time, small date",
  "type": "digital",
  "format": 2,
  "layout": "stacked",                     // stacked (time over date) | date-first | inline (one line)
  "align": "center",                       // center | start | end: the lines inside the block
  "separator": "  ",                       // inline: between the time and the date
  "lineGap": -2,                           // DIPs between the lines (may be negative)
  "time": { "font": "Segoe UI", "weight": 300, "size": 1.4, "color": "theme:text",
            "case": "as-is", "separator": ":" },      // separator: replaces ':' in the time
  "date": { "font": "Segoe UI", "weight": 600, "size": 0.8, "color": "theme:accent",
            "format": "%a %d %b", "case": "upper" }, // case: as-is | upper | lower
  "seconds": { "style": "small", "size": 0.55, "color": "theme:dateText" },
                                            // with clock.showSeconds: inline (as the built-in clock) |
                                            // small (a smaller run after the minutes) | line (their own
                                            // line under the time) | hidden (and the clock ticks once a minute)
  "suffix": { "style": "small", "size": 0.5 },
                                            // the 12-hour AM / PM (as %p gives it): as-is | small | lower | none
  "background": { "fill": "#2EFFFFFF", "border": "#26FFFFFF", "borderWidth": 1, "radius": 8, "padX": 8, "padY": 3 },
                                            // optional: a rounded pill behind the text (DIPs)
  "vertical": { "stack": true, "align": "center" }
                                            // left / right docks: every space breaks the line (an inline
                                            // face stacks), and the lines' alignment there
}
```

Colours (`color`, `fill`, `border`) are `"#AARRGGBB"` / `"#RRGGBB"` or a reference to the dock's
theme: `"theme:text"` (the clock's text), `"theme:dateText"`, `"theme:accent"`, `"theme:background"`,
`"theme:labelText"`, `"theme:labelBackground"`, `"theme:separator"`, `"theme:border"`; the defaults
are `theme:text` for the time, the seconds and the suffix, `theme:dateText` for the date. Every key
is optional; a value of the wrong kind makes the whole file unreadable (the page shows the field).
An analog clock on the dock is the Analog clock widget (Dock settings > Contents, the widget drawer), not a face.

## Limits and errors

- **Watchdog.** Every call into the script (an export, a timer callback, the jobs it queues such as
  promise reactions) may run at most `timeLimitMs` (default 50 ms). Past it the script is stopped
  where it is (an endless loop, a runaway regular expression, an async loop: none can catch it),
  the widget is **suspended** and shows a "Stopped" tile until it is reloaded. The dock itself
  keeps responding: at worst it waits that long once.
- **CPU budget** (round 25). The watchdog limits one call, not how many come: a widget whose calls
  (draws, measures, events, timers, provider callbacks, http results, together) take **more than
  5 seconds of the last 10** is suspended the same way, with "the script used the UI thread for
  ... s of the last 10 s" as its message. Every widget runs on the dock's one UI thread, so that
  is a widget keeping the dock half frozen; the bundled widgets stay far below it (a 60 frames a
  second animation that draws in about a millisecond uses under 10 %). Due timers also run in short
  slices (about 6 ms of callbacks, then input and painting get a turn), so many timers falling due
  together are spread over a few frames instead of running as one block.
- **Re-entry** (round 25). A call into a widget's script never starts while another of its calls
  is running. When something the script does leads straight back to it (calling
  `notifications.dismiss` / `dismissAll` / `markRead` / `activate`, or `calendar.refresh()`, changes
  what that provider reports, so its own `onChange` listeners are due), the listener call is kept
  and runs right after the current call returns, in order; the same timer, provider channel or
  storage key kept twice runs once, and so do repeated `onHover`, `onDrag`, `onVisibilityChange`,
  `onSettingsChanged` and `tick` calls (with the latest event). A listener whose own work keeps
  triggering it again is cut off after 8 such rounds (a warning in the log), and at most 64 calls
  are kept at a time. A widget may also see `shell.open` and `notifications.activate` take effect
  only after its function returned (see their entries).
- **Needs permission** (round 25). A widget from the widgets folder that the user has not allowed
  yet (see [Consent](#consent)) does not load its script at all: its tile says "Needs permission"
  (on a narrow tile "Permission" or "?") and a click on it, or **Review permissions…** in its
  right-click menu, opens the consent dialog. The Console lists it as "needs permission";
  `QueryState` 54 (and 244 for a standalone widget) report state 3.
- **Errors.** An uncaught exception in any export or timer callback (and a module that fails to
  load, a missing `draw` export, a package that is not installed) puts the widget in the **error**
  state: an "Error" tile with the first line of the message, the message and its stack
  (`file:line:column`) in the widget's log, its timers stopped, its script heap freed. An unhandled
  promise rejection is only logged.
- **Memory.** Each widget has its own runtime with a `memoryLimitMB` heap limit; running out is an
  error. Deep recursion throws a `RangeError` (the script's stack is limited well inside the dock's
  own).
- **Reload.** **Reload widgets** in the dock menu's Tools submenu (greyed while no dock has a
  widget), a widget's own menu (**Reload widget**), App settings > Widget library, the Console or `WinDock.DockCommand` 126 reloads every widget; the Console's Reload or
  `WinDock.ItemCommand` 809 reloads one. Reloading reads the package again (the folder version if
  there is one) and starts the script over (its storage stays). Editing config.json and **Reload
  config** also re-creates widgets whose items changed.

## Sandbox

A widget sees only the host API above; each widget has its own runtime and cannot reach another
widget's globals. Without permissions there is no file system, process or network access at all (text entry
through `popup.editText` needs none: the editor is a window of the dock that hands the script only
what the user typed and confirmed).
`"http"` opens requests to the listed hosts only; `"shell"` opens files and URLs, lists folders,
draws file icons, opens the folder popup, receives file drops and presses the `sendKeys`
allow-list; `"audio"` reads and changes the system volume / mutes / the default output; `"media"`
reads the current media session and controls its playback; `"notifications"` reads Windows'
notifications and dismisses them; `"calendar"` reads the dock's calendar feeds (and asks them to
refresh); `"weather"` reads the dock's forecast (and asks it to refresh); `"perf"` reads the
machine's performance counters and HWiNFO's shared sensor memory (nothing else); `"network"` (round
27) reads the state of the internet connection (online or not, its kind, the Wi-Fi network's name and
its signal bars); `"input"` (round 27) reads the keyboard layouts and switches the one in use (a
posted request to the window the user works in, or Windows + Space); `"notify"` (round 28) shows
a banner on screen, with the limits in [notify](#notify); `"links"` (round 29) opens http(s) pages in
the browser, only as the answer to a click ([links](#links)). A `secret` setting is stored protected
and never shown in the log; a host the user types into a `url` setting is reachable only after the
prompt ([http](#http), user hosts). Modules and images
are read
only from the widget's own package (and `media.art()` tokens, with `"media"`):

- `import` / `import()` specifiers are package paths: `"./x.js"` and `"../x.js"` relative to the
  importing module, anything else relative to the package root (`"lib/x.js"`).
- Rejected: absolute paths (`/x`, `\x`), drive letters (`C:/...`), URLs and anything with `:`,
  backslashes, empty segments, and any `..` that would leave the package.
- For folder widgets the file actually opened must be inside the widget's folder (a junction or
  symbolic link pointing elsewhere is refused), and every file is at most 8 MB.

### Consent

Round 25. Widgets written by other people are expected, so the permissions are a boundary the user
decides on:

- **Built-in widgets** (compiled into the exe) never ask.
- **A widget from the widgets folder** that asks for any permission, or whose id is a built-in's
  (it replaces that widget, its settings and its saved data), runs only after the user allowed it.
  Without that its script is not loaded at all (not even in the drawer's preview): it shows the
  "Needs permission" tile. A folder asking for nothing and replacing nothing needs no consent.
- **The question** comes when the user adds or installs such a widget through the settings or the
  drawer (App settings > Widget library's Add from folder…, Add to dock, Add to the desktop; the
  Contents tab's drawer; customize mode's drawer; the standalone widget picker), and from a click on
  its tile. It names the widget (name, id, version, author, folder), lists each permission in plain
  words (for `http` every host, a wildcard as "any sub-domain of", a local address as "this PC" /
  "your local network"), says so prominently when the folder replaces a built-in, and offers
  **Allow** / **Don't allow** (the default). Nothing asks at startup: widgets installed before
  round 25 show the tile until they are reviewed. Automation installs (`DockCommand` 141) never
  ask.
- **What is stored**: `widgetConsent` in config.json, the widget's id with the sorted list of what
  was allowed (`"shell"`, `"http:api.example.com"`, ...) and `"replacesBuiltIn"`. It is asked again
  only when the manifest asks for something that list does not hold: a new version with the same
  or fewer permissions keeps running. Undo / Redo never change it.
- **Review and revoke**: App settings > Widget library shows every widget's permissions and state;
  **Permissions…** opens the dialog (Keep allowed / Revoke for an allowed widget). Revoking stops the
  widget at once and brings the tile back.
- **Hosts the user types** (round 29): a widget with `hostSettings` lists "Internet: connect to the
  address you enter as ..." among its permissions; each host itself is asked about later, per item,
  when the user types it ([http](#http)). Those answers are stored with the consent
  (`widgetConsent.<id>.hosts`, `"<item key>|<setting key>": "<host>"`, also for built-in widgets);
  revoking the widget, uninstalling it or installing another zip of that id forgets them.

For authors: ask for the minimum. Every permission is one more line the user must accept, and
`"shell"` (which can run any program) is the one most users should refuse; prefer `http` with exact
host names over wildcards, and never list a local address unless the widget is for a device on the
user's network. `WinDock.exe --validate-widget` prints the lines the dialog will show.

## Developing a widget

- **Export a built-in** to start from it: App settings > **Widget library** lists every installed widget (name,
  id, source: built-in, folder, or folder replacing a built-in; version). **Export built-in to
  folder** writes its files to `widgets\<id>\` (refused if that folder exists), sub-folders such as
  the analog clock's `faces\` included, plus a `README.md` stub pointing here; the copy then
  replaces the built-in (delete the folder to get the built-in back).
- **Start from an example**: the repository's `examples\` folder has four small annotated widgets,
  each with a README: `minimal` (one line of text: `measure` and `draw`), `http-json` (a public JSON
  API with a host allow-list, `timer.setRefresh` + `tick`, `storage`), `popup-list` (a list in a
  script popup with the shell's icons and click handling) and `edit-text` (a label renamed through
  `popup.editText`). They are not built in: install one with
  **Add from folder…**. **Add from folder…** copies a folder that holds a widget.json into the
  widgets folder (its name, lowercased, becomes the id). **Add to dock** adds the selected widget
  to a dock (with several docks a small menu asks which); **Reload widgets**, **Open widgets
  folder** and **Console…** do what they say.
- **`"watch": true`** in widget.json (folder widgets): every change in the widget's folder (any
  file, sub-folders included) reloads it a quarter of a second after the first change (a burst of
  writes by an editor is one reload). One
  thread waits in `ReadDirectoryChangesW` on every watched folder while any watched widget is on a
  dock; nothing polls, and it costs nothing while the files do not change.
- **The Console** (App settings > Widget library > Console…, the dock menu's **Tools** > **Widget console…**,
  `WinDock.DockCommand` 127): every widget instance of every dock with its state (ok, error,
  stopped), the selected one's log (console output, errors with their stacks, the host's own
  lines: loading, reloads, file changes, shell.open), **Reload**, **Open folder** (the widget's
  folder, or the widgets folder for a built-in) and **Clear** (the view; the log is kept). It
  updates as lines arrive. Round 29: **Copy all** puts the log as shown (from the last Clear on) on
  the clipboard, **Save to file…** writes it to a text file (UTF-8, `<id>-log.txt` by default), for a
  bug report. A [secret](#settings-schema) setting's value is never in it: a line that contains
  the value shows `[secret]` in its place (the host's own lines never print it to begin with).
- **Editor help** (round 29): `docs/windock.d.ts` declares the whole API (every global, method,
  event, the export signatures, the manifest and its settings schema) with short doc comments, for
  VS Code or any TypeScript-aware editor. Put these two lines at the top of main.js:

  ```js
  // @ts-check
  /// <reference path="./windock.d.ts" />
  ```

  and `canvas.`, `dock.`, `settings.` ... complete with their documentation, wrong argument types are
  underlined, and `/** @type {ViberDock.Exports['draw']} */` above an export types its parameters.
  The dock ignores both lines (they are comments). **Export built-in to folder** writes a copy of the
  file and a `jsconfig.json` (ES2023 only, no DOM) into the exported folder, so the globals are known
  there without the reference line; the repository's examples reference `../../docs/windock.d.ts`.
  `// @ts-check` is optional: without it the editor still completes, it just does not underline.
  `tools/check_widget_docs.py` keeps the file in step with the bindings.
- **Errors** show on the widget's tile ("Error" / "Stopped" and the first line of the message).

## Validating a widget

The dock checks a widget's manifest every time it loads the widget (a reload, a dock start), and
`WinDock.exe --validate-widget <folder>` does the same for a folder that is not installed (for
authors and CI; no dock starts, nothing is shown). Each problem names the field and the fix:

```
> WinDock.exe --validate-widget C:\src\my-widget
ViberDock widget check: C:\src\my-widget
  name: My widget
  permissions (installed, it runs only after the user allows them):
    - Internet: connect to api.example.com
  error: main: "app.js" was not found in the widget (fix: add the module or set "main" to its path)
  error: permissions[1]: unknown permission "camera" (fix: use "shell", "audio", ... or { "http": ["host"] })
  warning: settings[2] (size).default: 500 is outside min - max (clamped)
FAILED: 2 errors, 1 warning
```

Round 25: after the name it prints the permissions as the consent dialog will list them ("none"
when it asks for nothing), and a note when the folder name is a built-in widget's id (installed,
it would replace that widget and ask for consent).

Round 29, what only the command line checks (the dock does not, to keep loading cheap):

- **Every script compiles**: each `.js` / `.mjs` file of the folder (at most 64) is parsed and
  compiled as a module, with its static imports, in a throwaway engine with no host API; nothing
  runs, not even the top-level code. A syntax error, or an import that is not a file of the
  widget, is an error with its place: `error: lib/util.js:12:7: SyntaxError: unexpected token in
  expression: ';'`. What it cannot see: errors that only happen when the code runs (a misspelt
  global, a wrong argument), which the Console shows.
- **Permissions in use** (warnings, textual): a permission that no script mentions (`"http"` asked
  for, no `http.` anywhere; `"shell"` counts `shell.`, `fileIcon`, `popup.folder`, `onDrop` and a
  `file` / `folder` setting), and the reverse for the calls that need one (`http.get(` without an
  `http` permission, `shell.open(` without `"shell"`, `notify.banner(` without `"notify"`).
- **What the user sees**: a missing `description` (the Widget library and the drawer show it),
  `version` or `author` (the consent dialog shows both).

The manifest has no icon: the drawer and the library show the widget's own live preview
([Preview mode](#preview-mode)), so there is nothing to check for one.

Exit code **0**: no errors (warnings may be listed), **1**: errors, **2**: no folder given, or no
widget.json in it. The output goes to the console that started it (or a redirected standard
output, UTF-8). Errors: widget.json that is not a JSON object; `main` that is not a package path or
names no file (also the default `main.js` missing); `size` (not an object, a missing or non-number
`main` / `cross`, outside 4 - 1024 / 4 - 512); `permissions` (not an array, an unknown permission,
`"http"` without hosts, a host with a scheme, path or port); the settings schema (an entry that is
not an object, no `key`, a duplicate key, an unknown `type`, a `choice` without `choices` or
`choiceFrom`, a `choiceFrom` glob that is not a package path or matches no file (`"perf"` is not a glob: the
perf metrics), `min` above `max`,
`step` 0 or less, `visibleIf` naming no other setting); the folder name is not a valid widget id.
Warnings: unknown keys (top level and in schema entries: ignored), defaults of the wrong type or
out of range (the type's default or the clamped value is used), `refreshMs` / `timeLimitMs` /
`memoryLimitMB` out of range (clamped), a `choiceFrom` file that is not a JSON object.

On a dock, a widget with an error does not run: its tile shows "Error" and the first problem
(`widget.json: permissions[1]: ...`), and the Console lists every problem with its fix; warnings
go to the Console only.

Round 29: given a `.zip` or `.viberwidget` file instead of a folder, the check reads the archive
exactly as **Install from zip…** does (see [Publishing a widget](#publishing-a-widget)), unpacks it
into a staging folder under `%LocalAppData%\WinDock` (removed again) and checks that folder; the
output adds `archive: <n> files, installs as "<id>"`. Run it on the file you are about to publish.

## Publishing a widget

Round 29. To give a widget to other people, zip its folder:

```
my-widget.zip
  my-widget\            the only top-level folder: its name (lower case) is the widget's id
    widget.json
    main.js
    lib\util.js         any modules and assets, as in the folder
    windock.d.ts        optional (editor help; the dock never reads it)
```

A zip whose root holds `widget.json` directly works too: the id is then the zip's file name, lower
case, with anything but a-z 0-9 - _ . turned into `-` (`My Clock (2).zip` installs as
`my-clock-2`). `.viberwidget` is the same format under its own extension. Check the file with
`WinDock.exe --validate-widget my-widget.zip` before you publish it.

**What the user does**: App settings > Widget library > More ▾ > **Install from zip…**. What
ViberDock does, in this order, refusing the whole archive at the first problem (the reason is
shown under the library's buttons):

1. Reads the archive (at most 20 MB) with its own reader: only the stored and deflate methods, no
   encryption, no zip64, at most 200 entries, a file at most 8 MB and all files at most 20 MB
   unpacked (as declared, and checked again while unpacking: a file never grows past its declared
   size), no file above 64 KB compressed more than 100 to 1, every CRC-32 checked.
2. Checks every name: no absolute paths, drive letters, `:` (alternate data streams), `..` or `.`
   segments, empty segments, names ending in a dot or a space, the characters `< > " | ? *` or
   control characters, reserved device names (`CON`, `NUL`, `COM1`, `LPT1`, ..., also `con.txt`),
   non-ASCII names unless the archive marks them UTF-8, two names that differ only in case, more
   than 16 levels; no symbolic links or reparse points. A Finder-made `__MACOSX/` folder and
   `.DS_Store` files are skipped.
3. Finds the widget: `widget.json` at the root, or inside the one top-level folder.
4. Unpacks into a fresh staging folder `%LocalAppData%\WinDock\install-<unique>\<id>\` (every file
   created new) and runs the [widget check](#validating-a-widget) on it, the script compile
   included; an error refuses it. Nothing of the archive runs.
5. If a widget with that id is installed, asks whether to replace it (the old folder goes to the
   Recycle Bin, its `widget-data` stays). Then moves the folder into the widgets folder.
6. Drops any consent the user gave an earlier widget of that id: the new one asks for its
   permissions like any widget from the widgets folder ([Consent](#consent)) and shows **Needs
   permission** until the user allows them. A widget that asks for nothing and replaces nothing
   runs at once.

**How your permissions read to the user**: the consent dialog lists one line per permission in
plain words, for example "Internet: connect to api.example.com", "Shell: open files, folders and
web pages, and run programs on this PC (with any arguments) ...", "Banners: show banners on
screen ...". Ask for as little as you can: every line is one more reason to click Don't allow, and
`"shell"` reads as what it is. Give `name`, `version`, `author` and `description` in the manifest:
the dialog and the library show them (`--validate-widget` warns when one is missing).

**Uninstall** (Widget library > More ▾ > **Uninstall…**) moves the widget's folder to the Recycle
Bin after a confirmation that names it and how many docks and desktop widgets use it; its
`widget-data\<id>.json` goes too only when the user ticks the box. Its consent is forgotten. Items
that showed it stay and show the "not installed" error tile. Built-in widgets cannot be
uninstalled; uninstalling a folder that replaced a built-in brings the built-in back.

## Standalone widgets

A widget can also live on the desktop by itself, in a window of its own (round 6 phase 6B): an
analog clock or the weather without a dock, moved and resized freely. Each one is a `docks[]`
entry of kind `"widget"` (README "Standalone widgets"); its window's client area **is** the widget's
box:

- `dock.standalone` is `true`; `dock.width` / `dock.height` are the window's size in DIPs (the
  canvas is the same size); `measure()` and the manifest's `size` hints are not used (the window
  decides the size); `dock.iconSize` is the shorter side / 1.5, so a widget that draws from
  `dock.iconSize` or from `canvas.width` / `canvas.height` grows with the window. A widget that
  draws fixed-size text can scale it: `const k = dock.standalone ? Math.min(w / 64, h / 40) : 1`
  (the built-in `hello`, `digital-clock`, `weather`, `agenda`, `media` and `battery` do this; the clocks, the
  calendar page and the icon-like widgets scale from the canvas already).
- The manifest's `standalone` object sets the smallest window, the aspect and the default size
  (see [widget.json](#widgetjson)); `"standalone": { "supported": false }` keeps the widget on
  docks (for a widget that means nothing without one).
- Round 16 phase 16E: a widget **dragged up off a dock** (into the zone where a pin would be
  removed; it dims there) moves to the desktop as a standalone widget centred under the pointer.
  The item's `settings` carry over; `storage` is per widget id, so the standalone widget sees the
  same store. Its running state does not: the dock's instance is released and the window starts a
  new one (`init()` runs again, timers and popups of the old instance are gone). Not while the dock
  is locked, and not for a widget whose manifest has `"supported": false`.
- Round 16 phase 16G, the reverse: a standalone widget dragged onto a dock's bar (or moved with its
  menu's **Move to dock**) becomes an item of that dock with the same `settings`; the same applies:
  `storage` is shared (per widget id), the window's instance is released and the dock starts a new
  one (`init()` runs again, now with `dock.standalone === false`).
- Input: `onPress`, `onClick`, `onMiddleClick`, `onWheel`, `onHover` and `onDrop` work as on a dock,
  with coordinates in DIPs from the window's top-left. A left **press and release without moving**
  is the widget's click; a left **drag** (past the system drag distance) moves the window instead
  (the widget saw the press, but no click follows), and an 8 DIP band along the edges and corners
  resizes it, while the entry is unlocked. A right click opens the window's menu, not the widget's.
  Round 23: a press `onPress` claims (returns `true`) never moves the window: its moves go to
  `onDrag` and its end to `onRelease` (the same on a dock: no item drag starts from it).
  `widget.resize(w, h)` sets the window's size.
- Popups (`popup.open`, the passive kind, `popup.folder`, `popup.calendar`,
  `notifications.openCentre()`) open above the window, centred on it, or below it when there is no
  room above; so does the text editor of `popup.editText` (owned by the window). `popup.previews` has no app windows to show there: it does nothing (a line in the
  Console). The notification centre opened from a standalone widget shows no app icons.
- A click on the widget while its popup is open closes the popup and arrives with
  `closedPopup: true`, as on a dock.

## Preview mode

Round 6 phase 6E. The settings windows show every installed widget **live**: Dock settings >
Contents > the widget drawer draws each script widget's row with its preview (72 x 44 DIPs on the
left of the row; the letter tile while it renders or when the widget fails), the inspector shows the
selected widget item's preview larger (fitted into 200 x 100 DIPs, above its name; the preview's
settings, not the item's own), and App settings >
Widget library shows the selected widget fitted into 240 x 120 DIPs beside its description.

Each preview is the widget's own `draw()`, run once by a **preview instance**:

- It sits on a virtual dock: horizontal, bottom edge, icon size 48 DIPs, the global theme's colours
  (`dock.theme`), the settings window's DPI scale, `dock.reduceMotion` true (no animation would get
  past its first frame), `dock.visible` true, `dock.id` `"preview"`, **`dock.preview` true**. Its
  box is what `measure()` (else the size hint) returns, or the manifest's `preview.width` /
  `height`; a box larger than the preview area is drawn at a smaller `dock.scale` (the canvas is
  vector: it stays sharp), the inspector and the library draw a smaller one up to twice as large.
- Its settings are the schema's defaults with the manifest's `preview.settings` over them (not any
  item's settings).
- It is never registered with the dock's widget runtime: it is not in the Console, has no timers,
  no `watch`, no provider service, and it is destroyed (its script heap freed) right after the draw.
  Its `console` lines stay in its own log, which goes with it.
- Every side effect is a no-op: `setTimeout` / `setInterval` return an id and never fire,
  `timer.setRefresh` and `requestRedraw` do nothing (a clock shows the time it was drawn),
  `http.*` returns a promise rejected with `"http: not available in preview"`, `storage.set` /
  `remove` / `clear` write to a scratch store of the preview only (reads see those writes over the
  widget's real store, which is read but never written), `shell.open` and `shell.sendKeys` return
  `false` and do nothing, `popup.open` / `close` / `folder` / `calendar` / `previews` do nothing,
  `popup.editText` returns a rejected promise, `audio.setVolume` and the other setters, `media.play()` ... and `media.seek()`, `widget.resize()` (returns false), `notifications.dismiss`
  ... and `weather.refresh()` / `openSettings()` change nothing, `onChange` listeners are kept and
  never called. Permissions are checked as always (a call without its permission still throws).
- The providers answer with **sample data**, in exactly the shapes of the real calls (the samples
  live in `src/WidgetSamples.cpp`; no service is started, nothing of the machine is read):

| Provider | Sample |
|---|---|
| `audio.get()` | `available` true, `volume` 0.62, not muted, `device` "Speakers (Sample)", a microphone ("Microphone (Sample)", 0.8, not muted), two outputs (Speakers, Headphones). |
| `media.get()` / `media.art()` | Playing "Sample Song" by "Sample Artist" from "Sample Album" (`app` "Sample.Player"), `position` 73 s of `duration` 214, every `can*` true (`canSeek` too); `art()` is `"media:art/1"`: a generated 64 x 64 violet-to-orange gradient square that `canvas.image` draws. |
| `notifications` | `state` "polling", 3 unread: "Mail" (5 minutes ago, "Sam Rivera"), "Mail" (20 minutes ago, "Weekly report"), "Calendar" (an hour ago, "Stand-up"), newest first. |
| `calendar` | One feed "Sample" in the first feed colour (`#FF4CC2FF`); today "Stand-up" 09:30 - 09:45 and "Lunch with Sam" 12:30 - 13:30 (local time, `recurring`); `eventsOn` another day is empty; `next()` lists today's events that have not ended, after 13:30 the same two tomorrow (so an agenda never previews "No events"); `events()` covers today and tomorrow. |
| `weather.get()` | `state` "fresh" (fetched 5 minutes ago), "Sample City", sunny (code 0, day), 21 °C or 70 °F (the default `units`), wind 12 km/h / 7 mph, humidity 45 %, feels like 20 °C / 68 °F, UV 3, 24 hourly columns from the current hour (a mild day / night curve), 7 days from today (WMO codes 0, 1, 2, 3, 61, 2, 0: clear, through overcast and rain, to clear; highs 23 - 16 °C; sunrise 07:10 and sunset 17:40 local, highest UV 4, 1 on the rain day). `weather.icon()` works as always. |
| `network.get()` | Round 27: `available`, `connected` and `internet` true, `state` "internet", `kind` "wifi", `name` "Sample Wi-Fi", `bars` 4, `signal` 80, not `metered`. |
| `input.get()` | Round 27: English (United States) current (`id` "04090409", `tag` "en-US", `code` "ENG", `region` "US", `layout` "US"), then German (Germany) (`id` "04070407", "de-DE", "DEU", "DE", "German"). `input.set` / `input.next` switch nothing and return true. |
| `perf` | `get()`: serial 1, interval 1000, `cpu.total` 37, eight cores, `cpu.frequency` 4180, `memory.percent` 58, `memory.used` / `total` 9.3 / 16 GB, `gpu.total` 12, `disk.total` 4, `network.down` 120 KB/s, `up` 18 KB/s, `system.uptime` 3 days 4 hours, a laptop on battery (round 7: `battery.percent` 74, `battery.charging` 0, `battery.acLine` 0, `battery.timeLeft` 11400 s = 3 h 10 min); `metrics()` lists those keys (35); `series(key, n)` up to 60 samples (a sine wave with noise, the newest one the snapshot's value; the same numbers every time); `hwinfo()` `{ available: false, reason: "preview" }`; `setInterval` answers 1000; round 28: `drives()` C: "Windows" 1 TB with 380 GB free and D: "Data" 2 TB with 1.2 TB free. |
| `notify.banner()` | Round 28: shows nothing and returns false (the permission and the title are checked). |
| `settings.set()`, `links.open()` | Round 29: change / open nothing and return false (the key, the value and the URL are checked as always). |

The manifest's `preview` object (see [widget.json](#widgetjson)) picks what the preview shows:

```json
"preview": { "settings": { "face": "classic", "seconds": true, "dateOnFace": "day" } }
```

The built-ins: the Analog clock previews the classic face with a second hand and the date window,
the Digital clock with its date line, the Weather widget with a three-day forecast on the slot
(round 7), the System monitor as a CPU graph, the Battery widget the sample laptop with its time
left, Show desktop its desktop glyph (round 11: a 10 DIP strip would be a faint line in the
drawer); the Notes widget draws a sample note when `dock.preview` is true; the others preview as
they are. A widget that throws (or fails to load, or measures an empty box) gets no preview: its row
keeps the letter tile and the failure goes to the debugger output once per widget id
(`WinDock preview <id>: <message>`).

**When previews render**: lazily, when a row is painted (a paint renders for up to 40 ms; the rows
it had no time for render on the next paint), once per (widget, box) per settings window. A window
drops its previews when the global theme or the units change, its DPI changes, the widgets are
reloaded (Reload widgets, a `watch` folder changed) or `WinDock.DockCommand` 148 asks; they render
again as they are painted. Measured (STATUS.md 6E): 1.5 - 10 ms per built-in widget. Nothing renders
while no settings window paints a row.

**Test hook**: when the environment variable `WINDOCK_PREVIEW_LOG` names a file, every render
appends a line to it: `ok <id> <w>x<h> <ms>` or `fail <id> <error text>` (a probe widget can report
what it sees in preview mode by throwing it).

## Adding a widget to a dock

The widget drawer of Dock settings > Contents lists the installed widgets (built-in and from the
widgets folder, each with its [live preview](#preview-mode)) under the built-in kinds (App settings > Widget library has **Add to dock**
too): drag a widget onto the item strip to put it there, or press its **Add** to add it before a
clock that ends the dock (else at the end); it can then be moved like any item, and **Open
settings in its own window** opens its settings. In config.json:

```json
{ "kind": "widget", "id": "digital-clock", "settings": { "showSeconds": true, "showDate": true } }
```

The same widget can be on a dock several times (each item is its own instance with its own
settings and state) and on several docks. Like every item it can take `"anchor"` (`"start"` /
`"center"` / `"end"`) and `"offset"` (DIPs) for its place along the bar (round 7; README, Position
> Item anchors); the widget's script sees nothing of it.

**On the desktop**: the drawer's **Desktop** button on a script widget's row (and **Add to the
desktop** in App settings > Widget library, **Add standalone widget…** in Docks and displays)
makes it a [standalone widget](#standalone-widgets): a window of the manifest's default size,
centred on the dock's display, under the other windows; its settings open in a panel beside it
(round 12 phase 12I; a captioned window before). In config.json:

```json
{ "id": "clock1", "kind": "widget", "widget": "analog-clock", "x": 100, "y": 100, "w": 200, "h": 200 }
```

## Examples

The repository's `examples\` folder holds four small annotated widgets to start from (README.md
in each; install with App settings > Widget library > **More ▾** > **Add from folder…**): `examples/minimal`,
`examples/http-json`, `examples/popup-list`, `examples/edit-text` ([Developing a widget](#developing-a-widget)). The
built-ins are worked examples too:

- `widgets/feed` ("Headlines", round 29): the newest items of an RSS or Atom feed. The worked
  example of a widget that reaches a server the user chooses: its manifest names no host
  (`{ "http": [], "hostSettings": ["feedUrl"] }` and `"links"`), the feed's address is a `url`
  setting, so ViberDock asks before the typed host is reached; `timer.setRefresh` every
  `refreshMinutes`; a small tolerant parser (RSS 2.0, RSS 1.0, Atom; CDATA and entities) with
  `indexOf` scans over at most the first 512 KB of the answer; the tile rotates the headlines with
  one `setTimeout` while `dock.visible`; the popup lists them and `links.open` opens a clicked one.
- `widgets/hello`: a label and a counter. `onClick` counts, `onMiddleClick` resets, `onWheel`
  counts up / down and returns `true`, `onHover` highlights the border; every handler ends with
  `requestRedraw()`.
- `widgets/digital-clock`: a scripted copy of the dock's own clock (the same box and pixels for
  the same settings). `measure()` sizes the box from `canvas.measureText`, `timer.setRefresh`
  wakes it once a minute (or once a second with seconds), `system.format` formats the time, and on
  a vertical dock every space breaks the line. Round 5: `timeZone` (a `timezone` field: the time of
  any Windows zone, passed to `system.format` and `setRefresh`; once per load or settings change it
  logs the zone, its UTC offset and the time it shows, and it warns about a zone this Windows does
  not know) and `label` (drawn small under the time: before the date, or alone, as the built-in
  clock draws its label). Round 11 phase 11B (1.1.0): `dateFormat` (strftime, default
  `"%a %d %b"`, the old fixed date; shown only with `showDate`) and `timeStyle` `"stacked"` (as
  before: the time semibold, a smaller date line in `dock.theme.dateText`) or `"taskbar"` (the
  Windows 7 clock: the time over the date in two centred lines of the same size and weight, both
  in `dock.theme.text`, 4 DIPs of room either side). `system.format` passes MSVC's `#` flag
  through, so `"%#I:%M %p"` gives `2:05 PM` and `"%#m/%#d/%Y"` `11/6/2026`.
- `widgets/analog-clock`: faces as data (`faces/*.json`, [the format](#analog-clock-faces-format-1)) listed
  by a `choiceFrom` setting and read with `widget.readJson`; settings (grouped: Face, Second hand,
  Motion and size; the motion shown only with a second hand: `visibleIf`) `face`, `seconds` (second
  hand), `secondsMotion` (`ticking`: one wake-up a second; `smooth`: 30 frames a second while the
  dock is visible), `dateOnFace` (`none`, `day`, `month-day`, `year`, `full`), `showDigital` (the
  time under the dial), `sweep` (the hands sweep in when it appears or its face changes), `size`
  (DIPs, 0 = the dock's icon size; the box is square), and (round 5, group "Time zone")
  `timeZone` (the hands, the date on the face and the digital time follow `system.localTime(zone)`)
  and `label` (drawn small under the dial, before the digital time when that is shown). Round 7
  (1.1.0): `hoverScale` (group "Motion and size", a `number` 1 - 2.5, step 0.1, default 1.6; 1 =
  off): while the pointer is over the clock, `measure()` returns the box times a factor that eases
  from 1 to `hoverScale` in 150 ms (ease-out, a 16 ms `setInterval` only during the ease, each frame
  a `requestRedraw()`, which re-measures, so the dock lays out again and the neighbours make room)
  and back on leave; with reduce motion (or off screen) it jumps; no timer at rest; never
  standalone or in a preview. The grown box is thicker than the bar, so it stands on the bar's near
  edge and grows outward. The [Animation](#animation) example.
- `widgets/analog-date` ("Calendar page"): today's date as a page with a coloured header (the
  month, or the weekday) over a big day number; `style` `flip` (split page, stacked pages, a curled
  corner, the page flips over when the date changes) or `flat`; `showMonth`, `showWeekday`,
  `showYear`, `accent` (header colour), `click` (`calendar`: `popup.calendar()`, `target`:
  `shell.open(clickTarget)`, `none`), `size`, `timeZone` (round 5: the date of another zone). One
  wake-up a day: `timer.setRefresh(86400000, { timeZone })` wakes it at midnight of its zone (local
  midnight without one). Test hook: an item setting `"debugNow": "2026-12-31T23:59:57"`
  runs it on a fake clock starting at that local time (the midnight timer becomes a `setTimeout`
  to the fake midnight) and writes each date it shows to its storage (`debugShown`).
- `widgets/volume`: `audio.get()` / `audio.onChange` for the glyph and the percentage; `onWheel`
  steps `settings.step` % (default 2) on the step grid and unmutes on the way up; `onMiddleClick`
  mutes (`middleClick`: `mute` / `none`); the popup (`drawPopup` with hit rectangles for
  `onPopupClick`): slider, Mute, Mute microphone, the outputs (`audio.setDevice`), Sound settings
  (`shell.open('ms-settings:sound')`). Each notch also opens the volume bar, a passive popup
  (`popup.open(44, 150, { passive: true })`, told apart in `drawPopup` by its size) that one
  `setTimeout` closes 1.2 s after the last notch; with the ordinary popup open the wheel redraws
  that one instead. Round 22 (1.1.0): every view (`style`: `icon`, `ring`, `bar-horizontal`,
  `bar-vertical`) is a design measured in one unit (`dock.iconSize` x `settings.scale`) that
  returns its box and a paint function, so `measure()` and `draw()` share it; a standalone window
  fits the design to itself, as a row or a column, whichever comes out larger. The icon
  (`icon`: `speaker`, `bars`, `headphones`, `note`, `image`, `none`) is drawn with paths, or from
  `settings.image` (a `file` field): `canvas.image` in proportion (`canvas.imageSize`) for a
  picture, `canvas.fileIcon` for any other file. Settings `style`, `icon`, `image`, `scale`,
  `showPercent`, `font`, `wheelBar`, `iconColor`, `textColor`, `levelColor`, `step`,
  `middleClick`. Permissions `audio`, `shell`.
- `widgets/media` ("Now playing"): `media.art()` drawn with `canvas.image` in a rounded clip, the
  note glyph when there is no art, title / artist beside it (faded at the edge, or scrolling); a
  middle click opens a popup with the album and the controls; the wheel changes the volume in 2 %
  steps and shows the same volume bar as `widgets/volume` (its own copy of the drawing: widgets
  cannot import each other). Settings `showText`, `textWidth`, `wheelVolume` (default on) and
  (round 7, 1.1.0) **`controls`**: `"hover"` (default: previous / play-pause / next over the whole
  face, art and text alike, while hovered; a click on a third sends it), `"always"` (the same,
  always drawn, over a lighter shade that dims the art and text a little) or `"sides"` (previous
  left of the art and next right of the text (right of the art without text), above / below the
  art on a left / right dock; the slot grows by two buttons of `round(iconSize x 0.6)` DIPs; play /
  pause over the art while hovered, else a small badge in the art's corner showing what a click
  does; the side buttons are drawn, dimmed, without a session too; clicks: the side buttons send
  previous / next, anywhere else toggles); **`marqueeMode`**: `"hover"` (default: the title and the
  artist scroll while hovered, each only if it is wider than the text column), `"always"` (they
  scroll whenever one overflows and `dock.visible`; `onVisibilityChange` stops the frames, never in
  a preview) or `"off"` (fade at the edge only); **`marqueeSpeed`** (5 - 120 DIPs a second, default
  28): the frames come every `clamp(round(1000 / speed), 33, 100)` ms (10 - 30 a second), so a slow
  scroll wakes the dock less often; none with reduce motion. The 1.0 `marquee` bool is no longer read
  (`marqueeMode` is authoritative; an old `"marquee": false` scrolls on hover again: pick "Never").
  Round 23 (1.2.0), the **desktop mini players**: `layout` `"tile"` (default: the above) or, only
  while `dock.standalone`, `"deck"` (356 x 100), `"capsule"` (360 x 104, shaped: drawn on the
  transparent window), `"cover"` (228 x 364) or `"disc"` (204 x 232, shaped), each drawn at its
  natural size under one `translate` + `scaleBy` that fits it into the window (centred; the Deck's
  display and the Capsule's pill take extra width instead of leaving it empty); `skin` `"theme"`
  (`dock.theme` colours), `"silver"`, `"night"` (Consolas in the display); `accentColor`
  (transparent: the skin's); `displayColor` (the display's text; transparent: the skin's, Night
  following a set `accentColor`); `showProgress`; `showVolume` (Deck, Cover). These five use
  `visibleIf` arrays. The transport buttons, the progress bar (the Disc's rim ring, by angle) and
  the volume slider **claim** their presses (`onPress` returns `true`), so they work with
  `onDrag` / `onRelease` (a button acts on release over it, drawn brighter while held; the bar
  previews while dragged and calls `media.seek` on release when `canSeek`; the slider calls
  `audio.setVolume` per 1 % step, unmuting), while a press on the art or the panel still moves the
  window. The position is carried forward from `media.get()`'s and redrawn by one 1 s
  `setInterval` that runs only while playing, visible, with the progress shown and a mini layout;
  `onSettingsChanged` calls `widget.resize` with the new layout's natural size (the tile's: the
  manifest's 340 x 96). Permissions `media`, `audio`.
- `widgets/start`: `shell.sendKeys('start-near')` (or `'win'`, below), falling back to `shell.open('shell:AppsFolder')` when
  Windows refuses the input; `icon` `windows` / `mono` / `image` with `image` (a package file, or
  any absolute path whose shell icon `canvas.fileIcon` draws), `size` (percent of the slot). Round
  11 phase 11B (1.1.0): `icon` **`orb`**, the Windows 7 Start orb drawn with canvas calls only: a
  sphere of radial gradients (navy rim, a lighter glow from below), the four-colour flag as four
  `canvas.path` panes whose horizontal edges follow one S-curve (the left column lower), an
  elliptical highlight and a radial gloss over the upper half, a thin light rim. Hovered
  (`onHover`) the sphere's colours are 20 % lighter and a radial-gradient glow rings it; pressed
  (`onPress`, until `onClick` or the pointer leaving, with one 2 s `setTimeout` as a backstop for a
  press that turns into a drag) they are 20 % darker. `size` 0 (the new default) is automatic: 62 %
  for the logos and images (the old default), 100 % for the orb (it fills 92 % of the slot's
  shorter side). Nothing runs at rest. Round 16 phase 16A (1.2.0): `menuPosition` (Content, "Start
  menu"): `near` (the default) sends `shell.sendKeys('start-near')`, so the menu opens beside the
  button; `windows` sends `'win'` (Windows' own place).
- `widgets/stocks` ("Stocks", round 16 phase 16C): quotes from Finnhub's REST API
  (`https://finnhub.io/api/v1/quote`, permission `{ "http": ["finnhub.io"] }`; the key goes in the
  `X-Finnhub-Token` header, never in the URL). `provider` (`finnhub`, the only one so far: the
  script's `PROVIDERS` table maps a source to its URL, headers, answer parser and error texts, so a
  second API is one more entry and one more choice), `apiKey` (empty: nothing is fetched, the slot
  reads "Stocks: no API key" and the popup says where to get one), `symbols` (comma-separated, as
  Finnhub spells them, up to 20, upper-cased), `view` `single` (one symbol: the wheel steps through
  them, `cycleSeconds` > 0 shows the next one on an interval that stops while the dock is hidden) or
  `all` (every symbol: `layout` `auto` / `row` / `list`: cells side by side on a top / bottom dock,
  stacked cells on a left / right dock, a table with a line per symbol standalone or as an explicit
  list on a top / bottom dock), `refreshMinutes` (`timer.setRefresh`; `tick()` fetches every symbol,
  4 requests at a time, and stops the round on a refused key or a 429), `clickOpens` `popup` /
  `refresh` / `none` (a middle click always fetches), `rowLink` (the page a popup row opens: Yahoo
  Finance, Google Finance, Finnhub, none; permission `shell`); `change` `percent` / `amount` / `both` /
  `none`, `showSymbol`, `arrows` (▲ ▼), `sparkline` (the prices this widget fetched over the last
  day, up to 200 points, drawn beside the price), `updatedLabel` `none` / `below` / `beside` (a
  9 DIP "Updated 3 min ago" line under the prices, or after them on a row; "Fetching…" while a
  round runs, "Not updated yet" before the first answer; kept current by one timeout to the next
  minute of the newest fetch's age, only while it is shown and the dock is visible),
  `upColor` / `downColor`, `textColor`, `background`.
  Storage, shared by every Stocks item: `quote:<SYMBOL>` (the last quote: `price`, `change`, `pct`,
  `high`, `low`, `open`, `prev`, `t`, `at`) and `hist:<SYMBOL>` (the sparkline's points);
  `storage.onChange` picks up another item's fetch. A quote older than two refreshes, or from the
  store before this run fetched it, is drawn at 60 %. The popup (340 wide, 44 per symbol): each
  symbol with its last trade time, the price, the amount and percent change, O / H / L / PC, the
  error if any, "Updated N min ago" (one timeout a minute while open), a refresh button and the
  Finnhub credit. Previews (`dock.preview`) draw sample quotes (AAPL, MSFT, NVDA) and a sample
  sparkline. Test hook: with the item setting `debugStore` the slot and the popup write what they
  show to `slot` / `popup`. Standalone 240 x 140 by default, the layout scaled to the window.
- `widgets/photo-frame` ("Photo frame", round 17 phase 17A): one picture, or a folder of pictures
  as a slideshow, in a frame. `source` `image` (the `file` field `image`) or `folder` (the
  `folder` field `folder`: its own files with an image extension, read with `shell.list`, up to
  1000, read again at the end of each round); `order` `name-asc` (Explorer's name order, so 2
  before 10) / `name-desc` / `oldest` / `newest` (the modified time) / `random` (a shuffle per
  round); `interval` 1 - 3600 s (one `setTimeout` to the next picture, none while the dock is
  hidden, with `pauseOnHover` none while the pointer rests on the frame); `transition` `none` /
  `fade` (the new picture fades in over the old one) / `slide-left` / `-right` / `-top` /
  `-bottom` (the new one slides in from that side while the old one slides out), `transitionMs`
  100 - 3000: the frames are the script's own `setInterval(16)` for that long only, never with
  Reduce motion or in a preview; `click` `next` (the next picture; a single picture: open it) /
  `open` / `folder` / `none`, a middle click always opens the picture, `wheel` steps through a
  slideshow. Appearance: `size` `icon` / `small` 64 / `medium` 96 / `large` 160 / `custom`
  (`width`) DIPs across on a dock, `aspect` `image` (the picture's own shape: `canvas.imageSize`
  in `measure()`, so a slideshow's frame changes shape with each picture) / `1:1` / `4:3` /
  `3:2` / `16:9` / `3:4` / `2:3` / `9:16` / `custom` (`width` : `height`); standalone the window
  is the frame and a fixed aspect is the largest picture of that shape that fits, centred;
  `fit` `cover` (cropped, centred) / `contain` (bands in `background`, `#00000000` = none);
  `border` `none` / `line` / `mat` / `polaroid` (the bottom band three times `borderWidth`),
  `borderWidth`, `borderColor`, `cornerRadius`; `shadow` `none` / `soft` / `medium` / `strong`
  (a stack of eight translucent rounded rectangles growing outward, no blur) and `shadowOffset`
  (downwards). `measure()` adds the border and the shadow's room to the picture. The next picture
  of a slideshow is decoded at the end of `draw()` (`imageSize`), so the change is instant.
  Previews draw a sample landscape with canvas calls. A picture that cannot be opened is skipped
  (the frame says so when it is the only one). Test hook: `"debugStore": true` keeps the last 50
  paths shown in `"shown:" + dock.itemKey` and the current layout in `"state:" + dock.itemKey`
  (`tools\smoke\photo-frame.ps1`). Permission `shell`.
- `widgets/show-desktop` ("Show desktop", round 11 phase 11B): the Windows 7 taskbar's sliver. A
  click sends `win+d` (`click` `desktop`, the default: show the desktop, a second click brings the
  windows back) or `win+m` (`minimize`). `style` `strip` (default): `measure()` returns `width`
  (6 - 24 DIPs, default 10) along the dock and the icon size across, with `flush: true` (round 11
  phase 11C: on a full-width dock it is as thick as the bar and, as the last item, touches the
  screen's end); the strip is the theme's text
  colour at 10 % (25 % hovered, 40 % pressed) with a one-pixel hairline at 35 % on the side towards
  the other items (left on a top / bottom dock, top on a left / right one). `style` `icon`: an
  icon-sized slot with a monitor glyph in the text colour (a standalone widget always draws the
  glyph; so does the drawer preview, through the manifest's `preview.settings`). Aero Peek
  (hovering the sliver turns the windows into glass) is not done: it needs DWM's peek on every
  window, which no API offers an app. Permission `shell`.
- `widgets/search`: `shell.sendKeys('win+s')`; `showLabel`, `label`, `image`.
- `widgets/agenda` ("Agenda"): the next event of the calendar feeds (`calendar.next`) in the slot:
  how long until it starts ("In 25 min", "Now", the time, "Tomorrow 9:00 AM") over its title, with
  the feed's colour bar; a click opens a popup with the next `count` events (a row opens the event's
  URL with `shell.open`, else the month calendar on its day; a button opens the month calendar), or
  (`clickOpens`) the month calendar (`calendar.openPopup()`) or `target`. Settings `count`,
  `lookaheadDays`, `showLocation`, `feeds` (names, comma separated), `clickOpens`, `target`,
  `width`. **One timer**: each draw sets a single `setTimeout` to the next moment the text changes
  (the event's start or end, each minute of the last hour before it, the next event entering the
  look-ahead, local midnight); `calendar.onChange` redraws. Test hook: an item setting
  `"debugStore": true` appends each text it shows to its storage (`shown`). Permissions `calendar`,
  `shell`.
- `widgets/weather` ("Weather"): the native weather element and its popup on the
  [weather](#weather) provider. `measure()` reproduces the element's box (`dock.clockDate` picks
  the smaller size), `draw()` the glyph (`weather.icon` + `canvas.image`, dimmed with the opacity
  argument) and the temperature; `weather.onChange` redraws the slot and the open popup; the popup
  (`drawPopup` with hit rectangles for `onPopupClick`) draws the place, the current conditions,
  "Updated N min ago" (one `setTimeout` per minute while it is open), the hourly strip with a
  temperature line (`canvas.path`) and the daily rows with gradient range bars; its buttons call
  `weather.refresh()` / `weather.openSettings()`, the credit `shell.open`. Settings `showPlace`,
  `showHigh`, `showLow`, `compact`, `clickOpens` (`popup`, `settings`, `none`), `days`, `hours`,
  and (round 7, 1.1.0, group "Slot") **`dockForecast`**: `"none"` (default), `"today"` (today's
  high / low under the temperature, as `showHigh` + `showLow`, which still work on their own) or
  `"days"`: after the glyph and temperature, **`dockDays`** (2 - 5, default 3; shown only for
  "days") compact columns from today, each the weekday's short name ("Today" for the first) over
  the condition glyph (16 DIPs) over the high and the low (9 DIP text, the low in `dateText`); on a
  left / right dock rows under the current conditions instead (the weekday's initial, a 14 DIP
  glyph, "high/low"). `measure()` grows the slot by the columns (rows); standalone the whole
  layout scales to the window (`fitLayout`). The manifest's `preview.settings` show three days.
  Round 16 phase 16D (1.2.0): the slot is two blocks, the current conditions and the day cells,
  and **`layout`** picks a preset: `auto` (the above), `strip` (icon beside the temperature, a
  column per day after it), `tiles` (a tile per day: the weekday over the icon on the left, the
  high over the low on the right, after the conditions on a top / bottom dock and stacked under
  them on a left / right dock), `list` (icon over the temperature, a line per day under them),
  `card` (icon over the temperature, tiles under them: a desktop widget) or `custom`, which shows
  `conditions` (`auto` / `beside` / `stacked`), `dayCell` (`auto` / `column` / `tile` / `row` /
  `icon`: the icon and "high/low" only) and `forecastPlacement` (`auto` / `beside` / `below`);
  `dockDays` goes to 7. Text: `showCondition` (the description under the temperature), `dayNames`
  (`auto`: short in columns and tiles, the initial in lines; `short`, `initial`, `full`),
  `todayLabel` ("Today" for the first cell, not with initials), `rangeStyle` (a high / low pair on
  one line: `auto` is arrows under the temperature and a slash in a cell; `arrows`, `slash`,
  `plain`). Icons: `iconPack` `windock` (`weather.icon`) or `lucide` (Lucide's line icons, ISC,
  https://lucide.dev: their SVG path data in `main.js`, parsed once into `canvas.path` objects by a
  small M / L / H / V / C / A / Z parser with the endpoint-to-centre arc conversion of the SVG
  spec, stroked 2 / 24 of the box wide with round caps; the popup uses the same pack),
  `iconStyle` `colour` (sun yellow, rain in the accent) or `mono` (the ink; the ViberDock pack keeps
  its sun). Round 16 phase 16F: `iconPack` `meteocons`, Bas Milius' coloured, animated Meteocons
  (MIT, https://meteocons.com, the fill style): `tools/meteocons_convert.py` turns 20 of the
  upstream animated SVGs into `widgets/weather/meteocons.js` (a third built-in file, `import`ed by
  `main.js`): per icon the gradients and a list of parts, each a few filled / stroked paths and
  circles with the SMIL loops as tracks (translate, rotate, opacity; key times, spline easing, the
  `begin` phase). `drawMeteocon` declares a clipped group `canvas.layer` for the icon (its opacity
  is the stale dimming) and one layer per part under it with the tracks scaled to the box, so on a
  dock DirectComposition animates them (see [Animation](#animation)); `iconMotion` false (Animated
  icons) hands no tracks over and poses the parts at their first keys; the popup and the previews
  get the same still picture inline. The pin and the refresh arrow stay Lucide's. Sizes and spacing
  (group "Layout"): `size` 60 - 250 % scales the whole slot (docks
  only; standalone fills the window as before), `daySize` 60 - 250 % the day cells on their own
  (their text, icons and the tile's inner gap, not the gap between cells), `dayGap` 0 - 48 DIPs
  between cells (default 6) and `forecastGap` 0 - 48 DIPs between the conditions and the first
  cell (default 8; half of it when the cells go under), both scaled by `size`. Round 33 phase 33A
  (1.4.0): two more `layout` values. **`trend`**: after the conditions a temperature curve of the
  next `trendHours` (6 - 24, default 12) hours from `hourly` (a `canvas.path` polyline,
  `trendWidth` 40 - 300 DIPs wide, default 120, times `size` and `daySize`), a dot at now in the
  accent, `trendRain` (default on) bars of `precip` under it, `trendLabels` `none` / `ends` (Now
  and the last hour) / `peak` (default: also the extreme ahead, the highest point or, when the
  hours fall further than they rise, the lowest: "25° at 4 PM"), `trendLine` `text` / `accent` /
  `temperature` (a vertical gradient on the scale below); on a left / right dock `trendSide`
  `thermometer` (default: a 7 x 52 DIP gradient bar from today's low to its high, a dot at the
  temperature now, the high and low at its ends) or `text` (↑high over ↓low). Its second line is
  the Text toggles' (no setting of its own): `showFeelsLike` (new, any layout: "Feels 20°") with
  `showCondition`, `showPlace`, `showHigh`, `showLow`. **`capsules`**: a rounded chip per day
  (`dockDays`, the day names, `daySize`, `dayGap`, `forecastGap` as for the cells; it shows days
  whatever `dockForecast` says, and choosing it sets `dockForecast` to `"days"` through
  `settings.set` so the page shows those fields): the icon, then the day's name over its high and
  (`capsuleLow`, default on) its low; stacked and upright on a left / right dock (the name, the
  icon, the high). `capsuleTint` `temperature` (default: the high on the scale) / `condition` /
  `accent` / `none`, `capsuleStrength` 0 - 100 % (default 25; the outline takes 20 % + 1.1 x
  that), `capsuleOutline` (default on), `capsuleRadius` 0 - 30 DIPs (default 30: pills). The
  temperature scale (group "Colours"): `coldColor` (default `#FF6CB8FF`) and `hotColor`
  (`#FFFF9F43`), the hue going straight from one to the other (blue to orange passes teal, green
  and yellow), slightly desaturated in the middle; `tempScale` `fixed` (0 - 30 °C, 32 - 86 °F) or
  `days` (the lowest to the highest value shown; the default since 33B). Round 33 phase 33B: a
  standalone widget's **`design`** (`classic` default, `sky`, `bento`, `bare`; ignored on a dock,
  where the slot keeps `layout`: a `visibleIf` can only name another field, so the row says it).
  `classic` is the fitted slot as before, now over an optional panel. The **Panel** card
  (`panelFill` `auto` / `none` / `solid` / `translucent` / `sky`, `auto` being Sky's sky, Bento's
  translucent tiles and nothing for Classic; `panelColor` (`#00000000`: the theme's background),
  `panelOpacity` 75 %, `panelRadius` 18, `panelOutline`, `panelShadow`, `panelPadding` 12; the
  detail rows show once a fill is chosen) has no glass: the canvas cannot blur what is behind the
  window, so translucent it is (a theme's acrylic still frosts through the window's own Background
  "Panel" with `panelFill` none), and a shadow is four low-alpha rings cut out under the panel
  (`fill(path, paint, "evenodd")`), the panel moved in from the window's edges for them. The
  window's own Background is not visible to the script; `design`'s description asks for None (the
  default). **`sky`**: a three-stop vertical gradient from the time of day at the place (dawn and
  dusk 45 minutes either side of today's `sunrise` / `sunset`, day, night; grey when the day is
  wet or overcast; `isDay` without sunrise or sunset) or `skyPalette` (one of the five, or
  `custom`: `skyTop` / `skyBottom`, automatic text turning dark on a light one); the place and
  "condition · feels" top left, the temperature at 54 DIPs light top right, `skyArc` (a dashed half
  ellipse of two cubic quarters over a horizon, the sun at (now − sunrise) / daylight), the
  sunrise / daylight / sunset line, a row of `designDays` (0 - 7, default 5) days. **One timer**:
  a Sky widget whose picture moves with the clock (`skyPalette` auto or the arc shown) keeps a
  single `setTimeout` to the next 10-minute mark or phase boundary, whichever is first; each draw
  replaces it, any other design clears it, a hidden window's redraw waits for the host. **`bento`**:
  tiles from `bentoTiles` (comma separated, in order: `main`, `feels`, `wind`, `humidity`, `uv`,
  `sun`, `hours`, `days`; unknown names skipped; default `main,feels,wind,uv,sun,hours`), four
  columns from 400 DIPs of width, else two; `main`, `hours` and `days` take two, the last tile of a
  short row the rest; `bentoGap` 8, the hour strip `bentoHours` (4 - 12, default 6) `bentoStep`
  (1 - 3, default 2) hours apart; the sun tile shows the next of sunset and sunrise (it changes on
  the next redraw: Bento has no timer). **`bare`**: no panel; the temperature 88 DIPs at
  `bareWeight` (`thin`: DirectWrite takes Segoe UI's lightest, Light; `regular`, `bold`), the
  place, condition and high / low beside it on its baseline, a hairline, a line per day with a
  range bar on one scale over the shown days (`bareBars` `gradient` / `accent` / `off`),
  `bareAlign`, `bareScrim` 0 - 60 % (a rounded wash), `bareShadow` `none` / `soft` / `strong`
  (copies of the text in a ring round it, dark under light text: there is no text shadow). The
  **Colours** card: `textTone` (`auto`: the theme's text and date colours, light on Sky; `light`,
  `dark`, `own` with `textColor`) and `accentColor` (`#00000000`: the theme's) for every layout
  and design; the popup keeps the theme's. Switching to a design grows the window once to its size
  (Sky 300 x 280, Bento 320 x 340, Bare 300 x 260) through `widget.resize` in `onSettingsChanged`,
  only when it is smaller and only on a change of `design`. Test hook: `"debugStore": true` stores
  what the slot (with its `days` count, `conditions`, `cell`, `placement`, `stack`, `pack`; round 33
  also `layout`, `block` (`days`, `trend`, `thermo`, `range`, `capsules` or `none`), `hours`,
  `design`, `panel`; a design stores its own record: `phase`, `palette`, `timerMs`, `sun`, `tiles`,
  `rows`, `cols`, `k` ...) and the popup drew. Permissions `weather`, `shell`.
- `widgets/notes` ("Notes", round 6): a note per item, written in the dock's text editor
  (`popup.editText`). Its text is kept in the widget's storage under `"note:" + dock.itemKey`, or
  under `"note:id:" + noteId` when the **Note id** setting is set (items with the same id, on any
  dock or the desktop, share one note; `storage.onChange` redraws the others after an edit). On a
  top / bottom dock the tile shows the first line as the title (bold with `boldTitle`) and up to
  `lines` (1 - 6) lines under it, as many as fit the dock's height (the font shrinks to make room,
  never below 7 DIPs); `measure()` widens it with the text up to `maxWidth` DIPs; the wheel
  scrolls the lines (a mark at the right edge shows where). A left / right dock: a square tile
  with the title. Standalone: the whole note wrapped in the window with 10 DIPs of padding, the
  title line bold, the wheel scrolls it (a bar shows where); the manifest's `standalone` is 220 x
  180 by default, 80 x 60 at least. An empty note shows "New note" in the date-text colour. A click
  opens the editor (`maxLength` 20000); OK saves `{ text, modified }`. Settings `paper` (`none`,
  `yellow` (default), `pink`, `blue`, `theme`: the theme's label panel; a rounded sheet with a soft
  shadow line), `fontSize` (9 - 24, default 12), `textColor` (`#00000000`: automatic, a dark ink on
  paper, the theme's text otherwise), `boldTitle`, `lines`, `maxWidth`, `noteId`. In the
  widget drawer's preview (`dock.preview`, see [Preview mode](#preview-mode)) it shows a sample "Shopping list" note. No
  timers, no permissions.
- `widgets/system-monitor` ("System monitor", round 6 phase 6D): one [perf](#perf) metric in one
  of eleven styles: `graph` (a line over `historySeconds`, from `perf.series`), `area` (filled),
  `sparkline` (a small graph and the value), `bar` / `vbar` (horizontal / vertical), `ring` (a 270°
  arc, the value inside), `gauge` (a half circle with ticks and a needle), `dial` (a 240°
  speedometer with numbers), `percent` (a big number + %), `value` (a big number + unit), `text`
  ("label: value"). Settings, grouped: **Metric** `metric` (`"choiceFrom": "perf"`, default
  `cpu.total`), `label` (empty: the metric's name), `unit` (text: replaces the unit, unscaled),
  `decimals` (0 - 3; scaled values under 10 get one), `scale` (`auto`: bytes as KB / MB / GB with
  1024, rates as KB/s ... GB/s, seconds as days / hours / minutes; `raw`), `refresh`
  (`perf.setInterval`, 250 - 10000 ms); **Display** `style`, `historySeconds` (10 - 600),
  `showLabel`, `showValue`, `showMinMax` (graphs: the lowest / highest shown; bars, rings, gauges:
  the range's ends), `lineWidth`, `fill` (under a graph's / sparkline's line), `sweep` (0: the
  style's own arc; else 90 - 300 degrees), `rangeMin` / `rangeMax` (both 0: the metric's range;
  rates and counts then scale to the highest value shown), `smooth` (the average of the last 3);
  **Colours** `color` (`#00000000`: the theme's accent), `warnAt` / `criticalAt` (in the metric's
  unit, 0 off) with `warnColor` / `criticalColor` (gauges and dials also mark the zones),
  `background` (`none`, `panel`, `theme`), `textColor`; **Size** `width` (40 - 400 DIPs on a top /
  bottom dock; the height is the icon size; a left / right dock: a square; standalone: it fills the
  window, 160 x 120 by default). A metric that is not available draws a dash. Redraws on
  `perf.onChange` only, no timers. Permission `perf`.
- `widgets/system-overview` ("System overview", round 6 phase 6D): CPU (`cpu.total`), memory
  (`memory.percent`; in a wide list also used / total GB), GPU (`gpu.total`), disk (`disk.total`)
  and network (`network.down` / `up`), each row toggleable (`showCpu` ... `showNetwork`); `rows`
  (`auto`: one row of cells on a top / bottom dock, a list on a left / right dock and on the
  desktop; `row`; `list`), `style` (`auto`: bars in a row, sparklines in a list; `bars`;
  `sparklines`), `showValues`, `historySeconds`, `refresh`, `color`, `warnAt` / `criticalAt`
  (percent, default 75 / 90) with their colours, `background`, `textColor`, `cellWidth` (DIPs per
  cell on a dock). Standalone 240 x 160 by default. Permission `perf`.
- `widgets/battery` ("Battery", round 7 phase 7C): the [perf](#perf) provider's battery keys
  (`battery.percent`, `.charging`, `.acLine`, `.timeLeft`). A battery outline filled to the level
  (`fillColor`, `#00000000` = the theme's accent; red at or below `lowPercent`, default 20), a bolt
  over it while charging, the percentage beside it on a top / bottom dock and under it on a left /
  right one (`showPercent`, default on) and the time left as "2 h 10 m" (`showTimeLeft`, default
  off; only on battery while Windows knows it); the size follows the icon size. Without a battery
  (the keys absent: a desktop PC) a crossed battery and "No battery", or, with
  `hideWithoutBattery`, nothing: `measure()` returns `main` 0 and the slot takes no room
  (standalone it still draws). `perf.setInterval(10000)`; `perf.onChange` redraws only when the
  percentage, charging, mains power or (while it is shown) the time left changed, so a steady
  battery costs a comparison per sample and no draw. A click opens a 260 x 120 popup: the level as
  a bar, the state ("Charging", "On battery", "Plugged in, not charging", "Full"), the time left
  and **Power & battery settings** (`shell.open('ms-settings:batterysaver')`). Standalone 160 x 80
  by default (64 x 48 at least), scaled to the window. Permissions `perf`, `shell`.
- `widgets/notification-bell` ("Notification bell"): a scriptable bell that behaves like the
  built-in notification centre by default; `unread` (default on; off: nothing polls and the fixed
  `badge` number shows) draws `notifications.count()` and redraws on `notifications.onChange`;
  `click` `centre` (default: `notifications.openCentre()`) or `windows`
  (`shell.sendKeys('win+n')`); `badgeColor`, `image`. Permissions `shell`, `notifications`.
- `widgets/network` ("Network", round 27 phase 27B): the [network](#network) provider as the
  taskbar's icon, drawn with paths in one square of `dock.iconSize` x 0.5 x `scale`: Wi-Fi as a fan
  (1.1.0: a filled wedge and three butt-capped bands, 0.96 of the square across; 1.0.0's dot and
  three arcs were wider than the square) lit up to the signal (the 0 - 5 bars mapped to four steps,
  the rest at 30 % opacity), an ethernet plug, five cellular bars, a padlock for a VPN, a globe otherwise; a cross
  badge (`warnColor`) without internet, an exclamation mark for a captive portal, a crossed globe
  without a connection. `showLabel` (default off) with `label` `name` (the SSID, else "No internet"
  / "Not connected") or `state`; on a side dock the label goes under the icon when it fits across.
  `click` `popup` (default; 1.1.0: 300 wide, 188 high with the signal card and 118 without, laid
  out from its width between 16 DIP margins: the icon on a disc tinted by the state, the name cut
  with an ellipsis (`measureText`, since `maxWidth` wraps), a state dot with the state, kind and
  "Metered", a card with the signal in a word and as five segments, and two buttons of equal
  width, **Quick settings** (`shell.sendKeys('win+a')`) and **Network settings**
  (`shell.open('ms-settings:network-status')`); cards and buttons are `theme.labelText` at a low
  alpha, so they show on a light panel; a state change while it is open opens it again at the new
  height), `quick` or `settings`. `network.onChange` redraws only when something drawn changed. Settings `showLabel`, `label`, `scale`, `font`,
  `iconColor`, `textColor`, `warnColor`, `click`. Permissions `network`, `shell`.
- `widgets/input-language` ("Input language", round 27 phase 27C): the [input](#input) provider as
  the taskbar's indicator: `code` ("ENG") semibold in `dock.theme.text`, `region` under it
  (`region` `auto`: only when two layouts share the code, `always`, `never`), `showName` (default
  off) the full language name beside it. `click` `next` (default: `input.next()`) or `popup`;
  `middleClick` `popup` (default), `next` or `none`. The popup lists `layouts` (a row of 40 DIPs
  each: the code, the language, the keyboard; the current one marked with the accent), a click on
  a row calls `input.set(id)`. After its own switch the widget reads the state once more 400 ms
  later (one `setTimeout`, never while idle), in case Windows does not report the switch.
  `hideSingle` (default off): with one layout `measure()` returns `main` 0. Settings `region`,
  `showName`, `scale`, `font`, `textColor`, `nameColor`, `click`, `middleClick`, `hideSingle`.
  Permission `input`.
- `widgets/timer` ("Timer", round 28 phase 28B): `mode` `countdown` / `pomodoro` / `stopwatch` /
  `date`, one state per item in `storage` (`"timer:" + (timerId || dock.itemKey)`) holding **end
  timestamps** (`endAt`, a stopwatch's `startAt`), never a tick count: it survives a restart, and
  one that ended while the dock was quit says so once on load. While running it calls
  `timer.setRefresh(1000)` (paused by the host while unseen), stopped `setRefresh(0)`; the end is
  one `setTimeout` (at most 6 hours ahead, re-armed), which also runs on a hidden dock, and raises
  `notify.banner` (`banner`, `sound` settings). A countdown to a date ticks once a minute while
  seen, each second in its last hour. Popup: Start / Pause, Reset, Skip (Pomodoro), the
  `presets` chips; `onMiddleClick` starts / pauses; `onWheel` changes a stopped countdown by a
  minute. Permission `notify`.
- `widgets/world-clock` ("World clock"): up to six `timezone` fields (`zone1` - `zone6`, Windows
  zone keys) with names; `system.formatTime(false, zone)` / `system.localTime(zone)` for each
  (no Date arithmetic across zones); `refreshMs` 60000 (the shared, minute-aligned tick); the
  popup's day offset from comparing `system.now()` with `system.localTime(zone)`.
- `widgets/todo` ("To-do"): a list in `storage` (`"todo:" + (listId || dock.itemKey)`, shared
  through `storage.onChange`), `popup.editText({ multiline: false })` to add, a paged popup (10
  rows) that `popup.open` resizes to the list. Nothing runs while idle.
- `widgets/moon` ("Moon phase"): Meeus' principal phases (Astronomical Algorithms ch. 49, the
  larger periodic terms) and the lunation fraction; the lit part drawn as a semicircle plus the
  terminator (a half ellipse of two cubic beziers); `hemisphere` mirrors it; `refreshMs` 3600000.
- `widgets/quick-buttons` ("Quick buttons"): 1 - 6 buttons, each a `shell.sendKeys` name of the
  allow-list or `shell.open` (`ms-settings:`, `rundll32.exe user32.dll,LockWorkStation`); icons
  drawn with `canvas.text` in the `Segoe Fluent Icons` font (two as paths). `onPress` (not
  claimed) shows the pressed tile. Permission `shell`.

A minimal widget:

```js
// widgets/dot/main.js, with widget.json { "name": "Dot", "size": { "horizontal": { "main": 40, "cross": 40 } } }
let on = false;
export function draw(canvas, dock) {
  canvas.fillCircle(canvas.width / 2, canvas.height / 2, 12, on ? dock.theme.accent : '#40FFFFFF');
}
export function onClick() {
  on = !on;
  requestRedraw();
}
```
