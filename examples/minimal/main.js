// The smallest useful widget: one line of text. Everything a widget needs is here:
// - `measure(canvas, dock)` returns the box it wants (DIPs along / across the dock), so the dock
//   can lay it out; without it the manifest's "size" is used.
// - `draw(canvas, dock)` draws into that box. It runs when the widget asks (requestRedraw), when
//   its settings change and when the dock lays it out again; never per frame.
// Nothing else runs: no timer, no wake-up, no CPU while it sits on the dock.

const style = { size: 13, weight: 'semibold', align: 'center', baseline: 'middle' };

export function measure(canvas, dock) {
  const m = canvas.measureText(settings.text || ' ', style);
  // A little room on both sides; on a vertical dock the box is as wide as the dock's icons.
  const main = Math.ceil(m.width) + 16;
  return dock.axis === 'vertical' ? { main: Math.ceil(m.height) + 12, cross: dock.iconSize } : { main, cross: 32 };
}

export function draw(canvas, dock) {
  const color = settings.accent ? dock.theme.accent : dock.theme.text;
  canvas.text(settings.text, canvas.width / 2, canvas.height / 2, { ...style, color });
}

// The settings page (Settings > Widget settings) changed a value: `settings` is the new object
// already; draw again (measure runs first, so a longer text widens the box).
export function onSettingsChanged() {
  requestRedraw();
}
