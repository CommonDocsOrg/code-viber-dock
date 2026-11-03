// Volume: the default output device's level (the `audio` provider, Core Audio). A speaker glyph
// with one to three waves (a cross when muted) and the level in percent under it. The wheel
// changes the level by settings.step percent a notch (unmuting on the way up), a middle click
// mutes, a click opens a popup: the level slider, Mute, Mute microphone, the output devices (a
// click makes one the default) and Sound settings. While the wheel turns, a volume bar (a passive
// popup: no focus, the mouse passes through it) stands beside the dock and goes 1.2 s after the
// last notch. Event-driven: audio.onChange redraws when the level, the mute state or the devices
// change (also when changed elsewhere: the volume keys, the quick settings); the bar's one-shot
// is the only timer, and only while the bar is up.

const POPUP_W = 300;
const BAR_W = 44, BAR_H = 150, BAR_MS = 1200;
let state = audio.get();
let hits = [];  // the popup's click targets: { x, y, w, h, act }
let bar = false;  // the volume bar (the passive popup) is up
let barTimer = 0;

audio.onChange((s) => {
  state = s;
  requestRedraw();
  if (popup.isOpen) popup.open(POPUP_W, popupHeight());  // resizes (the device list) and redraws
  else if (bar) popup.redraw();
});

// The bar's state goes (it closed, or the ordinary popup replaces it); the host closes the window.
function endBar() {
  if (barTimer) clearTimeout(barTimer);
  barTimer = 0;
  bar = false;
}

// Opens the bar (or redraws it in place) and starts its 1.2 s again.
function showBar() {
  bar = true;
  popup.open(BAR_W, BAR_H, { passive: true });
  if (barTimer) clearTimeout(barTimer);
  barTimer = setTimeout(() => {
    barTimer = 0;
    if (bar) popup.close();  // the bar is the one on screen: the ordinary popup ended it
  }, BAR_MS);
}

const percent = (v) => Math.round((v || 0) * 100);

// Our own changes show at once: Core Audio's notification of a mute change can take most of a
// second to arrive (the onChange that follows confirms it).
function changed() {
  requestRedraw();
  if (popup.isOpen || bar) popup.redraw();
}
function setVolume(v) {
  audio.setVolume(v);
  state.volume = v;
  changed();
}
function setMuted(b) {
  audio.setMuted(b);
  state.muted = b;
  changed();
}
function setMicMuted(b) {
  audio.setMicMuted(b);
  state.micMuted = b;
  changed();
}

// `text` cut to `w` DIPs with an ellipsis (one line).
function fit(canvas, text, style, w) {
  if (canvas.measureText(text, style).width <= w) return text;
  let lo = 0, hi = text.length;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (canvas.measureText(text.slice(0, mid) + '…', style).width <= w) lo = mid;
    else hi = mid - 1;
  }
  return text.slice(0, lo) + '…';
}

export function measure(canvas, dock) {
  const s = Math.round(dock.iconSize);
  return dock.axis === 'vertical' ? { main: s, cross: s } : { main: Math.round(s * 0.8), cross: s };
}

// A speaker in a g x g box at (x, y): waves 0 - 3, or a cross.
function speaker(canvas, x, y, g, waves, muted, color) {
  canvas.fill(canvas.path().moveTo(x + 0.06 * g, y + 0.36 * g).lineTo(x + 0.24 * g, y + 0.36 * g)
    .lineTo(x + 0.48 * g, y + 0.14 * g).lineTo(x + 0.48 * g, y + 0.86 * g).lineTo(x + 0.24 * g, y + 0.64 * g)
    .lineTo(x + 0.06 * g, y + 0.64 * g).close(), color);
  const st = { width: Math.max(1.2, g * 0.075), cap: 'round' };
  if (muted) {
    canvas.line(x + 0.62 * g, y + 0.38 * g, x + 0.88 * g, y + 0.62 * g, color, st);
    canvas.line(x + 0.88 * g, y + 0.38 * g, x + 0.62 * g, y + 0.62 * g, color, st);
    return;
  }
  const cx = x + 0.48 * g, cy = y + 0.5 * g;
  for (let i = 1; i <= 3; i++) {
    const r = 0.13 * g * i + 0.04 * g;
    canvas.stroke(canvas.path().arc(cx, cy, r, -0.85, 0.85), i <= waves ? color : '#30808080', st);
  }
}

