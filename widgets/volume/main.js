// Volume: the default output device's level (the `audio` provider, Core Audio). A speaker glyph
// with one to three waves (a cross when muted) and the level in percent under it. The wheel
// changes the level by settings.step percent a notch (unmuting on the way up), a middle click
// mutes, a click opens a popup: the level slider, Mute, Mute microphone, the output devices (a
// click makes one the default) and Sound settings. While the wheel turns, a volume bar (a passive
// popup: no focus, the mouse passes through it) stands beside the dock and goes 1.2 s after the
// last notch. Event-driven: audio.onChange redraws when the level, the mute state or the devices
// change (also when changed elsewhere: the volume keys, the quick settings); the bar's one-shot
// is the only timer, and only while the bar is up.
//
// Round 22 phase 22A (1.1.0): the look. settings.style is the view: "icon" (as before), "ring" (a
// circle that fills clockwise from the top, the icon inside it), "bar-horizontal" / "bar-vertical"
// (a bar that fills left to right / bottom to top, in a row with the icon and the percentage on a
// bottom / top dock and in a column on a side dock). settings.icon: the speaker, level bars,
// headphones, a note, an image (settings.image: a picture, or any file's shell icon) or none.
// settings.scale multiplies every size, as the Digital clock's (a standalone window fits the
// design to itself instead, in the arrangement, row or column, that comes out larger);
// settings.font is the percentage's family; iconColor / textColor / levelColor replace the
// theme's text, date and accent colours (a transparent value, the colour field's "the theme's",
// is not set). Every view is a design measured in one unit u (the dock's icon size x the scale):
// design() gives its box and its paint, measure() and draw() both use it.

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

// --- the look (round 22 phase 22A) -----------------------------------------------------------------

const STYLES = ['icon', 'ring', 'bar-horizontal', 'bar-vertical'];
const ICONS = ['speaker', 'bars', 'headphones', 'note', 'image', 'none'];
const viewStyle = () => (STYLES.includes(settings.style) ? settings.style : 'icon');
const iconKind = () => (ICONS.includes(settings.icon) ? settings.icon : 'speaker');

