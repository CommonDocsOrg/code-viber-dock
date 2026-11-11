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

#include "CalendarService.h"
#include "JsonUtil.h"
#include "NotificationService.h"
#include "StartMenuPlacer.h"
#include "TextEditorWindow.h"
#include "Weather.h"
#include "WeatherGlyphs.h"
#include "WidgetRuntime.h"
#include "WidgetSamples.h"

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
                               const ScriptDockInfo& dock, const WeatherConfig& weather)
    : runtime_(runtime),
      client_(client),
      preview_(dock.preview),
      id_(std::move(id)),
      key_(std::move(key)),
      itemSettings_(std::move(itemSettings)),
      weather_(weather),
      canvas_(d2d, dwrite, [this](const std::string& path, std::string& bytes, std::wstring& error) {
          return ReadFile(path, bytes, error);
      }),
      dock_(dock) {
    dock_.widgetId = JsonUtil::WideToUtf8(id_);
    UpdateItemKey();
    if (!preview_) runtime_.Add(this);  // a preview is nobody's: no Console, timers or providers
    Reload();
}

WidgetInstance::~WidgetInstance() {
    DropLayers(nullptr);  // round 16 phase 16F: the slot's child visuals go with the instance
    DropEditor(false);  // the host forgets its hold itself (it is removing this instance)
    if (!preview_) {
        runtime_.CancelAll(this);
        runtime_.Remove(this);
    }
    engine_.reset();  // its finalizers may still log
}

std::wstring WidgetInstance::Name() const {
    return package_ && !package_->manifest.name.empty() ? package_->manifest.name : id_;
}

void WidgetInstance::Reload() {
    DropEditor(true);  // an open editor goes; the old script's promise is never settled
    if (!preview_) {
        runtime_.CancelAll(this);
        runtime_.ReleaseProviders(this);
    }
    previewTimers_ = 0;
    scratch_.clear();
    if (PopupShown() || client_.WidgetPassiveShown(*this)) RequestPopup(PopupRequest{PopupRequest::Kind::Close});
    engine_.reset();
    canvas_.ReleaseImages();
    state_ = State::Ok;
    error_.clear();
    refreshMs_ = 0;
    refreshSet_ = false;
    refreshZone_.clear();
    measured_ = false;
    changed_ = true;
    ++generation_;  // results of the old script's requests are dropped
    requests_ = 0;
    struct Synced {  // whatever happens below, the watcher and the Console follow (not for a preview)
        WidgetRuntime& rt;
        bool on;
        ~Synced() {
            if (!on) return;
            rt.SyncWatch();
            rt.NotifyChanged();
        }
    } synced{runtime_, !preview_};

    std::wstring error;
    package_ = Widgets::Load(id_, error);
    if (!package_) {
        Fail(State::Error, error);
        return;
    }
    const Widgets::Manifest& m = package_->manifest;
    // The manifest's problems (Widgets::Validate): every one logged with its field and fix; an
    // error stops the widget (the tile shows the first).
    const Widgets::Issue* firstError = nullptr;
    for (const Widgets::Issue& i : m.issues) {
        Log(i.error ? ScriptLogLevel::Error : ScriptLogLevel::Warn, L"widget.json: " + i.Text());
        if (i.error && !firstError) firstError = &i;
    }
    if (firstError) {
        Fail(State::Error, L"widget.json: " + firstError->field + L": " + firstError->message +
                               (firstError->fix.empty() ? L"" : L" (fix: " + firstError->fix + L")"));
        return;
    }
    // A preview (round 6 phase 6E): the manifest's preview.settings over the defaults.
    settings_ = Widgets::MergeSettings(m, preview_ ? m.previewSettings : itemSettings_);
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

void WidgetInstance::SetWeatherConfig(const WeatherConfig& weather) {
    if (weather == weather_) return;
    const bool samePlace = WeatherService::SamePlace(weather, weather_);
    weather_ = weather;
    if (!samePlace && !preview_) runtime_.WeatherPlaceChanged(this);  // its service follows (posted)
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
    WidgetSystem::ForgetZones();  // the zone data may have changed with it
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
    UpdateItemKey();
}

void WidgetInstance::UpdateItemKey() {
    // A dock: "<dock id>/<widget id>#<n>" (the instance key: the n-th item of this widget on the
    // dock, so it survives moves past other items); standalone: the entry's id.
    dock_.itemKey = dock_.standalone ? dock_.dockId : dock_.dockId + "/" + JsonUtil::WideToUtf8(key_);
}

void WidgetInstance::BoxDips(float& w, float& h) const {
    const float s = dock_.scale > 0.0f ? dock_.scale : 1.0f;
    const bool vertical = measuredAxis_ == DockAxis::Vertical;
    w = (vertical ? size_.cross : size_.main) / s;
    h = (vertical ? size_.main : size_.cross) / s;
}

void WidgetInstance::SetStandaloneBox(float widthDip, float heightDip) {
    if (widthDip == boxW_ && heightDip == boxH_) return;
    boxW_ = widthDip;
    boxH_ = heightDip;
    measured_ = false;
    changed_ = true;
}

ElementSize WidgetInstance::Measure(const LayoutContext& ctx) {
    if (measured_ && measuredAxis_ == ctx.axis && measuredScale_ == ctx.scale && measuredIcon_ == ctx.iconPx &&
        measuredBarCross_ == ctx.barCross) {
        return size_;
    }
    const bool vertical = ctx.axis == DockAxis::Vertical;
    const float s = ctx.scale > 0.0f ? ctx.scale : 1.0f;
    if (dock_.standalone) {
        // Round 6 phase 6B: the window is the box (no measure(), no size hint); iconSize is the
        // shorter side / 1.5 so widgets that scale from it fill the window.
        const float w = std::max(1.0f, boxW_), h = std::max(1.0f, boxH_);
        dock_.axis = "horizontal";
        dock_.edge = "bottom";
        dock_.scale = s;
        dock_.iconSize = std::min(w, h) / 1.5f;
        size_ = ElementSize{std::max(1.0f, std::round(w * s)), std::max(1.0f, std::round(h * s))};
        measured_ = true;
        measuredAxis_ = DockAxis::Horizontal;
        measuredScale_ = ctx.scale;
        measuredIcon_ = ctx.iconPx;
        measuredBarCross_ = ctx.barCross;
        dock_.main = dock_.width = size_.main / s;
        dock_.cross = dock_.height = size_.cross / s;
        return size_;
    }
    Widgets::SizeHint hint = vertical ? Widgets::SizeHint{48.0f, 48.0f} : Widgets::SizeHint{64.0f, 40.0f};
    if (package_) hint = vertical ? package_->manifest.vertical : package_->manifest.horizontal;
    float mainDip = hint.main, crossDip = hint.cross;
    dock_.axis = vertical ? "vertical" : "horizontal";
    dock_.scale = s;
    if (!preview_) dock_.iconSize = ctx.iconPx / s;  // a preview keeps its virtual dock's 48 DIPs at any scale
    dock_.main = mainDip;
    dock_.cross = crossDip;
    dock_.width = vertical ? crossDip : mainDip;
    dock_.height = vertical ? mainDip : crossDip;
    bool flush = false;
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
                    JsonUtil::ReadBool(o, L"flush", flush);  // round 11 phase 11C
                }
            } catch (...) {
            }
        }
    }
    // A preview (round 6 phase 6E): the manifest's preview.width / height replace the measured box
    // (DIPs, upright: on the virtual horizontal dock width is main and height cross).
    if (preview_ && package_ && state_ == State::Ok) {
        const Widgets::Manifest& pm = package_->manifest;
        if (pm.previewWidth > 0.0f) (vertical ? crossDip : mainDip) = pm.previewWidth;
        if (pm.previewHeight > 0.0f) (vertical ? mainDip : crossDip) = pm.previewHeight;
    }
    // Never thinner than the error tile can show something in.
    if (state_ != State::Ok) {
        mainDip = std::max(mainDip, 24.0f);
        crossDip = std::max(crossDip, 24.0f);
    }
    size_ = ElementSize{std::ceil(mainDip * s), std::ceil(crossDip * s)};
    // Round 11 phase 11C: `flush` (measure()'s result) on a full-width bar: the box is the bar's
    // whole thickness, so the canvas the widget draws in is too (dock.cross / height below).
    if (flush && state_ == State::Ok && !preview_ && ctx.barCross > 0.0f) {
        size_.cross = std::round(ctx.barCross);
        size_.flush = true;
    }
    measured_ = true;
    measuredAxis_ = ctx.axis;
    measuredScale_ = ctx.scale;
    measuredIcon_ = ctx.iconPx;
    measuredBarCross_ = ctx.barCross;
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
    // Round 16 phase 16F: layers. With reduce motion the layers' content is drawn inline (a
    // still picture at the first keys), which also means no child visuals at all.
    layerRenderer_ = &renderer;
    canvas_.SetLayerSink(dock_.reduceMotion ? nullptr : this);
    for (auto& [id, ch] : layers_) ch.used = false;
    const HRESULT hr = renderer.DrawLayer(layer, wPx, hPx, [&](ID2D1DeviceContext* dc) {
        if (state_ == State::Ok && engine_) {
            ++draws_;
            canvas_.Begin(dc, dock_.scale, w, h);
            inCanvas_ = true;
            const ScriptResult r = engine_->Call(ScriptEngine::Fn::Draw, {});
            inCanvas_ = false;
            canvas_.End();
            if (Check(r, FnName(ScriptEngine::Fn::Draw))) return;
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));  // whatever it drew before it failed
            for (auto& [id, ch] : layers_) ch.used = false;  // and its layers
            changed_ = false;  // the tile is drawn right here
        }
        DrawErrorTile(dc, w, h);
    });
    canvas_.SetLayerSink(nullptr);
    layerRenderer_ = nullptr;
    RealizeLayers(renderer, layer, dock_.scale);
    return hr;
}