const wavesFor = (v) => (v <= 0.005 ? 0 : v < 0.34 ? 1 : v < 0.67 ? 2 : 3);

export function draw(canvas, dock) {
  const w = canvas.width, h = canvas.height;
  const ink = state.available ? dock.theme.text : '#80808080';
  const text = settings.showPercent && state.available;
  const g = Math.min(w, h) * (text ? 0.58 : 0.66);
  const y = text ? h * 0.12 : (h - g) / 2;
  speaker(canvas, (w - g) / 2, y, g, wavesFor(state.volume), state.muted || !state.available, ink);
  if (text) {
    canvas.text(state.muted ? 'muted' : percent(state.volume) + '%', w / 2, h - 2,
      { size: Math.max(8, h * 0.21), weight: 'semibold', align: 'center', baseline: 'bottom', color: dock.theme.dateText });
  }
}

export function onWheel(e) {
  if (!state.available) return false;
  const step = (settings.step || 2) / 100;
  const base = Math.round(state.volume / step) * step;  // stay on the step's grid
  const v = Math.min(1, Math.max(0, base + Math.sign(e.notches) * Math.max(1, Math.round(Math.abs(e.notches))) * step));
  setVolume(v);
  if (state.muted && e.notches > 0) setMuted(false);
  // The bar, unless the ordinary popup is open (changed() redrew that one with the new level).
  if (settings.wheelBar && !popup.isOpen) showBar();
  return true;
}

export function onMiddleClick() {
  if (settings.middleClick === 'mute' && state.available) setMuted(!state.muted);
}

export function onClick(e) {
  if (e.closedPopup) return;
  endBar();  // the ordinary popup replaces the bar (the host closes it)
  popup.open(POPUP_W, popupHeight());
}

export function onPopupClose(e) {
  if (e.passive) endBar();
}

function popupHeight() {
  return 136 + (state.mic ? 30 : 0) + 30 + state.devices.length * 28 + 48;
}

// --- the popup ------------------------------------------------------------------------------------

function checkbox(canvas, x, y, on, label, dock, act) {
  const accent = dock.theme.accent;
  canvas.fillRoundRect(x, y + 4, 16, 16, 4, on ? accent : '#00000000');
  canvas.strokeRoundRect(x + 0.5, y + 4.5, 15, 15, 4, on ? accent : dock.theme.labelText, 1);
  if (on) {
    canvas.stroke(canvas.path().moveTo(x + 4, y + 12).lineTo(x + 7, y + 15).lineTo(x + 12.5, y + 8.5), '#FFFFFFFF',
      { width: 1.8, cap: 'round', join: 'round' });
  }
  const style = { size: 12.5, baseline: 'middle' };
  canvas.text(fit(canvas, label, style, POPUP_W - x - 40), x + 26, y + 12, { ...style, color: dock.theme.labelText });
  hits.push({ x: x - 4, y, w: POPUP_W - 2 * (x - 4), h: 24, act });
}

// The volume bar: the level on top, a vertical track filled from the bottom with a knob at the
// fill's top, the speaker under it.
function drawBar(canvas, dock) {
  const W = canvas.width, H = canvas.height;
  const ink = dock.theme.labelText;
  const v = state.volume || 0, off = state.muted || !state.available;
  canvas.text(off ? 'Muted' : percent(v) + '%', W / 2, 9,
    { size: 11, weight: 'semibold', align: 'center', color: ink });
  const tw = 8, tx = (W - tw) / 2, t0 = 32, t1 = H - 38;
  canvas.fillRoundRect(tx, t0, tw, t1 - t0, tw / 2, '#40808080');
  const ky = t1 - (t1 - t0) * v;
  canvas.fillRoundRect(tx, ky, tw, Math.max(tw, t1 - ky), tw / 2, off ? '#80808080' : dock.theme.accent);
  canvas.fillCircle(W / 2, ky, 6, '#FFFFFFFF');
  canvas.strokeCircle(W / 2, ky, 6, '#30000000', 1);
  speaker(canvas, (W - 20) / 2, H - 28, 20, wavesFor(v), off, ink);
}

