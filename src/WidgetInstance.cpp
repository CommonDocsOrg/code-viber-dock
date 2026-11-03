#include "WidgetInstance.h"

#include <shellapi.h>
#include <winhttp.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>

#include "JsonUtil.h"
#include "WidgetRuntime.h"

namespace {

namespace json = winrt::Windows::Data::Json;

constexpr char kArtToken[] = "media:art/";

const char* FnName(ScriptEngine::Fn fn) {
    switch (fn) {
        case ScriptEngine::Fn::Draw: return "draw()";
        case ScriptEngine::Fn::Measure: return "measure()";
        case ScriptEngine::Fn::Tick: return "tick()";
        case ScriptEngine::Fn::OnClick: return "onClick()";
        case ScriptEngine::Fn::OnMiddleClick: return "onMiddleClick()";
        case ScriptEngine::Fn::OnWheel: return "onWheel()";
        case ScriptEngine::Fn::OnHover: return "onHover()";
        case ScriptEngine::Fn::OnSettingsChanged: return "onSettingsChanged()";
        case ScriptEngine::Fn::OnPress: return "onPress()";
        case ScriptEngine::Fn::OnDrop: return "onDrop()";
        case ScriptEngine::Fn::DrawPopup: return "drawPopup()";
        case ScriptEngine::Fn::OnPopupClick: return "onPopupClick()";
        case ScriptEngine::Fn::OnPopupClose: return "onPopupClose()";
        case ScriptEngine::Fn::OnVisibilityChange: return "onVisibilityChange()";
        default: return "a call";
    }
}

}  // namespace

WidgetInstance::WidgetInstance(WidgetRuntime& runtime, Client& client, ID2D1Factory1* d2d, IDWriteFactory* dwrite,
                               std::wstring id, std::wstring key, std::wstring itemSettings,
                               const ScriptDockInfo& dock)
    : runtime_(runtime),
      client_(client),
      id_(std::move(id)),
      key_(std::move(key)),
      itemSettings_(std::move(itemSettings)),
      canvas_(d2d, dwrite, [this](const std::string& path, std::string& bytes, std::wstring& error) {
          return ReadFile(path, bytes, error);
      }),
      dock_(dock) {
    dock_.widgetId = JsonUtil::WideToUtf8(id_);
    runtime_.Add(this);
    Reload();
}

WidgetInstance::~WidgetInstance() {
    runtime_.CancelAll(this);
    runtime_.Remove(this);
    engine_.reset();  // its finalizers may still log
}

std::wstring WidgetInstance::Name() const {
    return package_ && !package_->manifest.name.empty() ? package_->manifest.name : id_;
}

void WidgetInstance::Reload() {
    runtime_.CancelAll(this);
    runtime_.ReleaseProviders(this);
    if (PopupShown()) RequestPopup(PopupRequest{PopupRequest::Kind::Close});
    engine_.reset();
    canvas_.ReleaseImages();
    state_ = State::Ok;
    error_.clear();
    refreshMs_ = 0;
    refreshSet_ = false;
    measured_ = false;
    changed_ = true;
    ++generation_;  // results of the old script's requests are dropped
    requests_ = 0;
    struct Synced {  // whatever happens below, the watcher and the Console follow
        WidgetRuntime& rt;
        ~Synced() {
            rt.SyncWatch();
            rt.NotifyChanged();
        }
    } synced{runtime_};

    std::wstring error;
    package_ = Widgets::Load(id_, error);
    if (!package_) {
        Fail(State::Error, error);
        return;
    }
    const Widgets::Manifest& m = package_->manifest;
    for (const std::wstring& w : m.warnings) Log(ScriptLogLevel::Warn, L"widget.json: " + w);
    settings_ = Widgets::MergeSettings(m, itemSettings_);
    Log(ScriptLogLevel::Info, L"loading " + m.name + (m.version.empty() ? L"" : L" " + m.version) +
                                  (package_->source == Widgets::Package::Source::BuiltIn ? L" (built-in)"
                                                                                       : L" from " + package_->folder));
    ScriptLimits limits;
    limits.timeLimitMs = static_cast<uint32_t>(m.timeLimitMs);
    limits.memoryLimit = static_cast<size_t>(m.memoryLimitMb) << 20;
    engine_ = CreateQuickJsEngine(*this, limits);
    if (!engine_) {
        Fail(State::Error, L"the script engine could not be started");
        return;
    }
    if (!Check(engine_->Load(JsonUtil::WideToUtf8(m.main)), "loading the module")) return;
    if (!engine_->Has(ScriptEngine::Fn::Draw)) {
        Fail(State::Error, L"main module does not export draw(canvas, dock)");
        return;
    }
    if (!refreshSet_) refreshMs_ = static_cast<uint32_t>(m.refreshMs);
    ScheduleRefresh();
    client_.WidgetChanged(*this);
}

void WidgetInstance::SetSettings(const std::wstring& itemSettings) {
    if (itemSettings == itemSettings_) return;
    itemSettings_ = itemSettings;
    if (!package_) return;
    const std::string merged = Widgets::MergeSettings(package_->manifest, itemSettings_);
    if (merged == settings_) return;
    settings_ = merged;
    measured_ = false;
    changed_ = true;
    if (state_ == State::Ok && engine_) {
        engine_->SettingsChanged();
        Check(engine_->Call(ScriptEngine::Fn::OnSettingsChanged, {}), FnName(ScriptEngine::Fn::OnSettingsChanged));
    }
    client_.WidgetChanged(*this);
}

