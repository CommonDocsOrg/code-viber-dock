# Quick links (example widget: a list in a script popup)

A click on the widget opens a popup listing your links (folders, files, apps, URLs) with the
shell's icons; a click on a row opens it. It is the pattern of the dock's own list views
(ItemGridView), drawn by the script with the canvas.

| File | What it shows |
|---|---|
| `widget.json` | The `"shell"` permission (file icons and `shell.open`), a string setting parsed by the script, an int with a unit. |
| `main.js` | `onClick(e)` toggles the popup (`e.closedPopup`: this press closed it), `popup.open(w, h)` sized from the rows, `drawPopup()` draws the rows (`canvas.fileIcon`, zebra stripes, `canvas.clip` for long names) and remembers their rectangles, `onPopupClick(e)` hit-tests them and calls `shell.open`, `popup.close()`. |

Script popups get clicks only (no hover or keyboard): highlight a row on a click if you need
feedback, or keep a selection in a variable and `popup.redraw()`.

Install: Settings > **Widgets** > **Add from folder…**, pick this folder, then **Add to dock**; edit
the links on Settings > **Widget settings** ("Name = target; Name = target"). Check it first with
`WinDock.exe --validate-widget examples\popup-list`. The full API is in
[docs/widgets.md](../../docs/widgets.md) ("popup", "shell", "canvas").
