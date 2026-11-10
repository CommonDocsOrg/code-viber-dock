// Start: a button that opens the Start menu. The click sends the Windows key
// (shell.sendKeys('start-near') or 'win', see 1.2.0 below; SendInput: the same as pressing it, so Start opens and closes as the key
// does); if Windows refuses the injected input (another program's window of a higher integrity
// level has the focus), the All apps folder (shell:AppsFolder) opens instead. Idle: nothing runs.
//
// Round 11 phase 11B (1.1.0): the "orb" icon, the Windows 7 Start orb drawn with canvas calls (a
// glossy sphere, the four-colour waving flag, the glass highlight): lighter with a soft outer glow
// while hovered, darker while pressed (onPress until the click / the pointer leaving, with a one-shot
// 2 s timer as the backstop for a press that ends in a drag). `size` 0 = automatic: 62 % of the slot
// for the logos and images, 100 % for the orb.
//
// Round 16 phase 16A (1.2.0): `menuPosition` "near" (the default) sends 'start-near' instead of
// 'win': the dock first moves the Start menu's host window so that the menu opens beside this
// button (above it on a bottom dock, beside it on a side dock, clamped to the monitor), and puts the
// window back when the menu closes. "windows" sends plain 'win' (Windows' own place). Where the
// menu's window is not found, 'start-near' is plain 'win'.

let hovered = false;
let pressed = false;
let pressTimer = 0;
let warned = false;

export function measure(canvas, dock) {
  const s = Math.round(dock.iconSize);
  return { main: s, cross: s };
}

// The four tiles snapped to device pixels (canvas.scale px per DIP): the tile size and the gap are
// whole pixels and the logo's corner lands on a pixel, centred on (cx, cy) to the nearest pixel, so
// the tiles' straight edges are crisp (at fractional DIPs they were anti-aliased: a blurred look).
function logo(canvas, cx, cy, g, mono, dock) {
  const k = canvas.scale > 0 ? canvas.scale : 1;
  const gapPx = Math.max(1, Math.round(g * 0.06 * k));
  const qPx = Math.max(1, Math.round((g * k - gapPx) / 2));
  const sizePx = 2 * qPx + gapPx;
  const x = Math.round(cx * k - sizePx / 2) / k, y = Math.round(cy * k - sizePx / 2) / k;
  const q = qPx / k, gap = gapPx / k, size = sizePx / k;
  const paint = mono ? dock.theme.text
    : canvas.linearGradient(x, y, x + size, y + size, [[0, '#FF3CB4FF'], [1, '#FF0063D1']]);
  canvas.fillRoundRect(x, y, q, q, q * 0.08, paint);
  canvas.fillRoundRect(x + q + gap, y, q, q, q * 0.08, paint);
  canvas.fillRoundRect(x, y + q + gap, q, q, q * 0.08, paint);
  canvas.fillRoundRect(x + q + gap, y + q + gap, q, q, q * 0.08, paint);
}

// "#AARRGGBB" moved towards white (k > 0) or black (k < 0) by |k|; the alpha is kept.
function shade(c, k) {
  const v = parseInt(c.slice(1), 16);
  const ch = [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255];
  const f = (x) => (k >= 0 ? x + (255 - x) * k : x * (1 + k));
  const out = [ch[0], f(ch[1]), f(ch[2]), f(ch[3])];
  return '#' + out.map((x) => Math.round(Math.max(0, Math.min(255, x))).toString(16).padStart(2, '0')).join('');
}

// The flag's four panes (red, green / blue, yellow) in a 2 x 2 grid, waving: every horizontal edge
// is the same S-curve, the left column lower than the right (the classic Windows flag).
const PANES = ['#FFE8402A', '#FF52B043', '#FF2C7FD1', '#FFF6C01B'];
function flag(canvas, cx, cy, R) {
  const fw = 0.54 * R, fh = 0.47 * R;            // half width / height of the flag
  const amp = 0.075 * R;                          // the wave: + left (lower), - right (higher)
  const gu = (0.075 * R) / (2 * fw), gv = (0.075 * R) / (2 * fh);  // the gap between panes
  const X = (u) => cx - fw + 2 * fw * u;
  const Y = (u, v) => cy - fh + 2 * fh * v + amp * Math.cos(Math.PI * u);
  const N = 8;
  for (let i = 0; i < 4; i++) {
    const col = i % 2, row = i >> 1;
    const u0 = col ? 0.5 + gu / 2 : 0, u1 = col ? 1 : 0.5 - gu / 2;
    const v0 = row ? 0.5 + gv / 2 : 0, v1 = row ? 1 : 0.5 - gv / 2;
    const p = canvas.path().moveTo(X(u0), Y(u0, v0));
    for (let s = 1; s <= N; s++) {
      const u = u0 + ((u1 - u0) * s) / N;
      p.lineTo(X(u), Y(u, v0));
    }
    for (let s = N; s >= 0; s--) {
      const u = u0 + ((u1 - u0) * s) / N;
      p.lineTo(X(u), Y(u, v1));
    }
    p.close();
    const top = Y(u0, v0) - amp, bottom = Y(u0, v1) + amp;
    canvas.fill(p, canvas.linearGradient(0, top, 0, bottom, [[0, shade(PANES[i], 0.3)], [1, PANES[i]]]));
  }
}