void WidgetInstance::SetVisible(bool visible) {
    if (dock_.visible == visible) return;
    dock_.visible = visible;
    if (state_ != State::Ok || !engine_) return;
    Check(engine_->Call(ScriptEngine::Fn::OnVisibilityChange,
                        std::string("{\"visible\":") + (visible ? "true" : "false") + "}"),
          FnName(ScriptEngine::Fn::OnVisibilityChange));
}

void WidgetInstance::TimeChanged() {
    if (state_ != State::Ok || !engine_ || refreshMs_ == 0) return;
    ScheduleRefresh();  // aligned to the new local time
    OnTimer(0);         // and up to date now
}

void WidgetInstance::SetDock(const ScriptDockInfo& info) {
    const bool hovered = dock_.hovered, magnified = dock_.magnified, visible = dock_.visible;
    const float w = dock_.width, h = dock_.height, mn = dock_.main, cr = dock_.cross;
    dock_ = info;
    dock_.widgetId = JsonUtil::WideToUtf8(id_);
    dock_.hovered = hovered;
    dock_.magnified = magnified;
    dock_.visible = visible;
    dock_.width = w;
    dock_.height = h;
    dock_.main = mn;
    dock_.cross = cr;
}

void WidgetInstance::BoxDips(float& w, float& h) const {
    const float s = dock_.scale > 0.0f ? dock_.scale : 1.0f;
    const bool vertical = measuredAxis_ == DockAxis::Vertical;
    w = (vertical ? size_.cross : size_.main) / s;
    h = (vertical ? size_.main : size_.cross) / s;
}

ElementSize WidgetInstance::Measure(const LayoutContext& ctx) {
    if (measured_ && measuredAxis_ == ctx.axis && measuredScale_ == ctx.scale && measuredIcon_ == ctx.iconPx) {
        return size_;
    }
    const bool vertical = ctx.axis == DockAxis::Vertical;
    const float s = ctx.scale > 0.0f ? ctx.scale : 1.0f;
    Widgets::SizeHint hint = vertical ? Widgets::SizeHint{48.0f, 48.0f} : Widgets::SizeHint{64.0f, 40.0f};
    if (package_) hint = vertical ? package_->manifest.vertical : package_->manifest.horizontal;
    float mainDip = hint.main, crossDip = hint.cross;
    dock_.axis = vertical ? "vertical" : "horizontal";
    dock_.scale = s;
    dock_.iconSize = ctx.iconPx / s;
    dock_.main = mainDip;
    dock_.cross = crossDip;
    dock_.width = vertical ? crossDip : mainDip;
    dock_.height = vertical ? mainDip : crossDip;
    if (state_ == State::Ok && engine_ && engine_->Has(ScriptEngine::Fn::Measure)) {
        canvas_.Begin(nullptr, s, dock_.width, dock_.height);
        inCanvas_ = true;
        const ScriptResult r = engine_->Call(ScriptEngine::Fn::Measure, {});
        inCanvas_ = false;
        canvas_.End();
        if (Check(r, FnName(ScriptEngine::Fn::Measure)) && !r.json.empty()) {
            try {
                json::JsonObject o{nullptr};
                if (json::JsonObject::TryParse(JsonUtil::Utf8ToWide(r.json), o) && o) {
                    double m = mainDip, c = crossDip;
                    JsonUtil::ReadNumber(o, L"main", m);
                    JsonUtil::ReadNumber(o, L"cross", c);
                    if (std::isfinite(m) && std::isfinite(c)) {
                        mainDip = static_cast<float>(std::clamp(m, 0.0, 1024.0));
                        crossDip = static_cast<float>(std::clamp(c, 0.0, 512.0));
                    }
                }
            } catch (...) {
            }
        }
    }
    // Never thinner than the error tile can show something in.
    if (state_ != State::Ok) {
        mainDip = std::max(mainDip, 24.0f);
        crossDip = std::max(crossDip, 24.0f);
    }
    size_ = ElementSize{std::ceil(mainDip * s), std::ceil(crossDip * s)};
    measured_ = true;
    measuredAxis_ = ctx.axis;
    measuredScale_ = ctx.scale;
    measuredIcon_ = ctx.iconPx;
    dock_.main = size_.main / s;
    dock_.cross = size_.cross / s;
    dock_.width = vertical ? dock_.cross : dock_.main;
    dock_.height = vertical ? dock_.main : dock_.cross;
    return size_;
}

HRESULT WidgetInstance::Render(Renderer& renderer, Layer& layer, const LayoutContext& ctx) {
    if (!measured_) Measure(ctx);
    changed_ = false;
    const bool vertical = measuredAxis_ == DockAxis::Vertical;
    const UINT wPx = static_cast<UINT>(vertical ? size_.cross : size_.main);
    const UINT hPx = static_cast<UINT>(vertical ? size_.main : size_.cross);
    if (wPx == 0 || hPx == 0) return renderer.DrawLayer(layer, 0, 0, {});
    float w = 0, h = 0;
    BoxDips(w, h);
    return renderer.DrawLayer(layer, wPx, hPx, [&](ID2D1DeviceContext* dc) {
        if (state_ == State::Ok && engine_) {
            ++draws_;
            canvas_.Begin(dc, dock_.scale, w, h);
            inCanvas_ = true;
            const ScriptResult r = engine_->Call(ScriptEngine::Fn::Draw, {});
            inCanvas_ = false;
            canvas_.End();
            if (Check(r, FnName(ScriptEngine::Fn::Draw))) return;
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));  // whatever it drew before it failed
            changed_ = false;  // the tile is drawn right here
        }
        DrawErrorTile(dc, w, h);
    });
}