// --- composition layers (round 16 phase 16F) -----------------------------------------------------

namespace {

// The value of a cubic-bezier easing (x1, y1, x2, y2) at progress s (0 - 1): the y of the curve
// where its x is s (a few Newton steps, then bisection when they do not converge).
float Eased(const std::array<float, 4>& b, double s) {
    if (s <= 0.0) return 0.0f;
    if (s >= 1.0) return 1.0f;
    const double x1 = b[0], y1 = b[1], x2 = b[2], y2 = b[3];
    auto bx = [&](double t) { return 3.0 * (1 - t) * (1 - t) * t * x1 + 3.0 * (1 - t) * t * t * x2 + t * t * t; };
    auto by = [&](double t) { return 3.0 * (1 - t) * (1 - t) * t * y1 + 3.0 * (1 - t) * t * t * y2 + t * t * t; };
    auto dbx = [&](double t) { return 3.0 * (1 - t) * (1 - t) * x1 + 6.0 * (1 - t) * t * (x2 - x1) + 3.0 * t * t * (1 - x2); };
    double t = s;
    for (int i = 0; i < 8; ++i) {
        const double d = dbx(t);
        if (std::fabs(d) < 1e-6) break;
        const double nt = t - (bx(t) - s) / d;
        if (nt < 0.0 || nt > 1.0) break;
        if (std::fabs(nt - t) < 1e-7) return static_cast<float>(by(nt));
        t = nt;
    }
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 40; ++i) {
        t = (lo + hi) / 2.0;
        if (bx(t) < s) lo = t; else hi = t;
    }
    return static_cast<float>(by(t));
}

// The track's value at loop time u (0 - duration), from its key spans.
double TrackValue(const CanvasTrack& t, double u) {
    const auto& k = t.keys;
    if (u <= k.front().first) return k.front().second;
    if (u >= k.back().first) return k.back().second;
    size_t j = 0;
    while (j + 2 < k.size() && u >= k[j + 1].first) ++j;
    const double t0 = k[j].first, t1 = k[j + 1].first;
    const double s = t1 > t0 ? (u - t0) / (t1 - t0) : 1.0;
    const double p = j < t.easing.size() ? Eased(t.easing[j], s) : s;
    return k[j].second + (k[j + 1].second - k[j].second) * p;
}

// A DirectComposition animation of one loop of `track` (values x `mult`): the loop is shifted by
// `begin` (the value at animation time s is the track's at (s - begin) mod duration, as SMIL's
// begin staggers a raindrop's loop against the next one's), cut at every key and at the wrap
// into pieces, each piece a cubic Hermite fit (exact for linear spans, within a hair of the
// bezier easings over 4 pieces), then repeated forever (or ended at the last key).
winrt::com_ptr<IDCompositionAnimation> BuildTrackAnimation(Renderer& renderer, const CanvasTrack& t, float mult) {
    const double D = t.duration;
    if (!(D > 0.0) || t.keys.size() < 2) return nullptr;
    winrt::com_ptr<IDCompositionAnimation> anim = renderer.CreateAnimation();
    if (!anim) return nullptr;
    const double b = std::fmod(std::fmod(t.begin, D) + D, D);
    // The shifted value; `side` picks the limit at the wrap (s = b): from the left the loop's end,
    // from the right its start.
    auto G = [&](double s, int side) {
        double u = std::fmod(s - b + D, D);
        if (side < 0 && u < 1e-9) u = D;
        if (side > 0 && u > D - 1e-9) u = 0.0;
        return TrackValue(t, u) * mult;
    };
    std::vector<double> cuts{0.0, D};
    for (const auto& k : t.keys) cuts.push_back(std::fmod(k.first + b, D));
    if (b > 0.0) cuts.push_back(b);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double x, double y) { return std::fabs(x - y) < 1e-6; }), cuts.end());
    const bool eased = !t.easing.empty();
    float last = static_cast<float>(G(D, -1));
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        const double s0 = cuts[i], s1 = cuts[i + 1];
        if (s1 - s0 < 1e-6) continue;
        const int pieces = eased ? 4 : 1;
        for (int p = 0; p < pieces; ++p) {
            const double a = s0 + (s1 - s0) * p / pieces, c = s0 + (s1 - s0) * (p + 1) / pieces, h = c - a;
            const double e = std::min(1e-4, h / 4.0);
            const double p0 = G(a, +1), p1 = G(c, -1);
            const double m0 = (G(a + e, +1) - p0) / e, m1 = (p1 - G(c - e, -1)) / e;
            const double c2 = (3.0 * (p1 - p0) - 2.0 * m0 * h - m1 * h) / (h * h);
            const double c3 = (2.0 * (p0 - p1) + m0 * h + m1 * h) / (h * h * h);
            anim->AddCubic(a, static_cast<float>(p0), static_cast<float>(m0), static_cast<float>(c2), static_cast<float>(c3));
            last = static_cast<float>(p1);
        }
    }
    if (t.repeat) {
        anim->AddRepeat(D, D);
    } else {
        anim->End(D, last);
    }
    return anim;
}

}  // namespace

