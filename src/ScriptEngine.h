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
    enum class Fn { Draw, Measure, Tick, OnClick, OnMiddleClick, OnWheel, OnHover, OnSettingsChanged, Count };

    virtual ~ScriptEngine() = default;
    // Evaluates the main module (`mainPath`, normalized) with its imports.
    virtual ScriptResult Load(const std::string& mainPath) = 0;
    virtual bool Has(Fn fn) const = 0;
    // Calls an export. Draw / Measure get (canvas, dock); OnSettingsChanged (settings, dock); the
    // others (event, dock) with the event parsed from `eventJson`.
    virtual ScriptResult Call(Fn fn, const std::string& eventJson) = 0;
    virtual ScriptResult FireTimer(int id) = 0;
    // The settings changed (re-read through ScriptHost::SettingsJson before the next call).
    virtual void SettingsChanged() = 0;
    // Bytes allocated by the script heap (JS_ComputeMemoryUsage malloc_size).
    virtual size_t HeapBytes() = 0;
};

std::unique_ptr<ScriptEngine> CreateQuickJsEngine(ScriptHost& host, const ScriptLimits& limits);
