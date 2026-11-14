// ViberDock widget API: TypeScript declarations for editors (round 29 phase 29A).
//
// The script API of docs/widgets.md, written from the bindings and the prelude in
// src/QuickJsEngine.cpp and the manifest rules in src/WidgetPackage.cpp. tools/check_widget_docs.py
// fails when a bound name, an export, a permission, a setting type or a manifest key is missing here.
// Nothing reads this file at run time: it is for VS Code (or any TypeScript-aware editor) only.
//
// Use it from a widget's main.js:
//
//   // @ts-check
//   /// <reference path="./windock.d.ts" />
//
//   /** @type {ViberDock.Exports['draw']} */
//   export function draw(canvas, dock) { canvas.text('Hi', 4, 4, { color: dock.theme.text }); }
//
// (from the repository's examples: `/// <reference path="../../docs/windock.d.ts" />`). An exported
// built-in widget folder gets a copy of this file and a jsconfig.json, so the globals are known
// there without the reference line.
//
// Widgets run in QuickJS-ng (ES2023) with no browser or Node API. An editor that also loads the
// DOM's declarations (VS Code does for a loose .js file) still offers `window`, `document` and the
// like: they do not exist in a widget. The jsconfig.json of an exported widget limits the library
// to ES2023 ({ "compilerOptions": { "lib": ["es2023"] } }). `console` and the timers are declared so
// that they merge with the DOM's when it is there.
/// <reference lib="es2023" />

declare namespace ViberDock {
  /** A colour: "#RGB", "#RRGGBB", "#AARRGGBB" (alpha first), "rgb(r, g, b)", "rgba(r, g, b, a)", "transparent", 0xAARRGGBB, or a gradient. */
  type Paint = string | number | Gradient;
  interface Gradient {
    readonly type: 'linear' | 'radial';
  }
  /** A gradient stop: [offset 0 - 1, colour]. */
  type GradientStop = [number, string | number];
  /** A width in DIPs, or the full form. */
  type Stroke = number | { width?: number; cap?: 'butt' | 'round' | 'square'; join?: 'miter' | 'round' | 'bevel'; dash?: number[] };
  type FontWeight = number | 'thin' | 'light' | 'normal' | 'medium' | 'semibold' | 'bold' | 'black';
  interface TextStyle {
    font?: string;
    /** DIPs, default 12. */
    size?: number;
    weight?: FontWeight;
    italic?: boolean;
    align?: 'left' | 'center' | 'right';
    baseline?: 'top' | 'middle' | 'bottom' | 'alphabetic';
    /** > 0: wraps the text at this width. */
    maxWidth?: number;
    color?: Paint;
  }
  interface TextMetrics {
    width: number;
    height: number;
    baseline: number;
    lines: number;
  }
  /** A composition layer's loop (canvas.layer options.tracks). */
  interface LayerTrack {
    prop: 'x' | 'y' | 'rotate' | 'opacity';
    /** Seconds, > 0. */
    duration: number;
    /** Seconds: the loop's phase. */
    begin?: number;
    /** true: ends at the last key. */
    once?: boolean;
    /** [[seconds, value], ...]: 2 - 64 keys in order. */
    keys: Array<[number, number]>;
    /** rotate: the centre [cx, cy]. */
    center?: [number, number];
    /** A cubic bezier [x1, y1, x2, y2] per key span (CSS style); left out: linear. */
    easing?: Array<[number, number, number, number]>;
  }
  interface LayerOptions {
    /** The id of a layer declared earlier in this draw. */
    parent?: string;
    clip?: boolean;
    opacity?: number;
    /** Up to 32. */
    tracks?: LayerTrack[];
  }

