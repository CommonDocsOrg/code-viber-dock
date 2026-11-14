# Example widgets

Four small, annotated ViberDock widgets to start your own from. They are not built into the dock:
install one with App settings > **Widget library** > **More ▾** > **Add from folder…** (it is copied
to `%LocalAppData%\WinDock\widgets\<folder name>`; a widget that asks for permissions runs once you
allow them), then **Add to dock**.

| Folder | Shows |
|---|---|
| [`minimal`](minimal) | The smallest useful widget: `measure()` and `draw()` for one line of text, two settings. |
| [`http-json`](http-json) | A public JSON API (Open-Meteo air quality) behind a host allow-list, `timer.setRefresh` + `tick()`, `storage`, settings with groups and units. |
| [`popup-list`](popup-list) | A list in a script popup: rows with the shell's icons, click handling, `shell.open`. |
| [`edit-text`](edit-text) | A label renamed with a click: `popup.editText`, the dock's text editor, the answer kept in `storage`. |

Check a folder before installing it: `WinDock.exe --validate-widget examples\minimal` prints the
manifest's problems (with the field and the fix) and every script's syntax errors (file:line:column),
and exits with 0 when there is no error. Each `main.js` starts with `// @ts-check` and a reference to
[docs/windock.d.ts](../docs/windock.d.ts), the API's TypeScript declarations: open the repository in
VS Code and the API completes and is type-checked as you type (a copy installed elsewhere loses the
reference; copy `windock.d.ts` next to it and change the path to `./windock.d.ts`). The full
API is in [docs/widgets.md](../docs/widgets.md); the built-in widgets in [`widgets/`](../widgets)
are larger worked examples (the analog clock's faces, the weather popup, the agenda).
