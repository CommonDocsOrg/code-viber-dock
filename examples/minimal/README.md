# Minimal (example widget)

The smallest useful WinDock widget: one line of text in the dock's own colours.

| File | What it shows |
|---|---|
| `widget.json` | The manifest: a name, the box size per dock axis, two settings (a string, a bool). |
| `main.js` | `measure()` sizes the box from `canvas.measureText`; `draw()` draws the text; `onSettingsChanged()` redraws. |

Install it: Settings > **Widgets** > **Add from folder…**, pick this folder (it is copied to
`%LocalAppData%\WinDock\widgets\minimal`), then **Add to dock**. Its settings are on Settings >
**Widget settings**. To edit the installed copy and see every save at once, add `"watch": true`
to its widget.json.

Check the manifest before installing: `WinDock.exe --validate-widget examples\minimal` (exit code 0:
fine). The full API is in [docs/widgets.md](../../docs/widgets.md).
