#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Config.h"

// Current conditions and a 7-day forecast from Open-Meteo (https://open-meteo.com, no API key;
// data under CC BY 4.0), for the dock's weather widget and its popup.
//
// Scheduling: one one-shot SetTimer (kTimerId) for the next refresh, refreshMinutes after the
// last successful fetch; after a failure 1, 2, 4 ... minutes (capped at refreshMinutes). Nothing
// polls and no thread exists between fetches: a fetch is a CreateThread worker that does one
// synchronous WinHTTP GET (automatic proxy, 5 s timeouts, TLS, body capped at 1 MB) and posts the
// result to the window as kResultMessage (wParam = generation, lParam = Response*, owned by the
// receiver; freed by the worker if the post fails). The worker touches nothing else: no WinRT, no
// dock state. Results of an older generation (the place or units changed, or the service stopped
// meanwhile) are ignored and freed. The JSON is parsed on the UI thread (Windows.Data.Json).
//
// The last good response is cached in %LocalAppData%\WinDock\weather-cache-<hash>.json (one file
// per place and units, round 5) and shown (as Stale) at start until the first fetch of the
// session completes.
//
// Round 5 phase A: one service per place (DockManager's registry: latitude, longitude, units,
// refreshMinutes), all on the same owner window. Each has its own refresh timer id (SetTimerId)
// and the generations of every service come from one process-wide counter, so a result message
// belongs to exactly one service (HandleMessage answers false for another's result).
class WeatherService {
public:
    // QueryState 27: 0 none (off / unconfigured / nothing yet), 1 stale, 2 fresh, 3 error.
    enum class Status { None = 0, Stale = 1, Fresh = 2, Error = 3 };
    static constexpr UINT kResultMessage = WM_APP + 20;   // a forecast fetch finished
    static constexpr UINT kGeocodeMessage = WM_APP + 23;  // a Geocode search finished
    static constexpr UINT_PTR kTimerId = 20;              // the next refresh (one-shot); the default id
    static constexpr size_t kMaxBody = 1u << 20;

    // The registry's key: the same place, units and interval share one service.
    static bool SamePlace(const WeatherConfig& a, const WeatherConfig& b) {
        return a.latitude == b.latitude && a.longitude == b.longitude && a.units == b.units &&
               a.refreshMinutes == b.refreshMinutes;
    }
    // Forecast requests started by every service of the process (QueryState 214 / 221).
    static uint64_t TotalFetches();

    // A finished HTTP GET, posted to the window (the receiver owns it).
    struct Response {
        DWORD error = 0;   // Win32 / WinHTTP error (0: a response arrived)
        DWORD status = 0;  // HTTP status
        std::string body;  // UTF-8
    };

    struct Hour {
        int64_t key = 0;       // minutes since 1970-01-01 00:00, in the location's local time
        int hour = 0;          // 0 - 23
        double temp = 0.0;     // NaN when missing
        int code = -1;         // WMO weather code (-1: missing)
        double precip = 0.0;   // precipitation probability, % (NaN when missing)
        bool isDay = true;
    };
    struct Day {
        int64_t day = 0;       // days since 1970-01-01 (location's calendar)
        int weekday = 0;       // 0 = Monday ... 6 = Sunday
        int code = -1;
        double tmin = 0.0, tmax = 0.0;
    };
    struct Forecast {
        bool valid = false;
        double temp = 0.0;       // current temperature, configured units
        int code = -1;           // current WMO code
        bool isDay = true;
        double wind = 0.0;       // km/h or mph (NaN when missing)
        double humidity = 0.0;   // % (NaN when missing)
        int utcOffset = 0;       // seconds, the location's offset from UTC when fetched
        std::vector<Hour> hourly;  // the whole forecast (7 days)
        std::vector<Day> daily;
        int64_t fetchedAt = 0;   // Unix seconds
    };
    // A geocoding result.
    struct Place {
        std::wstring name, admin1, country;
        double latitude = 0.0, longitude = 0.0;
        // "Name, Admin1, Country" (empty parts skipped).
        std::wstring Label() const;
    };

    WeatherService() = default;
    WeatherService(const WeatherService&) = delete;
    WeatherService& operator=(const WeatherService&) = delete;
    ~WeatherService() { Stop(); }

