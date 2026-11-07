# Air quality (example widget: http + JSON)

Fetches a public JSON API, Open-Meteo's air quality (free, no key), for one place and shows the
European air quality index as "AQI 23" with the band's colour. A click fetches again.

| File | What it shows |
|---|---|
| `widget.json` | The **host allow-list**: `"permissions": [{ "http": ["air-quality-api.open-meteo.com"] }]`. A request to any other host throws a TypeError at the call. Settings with groups and units (`"group"`, `"unit"`). |
| `main.js` | `http.get(url, { type: "json" })` as a promise, errors caught and logged (`console.warn`: the dock menu's Tools > Widget console…), `timer.setRefresh(ms)` + `tick()` for the cadence (one wake-up per refresh, nothing in between), `storage` for the last value across restarts, `requestRedraw()` when the answer arrives. |

Install: Settings > **Widgets** > **Add from folder…**, pick this folder, then **Add to dock**; set
the place on Settings > **Widget settings**. Check it first with
`WinDock.exe --validate-widget examples\http-json`.

Data: [Open-Meteo](https://open-meteo.com) air quality API (CC BY 4.0). The full widget API is in
[docs/widgets.md](../../docs/widgets.md) ("http", "timer and redraws", "storage").
