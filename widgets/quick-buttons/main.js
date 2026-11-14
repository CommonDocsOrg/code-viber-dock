// Quick buttons (round 28 phase 28B): a row of 1 - 6 buttons for Windows' own UI: quick settings,
// Task view, clipboard history, a screen snip, the notification centre, the desktop, the emoji
// panel, File Explorer, Settings, Search, the previous / next virtual desktop, Cast, Task Manager,
// Lock. Each is a key of shell.sendKeys' fixed allow-list or a shell verb (ms-settings:, Lock via
// rundll32 user32.dll,LockWorkStation, Windows' own documented route). A row along the dock, a
// column on a side dock.
//
// Icons: Segoe Fluent Icons glyphs (Windows 11's icon font), two drawn as paths. Nothing runs while
// idle: no timer, no provider; a click does its one thing.

const ACTIONS = {
  quick: { name: 'Quick settings', keys: 'win+a', draw: 'sliders' },
  taskview: { name: 'Task view', keys: 'win+tab', glyph: '' },
  clipboard: { name: 'Clipboard history', keys: 'win+v', glyph: '' },
  snip: { name: 'Snip', keys: 'win+shift+s', glyph: '' },
  notifications: { name: 'Notification centre', keys: 'win+n', glyph: '' },
  desktop: { name: 'Show desktop', keys: 'win+d', glyph: '' },
  emoji: { name: 'Emoji', keys: 'win+.', glyph: '' },
  explorer: { name: 'File Explorer', keys: 'win+e', glyph: '' },
  settings: { name: 'Settings', open: 'ms-settings:', glyph: '' },
  search: { name: 'Search', keys: 'win+s', glyph: '' },
  deskprev: { name: 'Previous desktop', keys: 'win+ctrl+left', glyph: '' },
  desknext: { name: 'Next desktop', keys: 'win+ctrl+right', glyph: '' },
  cast: { name: 'Cast', keys: 'win+k', glyph: '' },
  taskmgr: { name: 'Task Manager', keys: 'ctrl+shift+esc', draw: 'pulse' },
  lock: { name: 'Lock', open: 'rundll32.exe', args: 'user32.dll,LockWorkStation', glyph: '' },
};
const ICON_FONT = 'Segoe Fluent Icons';

let pressed = -1;  // the button under a press (drawn darker until the click)