// A colour setting that is set: "#AARRGGBB" / "#RRGGBB" with a non-zero alpha.
function colorOf(value, fallback) {
  if (typeof value !== 'string' || !/^#([0-9a-f]{6}|[0-9a-f]{8})$/i.test(value)) return fallback;
  if (value.length === 9 && value.slice(1, 3) === '00') return fallback;
  return value;
}
const lookScale = () => (typeof settings.scale === 'number' && settings.scale > 0 ? settings.scale : 1);
const lookFont = () => (typeof settings.font === 'string' && settings.font.trim() ? { font: settings.font.trim() } : {});
const textStyle = (size) => ({ size, weight: 'semibold', ...lookFont() });

// The percentage is written: the setting, and always in the icon view without an icon (the text
// then is all there is to draw).
const showText = () => (settings.showPercent && state.available) || (viewStyle() === 'icon' && iconKind() === 'none');
const label = () => (!state.available ? '–' : state.muted ? 'muted' : percent(state.volume) + '%');
// The room the text takes: the wider of its two longest values, so the dock is not laid out again
// with every notch.
function textBox(canvas, style) {
  const a = canvas.measureText('100%', style), b = canvas.measureText('muted', style);
  return { width: Math.max(a.width, b.width), height: Math.max(a.height, b.height) };
}
// One line at (x, y) per the style's align, made smaller when it is wider than maxW.
function fitText(canvas, str, x, y, style, maxW) {
  const w = canvas.measureText(str, style).width;
  canvas.text(str, x, y, w > maxW && w > 0 ? { ...style, size: (style.size * maxW) / w } : style);
}

// settings.image in a g x g box: a picture (a package file, or a picture file anywhere: kept in
// proportion) or any other file's shell icon; faded while muted. False if it cannot be drawn (the
// speaker is drawn then).
let warnedFor = null;
function picture(canvas, x, y, g, off) {
  const p = String(settings.image || '').trim();
  if (!p) return false;
  try {
    const absolute = /^[A-Za-z]:|^\\\\|^%/.test(p);
    if (absolute && !/\.(png|jpe?g|jfif|webp|bmp|gif|tiff?|heic|heif|avif)$/i.test(p)) {
      canvas.save();
      if (off) canvas.opacity(0.4);
      canvas.fileIcon(p, x, y, g);
      canvas.restore();
    } else {
      const s = canvas.imageSize(p);
      const k = Math.min(g / Math.max(1, s.width), g / Math.max(1, s.height));
      canvas.image(p, x + (g - s.width * k) / 2, y + (g - s.height * k) / 2, s.width * k, s.height * k, off ? 0.4 : 1);
    }
    return true;
  } catch (e) {
    if (warnedFor !== p) console.warn('image: ' + e.message);
    warnedFor = p;
    return false;
  }
}

// settings.icon in a g x g box at (x, y). Off (muted, no device): the speaker gets its cross, the
// others are faded and crossed out.
function glyph(canvas, x, y, g, v, off, color) {
  const kind = iconKind();
  if (kind === 'none') return;
  if (kind === 'image' && picture(canvas, x, y, g, off)) return;
  if (kind === 'speaker' || kind === 'image') {
    speaker(canvas, x, y, g, wavesFor(v), off, color);
    return;
  }
  canvas.save();
  if (off) canvas.opacity(0.45);
  if (kind === 'bars') {
    // Four bars, lit from the left by quarters of the level.
    const lit = off || v <= 0.005 ? 0 : Math.ceil(v * 4);
    for (let i = 0; i < 4; i++) {
      const bh = (0.28 + 0.2 * i) * g;
      canvas.fillRoundRect(x + (0.095 + 0.22 * i) * g, y + 0.94 * g - bh, 0.15 * g, bh, 0.045 * g, i < lit || off ? color : '#40808080');
    }
  } else if (kind === 'headphones') {
    canvas.stroke(canvas.path().arc(x + 0.5 * g, y + 0.54 * g, 0.36 * g, Math.PI, 2 * Math.PI), color,
      { width: Math.max(1.2, 0.09 * g), cap: 'round' });
    canvas.fillRoundRect(x + 0.08 * g, y + 0.5 * g, 0.17 * g, 0.36 * g, 0.06 * g, color);
    canvas.fillRoundRect(x + 0.75 * g, y + 0.5 * g, 0.17 * g, 0.36 * g, 0.06 * g, color);
  } else {
    // The note: a head, a stem and a flag.
    canvas.save();
    canvas.rotate(-0.35, x + 0.36 * g, y + 0.75 * g);
    canvas.fillEllipse(x + 0.36 * g, y + 0.75 * g, 0.17 * g, 0.125 * g, color);
    canvas.restore();
    const st = { width: Math.max(1.2, 0.08 * g), cap: 'round', join: 'round' };
    canvas.stroke(canvas.path().moveTo(x + 0.5 * g, y + 0.72 * g).lineTo(x + 0.5 * g, y + 0.14 * g)
      .quadTo(x + 0.56 * g, y + 0.32 * g, x + 0.78 * g, y + 0.4 * g), color, st);
  }
  canvas.restore();
  if (off) canvas.line(x + 0.14 * g, y + 0.14 * g, x + 0.86 * g, y + 0.86 * g, color, { width: Math.max(1.2, 0.09 * g), cap: 'round' });
}

// What every view draws with.
function look(dock) {
  const off = state.muted || !state.available;
  return {
    v: Math.min(1, Math.max(0, state.volume || 0)),
    off,
    icon: state.available ? colorOf(settings.iconColor, dock.theme.text) : '#80808080',
    text: colorOf(settings.textColor, dock.theme.dateText),
    level: off ? '#80808080' : colorOf(settings.levelColor, dock.theme.accent),
    track: '#40808080',
  };
}

// The icon and ring views: one square (the icon view on a bottom / top dock: 0.8 as wide, as the
// widget always was) holding the icon, or the ring around it, with the percentage under it.
// Without an icon the percentage takes its place.
function badgeDesign(canvas, u, row) {
  const ring = viewStyle() === 'ring';
  const W = ring || !row ? u : 0.8 * u, H = u;
  return { w: W, h: H, paint(x, y, c) {
    const m = Math.min(W, H), cx = x + W / 2;
    const hasIcon = iconKind() !== 'none', text = showText();
    const under = text && hasIcon;
    if (ring) {
      const d = m * (under ? 0.64 : 0.88), t = Math.max(2, d * 0.1), r = (d - t) / 2;
      const cy = under ? y + H * 0.06 + d / 2 : y + H / 2;
      canvas.strokeCircle(cx, cy, r, c.track, t);
      if (c.v >= 0.999) canvas.strokeCircle(cx, cy, r, c.level, t);
      else if (c.v > 0.005) {
        canvas.stroke(canvas.path().arc(cx, cy, r, -Math.PI / 2, -Math.PI / 2 + 2 * Math.PI * c.v), c.level, { width: t, cap: 'round' });
      }
      const inner = d - 2 * t;
      if (hasIcon) glyph(canvas, cx - inner * 0.31, cy - inner * 0.31, inner * 0.62, c.v, c.off, c.icon);
      else if (text) fitText(canvas, label(), cx, cy, { ...textStyle(Math.max(7, inner * 0.36)), align: 'center', baseline: 'middle', color: c.text }, inner * 0.88);
    } else if (hasIcon) {
      const g = m * (under ? 0.58 : 0.66);
      glyph(canvas, cx - g / 2, under ? y + H * 0.12 : y + (H - g) / 2, g, c.v, c.off, c.icon);
    } else {
      fitText(canvas, label(), cx, y + H / 2, { ...textStyle(Math.max(8, m * 0.34)), align: 'center', baseline: 'middle', color: c.text }, W * 0.92);
    }
    if (under) {
      fitText(canvas, label(), cx, y + H * 0.96, { ...textStyle(Math.max(8, H * 0.21)), align: 'center', baseline: 'bottom', color: c.text }, W * 0.96);
    }
  } };
}

// The bar views: the icon, the bar and the percentage one after the other, in a row (bottom / top
// docks) or a column (side docks). The bar is long when it runs along that direction (1.3 u) and
// short across it (0.8 u). A bottom-to-top bar in a column is read as the wheel's volume bar: the
// percentage on top, the icon at the foot.
function barDesign(canvas, u, row) {
  const horizontal = viewStyle() === 'bar-horizontal';
  const along = horizontal === row;
  const t = Math.max(3, 0.12 * u), g = 0.46 * u, gap = 0.1 * u, pad = 0.06 * u;
  const parts = [];
  if (iconKind() !== 'none') parts.push({ kind: 'icon', a: g, c: g });
  parts.push({ kind: 'bar', a: along ? 1.3 * u : t, c: along ? t : 0.8 * u });
  if (showText()) {
    const style = textStyle(Math.max(8, (row ? 0.26 : 0.21) * u));
    const m = textBox(canvas, style);
    parts.push({ kind: 'text', a: row ? m.width : m.height, c: row ? m.height : m.width, style });
  }
  if (!row && !horizontal) parts.reverse();
  let a = 2 * pad + gap * (parts.length - 1), across = 0;
  for (const p of parts) {
    a += p.a;
    across = Math.max(across, p.c);
  }
  across += 2 * pad;
  return { w: row ? a : across, h: row ? across : a, paint(x, y, c) {
    let at = pad;
    for (const p of parts) {
      // The part's box: `a` along the row / column, `c` across it, centred across.
      const px = row ? x + at : x + (across - p.c) / 2, py = row ? y + (across - p.c) / 2 : y + at;
      const pw = row ? p.a : p.c, ph = row ? p.c : p.a;
      if (p.kind === 'icon') glyph(canvas, px, py, g, c.v, c.off, c.icon);
      else if (p.kind === 'text') {
        // In a row the text starts right after the bar (its box is as wide as its widest value).
        fitText(canvas, label(), row ? px : px + pw / 2, py + ph / 2,
          { ...p.style, align: row ? 'left' : 'center', baseline: 'middle', color: c.text }, pw);
      } else {
        canvas.fillRoundRect(px, py, pw, ph, t / 2, c.track);
        if (c.v > 0.005) {
          if (horizontal) canvas.fillRoundRect(px, py, Math.max(t, pw * c.v), ph, t / 2, c.level);
          else {
            const fh = Math.max(t, ph * c.v);
            canvas.fillRoundRect(px, py + ph - fh, pw, fh, t / 2, c.level);
          }
        }
      }
      at += p.a + gap;
    }
  } };
}

const design = (canvas, u, row) => (viewStyle().startsWith('bar') ? barDesign(canvas, u, row) : badgeDesign(canvas, u, row));

export function measure(canvas, dock) {
  const row = dock.axis !== 'vertical';
  const d = design(canvas, dock.iconSize * lookScale(), row);
  // Never thinner than an icon across the dock: the wheel and the click keep their target.
  const s = Math.round(dock.iconSize);
  return row ? { main: Math.ceil(d.w), cross: Math.max(s, Math.ceil(d.h)) } : { main: Math.ceil(d.h), cross: Math.max(s, Math.ceil(d.w)) };
}

export function onSettingsChanged() {
  requestRedraw();  // also measures again
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
  // How much of the box the design at 1 unit fills, for a row and for a column.
  const fit = (row) => {
    const d = design(canvas, 100, row);
    return Math.min(w / d.w, h / d.h) * 100;
  };
  let row = dock.axis !== 'vertical', u = dock.iconSize * lookScale();
  if (dock.standalone) {
    // A window of its own: the arrangement that comes out larger, grown to fill the window.
    const a = fit(true), b = fit(false);
    row = a >= b;
    u = Math.max(a, b) * 0.94;
  } else {
    u = Math.min(u, fit(row));  // a box smaller than measured (a preview's): the design shrinks into it
  }
  const d = design(canvas, u, row);
  d.paint((w - d.w) / 2, (h - d.h) / 2, look(dock));
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
