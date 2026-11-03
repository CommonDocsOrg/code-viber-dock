# Example widgets

Three small, annotated WinDock widgets to start your own from. They are not built into the dock:
install one with Settings > **Widgets** > **Add from folder…** (it is copied to
`%LocalAppData%\WinDock\widgets\<folder name>`), then **Add to dock**.

| Folder | Shows |
|---|---|
| [`minimal`](minimal) | The smallest useful widget: `measure()` and `draw()` for one line of text, two settings. |
| [`http-json`](http-json) | A public JSON API (Open-Meteo air quality) behind a host allow-list, `timer.setRefresh` + `tick()`, `storage`, settings with groups and units. |
| [`popup-list`](popup-list) | A list in a script popup: rows with the shell's icons, click handling, `shell.open`. |

Check a folder before installing it: `WinDock.exe --validate-widget examples\minimal` prints the
manifest's problems (with the field and the fix) and exits with 0 when there is no error. The full
API is in [docs/widgets.md](../docs/widgets.md); the built-in widgets in [`widgets/`](../widgets)
are larger worked examples (the analog clock's faces, the weather popup, the agenda).