void WidgetInstance::DrawErrorTile(ID2D1DeviceContext* dc, float w, float h) {
    canvas_.Begin(dc, dock_.scale, w, h);
    CanvasPaint fill;
    fill.color = CanvasColorFromArgb(0xD8471C1C);
    CanvasPaint edge;
    edge.color = CanvasColorFromArgb(0xC0FF7A6E);
    CanvasPaint ink;
    ink.color = CanvasColorFromArgb(0xFFFFFFFF);
    CanvasStroke stroke;
    stroke.width = 1.0f;
    const float r = std::min(6.0f, std::min(w, h) / 4.0f);
    canvas_.FillRoundRect(0.5f, 0.5f, w - 1.0f, h - 1.0f, r, fill);
    canvas_.StrokeRoundRect(0.5f, 0.5f, w - 1.0f, h - 1.0f, r, edge, stroke);
    CanvasTextStyle title;
    title.size = 11.0f;
    title.weight = 700;
    title.align = 1;
    title.valign = 0;
    const std::wstring head = state_ == State::Suspended ? L"Stopped" : L"Error";
    CanvasTextMetrics tm = canvas_.MeasureText(head, title);
    if (tm.width > w - 4.0f && tm.width > 0.0f) {  // a narrow tile: a smaller title
        title.size = std::max(7.0f, title.size * (w - 4.0f) / tm.width);
        tm = canvas_.MeasureText(head, title);
    }
    const bool room = h >= tm.height + 16.0f && w >= 40.0f;
    if (!room) {
        title.valign = 1;
        canvas_.Text(w >= tm.width + 4.0f ? head : L"!", w / 2.0f, h / 2.0f, title, ink);
        canvas_.End();
        return;
    }
    canvas_.Text(head, w / 2.0f, 3.0f, title, ink);
    // The message under it (the first line), wrapped into what is left.
    std::wstring msg = error_;
    if (const size_t nl = msg.find(L'\n'); nl != std::wstring::npos) msg.resize(nl);
    CanvasTextStyle body;
    body.size = 8.5f;
    body.maxWidth = w - 6.0f;
    body.align = 1;
    canvas_.ClipRect(3.0f, 3.0f + tm.height, w - 6.0f, h - tm.height - 6.0f);
    canvas_.Text(msg, w / 2.0f, 3.0f + tm.height, body, ink);
    canvas_.End();
}

std::string WidgetInstance::EventJson(float x, float y) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "{\"x\":%.2f,\"y\":%.2f", std::isfinite(x) ? x : 0.0f, std::isfinite(y) ? y : 0.0f);
    return buf;
}

void WidgetInstance::Click(float x, float y, bool closedPopup) {
    if (state_ != State::Ok || !engine_) return;
    Check(engine_->Call(ScriptEngine::Fn::OnClick, EventJson(x, y) + ",\"button\":\"left\",\"closedPopup\":" +
                                                      (closedPopup ? "true" : "false") + "}"),
          FnName(ScriptEngine::Fn::OnClick));
}

void WidgetInstance::MiddleClick(float x, float y) {
    if (state_ != State::Ok || !engine_) return;
    Check(engine_->Call(ScriptEngine::Fn::OnMiddleClick, EventJson(x, y) + ",\"button\":\"middle\"}"),
          FnName(ScriptEngine::Fn::OnMiddleClick));
}

bool WidgetInstance::Wheel(float x, float y, int delta, bool horizontal) {
    if (state_ != State::Ok || !engine_ || !engine_->Has(ScriptEngine::Fn::OnWheel)) return false;
    const std::string e = EventJson(x, y) + ",\"delta\":" + std::to_string(delta) +
                          ",\"notches\":" + std::to_string(static_cast<double>(delta) / WHEEL_DELTA) +
                          ",\"horizontal\":" + (horizontal ? "true" : "false") + "}";
    const ScriptResult r = engine_->Call(ScriptEngine::Fn::OnWheel, e);
    return Check(r, FnName(ScriptEngine::Fn::OnWheel)) && r.json == "true";
}

void WidgetInstance::Hover(bool inside, float x, float y) {
    dock_.hovered = inside;
    if (state_ != State::Ok || !engine_) return;
    Check(engine_->Call(ScriptEngine::Fn::OnHover, EventJson(x, y) + ",\"inside\":" + (inside ? "true" : "false") + "}"),
          FnName(ScriptEngine::Fn::OnHover));
}

void WidgetInstance::OnTimer(int id) {
    if (state_ != State::Ok || !engine_) return;
    if (id == 0) {
        // The refresh: tick(), then a redraw unless it returned false.
        if (engine_->Has(ScriptEngine::Fn::Tick)) {
            const ScriptResult r = engine_->Call(ScriptEngine::Fn::Tick, {});
            if (!Check(r, FnName(ScriptEngine::Fn::Tick)) || r.json == "false") return;
        }
        RequestRedraw();
        return;
    }
    Check(engine_->FireTimer(id), "a timer callback");
}