  /** A path built with chained calls; draw it with canvas.fill / canvas.stroke. */
  interface Path {
    moveTo(x: number, y: number): Path;
    lineTo(x: number, y: number): Path;
    quadTo(cx: number, cy: number, x: number, y: number): Path;
    bezierTo(c1x: number, c1y: number, c2x: number, c2y: number, x: number, y: number): Path;
    /** Radians, 0 = +x, clockwise (y points down). */
    arc(cx: number, cy: number, r: number, start: number, end: number, counterClockwise?: boolean): Path;
    rect(x: number, y: number, w: number, h: number): Path;
    close(): Path;
  }

  /** The drawing surface of draw() / measure() / drawPopup(): DIPs from the box's top-left. */
  interface Canvas {
    /** The box in DIPs. */
    readonly width: number;
    readonly height: number;
    /** Physical pixels per DIP. */
    readonly scale: number;
    /** false in measure() (only measureText works there). */
    readonly drawing: boolean;
    fillRect(x: number, y: number, w: number, h: number, paint?: Paint): void;
    strokeRect(x: number, y: number, w: number, h: number, paint?: Paint, stroke?: Stroke): void;
    fillRoundRect(x: number, y: number, w: number, h: number, r: number, paint?: Paint): void;
    strokeRoundRect(x: number, y: number, w: number, h: number, r: number, paint?: Paint, stroke?: Stroke): void;
    fillEllipse(cx: number, cy: number, rx: number, ry: number, paint?: Paint): void;
    strokeEllipse(cx: number, cy: number, rx: number, ry: number, paint?: Paint, stroke?: Stroke): void;
    fillCircle(cx: number, cy: number, r: number, paint?: Paint): void;
    strokeCircle(cx: number, cy: number, r: number, paint?: Paint, stroke?: Stroke): void;
    line(x0: number, y0: number, x1: number, y1: number, paint?: Paint, stroke?: Stroke): void;
    /** A new path. */
    path(): Path;
    fill(path: Path, paint?: Paint, rule?: 'nonzero' | 'evenodd'): void;
    stroke(path: Path, paint?: Paint, stroke?: Stroke): void;
    /** DirectWrite text; "\n" breaks lines. */
    text(str: string, x: number, y: number, style?: TextStyle): void;
    /** Also in measure(). */
    measureText(str: string, style?: TextStyle): TextMetrics;
    /** A package image ("assets/icon.png"), a media / weather image token, or (with "shell") an absolute path. */
    image(path: string, x: number, y: number, w?: number, h?: number, opacity?: number): void;
    /** Pixels (a photo from disk: as decoded). */
    imageSize(path: string): { width: number; height: number };
    /** The shell's icon of a file or folder (permission "shell"). */
    fileIcon(path: string, x: number, y: number, size?: number): void;
    save(): void;
    restore(): void;
    translate(x: number, y: number): void;
    /** Clockwise for positive radians, about (cx, cy). */
    rotate(radians: number, cx?: number, cy?: number): void;
    /** `scale` is the DPI property, hence the name. */
    scaleBy(sx: number, sy?: number): void;
    /** Multiplies the opacity of what follows (0 - 1). */
    opacity(a: number): void;
    clip(x: number, y: number, w: number, h: number): void;
    clipPath(path: Path): void;
    /** Starts a composition layer; true when it is live, false when drawn inline (popup, preview, reduce motion). */
    layer(id: string, x: number, y: number, w: number, h: number, options?: LayerOptions): boolean;
    endLayer(): void;
    linearGradient(x0: number, y0: number, x1: number, y1: number, stops: GradientStop[]): Gradient;
    radialGradient(cx: number, cy: number, rx: number, ry: number | undefined, stops: GradientStop[], fx?: number, fy?: number): Gradient;
  }

  /** The dock's theme colours as "#AARRGGBB". */
  interface DockTheme {
    readonly text: string;
    readonly dateText: string;
    readonly accent: string;
    readonly background: string;
    readonly labelBackground: string;
    readonly labelText: string;
    readonly separator: string;
    readonly border: string;
  }

