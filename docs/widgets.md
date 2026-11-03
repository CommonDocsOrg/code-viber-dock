# WinDock widgets: API reference

A widget is a small JavaScript program that draws into one slot of a dock. It is a folder (or a
built-in shipped inside `WinDock.exe`), loads without compiling, and runs in its own sandboxed
script engine ([QuickJS-ng](../third_party/quickjs-ng/VERSION.md) 0.18, ES2023) on the dock's UI
thread. This file is the full reference; README.md has the short version.

- [Packages](#packages): where widgets live, the folder layout
- [widget.json](#widgetjson): the manifest
- [main.js](#mainjs): the module and its exports, when they are called
- [The host API](#the-host-api): `canvas`, `dock`, `timer`, `console`, `system`, `settings`
- [Limits and errors](#limits-and-errors): the watchdog, memory, the error tile, reloading
- [Sandbox](#sandbox): what a widget can and cannot reach
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

Built-in widgets (`hello`, `digital-clock`) are compiled into the exe as resources (their sources
are in the repository's `widgets\` folder). A folder with the same id as a built-in replaces it:
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
| `name` | the id | Shown in Settings (Dock contents, Add widget). |
| `version`, `author`, `description` | "" | Informational. |
| `main` | `"main.js"` | The module to load, a path inside the package. |
| `size.horizontal`, `size.vertical` | 64 x 40, 48 x 48 | The box in DIPs (pixels at 100 % scale) on a horizontal (bottom / top) and a vertical (left / right) dock: `main` along the dock, `cross` across it. Used unless the module exports `measure()`. Clamped to 4 - 1024 (main) and 4 - 512 (cross). |
| `refreshMs` | 0 | Above 0: the dock calls `tick()` every `refreshMs` ms (at least 100), aligned to multiples of it in local time (60000 = on the minute), then redraws. 0: no ticking. `timer.setRefresh(ms)` overrides it at run time. |
| `timeLimitMs` | 50 | The watchdog: the longest one call into the script may run (10 - 1000; loading the module gets four times as long, at least 200 ms). |
| `memoryLimitMB` | 32 | The script heap (4 - 256 MB). |
| `permissions` | [] | What the widget may do beyond drawing: `"shell"`, `{ "http": ["api.example.com", "*.example.org"] }` (also written `"http:api.example.com"`). Unknown entries are logged and ignored. (The `http` and `shell` APIs are part of the next step of the runtime.) |
| `watch` | false | Reload when a file of the widget's folder changes (next step of the runtime). |
| `settings` | [] | The widget's settings schema, see below. |

### Settings schema

Each entry: `key` (required), `type`, `label`, `description`, `default`, and per type:

| `type` | Value | Extra keys |
|---|---|---|
| `bool` | true / false | |
| `int` | a whole number | `min`, `max`, `step` |
| `number` | a number | `min`, `max`, `step` |
| `string` | text | |
| `choice` | one of `choices` | `choices`: `["a", "b"]` or `[{ "value": "a", "label": "A" }]` |
| `colour` (`color`) | `"#AARRGGBB"` / `"#RRGGBB"` | |
| `font` | a font family name | |

The values live in the dock item's `settings` object in config.json. The script sees the schema's
defaults overridden by the item's values (a value of the wrong type or out of range falls back to
the default; ints are rounded, numbers clamped); keys the schema does not name are passed through
unchanged.

## main.js

An ES module. The dock imports it once, evaluates its top-level code (with the whole host API
available), then calls its exports:

| Export | Called | Arguments | Returns |
|---|---|---|---|
| `draw(canvas, dock)` | **required.** When the widget asked for a redraw (`requestRedraw()`, a tick), when its settings changed, and when the dock lays it out again (its size, scale, theme or edge changed). Never per frame while idle. | the canvas, the dock | |
| `measure(canvas, dock)` | Before the dock lays the widget out (and after every redraw request). Only `canvas.measureText` works here. | | `{ main, cross }` in DIPs; without this export the manifest's size hint for the dock's axis is used |
| `tick(dock)` | Every `refreshMs` (see the manifest, `timer.setRefresh`). | the dock | `false` to skip the redraw that follows |
| `onClick(e)` | A left click on the widget (press and release on it). | `{ x, y, button }`: DIPs from the box's top-left | |
| `onMiddleClick(e)` | A middle click. | `{ x, y, button }` | |
| `onWheel(e)` | The mouse wheel over the widget. | `{ x, y, delta, notches, horizontal }`: `delta` in WHEEL_DELTA units (120 per notch; positive: away from the user) | `true` if the widget used it |
| `onHover(e)` | The pointer entered / left the widget. | `{ x, y, inside }` | |
| `onSettingsChanged(settings)` | The item's settings changed (Settings, config.json reload). The global `settings` is the new object already. | the settings | |

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
| `image(path, x, y, w, h, opacity)` | An image file of the package (`"assets/icon.png"`; PNG, JPEG, BMP, GIF, ICO, TIFF via WIC; up to 8 MB and 4096 x 4096). `w` / `h` omitted or 0: its pixel size in DIPs. Decoded once and cached. Throws if it cannot be read. |
| `imageSize(path)` | `{ width, height }` in pixels. |
| `save()`, `restore()` | The transform, opacity and clips (up to 64 levels). Whatever is left pushed at the end of `draw()` is popped. |
| `translate(x, y)`, `rotate(radians, cx, cy)`, `scale(sx, sy)` | Change the transform of what follows. |
| `opacity(a)` | Multiplies the opacity of everything drawn from now on (0 - 1). |
| `clip(x, y, w, h)` | Clips to a rectangle (in the current transform; rotated: its bounds). |
| `clipPath(path)` | Clips to a path (anti-aliased). |
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
| `id`, `widgetId` | The dock's id (`docks[].id`), the widget's id. |
| `theme` | The dock's theme colours as `"#AARRGGBB"`: `text`, `dateText`, `accent`, `background`, `labelBackground`, `labelText`, `separator`, `border`. |
| `requestRedraw()` | Same as the global. |

### timer and redraws

| Call | |
|---|---|
| `requestRedraw()` (also `timer.requestRedraw()`, `dock.requestRedraw()`) | Redraw (and re-measure) the widget soon: once, after the current call returns, however often it is called. |
| `setTimeout(fn, ms)`, `setInterval(fn, ms)` (also `timer.setTimeout` / `timer.setInterval`) | Returns an id. One shared host scheduler runs every widget's timers (one Windows timer for the earliest deadline; none while nothing waits). Intervals are at least 16 ms. At most 64 timers per widget (more throws a RangeError). No extra arguments are passed to `fn`. |
| `clearTimeout(id)`, `clearInterval(id)`, `timer.clear(id)` | |
| `timer.setRefresh(ms)` | `tick()` every `ms` (at least 100) aligned to local time, replacing the manifest's `refreshMs`; 0 stops it. A clock that shows seconds calls `timer.setRefresh(1000)`, one without `60000`. |

Prefer `setRefresh` / `refreshMs` for anything tied to the clock (it wakes at the boundary itself)
and redraw only what changed: a widget that does nothing costs no CPU at all.

### console

`console.log`, `console.info`, `console.debug`, `console.warn`, `console.error`: the arguments,
joined with spaces (objects as JSON, errors with their stack), go to the widget's log (the last 500
lines; also sent to the debugger output as `WinDock widget <id>#<n> [log] ...`).

### system

Computed by the host in the user's locale (QuickJS's own `Date` applies the current daylight-saving
offset to every date on Windows: use these for local time).

| Call | |
|---|---|
| `system.now()` | `{ year, month (1 - 12), day, hour, minute, second, millisecond, weekday (0 = Sunday), yearDay, dst, utcOffset (minutes east of UTC) }` |
| `system.format(fmt)` | The local time now through strftime (`wcsftime`, as `clock.format`): `"%H:%M"`, `"%a %d %b"`, `"%#I:%M %p"`. A bad specifier falls back to `"%H:%M"`. |
| `system.formatDate("long" \| "short")`, `system.formatTime(seconds)` | The user's own date / time formats (Windows region settings). |
| `system.locale` | `"en-US"`. |
| `system.hour12` | The user's time format is a 12-hour clock. |
| `system.firstDayOfWeek` | 0 = Sunday. |
| `system.dayNames`, `system.shortDayNames` | Sunday first. |
| `system.monthNames`, `system.shortMonthNames` | January first. |
| `system.units`, `system.temperatureUnit` | `"metric"` / `"imperial"` and `"C"` / `"F"`, from `weather.units` in config.json. |

### settings

The global `settings` is the merged settings object (see [Settings schema](#settings-schema)). It
is replaced (and `onSettingsChanged` called) when the item's settings change.

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
- **Reload.** **Reload widgets** in the dock menu (shown when a dock has a widget) or
  `WinDock.DockCommand` 126 reloads every widget; `WinDock.ItemCommand` 809 reloads one slot.
  Reloading reads the package again (the folder version if there is one) and starts the script
  over. Editing config.json and **Reload config** also re-creates widgets whose items changed.

## Sandbox

A widget sees only the host API above. There is no file system, process or network access
(`http` and `shell` come with permissions in the next step of the runtime), no `eval` of other
widgets' code: each widget has its own runtime. Modules and images are read only from the
widget's own package:

- `import` / `import()` specifiers are package paths: `"./x.js"` and `"../x.js"` relative to the
  importing module, anything else relative to the package root (`"lib/x.js"`).
- Rejected: absolute paths (`/x`, `\x`), drive letters (`C:/...`), URLs and anything with `:`,
  backslashes, empty segments, and any `..` that would leave the package.
- For folder widgets the file actually opened must be inside the widget's folder (a junction or
  symbolic link pointing elsewhere is refused), and every file is at most 8 MB.

## Adding a widget to a dock

Settings > Dock contents > **Add widget ▾** lists the installed widgets (built-in and from the
widgets folder) after Clock, Weather and Running apps; the widget is added before a clock that ends
the dock (else at the end) and can then be moved like any item. In config.json:

```json
{ "kind": "widget", "id": "digital-clock", "settings": { "showSeconds": true, "showDate": true } }
```

The same widget can be on a dock several times (each item is its own instance with its own
settings and state) and on several docks.

## Examples

The built-ins are the examples:

- `widgets/hello`: a label and a counter. `onClick` counts, `onMiddleClick` resets, `onWheel`
  counts up / down and returns `true`, `onHover` highlights the border; every handler ends with
  `requestRedraw()`.
- `widgets/digital-clock`: a scripted copy of the dock's own clock (the same box and pixels for
  the same settings). `measure()` sizes the box from `canvas.measureText`, `timer.setRefresh`
  wakes it once a minute (or once a second with seconds), `system.format` formats the time, and on
  a vertical dock every space breaks the line.

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