bool WidgetInstance::Check(const ScriptResult& r, const char* what) {
    switch (r.status) {
        case ScriptStatus::Ok:
        case ScriptStatus::Missing:
            return true;
        case ScriptStatus::TimedOut: {
            wchar_t buf[160];
            swprintf_s(buf, L"%hs ran longer than %d ms and was stopped", what,
                       package_ ? package_->manifest.timeLimitMs : 50);
            Fail(State::Suspended, buf + (r.error.empty() ? std::wstring() : L"\n" + r.error));
            return false;
        }
        case ScriptStatus::OutOfMemory:
            Fail(State::Error, JsonUtil::Utf8ToWide(what) + L": out of memory (" +
                                   std::to_wstring(package_ ? package_->manifest.memoryLimitMb : 32) + L" MB limit)");
            return false;
        case ScriptStatus::Threw:
        default:
            Fail(State::Error, r.error.empty() ? JsonUtil::Utf8ToWide(what) + L" failed" : r.error);
            return false;
    }
}

void WidgetInstance::Fail(State state, const std::wstring& message) {
    state_ = state;
    error_ = message;
    runtime_.CancelAll(this);
    runtime_.ReleaseProviders(this);
    // Never called again until a reload: give its heap back now (Fail only runs after a call into
    // the script has returned).
    engine_.reset();
    ++generation_;
    requests_ = 0;
    if (PopupShown()) RequestPopup(PopupRequest{PopupRequest::Kind::Close});
    runtime_.NotifyChanged();
    Log(ScriptLogLevel::Error, (state == State::Suspended ? L"suspended: " : L"error: ") + message);
    measured_ = false;
    changed_ = true;
    client_.WidgetChanged(*this);
}

void WidgetInstance::ScheduleRefresh() {
    runtime_.Cancel(this, 0);
    if (refreshMs_ > 0 && state_ == State::Ok) runtime_.Schedule(this, 0, refreshMs_, true, true);
}

// --- ScriptHost --------------------------------------------------------------------------------

void WidgetInstance::Log(ScriptLogLevel level, const std::wstring& line) {
    LogLine l;
    GetLocalTime(&l.time);
    l.level = level;
    l.text = line.size() > 4000 ? line.substr(0, 4000) + L"…" : line;
    const wchar_t* tag = level == ScriptLogLevel::Error ? L"error" : level == ScriptLogLevel::Warn ? L"warn" : L"log";
    OutputDebugStringW((L"WinDock widget " + key_ + L" [" + tag + L"] " + l.text + L"\n").c_str());
    log_.push_back(std::move(l));
    ++logTotal_;
    while (log_.size() > kMaxLogLines) log_.pop_front();
    runtime_.NotifyLog(this);  // the Console (posts itself an update)
}

bool WidgetInstance::ReadFile(const std::string& path, std::string& bytes, std::wstring& error) {
    // media.art()'s token (only canvas.image passes it here; module names cannot contain ':').
    if (path.rfind(kArtToken, 0) == 0) {
        const MediaService* media = Permitted(Permission::Media) ? &runtime_.Media() : nullptr;
        const uint64_t serial = std::strtoull(path.c_str() + std::strlen(kArtToken), nullptr, 10);
        if (!media || !media->Active() || !media->Get().art || media->Get().artSerial != serial) {
            error = L"the media art is not available (any more)";
            return false;
        }
        bytes = *media->Get().art;
        return true;
    }
    if (!package_) {
        error = L"no package";
        return false;
    }
    return package_->Read(path, bytes, error);
}

bool WidgetInstance::ScheduleTimer(int id, uint32_t ms, bool repeat) {
    if (state_ != State::Ok) return false;
    if (runtime_.PendingTimers(this) >= kMaxTimers) return false;
    runtime_.Schedule(this, id, std::max<uint32_t>(ms, repeat ? 16u : 1u), repeat);
    return true;
}

void WidgetInstance::CancelTimer(int id) {
    if (id > 0) runtime_.Cancel(this, id);
}

void WidgetInstance::RequestRedraw() {
    measured_ = false;
    if (changed_) return;  // already asked (the dock redraws once)
    changed_ = true;
    client_.WidgetChanged(*this);
}

void WidgetInstance::SetRefresh(uint32_t ms) {
    refreshSet_ = true;
    refreshMs_ = ms == 0 ? 0 : std::clamp<uint32_t>(ms, 100u, 86400000u);
    ScheduleRefresh();
}

// --- 4b: press, drop, popups -------------------------------------------------------------------

void WidgetInstance::Press(float x, float y) {
    if (state_ != State::Ok || !engine_ || !engine_->Has(ScriptEngine::Fn::OnPress)) return;
    Check(engine_->Call(ScriptEngine::Fn::OnPress, EventJson(x, y) + ",\"button\":\"left\"}"),
          FnName(ScriptEngine::Fn::OnPress));
}

bool WidgetInstance::AcceptsDrops() const {
    return state_ == State::Ok && engine_ && engine_->Has(ScriptEngine::Fn::OnDrop) && Permitted(Permission::Shell);
}

