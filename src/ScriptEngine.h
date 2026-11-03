#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class WidgetCanvas;

// The seam between the widget host (WidgetInstance: timers, canvas, settings, logs, the dock) and
// a script engine. The engine binds the host API (docs/widgets.md) to its language objects and
// calls back through ScriptHost; the host calls the widget's exported functions through
// ScriptEngine. QuickJsEngine.cpp is the implementation; another engine (Chakra) would implement
// the same two interfaces and nothing else would change.

// What the host tells the script about its dock and slot (the `dock` object).
struct ScriptDockInfo {
    std::string axis = "horizontal";  // "horizontal" | "vertical"
    std::string edge = "bottom";      // "bottom" | "top" | "left" | "right"
    std::string dockId;
    std::string widgetId;
    float width = 0, height = 0;  // the box, DIPs, upright (horizontal: main x cross)
    float main = 0, cross = 0;    // the same box along / across the dock
    float scale = 1.0f;           // physical px per DIP
    float iconSize = 48.0f;       // the dock's icon size, DIPs
    bool hovered = false;         // the pointer is over this widget
    bool magnified = false;       // the dock is magnifying (the pointer is on it)
    bool reduceMotion = false;
    bool locked = false;
    bool visible = true;          // the dock is on screen (not auto-hidden, not a hidden desktop dock)
    bool metric = true;           // config weather.units (system.units)
    // Theme colours, 0xAARRGGBB (dock.theme.*, as "#AARRGGBB").
    uint32_t text = 0, dateText = 0, accent = 0, background = 0, labelBackground = 0, labelText = 0,
             separator = 0, border = 0;
};

enum class ScriptLogLevel { Info, Warn, Error };

// What a script may ask of its host.
class ScriptHost {
public:
    virtual void Log(ScriptLogLevel level, const std::wstring& line) = 0;
    // A package file by its normalized path (Widgets::ResolvePath): modules and assets.
    virtual bool ReadFile(const std::string& path, std::string& bytes, std::wstring& error) = 0;
    // The canvas of the draw / measure in progress (null outside them).
    virtual WidgetCanvas* Canvas() = 0;
    virtual const ScriptDockInfo& Dock() const = 0;
    // The merged settings (Widgets::MergeSettings), a UTF-8 JSON object.
    virtual const std::string& SettingsJson() const = 0;
    // Timers: `id` is the engine's; the host calls ScriptEngine::FireTimer(id) when it is due.
    // False: refused (too many timers).
    virtual bool ScheduleTimer(int id, uint32_t ms, bool repeat) = 0;
    virtual void CancelTimer(int id) = 0;
    // Redraw the widget soon (coalesced; never inside the current call).
    virtual void RequestRedraw() = 0;
    // tick() every `ms` aligned to local time (0: none), replacing the manifest's refreshMs.
    virtual void SetRefresh(uint32_t ms) = 0;

    // --- permissions ---
    enum class Permission { Http, Shell, Audio, Media };
    virtual bool Permitted(Permission p) const = 0;

    // --- storage: one JSON object per widget id, values as JSON text ---
    virtual bool StorageGet(const std::string& key, std::string& json) = 0;
    virtual bool StorageSet(const std::string& key, const std::string& json, std::wstring& error) = 0;
    virtual void StorageRemove(const std::string& key) = 0;
    virtual std::vector<std::string> StorageKeys() = 0;

    // --- http: starts a request (0 + `error` when refused: no permission, a host that is not
    // allowed, too many in flight). The result arrives later as ScriptEngine::Resolve(id, ...).
    virtual int HttpStart(const std::wstring& method, const std::wstring& url, const std::wstring& headers,
                          const std::string& body, uint32_t timeoutMs, std::wstring& error) = 0;

    // --- shell (permission "shell") ---
    virtual bool ShellOpen(const std::wstring& target, const std::wstring& args, std::wstring& error) = 0;
    // The folder's entries as a JSON array of { name, path, folder, size, modified }.
    virtual bool ShellList(const std::wstring& folder, std::string& json, std::wstring& error) = 0;
    // A key combination from a fixed allow-list ("win", "win+s" ...), sent with SendInput.
    virtual bool SendKeys(const std::wstring& name, std::wstring& error) = 0;

    // --- audio (permission "audio"): the default devices' volume / mute, the outputs ---
    // The state as a JSON object (docs/widgets.md, audio.get()).
    virtual bool AudioGet(std::string& json, std::wstring& error) = 0;
    // what: "volume" (number 0 - 1), "muted" / "micMuted" (number 0 / 1), "device" (text: an id).
    virtual bool AudioSet(const std::string& what, double number, const std::wstring& text, std::wstring& error) = 0;

