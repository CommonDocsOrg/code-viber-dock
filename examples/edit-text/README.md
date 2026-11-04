# Edit text (example widget)

A label you rename with a click: the smallest use of `popup.editText`, the dock's text editor.

| File | What it shows |
|---|---|
| `widget.json` | A manifest without permissions: text entry needs none (the editor is a window of the dock). |
| `main.js` | `onClick` awaits `popup.editText({ title, text, multiline: false, maxLength })`; the promise gives the text (OK, Enter) or `null` (Cancel, Escape); the answer goes to `storage` and the widget redraws. A flag keeps a second click from asking while the editor is open (that call would reject). |

Install it: Global settings > **Widget library** > **Add from folder…**, pick this folder, then
**Add to dock** (or **Add to the desktop**). The built-in **Notes** widget (`widgets/notes`) is the
larger worked example: a multi-line editor, one note per item, notes shared by id.

Check the manifest first: `WinDock.exe --validate-widget examples\edit-text`. The full API is in
[docs/widgets.md](../../docs/widgets.md) ("popup").
