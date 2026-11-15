// Network (round 27 phase 27B): the taskbar's network icon, from the `network` provider
// (network.get(): { available, connected, internet, state, kind, name, bars, signal, metered }).
// Wi-Fi: a fan of a wedge and three bands, lit up to the signal (Windows' 0 - 5 bars as four steps);
// ethernet: a plug; cellular: five rising bars; a VPN: a padlock; anything else: a globe. No
// internet (local access only, or a captive portal asking to sign in): the icon with a small cross
// (an exclamation mark for the sign-in) in the corner; no connection at all: a crossed-out globe.
// Optionally a label (the network's name, or the state). A click opens a popup with the details
// and buttons for Windows' settings, or (settings.click) the quick settings / the settings page.
//
// Cost: nothing runs while the network does not change. The provider is event-driven (Windows'
// NetworkStatusChanged) and quiet while this widget cannot be seen; the widget redraws only when
// what it shows changed.

let net = network.get();
let hits = [];

network.onChange((s) => {
  if (same(s, net)) return;
  net = s;
  requestRedraw();  // re-measures too (the label's width may change)
  if (popup.isOpen) openPopup();  // its height follows the state (the signal card)
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

// Widget 1.1.0: a fan (a quarter circle standing on its point) cut into a wedge and three bands,
// the way Windows 11 draws it, instead of a dot and three thin round-capped arcs. The old arcs
// reached a radius of 0.8 g at a 90 degree span, 1.13 g across: wider than the glyph's own square,
// which made the icon look bigger than its neighbours. The fan's radius is 0.68 g (0.96 g across).
function wifi(canvas, x, y, g, ink, steps) {
  const cx = x + g / 2, by = y + g * 0.83;
  const R = g * 0.68, a0 = -Math.PI * 0.75, a1 = -Math.PI * 0.25;
  const r0 = R * 0.22, gap = R * 0.085, t = (R - r0 - 3 * gap) / 3;
  const dim = (on) => (on ? 1 : 0.3);
  canvas.save();
  canvas.opacity(dim(steps >= 1));
  canvas.fill(canvas.path().moveTo(cx, by).arc(cx, by, r0, a0, a1, false).close(), ink);
  canvas.restore();
  for (let i = 1; i <= 3; i++) {
    const r = r0 + i * gap + (i - 0.5) * t;
    const p = canvas.path();
    p.moveTo(cx + r * Math.cos(a0), by + r * Math.sin(a0));
    p.arc(cx, by, r, a0, a1, false);
    canvas.save();
    canvas.opacity(dim(steps >= i + 1));
    canvas.stroke(p, ink, { width: t, cap: 'butt' });
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

function glyph(canvas, ink, x, y, g) {
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
    glyph(canvas, inkOf(dock), Math.round((W - l.g) / 2), top, l.g);
    if (l.text) canvas.text(l.text.str, W / 2, top + l.g + l.gap, { ...l.text.style, align: 'center', color: textOf(dock) });
    return;
  }
  const left = dock.standalone || !l.text ? Math.round((W - l.w) / 2) : l.pad;
  glyph(canvas, inkOf(dock), left, Math.round((H - l.g) / 2), l.g);
  if (l.text) {
    canvas.text(l.text.str, left + l.g + l.gap, H / 2, { ...l.text.style, baseline: 'middle', color: textOf(dock) });
  }
}

// --- clicks and the popup --------------------------------------------------------------------------
//
// Widget 1.1.0: the popup is laid out from its width (everything sits between the two 16 DIP
// margins), so nothing can run off an edge: 1.0.0 placed two buttons as wide as their labels from
// the right, 304 DIPs of buttons in a 280 DIP popup, and the left one was cut off. Its height
// follows what there is to show (the signal card only for Wi-Fi and cellular). Colours come from
// the theme's label text at a low alpha, so the cards and buttons show on a light panel too.

const POPUP_W = 300, PAD = 16;
const SIGNAL = ['No signal', 'Very weak', 'Weak', 'Fair', 'Good', 'Excellent'];

const hasBars = (s) => s.available && s.connected && s.bars !== null && s.bars !== undefined;
const popupHeight = (s) => (hasBars(s) ? 188 : 118);

// A theme colour ("#AARRGGBB" or "#RRGGBB") at another alpha (0 - 1).
function alpha(c, a) {
  const rgb = typeof c === 'string' && /^#[0-9a-f]{8}$/i.test(c) ? c.slice(3) : typeof c === 'string' && /^#[0-9a-f]{6}$/i.test(c) ? c.slice(1) : 'FFFFFF';
  return '#' + Math.round(a * 255).toString(16).padStart(2, '0') + rgb;
}

// text()'s maxWidth wraps; a long network name must end in an ellipsis on its one line instead.
function fit(canvas, str, style, maxW) {
  if (canvas.measureText(str, style).width <= maxW) return str;
  let lo = 0, hi = str.length;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (canvas.measureText(str.slice(0, mid).trimEnd() + '…', style).width <= maxW) lo = mid; else hi = mid - 1;
  }
  return str.slice(0, lo).trimEnd() + '…';
}

// The state's colour: the dot beside the state and the tint of the icon's disc.
function stateColor(s) {
  if (!s.available || !s.connected || s.state === 'none' || s.state === 'local') return warnOf();
  if (s.state === 'constrained') return '#FFF7B500';
  return '#FF3FB950';
}

function openPopup() {
  popup.open(POPUP_W, popupHeight(net));
}

export function onClick(e) {
  if (e.closedPopup) return;  // the press closed the popup: a click toggles it
  if (settings.click === 'quick') { shell.sendKeys('win+a'); return; }
  if (settings.click === 'settings') { shell.open('ms-settings:network-status'); return; }
  net = network.get();
  requestRedraw();
  openPopup();
}

function button(canvas, ink, label, x, y, w, act) {
  const b = { x, y, w, h: 30 };
  canvas.fillRoundRect(b.x, b.y, b.w, b.h, 7, alpha(ink, 0.09));
  canvas.strokeRoundRect(b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1, 7, alpha(ink, 0.12), 1);
  const style = { size: 12.5 };
  canvas.text(fit(canvas, label, style, b.w - 12), b.x + b.w / 2, b.y + b.h / 2, { ...style, align: 'center', baseline: 'middle', color: ink });
  hits.push({ ...b, act });
}

export function drawPopup(canvas, dock) {
  hits = [];
  const W = canvas.width, inner = W - 2 * PAD;
  const ink = dock.theme.labelText, dim = dock.theme.dateText;
  const online = net.available && net.connected;
  const tone = stateColor(net);

  // The header: the icon on a disc in the state's colour, the name, the state and the kind.
  const disc = 40, dcx = PAD + disc / 2, dcy = PAD + disc / 2;
  canvas.fillCircle(dcx, dcy, disc / 2, alpha(net.internet ? dock.theme.accent : tone, 0.2));
  glyph(canvas, ink, dcx - 11, dcy - 11, 22);
  const tx = PAD + disc + 12, tw = W - PAD - tx;
  const titleStyle = { size: 14.5, weight: 'semibold' };
  const title = online ? net.name || KINDS[net.kind] || 'Network' : 'Not connected';
  canvas.text(fit(canvas, title, titleStyle, tw), tx, dcy - 19, { ...titleStyle, color: ink });
  const parts = [stateText(net)];
  if (online) parts.push(KINDS[net.kind] || 'Network');
  if (online && net.metered) parts.push('Metered');
  const subStyle = { size: 12 };
  canvas.fillCircle(tx + 3.5, dcy + 10, 3.5, tone);
  canvas.text(fit(canvas, parts.join(' · '), subStyle, tw - 13), tx + 13, dcy + 10, { ...subStyle, baseline: 'middle', color: dim });

  // The signal (Wi-Fi, cellular): Windows' five bars as segments across the card, and in a word.
  let y = PAD + disc + PAD;
  if (hasBars(net)) {
    const bars = Math.max(0, Math.min(5, net.bars));
    canvas.fillRoundRect(PAD, y, inner, 58, 8, alpha(ink, 0.06));
    canvas.text('Signal', PAD + 12, y + 11, { size: 12, color: dim });
    canvas.text(SIGNAL[bars], W - PAD - 12, y + 11, { size: 12, weight: 'semibold', align: 'right', color: ink });
    const sx = PAD + 12, sg = 4, sw = (inner - 24 - 4 * sg) / 5;
    for (let i = 0; i < 5; i++) {
      canvas.fillRoundRect(sx + i * (sw + sg), y + 36, sw, 6, 3, i < bars ? dock.theme.accent : alpha(ink, 0.14));
    }
    y += 58 + 12;
  }

  // Two buttons of the same width, side by side between the margins.
  const bw = (inner - 8) / 2;
  button(canvas, ink, 'Quick settings', PAD, y, bw, () => { popup.close(); shell.sendKeys('win+a'); });
  button(canvas, ink, 'Network settings', PAD + bw + 8, y, bw, () => { shell.open('ms-settings:network-status'); popup.close(); });
}

export function onPopupClick(e) {
  if (e.button !== 'left') return;
  const h = hits.find((t) => e.x >= t.x && e.x < t.x + t.w && e.y >= t.y && e.y < t.y + t.h);
  if (h) h.act();
}
