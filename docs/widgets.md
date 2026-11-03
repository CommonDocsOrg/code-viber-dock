# WinDock widgets: API reference

A widget is a small JavaScript program that draws into one slot of a dock. It is a folder (or a
built-in shipped inside `WinDock.exe`), loads without compiling, and runs in its own sandboxed
script engine ([QuickJS-ng](../third_party/quickjs-ng/VERSION.md) 0.18, ES2023) on the dock's UI
thread. This file is the full reference; README.md has the short version.

- [Packages](#packages): where widgets live, the folder layout
- [widget.json](#widgetjson): the manifest
- [main.js](#mainjs): the module and its exports, when they are called
- [The host API](#the-host-api): `canvas`, `dock`, `timer`, `console`, `system`, `settings`,
  `storage`, `http`, `shell`, `audio`, `media`, `popup`, `widget`
- [Animation](#animation): frames with timers, `dock.visible`, `dock.reduceMotion`, the cost rules
- [Analog clock faces](#analog-clock-faces): the `faces/*.json` format
- [Limits and errors](#limits-and-errors): the watchdog, memory, the error tile, reloading
- [Sandbox](#sandbox): what a widget can and cannot reach, permissions
- [Developing a widget](#developing-a-widget): `watch`, the Console, the Settings pages
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
`start`, `search`, `notification-bell`) are compiled into the
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
| `name` | the id | Shown in Settings (Dock contents, Add widget). |
| `version`, `author`, `description` | "" | Informational. |
| `main` | `"main.js"` | The module to load, a path inside the package. |
| `size.horizontal`, `size.vertical` | 64 x 40, 48 x 48 | The box in DIPs (pixels at 100 % scale) on a horizontal (bottom / top) and a vertical (left / right) dock: `main` along the dock, `cross` across it. Used unless the module exports `measure()`. Clamped to 4 - 1024 (main) and 4 - 512 (cross). |
| `refreshMs` | 0 | Above 0: the dock calls `tick()` every `refreshMs` ms (at least 100), aligned to multiples of it in local time (60000 = on the minute), then redraws. 0: no ticking. `timer.setRefresh(ms)` overrides it at run time. |
| `timeLimitMs` | 50 | The watchdog: the longest one call into the script may run (10 - 1000; loading the module gets four times as long, at least 200 ms). |
| `memoryLimitMB` | 32 | The script heap (4 - 256 MB). |
| `permissions` | [] | What the widget may do beyond drawing (see [Sandbox](#sandbox)): `"shell"` (open files / URLs, list folders, file icons, the folder popup, file drops, `shell.sendKeys`), `"audio"` (the [audio](#audio) provider), `"media"` (the [media](#media) provider), `"notifications"` (the [notifications](#notifications) provider), `{ "http": ["api.example.com", "*.example.org"] }` (also written `"http:api.example.com"`: requests to those hosts only; `*.example.org` is any sub-domain of it, not example.org itself). Unknown entries are logged and ignored. |
| `watch` | false | Folder widgets: reload when a file of the widget's folder changes (see [Developing a widget](#developing-a-widget)). |
| `settings` | [] | The widget's settings schema, see below. |

### Settings schema

Each entry: `key` (required), `type`, `label`, `description`, `default`, and per type:

| `type` | Value | Extra keys |
|---|---|---|
| `bool` | true / false | |
| `int` | a whole number | `min`, `max`, `step` |
| `number` | a number | `min`, `max`, `step` |
| `string` | text | |
| `choice` | one of `choices` | `choices`: `["a", "b"]` or `[{ "value": "a", "label": "A" }]`; `choiceFrom`: a glob of package files (below) |
| `colour` (`color`) | `"#AARRGGBB"` / `"#RRGGBB"` | |
| `font` | a font family name | |

`choiceFrom` (choice fields): `"faces/*.json"` adds one choice per package file matching the glob
(`*` and `?` stay inside one folder level), after any fixed `choices`: the value is the file name
without its folder and extension (`classic`), the label the file's JSON `"name"` (else the value).
The list is read whenever the package is loaded (a widget reload, opening its settings page), so a
file added to an exported widget's folder shows up after **Reload widgets**. A default that names no
file falls back to the first one.

The values live in the dock item's `settings` object in config.json. The script sees the schema's
defaults overridden by the item's values (a value of the wrong type or out of range falls back to
the default; ints are rounded, numbers clamped); keys the schema does not name are passed through
unchanged. Settings > **Widget settings** is generated from the schema for each widget item of
the dock the settings window edits (a drop-down picks the item; Dock contents > Settings… on a
widget row opens it): a check box per `bool`, an up-down per `int`, a drop-down per `choice`, an
edit box for the others (`colour` as `#AARRGGBB`), each field's `description` under it, and
**Reset to defaults** (removes the item's `settings`). A change is saved and reaches the widget at
once (`onSettingsChanged`).

## main.js

An ES module. The dock imports it once, evaluates its top-level code (with the whole host API
available), then calls its exports:

| Export | Called | Arguments | Returns |
|---|---|---|---|
| `draw(canvas, dock)` | **required.** When the widget asked for a redraw (`requestRedraw()`, a tick), when its settings changed, and when the dock lays it out again (its size, scale, theme or edge changed). Never per frame while idle. | the canvas, the dock | |
| `measure(canvas, dock)` | Before the dock lays the widget out (and after every redraw request). Only `canvas.measureText` works here. | | `{ main, cross }` in DIPs; without this export the manifest's size hint for the dock's axis is used |
| `tick(dock)` | Every `refreshMs` (see the manifest, `timer.setRefresh`). | the dock | `false` to skip the redraw that follows |
| `onPress(e)` | The left button went down on the widget. | `{ x, y, button }`: DIPs from the box's top-left | |
| `onClick(e)` | A left click on the widget (press and release on it). | `{ x, y, button, closedPopup }`: `closedPopup` is true when that press closed this widget's popup (so a click toggles it: `if (!e.closedPopup) popup.open(...)`) | |
| `onMiddleClick(e)` | A middle click. | `{ x, y, button }` | |
| `onWheel(e)` | The mouse wheel over the widget. | `{ x, y, delta, notches, horizontal }`: `delta` in WHEEL_DELTA units (120 per notch; positive: away from the user) | `true` if the widget used it |
| `onHover(e)` | The pointer entered / left the widget. | `{ x, y, inside }` | |
| `onDrop(e)` | Files were dropped on the widget (only with the `shell` permission and this export; the dock then shows the drop as allowed over the whole widget instead of pinning). | `{ x, y, files: [paths] }` | |
| `onSettingsChanged(settings)` | The item's settings changed (Settings, config.json reload). The global `settings` is the new object already. | the settings | |
| `drawPopup(canvas, dock)` | The widget's popup (`popup.open`) is shown or redrawn (`popup.redraw()`). The canvas is the popup's (`canvas.width` / `height`: its size in DIPs), over the theme's panel. | the canvas, the dock | |
| `onPopupClick(e)` | A click in the popup. | `{ x, y, button }`: DIPs from the popup's top-left; `button` `"left"`, `"middle"`, `"right"` | |
| `onPopupClose(e)` | The popup closed (Escape, another window came forward, a click on the dock, `popup.close()`, a reload). | `{}` | |
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
| `image(path, x, y, w, h, opacity)` | An image file of the package (`"assets/icon.png"`; PNG, JPEG, BMP, GIF, ICO, TIFF via WIC; up to 8 MB and 4096 x 4096). `w` / `h` omitted or 0: its pixel size in DIPs. Decoded once and cached. Throws if it cannot be read. |
| `imageSize(path)` | `{ width, height }` in pixels. |
| `fileIcon(path, x, y, size)` | The shell's icon of a file or folder (an absolute path; `%VAR%` expanded), `size` DIPs square. Needs the `shell` permission. Cached like images. |
| `save()`, `restore()` | The transform, opacity and clips (up to 64 levels). Whatever is left pushed at the end of `draw()` is popped. |
| `translate(x, y)`, `rotate(radians, cx, cy)`, `scaleBy(sx, sy)` | Change the transform of what follows (`rotate`: clockwise for positive angles, about (cx, cy)). The scale call is `scaleBy` because `canvas.scale` is the DPI property (before 5a the method was listed as `scale(sx, sy)`, which the property shadowed). |
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
| `visible` | The dock is on screen: shown or sliding in, not auto-hidden or hiding, not a desktop dock hidden behind another window. `onVisibilityChange` reports changes. |
| `id`, `widgetId` | The dock's id (`docks[].id`), the widget's id. |
| `theme` | The dock's theme colours as `"#AARRGGBB"`: `text`, `dateText`, `accent`, `background`, `labelBackground`, `labelText`, `separator`, `border`. |
| `requestRedraw()` | Same as the global. |

### timer and redraws

| Call | |
|---|---|
| `requestRedraw()` (also `timer.requestRedraw()`, `dock.requestRedraw()`) | Redraw (and re-measure) the widget soon: once, after the current call returns, however often it is called. |
| `setTimeout(fn, ms)`, `setInterval(fn, ms)` (also `timer.setTimeout` / `timer.setInterval`) | Returns an id. One shared host scheduler runs every widget's timers (one Windows timer for the earliest deadline; none while nothing waits). Intervals are at least 16 ms. At most 64 timers per widget (more throws a RangeError). No extra arguments are passed to `fn`. |
| `clearTimeout(id)`, `clearInterval(id)`, `timer.clear(id)` | |
| `timer.setRefresh(ms)` | `tick()` every `ms` (at least 100, at most 86400000) aligned to local time, replacing the manifest's `refreshMs`; 0 stops it. A clock that shows seconds calls `timer.setRefresh(1000)`, one without `60000`; `86400000` wakes at local midnight. When the system time or time zone changes, or the machine resumes from sleep, the refresh is re-aligned to the new local time and `tick()` runs at once. |

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

The `sendKeys` allow-list (names are case-insensitive):

| Name | Keys | Opens |
|---|---|---|
| `win` | Windows | Start |
| `win+a` | Windows + A | Quick settings |
| `win+b` | Windows + B | The notification area (keyboard focus) |
| `win+d` | Windows + D | Show / hide the desktop |
| `win+e` | Windows + E | File Explorer |
| `win+i` | Windows + I | Settings |
| `win+n` | Windows + N | The notification centre and calendar |
| `win+s` | Windows + S | Search |
| `win+tab` | Windows + Tab | Task view |
| `win+v` | Windows + V | Clipboard history |
| `win+w` | Windows + W | Widgets |
| `win+x` | Windows + X | The Start button's menu |
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
| `notifications.openCentre()` | Toggles the dock's own notification centre popup over the widget (throws when the dock cannot show it). |
| `notifications.onChange(fn)` | `fn(list)` (the `get()` array) when notifications come, go or change state. Returns a remover. |

### popup

A widget can open one popup at a time, anchored on its slot beside the dock (above a bottom dock,
right of a left dock ...), like the dock's own popups. It holds the dock shown while it is open
and closes on Escape, when another window comes forward, on a click on the dock, and when the
widget reloads.

| Call | |
|---|---|
| `popup.open(width, height)` (or `popup.open({ width, height })`) | A popup of that size in DIPs (16 - 1600 x 16 - 1200) that the module draws in `drawPopup(canvas, dock)` over the theme's panel; clicks in it go to `onPopupClick`. Opening it again resizes and redraws it. |
| `popup.redraw()` | Calls `drawPopup` again. |
| `popup.close()` | |
| `popup.isOpen` | The widget's popup (of any kind) is on screen. |
| `popup.folder(path[, view])` | The dock's own folder popup (the folder's contents, scrolling, keyboard, navigation into sub-folders, file drops, Open in Explorer) for that folder, anchored on the widget. `view`: `"auto"` (default: a grid up to 48 entries, a list above), `"grid"` or `"list"`; anything else throws. Needs `"shell"`. |
| `popup.calendar()` | The dock's own month calendar (the clock's popup: locale weekday names, first day of the week, today marked), anchored on the widget. A second call while it is open does nothing; a click on the widget closes it (`closedPopup`). |
| `popup.previews(app)` | The window previews of an app the dock shows (its exe path, e.g. `"C:\\Windows\\explorer.exe"`, or its AppUserModelID; any case), anchored on the widget; nothing (a log line) when the app has no window on the dock. |

The calls are carried out right after the current call returns.

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

Measured (5a, this machine, user config): the analog clock with a ticking second hand wakes 10
times in 10 s; with the smooth second hand ~300 times in 10 s while the dock is shown and 0 while
it is auto-hidden; without a second hand 0 in 10 s (once a minute); the sweep is ~45 frames.

## Analog clock faces

The analog clock widget draws a face file: `faces/<name>.json` in its package (the built-ins:
`classic`, `minimal`, `modern-dark`, `roman`, `station`). To add one, export the widget (Settings >
Widgets > **Export built-in to folder**), put a new `.json` next to the others in
`%LocalAppData%\WinDock\widgets\analog-clock\faces\`, and press **Reload widgets**: the Face
drop-down lists it (`choiceFrom`). A face only needs the keys it changes; everything else has a
default. The same format is meant for other clock faces later (the native clock's custom faces).

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
                "counterweight": 0.055, "tipDot": 0 },
                                            // shape: taper | line | block | leaf; counterweight: a disc on
                                            // the tail; tipDot: a disc at the tip (station clocks)
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
- **Reload.** **Reload widgets** in the dock menu (shown when a dock has a widget), Settings >
  Widgets, the Console or `WinDock.DockCommand` 126 reloads every widget; the Console's Reload or
  `WinDock.ItemCommand` 809 reloads one. Reloading reads the package again (the folder version if
  there is one) and starts the script over (its storage stays). Editing config.json and **Reload
  config** also re-creates widgets whose items changed.

## Sandbox

A widget sees only the host API above; each widget has its own runtime and cannot reach another
widget's globals. Without permissions there is no file system, process or network access at all.
`"http"` opens requests to the listed hosts only; `"shell"` opens files and URLs, lists folders,
draws file icons, opens the folder popup, receives file drops and presses the `sendKeys`
allow-list; `"audio"` reads and changes the system volume / mutes / the default output; `"media"`
reads the current media session and controls its playback; `"notifications"` reads Windows'
notifications and dismisses them. Modules and images are read
only from the widget's own package (and `media.art()` tokens, with `"media"`):

- `import` / `import()` specifiers are package paths: `"./x.js"` and `"../x.js"` relative to the
  importing module, anything else relative to the package root (`"lib/x.js"`).
- Rejected: absolute paths (`/x`, `\x`), drive letters (`C:/...`), URLs and anything with `:`,
  backslashes, empty segments, and any `..` that would leave the package.
- For folder widgets the file actually opened must be inside the widget's folder (a junction or
  symbolic link pointing elsewhere is refused), and every file is at most 8 MB.

## Developing a widget

- **Export a built-in** to start from it: Settings > **Widgets** lists every installed widget (name,
  id, source: built-in, folder, or folder replacing a built-in; version). **Export built-in to
  folder** writes its files to `widgets\<id>\` (refused if that folder exists); the copy then
  replaces the built-in. **Add from folder…** copies a folder that holds a widget.json into the
  widgets folder (its name, lowercased, becomes the id). **Add to dock** adds the selected widget
  to the dock the settings window edits; **Reload widgets**, **Open widgets folder** and
  **Console…** do what they say.
- **`"watch": true`** in widget.json (folder widgets): every change in the widget's folder (any
  file, sub-folders included) reloads it a quarter of a second after the first change (a burst of
  writes by an editor is one reload). One
  thread waits in `ReadDirectoryChangesW` on every watched folder while any watched widget is on a
  dock; nothing polls, and it costs nothing while the files do not change.
- **The Console** (Settings > Widgets > Console…, the dock menu's **Widget console…**,
  `WinDock.DockCommand` 127): every widget instance of every dock with its state (ok, error,
  stopped), the selected one's log (console output, errors with their stacks, the host's own
  lines: loading, reloads, file changes, shell.open), **Reload**, **Open folder** (the widget's
  folder, or the widgets folder for a built-in) and **Clear** (the view; the log is kept). It
  updates as lines arrive.
- **Errors** show on the widget's tile ("Error" / "Stopped" and the first line of the message).

## Adding a widget to a dock

Settings > Dock contents > **Add widget ▾** lists the installed widgets (built-in and from the
widgets folder) after Clock, Weather and Running apps (Settings > Widgets has **Add to dock**
too); the widget is added before a clock that ends the dock (else at the end) and can then be
moved like any item; the row's **Settings…** opens its Widget settings page. In config.json:

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
- `widgets/analog-clock`: faces as data (`faces/*.json`, [the format](#analog-clock-faces)) listed
  by a `choiceFrom` setting and read with `widget.readJson`; settings `face`, `seconds` (second
  hand), `secondsMotion` (`ticking`: one wake-up a second; `smooth`: 30 frames a second while the
  dock is visible), `dateOnFace` (`none`, `day`, `month-day`, `year`, `full`), `showDigital` (the
  time under the dial), `sweep` (the hands sweep in when it appears or its face changes), `size`
  (DIPs, 0 = the dock's icon size; the box is square). The [Animation](#animation) example.
- `widgets/analog-date` ("Calendar page"): today's date as a page with a coloured header (the
  month, or the weekday) over a big day number; `style` `flip` (split page, stacked pages, a curled
  corner, the page flips over when the date changes) or `flat`; `showMonth`, `showWeekday`,
  `showYear`, `accent` (header colour), `click` (`calendar`: `popup.calendar()`, `target`:
  `shell.open(clickTarget)`, `none`), `size`. One wake-up a day: `timer.setRefresh(86400000)`
  wakes it at local midnight. Test hook: an item setting `"debugNow": "2026-12-31T23:59:57"`
  runs it on a fake clock starting at that local time (the midnight timer becomes a `setTimeout`
  to the fake midnight) and writes each date it shows to its storage (`debugShown`).
- `widgets/volume`: `audio.get()` / `audio.onChange` for the glyph and the percentage; `onWheel`
  steps `settings.step` % (default 2) on the step grid and unmutes on the way up; `onMiddleClick`
  mutes (`middleClick`: `mute` / `none`); the popup (`drawPopup` with hit rectangles for
  `onPopupClick`): slider, Mute, Mute microphone, the outputs (`audio.setDevice`), Sound settings
  (`shell.open('ms-settings:sound')`). Settings `showPercent`, `step`, `middleClick`. Permissions
  `audio`, `shell`.
- `widgets/media` ("Now playing"): `media.art()` drawn with `canvas.image` in a rounded clip, the
  note glyph when there is no art, title / artist beside it (faded at the edge; a marquee at 30
  frames a second only while hovered); hovering shows previous / play-pause / next over the art;
  a click on the text opens a popup with the album and the controls. Settings `showText`,
  `textWidth`, `marquee`, `wheelVolume`. Permissions `media`, `audio`.
- `widgets/start`: `shell.sendKeys('win')`, falling back to `shell.open('shell:AppsFolder')` when
  Windows refuses the input; `icon` `windows` / `mono` / `image` with `image` (a package file, or
  any absolute path whose shell icon `canvas.fileIcon` draws), `size` (percent of the slot).
- `widgets/search`: `shell.sendKeys('win+s')`; `showLabel`, `label`, `image`.
- `widgets/notification-bell` ("Notifications"): a bell; `unread` (default off: then nothing polls
  and the fixed `badge` number shows) draws `notifications.count()` and redraws on
  `notifications.onChange`; `click` `windows` (default: `shell.sendKeys('win+n')`) or `centre`
  (`notifications.openCentre()`); `badgeColor`, `image`. Permissions `shell`, `notifications`.

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
