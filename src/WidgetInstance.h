#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "DockElement.h"
#include "Renderer.h"
#include "ScriptEngine.h"
#include "WidgetCanvas.h"
#include "WidgetPackage.h"

class WidgetRuntime;

// One script widget on one dock (a `{ "kind": "widget", "id": ... }` item): its package, its own
// script runtime (QuickJS: one runtime per instance, so a widget's heap limit, watchdog and garbage
// collector are its own; ~150 KB each), its canvas, timers, settings and log. Owned by the dock
// window (DockWindow::widgets_, keyed by "<id>#<n>"), so it survives the element rebuilds that
// every item-list change causes; the dock's ScriptWidgetElement forwards layout, drawing and input.
//
// States: Ok; Error (the package is missing / unreadable, or the script threw an uncaught
// exception: an error tile with the message is shown until a reload); Suspended (a call ran past
// the manifest's timeLimitMs and the watchdog stopped it: the same tile, no more calls until a
// reload). Reload (ItemCommand 809, DockCommand 126) re-reads the package and starts over.
class WidgetInstance final : private ScriptHost {
public:
    enum class State { Ok = 0, Error = 1, Suspended = 2 };

    // The dock that shows the widget.
    class Client {
    public:
        // The widget wants to be redrawn (and re-measured). Called from inside script calls:
        // the dock must defer the redraw (post it).
        virtual void WidgetChanged(WidgetInstance& widget) = 0;

    protected:
        ~Client() = default;
    };

    struct LogLine {
        SYSTEMTIME time{};
        ScriptLogLevel level = ScriptLogLevel::Info;
        std::wstring text;
    };

    WidgetInstance(WidgetRuntime& runtime, Client& client, ID2D1Factory1* d2d, IDWriteFactory* dwrite,
                   std::wstring id, std::wstring key, std::wstring itemSettings);
    ~WidgetInstance();
    WidgetInstance(const WidgetInstance&) = delete;
    WidgetInstance& operator=(const WidgetInstance&) = delete;

    const std::wstring& Id() const { return id_; }
    const std::wstring& Key() const { return key_; }
    State GetState() const { return state_; }
    const std::wstring& ErrorText() const { return error_; }
    std::wstring Name() const;
    const Widgets::Package* Package() const { return package_.get(); }

    // Loads (again) the package and the script. The first load happens in the constructor.
    void Reload();
    // The item's `settings` object changed (raw JSON): onSettingsChanged(settings) and a redraw.
    void SetSettings(const std::wstring& itemSettings);
    // The dock's view of the widget (axis, edge, theme, scale ...), before measuring and drawing.
    void SetDock(const ScriptDockInfo& info);
    void SetMagnified(bool magnified) { dock_.magnified = magnified; }

    // The box in physical px (main along the dock, cross across it): measure(), else the
    // manifest's size hint for the axis. Cached until the widget asks for a redraw.
    ElementSize Measure(const LayoutContext& ctx);
    // Draws the content (or the error tile) into the slot's surface, at the last Measure's size.
    HRESULT Render(Renderer& renderer, Layer& layer, const LayoutContext& ctx);

    // Input on the widget's box; x / y in DIPs from its top-left (upright).
    void Click(float x, float y);
    void MiddleClick(float x, float y);
    // True if the widget consumed it (its onWheel returned true).
    bool Wheel(float x, float y, int delta, bool horizontal);
    void Hover(bool inside, float x, float y);

    // A redraw was requested since the last Render (clears the flag).
    bool TakeChanged() {
        const bool c = changed_;
        changed_ = false;
        return c;
    }
    // WidgetRuntime: timer `id` (0: the refresh tick) is due.
    void OnTimer(int id);

    // Diagnostics (QueryState, the Console).
    size_t HeapBytes() const { return engine_ ? engine_->HeapBytes() : 0; }
    uint64_t DrawCount() const { return draws_; }
    const std::deque<LogLine>& Log() const { return log_; }
    uint64_t LogTotal() const { return logTotal_; }

private:
    static constexpr size_t kMaxLogLines = 500;
    static constexpr size_t kMaxTimers = 64;

    // --- ScriptHost ---
    void Log(ScriptLogLevel level, const std::wstring& line) override;
    bool ReadFile(const std::string& path, std::string& bytes, std::wstring& error) override;
    WidgetCanvas* Canvas() override { return inCanvas_ ? &canvas_ : nullptr; }
    const ScriptDockInfo& Dock() const override { return dock_; }
    const std::string& SettingsJson() const override { return settings_; }
    bool ScheduleTimer(int id, uint32_t ms, bool repeat) override;
    void CancelTimer(int id) override;
    void RequestRedraw() override;
    void SetRefresh(uint32_t ms) override;

    // A call's outcome: a failure moves the widget to Error / Suspended (logged, timers stopped,
    // the tile shown). True if it went fine.
    bool Check(const ScriptResult& r, const char* what);
    void Fail(State state, const std::wstring& message);
    void ScheduleRefresh();
    // The upright box size in DIPs for the current dock (from the cached measure).
    void BoxDips(float& w, float& h) const;
    void DrawErrorTile(ID2D1DeviceContext* dc, float w, float h);
    static std::string EventJson(float x, float y);

    WidgetRuntime& runtime_;
    Client& client_;
    std::wstring id_;
    std::wstring key_;
    std::wstring itemSettings_;
    std::shared_ptr<const Widgets::Package> package_;
    std::unique_ptr<ScriptEngine> engine_;
    WidgetCanvas canvas_;
    bool inCanvas_ = false;
    ScriptDockInfo dock_;
    std::string settings_ = "{}";
    State state_ = State::Ok;
    std::wstring error_;
    bool changed_ = true;
    uint32_t refreshMs_ = 0;
    bool refreshSet_ = false;  // the script called timer.setRefresh (overrides the manifest)
    size_t timers_ = 0;        // script timers scheduled (not the refresh)
    // Measure cache.
    bool measured_ = false;
    DockAxis measuredAxis_ = DockAxis::Horizontal;
    float measuredScale_ = 0.0f;
    float measuredIcon_ = 0.0f;
    ElementSize size_{};  // px
    uint64_t draws_ = 0;
    std::deque<LogLine> log_;
    uint64_t logTotal_ = 0;
};