void WidgetInstance::Drop(const std::vector<std::wstring>& files, float x, float y) {
    if (!AcceptsDrops()) return;
    std::wstring list;
    for (size_t i = 0; i < files.size() && i < 256; ++i) list += (i ? L"," : L"") + JsonUtil::Quote(files[i]);
    Check(engine_->Call(ScriptEngine::Fn::OnDrop, EventJson(x, y) + ",\"files\":[" + JsonUtil::WideToUtf8(list) + "]}"),
          FnName(ScriptEngine::Fn::OnDrop));
}

void WidgetInstance::RequestPopup(PopupRequest r) {
    popupRequest_ = std::move(r);
    client_.WidgetPopupRequest(*this);
}

bool WidgetInstance::PopupOpen(float w, float h, std::wstring& error) {
    if (!engine_ || !engine_->Has(ScriptEngine::Fn::DrawPopup)) {
        error = L"popup.open: the module does not export drawPopup(canvas, dock)";
        return false;
    }
    if (!std::isfinite(w) || !std::isfinite(h) || w < 16.0f || h < 16.0f || w > 1600.0f || h > 1200.0f) {
        error = L"popup.open: width and height must be 16 - 1600 x 16 - 1200 DIPs";
        return false;
    }
    PopupRequest r;
    r.kind = PopupRequest::Kind::Open;
    r.width = w;
    r.height = h;
    RequestPopup(std::move(r));
    return true;
}

void WidgetInstance::PopupClose() { RequestPopup(PopupRequest{PopupRequest::Kind::Close}); }

void WidgetInstance::PopupRedraw() {
    if (popupRequest_.kind == PopupRequest::Kind::Open) return;  // it draws when it opens
    RequestPopup(PopupRequest{PopupRequest::Kind::Redraw});
}

bool WidgetInstance::PopupFolder(const std::wstring& path, const std::wstring& view, std::wstring& error) {
    if (!Permitted(Permission::Shell)) {
        error = L"popup.folder needs the \"shell\" permission";
        return false;
    }
    wchar_t expanded[MAX_PATH * 2]{};
    ExpandEnvironmentStringsW(path.c_str(), expanded, static_cast<DWORD>(std::size(expanded)));
    const DWORD attrs = GetFileAttributesW(expanded);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        error = L"popup.folder: not a folder: " + path;
        return false;
    }
    ItemView v = ItemView::Auto;
    if (!view.empty() && !FromString(view, v)) {
        error = L"popup.folder: view must be \"auto\", \"grid\" or \"list\"";
        return false;
    }
    PopupRequest r;
    r.kind = PopupRequest::Kind::Folder;
    r.arg = expanded;
    r.view = ToString(v);
    RequestPopup(std::move(r));
    return true;
}

bool WidgetInstance::PopupPreviews(const std::wstring& app, std::wstring& error) {
    if (app.empty()) {
        error = L"popup.previews: an app (exe path or AppUserModelID) is needed";
        return false;
    }
    PopupRequest r;
    r.kind = PopupRequest::Kind::Previews;
    r.arg = app;
    RequestPopup(std::move(r));
    return true;
}

bool WidgetInstance::PopupCalendar(std::wstring&) {
    RequestPopup(PopupRequest{PopupRequest::Kind::Calendar});
    return true;
}

std::vector<std::string> WidgetInstance::PackageFiles() {
    return package_ ? package_->Files() : std::vector<std::string>{};
}

void WidgetInstance::RenderPopup(ID2D1DeviceContext* dc, float w, float h, float scale) {
    if (state_ != State::Ok || !engine_) return;
    canvas_.Begin(dc, scale, w, h);
    inCanvas_ = true;
    const ScriptResult r = engine_->Call(ScriptEngine::Fn::DrawPopup, {});
    inCanvas_ = false;
    canvas_.End();
    Check(r, FnName(ScriptEngine::Fn::DrawPopup));
}

void WidgetInstance::PopupClick(float x, float y, int button) {
    if (state_ != State::Ok || !engine_) return;
    const char* b = button == 1 ? "middle" : button == 2 ? "right" : "left";
    Check(engine_->Call(ScriptEngine::Fn::OnPopupClick, EventJson(x, y) + ",\"button\":\"" + b + "\"}"),
          FnName(ScriptEngine::Fn::OnPopupClick));
}

void WidgetInstance::PopupClosed() {
    if (state_ != State::Ok || !engine_) return;
    Check(engine_->Call(ScriptEngine::Fn::OnPopupClose, "{}"), FnName(ScriptEngine::Fn::OnPopupClose));
}

bool WidgetInstance::WatchFolder(std::wstring& id, std::wstring& folder) const {
    if (!package_ || package_->source != Widgets::Package::Source::Folder || !package_->manifest.watch) return false;
    id = id_;
    folder = package_->folder;
    return true;
}

// --- 4b: permissions, storage, http, shell -------------------------------------------------------

bool WidgetInstance::Permitted(Permission p) const {
    if (!package_) return false;
    switch (p) {
        case Permission::Shell: return package_->manifest.shell;
        case Permission::Audio: return package_->manifest.audio;
        case Permission::Media: return package_->manifest.media;
        case Permission::Http:
        default: return package_->manifest.http;
    }
}

bool WidgetInstance::StorageGet(const std::string& key, std::string& json) {
    return runtime_.StorageGet(id_, key, json);
}