    // Applies the config (the window receives kResultMessage and the timer). Starts, stops or
    // restarts as needed: a new place or units drop the data, load the cache and fetch now; turning
    // the widget on fetches now unless the data is from within the refresh interval (then the
    // next fetch keeps its time); a new interval reschedules. `shown`: a shown dock has a weather
    // item or a script widget uses the provider.
    void Configure(const WeatherConfig& cfg, bool shown, HWND hwnd);
    // Stops the timer and ignores a fetch in flight (keeps the data).
    void Stop();
    // Fetches now (unless one is in flight). No-op while off / unconfigured.
    void RefreshNow();
    // kResultMessage of this service's fetch and the WM_TIMER of its timer id; true if handled
    // (another service's result: false, the caller passes it on or frees it).
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    // The owner window's timer id of this service's refresh (before the first Configure).
    void SetTimerId(UINT_PTR id) { timerId_ = id; }
    UINT_PTR TimerId() const { return timerId_; }
    // Called after the data or the status changed (a fetch started, finished or failed).
    void SetChangedCallback(std::function<void()> cb) { changed_ = std::move(cb); }

    // Shown on the dock and has a location.
    bool Active() const { return hwnd_ && shown_ && cfg_.Configured(); }
    // The last Configure was told the widget is shown (the dock has a weather item).
    bool Shown() const { return shown_; }
    Status State() const { return Active() ? status_ : Status::None; }
    const Forecast& Data() const { return data_; }
    const WeatherConfig& Settings() const { return cfg_; }
    bool Busy() const { return inFlight_ && inFlightGen_ == generation_; }
    const std::wstring& LastError() const { return error_; }
    // Forecast requests started since the dock started (QueryState 214).
    uint64_t FetchCount() const { return fetches_; }
    // Seconds since the data was fetched (-1: no data).
    int64_t Age() const;
    // "Updated 10:15", "Updated 10:15 (stale)", "Updating…", "Not configured", "Error: …".
    std::wstring StatusText() const;
    // Up to `n` hours from the current hour (the location's local time), and up to `n` days from
    // today.
    std::vector<Hour> HourlyWindow(size_t n) const;
    std::vector<Day> DailyWindow(size_t n) const;

    // Starts a geocoding search (Open-Meteo, 10 results, English names); the result is posted to
    // `hwnd` as `msg` (wParam = `tag`, lParam = Response*, owned by the receiver). False if the
    // worker could not be started.
    static bool Geocode(const std::wstring& query, HWND hwnd, UINT msg, WPARAM tag);
    // Parses a geocoding response. False with `error` set on failure; true (maybe no places) else.
    static bool ParsePlaces(const Response& response, std::vector<Place>& places, std::wstring& error);
    // "Error text" of a failed response ("timed out", "HTTP 400: …").
    static std::wstring ErrorText(const Response& response);
    // Locale short time of a Unix time ("10:15", "10:15 AM").
    static std::wstring FormatTime(int64_t unixSeconds);
    // %LocalAppData%\WinDock\weather-cache-<8 hex of an FNV-1a hash of "lat,lon,units">.json
    static std::wstring CachePath(const WeatherConfig& cfg);
    // %LocalAppData%\WinDock\weather-cache.json (before round 5: one file; read as a fallback)
    static std::wstring LegacyCachePath();

private:
    void Fetch();
    void Schedule(UINT ms);
    void CancelTimer();
    void OnResponse(WPARAM generation, Response* response);
    void OnFailure(std::wstring error);
    bool LoadCache();
    bool LoadCacheFile(const std::wstring& path);
    void SaveCache(const std::string& body) const;
    std::wstring ForecastPath() const;
    void Notify() {
        if (changed_) changed_();
    }

    HWND hwnd_ = nullptr;
    WeatherConfig cfg_;
    bool shown_ = false;  // the dock has a weather item
    bool configured_ = false;  // Configure ran once
    Forecast data_;
    Status status_ = Status::None;
    std::wstring error_;       // the last failure (cleared by a success)
    int failures_ = 0;         // consecutive failures (backoff)
    UINT_PTR generation_ = NextGeneration();  // bumped whenever a result in flight must be ignored
    bool inFlight_ = false;
    UINT_PTR inFlightGen_ = 0;
    bool timerPending_ = false;
    UINT_PTR timerId_ = kTimerId;
    static UINT_PTR NextGeneration();  // process-wide: a result names one service's fetch
    uint64_t fetches_ = 0;
    std::function<void()> changed_;
};