// The Windows 7 orb, centred on (cx, cy), radius R; k: 0 at rest, +0.2 hovered (lighter, glowing),
// -0.2 pressed (darker). `outer`: how far the hover glow may reach (the box's half side).
function orb(canvas, cx, cy, R, k, outer) {
  if (k > 0 && outer > R) {
    // The glow: a ring fading out from the sphere's edge (a gradient, no blur needed).
    const g = Math.min(outer, R * 1.12);
    canvas.fillCircle(cx, cy, g, canvas.radialGradient(cx, cy, g, g,
      [[0, '#60A8D8FF'], [Math.min(0.99, (R * 0.96) / g), '#60A8D8FF'], [1, '#00A8D8FF']]));
  }
  // The sphere: dark navy at the rim to a deeper blue inside.
  canvas.fillCircle(cx, cy, R, canvas.radialGradient(cx, cy - R * 0.1, R * 1.02, R * 1.02,
    [[0, shade('#FF2C74C8', k)], [0.55, shade('#FF1E5FB4', k)], [0.85, shade('#FF123F80', k)], [1, shade('#FF0B2A5C', k)]]));
  // The light from below (the glass's bottom glow), clipped to the sphere by the circle's fill.
  canvas.fillCircle(cx, cy, R, canvas.radialGradient(cx, cy + R * 0.78, R * 0.9, R * 0.62,
    [[0, shade('#B868D0FF', k)], [1, '#0068D0FF']]));
  flag(canvas, cx, cy + R * 0.02, R);
  // The glass: an elliptical highlight over the upper part (white 55 % fading out), then a soft
  // radial gloss over the upper half of the whole orb.
  canvas.fillEllipse(cx, cy - R * 0.47, R * 0.8, R * 0.47, canvas.linearGradient(0, cy - R * 0.94, 0, cy - R * 0.02,
    [[0, '#8CFFFFFF'], [1, '#00FFFFFF']]));
  canvas.fillCircle(cx, cy, R, canvas.radialGradient(cx, cy - R * 0.55, R * 0.95, R * 0.6,
    [[0, '#40FFFFFF'], [1, '#00FFFFFF']]));
  // The rim: a thin light edge.
  const w = Math.max(1 / (canvas.scale > 0 ? canvas.scale : 1), R * 0.045);
  canvas.strokeCircle(cx, cy, R - w / 2, k > 0 ? '#B0FFFFFF' : '#80FFFFFF', w);
}

// settings.image: a package file or an absolute path (its shell icon); false if it cannot be drawn.
function image(canvas, x, y, g) {
  const p = String(settings.image || '');
  if (!p) return false;
  try {
    if (/^[A-Za-z]:|^\\\\|^%/.test(p)) canvas.fileIcon(p, x, y, g);
    else canvas.image(p, x, y, g, g);
    return true;
  } catch (e) {
    if (!warned) console.warn('icon: ' + e.message);
    warned = true;
    return false;
  }
}

export function draw(canvas, dock) {
  const w = canvas.width, h = canvas.height, side = Math.min(w, h);
  const isOrb = settings.icon === 'orb';
  const pct = settings.size > 0 ? Math.max(30, settings.size) : isOrb ? 100 : 62;
  const g = (side * pct) / 100;
  if (isOrb) {
    orb(canvas, w / 2, h / 2, (g * 0.92) / 2, pressed ? -0.2 : hovered ? 0.2 : 0, side / 2);
    return;
  }
  if (hovered) canvas.fillRoundRect(2, 2, w - 4, h - 4, side * 0.22, '#26FFFFFF');
  if (settings.icon === 'image' && image(canvas, (w - g) / 2, (h - g) / 2, g)) return;
  logo(canvas, w / 2, h / 2, g * 0.86, settings.icon === 'mono', dock);
}

export function onSettingsChanged() {
  warned = false;
}

function release() {
  if (pressTimer) clearTimeout(pressTimer);
  pressTimer = 0;
  if (!pressed) return;
  pressed = false;
  requestRedraw();
}

export function onPress() {
  if (settings.icon !== 'orb') return;
  pressed = true;
  if (pressTimer) clearTimeout(pressTimer);
  pressTimer = setTimeout(() => { pressTimer = 0; release(); }, 2000);
  requestRedraw();
}

export function onHover(e) {
  hovered = e.inside;
  if (!e.inside) release();
  requestRedraw();
}

export function onClick() {
  release();
  try {
    shell.sendKeys(settings.menuPosition === 'windows' ? 'win' : 'start-near');
  } catch (e) {
    console.warn(e.message + ': opening All apps instead');
    shell.open('shell:AppsFolder');
  }
}