bool WidgetInstance::StorageSet(const std::string& key, const std::string& json, std::wstring& error) {
    if (key.empty() || key.size() > 256) {
        error = L"storage keys are 1 - 256 characters";
        return false;
    }
    return runtime_.StorageSet(id_, key, json, error);
}

void WidgetInstance::StorageRemove(const std::string& key) { runtime_.StorageRemove(id_, key); }

std::vector<std::string> WidgetInstance::StorageKeys() { return runtime_.StorageKeys(id_); }

bool WidgetInstance::HostAllowed(const std::wstring& host) const {
    if (!package_) return false;
    std::wstring h = host;
    for (wchar_t& c : h) c = static_cast<wchar_t>(std::towlower(c));
    for (const std::wstring& allowed : package_->manifest.httpHosts) {
        if (allowed == h) return true;
        // "*.example.com": any sub-domain (not example.com itself).
        if (allowed.size() > 2 && allowed.rfind(L"*.", 0) == 0) {
            const std::wstring suffix = allowed.substr(1);  // ".example.com"
            if (h.size() > suffix.size() && h.compare(h.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
        }
    }
    return false;
}

int WidgetInstance::HttpStart(const std::wstring& method, const std::wstring& url, const std::wstring& headers,
                              const std::string& body, uint32_t timeoutMs, std::wstring& error) {
    if (!Permitted(Permission::Http)) {
        error = L"http needs the \"http\" permission with the hosts it may reach (widget.json)";
        return 0;
    }
    bool secure = true;
    std::wstring host, path;
    unsigned short port = 0;
    if (!Http::SplitUrl(url, secure, host, port, path)) {
        error = L"not an http(s) URL: " + url;
        return 0;
    }
    if (!HostAllowed(host)) {
        error = L"host \"" + host + L"\" is not in the widget's http permission";
        return 0;
    }
    if (requests_ >= WidgetRuntime::kMaxRequests) {
        error = L"too many requests in flight (4 per widget)";
        return 0;
    }
    Http::Request rq;
    rq.method = method;
    rq.url = url;
    rq.headers = headers;
    rq.body = body;
    rq.timeoutMs = std::clamp<uint32_t>(timeoutMs ? timeoutMs : 10000u, 1000u, 30000u);
    rq.maxBody = 2u << 20;
    const int id = nextRequest_;
    nextRequest_ = nextRequest_ >= 0x3FFFFFFF ? 1 : nextRequest_ + 1;
    if (!runtime_.StartHttp(this, generation_, id, std::move(rq))) {
        error = L"the request could not be started";
        return 0;
    }
    ++requests_;
    return id;
}

void WidgetInstance::OnHttpResult(uint64_t generation, int id, const Http::Result& result) {
    if (generation != generation_) return;  // the script was reloaded (or failed) since
    if (requests_ > 0) --requests_;
    if (state_ != State::Ok || !engine_) return;
    std::string json;
    bool ok = result.error == 0;
    if (ok) {
        json = "{\"status\":" + std::to_string(result.status) +
               ",\"ok\":" + (result.status >= 200 && result.status < 300 ? "true" : "false") +
               ",\"contentType\":" + JsonUtil::WideToUtf8(JsonUtil::Quote(result.contentType)) +
               ",\"text\":" + JsonUtil::WideToUtf8(JsonUtil::Quote(JsonUtil::Utf8ToWide(result.body))) + "}";
    } else {
        wchar_t msg[160];
        if (result.error == ERROR_FILE_TOO_LARGE) {
            swprintf_s(msg, L"http: the response is larger than 2 MB");
        } else if (result.error == ERROR_WINHTTP_TIMEOUT) {
            swprintf_s(msg, L"http: timed out");
        } else {
            swprintf_s(msg, L"http: the request failed (error %lu)", result.error);
        }
        json = JsonUtil::WideToUtf8(JsonUtil::Quote(msg));
    }
    Check(engine_->Resolve(id, ok, json), "an http callback");
}

bool WidgetInstance::ShellOpen(const std::wstring& target, const std::wstring& args, std::wstring& error) {
    if (!Permitted(Permission::Shell)) {
        error = L"shell.open needs the \"shell\" permission";
        return false;
    }
    if (target.empty() || target.size() > 2048) {
        error = L"shell.open: a file, folder or URL is needed";
        return false;
    }
    wchar_t expanded[MAX_PATH * 4]{};
    ExpandEnvironmentStringsW(target.c_str(), expanded, static_cast<DWORD>(std::size(expanded)));
    AllowSetForegroundWindow(ASFW_ANY);
    const auto rc = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, nullptr, expanded, args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL));
    if (rc <= 32) {
        error = L"shell.open: could not open " + target + L" (" + std::to_wstring(rc) + L")";
        return false;
    }
    Log(ScriptLogLevel::Info, L"shell.open " + target);
    return true;
}

