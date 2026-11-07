// Show desktop (round 11 phase 11B): the Windows 7 taskbar's "Show desktop" sliver. A click sends
// Win+D (show the desktop; a second Win+D brings the windows back) or Win+M (minimize every window)
// with shell.sendKeys. Aero Peek (hovering the sliver turns every window into glass) is not done: it
// needs DWM's peek on every window, which no API offers an app.
//
// style "strip": a translucent strip in the theme's text colour (10 %, 25 % hovered, 40 % pressed),
// `width` DIPs along the dock and as thick as the icons, with a hairline (35 %) on the side towards
// the other items (left on a top / bottom dock, top on a left / right one), as Windows 7 draws it.
// style "icon": a desktop glyph (a monitor) in an icon-sized slot, for other themes; a standalone
// widget always draws the glyph. Idle: nothing runs (one 2 s backstop timer after a press).

let hovered = false;
let pressed = false;
let pressTimer = 0;

const glyph = () => settings.style === 'icon';

export function measure(canvas, dock) {
  const s = Math.round(dock.iconSize);
  if (glyph()) return { main: s, cross: s };
  const width = Math.max(6, Math.min(24, Math.round(settings.width || 10)));
  return { main: width, cross: s };
}

// "#AARRGGBB" with its alpha multiplied by a (0 - 1).
function alpha(c, a) {
  const v = parseInt(String(c).slice(1), 16) >>> 0;
  const out = Math.round(((v >>> 24) & 255) * a);
  return '#' + out.toString(16).padStart(2, '0') + (v & 0xFFFFFF).toString(16).padStart(6, '0');
}

function strip(canvas, dock) {
  const w = canvas.width, h = canvas.height, text = dock.theme.text;
  canvas.fillRect(0, 0, w, h, alpha(text, pressed ? 0.4 : hovered ? 0.25 : 0.1));
  const px = 1 / (canvas.scale > 0 ? canvas.scale : 1);  // one device pixel
  if (dock.axis === 'vertical') canvas.fillRect(0, 0, w, px, alpha(text, 0.35));
  else canvas.fillRect(0, 0, px, h, alpha(text, 0.35));
}

// A monitor: the screen (a light fill under a rounded outline), the neck and the foot.
function monitor(canvas, dock) {
  const w = canvas.width, h = canvas.height, side = Math.min(w, h);
  const text = dock.theme.text;
  if (hovered || pressed) canvas.fillRoundRect(2, 2, w - 4, h - 4, side * 0.22, pressed ? '#40FFFFFF' : '#26FFFFFF');
  const g = side * 0.62, sw = Math.max(1, g * 0.07);
  const cx = w / 2, x0 = cx - g / 2, y0 = h / 2 - g * 0.42, sh = g * 0.62;
  canvas.fillRoundRect(x0, y0, g, sh, g * 0.06, alpha(text, 0.22));
  canvas.strokeRoundRect(x0, y0, g, sh, g * 0.06, text, sw);
  canvas.line(cx, y0 + sh, cx, y0 + sh + g * 0.16, text, sw);
  canvas.line(cx - g * 0.22, y0 + sh + g * 0.18, cx + g * 0.22, y0 + sh + g * 0.18, text, { width: sw, cap: 'round' });
}

export function draw(canvas, dock) {
  if (glyph() || dock.standalone) monitor(canvas, dock);
  else strip(canvas, dock);
}

function release() {
  if (pressTimer) clearTimeout(pressTimer);
  pressTimer = 0;
  if (!pressed) return;
  pressed = false;
  requestRedraw();
}

export function onPress() {
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
  const keys = settings.click === 'minimize' ? 'win+m' : 'win+d';
  try {
    shell.sendKeys(keys);
  } catch (e) {
    console.warn(e.message);
  }
}
