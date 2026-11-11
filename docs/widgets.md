# WinDock widgets: API reference

A widget is a small JavaScript program that draws into one slot of a dock. It is a folder (or a
built-in shipped inside `WinDock.exe`), loads without compiling, and runs in its own sandboxed
script engine ([QuickJS-ng](../third_party/quickjs-ng/VERSION.md) 0.18, ES2023) on the dock's UI
thread. This file is the full reference; README.md has the short version.

- [Packages](#packages): where widgets live, the folder layout
- [widget.json](#widgetjson): the manifest
- [main.js](#mainjs): the module and its exports, when they are called
- [The host API](#the-host-api): `canvas`, `dock`, `timer`, `console`, `system`, `settings`,
  `storage`, `http`, `shell`, `audio`, `media`, `notifications`, `calendar`, `weather`, `perf`, `popup`, `widget`
- [Animation](#animation): frames with timers, `dock.visible`, `dock.reduceMotion`, the cost rules
- [Clock faces](#clock-faces): the face formats: analog (`faces/*.json` of the Analog clock widget)
  and digital (the dock's own clock, `clock.face`)
- [Limits and errors](#limits-and-errors): the watchdog, memory, the error tile, reloading
- [Sandbox](#sandbox): what a widget can and cannot reach, permissions
- [Developing a widget](#developing-a-widget): `watch`, the Console, the Settings pages, `examples/`
- [Validating a widget](#validating-a-widget): the manifest check, `WinDock.exe --validate-widget`
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
`system-overview`, `battery`, `show-desktop`, `stocks`, `photo-frame`) are compiled into the
exe as resources (their sources are in the repository's `widgets\` folder). A folder with the same id as a built-in replaces it:
the folder wins, so copying a built-in's files into `widgets\<id>\` and editing them changes the
widget. Widgets are always loaded as source; bytecode is never loaded.

## widget.json

```json
{
  "name": "Hello",
  "version": "1.0.0",
  "author": "WinDock",
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
| `refreshMs` | 0 | Above 0: the dock calls `tick()` every `refreshMs` ms (at least 100), aligned to multiples of it in local time (60000 = on the minute), then redraws. 0: no ticking. `timer.setRefresh(ms)` overrides it at run time (also for another time zone: `timer.setRefresh(ms, { timeZone })`). |
| `timeLimitMs` | 50 | The watchdog: the longest one call into the script may run (10 - 1000; loading the module gets four times as long, at least 200 ms). |
| `memoryLimitMB` | 32 | The script heap (4 - 256 MB). |
| `permissions` | [] | What the widget may do beyond drawing (see [Sandbox](#sandbox)): `"shell"` (open files / URLs, list folders, file icons, the folder popup, file drops, `shell.sendKeys`), `"audio"` (the [audio](#audio) provider), `"media"` (the [media](#media) provider), `"notifications"` (the [notifications](#notifications) provider), `"calendar"` (the [calendar](#calendar) provider), `"weather"` (the [weather](#weather) provider), `"perf"` (the [perf](#perf) provider: performance counters and HWiNFO's sensors), `{ "http": ["api.example.com", "*.example.org"] }` (also written `"http:api.example.com"`: requests to those hosts only; `*.example.org` is any sub-domain of it, not example.org itself). An unknown entry is an error: the widget does not load (see [Validating a widget](#validating-a-widget)). |
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
| `visibleIf` | `"visibleIf": { "seconds": true }` | The field (and a group header that would be left empty) is shown only while every named field has that value; the page is generated again when one of them changes. A key that names no other field is an error. |
| `common` | `"common": true` | Round 10 phase 10B: one of the widget's few everyday settings. Dock settings > Contents shows the fields marked `common` in the inspector of the selected widget, editable in place (in schema order, at most six, each only while its `visibleIf` holds), above **More settings…**, which opens the whole page. Without any `common` field the inspector shows the first three fields of a simple type (`bool`, `int`, `number`, `string`, a `choice` not from `"perf"`). The inspector edits `bool`, `int`, `number`, `string`, `colour`, `font`, `choice` and `timezone` fields; a `place` shows as a read-only line, a `"choiceFrom": "perf"` choice is left to the page. Not a boolean: an error. Mark two to four fields. |
| `quick` | `"quick": true` | Round 13 phase 13C: the field is a quick setting in the widget's right-click menu, between its own entries and **Widget settings…**. A `bool` is one checkable entry (its `label`, checked while the value is true; a click toggles it); a `choice` is a submenu titled with its `label`, holding one radio entry per choice (the current value checked; a click chooses it). Fields are listed in schema order. Only `bool` and `choice` fields can be quick: on another type the key is ignored and the manifest check warns. At most 8 quick fields per widget: the ones after the eighth are left out of the menu, with a warning. Not a boolean: an error. A change from the menu is written and applied as the settings window does (saved, `onSettingsChanged`, one Undo step named after the field and the widget, "Changed Units of Weather (script)"). A `"choiceFrom": "perf"` choice lists only its fixed `choices` there. |

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
| `tick(dock)` | Every `refreshMs` (see the manifest, `timer.setRefresh`). | the dock | `false` to skip the redraw that follows |
| `onPress(e)` | The left button went down on the widget. | `{ x, y, button }`: DIPs from the box's top-left | |
| `onClick(e)` | A left click on the widget (press and release on it). | `{ x, y, button, closedPopup }`: `closedPopup` is true when that press closed this widget's popup (so a click toggles it: `if (!e.closedPopup) popup.open(...)`) | |
| `onMiddleClick(e)` | A middle click. | `{ x, y, button }` | |
| `onWheel(e)` | The mouse wheel over the widget. | `{ x, y, delta, notches, horizontal }`: `delta` in WHEEL_DELTA units (120 per notch; positive: away from the user) | `true` if the widget used it |
| `onHover(e)` | The pointer entered / left the widget. | `{ x, y, inside }` | |
| `onDrop(e)` | Files were dropped on the widget (only with the `shell` permission and this export; the dock then shows the drop as allowed over the whole widget instead of pinning). | `{ x, y, files: [paths] }` | |
| `onSettingsChanged(settings)` | The item's settings changed (Settings, config.json reload). The global `settings` is the new object already. | the settings | |
| `drawPopup(canvas, dock)` | The widget's popup (`popup.open`) is shown or redrawn (`popup.redraw()`). The canvas is the popup's (`canvas.width` / `height`: its size in DIPs), over the theme's panel. It draws both kinds, the ordinary popup and a passive one: the module tells them apart by what it opened (e.g. `canvas.width`). | the canvas, the dock | |
| `onPopupClick(e)` | A click in the popup (never in a passive one). | `{ x, y, button }`: DIPs from the popup's top-left; `button` `"left"`, `"middle"`, `"right"` | |
| `onPopupClose(e)` | The popup closed (Escape, another window came forward, a click on the dock, `popup.close()`, a reload; a passive one: see `popup.open`). | `{ passive }`: true when the passive popup closed | |
| `onVisibilityChange(e)` | The dock went off screen (auto-hide hid it, a desktop dock lost the desktop) or came back. Stop animations while hidden (see [Animation](#animation)). | `{ visible }` | |

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
| `visible` | The dock is on screen: shown or sliding in, not auto-hidden or hiding, not a desktop dock hidden behind another window. `onVisibilityChange` reports changes. |
| `id`, `widgetId` | The dock's id (`docks[].id`; a standalone widget: its entry's id), the widget's id. |
| `itemKey` | Round 6: a key for this item, for per-item data in `storage` (the widget's storage is shared by every item of the widget). On a dock `"<dock id>/<widget id>#<n>"`: the n-th item of this widget on that dock (`"main/notes#0"`), so it stays the same when the item is moved past other items, and changes when it is moved past another item of the same widget or when one of those before it is removed (the items then trade keys: the data follows the position among them). A standalone widget: its entry's id (`"note1"`), which never changes. A widget that must keep its data across such moves can offer a setting for the key as well (the Notes widget's **Note id**). |
| `standalone` | `true` for a [standalone widget](#standalone-widgets) (a window of its own on the desktop): `width` / `height` (and `main` / `cross`) are the window's client size, `iconSize` is the shorter side / 1.5, `axis` is `"horizontal"`, `edge` `"bottom"`, `magnified` and `clockDate` are false, `locked` is the entry's lock, `visible` false while a "desktop" layer hides it. |
| `preview` | Round 6 phase 6E: `true` while the widget draws its live preview in a settings window ([Preview mode](#preview-mode)): drawn once on a virtual dock, every side effect a no-op, the providers answering with sample data. A widget with content of its own (a note, a list) can show a sample instead (the Notes widget's "Shopping list"). |
| `clockDate` | A built-in clock item of the dock draws a second line (its date or, since round 5, its label): the dock's own widgets then use their smaller size (the weather element: a 24 instead of 26 DIP glyph, 14 instead of 16 DIP text). |
| `theme` | The dock's theme colours as `"#AARRGGBB"`: `text`, `dateText`, `accent`, `background`, `labelBackground`, `labelText`, `separator`, `border`. |
| `requestRedraw()` | Same as the global. |

### timer and redraws

| Call | |
|---|---|
| `requestRedraw()` (also `timer.requestRedraw()`, `dock.requestRedraw()`) | Redraw (and re-measure) the widget soon: once, after the current call returns, however often it is called. |
| `setTimeout(fn, ms)`, `setInterval(fn, ms)` (also `timer.setTimeout` / `timer.setInterval`) | Returns an id. One shared host scheduler runs every widget's timers (one Windows timer for the earliest deadline; none while nothing waits). Intervals are at least 16 ms. At most 64 timers per widget (more throws a RangeError). No extra arguments are passed to `fn`. |
| `clearTimeout(id)`, `clearInterval(id)`, `timer.clear(id)` | |
| `timer.setRefresh(ms[, { timeZone }])` | `tick()` every `ms` (at least 100, at most 86400000) aligned to local time, replacing the manifest's `refreshMs`; 0 stops it. A clock that shows seconds calls `timer.setRefresh(1000)`, one without `60000`; `86400000` wakes at local midnight. With `timeZone` (round 5: a Windows time zone key name, see [system](#system); the key alone as the second argument works too) the multiples are of that zone's time of day: `timer.setRefresh(86400000, { timeZone: 'Tokyo Standard Time' })` wakes at Tokyo's midnight, `3600000` on its hours (zones 5:45 or 12:45 from UTC included). Every Windows zone is a whole number of minutes from UTC, so periods up to a minute fall on the same instants in every zone. When the system time or time zone changes, or the machine resumes from sleep, the refresh is re-aligned and `tick()` runs at once; a daylight-saving change of the zone itself is picked up at the next wake-up (each wake-up aligns the next one again). |

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
is replaced (and `onSettingsChanged` called) when the item's settings change.

### storage

A small persistent JSON store per widget **id** (every item with that id shares it), in
`%LocalAppData%\WinDock\widget-data\<id>.json` (outside the widget's folder, so exporting,
replacing or deleting the folder keeps the data, and `watch` does not see the writes). Changes are
written one second after the last change (and when the dock quits); nothing is written while
nothing changes. At most 1 MB per widget.

| Call | |
|---|---|
| `storage.get(key, fallback)` | The stored value (any JSON value), or `fallback` (default `undefined`). |
| `storage.set(key, value)` | Stores `JSON.stringify(value)`; `undefined` removes the key. Keys are 1 - 256 characters. Throws a RangeError when the store would pass 1 MB. |
| `storage.remove(key)`, `storage.keys()`, `storage.clear()` | |
| `storage.onChange(fn)` | Round 6: `fn({ key })` when **another item** of the same widget (on any dock or the desktop) set or removed `key` (a value that did not change is not reported, nor are the item's own writes). Delivered shortly after the write (posted, once per key and burst). Returns a remover. Use it to redraw what several items show from one key (the Notes widget's shared notes). |

### http

Needs `{ "http": [hosts] }` in `permissions`; any other host, or no permission, throws a
TypeError at the call. Requests run on a worker thread (WinHTTP, the system's proxy settings) and
come back as a promise resolved on the dock's thread.

| Call | |
|---|---|
| `http.get(url, options)` | `options`: `{ headers: { name: value }, timeout: ms (1000 - 30000, default 10000), type: "json" \| "text" }`. |
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
| `shell.open(target, args)` | Opens a file, folder, app or URL the way Explorer does (ShellExecute; `%VAR%` expanded). |
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
| `ctrl+shift+esc` | Ctrl + Shift + Esc | Task Manager |
| `volume-up`, `volume-down`, `volume-mute` | the volume keys | (the system volume; prefer `audio`) |
| `media-play-pause`, `media-next`, `media-previous`, `media-stop` | the media keys | (the focused media app; prefer `media`) |

### audio

Needs `"audio"` in `permissions`. The default output device (Windows' "Speakers") and input device
(the microphone) through Core Audio. The provider starts with the first call of a widget that has
the permission and stops when the last such widget goes; it is event-driven (Core Audio's device
and volume callbacks), nothing polls.

| Call | |
|---|---|
| `audio.get()` | `{ available, volume, muted, device, deviceId, mic, micVolume, micMuted, micName, devices }`: `available` false when there is no output; `volume` 0 - 1 (the number Windows' volume slider shows / 100); `device` the output's name; `mic` true when there is an input device; `devices`: the active outputs `[{ id, name, default }]`. |
| `audio.setVolume(v)` | The output level, 0 - 1 (clamped). |
| `audio.setMuted(b)`, `audio.setMicMuted(b)` | Mutes / unmutes the output / the microphone. |
| `audio.setDevice(id)` | Makes the output with this id (from `devices`) the default (the console and multimedia roles, as the Sound settings do). This uses `IPolicyConfig`, the undocumented interface Windows' own settings use (stable since Windows 7). An id that is not an active output throws. |
| `audio.onChange(fn)` | `fn(state)` (the `get()` object) whenever the level, a mute state, the default devices or the device list change, by this widget, another program, the volume keys or the quick settings. Returns a function that removes the listener. Changes are coalesced (one call per burst). A mute change can take most of a second to be reported by Windows: a widget that changes something itself should update its own view at once. |

### media

Needs `"media"` in `permissions`. The system's current media session (what the media keys and the
quick settings' media controls act on: Spotify, browsers playing media, the Media Player app ...),
through `GlobalSystemMediaTransportControlsSessionManager`. The provider starts with the first call
and stops when the last widget using it goes; all its WinRT calls run on a worker thread of its own,
woken by the session manager's events (sessions, current session, media properties, playback
state); nothing polls and the timeline is not followed.

| Call | |
|---|---|
| `media.get()` | `{ available, session, title, artist, album, app, status, hasArt, position, duration, canPlay, canPause, canToggle, canNext, canPrevious }`: `session` false when nothing is current; `status` `"playing"`, `"paused"`, `"stopped"` or `"none"`; `app` the session's AppUserModelId (`"Spotify.exe"`, `"Chrome"`, `"Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic"`); `position` / `duration` in seconds or `null` (the position is carried forward while playing, from the app's last report). The first call returns `available: false` until the worker's first snapshot (a moment later, `onChange` follows). |
| `media.art()` | The current art as an image token (`"media:art/3"`) for `canvas.image(token, x, y, w, h)` / `imageSize`, or `null`. The thumbnail is read once per track (app + title + artist + album) and decoded by the canvas like a package image; a token of an older track throws in `canvas.image`. |
| `media.play()`, `pause()`, `toggle()`, `next()`, `previous()` | Sent to the current session (asynchronously; `onChange` reports the result). |
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
| `notifications.activate(id)` | Brings its app to the front from the dock (its slot's windows, or starts it: shell:AppsFolder for an AUMID, the exe for a path) and marks its notifications read. Windows does not let another app activate the toast itself. |
| `notifications.openCentre()` | Toggles the dock's notification centre (README "Notifications and badges") over the widget: the same list as the `notifications` element's, opened after the call returns. Throws when the widget has no slot. |
| `notifications.onChange(fn)` | `fn(list)` (the `get()` array) when notifications come, go or change state. Returns a remover. |

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
| `weather.get()` | `{ state, configured, place, latitude, longitude, units, temperatureUnit, windUnit, refreshMinutes, busy, error, status, age, fetchedAt, updated, current, hourly, daily }`: `state` `"none"` (no data yet / not configured / not running), `"stale"` (the cached or last data; a fetch failed or has not finished), `"fresh"` or `"error"` (no data and the last fetch failed: `error` says why, e.g. `"cannot connect"`); `configured` false while no place is set; `place` the name shown (the widget's own `location`, else its place's); `label` the widget's own `label` setting (`""` none); `units` `"metric"` / `"imperial"`, `temperatureUnit` `"C"` / `"F"`, `windUnit` `"km/h"` / `"mph"`; `busy` a fetch is in flight; `status` the Settings page's status line; `age` seconds since the data was fetched (-1 none); `fetchedAt` ms since 1970 (0 none); `updated` its local short time (`"10:15"`); `current` `{ temp, code, isDay, description, glyph, wind, humidity }` or `null` (`code` the WMO weather code, `description` "Partly cloudy", `glyph` the glyph's name); `hourly` up to 48 hours from the current one: `[{ time (ms since 1970), hour (0 - 23, the place's local time), temp, code, isDay, precip (% chance) }]`; `daily` up to 7 days from today: `[{ day ("YYYY-MM-DD"), weekday (0 = Sunday), code, description, min, max }]`. Missing numbers are `null`. |
| `weather.refresh()` | Fetches the widget's place now (its service: every item of that place gets the result). After the call returns. |
| `weather.openSettings()` | Opens this widget item's own settings window (its Item window, round 5, where its `place` field is; after the call returns; the widget's popup closes). |
| `weather.icon(code, isDay, colours)` | An image token for `canvas.image(token, x, y, w, h, opacity)`: the dock's own vector condition glyph for a WMO `code` (sun / moon with `isDay` false, partly cloudy, overcast, fog, drizzle, rain, snow, thunder), `"setLocation"` (the map pin) or `"refresh"` (the circular arrow). `colours`: `{ ink, water }` (default `dock.theme.text` and `dock.theme.accent`; `refresh` uses `ink`). Drawn as geometry at any size (crisp at any scale; `w` / `h` omitted: 24 DIPs); `opacity` dims it as the native element does for stale data (0.6). |
| `weather.onChange(fn)` | `fn(state)` (the `get()` object) whenever the data or the status of the widget's place changes: a fetch started, finished or failed, the place or units changed. Returns a remover. |

The built-in `weather` widget ("Weather (script)", `widgets/weather`) is the worked example: its
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
  hidden and redraws once when it comes back).
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
pixel for pixel the same: a starting point), `large-time-small-date`, `mono-pill`. Your own:
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
machine's performance counters and HWiNFO's shared sensor memory (nothing else). Modules and images
are read
only from the widget's own package (and `media.art()` tokens, with `"media"`):

- `import` / `import()` specifiers are package paths: `"./x.js"` and `"../x.js"` relative to the
  importing module, anything else relative to the package root (`"lib/x.js"`).
- Rejected: absolute paths (`/x`, `\x`), drive letters (`C:/...`), URLs and anything with `:`,
  backslashes, empty segments, and any `..` that would leave the package.
- For folder widgets the file actually opened must be inside the widget's folder (a junction or
  symbolic link pointing elsewhere is refused), and every file is at most 8 MB.

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
  updates as lines arrive.
- **Errors** show on the widget's tile ("Error" / "Stopped" and the first line of the message).

## Validating a widget

The dock checks a widget's manifest every time it loads the widget (a reload, a dock start), and
`WinDock.exe --validate-widget <folder>` does the same for a folder that is not installed (for
authors and CI; no dock starts, nothing is shown). Each problem names the field and the fix:

```
> WinDock.exe --validate-widget C:\src\my-widget
WinDock widget check: C:\src\my-widget
  name: My widget
  error: main: "app.js" was not found in the widget (fix: add the module or set "main" to its path)
  error: permissions[1]: unknown permission "camera" (fix: use "shell", "audio", ... or { "http": ["host"] })
  warning: settings[2] (size).default: 500 is outside min - max (clamped)
FAILED: 2 errors, 1 warning
```

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
  `popup.editText` returns a rejected promise, `audio.setVolume` and the other setters, `media.play()` ..., `notifications.dismiss`
  ... and `weather.refresh()` / `openSettings()` change nothing, `onChange` listeners are kept and
  never called. Permissions are checked as always (a call without its permission still throws).
- The providers answer with **sample data**, in exactly the shapes of the real calls (the samples
  live in `src/WidgetSamples.cpp`; no service is started, nothing of the machine is read):

| Provider | Sample |
|---|---|
| `audio.get()` | `available` true, `volume` 0.62, not muted, `device` "Speakers (Sample)", a microphone ("Microphone (Sample)", 0.8, not muted), two outputs (Speakers, Headphones). |
| `media.get()` / `media.art()` | Playing "Sample Song" by "Sample Artist" from "Sample Album" (`app` "Sample.Player"), `position` 73 s of `duration` 214, every `can*` true; `art()` is `"media:art/1"`: a generated 64 x 64 violet-to-orange gradient square that `canvas.image` draws. |
| `notifications` | `state` "polling", 3 unread: "Mail" (5 minutes ago, "Sam Rivera"), "Mail" (20 minutes ago, "Weekly report"), "Calendar" (an hour ago, "Stand-up"), newest first. |
| `calendar` | One feed "Sample" in the first feed colour (`#FF4CC2FF`); today "Stand-up" 09:30 - 09:45 and "Lunch with Sam" 12:30 - 13:30 (local time, `recurring`); `eventsOn` another day is empty; `next()` lists today's events that have not ended, after 13:30 the same two tomorrow (so an agenda never previews "No events"); `events()` covers today and tomorrow. |
| `weather.get()` | `state` "fresh" (fetched 5 minutes ago), "Sample City", sunny (code 0, day), 21 °C or 70 °F (the default `units`), wind 12 km/h / 7 mph, humidity 45 %, 24 hourly columns from the current hour (a mild day / night curve), 7 days from today (WMO codes 0, 1, 2, 3, 61, 2, 0: clear, through overcast and rain, to clear; highs 23 - 16 °C). `weather.icon()` works as always. |
| `perf` | `get()`: serial 1, interval 1000, `cpu.total` 37, eight cores, `cpu.frequency` 4180, `memory.percent` 58, `memory.used` / `total` 9.3 / 16 GB, `gpu.total` 12, `disk.total` 4, `network.down` 120 KB/s, `up` 18 KB/s, `system.uptime` 3 days 4 hours, a laptop on battery (round 7: `battery.percent` 74, `battery.charging` 0, `battery.acLine` 0, `battery.timeLeft` 11400 s = 3 h 10 min); `metrics()` lists those keys (35); `series(key, n)` up to 60 samples (a sine wave with noise, the newest one the snapshot's value; the same numbers every time); `hwinfo()` `{ available: false, reason: "preview" }`; `setInterval` answers 1000. |

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
in each; install with App settings > Widget library > **Add from folder…**): `examples/minimal`,
`examples/http-json`, `examples/popup-list`, `examples/edit-text` ([Developing a widget](#developing-a-widget)). The
built-ins are worked examples too:

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
  that one instead. Settings `showPercent`, `step`, `wheelBar`, `middleClick`. Permissions
  `audio`, `shell`.
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
  Permissions `media`, `audio`.
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
- `widgets/weather` ("Weather (script)"): the native weather element and its popup on the
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
  `iconStyle` `colour` (sun yellow, rain in the accent) or `mono` (the ink; the WinDock pack keeps
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
  cell (default 8; half of it when the cells go under), both scaled by `size`. Test hook:
  `"debugStore": true` stores what the slot (with its `days` count, `conditions`, `cell`,
  `placement`, `stack`, `pack`) and the popup drew. Permissions `weather`, `shell`.
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
