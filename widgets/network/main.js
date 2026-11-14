// Network (round 27 phase 27B): the taskbar's network icon, from the `network` provider
// (network.get(): { available, connected, internet, state, kind, name, bars, signal, metered }).
// Wi-Fi: a dot and three arcs, lit up to the signal (Windows' 0 - 5 bars mapped to the four steps);
// ethernet: a plug; cellular: five rising bars; a VPN: a padlock; anything else: a globe. No
// internet (local access only, or a captive portal asking to sign in): the icon with a small cross
// (an exclamation mark for the sign-in) in the corner; no connection at all: a crossed-out globe.
// Optionally a label (the network's name, or the state). A click opens a popup with the details
// and buttons for Windows' settings, or (settings.click) the quick settings / the settings page.
//
// Cost: nothing runs while the network does not change. The provider is event-driven (Windows'
// NetworkStatusChanged) and quiet while this widget cannot be seen; the widget redraws only when
// what it shows changed.

const POPUP_W = 280, POPUP_H = 150;

let net = network.get();
let hits = [];

network.onChange((s) => {
  if (same(s, net)) return;
  net = s;
  requestRedraw();  // re-measures too (the label's width may change)
  if (popup.isOpen) popup.redraw();
});

function same(a, b) {
  return a.connected === b.connected && a.state === b.state && a.kind === b.kind && a.name === b.name &&
    a.bars === b.bars && a.metered === b.metered;
}

export function onSettingsChanged() {
  requestRedraw();
}

// --- text ------------------------------------------------------------------------------------------

const KINDS = { wifi: 'Wi-Fi', ethernet: 'Ethernet', cellular: 'Cellular', vpn: 'VPN', other: 'Network', none: 'None' };

function stateText(s) {
  if (!s.available) return 'Unknown';
  if (!s.connected || s.state === 'none') return 'Not connected';
  if (s.state === 'constrained') return 'Sign-in needed';
  if (s.state === 'local') return 'No internet';
  return 'Connected';
}

function labelText(s) {
  if (settings.label === 'state') return stateText(s);
  if (!s.connected) return 'Not connected';
  if (s.state === 'local') return 'No internet';
  return s.name || stateText(s);
}

// Windows' 0 - 5 bars as the four steps of the Wi-Fi icon (the dot is step 1).
const wifiSteps = (bars) => (bars === null || bars === undefined ? 4 : bars <= 0 ? 0 : Math.min(4, Math.ceil((bars * 4) / 5)));