  /** What the widget knows about its dock and slot (read-only, always current). */
  interface Dock {
    readonly axis: 'horizontal' | 'vertical';
    readonly edge: 'bottom' | 'top' | 'left' | 'right';
    /** The box in DIPs (upright). */
    readonly width: number;
    readonly height: number;
    /** The box along / across the dock. */
    readonly main: number;
    readonly cross: number;
    readonly scale: number;
    readonly iconSize: number;
    readonly hovered: boolean;
    readonly magnified: boolean;
    readonly reduceMotion: boolean;
    readonly locked: boolean;
    /** On screen (not auto-hidden, not behind a window, not guarded, session not paused). */
    readonly visible: boolean;
    /** The dock's id (a standalone widget: its entry's id). */
    readonly id: string;
    readonly widgetId: string;
    /** "<dock id>/<widget id>#<n>", or a standalone widget's entry id. */
    readonly itemKey: string;
    readonly standalone: boolean;
    /** A settings window's live preview: side effects are no-ops, providers give sample data. */
    readonly preview: boolean;
    readonly clockDate: boolean;
    readonly theme: DockTheme;
    requestRedraw(): void;
  }

  interface Timer {
    setTimeout(fn: () => void, ms: number): number;
    setInterval(fn: () => void, ms: number): number;
    clear(id: number): void;
    /** tick() every `ms` aligned to the time of day (0 stops it); timeZone: a Windows zone key. */
    setRefresh(ms: number, options?: { timeZone?: string } | string): void;
    requestRedraw(): void;
  }

  interface LocalTime {
    year: number;
    /** 1 - 12 */
    month: number;
    day: number;
    hour: number;
    minute: number;
    second: number;
    millisecond: number;
    /** 0 = Sunday */
    weekday: number;
    yearDay: number;
    dst: boolean;
    /** Minutes east of UTC. */
    utcOffset: number;
    /** "" = the local time. */
    timeZone: string;
  }
  interface TimeZoneInfo {
    key: string;
    name: string;
    standardOffset: number;
  }
  /** Local time and the user's formats (QuickJS's Date applies today's DST offset to every date). */
  interface System {
    now(): LocalTime;
    localTime(timeZone?: string | null): LocalTime;
    timeZones(): TimeZoneInfo[];
    /** strftime ("%H:%M"); `%#I` drops the leading zero. */
    format(fmt: string, timeZone?: string | null): string;
    formatDate(style: 'long' | 'short', timeZone?: string | null): string;
    formatTime(seconds: boolean, timeZone?: string | null): string;
    readonly locale: string;
    readonly hour12: boolean;
    readonly units: 'metric' | 'imperial';
    readonly temperatureUnit: 'C' | 'F';
    readonly firstDayOfWeek: number;
    readonly dayNames: string[];
    readonly shortDayNames: string[];
    readonly monthNames: string[];
    readonly shortMonthNames: string[];
  }

  /** One JSON store per widget id (every item of the widget shares it), at most 1 MB. */
  interface Storage {
    get<T = unknown>(key: string, fallback?: T): T;
    /** undefined removes the key. */
    set(key: string, value: unknown): void;
    remove(key: string): void;
    keys(): string[];
    clear(): void;
    /** Another item of this widget changed `key`. Returns a remover. */
    onChange(fn: (e: { key: string }) => void): () => void;
  }

  interface HttpOptions {
    headers?: Record<string, string>;
    /** 1000 - 30000 ms, the whole request. */
    timeout?: number;
    type?: 'json' | 'text';
  }
  interface HttpRequest extends HttpOptions {
    method?: 'GET' | 'POST' | 'PUT' | 'DELETE' | 'PATCH' | 'HEAD';
    url: string;
    body?: unknown;
  }
  interface HttpResponse {
    status: number;
    ok: boolean;
    contentType: string;
    text: string;
    /** The parsed body (type: "json", or a JSON content type). */
    json?: any;
  }
  /** Permission { "http": [hosts] }: requests to those hosts only. */
  interface Http {
    get(url: string, options?: HttpOptions): Promise<HttpResponse>;
    /** A string body is text, anything else JSON. */
    post(url: string, body?: unknown, options?: HttpOptions): Promise<HttpResponse>;
    request(options: HttpRequest): Promise<HttpResponse>;
  }

