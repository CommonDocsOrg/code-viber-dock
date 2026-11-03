// Notifications: a bell with the unread count (the `notifications` provider, round 4 phase 9) that
// opens a notification centre: Windows' (Win+N, shell.sendKeys) or WinDock's own popup
// (notifications.openCentre()). With "Show the unread count" off the provider is never called, so
// WinDock does not read notifications for this widget (the fixed badge number shows instead).
// Idle: nothing runs; a change of the list calls onChange, which redraws.

let hovered = false;
let warned = false;
let listening = null;  // the unsubscribe of the onChange listener while the count is shown

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

// Subscribes while the unread count is on (the first call starts WinDock's notification service).
function follow() {
  if (settings.unread && !listening) {
    try {
      listening = notifications.onChange(() => requestRedraw());
    } catch (e) {
      if (!warned) console.warn('notifications: ' + e.message);
      warned = true;
    }
  } else if (!settings.unread && listening) {
    listening();
    listening = null;
  }
}

function count() {
  if (!settings.unread) return settings.badge | 0;
  try {
    return notifications.count();
  } catch (e) {
    return 0;
  }
}

export function draw(canvas, dock) {
  follow();
  const w = canvas.width, h = canvas.height, s = Math.min(w, h), g = s * 0.56;
  if (hovered) canvas.fillRoundRect(2, 2, w - 4, h - 4, s * 0.22, '#26FFFFFF');
  if (!image(canvas, (w - g) / 2, (h - g) / 2, g)) bell(canvas, w / 2, h / 2, g, dock.theme.text);
  const n = count();
  if (n > 0) {
    const text = n > 9 ? '9+' : String(n), r = s * 0.15;
    const bx = w / 2 + g * 0.38, by = h / 2 - g * 0.38;
    canvas.fillCircle(bx, by, r, settings.badgeColor || '#FFE0453A');
    canvas.text(text, bx, by + 0.5, { size: r * 1.25, weight: 'bold', align: 'center', baseline: 'middle', color: '#FFFFFFFF' });
  }
}

export function onSettingsChanged() {
  warned = false;
  follow();
}

export function onHover(e) {
  hovered = e.inside;
  requestRedraw();
}

export function onClick() {
  if (settings.click === 'centre') {
    try {
      notifications.openCentre();
      return;
    } catch (e) {
      console.warn('openCentre: ' + e.message);
    }
  }
  shell.sendKeys('win+n');
}