    // --- media (permission "media"): the current media session ---
    virtual bool MediaGet(std::string& json, std::wstring& error) = 0;
    // "play", "pause", "toggle", "next", "previous".
    virtual bool MediaCommand(const std::string& command, std::wstring& error) = 0;
    // The current art as an image token for canvas.image ("" none).
    virtual bool MediaArt(std::string& token, std::wstring& error) = 0;

    // --- popups (anchored on the widget's slot, beside the dock) ---
    // A popup the script draws (drawPopup) of w x h DIPs.
    virtual bool PopupOpen(float w, float h, std::wstring& error) = 0;
    virtual void PopupClose() = 0;
    virtual bool PopupShown() const = 0;
    virtual void PopupRedraw() = 0;
    // The dock's own folder popup for a folder / its window previews for an app (exe path or
    // AppUserModelID) that the dock shows.
    virtual bool PopupFolder(const std::wstring& path, std::wstring& error) = 0;
    virtual bool PopupPreviews(const std::wstring& app, std::wstring& error) = 0;
    // The dock's own month calendar (the clock's CalendarPopup), anchored on the widget.
    virtual bool PopupCalendar(std::wstring& error) = 0;

    // --- the widget's own package (no permission: modules and images come from it anyway) ---
    // Its files ("main.js", "faces/classic.json" ...), sorted.
    virtual std::vector<std::string> PackageFiles() = 0;

protected:
    ~ScriptHost() = default;
};

enum class ScriptStatus {
    Ok,
    Missing,      // the function is not exported (nothing ran)
    Threw,        // an uncaught exception (error: message and stack)
    TimedOut,     // the watchdog interrupted it (it ran longer than the time limit)
    OutOfMemory,  // the heap limit was reached
};

struct ScriptResult {
    ScriptStatus status = ScriptStatus::Ok;
    std::string json;    // the return value as JSON (undefined: empty)
    std::wstring error;  // Threw / TimedOut / OutOfMemory: what happened, with the stack
    bool Failed() const { return status != ScriptStatus::Ok && status != ScriptStatus::Missing; }
};

struct ScriptLimits {
    uint32_t timeLimitMs = 50;     // per call into the script (and the jobs it queued)
    size_t memoryLimit = 32u << 20;
    size_t stackLimit = 4u << 20;  // JS_SetMaxStackSize: well inside the exe's 8 MB stack
};

class ScriptEngine {
public:
    // The widget's exports the host calls (docs/widgets.md, "Exports").
    enum class Fn {
        Draw,
        Measure,
        Tick,
        OnClick,
        OnMiddleClick,
        OnWheel,
        OnHover,
        OnSettingsChanged,
        OnPress,
        OnDrop,
        DrawPopup,      // (canvas, dock): the script popup's content
        OnPopupClick,   // (event, dock)
        OnPopupClose,   // (event, dock): the popup closed (by itself or popup.close())
        OnVisibilityChange,  // ({ visible }, dock): the dock was hidden / shown
        Count
    };

    virtual ~ScriptEngine() = default;
    // Evaluates the main module (`mainPath`, normalized) with its imports.
    virtual ScriptResult Load(const std::string& mainPath) = 0;
    virtual bool Has(Fn fn) const = 0;
    // Calls an export. Draw / Measure get (canvas, dock); OnSettingsChanged (settings, dock); the
    // others (event, dock) with the event parsed from `eventJson`.
    virtual ScriptResult Call(Fn fn, const std::string& eventJson) = 0;
    virtual ScriptResult FireTimer(int id) = 0;
    // An http request finished: resolves (ok, `json` = the response object as JSON) or rejects
    // (`json` = the error message as a JSON string) its promise, then runs the jobs that follow.
    virtual ScriptResult Resolve(int id, bool ok, const std::string& json) = 0;
    // The settings changed (re-read through ScriptHost::SettingsJson before the next call).
    virtual void SettingsChanged() = 0;
    // Bytes allocated by the script heap (JS_ComputeMemoryUsage malloc_size).
    virtual size_t HeapBytes() = 0;
    // A provider changed ("audio", "media"): its onChange listeners run with its new state.
    virtual ScriptResult Emit(const std::string& channel) = 0;
};

std::unique_ptr<ScriptEngine> CreateQuickJsEngine(ScriptHost& host, const ScriptLimits& limits);