  interface ShellEntry {
    name: string;
    path: string;
    folder: boolean;
    size: number;
    /** ms since 1970 (UTC). */
    modified: number;
  }
  type SendKeysName =
    | 'win' | 'start-near' | 'win+a' | 'win+b' | 'win+d' | 'win+e' | 'win+i' | 'win+m' | 'win+shift+m' | 'win+n'
    | 'win+s' | 'win+tab' | 'win+v' | 'win+w' | 'win+x' | 'win+space' | 'win+shift+s' | 'win+k' | 'win+.'
    | 'win+ctrl+left' | 'win+ctrl+right' | 'ctrl+shift+esc' | 'volume-up' | 'volume-down' | 'volume-mute'
    | 'media-play-pause' | 'media-next' | 'media-previous' | 'media-stop';
  /** Permission "shell". */
  interface Shell {
    /** Opens after the calling function returns; returns true at once (false in a preview). */
    open(target: string, args?: string): boolean;
    list(folder: string): ShellEntry[];
    /** One combination of the fixed allow-list. */
    sendKeys(name: SendKeysName): boolean;
  }

  interface AudioState {
    available: boolean;
    volume: number;
    muted: boolean;
    device: string;
    deviceId: string;
    mic: boolean;
    micVolume: number;
    micMuted: boolean;
    micName: string;
    devices: Array<{ id: string; name: string; default: boolean }>;
  }
  /** Permission "audio". */
  interface Audio {
    get(): AudioState;
    setVolume(v: number): void;
    setMuted(b: boolean): void;
    setMicMuted(b: boolean): void;
    setDevice(id: string): void;
    onChange(fn: (state: AudioState) => void): () => void;
  }

  interface MediaState {
    available: boolean;
    session: boolean;
    title: string;
    artist: string;
    album: string;
    app: string;
    status: 'playing' | 'paused' | 'stopped' | 'none';
    hasArt: boolean;
    position: number | null;
    duration: number | null;
    canPlay: boolean;
    canPause: boolean;
    canToggle: boolean;
    canNext: boolean;
    canPrevious: boolean;
    canSeek: boolean;
  }
  /** Permission "media". */
  interface Media {
    get(): MediaState;
    /** An image token for canvas.image, or null. */
    art(): string | null;
    play(): void;
    pause(): void;
    toggle(): void;
    next(): void;
    previous(): void;
    seek(seconds: number): void;
    onChange(fn: (state: MediaState) => void): () => void;
  }

  interface Notification {
    id: number;
    app: string;
    appKey: string;
    aumid: string;
    title: string;
    body: string;
    time: number;
    unread: boolean;
    balloon: boolean;
  }
  /** Permission "notifications". */
  interface Notifications {
    get(): Notification[];
    state(): { state: 'polling' | 'denied' | 'off'; reason: string };
    count(): number;
    countFor(appKey: string): number;
    dismiss(id: number): void;
    dismissAll(): void;
    markRead(id: number): void;
    activate(id: number): void;
    openCentre(): void;
    onChange(fn: (list: Notification[]) => void): () => void;
  }

  /** A Date, ms since 1970 (UTC), or "YYYY-MM-DD" (a local day). */
  type DayArg = Date | number | string;
  interface CalendarEvent {
    id: string;
    feed: string;
    color: string;
    title: string;
    location: string;
    description: string;
    start: number;
    end: number;
    allDay: boolean;
    url: string;
    recurring: boolean;
    startDay: string;
    endDay: string;
    startTime: string;
    endTime: string;
    reminder: number;
  }
  interface CalendarFeed {
    name: string;
    color: string;
    enabled: boolean;
    file: boolean;
    status: 'off' | 'loading' | 'ok' | 'error';
    error: string;
    lastFetch: number;
    events: CalendarEvent[];
  }
  /** Permission "calendar". */
  interface Calendar {
    feeds(): CalendarFeed[];
    events(range?: { from?: DayArg; to?: DayArg }): CalendarEvent[];
    eventsOn(date?: DayArg): CalendarEvent[];
    today(): CalendarEvent[];
    next(n?: number): CalendarEvent[];
    refresh(): void;
    openPopup(date?: DayArg): void;
    onChange(fn: (feeds: CalendarFeed[]) => void): () => void;
    onReminder(fn: (event: CalendarEvent) => void): () => void;
  }