bool WidgetInstance::ShellList(const std::wstring& folder, std::string& json, std::wstring& error) {
    if (!Permitted(Permission::Shell)) {
        error = L"shell.list needs the \"shell\" permission";
        return false;
    }
    wchar_t expanded[MAX_PATH * 2]{};
    ExpandEnvironmentStringsW(folder.c_str(), expanded, static_cast<DWORD>(std::size(expanded)));
    std::wstring dir = expanded;
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    struct Entry {
        std::wstring name;
        bool folder;
        uint64_t size;
        uint64_t modified;  // ms since 1970 (UTC)
    };
    std::vector<Entry> entries;
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) {
        error = L"shell.list: cannot list " + folder;
        return false;
    }
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;  // as Explorer
        ULARGE_INTEGER t{};
        t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        const uint64_t ms = t.QuadPart >= 116444736000000000ull ? (t.QuadPart - 116444736000000000ull) / 10000ull : 0;
        entries.push_back(Entry{name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                                (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow, ms});
    } while (entries.size() < 1000 && FindNextFileW(f, &fd));
    FindClose(f);
    // Folders first, then Explorer's name order ("file2" before "file10").
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.folder != b.folder) return a.folder;
        return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    });
    std::wstring out = L"[";
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        out += (i ? L"," : L"") + std::wstring(L"{\"name\":") + JsonUtil::Quote(e.name) + L",\"path\":" +
               JsonUtil::Quote(dir + L"\\" + e.name) + L",\"folder\":" + (e.folder ? L"true" : L"false") +
               L",\"size\":" + std::to_wstring(e.size) + L",\"modified\":" + std::to_wstring(e.modified) + L"}";
    }
    out += L"]";
    json = JsonUtil::WideToUtf8(out);
    return true;
}

// --- 5b: providers (audio, media) and shell.sendKeys ------------------------------------------------

void WidgetInstance::ProviderChanged(const char* channel) {
    if (state_ != State::Ok || !engine_) return;
    Check(engine_->Emit(channel), channel[0] == 'a' ? "audio.onChange" : "media.onChange");
}

namespace {

std::string J(const std::wstring& s) { return JsonUtil::WideToUtf8(JsonUtil::Quote(s)); }
std::string Num(double v) { return JsonUtil::WideToUtf8(JsonUtil::Num(v)); }
const char* Bool(bool b) { return b ? "true" : "false"; }

// The keys shell.sendKeys may press: Windows' own UI and the media keys only (docs/widgets.md).
struct KeyCombo {
    const wchar_t* name;
    WORD keys[2];  // pressed in order, released in reverse; 0 ends the list
};
constexpr KeyCombo kKeyCombos[] = {
    {L"win", {VK_LWIN, 0}},             // Start
    {L"win+a", {VK_LWIN, 'A'}},         // quick settings
    {L"win+b", {VK_LWIN, 'B'}},         // the notification area (keyboard focus)
    {L"win+d", {VK_LWIN, 'D'}},         // show / hide the desktop
    {L"win+e", {VK_LWIN, 'E'}},         // File Explorer
    {L"win+i", {VK_LWIN, 'I'}},         // Settings
    {L"win+n", {VK_LWIN, 'N'}},         // the notification centre and calendar
    {L"win+s", {VK_LWIN, 'S'}},         // Search
    {L"win+tab", {VK_LWIN, VK_TAB}},    // Task view
    {L"win+v", {VK_LWIN, 'V'}},         // clipboard history
    {L"win+w", {VK_LWIN, 'W'}},         // Widgets
    {L"win+x", {VK_LWIN, 'X'}},         // the Start button's context menu
    {L"volume-up", {VK_VOLUME_UP, 0}},
    {L"volume-down", {VK_VOLUME_DOWN, 0}},
    {L"volume-mute", {VK_VOLUME_MUTE, 0}},
    {L"media-play-pause", {VK_MEDIA_PLAY_PAUSE, 0}},
    {L"media-next", {VK_MEDIA_NEXT_TRACK, 0}},
    {L"media-previous", {VK_MEDIA_PREV_TRACK, 0}},
    {L"media-stop", {VK_MEDIA_STOP, 0}},
};

bool ExtendedKey(WORD vk) {
    switch (vk) {
        case VK_LWIN:
        case VK_RWIN:
        case VK_VOLUME_UP:
        case VK_VOLUME_DOWN:
        case VK_VOLUME_MUTE:
        case VK_MEDIA_PLAY_PAUSE:
        case VK_MEDIA_NEXT_TRACK:
        case VK_MEDIA_PREV_TRACK:
        case VK_MEDIA_STOP:
            return true;
        default:
            return false;
    }
}

}  // namespace

