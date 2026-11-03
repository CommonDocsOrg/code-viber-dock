#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "DockElement.h"
#include "HttpClient.h"
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
        // The widget asked for a popup to open / close / redraw (TakePopupRequest). Called from
        // inside script calls: the dock defers it.
        virtual void WidgetPopupRequest(WidgetInstance& widget) = 0;
        // The widget's script popup is on screen.
        virtual bool WidgetPopupShown(const WidgetInstance& widget) const = 0;

    protected:
        ~Client() = default;
    };

    // What the widget asked of its popup (popup.open / close / redraw / folder / previews).
    struct PopupRequest {
        enum class Kind { None, Open, Close, Redraw, Folder, Previews, Calendar };
        Kind kind = Kind::None;
        float width = 0, height = 0;  // Open: DIPs
        std::wstring arg;             // Folder: the path; Previews: the app
        std::wstring view;            // Folder: "auto" / "grid" / "list" ("" auto)
    };

    struct LogLine {
        SYSTEMTIME time{};
        ScriptLogLevel level = ScriptLogLevel::Info;
        std::wstring text;
    };

    // `dock`: what the script sees as `dock` while its module loads (SetDock updates it later).
    WidgetInstance(WidgetRuntime& runtime, Client& client, ID2D1Factory1* d2d, IDWriteFactory* dwrite,
                   std::wstring id, std::wstring key, std::wstring itemSettings, const ScriptDockInfo& dock);
    ~WidgetInstance();
    WidgetInstance(const WidgetInstance&) = delete;
    WidgetInstance& operator=(const WidgetInstance&) = delete;

    const std::wstring& Id() const { return id_; }
    const std::wstring& Key() const { return key_; }
    State GetState() const { return state_; }
    const std::wstring& ErrorText() const { return error_; }
    std::wstring Name() const;
    const Widgets::Package* Package() const { return package_.get(); }
    const ScriptDockInfo& DockInfo() const { return dock_; }

    // Loads (again) the package and the script. The first load happens in the constructor.
    void Reload();
    // The item's `settings` object changed (raw JSON): onSettingsChanged(settings) and a redraw.
    void SetSettings(const std::wstring& itemSettings);
    // The dock's view of the widget (axis, edge, theme, scale ...), before measuring and drawing.
    void SetDock(const ScriptDockInfo& info);
    void SetMagnified(bool magnified) { dock_.magnified = magnified; }
    // The dock went off / came back on screen (auto-hide, a desktop dock): dock.visible and
    // onVisibilityChange({ visible }) when it changed.
    void SetVisible(bool visible);
    // The system time / time zone changed or the machine resumed: aligned refreshes are re-aligned
    // and a refreshing widget ticks at once.
    void TimeChanged();

    // The box in physical px (main along the dock, cross across it): measure(), else the
    // manifest's size hint for the axis. Cached until the widget asks for a redraw.
    ElementSize Measure(const LayoutContext& ctx);
    // Draws the content (or the error tile) into the slot's surface, at the last Measure's size.
    HRESULT Render(Renderer& renderer, Layer& layer, const LayoutContext& ctx);

    // Input on the widget's box; x / y in DIPs from its top-left (upright).
    void Press(float x, float y);
    // `closedPopup`: the press closed this widget's popup (a click on the widget toggles it).
    void Click(float x, float y, bool closedPopup = false);
    void MiddleClick(float x, float y);
    // True if the widget consumed it (its onWheel returned true).
    bool Wheel(float x, float y, int delta, bool horizontal);
    void Hover(bool inside, float x, float y);
    // Files dropped on the widget: it exports onDrop and has the "shell" permission.
    bool AcceptsDrops() const;
    void Drop(const std::vector<std::wstring>& files, float x, float y);

    // --- its popup (hosted by the dock) ---
    bool PopupShownNow() const { return client_.WidgetPopupShown(*this); }
    PopupRequest TakePopupRequest() {
        PopupRequest r = popupRequest_;
        popupRequest_ = PopupRequest{};
        return r;
    }
    // drawPopup(canvas, dock) into the popup's surface (w x h DIPs at `scale`).
    void RenderPopup(ID2D1DeviceContext* dc, float w, float h, float scale);
    void PopupClick(float x, float y, int button);
    // The popup closed (by itself, or the dock closed it): onPopupClose.
    void PopupClosed();

    // WidgetRuntime: a provider this widget used changed ("audio", "media").
    void ProviderChanged(const char* channel);
    // WidgetRuntime: an http request of generation `generation` finished.
    void OnHttpResult(uint64_t generation, int id, const Http::Result& result);
    // A folder widget with `watch`: its id and canonical folder (false: not watched).
    bool WatchFolder(std::wstring& id, std::wstring& folder) const;
    // A line of the host's own in the widget's log (reloads, file changes).
    void Note(const std::wstring& line) { Log(ScriptLogLevel::Info, line); }

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
    const std::deque<LogLine>& LogLines() const { return log_; }
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
    bool Permitted(Permission p) const override;
    bool StorageGet(const std::string& key, std::string& json) override;
    bool StorageSet(const std::string& key, const std::string& json, std::wstring& error) override;
    void StorageRemove(const std::string& key) override;
    std::vector<std::string> StorageKeys() override;
    int HttpStart(const std::wstring& method, const std::wstring& url, const std::wstring& headers,
                  const std::string& body, uint32_t timeoutMs, std::wstring& error) override;
    bool ShellOpen(const std::wstring& target, const std::wstring& args, std::wstring& error) override;
    bool ShellList(const std::wstring& folder, std::string& json, std::wstring& error) override;
    bool SendKeys(const std::wstring& name, std::wstring& error) override;
    bool AudioGet(std::string& json, std::wstring& error) override;
    bool AudioSet(const std::string& what, double number, const std::wstring& text, std::wstring& error) override;
    bool MediaGet(std::string& json, std::wstring& error) override;
    bool MediaCommand(const std::string& command, std::wstring& error) override;
    bool MediaArt(std::string& token, std::wstring& error) override;
    bool PopupOpen(float w, float h, std::wstring& error) override;
    void PopupClose() override;
    bool PopupShown() const override { return client_.WidgetPopupShown(*this); }
    void PopupRedraw() override;
    bool PopupFolder(const std::wstring& path, const std::wstring& view, std::wstring& error) override;
    bool PopupPreviews(const std::wstring& app, std::wstring& error) override;
    bool PopupCalendar(std::wstring& error) override;
    std::vector<std::string> PackageFiles() override;
    void RequestPopup(PopupRequest r);
    bool HostAllowed(const std::wstring& host) const;

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
    uint64_t generation_ = 0;  // +1 per load: http results of an older load are dropped
    int nextRequest_ = 1;
    int requests_ = 0;         // http requests in flight (this generation)
    PopupRequest popupRequest_;
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
