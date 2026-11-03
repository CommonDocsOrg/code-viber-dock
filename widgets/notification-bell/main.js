// Notifications (placeholder): a bell that opens the notification centre with Win+N
// (shell.sendKeys('win+n')). The badge is a fixed number from the settings until WinDock reads
// notifications itself (roadmap phase 9). Idle: nothing runs.

let hovered = false;
let warned = false;

export function measure(canvas, dock) {
  const s = Math.round(dock.iconSize);
  return { main: s, cross: s };
}

function bell(canvas, cx, cy, g, color) {
  const top = cy - g * 0.42, rim = cy + g * 0.26, half = g * 0.4;
  canvas.fill(canvas.path().moveTo(cx - half, rim)
    .bezierTo(cx - half * 0.72, rim - g * 0.12, cx - half * 0.78, top + g * 0.1, cx - half * 0.55, top + g * 0.02)
    .quadTo(cx, top - g * 0.12, cx + half * 0.55, top + g * 0.02)
    .bezierTo(cx + half * 0.78, top + g * 0.1, cx + half * 0.72, rim - g * 0.12, cx + half, rim).close(), color);
  canvas.fillRoundRect(cx - half * 1.05, rim - g * 0.02, half * 2.1, g * 0.08, g * 0.04, color);
  canvas.fillEllipse(cx, rim + g * 0.13, g * 0.1, g * 0.08, color);
  canvas.fillCircle(cx, top - g * 0.04, g * 0.05, color);
}

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
  const w = canvas.width, h = canvas.height, s = Math.min(w, h), g = s * 0.56;
  if (hovered) canvas.fillRoundRect(2, 2, w - 4, h - 4, s * 0.22, '#26FFFFFF');
  if (!image(canvas, (w - g) / 2, (h - g) / 2, g)) bell(canvas, w / 2, h / 2, g, dock.theme.text);
  const n = settings.badge | 0;
  if (n > 0) {
    const text = n > 9 ? '9+' : String(n), r = s * 0.15;
    const bx = w / 2 + g * 0.38, by = h / 2 - g * 0.38;
    canvas.fillCircle(bx, by, r, settings.badgeColor || '#FFE0453A');
    canvas.text(text, bx, by + 0.5, { size: r * 1.25, weight: 'bold', align: 'center', baseline: 'middle', color: '#FFFFFFFF' });
  }
}

export function onSettingsChanged() {
  warned = false;
}

export function onHover(e) {
  hovered = e.inside;
  requestRedraw();
}

export function onClick() {
  shell.sendKeys('win+n');
}