ID2D1DeviceContext* WidgetInstance::BeginLayer(const CanvasLayerSpec& spec, float) {
    if (!layerRenderer_ || !openLayer_.empty() || preview_) return nullptr;
    ID2D1DeviceContext* dc = layerRenderer_->ResourceContext();
    if (!dc) return nullptr;
    winrt::com_ptr<ID2D1CommandList> list;
    if (FAILED(dc->CreateCommandList(list.put()))) return nullptr;
    ChildLayer& ch = layers_[spec.id];
    if (std::find(layerOrder_.begin(), layerOrder_.end(), spec.id) == layerOrder_.end()) layerOrder_.push_back(spec.id);
    ch.used = true;
    ch.spec = spec;
    savedTarget_ = nullptr;
    dc->GetTarget(savedTarget_.put());
    dc->SetTarget(list.get());
    dc->BeginDraw();
    dc->SetDpi(96.0f, 96.0f);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ch.list = std::move(list);
    openLayer_ = spec.id;
    return dc;
}

void WidgetInstance::EndLayer() {
    if (openLayer_.empty()) return;
    ChildLayer& ch = layers_[openLayer_];
    ID2D1DeviceContext* dc = layerRenderer_ ? layerRenderer_->ResourceContext() : nullptr;
    if (dc) {
        const HRESULT hr = dc->EndDraw();
        dc->SetTarget(savedTarget_.get());
        if (FAILED(hr)) ch.list = nullptr;
    } else {
        ch.list = nullptr;
    }
    if (ch.list && FAILED(ch.list->Close())) ch.list = nullptr;
    savedTarget_ = nullptr;
    openLayer_.clear();
}

void WidgetInstance::RealizeLayers(Renderer& renderer, Layer& slot, float scale) {
    for (auto it = layers_.begin(); it != layers_.end();) {
        if (it->second.used) {
            ++it;
            continue;
        }
        renderer.DestroyLayer(it->second.layer);
        layerOrder_.erase(std::remove(layerOrder_.begin(), layerOrder_.end(), it->first), layerOrder_.end());
        it = layers_.erase(it);
    }
    if (layers_.empty() || !slot.visual) return;
    for (const std::string& id : layerOrder_) {
        const auto it = layers_.find(id);
        if (it == layers_.end()) continue;
        ChildLayer& ch = it->second;
        Layer* parent = &slot;
        if (!ch.spec.parent.empty()) {
            const auto p = layers_.find(ch.spec.parent);
            if (p != layers_.end() && p->second.layer.visual) parent = &p->second.layer;
        }
        if (!ch.layer.visual || ch.parentVisual != parent->visual.get()) {
            renderer.DestroyLayer(ch.layer);
            if (FAILED(renderer.CreateLayer(ch.layer, parent))) {
                ch.list = nullptr;
                continue;
            }
            ch.parentVisual = parent->visual.get();
            ch.rotate = nullptr;
            ch.translate = nullptr;
            ch.animatedKey.clear();
        }
        const UINT w = static_cast<UINT>(std::max(1.0f, std::ceil(ch.spec.w * scale)));
        const UINT h = static_cast<UINT>(std::max(1.0f, std::ceil(ch.spec.h * scale)));
        if (ch.list) {
            winrt::com_ptr<ID2D1CommandList> list = std::move(ch.list);
            renderer.DrawLayer(ch.layer, w, h, [&](ID2D1DeviceContext* dc) { dc->DrawImage(list.get()); });
        } else if (ch.layer.width != w || ch.layer.height != h) {
            renderer.DrawLayer(ch.layer, w, h, [](ID2D1DeviceContext*) {});  // a group without content of its own
        }
        renderer.SetOffset(ch.layer, std::round(ch.spec.x * scale), std::round(ch.spec.y * scale));
        if (ch.spec.clip) renderer.SetClip(ch.layer, D2D1::RectF(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)));
        if (ch.animatedKey != ch.spec.key) {
            ApplyLayerTracks(renderer, ch, scale);
            ch.animatedKey = ch.spec.key;
        }
    }
}

void WidgetInstance::ApplyLayerTracks(Renderer& renderer, ChildLayer& ch, float scale) {
    IDCompositionDesktopDevice* dcomp = renderer.DComp();
    if (!dcomp || !ch.layer.visual) return;
    bool moves = false;
    for (const CanvasTrack& t : ch.spec.tracks) moves = moves || t.prop != "opacity";
    // The static values first (they also detach older animations).
    renderer.StopOpacityAnimation(ch.layer, ch.spec.opacity);
    if (!moves) {
        ch.layer.visual->SetTransform(D2D1::Matrix3x2F::Identity());
        ch.rotate = nullptr;
        ch.translate = nullptr;
    } else {
        if (!ch.rotate || !ch.translate) {
            winrt::com_ptr<IDCompositionRotateTransform> rotate;
            winrt::com_ptr<IDCompositionTranslateTransform> translate;
            winrt::com_ptr<IDCompositionTransform> group;
            if (FAILED(dcomp->CreateRotateTransform(rotate.put())) || FAILED(dcomp->CreateTranslateTransform(translate.put()))) return;
            IDCompositionTransform* parts[] = {rotate.get(), translate.get()};  // rotate about the centre, then move
            if (FAILED(dcomp->CreateTransformGroup(parts, 2, group.put())) || FAILED(ch.layer.visual->SetTransform(group.get()))) return;
            ch.rotate = std::move(rotate);
            ch.translate = std::move(translate);
        }
        ch.rotate->SetAngle(0.0f);
        ch.translate->SetOffsetX(0.0f);
        ch.translate->SetOffsetY(0.0f);
    }
    for (const CanvasTrack& t : ch.spec.tracks) {
        const bool px = t.prop == "x" || t.prop == "y";
        const float mult = px ? scale : t.prop == "opacity" ? ch.spec.opacity : 1.0f;
        winrt::com_ptr<IDCompositionAnimation> anim = BuildTrackAnimation(renderer, t, mult);
        if (!anim) continue;
        if (t.prop == "rotate") {
            ch.rotate->SetCenterX(t.cx * scale);
            ch.rotate->SetCenterY(t.cy * scale);
            ch.rotate->SetAngle(anim.get());
        } else if (t.prop == "x") {
            ch.translate->SetOffsetX(anim.get());
        } else if (t.prop == "y") {
            ch.translate->SetOffsetY(anim.get());
        } else {
            renderer.AnimateOpacity(ch.layer, anim.get());
        }
    }
}

void WidgetInstance::DropLayers(Renderer* renderer) {
    for (auto& [id, ch] : layers_) {
        if (renderer) {
            renderer->DestroyLayer(ch.layer);
        } else {
            if (ch.layer.visual && ch.layer.parent) ch.layer.parent->RemoveVisual(ch.layer.visual.get());
            ch.layer.Reset();
        }
    }
    layers_.clear();
    layerOrder_.clear();
}

void WidgetInstance::PixelSize(UINT& w, UINT& h) const {
    const bool vertical = measuredAxis_ == DockAxis::Vertical;
    w = measured_ ? static_cast<UINT>(vertical ? size_.cross : size_.main) : 0;
    h = measured_ ? static_cast<UINT>(vertical ? size_.main : size_.cross) : 0;
}