  interface WeatherState {
    state: 'none' | 'stale' | 'fresh' | 'error';
    configured: boolean;
    place: string;
    label: string;
    latitude: number;
    longitude: number;
    units: 'metric' | 'imperial';
    temperatureUnit: 'C' | 'F';
    windUnit: 'km/h' | 'mph';
    refreshMinutes: number;
    busy: boolean;
    error: string;
    status: string;
    age: number;
    fetchedAt: number;
    updated: string;
    current: {
      temp: number | null;
      code: number;
      isDay: boolean;
      description: string;
      glyph: string;
      wind: number | null;
      humidity: number | null;
      feelsLike: number | null;
      uv: number | null;
    } | null;
    hourly: Array<{ time: number; hour: number; temp: number | null; code: number; isDay: boolean; precip: number | null }>;
    daily: Array<{
      day: string;
      weekday: number;
      code: number;
      description: string;
      min: number | null;
      max: number | null;
      uvMax: number | null;
      sunrise: number | null;
      sunset: number | null;
    }>;
  }
  /** Permission "weather". */
  interface Weather {
    get(): WeatherState;
    refresh(): void;
    openSettings(): void;
    /** An image token for canvas.image: a WMO code, "setLocation" or "refresh". */
    icon(code: number | string, isDay?: boolean, colours?: { ink?: string; water?: string }): string;
    onChange(fn: (state: WeatherState) => void): () => void;
  }

  interface PerfSnapshot {
    time: number;
    interval: number;
    serial: number;
    metrics: Readonly<Record<string, number>>;
  }
  interface PerfMetric {
    key: string;
    name: string;
    unit: string;
    min: number;
    max: number;
    kind: 'percent' | 'bytes' | 'bytesPerSecond' | 'temperature' | 'voltage' | 'fan' | 'current' | 'power' | 'clock' | 'number' | 'bool' | 'seconds';
    group: string;
    hwinfo: boolean;
  }
  /** Permission "perf". */
  interface Perf {
    get(): PerfSnapshot;
    metrics(): readonly PerfMetric[];
    /** Oldest first; NaN where a sample lacked the key. */
    series(key: string, n?: number): number[];
    setInterval(ms: number): number;
    hwinfo(): { available: boolean; reason: string; version: string; pollTime: number; sensors: Array<{ index: number; name: string }>; readingCount: number };
    drives(): Array<{ drive: string; label: string; ready: boolean; total?: number; free?: number; used?: number }>;
    onChange(fn: (snapshot: PerfSnapshot) => void): () => void;
  }

  interface NetworkState {
    available: boolean;
    connected: boolean;
    internet: boolean;
    state: 'internet' | 'local' | 'constrained' | 'none';
    kind: 'wifi' | 'ethernet' | 'cellular' | 'vpn' | 'other' | 'none';
    name: string;
    bars: number | null;
    signal: number | null;
    metered: boolean;
  }
  /** Permission "network". */
  interface Network {
    get(): NetworkState;
    onChange(fn: (state: NetworkState) => void): () => void;
  }

  interface InputLayout {
    id: string;
    tag: string;
    code: string;
    region: string;
    language: string;
    layout: string;
    current: boolean;
  }
  interface InputState {
    available: boolean;
    id: string;
    tag: string;
    code: string;
    region: string;
    language: string;
    layout: string;
    current: number;
    layouts: InputLayout[];
  }
  /** Permission "input". */
  interface Input {
    get(): InputState;
    /** A layout id or a language tag. */
    set(idOrTag: string): boolean;
    next(): boolean;
    onChange(fn: (state: InputState) => void): () => void;
  }