bool WidgetInstance::SendKeys(const std::wstring& name, std::wstring& error) {
    if (!Permitted(Permission::Shell)) {
        error = L"shell.sendKeys needs the \"shell\" permission";
        return false;
    }
    std::wstring n = name;
    for (wchar_t& c : n) c = static_cast<wchar_t>(std::towlower(c));
    const KeyCombo* combo = nullptr;
    for (const KeyCombo& k : kKeyCombos) {
        if (n == k.name) combo = &k;
    }
    if (!combo) {
        error = L"shell.sendKeys: \"" + name + L"\" is not in the allow-list (docs/widgets.md)";
        return false;
    }
    const size_t count = combo->keys[1] ? 2 : 1;
    INPUT in[4]{};
    for (size_t i = 0; i < count; ++i) {
        INPUT& down = in[i];
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = combo->keys[i];
        down.ki.dwFlags = ExtendedKey(combo->keys[i]) ? KEYEVENTF_EXTENDEDKEY : 0;
        INPUT& up = in[2 * count - 1 - i];
        up.type = INPUT_KEYBOARD;
        up.ki.wVk = combo->keys[i];
        up.ki.dwFlags = KEYEVENTF_KEYUP | (ExtendedKey(combo->keys[i]) ? KEYEVENTF_EXTENDEDKEY : 0);
    }
    const UINT total = static_cast<UINT>(2 * count);
    if (SendInput(total, in, sizeof(INPUT)) != total) {
        error = L"shell.sendKeys: Windows blocked the input (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    Log(ScriptLogLevel::Info, L"shell.sendKeys " + n);
    return true;
}

bool WidgetInstance::AudioGet(std::string& json, std::wstring& error) {
    if (!Permitted(Permission::Audio)) {
        error = L"audio needs the \"audio\" permission";
        return false;
    }
    const AudioService* a = runtime_.UseAudio(this);
    if (!a) {
        error = L"audio: Core Audio is not available";
        return false;
    }
    const AudioService::State& s = a->Get();
    std::string devices = "[";
    for (size_t i = 0; i < s.devices.size(); ++i) {
        const AudioService::Device& d = s.devices[i];
        devices += std::string(i ? "," : "") + "{\"id\":" + J(d.id) + ",\"name\":" + J(d.name) +
                   ",\"default\":" + Bool(d.isDefault) + "}";
    }
    devices += "]";
    json = std::string("{\"available\":") + Bool(s.output) + ",\"volume\":" + Num(s.volume) +
           ",\"muted\":" + Bool(s.muted) + ",\"device\":" + J(s.deviceName) + ",\"deviceId\":" + J(s.deviceId) +
           ",\"mic\":" + Bool(s.input) + ",\"micVolume\":" + Num(s.micVolume) + ",\"micMuted\":" + Bool(s.micMuted) +
           ",\"micName\":" + J(s.micName) + ",\"devices\":" + devices + "}";
    return true;
}

bool WidgetInstance::AudioSet(const std::string& what, double number, const std::wstring& text, std::wstring& error) {
    if (!Permitted(Permission::Audio)) {
        error = L"audio needs the \"audio\" permission";
        return false;
    }
    AudioService* a = runtime_.UseAudio(this);
    if (!a) {
        error = L"audio: Core Audio is not available";
        return false;
    }
    if (what == "volume") return a->SetVolume(static_cast<float>(number), error);
    if (what == "muted") return a->SetMuted(number != 0.0, error);
    if (what == "micMuted") return a->SetMicMuted(number != 0.0, error);
    if (what == "device") return a->SetDevice(text, error);
    error = L"audio: unknown setting";
    return false;
}

bool WidgetInstance::MediaGet(std::string& json, std::wstring& error) {
    if (!Permitted(Permission::Media)) {
        error = L"media needs the \"media\" permission";
        return false;
    }
    const MediaService* m = runtime_.UseMedia(this);
    if (!m) {
        error = L"media: the media session manager could not be started";
        return false;
    }
    const MediaService::State& s = m->Get();
    static const char* const kStatus[] = {"none", "stopped", "paused", "playing"};
    const MediaService::Status status = s.session ? s.status : MediaService::Status::None;
    const double pos = m->PositionNow();
    json = std::string("{\"available\":") + Bool(s.available) + ",\"session\":" + Bool(s.session) +
           ",\"title\":" + J(s.title) + ",\"artist\":" + J(s.artist) + ",\"album\":" + J(s.album) +
           ",\"app\":" + J(s.app) + ",\"status\":\"" + kStatus[static_cast<int>(status)] +
           "\",\"hasArt\":" + Bool(s.art != nullptr) +
           ",\"position\":" + (pos >= 0.0 ? Num(std::round(pos * 10.0) / 10.0) : std::string("null")) +
           ",\"duration\":" + (s.duration > 0.0 ? Num(std::round(s.duration * 10.0) / 10.0) : std::string("null")) +
           ",\"canPlay\":" + Bool(s.canPlay) + ",\"canPause\":" + Bool(s.canPause) + ",\"canToggle\":" +
           Bool(s.canToggle) + ",\"canNext\":" + Bool(s.canNext) + ",\"canPrevious\":" + Bool(s.canPrevious) + "}";
    return true;
}

bool WidgetInstance::MediaCommand(const std::string& command, std::wstring& error) {
    if (!Permitted(Permission::Media)) {
        error = L"media needs the \"media\" permission";
        return false;
    }
    MediaService* m = runtime_.UseMedia(this);
    if (!m) {
        error = L"media: the media session manager could not be started";
        return false;
    }
    using C = MediaService::Command;
    if (command == "play") {
        m->Send(C::Play);
    } else if (command == "pause") {
        m->Send(C::Pause);
    } else if (command == "toggle") {
        m->Send(C::Toggle);
    } else if (command == "next") {
        m->Send(C::Next);
    } else if (command == "previous") {
        m->Send(C::Previous);
    } else {
        error = L"media: unknown command";
        return false;
    }
    Log(ScriptLogLevel::Info, L"media." + JsonUtil::Utf8ToWide(command));
    return true;
}

bool WidgetInstance::MediaArt(std::string& token, std::wstring& error) {
    if (!Permitted(Permission::Media)) {
        error = L"media needs the \"media\" permission";
        return false;
    }
    const MediaService* m = runtime_.UseMedia(this);
    token.clear();
    if (m && m->Get().art && m->Get().artSerial) token = kArtToken + std::to_string(m->Get().artSerial);
    return true;
}
