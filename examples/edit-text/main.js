// popup.editText: a text editor window the dock opens next to the widget. It answers with a
// promise: the edited text after OK (or Enter / Ctrl+Enter), null after Cancel (or Escape, the
// close box, a reload of the widget). Only one editor per widget is open at a time: a second call
// while one is open rejects with an Error, so keep a flag.
let busy = false;

const label = () => storage.get('label', 'Click to rename');
const style = { size: 13, weight: 'semibold', align: 'center', baseline: 'middle' };

export function measure(canvas, dock) {
  const w = Math.ceil(canvas.measureText(label(), style).width) + 16;
  return dock.axis === 'vertical' ? { main: 40, cross: dock.iconSize } : { main: Math.min(240, w), cross: 32 };
}

export function draw(canvas, dock) {
  canvas.text(label(), canvas.width / 2, canvas.height / 2, { ...style, maxWidth: canvas.width - 8, color: dock.theme.text });
}

export async function onClick() {
  if (busy) return;
  busy = true;
  try {
    // multiline: false is a one-line box where Enter means OK; maxLength caps what can be typed.
    const text = await popup.editText({ title: 'Rename', text: label(), multiline: false, maxLength: 40 });
    if (text !== null && text.trim()) {
      storage.set('label', text.trim());
      requestRedraw();
    }
  } catch (e) {
    console.warn(e.message);  // e.g. "popup.editText: there is no editor for this widget here"
  } finally {
    busy = false;
  }
}