const custom = (c) => (c && !/^#00[0-9a-f]{6}$/i.test(c) ? c : null);
const inkOf = (dock) => custom(settings.iconColor) || dock.theme.text;
const textOf = (dock) => custom(settings.textColor) || dock.theme.dateText;
const warnOf = () => settings.warnColor || '#FFE81123';

// --- layout ----------------------------------------------------------------------------------------

function layout(canvas, dock, k = 1) {
  const vertical = dock.axis === 'vertical' && !dock.standalone;
  const icon = dock.standalone ? 48 : dock.iconSize;
  const g = Math.max(10, Math.round(icon * 0.5 * (settings.scale || 1) * k));  // the glyph's square
  let text = null;
  if (settings.showLabel) {
    const style = { size: Math.max(9, icon * 0.24) * k, font: settings.font || undefined };
    const str = labelText(net);
    const m = canvas.measureText(str, style);
    text = { str, style, w: Math.ceil(m.width), h: Math.ceil(m.height) };
    if (vertical && text.w > icon) text = null;  // no room across a side dock: the icon alone
  }
  const gap = Math.round((vertical ? 2 : 5) * k), pad = Math.round(3 * k);
  const w = vertical ? Math.max(g, text ? text.w : 0) : g + (text ? gap + text.w : 0);
  const h = vertical ? g + (text ? gap + text.h : 0) : Math.max(g, text ? text.h : 0);
  return { vertical, icon, g, text, gap, pad, w, h };
}

export function measure(canvas, dock) {
  const l = layout(canvas, dock);
  const icon = Math.round(dock.iconSize);
  return l.vertical ? { main: Math.ceil(l.h + 2 * l.pad + 4), cross: icon } : { main: Math.ceil(l.w + 2 * l.pad), cross: icon };
}

// --- glyphs (in a g x g square at x, y) ------------------------------------------------------------

function wifi(canvas, x, y, g, ink, steps) {
  const cx = x + g / 2, by = y + g * 0.86;
  const sw = Math.max(1.2, g * 0.085);
  const dim = (on) => (on ? 1 : 0.28);
  canvas.save();
  canvas.opacity(dim(steps >= 1));
  canvas.fillCircle(cx, by - sw * 0.2, sw * 0.95, ink);
  canvas.restore();
  for (let i = 1; i <= 3; i++) {
    const r = g * (0.2 + 0.2 * i);
    const a = canvas.path().arc(cx, by, r, -Math.PI * 0.75, -Math.PI * 0.25, false);
    canvas.save();
    canvas.opacity(dim(steps >= i + 1));
    canvas.stroke(a, ink, { width: sw, cap: 'round' });
    canvas.restore();
  }
}

function ethernet(canvas, x, y, g, ink) {
  // An RJ45 plug seen from the front: the body, the latch's notch, three of its contacts.
  const sw = Math.max(1.2, g * 0.08);
  const bw = g * 0.62, bh = g * 0.5, bx = x + (g - bw) / 2, byy = y + g * 0.3;
  const p = canvas.path()
    .moveTo(bx, byy).lineTo(bx + bw, byy).lineTo(bx + bw, byy + bh * 0.72)
    .lineTo(bx + bw * 0.7, byy + bh * 0.72).lineTo(bx + bw * 0.7, byy + bh).lineTo(bx + bw * 0.3, byy + bh)
    .lineTo(bx + bw * 0.3, byy + bh * 0.72).lineTo(bx, byy + bh * 0.72).close();
  canvas.stroke(p, ink, { width: sw, join: 'round' });
  for (let i = 0; i < 3; i++) {
    const px = bx + bw * (0.3 + 0.2 * i);
    canvas.line(px, byy + sw * 1.3, px, byy + bh * 0.32, ink, { width: sw * 0.8, cap: 'round' });
  }
  canvas.line(x + g / 2, y + g * 0.12, x + g / 2, byy, ink, { width: sw, cap: 'round' });  // the cable
}

function cellular(canvas, x, y, g, ink, bars) {
  const n = 5, gapw = g * 0.05, bw = (g * 0.8 - gapw * (n - 1)) / n;
  for (let i = 0; i < n; i++) {
    const h = g * (0.2 + 0.15 * i);
    const bx = x + g * 0.1 + i * (bw + gapw);
    canvas.save();
    canvas.opacity(bars === null || bars === undefined || i < bars ? 1 : 0.28);
    canvas.fillRoundRect(bx, y + g * 0.88 - h, bw, h, Math.min(bw, h) * 0.25, ink);
    canvas.restore();
  }
}

function padlock(canvas, x, y, g, ink) {
  const sw = Math.max(1.2, g * 0.085);
  const bw = g * 0.56, bh = g * 0.4, bx = x + (g - bw) / 2, byy = y + g * 0.46;
  canvas.fillRoundRect(bx, byy, bw, bh, g * 0.06, ink);
  const r = bw * 0.3;
  const shackle = canvas.path().moveTo(x + g / 2 - r, byy).lineTo(x + g / 2 - r, byy - g * 0.1)
    .arc(x + g / 2, byy - g * 0.1, r, Math.PI, Math.PI * 2, false).lineTo(x + g / 2 + r, byy);
  canvas.stroke(shackle, ink, { width: sw, cap: 'round' });
}

function globe(canvas, x, y, g, ink) {
  const sw = Math.max(1, g * 0.07);
  const cx = x + g / 2, cy = y + g / 2, r = g * 0.36;
  canvas.strokeCircle(cx, cy, r, ink, sw);
  canvas.strokeEllipse(cx, cy, r * 0.45, r, ink, sw);
  canvas.line(cx - r, cy, cx + r, cy, ink, sw);
}

// The corner badge: a cross (no internet, no connection) or "!" (sign in).
function badge(canvas, x, y, g, mark, color) {
  const r = g * 0.2, cx = x + g - r * 0.9, cy = y + g - r * 0.9;
  canvas.fillCircle(cx, cy, r, color);
  const s = r * 0.45, w = Math.max(1, r * 0.3);
  if (mark === '!') {
    canvas.line(cx, cy - s, cx, cy + s * 0.25, '#FFFFFFFF', { width: w, cap: 'round' });
    canvas.fillCircle(cx, cy + s * 0.8, w * 0.55, '#FFFFFFFF');
  } else {
    canvas.line(cx - s, cy - s, cx + s, cy + s, '#FFFFFFFF', { width: w, cap: 'round' });
    canvas.line(cx + s, cy - s, cx - s, cy + s, '#FFFFFFFF', { width: w, cap: 'round' });
  }
}

function glyph(canvas, dock, x, y, g) {
  const ink = inkOf(dock);
  if (!net.available || !net.connected) {
    canvas.save();
    canvas.opacity(0.7);
    globe(canvas, x, y, g, ink);
    canvas.restore();
    canvas.line(x + g * 0.16, y + g * 0.84, x + g * 0.84, y + g * 0.16, warnOf(), { width: Math.max(1.2, g * 0.09), cap: 'round' });
    return;
  }
  switch (net.kind) {
    case 'wifi': wifi(canvas, x, y, g, ink, wifiSteps(net.bars)); break;
    case 'ethernet': ethernet(canvas, x, y, g, ink); break;
    case 'cellular': cellular(canvas, x, y, g, ink, net.bars); break;
    case 'vpn': padlock(canvas, x, y, g, ink); break;
    default: globe(canvas, x, y, g, ink); break;
  }
  if (net.state === 'local') badge(canvas, x, y, g, 'x', warnOf());
  else if (net.state === 'constrained') badge(canvas, x, y, g, '!', '#FFF7B500');
}

// --- drawing ---------------------------------------------------------------------------------------

export function draw(canvas, dock) {
  let l = layout(canvas, dock);
  if (dock.standalone) {
    const k = Math.max(0.5, Math.min((canvas.width * 0.85) / Math.max(1, l.w), (canvas.height * 0.85) / Math.max(1, l.h)));
    l = layout(canvas, dock, k);
  }
  const W = canvas.width, H = canvas.height;
  if (l.vertical) {
    const top = Math.round((H - l.h) / 2);
    glyph(canvas, dock, Math.round((W - l.g) / 2), top, l.g);
    if (l.text) canvas.text(l.text.str, W / 2, top + l.g + l.gap, { ...l.text.style, align: 'center', color: textOf(dock) });
    return;
  }
  const left = dock.standalone || !l.text ? Math.round((W - l.w) / 2) : l.pad;
  glyph(canvas, dock, left, Math.round((H - l.g) / 2), l.g);
  if (l.text) {
    canvas.text(l.text.str, left + l.g + l.gap, H / 2, { ...l.text.style, baseline: 'middle', color: textOf(dock) });
  }
}

// --- clicks and the popup --------------------------------------------------------------------------

export function onClick(e) {
  if (e.closedPopup) return;  // the press closed the popup: a click toggles it
  if (settings.click === 'quick') { shell.sendKeys('win+a'); return; }
  if (settings.click === 'settings') { shell.open('ms-settings:network-status'); return; }
  net = network.get();
  requestRedraw();
  popup.open(POPUP_W, POPUP_H);
}

function button(canvas, dock, label, rightX, y, act) {
  const ink = dock.theme.labelText;
  const bw = Math.ceil(canvas.measureText(label, { size: 12.5 }).width) + 24;
  const b = { x: rightX - bw, y, w: bw, h: 26 };
  canvas.fillRoundRect(b.x, b.y, b.w, b.h, 6, '#20FFFFFF');
  canvas.strokeRoundRect(b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1, 6, '#30808080', 1);
  canvas.text(label, b.x + b.w / 2, b.y + b.h / 2, { size: 12.5, align: 'center', baseline: 'middle', color: ink });
  hits.push({ ...b, act });
  return b.x;
}

export function drawPopup(canvas, dock) {
  hits = [];
  const W = canvas.width;
  const ink = dock.theme.labelText, dim = dock.theme.dateText;
  glyph(canvas, { theme: { text: ink } }, 16, 14, 28);
  const title = net.connected ? net.name || KINDS[net.kind] || 'Network' : 'Not connected';
  canvas.text(title, 54, 14, { size: 14, weight: 'semibold', color: ink, maxWidth: W - 70 });
  const kind = net.connected ? KINDS[net.kind] || 'Network' : 'No network';
  canvas.text(kind + ' · ' + stateText(net) + (net.metered ? ' · metered' : ''), 54, 34, { size: 12, color: dim, maxWidth: W - 70 });
  if (net.connected && net.bars !== null && net.bars !== undefined) {
    canvas.text('Signal', 16, 66, { size: 12, color: ink });
    const x0 = 70, segW = 26, segH = 8;
    for (let i = 0; i < 5; i++) {
      canvas.fillRoundRect(x0 + i * (segW + 4), 70, segW, segH, segH / 2, i < net.bars ? dock.theme.accent : '#40808080');
    }
    canvas.text(net.bars + ' of 5', W - 16, 66, { size: 12, align: 'right', color: dim });
  }
  const left = button(canvas, dock, 'Network & internet settings', W - 16, 108, () => { shell.open('ms-settings:network-status'); popup.close(); });
  button(canvas, dock, 'Quick settings', left - 8, 108, () => { popup.close(); shell.sendKeys('win+a'); });
}

export function onPopupClick(e) {
  if (e.button !== 'left') return;
  const h = hits.find((t) => e.x >= t.x && e.x < t.x + t.w && e.y >= t.y && e.y < t.y + t.h);
  if (h) h.act();
}