const custom = (c) => (c && !/^#00[0-9a-f]{6}$/i.test(c) ? c : null);

function buttons() {
  const n = Math.max(1, Math.min(6, settings.count || 4));
  const out = [];
  for (let i = 1; i <= n; i++) {
    const a = ACTIONS[settings['button' + i]];
    if (a) out.push(a);
  }
  return out.length ? out : [ACTIONS.quick];
}

// --- layout: n square cells along the main axis --------------------------------------------------------

function layout(canvas, dock) {
  const list = buttons();
  const n = list.length;
  const vertical = dock.axis === 'vertical' && !dock.standalone;
  if (dock.standalone) {
    const W = canvas.width, H = canvas.height;
    const gap = Math.max(2, Math.min(W, H) * 0.05);
    const cell = Math.max(8, Math.min((W - gap * (n + 1)) / n, H - 2 * gap));
    return { list, n, vertical: false, cell, gap, x0: (W - (n * cell + (n - 1) * gap)) / 2, y0: (H - cell) / 2 };
  }
  const icon = dock.iconSize;
  const cell = Math.round(icon * 0.72 * (settings.scale || 1));
  const gap = Math.round(icon * 0.12);
  const along = n * cell + (n - 1) * gap;
  return { list, n, vertical, cell, gap, along };
}

export function measure(canvas, dock) {
  if (dock.standalone) return { main: Math.round(dock.width), cross: Math.round(dock.height) };  // the window's size
  const l = layout(canvas, dock);
  return { main: Math.ceil(l.along + 2 * l.gap), cross: Math.round(dock.iconSize) };
}

function cellRect(canvas, l, i) {
  if (l.x0 !== undefined) return { x: l.x0 + i * (l.cell + l.gap), y: l.y0, s: l.cell };
  const W = canvas.width, H = canvas.height;
  if (l.vertical) return { x: (W - l.cell) / 2, y: (H - l.along) / 2 + i * (l.cell + l.gap), s: l.cell };
  return { x: (W - l.along) / 2 + i * (l.cell + l.gap), y: (H - l.cell) / 2, s: l.cell };
}

// --- icons ---------------------------------------------------------------------------------------------

function sliders(canvas, x, y, s, ink) {
  // Quick settings: three toggles' tracks with their knobs.
  const sw = Math.max(1.2, s * 0.08);
  for (let i = 0; i < 3; i++) {
    const yy = y + s * (0.28 + 0.22 * i);
    canvas.line(x + s * 0.18, yy, x + s * 0.82, yy, ink, { width: sw, cap: 'round' });
    const kx = x + s * [0.66, 0.34, 0.56][i];
    canvas.fillCircle(kx, yy, s * 0.09, ink);
  }
}

function pulse(canvas, x, y, s, ink) {
  // Task Manager: a heartbeat line.
  const p = canvas.path().moveTo(x + s * 0.14, y + s * 0.55).lineTo(x + s * 0.34, y + s * 0.55).lineTo(x + s * 0.44, y + s * 0.3)
    .lineTo(x + s * 0.56, y + s * 0.76).lineTo(x + s * 0.66, y + s * 0.5).lineTo(x + s * 0.86, y + s * 0.5);
  canvas.stroke(p, ink, { width: Math.max(1.2, s * 0.08), cap: 'round', join: 'round' });
}

function icon(canvas, a, cx, cy, g, ink) {
  if (a.draw === 'sliders') return sliders(canvas, cx - g / 2, cy - g / 2, g, ink);
  if (a.draw === 'pulse') return pulse(canvas, cx - g / 2, cy - g / 2, g, ink);
  canvas.text(a.glyph, cx, cy, { font: ICON_FONT, size: g * 0.78, align: 'center', baseline: 'middle', color: ink });
}

export function draw(canvas, dock) {
  const l = layout(canvas, dock);
  const ink = custom(settings.iconColor) || dock.theme.text;
  const tiles = (settings.style || 'tiles') === 'tiles';
  l.list.forEach((a, i) => {
    const r = cellRect(canvas, l, i);
    const radius = r.s * 0.22;
    if (tiles || i === pressed) {
      canvas.fillRoundRect(r.x, r.y, r.s, r.s, radius, i === pressed ? '#40808080' : custom(settings.tileColor) || '#1F808080');
    }
    icon(canvas, a, r.x + r.s / 2, r.y + r.s / 2, r.s * 0.62, ink);
  });
}

// --- input -------------------------------------------------------------------------------------------

function indexAt(e) {
  // The box from dock.width / height (canvas sizes belong to draw()).
  const box = { width: dock.width, height: dock.height };
  const l = layout(box, dock);
  for (let i = 0; i < l.n; i++) {
    const r = cellRect(box, l, i);
    const slack = l.gap / 2;
    if (e.x >= r.x - slack && e.x < r.x + r.s + slack && e.y >= r.y - slack && e.y < r.y + r.s + slack) return i;
  }
  return -1;
}

function run(a) {
  if (a.keys) shell.sendKeys(a.keys);
  else shell.open(a.open, a.args || '');
}

export function onClick(e) {
  pressed = -1;
  requestRedraw();
  const i = indexAt(e);
  if (i < 0) return;
  try {
    run(buttons()[i]);
  } catch (err) {
    console.warn(buttons()[i].name + ': ' + err.message);
  }
}

export function onPress(e) {
  pressed = indexAt(e);  // not claimed: the dock may still drag the widget
  if (pressed >= 0) requestRedraw();
}

export function onHover(e) {
  if (!e.inside && pressed >= 0) {
    pressed = -1;
    requestRedraw();
  }
}

export function onSettingsChanged() {
  requestRedraw();
}
