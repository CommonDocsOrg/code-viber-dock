// Hello: a label and a click counter (docs/widgets.md walks through it).
// draw() runs only when the widget asks for it (requestRedraw) or the dock relays it out.

let count = 0;
let hot = false; // the pointer is over the widget

export function draw(canvas, dock) {
  const w = canvas.width, h = canvas.height;
  // On a dock the box is the size hint; standalone (a window of its own) it is the window, so
  // everything grows with it.
  const k = dock.standalone ? Math.max(1, Math.min(w / 64, h / 40)) : 1;
  const fill = canvas.linearGradient(0, 0, 0, h, [[0, '#30FFFFFF'], [1, '#12FFFFFF']]);
  canvas.fillRoundRect(0.5, 0.5, w - 1, h - 1, 8 * k, fill);
  canvas.strokeRoundRect(0.5, 0.5, w - 1, h - 1, 8 * k, hot ? dock.theme.accent : dock.theme.border, 1);
  canvas.text(settings.label, w / 2, h * 0.3, {
    size: 11 * k, weight: 'semibold', align: 'center', baseline: 'middle', color: dock.theme.text,
  });
  canvas.text(String(count), w / 2, h * 0.7, {
    size: 13 * k, weight: 'bold', align: 'center', baseline: 'middle', color: dock.theme.accent,
  });
}

export function onClick() {
  count++;
  requestRedraw();
}

export function onMiddleClick() {
  count = 0;
  requestRedraw();
}

// Return true: the wheel turn is used here (the dock does nothing else with it).
export function onWheel(e) {
  count += e.delta > 0 ? 1 : -1;
  requestRedraw();
  return true;
}

export function onHover(e) {
  hot = e.inside;
  requestRedraw();
}

export function onSettingsChanged() {
  requestRedraw();
}