export function drawPopup(canvas, dock) {
  if (bar && canvas.width === BAR_W && canvas.height === BAR_H) {
    drawBar(canvas, dock);
    return;
  }
  hits = [];
  const W = canvas.width;
  const ink = dock.theme.labelText, dim = dock.theme.dateText, accent = dock.theme.accent;
  canvas.text('Sound', 16, 14, { size: 14, weight: 'semibold', color: ink });
  canvas.text(state.available ? (state.muted ? 'Muted' : percent(state.volume) + '%') : '', W - 16, 14,
    { size: 14, weight: 'semibold', align: 'right', color: ink });
  canvas.text(fit(canvas, state.available ? state.device : 'No output device', { size: 11.5 }, W - 32), 16, 36, { size: 11.5, color: dim });
  // The slider, with the mute button in front of it.
  const sy = 72, x0 = 52, x1 = W - 20;
  canvas.fillRoundRect(12, sy - 14, 28, 28, 6, '#18FFFFFF');
  speaker(canvas, 16, sy - 10, 20, wavesFor(state.volume), state.muted, ink);
  hits.push({ x: 12, y: sy - 14, w: 28, h: 28, act: () => setMuted(!state.muted) });
  const v = state.volume || 0, kx = x0 + (x1 - x0) * v;
  canvas.fillRoundRect(x0, sy - 2.5, x1 - x0, 5, 2.5, '#40808080');
  canvas.fillRoundRect(x0, sy - 2.5, Math.max(5, kx - x0), 5, 2.5, state.muted ? '#80808080' : accent);
  canvas.fillCircle(kx, sy, 8, '#FFFFFFFF');
  canvas.strokeCircle(kx, sy, 8, '#30000000', 1);
  hits.push({ x: x0 - 10, y: sy - 14, w: x1 - x0 + 20, h: 28, act: (e) => {
    setVolume(Math.min(1, Math.max(0, (e.x - x0) / (x1 - x0))));
    if (state.muted) setMuted(false);
  } });
  let y = 100;
  checkbox(canvas, 16, y, state.muted, 'Mute', dock, () => setMuted(!state.muted));
  y += 30;
  if (state.mic) {
    checkbox(canvas, 16, y, state.micMuted, 'Mute microphone' + (state.micName ? ' (' + state.micName + ')' : ''), dock,
      () => setMicMuted(!state.micMuted));
    y += 30;
  }
  canvas.line(16, y + 2, W - 16, y + 2, '#30808080', 1);
  canvas.text('Output', 16, y + 10, { size: 11.5, weight: 'semibold', color: dim });
  y += 30;
  for (const d of state.devices) {
    if (d.default) canvas.fillRoundRect(8, y, W - 16, 26, 5, '#20FFFFFF');
    canvas.strokeCircle(24, y + 13, 6, d.default ? accent : ink, 1.2);
    if (d.default) canvas.fillCircle(24, y + 13, 3.2, accent);
    canvas.text(fit(canvas, d.name, { size: 12.5 }, W - 54), 38, y + 13, { size: 12.5, baseline: 'middle', color: ink });
    const id = d.id;
    hits.push({ x: 8, y, w: W - 16, h: 26, act: () => audio.setDevice(id) });
    y += 28;
  }
  y += 8;
  canvas.fillRoundRect(16, y, W - 32, 30, 6, '#20FFFFFF');
  canvas.strokeRoundRect(16.5, y + 0.5, W - 33, 29, 6, '#30808080', 1);
  canvas.text('Sound settings', W / 2, y + 15, { size: 12.5, align: 'center', baseline: 'middle', color: ink });
  hits.push({ x: 16, y, w: W - 32, h: 30, act: () => { shell.open('ms-settings:sound'); popup.close(); } });
}

export function onPopupClick(e) {
  if (e.button !== 'left') return;
  for (const h of hits) {
    if (e.x >= h.x && e.x < h.x + h.w && e.y >= h.y && e.y < h.y + h.h) {
      h.act(e);
      return;
    }
  }
}