void WidgetInstance::DrawInto(ID2D1DeviceContext* dc) {
    if (!dc || state_ != State::Ok || !engine_ || !measured_) return;
    float w = 0, h = 0;
    BoxDips(w, h);
    ++draws_;
    changed_ = false;
    canvas_.Begin(dc, dock_.scale, w, h);
    inCanvas_ = true;
    const ScriptResult r = engine_->Call(ScriptEngine::Fn::Draw, {});
    inCanvas_ = false;
    canvas_.End();
    Check(r, FnName(ScriptEngine::Fn::Draw));
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
    DropEditor(true);
    if (!preview_) {
        runtime_.CancelAll(this);
        runtime_.ReleaseProviders(this);
    }
    // Never called again until a reload: give its heap back now (Fail only runs after a call into
    // the script has returned).
    engine_.reset();
    ++generation_;
    requests_ = 0;
    if (PopupShown() || client_.WidgetPassiveShown(*this)) RequestPopup(PopupRequest{PopupRequest::Kind::Close});
    if (!preview_) runtime_.NotifyChanged();
    Log(ScriptLogLevel::Error, (state == State::Suspended ? L"suspended: " : L"error: ") + message);
    measured_ = false;
    changed_ = true;
    client_.WidgetChanged(*this);
}

void WidgetInstance::ScheduleRefresh() {
    if (preview_) return;  // a preview never ticks (it shows the time it was drawn)
    runtime_.Cancel(this, 0);
    if (refreshMs_ > 0 && state_ == State::Ok) runtime_.Schedule(this, 0, refreshMs_, true, true, refreshZone_);
}

// --- ScriptHost --------------------------------------------------------------------------------

void WidgetInstance::Log(ScriptLogLevel level, const std::wstring& line) {
    LogLine l;
    GetLocalTime(&l.time);
    l.level = level;
    l.text = line.size() > 4000 ? line.substr(0, 4000) + L"…" : line;
    const wchar_t* tag = level == ScriptLogLevel::Error ? L"error" : level == ScriptLogLevel::Warn ? L"warn" : L"log";
    log_.push_back(std::move(l));
    ++logTotal_;
    while (log_.size() > kMaxLogLines) log_.pop_front();
    if (preview_) return;  // a preview's lines stay in its own log (WidgetPreview reports a failure once)
    OutputDebugStringW((L"WinDock widget " + key_ + L" [" + tag + L"] " + log_.back().text + L"\n").c_str());
    runtime_.NotifyLog(this);  // the Console (posts itself an update)
}

bool WidgetInstance::ReadFile(const std::string& path, std::string& bytes, std::wstring& error) {
    // media.art()'s token (only canvas.image passes it here; module names cannot contain ':').
    if (path.rfind(kArtToken, 0) == 0 && preview_) {
        // A preview: the sample art (WidgetSamples).
        if (!Permitted(Permission::Media) ||
            std::strtoull(path.c_str() + std::strlen(kArtToken), nullptr, 10) != WidgetSamples::kArtSerial) {
            error = L"the media art is not available (any more)";
            return false;
        }
        bytes = WidgetSamples::MediaArtBytes();
        return true;
    }
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
    if (preview_) return ++previewTimers_ <= kMaxTimers;  // accepted, never scheduled: it never fires
    if (runtime_.PendingTimers(this) >= kMaxTimers) return false;
    runtime_.Schedule(this, id, std::max<uint32_t>(ms, repeat ? 16u : 1u), repeat);
    return true;
}

void WidgetInstance::CancelTimer(int id) {
    if (preview_) {
        if (previewTimers_ > 0) --previewTimers_;
        return;
    }
    if (id > 0) runtime_.Cancel(this, id);
}

void WidgetInstance::RequestRedraw() {
    measured_ = false;
    if (changed_) return;  // already asked (the dock redraws once)
    changed_ = true;
    client_.WidgetChanged(*this);
}

void WidgetInstance::SetRefresh(uint32_t ms, const std::wstring& timeZone) {
    refreshSet_ = true;
    refreshZone_ = timeZone.size() > 128 ? std::wstring() : timeZone;
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
    if (preview_) return;  // a preview opens nothing (popups, the folder / calendar popups, settings)
    popupRequest_ = std::move(r);
    client_.WidgetPopupRequest(*this);
}

