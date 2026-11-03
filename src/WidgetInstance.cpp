#include "WidgetInstance.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "JsonUtil.h"
#include "WidgetRuntime.h"

namespace {

namespace json = winrt::Windows::Data::Json;

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
        default: return "a call";
    }
}

}  // namespace

WidgetInstance::WidgetInstance(WidgetRuntime& runtime, Client& client, ID2D1Factory1* d2d, IDWriteFactory* dwrite,
                               std::wstring id, std::wstring key, std::wstring itemSettings)
    : runtime_(runtime),
      client_(client),
      id_(std::move(id)),
      key_(std::move(key)),
      itemSettings_(std::move(itemSettings)),
      canvas_(d2d, dwrite, [this](const std::string& path, std::string& bytes, std::wstring& error) {
          return ReadFile(path, bytes, error);
      }) {
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
    engine_.reset();
    canvas_.ReleaseImages();
    state_ = State::Ok;
    error_.clear();
    refreshMs_ = 0;
    refreshSet_ = false;
    measured_ = false;
    changed_ = true;

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

void WidgetInstance::SetDock(const ScriptDockInfo& info) {
    const bool hovered = dock_.hovered, magnified = dock_.magnified;
    const float w = dock_.width, h = dock_.height, mn = dock_.main, cr = dock_.cross;
    dock_ = info;
    dock_.widgetId = JsonUtil::WideToUtf8(id_);
    dock_.hovered = hovered;
    dock_.magnified = magnified;
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

void WidgetInstance::Click(float x, float y) {
    if (state_ != State::Ok || !engine_) return;
    Check(engine_->Call(ScriptEngine::Fn::OnClick, EventJson(x, y) + ",\"button\":\"left\"}"), FnName(ScriptEngine::Fn::OnClick));
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
    // Never called again until a reload: give its heap back now (Fail only runs after a call into
    // the script has returned).
    engine_.reset();
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
}

bool WidgetInstance::ReadFile(const std::string& path, std::string& bytes, std::wstring& error) {
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