  /** Permission "notify". */
  interface Notify {
    /** true: shown (after the call); false: the rate limit refused it (one per 5 s) or a preview. */
    banner(options: { title: string; text?: string; sound?: boolean }): boolean;
  }

  interface EditTextOptions {
    title?: string;
    text?: string;
    multiline?: boolean;
    maxLength?: number;
    placeholder?: string;
  }
  interface Popup {
    /** 16 - 1600 x 16 - 1200 DIPs, drawn by drawPopup. */
    open(width: number, height: number, options?: { passive?: boolean }): void;
    open(options: { width: number; height: number; passive?: boolean }): void;
    close(): void;
    redraw(): void;
    /** The ordinary popup is on screen. */
    readonly isOpen: boolean;
    /** The dock's folder popup (permission "shell"). */
    folder(path: string, view?: 'auto' | 'grid' | 'list'): void;
    previews(app: string): void;
    calendar(): void;
    /** The text after OK, null after Cancel. */
    editText(options?: EditTextOptions): Promise<string | null>;
  }

  /** The widget's own package files (no permission). */
  interface Widget {
    files(pattern?: string): string[];
    read(path: string): string;
    readJson(path: string): any;
    /** A standalone widget's window size in DIPs (false on a dock or in a preview). */
    resize(width: number, height: number): boolean;
  }

  /** The merged settings of this item: the schema's defaults overridden by the item's values. */
  interface Settings {
    [key: string]: any;
    /**
     * Round 29: changes one of this item's own settings (a key of the manifest's schema), validated
     * as the settings page would; written after the call (onSettingsChanged follows). false: the
     * rate limit refused it (5 at once, then one every 10 s). Throws on a bad key or value. A url
     * set this way never makes its host reachable: only one the user types and allows does.
     */
    set(key: string, value: unknown): boolean;
    /** Round 29: fn(settings) after the item's settings changed (after onSettingsChanged). Returns a remover. */
    onChange(fn: (settings: Settings) => void): () => void;
  }

  /** Permission "links" (round 29). */
  interface Links {
    /** Opens an http(s) address in the browser after the call; only right after a click on the widget (once per click): false otherwise. */
    open(url: string): boolean;
  }

  interface PointerEvent {
    /** DIPs from the box's top-left. */
    x: number;
    y: number;
    button: 'left' | 'middle' | 'right';
  }
  /** The functions main.js may export; the dock calls them (docs/widgets.md "main.js"). */
  interface Exports {
    /** Required. */
    draw(canvas: Canvas, dock: Dock): void;
    measure(canvas: Canvas, dock: Dock): { main: number; cross: number; flush?: boolean } | undefined | void;
    /** false: skip the redraw. */
    tick(dock: Dock): boolean | void;
    /** true claims the press (onDrag / onRelease follow, no click). */
    onPress(e: PointerEvent, dock: Dock): boolean | void;
    onDrag(e: PointerEvent, dock: Dock): void;
    onRelease(e: PointerEvent & { cancelled: boolean }, dock: Dock): void;
    onClick(e: PointerEvent & { closedPopup: boolean }, dock: Dock): void | Promise<void>;
    onMiddleClick(e: PointerEvent, dock: Dock): void;
    /** true when the widget used it. */
    onWheel(e: { x: number; y: number; delta: number; notches: number; horizontal: boolean }, dock: Dock): boolean | void;
    onHover(e: { x: number; y: number; inside: boolean }, dock: Dock): void;
    /** Permission "shell". */
    onDrop(e: { x: number; y: number; files: string[] }, dock: Dock): void;
    onSettingsChanged(settings: Settings, dock: Dock): void;
    drawPopup(canvas: Canvas, dock: Dock): void;
    onPopupClick(e: PointerEvent, dock: Dock): void;
    onPopupClose(e: { passive: boolean }, dock: Dock): void;
    onVisibilityChange(e: { visible: boolean }, dock: Dock): void;
  }