bool WidgetInstance::PopupOpen(float w, float h, bool passive, std::wstring& error) {
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
    r.passive = passive;
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

bool WidgetInstance::PopupCalendar(int64_t day, std::wstring&) {
    PopupRequest r;
    r.kind = PopupRequest::Kind::Calendar;
    r.day = day;
    RequestPopup(std::move(r));
    return true;
}

// --- round 6 phase 6C: popup.editText --------------------------------------------------------------

int WidgetInstance::EditText(const EditTextRequest& request, std::wstring& error) {
    if (preview_) {
        error = L"not available in preview";
        return 0;
    }
    if (state_ != State::Ok || !engine_) {
        error = L"the widget is not running";
        return 0;
    }
    if (editor_ || editPending_) {
        error = L"an editor is already open";
        return 0;
    }
    if (!client_.WidgetEditorPlace(*this).owner) {
        error = L"there is no editor for this widget here";
        return 0;
    }
    auto p = std::make_unique<EditPending>();
    p->id = nextEdit_;
    nextEdit_ = nextEdit_ >= 0x3FFFFFFF ? 1 : nextEdit_ + 1;
    p->generation = generation_;
    p->request = request;
    const int id = p->id;
    editPending_ = std::move(p);
    client_.WidgetPopupRequest(*this);  // posted: the host calls OpenPendingEditor
    return id;
}

void WidgetInstance::OpenPendingEditor() {
    if (!editPending_) return;
    const std::unique_ptr<EditPending> p = std::move(editPending_);
    if (p->generation != generation_ || state_ != State::Ok || !engine_ || editor_) return;
    const Client::EditorPlace place = client_.WidgetEditorPlace(*this);
    TextEditorWindow::Options o;
    o.title = p->request.title;
    o.text = p->request.text;
    o.placeholder = p->request.placeholder;
    o.multiline = p->request.multiline;
    o.maxLength = p->request.maxLength;
    o.sizeKey = id_ + (o.multiline ? L"" : L"|line");
    o.side = static_cast<TextEditorWindow::Side>(std::clamp(place.side, 0, 3));
    const uint64_t generation = p->generation;
    const int id = p->id;
    TextEditorWindow* e = nullptr;
    if (place.owner) {
        e = TextEditorWindow::Open(place.owner, place.box, place.topmost, std::move(o),
                                   [this, generation, id](bool ok, const std::wstring& text) {
                                       OnEditorDone(generation, id, ok, text);
                                   });
    }
    if (!e) {
        Log(ScriptLogLevel::Warn, L"popup.editText: the editor could not be opened");
        Check(engine_->Settle(id, false, "\"popup.editText: the editor could not be opened\""), "an editText callback");
        return;
    }
    editor_ = e;
    Log(ScriptLogLevel::Info, L"popup.editText: the editor opened");
    client_.WidgetEditorChanged(*this, true);
}

void WidgetInstance::OnEditorDone(uint64_t generation, int id, bool ok, const std::wstring& text) {
    if (!editor_) return;  // dropped already (DropEditor clears the callback first)
    editor_ = nullptr;
    Log(ScriptLogLevel::Info, ok ? L"popup.editText: OK (" + std::to_wstring(text.size()) + L" characters)"
                                 : std::wstring(L"popup.editText: cancelled"));
    client_.WidgetEditorChanged(*this, false);
    if (generation != generation_ || state_ != State::Ok || !engine_) return;
    Check(engine_->Settle(id, true, ok ? JsonUtil::WideToUtf8(JsonUtil::Quote(text)) : std::string("null")),
          "an editText callback");
}

void WidgetInstance::DropEditor(bool tellClient) {
    editPending_.reset();
    if (!editor_) return;
    TextEditorWindow* e = editor_;
    editor_ = nullptr;
    e->Abandon();  // no callback; the window (and the object) go
    if (tellClient) client_.WidgetEditorChanged(*this, false);
}

HWND WidgetInstance::EditorHwnd() const { return editor_ ? editor_->Hwnd() : nullptr; }

bool WidgetInstance::CloseEditor(bool ok) {
    if (!editor_) return false;
    editor_->Finish(ok);
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

void WidgetInstance::PopupClosed(bool passive) {
    if (state_ != State::Ok || !engine_) return;
    Check(engine_->Call(ScriptEngine::Fn::OnPopupClose, passive ? "{\"passive\":true}" : "{\"passive\":false}"),
          FnName(ScriptEngine::Fn::OnPopupClose));
}

bool WidgetInstance::WatchFolder(std::wstring& id, std::wstring& folder) const {
    if (preview_ || !package_ || package_->source != Widgets::Package::Source::Folder || !package_->manifest.watch) return false;
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
        case Permission::Notifications: return package_->manifest.notifications;
        case Permission::Calendar: return package_->manifest.calendar;
        case Permission::Weather: return package_->manifest.weather;
        case Permission::Perf: return package_->manifest.perf;
        case Permission::Http:
        default: return package_->manifest.http;
    }
}

bool WidgetInstance::StorageGet(const std::string& key, std::string& json) {
    if (preview_) {
        // A preview's own writes first (an in-memory scratch store), then the real store (read only).
        if (const auto it = scratch_.find(key); it != scratch_.end()) {
            if (!it->second) return false;
            json = *it->second;
            return true;
        }
    }
    return runtime_.StorageGet(id_, key, json);
}

bool WidgetInstance::StorageSet(const std::string& key, const std::string& json, std::wstring& error) {
    if (key.empty() || key.size() > 256) {
        error = L"storage keys are 1 - 256 characters";
        return false;
    }
    if (preview_) {
        if (json.size() > WidgetRuntime::kMaxStorageBytes) {
            error = L"storage: the widget's store would pass 1 MB";
            return false;
        }
        scratch_[key] = json;  // never written to widget-data\<id>.json
        return true;
    }
    return runtime_.StorageSet(id_, key, json, error, this);
}

void WidgetInstance::StorageRemove(const std::string& key) {
    if (preview_) {
        scratch_[key] = std::nullopt;
        return;
    }
    runtime_.StorageRemove(id_, key, this);
}

void WidgetInstance::StorageChanged(const std::string& key) {
    if (state_ != State::Ok || !engine_) return;
    Check(engine_->Emit("storage", "{\"key\":" + JsonUtil::WideToUtf8(JsonUtil::Quote(JsonUtil::Utf8ToWide(key))) + "}"),
          "storage.onChange");
}

std::vector<std::string> WidgetInstance::StorageKeys() {
    std::vector<std::string> keys = runtime_.StorageKeys(id_);
    if (!preview_ || scratch_.empty()) return keys;
    std::erase_if(keys, [&](const std::string& k) { return scratch_.count(k) != 0; });
    for (const auto& [k, v] : scratch_) {
        if (v) keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

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
    if (preview_) {
        error = L"http: not available in preview";  // the engine rejects the promise with it
        return 0;
    }
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
    if (preview_) return true;  // nothing opens in a preview (the script gets false)
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

void WidgetInstance::ProviderChanged(const char* channel, const std::string& payload) {
    if (state_ != State::Ok || !engine_) return;
    const std::string what = std::string(channel) + ".onChange";
    Check(engine_->Emit(channel, payload), what.c_str());
}

namespace {

std::string J(const std::wstring& s) { return JsonUtil::WideToUtf8(JsonUtil::Quote(s)); }
std::string Num(double v) { return JsonUtil::WideToUtf8(JsonUtil::Num(v)); }
const char* Bool(bool b) { return b ? "true" : "false"; }

// The keys shell.sendKeys may press: Windows' own UI and the media keys only (docs/widgets.md).
// Round 11 phase 11B: a fixed list on purpose, not a parser of "modifier+key" strings: a widget with
// the "shell" permission must not be able to type Ctrl+C / Alt+F4 into whatever has the focus. Up to
// three keys (the modifiers first, held while the last one is pressed).
constexpr size_t kMaxComboKeys = 3;
struct KeyCombo {
    const wchar_t* name;
    WORD keys[kMaxComboKeys];  // pressed in order, released in reverse; 0 ends the list
};
constexpr KeyCombo kKeyCombos[] = {
    {L"win", {VK_LWIN, 0}},             // Start
    // Round 16 phase 16A: the Windows key after StartMenuPlacer::Place moved the Start menu's host
    // window beside the widget (the Start widget's Menu position); without a host it is just 'win'.
    {L"start-near", {VK_LWIN, 0}},
    {L"win+a", {VK_LWIN, 'A'}},         // quick settings
    {L"win+b", {VK_LWIN, 'B'}},         // the notification area (keyboard focus)
    {L"win+d", {VK_LWIN, 'D'}},         // show / hide the desktop
    {L"win+e", {VK_LWIN, 'E'}},         // File Explorer
    {L"win+i", {VK_LWIN, 'I'}},         // Settings
    {L"win+m", {VK_LWIN, 'M'}},         // minimize every window (round 11 phase 11B: Show desktop)
    {L"win+shift+m", {VK_LWIN, VK_SHIFT, 'M'}},  // restore what Win+M minimized (11B)
    {L"win+n", {VK_LWIN, 'N'}},         // the notification centre and calendar
    {L"win+s", {VK_LWIN, 'S'}},         // Search
    {L"win+tab", {VK_LWIN, VK_TAB}},    // Task view
    {L"win+v", {VK_LWIN, 'V'}},         // clipboard history
    {L"win+w", {VK_LWIN, 'W'}},         // Widgets
    {L"win+x", {VK_LWIN, 'X'}},         // the Start button's context menu
    {L"ctrl+shift+esc", {VK_CONTROL, VK_SHIFT, VK_ESCAPE}},  // Task Manager (11B)
    {L"volume-up", {VK_VOLUME_UP, 0}},
    {L"volume-down", {VK_VOLUME_DOWN, 0}},
    {L"volume-mute", {VK_VOLUME_MUTE, 0}},
    {L"media-play-pause", {VK_MEDIA_PLAY_PAUSE, 0}},
    {L"media-next", {VK_MEDIA_NEXT_TRACK, 0}},
    {L"media-previous", {VK_MEDIA_PREV_TRACK, 0}},
    {L"media-stop", {VK_MEDIA_STOP, 0}},
};

// Test hook (round 11 phase 11B): the environment variable WINDOCK_SENDKEYS_LOG of the dock process
// names a file that gets a line per accepted shell.sendKeys call ("<name>\t<virtual keys, hex>") and
// then NOTHING is pressed (a dry run), so a test can click the Start / Show desktop widgets without
// opening Start or minimizing the session's windows. False without the variable. Round 16 phase
// 16A: `extra` (start-near: the placer's shift "<dx> <dy>") is appended after another tab.
bool SendKeysDryRunOn() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"WINDOCK_SENDKEYS_LOG", path, MAX_PATH);
    return n != 0 && n < MAX_PATH;
}

bool SendKeysDryRun(const std::wstring& name, const WORD* keys, size_t count, const std::wstring& extra = {}) {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"WINDOCK_SENDKEYS_LOG", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::wstring line = name + L"\t";
    for (size_t i = 0; i < count; ++i) {
        wchar_t hex[8];
        swprintf_s(hex, i ? L" %02X" : L"%02X", keys[i]);
        line += hex;
    }
    if (!extra.empty()) line += L"\t" + extra;
    const std::string bytes = JsonUtil::WideToUtf8(line) + "\r\n";
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        CloseHandle(f);
    }
    return true;  // a dry run even when the file cannot be written: never press keys under a test
}

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
    if (preview_) return true;  // nothing is pressed in a preview (the script gets false)
    size_t count = 0;
    while (count < kMaxComboKeys && combo->keys[count]) ++count;
    // Round 16 phase 16A: 'start-near' first moves the Start menu's host window beside the widget's
    // box (the box and side WidgetEditorPlace gives the text editor: the dock's popup side of the
    // slot, a standalone widget's window), then presses the key, so the menu opens in place.
    StartMenuPlacer* placer = nullptr;
    std::wstring extra;
    if (n == L"start-near") {
        placer = client_.WidgetStartPlacer();
        const Client::EditorPlace place = client_.WidgetEditorPlace(*this);
        if (placer && place.owner) {
            const HMONITOR monitor = MonitorFromRect(&place.box, MONITOR_DEFAULTTONEAREST);
            const bool dry = SendKeysDryRunOn();
            placer->Place(place.box, place.side, monitor, dry);
            if (dry) {
                const LPARAM s = placer->LastShift();
                extra = std::to_wstring(static_cast<int16_t>(LOWORD(s))) + L" " + std::to_wstring(static_cast<int16_t>(HIWORD(s)));
            }
        } else {
            placer = nullptr;
        }
    }
    if (SendKeysDryRun(n, combo->keys, count, extra)) {
        if (placer) placer->Restore();  // nothing opens: nothing may stay shifted (Place moved nothing)
        Log(ScriptLogLevel::Info, L"shell.sendKeys " + n + L" (dry run: WINDOCK_SENDKEYS_LOG)");
        return true;
    }
    INPUT in[2 * kMaxComboKeys]{};
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
        if (placer) placer->Restore();  // 16A: the menu does not open; the script opens All apps instead
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
    if (preview_) {
        json = WidgetSamples::AudioJson();
        return true;
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
    if (preview_) return true;  // changes nothing
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
    if (preview_) {
        json = WidgetSamples::MediaJson();
        return true;
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
    if (preview_) return true;  // sends nothing
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
    token.clear();
    if (preview_) {
        token = kArtToken + std::to_string(WidgetSamples::kArtSerial);
        return true;
    }
    const MediaService* m = runtime_.UseMedia(this);
    if (m && m->Get().art && m->Get().artSerial) token = kArtToken + std::to_string(m->Get().artSerial);
    return true;
}

// --- round 4 phase 9: notifications ------------------------------------------------------------

bool WidgetInstance::NotificationsGet(std::string& json, std::wstring& error) {
    if (!Permitted(Permission::Notifications)) {
        error = L"notifications needs the \"notifications\" permission";
        return false;
    }
    if (preview_) {
        json = WidgetSamples::NotificationsJson();
        return true;
    }
    const NotificationService* n = runtime_.UseNotifications(this);
    if (!n) {
        error = L"notifications: the notification service is not available";
        return false;
    }
    static const char* const kStates[] = {"off", "polling", "denied"};
    json = std::string("{\"state\":\"") + kStates[static_cast<int>(n->GetState())] + "\",\"reason\":" + J(n->StateReason()) +
           ",\"unread\":" + std::to_string(n->Unread()) + ",\"items\":[";
    bool first = true;
    for (const NotificationItem& it : n->Items()) {
        json += first ? "{" : ",{";
        first = false;
        json += "\"id\":" + std::to_string(it.id) + ",\"app\":" + J(it.app) + ",\"appKey\":" + J(it.key.id) +
                ",\"aumid\":" + J(it.aumid) + ",\"title\":" + J(it.title) + ",\"body\":" + J(it.body) +
                ",\"time\":" + std::to_string(it.time) + ",\"unread\":" + Bool(it.unread) + ",\"balloon\":" +
                Bool(it.balloon) + "}";
    }
    json += "]}";
    return true;
}

bool WidgetInstance::NotificationsCommand(const std::string& command, double id, std::wstring& error) {
    if (!Permitted(Permission::Notifications)) {
        error = L"notifications needs the \"notifications\" permission";
        return false;
    }
    if (preview_) return true;  // dismisses / activates / opens nothing
    NotificationService* n = runtime_.UseNotifications(this);
    if (!n) {
        error = L"notifications: the notification service is not available";
        return false;
    }
    const uint32_t nid = id >= 0.0 && id <= 4294967295.0 ? static_cast<uint32_t>(id) : 0;
    if (command == "dismiss") {
        n->Dismiss(nid);
    } else if (command == "dismissAll") {
        n->DismissAll();
    } else if (command == "markRead") {
        n->MarkRead(nid);
    } else if (command == "activate") {
        if (!client_.WidgetActivateNotification(nid)) {
            error = L"notifications.activate: no such notification";
            return false;
        }
    } else if (command == "openCentre") {
        if (!client_.WidgetOpenCentre(*this)) {
            error = L"notifications.openCentre: the dock cannot show its notification centre";
            return false;
        }
    } else {
        error = L"notifications: unknown command";
        return false;
    }
    Log(ScriptLogLevel::Info, L"notifications." + JsonUtil::Utf8ToWide(command));
    return true;
}

// --- round 4 phase 13: calendar ------------------------------------------------------------------

namespace {

// A date argument: ms since 1970 (UTC; its local day), or "YYYY-MM-DD" (a local day). False if absent
// or malformed. `day`: the local day; `utc`: the instant (a day: its local midnight).
bool CalendarDate(const json::JsonObject& o, const wchar_t* key, const CalendarService& c, int64_t& day, int64_t& utc) {
    const json::IJsonValue v = JsonUtil::Find(o, key);
    if (!v) return false;
    if (v.ValueType() == json::JsonValueType::Number) {
        const double ms = v.GetNumber();
        if (!std::isfinite(ms) || std::fabs(ms) > 8.0e15) return false;
        utc = static_cast<int64_t>(std::floor(ms / 1000.0));
        day = Ics::DayOf(c.LocalWall(utc));
        return true;
    }
    if (v.ValueType() != json::JsonValueType::String) return false;
    const std::wstring s(v.GetString());
    int y = 0, m = 0, d = 0;
    if (s.size() != 10 || swscanf_s(s.c_str(), L"%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 ||
        d > Ics::DaysInMonth(y, m) || y < 1601 || y > 9999) {
        return false;
    }
    day = Ics::DaysFromCivil(y, m, d);
    utc = c.LocalToUtc(day * 86400);
    return true;
}

}  // namespace

bool WidgetInstance::CalendarCall(const std::string& what, const std::string& args, std::string& out, std::wstring& error) {
    if (!Permitted(Permission::Calendar)) {
        error = L"calendar needs the \"calendar\" permission";
        return false;
    }
    if (preview_) return WidgetSamples::CalendarCall(what, args, out, error);
    CalendarService* c = runtime_.UseCalendar(this);
    if (!c) {
        error = L"calendar: the calendar service is not available";
        return false;
    }
    json::JsonObject o{nullptr};
    try {
        if (!json::JsonObject::TryParse(JsonUtil::Utf8ToWide(args), o) || !o) o = json::JsonObject();
    } catch (...) {
        o = json::JsonObject();
    }
    const auto& items = c->Items();
    auto list = [&](const std::vector<size_t>& indexes) {
        out = "[";
        for (size_t k = 0; k < indexes.size(); ++k) out += (k ? "," : "") + c->EventJson(items[indexes[k]]);
        out += "]";
    };
    if (what == "feeds") {
        out = c->FeedsJson();
        return true;
    }
    if (what == "eventsOn") {
        int64_t day = CalendarService::Today(), utc = 0;
        if (JsonUtil::Find(o, L"date") && !CalendarDate(o, L"date", *c, day, utc)) {
            error = L"calendar.eventsOn: the date must be a Date, ms since 1970 or \"YYYY-MM-DD\"";
            return false;
        }
        c->EnsureDays(day, day);
        list(c->On(day));
        return true;
    }
    if (what == "events") {
        // Default: from now to the end of the expansion window.
        int64_t fromDay = 0, toDay = 0, from = CalendarService::Now(), to = 0;
        if (JsonUtil::Find(o, L"from") && !CalendarDate(o, L"from", *c, fromDay, from)) {
            error = L"calendar.events: from must be a Date, ms since 1970 or \"YYYY-MM-DD\"";
            return false;
        }
        fromDay = Ics::DayOf(c->LocalWall(from));
        if (JsonUtil::Find(o, L"to")) {
            if (!CalendarDate(o, L"to", *c, toDay, to)) {
                error = L"calendar.events: to must be a Date, ms since 1970 or \"YYYY-MM-DD\"";
                return false;
            }
        } else {
            toDay = c->WindowTo() + 1;
            to = c->LocalToUtc(toDay * 86400);
        }
        if (to <= from) {
            out = "[]";
            return true;
        }
        if (toDay - fromDay > 1100) {
            error = L"calendar.events: at most about three years at a time";
            return false;
        }
        c->EnsureDays(fromDay, toDay);
        std::vector<size_t> hits;
        const auto& all = c->Items();
        for (size_t i = 0; i < all.size() && hits.size() < 2000; ++i) {
            const Ics::Occurrence& oc = all[i].occ;
            const bool inside = oc.end > oc.start ? (oc.end > from && oc.start < to) : (oc.start >= from && oc.start < to);
            if (inside) hits.push_back(i);
        }
        list(hits);
        return true;
    }
    if (what == "next") {
        double n = 5.0;
        JsonUtil::ReadNumber(o, L"n", n);
        const size_t count = static_cast<size_t>(std::clamp(std::isfinite(n) ? n : 5.0, 1.0, 100.0));
        const int64_t now = CalendarService::Now();
        std::vector<size_t> hits;
        for (size_t i = 0; i < items.size() && hits.size() < count; ++i) {
            const Ics::Occurrence& oc = items[i].occ;
            if (oc.end > now || (oc.end <= oc.start && oc.start >= now)) hits.push_back(i);
        }
        list(hits);
        return true;
    }
    if (what == "refresh") {
        c->RefreshNow();
        out = "null";
        return true;
    }
    if (what == "openPopup") {
        int64_t day = INT64_MIN, utc = 0;
        if (JsonUtil::Find(o, L"date") && !CalendarDate(o, L"date", *c, day, utc)) {
            error = L"calendar.openPopup: the date must be a Date, ms since 1970 or \"YYYY-MM-DD\"";
            return false;
        }
        PopupCalendar(day, error);
        out = "null";
        return true;
    }
    error = L"calendar: unknown call";
    return false;
}

// --- weather (round 4 phase 14) ----------------------------------------------------------------

namespace {

std::string NumOrNull(double v) {
    if (!std::isfinite(v)) return "null";
    return JsonUtil::WideToUtf8(JsonUtil::Num(std::round(v * 100.0) / 100.0));
}

const char* GlyphName(WeatherGlyphs::Glyph g) {
    using G = WeatherGlyphs::Glyph;
    switch (g) {
        case G::Clear: return "clear";
        case G::PartlyCloudy: return "partly-cloudy";
        case G::Overcast: return "overcast";
        case G::Fog: return "fog";
        case G::Drizzle: return "drizzle";
        case G::Rain: return "rain";
        case G::Snow: return "snow";
        case G::Thunder: return "thunder";
        case G::SetLocation: return "set-location";
        default: return "overcast";
    }
}

// Days since 1970-01-01 -> "YYYY-MM-DD" (H. Hinnant's civil_from_days).
std::string DayText(int64_t days) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    const int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    char buf[24];
    snprintf(buf, sizeof buf, "%04lld-%02d-%02d", static_cast<long long>(y), m, d);
    return buf;
}

const char* StateName(WeatherService::Status s) {
    switch (s) {
        case WeatherService::Status::Stale: return "stale";
        case WeatherService::Status::Fresh: return "fresh";
        case WeatherService::Status::Error: return "error";
        case WeatherService::Status::None:
        default: return "none";
    }
}

}  // namespace

std::string WidgetInstance::WeatherJson(const WeatherService& w, const WeatherConfig& place) {
    const WeatherConfig& cfg = w.Settings();
    const WeatherService::Forecast& f = w.Data();
    const bool imperial = cfg.units == Units::Imperial;
    std::string o = "{\"state\":\"" + std::string(StateName(w.State())) + "\"";
    o += ",\"configured\":" + std::string(Bool(cfg.Configured()));
    o += ",\"place\":" + J(place.location.empty() ? cfg.location : place.location);
    o += ",\"label\":" + J(place.label);
    o += ",\"latitude\":" + NumOrNull(cfg.latitude) + ",\"longitude\":" + NumOrNull(cfg.longitude);
    o += std::string(",\"units\":\"") + (imperial ? "imperial" : "metric") + "\"";
    o += std::string(",\"temperatureUnit\":\"") + (imperial ? "F" : "C") + "\"";
    o += std::string(",\"windUnit\":\"") + (imperial ? "mph" : "km/h") + "\"";
    o += ",\"refreshMinutes\":" + std::to_string(cfg.refreshMinutes);
    o += ",\"busy\":" + std::string(Bool(w.Busy()));
    o += ",\"error\":" + J(w.LastError());
    o += ",\"status\":" + J(w.StatusText());
    o += ",\"age\":" + std::to_string(w.Active() ? w.Age() : -1);
    o += ",\"fetchedAt\":" + (f.valid && f.fetchedAt > 0 ? std::to_string(f.fetchedAt * 1000) : std::string("0"));
    o += ",\"updated\":" + J(f.valid && f.fetchedAt > 0 ? WeatherService::FormatTime(f.fetchedAt) : std::wstring());
    if (f.valid) {
        o += ",\"current\":{\"temp\":" + NumOrNull(f.temp) + ",\"code\":" + std::to_string(f.code) +
             ",\"isDay\":" + Bool(f.isDay) + ",\"description\":" + J(WeatherGlyphs::Describe(f.code)) +
             ",\"glyph\":\"" + GlyphName(WeatherGlyphs::FromWmo(f.code)) + "\",\"wind\":" + NumOrNull(f.wind) +
             ",\"humidity\":" + NumOrNull(f.humidity) + "}";
    } else {
        o += ",\"current\":null";
    }
    o += ",\"hourly\":[";
    const std::vector<WeatherService::Hour> hours = w.HourlyWindow(48);
    for (size_t i = 0; i < hours.size(); ++i) {
        const WeatherService::Hour& h = hours[i];
        const int64_t utcMs = (h.key * 60 - f.utcOffset) * 1000;
        o += (i ? ",{" : "{") + std::string("\"time\":") + std::to_string(utcMs) + ",\"hour\":" + std::to_string(h.hour) +
             ",\"temp\":" + NumOrNull(h.temp) + ",\"code\":" + std::to_string(h.code) + ",\"isDay\":" + Bool(h.isDay) +
             ",\"precip\":" + NumOrNull(h.precip) + "}";
    }
    o += "],\"daily\":[";
    const std::vector<WeatherService::Day> days = w.DailyWindow(7);
    for (size_t i = 0; i < days.size(); ++i) {
        const WeatherService::Day& d = days[i];
        o += (i ? ",{" : "{") + std::string("\"day\":\"") + DayText(d.day) + "\",\"weekday\":" +
             std::to_string((d.weekday + 1) % 7) + ",\"code\":" + std::to_string(d.code) +
             ",\"description\":" + J(WeatherGlyphs::Describe(d.code)) + ",\"min\":" + NumOrNull(d.tmin) +
             ",\"max\":" + NumOrNull(d.tmax) + "}";
    }
    o += "]}";
    return o;
}

bool WidgetInstance::WeatherCall(const std::string& what, const std::string& args, std::string& out, std::wstring& error) {
    if (!Permitted(Permission::Weather)) {
        error = L"weather needs the \"weather\" permission";
        return false;
    }
    // A preview: the sample forecast; refresh / openSettings do nothing (icon below is pure).
    WeatherService* w = preview_ ? nullptr : runtime_.UseWeather(this);
    if (!w && !preview_) {
        error = L"weather: the weather service is not available";
        return false;
    }
    if (what == "get") {
        out = preview_ ? WidgetSamples::WeatherJson(weather_.units == Units::Imperial, weather_.label) : WeatherJson(*w, weather_);
        return true;
    }
    if (preview_ && (what == "refresh" || what == "openSettings")) {
        out = "null";
        return true;
    }
    if (what == "refresh") {
        runtime_.WeatherRequest(WidgetRuntime::WeatherAction::Refresh, weather_);  // posted: never inside this call
        out = "null";
        return true;
    }
    if (what == "openSettings") {
        RequestPopup(PopupRequest{PopupRequest::Kind::WeatherSettings});  // the dock opens this item's settings
        out = "null";
        return true;
    }
    if (what == "icon") {
        json::JsonObject o{nullptr};
        try {
            if (!json::JsonObject::TryParse(JsonUtil::Utf8ToWide(args), o) || !o) o = json::JsonObject();
        } catch (...) {
            o = json::JsonObject();
        }
        std::wstring name, ink, water;
        double code = -1.0;
        bool isDay = true;
        const json::IJsonValue c = JsonUtil::Find(o, L"code");
        if (c && c.ValueType() == json::JsonValueType::String) {
            name = c.GetString();
        } else {
            JsonUtil::ReadNumber(o, L"code", code);
        }
        JsonUtil::ReadBool(o, L"isDay", isDay);
        JsonUtil::ReadString(o, L"ink", ink);
        JsonUtil::ReadString(o, L"water", water);
        uint32_t inkArgb = 0xF2FFFFFF, waterArgb = 0xFF4CC2FF;
        if (!ink.empty() && !ParseArgbColor(ink, inkArgb)) {
            error = L"weather.icon: ink must be a colour (\"#AARRGGBB\")";
            return false;
        }
        if (!water.empty() && !ParseArgbColor(water, waterArgb)) {
            error = L"weather.icon: water must be a colour (\"#AARRGGBB\")";
            return false;
        }
        char colours[40];
        snprintf(colours, sizeof colours, "/%08X/%08X", inkArgb, waterArgb);
        if (name == L"refresh") {
            char one[24];
            snprintf(one, sizeof one, "/%08X", inkArgb);
            out = std::string("\"weather:refresh") + one + "\"";
            return true;
        }
        std::string glyph;
        if (name == L"setLocation" || name == L"set-location") {
            glyph = "set-location";
        } else if (!name.empty()) {
            error = L"weather.icon: the code must be a WMO weather code, \"setLocation\" or \"refresh\"";
            return false;
        } else {
            glyph = GlyphName(WeatherGlyphs::FromWmo(std::isfinite(code) ? static_cast<int>(code) : -1));
        }
        out = "\"weather:icon/" + glyph + (isDay ? "/d" : "/n") + colours + "\"";
        return true;
    }
    error = L"weather: unknown call";
    return false;
}

// --- round 6 phase 6D: perf ------------------------------------------------------------------------
// Phase 6E (preview mode) answers these two from sample data (WidgetSamples) instead of the service.

bool WidgetInstance::PerfCall(const std::string& what, double number, std::string& json, std::wstring& error) {
    if (!Permitted(Permission::Perf)) {
        error = L"perf needs the \"perf\" permission";
        return false;
    }
    if (preview_) {
        if (what == "get") {
            json = number == 1.0 ? std::string("null") : WidgetSamples::PerfSnapshotJson();  // serial 1
        } else if (what == "metrics") {
            json = number == 1.0 ? std::string("null") : WidgetSamples::PerfMetricsJson();
        } else if (what == "hwinfo") {
            json = WidgetSamples::PerfHwinfoJson();
        } else if (what == "use") {
            json = "null";
        } else if (what == "setInterval") {
            json = "1000";  // the sampler is not started
        } else {
            error = L"perf: unknown call";
            return false;
        }
        return true;
    }
    PerfService* perf = runtime_.UsePerf(this);
    if (!perf) {
        error = L"perf: the sampler could not start";
        return false;
    }
    if (what == "get") {
        // "null" when the script has this snapshot already (the prelude keeps its object).
        if (number == static_cast<double>(perf->Snapshots())) {
            json = "null";
        } else {
            json = perf->SnapshotJson();
        }
        return true;
    }
    if (what == "metrics") {
        if (number == static_cast<double>(perf->CatalogSerial())) {
            json = "null";
        } else {
            json = perf->MetricsJson();
        }
        return true;
    }
    if (what == "hwinfo") {
        json = perf->HwinfoJson();
        return true;
    }
    if (what == "use") {
        json = "null";
        return true;
    }
    if (what == "setInterval") {
        if (!std::isfinite(number)) {
            error = L"perf.setInterval: the interval must be a number of ms (250 - 10000)";
            return false;
        }
        const double ms = std::clamp(std::round(number), static_cast<double>(PerfService::kMinInterval),
                                     static_cast<double>(PerfService::kMaxInterval));
        runtime_.SetPerfInterval(this, static_cast<uint32_t>(ms));
        json = std::to_string(perf->Interval());
        return true;
    }
    error = L"perf: unknown call";
    return false;
}

bool WidgetInstance::PerfSeries(const std::string& key, size_t n, std::vector<double>& out, std::wstring& error) {
    if (!Permitted(Permission::Perf)) {
        error = L"perf needs the \"perf\" permission";
        return false;
    }
    if (preview_) {
        WidgetSamples::PerfSeries(key, std::min(n, PerfService::kHistory), out);
        return true;
    }
    PerfService* perf = runtime_.UsePerf(this);
    if (!perf) {
        error = L"perf: the sampler could not start";
        return false;
    }
    perf->Series(key, std::min(n, PerfService::kHistory), out);
    return true;
}