  // --- widget.json ---------------------------------------------------------------------------------

  type Permission =
    | 'shell' | 'audio' | 'media' | 'notifications' | 'calendar' | 'weather' | 'perf' | 'network' | 'input' | 'notify' | 'links'
    | `http:${string}`
    /** hostSettings (round 29): keys of `url` settings whose host the user may allow, per item, after a prompt. */
    | { http: string[]; hostSettings?: string[]; shell?: boolean };
  type SettingType =
    | 'bool' | 'boolean' | 'int' | 'integer' | 'number' | 'string' | 'text' | 'choice' | 'enum' | 'colour' | 'color'
    | 'font' | 'place' | 'timezone' | 'file' | 'folder'
    /** Round 29: a password box, stored protected for the Windows user (the script gets the plain value). */
    | 'secret'
    /** Round 29: an https address (http only on the local network). */
    | 'url';
  interface SettingSchema {
    key: string;
    type?: SettingType;
    label?: string;
    description?: string;
    default?: unknown;
    min?: number;
    max?: number;
    step?: number;
    choices?: Array<string | { value: string; label?: string }>;
    /** A glob of package files ("faces/*.json") or "perf". */
    choiceFrom?: string;
    group?: string;
    unit?: string;
    visibleIf?: Record<string, unknown>;
    common?: boolean;
    section?: 'content' | 'appearance';
    quick?: boolean;
    /** A file field's picker patterns ("*.png;*.jpg"). */
    filter?: string;
  }
  interface SizeHint {
    main: number;
    cross: number;
  }
  /** widget.json. */
  interface Manifest {
    name?: string;
    version?: string;
    author?: string;
    description?: string;
    main?: string;
    api?: number;
    size?: { horizontal?: SizeHint; vertical?: SizeHint };
    refreshMs?: number;
    timeLimitMs?: number;
    memoryLimitMB?: number;
    permissions?: Permission[];
    watch?: boolean;
    settings?: SettingSchema[];
    standalone?: { minWidth?: number; minHeight?: number; aspect?: 'free' | 'keep' | number; defaultWidth?: number; defaultHeight?: number; supported?: boolean };
    preview?: { settings?: Record<string, unknown>; width?: number; height?: number };
  }
}

/** The widget's log (the Console), objects as JSON, errors with their stack; the last 500 lines. */
interface Console {
  log(...args: any[]): void;
  info(...args: any[]): void;
  debug(...args: any[]): void;
  warn(...args: any[]): void;
  error(...args: any[]): void;
}
declare var console: Console;

declare const canvas: ViberDock.Canvas;
declare const dock: ViberDock.Dock;
declare const timer: ViberDock.Timer;
declare const system: ViberDock.System;
declare const settings: ViberDock.Settings;
declare const storage: ViberDock.Storage;
declare const http: ViberDock.Http;
declare const shell: ViberDock.Shell;
declare const audio: ViberDock.Audio;
declare const media: ViberDock.Media;
declare const notifications: ViberDock.Notifications;
declare const calendar: ViberDock.Calendar;
declare const weather: ViberDock.Weather;
declare const perf: ViberDock.Perf;
declare const network: ViberDock.Network;
declare const input: ViberDock.Input;
declare const notify: ViberDock.Notify;
declare const popup: ViberDock.Popup;
declare const widget: ViberDock.Widget;
declare const links: ViberDock.Links;

/** At least 16 ms between runs of an interval; at most 64 timers per widget. */
declare function setTimeout(fn: () => void, ms?: number): number;
declare function setInterval(fn: () => void, ms?: number): number;
declare function clearTimeout(id: number): void;
declare function clearInterval(id: number): void;
/** Redraw (and re-measure) soon: once, after the current call returns. */
declare function requestRedraw(): void;
