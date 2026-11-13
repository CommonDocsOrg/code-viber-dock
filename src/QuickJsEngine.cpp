// The QuickJS-ng implementation of ScriptEngine (ScriptEngine.h): one runtime + one context per
// widget instance, on the UI thread. Binds the host API of docs/widgets.md (canvas, dock, timer,
// console, system, settings) and calls the widget module's exports under a watchdog.
//
// Rules from the 2026-11-02 spike (ROADMAP Phase 4), all kept here:
// - every native callback is noexcept and turns a C++ exception into a JS throw (an exception that
//   crosses QuickJS's C frames would end the process): Guard<> below;
// - JS_SetMaxStackSize well inside the exe's 8 MB stack (CMakeLists /STACK), with the stack top
//   refreshed on every entry (JS_UpdateStackTop);
// - the module loader is sandboxed: Widgets::ResolvePath rejects absolute paths, drive letters,
//   URLs and ".." leaving the package, and Package::Read checks the file actually opened;
// - the interrupt handler is the watchdog: every call into the script (with the jobs it queues)
//   gets timeLimitMs; past it the script is stopped and the widget suspended;
// - JS_SetMemoryLimit per runtime (an exhausted heap can throw `null` rather than an Error);
// - setTimeout / console / local time come from the host; scripts are always source (bytecode is
//   never loaded).

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <map>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#pragma warning(push, 0)
#include "quickjs.h"
#pragma warning(pop)

#include "JsonUtil.h"
#include "ScriptEngine.h"
#include "WidgetCanvas.h"
#include "WidgetPackage.h"
#include "WidgetRuntime.h"

namespace {

using JsonUtil::Utf8ToWide;
using JsonUtil::WideToUtf8;

std::string ToStd(JSContext* ctx, JSValueConst v) {
    size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, v);
    if (!s) return {};
    std::string r(s, len);
    JS_FreeCString(ctx, s);
    return r;
}

std::wstring ToWide(JSContext* ctx, JSValueConst v) { return Utf8ToWide(ToStd(ctx, v)); }

JSValue NewString(JSContext* ctx, const std::wstring& s) {
    const std::string u = WideToUtf8(s);
    return JS_NewStringLen(ctx, u.data(), u.size());
}

JSValue NewString(JSContext* ctx, const std::string& s) { return JS_NewStringLen(ctx, s.data(), s.size()); }

std::string ArgbText(uint32_t argb) {
    char buf[16];
    snprintf(buf, sizeof(buf), "#%08X", argb);
    return buf;
}

// Every native function goes through this: exceptions never reach QuickJS's frames.
template <JSValue (*F)(JSContext*, JSValueConst, int, JSValueConst*)>
JSValue Guard(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) noexcept {
    try {
        return F(ctx, self, argc, argv);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(ctx);
    } catch (...) {
        return JS_ThrowInternalError(ctx, "host error");
    }
}

template <JSValue (*F)(JSContext*, JSValueConst, int)>
JSValue GuardGet(JSContext* ctx, JSValueConst self, int magic) noexcept {
    try {
        return F(ctx, self, magic);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(ctx);
    } catch (...) {
        return JS_ThrowInternalError(ctx, "host error");
    }
}

// Argument access that remembers a failed conversion (a pending JS exception).
struct Args {
    JSContext* ctx;
    int argc;
    JSValueConst* argv;
    bool failed = false;

    bool Has(int i) const { return i < argc && !JS_IsUndefined(argv[i]); }
    JSValueConst At(int i) const { return i < argc ? argv[i] : JS_UNDEFINED; }
    float F(int i, float def = 0.0f) {
        if (!Has(i)) return def;
        double d = 0;
        if (JS_ToFloat64(ctx, &d, argv[i])) {
            failed = true;
            return def;
        }
        return static_cast<float>(d);
    }
    bool B(int i, bool def = false) {
        if (!Has(i)) return def;
        const int r = JS_ToBool(ctx, argv[i]);
        if (r < 0) {
            failed = true;
            return def;
        }
        return r != 0;
    }
    std::wstring S(int i) {
        if (!Has(i)) return {};
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!s) {
            failed = true;
            return {};
        }
        std::wstring r = Utf8ToWide(std::string(s, len));
        JS_FreeCString(ctx, s);
        return r;
    }
};

// obj[key] as a number / string / bool (absent: unchanged). False on a pending exception.
bool PropF(JSContext* ctx, JSValueConst obj, const char* key, float& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsException(v)) return false;
    bool ok = true;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v)) {
            ok = false;
        } else {
            out = static_cast<float>(d);
        }
    }
    JS_FreeValue(ctx, v);
    return ok;
}

bool PropS(JSContext* ctx, JSValueConst obj, const char* key, std::string& out, bool* present = nullptr) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsException(v)) return false;
    bool ok = true;
    if (present) *present = !JS_IsUndefined(v) && !JS_IsNull(v);
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, v);
        if (!s) {
            ok = false;
        } else {
            out.assign(s, len);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);
    return ok;
}

bool PropB(JSContext* ctx, JSValueConst obj, const char* key, bool& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsException(v)) return false;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) out = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    return true;
}

class QuickJsEngine;
QuickJsEngine* Self(JSContext* ctx) { return static_cast<QuickJsEngine*>(JS_GetContextOpaque(ctx)); }

// JS prelude: the bits of the API that are plain JavaScript over the native functions
// (console's formatting, gradient objects). `__windock` is removed again.
constexpr char kPrelude[] = R"JS(
(function (g, n) {
  'use strict';
  const fmt = (v) => {
    if (typeof v === 'string') return v;
    if (v instanceof Error) return v.name + ': ' + v.message + (v.stack ? '\n' + v.stack.replace(/\s+$/, '') : '');
    if (typeof v === 'object' && v !== null) { try { return JSON.stringify(v); } catch (e) { return String(v); } }
    return String(v);
  };
  const out = (level) => (...a) => n.log(level, a.map(fmt).join(' '));
  g.console = Object.freeze({ log: out(0), info: out(0), debug: out(0), warn: out(1), error: out(2) });
  const c = n.canvas;
  c.linearGradient = (x0, y0, x1, y1, stops) => Object.freeze({ type: 'linear', x0, y0, x1, y1, stops });
  c.radialGradient = (cx, cy, rx, ry, stops, fx, fy) =>
    Object.freeze({ type: 'radial', cx, cy, rx, ry: ry ?? rx, stops, fx: fx ?? 0, fy: fy ?? 0 });
  Object.freeze(c);
  // http: headers as an object, a string body as text, any other body as JSON.
  const headerText = (h) => {
    let s = '';
    for (const [k, v] of Object.entries(h ?? {})) {
      const name = String(k), value = String(v);
      if (!/^[A-Za-z0-9!#$%&'*+.^_`|~-]+$/.test(name) || /[\r\n]/.test(value)) throw new TypeError('http: bad header ' + name);
      s += name + ': ' + value + '\r\n';
    }
    return s;
  };
  const send = (method, url, body, o) => {
    o = o ?? {};
    const headers = Object.assign({}, o.headers);
    const typed = Object.keys(headers).some((k) => k.toLowerCase() === 'content-type');
    let data = '';
    if (body !== undefined && body !== null) {
      if (typeof body === 'string') {
        data = body;
        if (!typed) headers['Content-Type'] = 'text/plain; charset=utf-8';
      } else {
        data = JSON.stringify(body);
        if (!typed) headers['Content-Type'] = 'application/json';
      }
    }
    return n.http(String(method).toUpperCase(), String(url), headerText(headers), data, Number(o.timeout ?? 10000), o.type === 'json');
  };
  g.http = Object.freeze({
    get: (url, o) => send('GET', url, undefined, o),
    post: (url, body, o) => send('POST', url, body, o),
    request: (o) => send(o?.method ?? 'GET', o?.url, o?.body, o),
  });
  // widget: the package's own files (faces, data, translations).
  const w = n.widget;
  g.widget = Object.freeze({
    files: (pattern) => w.files(pattern),
    read: (path) => w.read(path),
    resize: (width, height) => w.resize(Number(width), Number(height)),
    readJson: (path) => {
      const text = w.read(path);
      try { return JSON.parse(text); } catch (e) { throw new SyntaxError(String(path) + ': ' + e.message); }
    },
  });
  // Providers: onChange listeners kept here; the host calls the returned emit(channel).
  const listeners = { audio: [], media: [], notifications: [], calendar: [], calendarReminder: [], weather: [], storage: [], perf: [] };
  const onChange = (ch) => (cb) => {
    if (typeof cb !== 'function') throw new TypeError(ch + '.onChange: the argument must be a function');
    listeners[ch].push(cb);
    return () => { const i = listeners[ch].indexOf(cb); if (i >= 0) listeners[ch].splice(i, 1); };
  };
  const p = n.providers;
  g.audio = Object.freeze({
    get: () => p.audioGet(),
    setVolume: (v) => p.audioSet('volume', Number(v)),
    setMuted: (b) => p.audioSet('muted', b ? 1 : 0),
    setMicMuted: (b) => p.audioSet('micMuted', b ? 1 : 0),
    setDevice: (id) => p.audioSet('device', 0, String(id)),
    onChange: onChange('audio'),
  });
  g.media = Object.freeze({
    get: () => p.mediaGet(),
    art: () => p.mediaArt(),
    play: () => p.mediaCmd('play'),
    pause: () => p.mediaCmd('pause'),
    toggle: () => p.mediaCmd('toggle'),
    next: () => p.mediaCmd('next'),
    previous: () => p.mediaCmd('previous'),
    seek: (s) => p.mediaSeek(Number(s)),
    onChange: onChange('media'),
  });
  // notifications (phase 9): get() the list (newest first), count() the unread ones.
  g.notifications = Object.freeze({
    get: () => p.notifGet().items,
    state: () => { const s = p.notifGet(); return { state: s.state, reason: s.reason }; },
    count: () => p.notifGet().unread,
    countFor: (key) => {
      const k = String(key ?? '').toLowerCase();
      return p.notifGet().items.filter((n) => n.unread && n.appKey === k).length;
    },
    dismiss: (id) => p.notifCmd('dismiss', Number(id)),
    dismissAll: () => p.notifCmd('dismissAll', 0),
    markRead: (id) => p.notifCmd('markRead', Number(id)),
    activate: (id) => p.notifCmd('activate', Number(id)),
    openCentre: () => p.notifCmd('openCentre', 0),
    onChange: onChange('notifications'),
  });
  // calendar (phase 13): the dock's ICS feeds. Dates: a Date, ms since 1970 (UTC) or "YYYY-MM-DD"
  // (a local day).
  const day = (v) => (v instanceof Date ? v.getTime() : v);
  const cal = (what, a) => p.cal(what, JSON.stringify(a ?? {}));
  g.calendar = Object.freeze({
    feeds: () => cal('feeds'),
    events: (o) => cal('events', { from: day(o?.from), to: day(o?.to) }),
    eventsOn: (d) => cal('eventsOn', { date: day(d) }),
    today: () => cal('eventsOn', {}),
    next: (n) => cal('next', { n: n === undefined ? 5 : Number(n) }),
    refresh: () => cal('refresh'),
    openPopup: (d) => cal('openPopup', { date: day(d) }),
    onChange: onChange('calendar'),
    onReminder: onChange('calendarReminder'),
  });
  // weather (phase 14): the dock's own forecast service (one fetcher for the native element and
  // every widget). icon(code, isDay, colours): an image token for canvas.image.
  const wx = (what, a) => p.weather(what, JSON.stringify(a ?? {}));
  g.weather = Object.freeze({
    get: () => wx('get'),
    refresh: () => wx('refresh'),
    openSettings: () => wx('openSettings'),
    icon: (code, isDay, o) => wx('icon', {
      code: typeof code === 'string' ? code : Number(code ?? -1), isDay: isDay !== false,
      ink: String(o?.ink ?? g.dock.theme.text), water: String(o?.water ?? g.dock.theme.accent),
    }),
    onChange: onChange('weather'),
  });
  // perf (round 6 phase 6D): the process's sampler. get() keeps one frozen object per snapshot and
  // metrics() one frozen list per catalog: the host sends a new one only when it has a newer one.
  let perfSnap = null, perfList = null, perfListSerial = -1;
  const perfGet = () => {
    const r = p.perf('get', perfSnap ? perfSnap.serial : -1);
    if (r !== null) {
      Object.freeze(r.metrics);
      perfSnap = Object.freeze(r);
    }
    return perfSnap;
  };
  const perfMetrics = () => {
    const r = p.perf('metrics', perfListSerial);
    if (r !== null) {
      for (const m of r.list) Object.freeze(m);
      perfList = Object.freeze(r.list);
      perfListSerial = r.serial;
    }
    return perfList;
  };
  const perfOnChange = (cb) => {
    const off = onChange('perf')(cb);
    p.perf('use', 0);  // the first listener starts the sampler, as get() does
    return off;
  };
  g.perf = Object.freeze({
    get: perfGet,
    metrics: perfMetrics,
    series: (key, n) => p.perfSeries(String(key), n === undefined ? 600 : Number(n)),
    setInterval: (ms) => p.perf('setInterval', Number(ms)),
    hwinfo: () => p.perf('hwinfo', 0),
    onChange: perfOnChange,
  });
  // storage.onChange (round 6 phase 6C): another item of this widget changed a key ({ key }).
  g.storage.onChange = onChange('storage');
  Object.freeze(g.storage);
  return (ch, data) => {
    const l = listeners[ch];
    if (!l || !l.length) return;
    const state = ch === 'audio' ? p.audioGet() : ch === 'media' ? p.mediaGet() : ch === 'calendar' ? cal('feeds')
      : ch === 'calendarReminder' || ch === 'storage' ? JSON.parse(data) : ch === 'weather' ? wx('get')
      : ch === 'perf' ? perfGet() : p.notifGet().items;
    for (const cb of l.slice()) cb(state);
  };
})(globalThis, globalThis.__windock);
)JS";

class QuickJsEngine final : public ScriptEngine {
public:
    QuickJsEngine(ScriptHost& host, const ScriptLimits& limits) : host_(host), limits_(limits) {}

    ~QuickJsEngine() override {
        if (!ctx_) {
            if (rt_) JS_FreeRuntime(rt_);
            return;
        }
        for (auto& [id, t] : timers_) JS_FreeValue(ctx_, t.fn);
        timers_.clear();
        for (auto& [id, p] : pending_) {
            JS_FreeValue(ctx_, p.resolve);
            JS_FreeValue(ctx_, p.reject);
        }
        pending_.clear();
        for (auto& [id, p] : settles_) {
            JS_FreeValue(ctx_, p.resolve);
            JS_FreeValue(ctx_, p.reject);
        }
        settles_.clear();
        for (JSValue& f : fns_) JS_FreeValue(ctx_, f);
        JS_FreeValue(ctx_, emit_);
        JS_FreeValue(ctx_, ns_);
        JS_FreeValue(ctx_, canvas_);
        JS_FreeValue(ctx_, dock_);
        JS_FreeValue(ctx_, settings_);
        JS_FreeContext(ctx_);
        JS_RunGC(rt_);
        JS_FreeRuntime(rt_);
    }

    bool Init() {
        rt_ = JS_NewRuntime();
        if (!rt_) return false;
        JS_SetRuntimeOpaque(rt_, this);
        JS_SetMemoryLimit(rt_, limits_.memoryLimit);
        JS_SetMaxStackSize(rt_, limits_.stackLimit);
        JS_SetInterruptHandler(rt_, &QuickJsEngine::Interrupt, this);
        JS_SetModuleLoaderFunc(rt_, &QuickJsEngine::Normalize, &QuickJsEngine::Loader, this);
        JS_SetHostPromiseRejectionTracker(rt_, &QuickJsEngine::Rejection, this);
        JS_NewClassID(rt_, &pathClass_);
        JSClassDef pathDef{};
        pathDef.class_name = "Path";
        pathDef.finalizer = &QuickJsEngine::PathFinalizer;
        if (JS_NewClass(rt_, pathClass_, &pathDef) < 0) return false;
        ctx_ = JS_NewContext(rt_);
        if (!ctx_) return false;
        JS_SetContextOpaque(ctx_, this);
        for (JSValue& f : fns_) f = JS_UNDEFINED;
        ns_ = canvas_ = dock_ = settings_ = JS_UNDEFINED;
        return InstallApi();
    }

    // --- ScriptEngine ---
    ScriptResult Load(const std::string& mainPath) override {
        ScriptResult res;
        Enter(std::max<uint32_t>(limits_.timeLimitMs * 4, 200));  // parsing + top-level code
        JSValue promise = JS_LoadModule(ctx_, "", mainPath.c_str(), JS_UNDEFINED);
        if (JS_IsException(promise)) {
            res = TakeException();
            Leave();
            return res;
        }
        Pump(res);
        if (res.Failed()) {
            JS_FreeValue(ctx_, promise);
            Leave();
            return res;
        }
        const JSPromiseStateEnum state = JS_PromiseState(ctx_, promise);
        JSValue value = JS_PromiseResult(ctx_, promise);
        JS_FreeValue(ctx_, promise);
        if (state == JS_PROMISE_REJECTED) {
            JS_Throw(ctx_, value);
            res = TakeException();
            Leave();
            return res;
        }
        if (state != JS_PROMISE_FULFILLED) {
            JS_FreeValue(ctx_, value);
            res.status = ScriptStatus::Threw;
            res.error = L"the main module did not finish loading (a top-level await that never settles)";
            Leave();
            return res;
        }
        ns_ = value;  // the module namespace
        static const char* const kNames[] = {"draw",      "measure",      "tick",         "onClick",
                                             "onMiddleClick", "onWheel", "onHover",     "onSettingsChanged",
                                             "onPress",   "onDrop",       "drawPopup",    "onPopupClick",
                                             "onPopupClose", "onVisibilityChange", "onDrag", "onRelease"};
        static_assert(std::size(kNames) == static_cast<size_t>(Fn::Count));
        for (size_t i = 0; i < std::size(kNames); ++i) {
            JSValue f = JS_GetPropertyStr(ctx_, ns_, kNames[i]);
            if (JS_IsException(f)) {
                JS_FreeValue(ctx_, JS_GetException(ctx_));
                f = JS_UNDEFINED;
            }
            if (!JS_IsFunction(ctx_, f)) {
                JS_FreeValue(ctx_, f);
                f = JS_UNDEFINED;
            }
            fns_[i] = f;
        }
        ReportRejections();
        Leave();
        return res;
    }

    bool Has(Fn fn) const override { return !JS_IsUndefined(fns_[static_cast<size_t>(fn)]); }

    ScriptResult Call(Fn fn, const std::string& eventJson) override {
        ScriptResult res;
        if (!Has(fn)) {
            res.status = ScriptStatus::Missing;
            return res;
        }
        JSValue args[2] = {JS_UNDEFINED, JS_DupValue(ctx_, dock_)};
        switch (fn) {
            case Fn::Draw:
            case Fn::Measure:
            case Fn::DrawPopup: args[0] = JS_DupValue(ctx_, canvas_); break;
            case Fn::OnSettingsChanged: args[0] = JS_DupValue(ctx_, settings_); break;
            case Fn::Tick: args[0] = JS_DupValue(ctx_, dock_); break;
            default:
                args[0] = eventJson.empty() ? JS_NewObject(ctx_)
                                            : JS_ParseJSON(ctx_, eventJson.data(), eventJson.size(), "<event>");
                if (JS_IsException(args[0])) {
                    JS_FreeValue(ctx_, JS_GetException(ctx_));
                    args[0] = JS_NewObject(ctx_);
                }
                break;
        }
        res = Invoke(fns_[static_cast<size_t>(fn)], 2, args);
        JS_FreeValue(ctx_, args[0]);
        JS_FreeValue(ctx_, args[1]);
        return res;
    }

    ScriptResult FireTimer(int id) override {
        ScriptResult res;
        auto it = timers_.find(id);
        if (it == timers_.end()) return res;
        JSValue fn = JS_DupValue(ctx_, it->second.fn);
        if (!it->second.repeat) {
            JS_FreeValue(ctx_, it->second.fn);
            timers_.erase(it);
        }
        res = Invoke(fn, 0, nullptr);
        JS_FreeValue(ctx_, fn);
        return res;
    }

    ScriptResult Resolve(int id, bool ok, const std::string& json) override {
        ScriptResult res;
        auto it = pending_.find(id);
        if (it == pending_.end()) return res;
        const Pending p = it->second;
        pending_.erase(it);
        JSValue arg = JS_UNDEFINED;
        bool fulfil = ok;
        if (ok) {
            arg = JS_ParseJSON(ctx_, json.data(), json.size(), "<http>");
            if (JS_IsException(arg)) {
                JS_FreeValue(ctx_, JS_GetException(ctx_));
                arg = JS_NewObject(ctx_);
            }
            // The body as JSON: asked for (type: "json"), or a JSON content type (then optional).
            std::string type;
            PropS(ctx_, arg, "contentType", type);
            const bool jsonType = type.find("json") != std::string::npos;
            if (p.wantJson || jsonType) {
                std::string text;
                PropS(ctx_, arg, "text", text);
                JSValue parsed = JS_ParseJSON(ctx_, text.data(), text.size(), "<http body>");
                if (JS_IsException(parsed)) {
                    JS_FreeValue(ctx_, JS_GetException(ctx_));
                    if (p.wantJson) {
                        JS_FreeValue(ctx_, arg);
                        arg = MakeError("http: the response is not JSON");
                        fulfil = false;
                    }
                } else {
                    JS_SetPropertyStr(ctx_, arg, "json", parsed);
                }
            }
        } else {
            std::string message = "http: the request failed";
            JSValue m = JS_ParseJSON(ctx_, json.data(), json.size(), "<http error>");
            if (JS_IsString(m)) message = ToStd(ctx_, m);
            if (JS_IsException(m)) JS_FreeValue(ctx_, JS_GetException(ctx_));
            JS_FreeValue(ctx_, m);
            arg = MakeError(message);
        }
        res = Invoke(fulfil ? p.resolve : p.reject, 1, &arg);
        JS_FreeValue(ctx_, arg);
        JS_FreeValue(ctx_, p.resolve);
        JS_FreeValue(ctx_, p.reject);
        return res;
    }

    ScriptResult Settle(int id, bool ok, const std::string& json) override {
        ScriptResult res;
        auto it = settles_.find(id);
        if (it == settles_.end()) return res;
        const Pending p = it->second;
        settles_.erase(it);
        JSValue arg = JS_UNDEFINED;
        if (ok) {
            arg = JS_ParseJSON(ctx_, json.data(), json.size(), "<result>");
            if (JS_IsException(arg)) {
                JS_FreeValue(ctx_, JS_GetException(ctx_));
                arg = JS_NULL;
            }
        } else {
            std::string message = "the request failed";
            JSValue m = JS_ParseJSON(ctx_, json.data(), json.size(), "<error>");
            if (JS_IsString(m)) message = ToStd(ctx_, m);
            if (JS_IsException(m)) JS_FreeValue(ctx_, JS_GetException(ctx_));
            JS_FreeValue(ctx_, m);
            arg = MakeError(message);
        }
        res = Invoke(ok ? p.resolve : p.reject, 1, &arg);
        JS_FreeValue(ctx_, arg);
        JS_FreeValue(ctx_, p.resolve);
        JS_FreeValue(ctx_, p.reject);
        return res;
    }

    void SettingsChanged() override {
        JSValue s = ParseSettings();
        JS_FreeValue(ctx_, settings_);
        settings_ = s;
        JSValue global = JS_GetGlobalObject(ctx_);
        JS_SetPropertyStr(ctx_, global, "settings", JS_DupValue(ctx_, settings_));
        JS_FreeValue(ctx_, global);
    }

    ScriptResult Emit(const std::string& channel, const std::string& payload) override {
        if (!JS_IsFunction(ctx_, emit_)) return ScriptResult{};
        JSValue args[2] = {NewString(ctx_, channel), NewString(ctx_, payload)};
        ScriptResult res = Invoke(emit_, 2, args);
        JS_FreeValue(ctx_, args[0]);
        JS_FreeValue(ctx_, args[1]);
        return res;
    }

    size_t HeapBytes() override {
        JSMemoryUsage mu{};
        JS_ComputeMemoryUsage(rt_, &mu);
        return static_cast<size_t>(std::max<int64_t>(0, mu.malloc_size));
    }

    // --- used by the native callbacks ---
    ScriptHost& host_;
    JSClassID pathClass_ = 0;

    // setTimeout / setInterval: the function kept here, the deadline in the host's scheduler.
    JSValue AddTimer(JSValueConst fn, uint32_t ms, bool repeat) {
        if (!JS_IsFunction(ctx_, fn)) return JS_ThrowTypeError(ctx_, "%s: the first argument must be a function",
                                                               repeat ? "setInterval" : "setTimeout");
        int id = nextTimer_;
        nextTimer_ = nextTimer_ >= 0x3FFFFFFF ? 1 : nextTimer_ + 1;
        if (!host_.ScheduleTimer(id, ms, repeat)) {
            return JS_ThrowRangeError(ctx_, "too many timers (64 per widget)");
        }
        Timer t;
        t.fn = JS_DupValue(ctx_, fn);
        t.repeat = repeat;
        if (auto old = timers_.find(id); old != timers_.end()) {
            JS_FreeValue(ctx_, old->second.fn);
            timers_.erase(old);
        }
        timers_.emplace(id, t);
        return JS_NewInt32(ctx_, id);
    }
    // http: the promise of request `id` (resolved / rejected by Resolve).
    JSValue AddRequest(int id, bool wantJson) {
        JSValue funcs[2] = {JS_UNDEFINED, JS_UNDEFINED};
        JSValue promise = JS_NewPromiseCapability(ctx_, funcs);
        if (JS_IsException(promise)) return promise;
        pending_[id] = Pending{funcs[0], funcs[1], wantJson};
        return promise;
    }
    // A host request's promise (popup.editText; settled by Settle(id, ...)).
    JSValue AddSettle(int id) {
        JSValue funcs[2] = {JS_UNDEFINED, JS_UNDEFINED};
        JSValue promise = JS_NewPromiseCapability(ctx_, funcs);
        if (JS_IsException(promise)) return promise;
        if (auto old = settles_.find(id); old != settles_.end()) {
            JS_FreeValue(ctx_, old->second.resolve);
            JS_FreeValue(ctx_, old->second.reject);
            settles_.erase(old);
        }
        settles_[id] = Pending{funcs[0], funcs[1], false};
        return promise;
    }
    // A promise already rejected with `error` (taken): an API that answers with a promise reports
    // a bad call through it rather than by throwing.
    JSValue Rejected(JSValue error) {
        JSValue funcs[2] = {JS_UNDEFINED, JS_UNDEFINED};
        JSValue promise = JS_NewPromiseCapability(ctx_, funcs);
        if (JS_IsException(promise)) {
            JS_FreeValue(ctx_, error);
            return promise;
        }
        JSValue r = JS_Call(ctx_, funcs[1], JS_UNDEFINED, 1, &error);
        if (JS_IsException(r)) JS_FreeValue(ctx_, JS_GetException(ctx_));
        JS_FreeValue(ctx_, r);
        JS_FreeValue(ctx_, error);
        JS_FreeValue(ctx_, funcs[0]);
        JS_FreeValue(ctx_, funcs[1]);
        return promise;
    }
    JSValue MakeError(const std::string& message) {
        JSValue e = JS_NewError(ctx_);
        JS_SetPropertyStr(ctx_, e, "message", NewString(ctx_, message));
        return e;
    }
    void ClearTimer(int id) {
        auto it = timers_.find(id);
        if (it == timers_.end()) return;
        JS_FreeValue(ctx_, it->second.fn);
        timers_.erase(it);
        host_.CancelTimer(id);
    }

private:
    struct Timer {
        JSValue fn = JS_UNDEFINED;
        bool repeat = false;
    };

    // --- calls and the watchdog ---
    void Enter(uint32_t limitMs) {
        JS_UpdateStackTop(rt_);  // the UI thread's stack, from wherever this call is made
        interrupted_ = false;
        deadline_ = WidgetNowMs() + static_cast<double>(limitMs);
        armed_ = true;
    }
    void Leave() { armed_ = false; }

    ScriptResult Invoke(JSValueConst fn, int argc, JSValueConst* argv) {
        ScriptResult res;
        Enter(limits_.timeLimitMs);
        JSValue r = JS_Call(ctx_, fn, JS_UNDEFINED, argc, argv);
        if (JS_IsException(r)) {
            res = TakeException();
        } else {
            if (!JS_IsUndefined(r) && !JS_IsFunction(ctx_, r)) {
                if (JS_IsBool(r)) {
                    res.json = JS_ToBool(ctx_, r) ? "true" : "false";
                } else {
                    JSValue s = JS_JSONStringify(ctx_, r, JS_UNDEFINED, JS_UNDEFINED);
                    if (JS_IsException(s)) {
                        JS_FreeValue(ctx_, JS_GetException(ctx_));
                    } else if (!JS_IsUndefined(s)) {
                        res.json = ToStd(ctx_, s);
                    }
                    JS_FreeValue(ctx_, s);
                }
            }
            JS_FreeValue(ctx_, r);
            Pump(res);
        }
        ReportRejections();
        Leave();
        return res;
    }

    // Runs the queued jobs (promise reactions, async functions) under the same deadline.
    void Pump(ScriptResult& res) {
        for (int guard = 0; guard < 100000; ++guard) {
            JSContext* c = nullptr;
            const int r = JS_ExecutePendingJob(rt_, &c);
            if (r == 0) return;
            if (r < 0) {
                ScriptResult e = TakeException();
                if (e.status == ScriptStatus::TimedOut || e.status == ScriptStatus::OutOfMemory || !res.Failed()) {
                    res = e;
                }
                if (res.status == ScriptStatus::TimedOut || res.status == ScriptStatus::OutOfMemory) return;
            }
        }
    }

    ScriptResult TakeException() {
        ScriptResult res;
        JSValue e = JS_GetException(ctx_);
        if (interrupted_) {
            res.status = ScriptStatus::TimedOut;
            res.error = StackOf(e);
            JS_FreeValue(ctx_, e);
            return res;
        }
        std::wstring text = Describe(e);
        if (JS_IsNull(e) || text.find(L"out of memory") != std::wstring::npos) {
            res.status = ScriptStatus::OutOfMemory;
            res.error = L"out of memory";
            JS_FreeValue(ctx_, e);
            return res;
        }
        res.status = ScriptStatus::Threw;
        res.error = std::move(text);
        JS_FreeValue(ctx_, e);
        return res;
    }

    // "TypeError: message" + "\n    at f (main.js:12:5)" lines.
    std::wstring Describe(JSValueConst e) {
        std::wstring text = ToWide(ctx_, e);
        if (text.empty()) text = L"(an exception without a message)";
        if (!JS_IsError(e)) text = L"Uncaught " + text;
        const std::wstring stack = StackOf(e);
        if (!stack.empty()) text += L"\n" + stack;
        return text;
    }
    std::wstring StackOf(JSValueConst e) {
        if (!JS_IsError(e)) return {};
        JSValue st = JS_GetPropertyStr(ctx_, e, "stack");
        std::wstring s;
        if (!JS_IsException(st) && !JS_IsUndefined(st)) s = ToWide(ctx_, st);
        if (JS_IsException(st)) JS_FreeValue(ctx_, JS_GetException(ctx_));
        JS_FreeValue(ctx_, st);
        while (!s.empty() && (s.back() == L'\n' || s.back() == L' ')) s.pop_back();
        return s;
    }

    void ReportRejections() {
        for (const auto& [key, text] : rejections_) host_.Log(ScriptLogLevel::Error, L"Uncaught (in promise) " + text);
        rejections_.clear();
    }

    static int Interrupt(JSContext*, void* opaque) {
        auto* self = static_cast<QuickJsEngine*>(opaque);
        if (!self->armed_ || WidgetNowMs() <= self->deadline_) return 0;
        self->interrupted_ = true;
        return 1;
    }

    static void Rejection(JSContext* ctx, JSValueConst promise, JSValueConst reason, bool handled, void* opaque) {
        auto* self = static_cast<QuickJsEngine*>(opaque);
        try {
            const void* key = JS_VALUE_GET_PTR(promise);
            if (handled) {
                self->rejections_.erase(key);
            } else {
                self->rejections_[key] = self->Describe(reason);
            }
        } catch (...) {
        }
        (void)ctx;
    }

    // --- modules (sandboxed) ---
    static char* Normalize(JSContext* ctx, const char* base, const char* name, void*) {
        try {
            std::string out;
            if (!Widgets::ResolvePath(base ? base : "", name ? name : "", out)) {
                JS_ThrowReferenceError(ctx, "module '%s' is not a path inside the widget folder", name ? name : "");
                return nullptr;
            }
            return js_strdup(ctx, out.c_str());
        } catch (...) {
            JS_ThrowInternalError(ctx, "module name error");
            return nullptr;
        }
    }

    static JSModuleDef* Loader(JSContext* ctx, const char* name, void* opaque) {
        auto* self = static_cast<QuickJsEngine*>(opaque);
        try {
            std::string src;
            std::wstring error;
            if (!self->host_.ReadFile(name, src, error)) {
                JS_ThrowReferenceError(ctx, "could not load module '%s': %s", name, WideToUtf8(error).c_str());
                return nullptr;
            }
            JSValue fn = JS_Eval(ctx, src.data(), src.size(), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            if (JS_IsException(fn)) return nullptr;
            auto* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(fn));
            JSValue meta = JS_GetImportMeta(ctx, m);
            if (!JS_IsException(meta)) {
                JS_SetPropertyStr(ctx, meta, "url", NewString(ctx, std::string("widget:/") + name));
            }
            JS_FreeValue(ctx, meta);
            JS_FreeValue(ctx, fn);  // the module stays referenced by the runtime's module list
            return m;
        } catch (...) {
            JS_ThrowInternalError(ctx, "module loader error");
            return nullptr;
        }
    }

    static void PathFinalizer(JSRuntime* rt, JSValueConst v) {
        auto* self = static_cast<QuickJsEngine*>(JS_GetRuntimeOpaque(rt));
        if (!self) return;
        delete static_cast<CanvasPath*>(JS_GetOpaque(v, self->pathClass_));
    }

    JSValue ParseSettings() {
        const std::string& s = host_.SettingsJson();
        JSValue v = JS_ParseJSON(ctx_, s.data(), s.size(), "<settings>");
        if (JS_IsException(v)) {
            JS_FreeValue(ctx_, JS_GetException(ctx_));
            v = JS_NewObject(ctx_);
        }
        return v;
    }

    bool InstallApi();

    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
    ScriptLimits limits_;
    JSValue fns_[static_cast<size_t>(Fn::Count)]{};
    JSValue ns_ = JS_UNDEFINED;
    JSValue emit_ = JS_UNDEFINED;  // the prelude's emit(channel) for provider changes
    JSValue canvas_ = JS_UNDEFINED;
    JSValue dock_ = JS_UNDEFINED;
    JSValue settings_ = JS_UNDEFINED;
    std::unordered_map<int, Timer> timers_;
    int nextTimer_ = 1;
    double deadline_ = 0.0;
    bool armed_ = false;
    bool interrupted_ = false;
    std::map<const void*, std::wstring> rejections_;
    struct Pending {
        JSValue resolve = JS_UNDEFINED;
        JSValue reject = JS_UNDEFINED;
        bool wantJson = false;
    };
    std::unordered_map<int, Pending> pending_;  // http requests in flight
    std::unordered_map<int, Pending> settles_;  // host requests in flight (popup.editText)
};

// --- canvas ------------------------------------------------------------------------------------

WidgetCanvas* CanvasOf(JSContext* ctx) { return Self(ctx)->host_.Canvas(); }

JSValue NoCanvas(JSContext* ctx) {
    return JS_ThrowTypeError(ctx, "the canvas can only be used inside draw(), measure() or drawPopup()");
}
JSValue FileIconFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv);  // below (shell)

// A colour (string / 0xAARRGGBB number) or a gradient object; undefined: `fallback`.
bool ReadPaint(JSContext* ctx, JSValueConst v, uint32_t fallback, CanvasPaint& p) {
    p = CanvasPaint{};
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        p.color = CanvasColorFromArgb(fallback);
        return true;
    }
    if (JS_IsNumber(v)) {
        double d = 0;
        JS_ToFloat64(ctx, &d, v);
        p.color = CanvasColorFromArgb(static_cast<uint32_t>(static_cast<int64_t>(d) & 0xFFFFFFFF));
        return true;
    }
    if (JS_IsString(v)) {
        const std::string s = ToStd(ctx, v);
        if (!ParseCanvasColor(s, p.color)) {
            JS_ThrowTypeError(ctx, "not a colour: '%s' (use #RRGGBB, #AARRGGBB, rgb() or rgba())", s.c_str());
            return false;
        }
        return true;
    }
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "not a colour or gradient");
        return false;
    }
    std::string type;
    if (!PropS(ctx, v, "type", type)) return false;
    if (type == "linear") {
        p.kind = CanvasPaint::Kind::Linear;
        if (!PropF(ctx, v, "x0", p.x0) || !PropF(ctx, v, "y0", p.y0) || !PropF(ctx, v, "x1", p.x1) ||
            !PropF(ctx, v, "y1", p.y1)) {
            return false;
        }
    } else if (type == "radial") {
        p.kind = CanvasPaint::Kind::Radial;
        if (!PropF(ctx, v, "cx", p.x0) || !PropF(ctx, v, "cy", p.y0) || !PropF(ctx, v, "rx", p.rx) ||
            !PropF(ctx, v, "ry", p.ry) || !PropF(ctx, v, "fx", p.x1) || !PropF(ctx, v, "fy", p.y1)) {
            return false;
        }
    } else {
        JS_ThrowTypeError(ctx, "a gradient needs type 'linear' or 'radial' (canvas.linearGradient / radialGradient)");
        return false;
    }
    JSValue stops = JS_GetPropertyStr(ctx, v, "stops");
    if (JS_IsException(stops)) return false;
    int64_t n = 0;
    if (!JS_IsArray(stops) || JS_GetLength(ctx, stops, &n) < 0 || n < 1 || n > 64) {
        JS_FreeValue(ctx, stops);
        JS_ThrowTypeError(ctx, "gradient stops: an array of 1 - 64 [offset, colour] pairs");
        return false;
    }
    for (int64_t i = 0; i < n; ++i) {
        JSValue s = JS_GetPropertyUint32(ctx, stops, static_cast<uint32_t>(i));
        CanvasGradientStop stop;
        bool ok = JS_IsArray(s);
        if (ok) {
            JSValue off = JS_GetPropertyUint32(ctx, s, 0), col = JS_GetPropertyUint32(ctx, s, 1);
            double d = 0;
            ok = !JS_ToFloat64(ctx, &d, off);
            stop.offset = static_cast<float>(d);
            CanvasPaint c;
            ok = ok && ReadPaint(ctx, col, 0xFFFFFFFF, c) && c.kind == CanvasPaint::Kind::Solid;
            stop.color = c.color;
            JS_FreeValue(ctx, off);
            JS_FreeValue(ctx, col);
        }
        JS_FreeValue(ctx, s);
        if (!ok) {
            JS_FreeValue(ctx, stops);
            if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "gradient stop %d: [offset, colour]", static_cast<int>(i));
            return false;
        }
        p.stops.push_back(stop);
    }
    JS_FreeValue(ctx, stops);
    return true;
}

// A width, or { width, cap: "butt" | "round" | "square", join: "miter" | "round" | "bevel", dash: [] }.
bool ReadStroke(JSContext* ctx, JSValueConst v, CanvasStroke& s) {
    s = CanvasStroke{};
    if (JS_IsUndefined(v) || JS_IsNull(v)) return true;
    if (JS_IsNumber(v)) {
        double d = 1;
        JS_ToFloat64(ctx, &d, v);
        s.width = static_cast<float>(d);
        return true;
    }
    if (!JS_IsObject(v)) return true;
    std::string cap, join;
    if (!PropF(ctx, v, "width", s.width) || !PropS(ctx, v, "cap", cap) || !PropS(ctx, v, "join", join)) return false;
    if (cap == "round") s.cap = D2D1_CAP_STYLE_ROUND;
    if (cap == "square") s.cap = D2D1_CAP_STYLE_SQUARE;
    if (join == "round") s.join = D2D1_LINE_JOIN_ROUND;
    if (join == "bevel") s.join = D2D1_LINE_JOIN_BEVEL;
    JSValue dash = JS_GetPropertyStr(ctx, v, "dash");
    if (JS_IsException(dash)) return false;
    int64_t n = 0;
    if (JS_IsArray(dash) && JS_GetLength(ctx, dash, &n) == 0) {
        for (int64_t i = 0; i < n && i < 16; ++i) {
            JSValue d = JS_GetPropertyUint32(ctx, dash, static_cast<uint32_t>(i));
            double x = 0;
            JS_ToFloat64(ctx, &x, d);
            JS_FreeValue(ctx, d);
            // The script gives DIPs; Direct2D dashes are in stroke widths.
            if (s.width > 0) s.dashes.push_back(static_cast<float>(std::max(0.0, x)) / s.width);
        }
    }
    JS_FreeValue(ctx, dash);
    return true;
}

// { font, size, weight: 100-900 | "normal" | "semibold" | "bold" ..., italic, align, baseline, maxWidth, color }
bool ReadTextStyle(JSContext* ctx, JSValueConst v, CanvasTextStyle& st, JSValue* color) {
    st = CanvasTextStyle{};
    *color = JS_UNDEFINED;
    if (!JS_IsObject(v)) return true;
    std::string font, align, baseline, weight;
    bool weightPresent = false;
    if (!PropS(ctx, v, "font", font) || !PropF(ctx, v, "size", st.size) || !PropS(ctx, v, "align", align) ||
        !PropS(ctx, v, "baseline", baseline) || !PropF(ctx, v, "maxWidth", st.maxWidth) ||
        !PropB(ctx, v, "italic", st.italic) || !PropS(ctx, v, "weight", weight, &weightPresent)) {
        return false;
    }
    if (!font.empty()) st.family = Utf8ToWide(font);
    if (weightPresent) {
        if (weight == "thin") st.weight = 100;
        else if (weight == "light") st.weight = 300;
        else if (weight == "normal" || weight == "regular") st.weight = 400;
        else if (weight == "medium") st.weight = 500;
        else if (weight == "semibold") st.weight = 600;
        else if (weight == "bold") st.weight = 700;
        else if (weight == "black") st.weight = 900;
        else st.weight = std::clamp(atoi(weight.c_str()), 1, 999);
    }
    if (align == "center") st.align = 1;
    if (align == "right") st.align = 2;
    if (baseline == "middle") st.valign = 1;
    if (baseline == "bottom") st.valign = 2;
    if (baseline == "alphabetic" || baseline == "baseline") st.valign = 3;
    *color = JS_GetPropertyStr(ctx, v, "color");
    if (JS_IsException(*color)) {
        *color = JS_UNDEFINED;
        return false;
    }
    return true;
}

#define CANVAS_OR_THROW()                     \
    WidgetCanvas* c = CanvasOf(ctx);          \
    if (!c) return NoCanvas(ctx);             \
    Args a{ctx, argc, argv};

uint32_t DefaultInk(JSContext* ctx) { return Self(ctx)->host_.Dock().text; }

JSValue CanvasRect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, bool fill, bool round) {
    CANVAS_OR_THROW();
    const float x = a.F(0), y = a.F(1), w = a.F(2), h = a.F(3);
    const float r = round ? a.F(4) : 0.0f;
    const int pi = round ? 5 : 4;
    if (a.failed) return JS_EXCEPTION;
    CanvasPaint p;
    if (!ReadPaint(ctx, a.At(pi), DefaultInk(ctx), p)) return JS_EXCEPTION;
    if (fill) {
        if (round) c->FillRoundRect(x, y, w, h, r, p);
        else c->FillRect(x, y, w, h, p);
        return JS_UNDEFINED;
    }
    CanvasStroke s;
    if (!ReadStroke(ctx, a.At(pi + 1), s)) return JS_EXCEPTION;
    if (round) c->StrokeRoundRect(x, y, w, h, r, p, s);
    else c->StrokeRect(x, y, w, h, p, s);
    return JS_UNDEFINED;
}
JSValue CvFillRect(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv) { return CanvasRect(ctx, t, argc, argv, true, false); }
JSValue CvStrokeRect(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv) { return CanvasRect(ctx, t, argc, argv, false, false); }
JSValue CvFillRoundRect(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv) { return CanvasRect(ctx, t, argc, argv, true, true); }
JSValue CvStrokeRoundRect(JSContext* ctx, JSValueConst t, int argc, JSValueConst* argv) { return CanvasRect(ctx, t, argc, argv, false, true); }

JSValue CanvasEllipse(JSContext* ctx, int argc, JSValueConst* argv, bool fill, bool circle) {
    CANVAS_OR_THROW();
    const float cx = a.F(0), cy = a.F(1), rx = a.F(2);
    const float ry = circle ? rx : a.F(3, rx);
    const int pi = circle ? 3 : 4;
    if (a.failed) return JS_EXCEPTION;
    CanvasPaint p;
    if (!ReadPaint(ctx, a.At(pi), DefaultInk(ctx), p)) return JS_EXCEPTION;
    if (fill) {
        c->FillEllipse(cx, cy, rx, ry, p);
        return JS_UNDEFINED;
    }
    CanvasStroke s;
    if (!ReadStroke(ctx, a.At(pi + 1), s)) return JS_EXCEPTION;
    c->StrokeEllipse(cx, cy, rx, ry, p, s);
    return JS_UNDEFINED;
}
JSValue CvFillEllipse(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { return CanvasEllipse(ctx, argc, argv, true, false); }
JSValue CvStrokeEllipse(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { return CanvasEllipse(ctx, argc, argv, false, false); }
JSValue CvFillCircle(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { return CanvasEllipse(ctx, argc, argv, true, true); }
JSValue CvStrokeCircle(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { return CanvasEllipse(ctx, argc, argv, false, true); }

JSValue LineFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    const float x0 = a.F(0), y0 = a.F(1), x1 = a.F(2), y1 = a.F(3);
    if (a.failed) return JS_EXCEPTION;
    CanvasPaint p;
    CanvasStroke s;
    if (!ReadPaint(ctx, a.At(4), DefaultInk(ctx), p) || !ReadStroke(ctx, a.At(5), s)) return JS_EXCEPTION;
    c->Line(x0, y0, x1, y1, p, s);
    return JS_UNDEFINED;
}

CanvasPath* PathOf(JSContext* ctx, JSValueConst v) {
    return static_cast<CanvasPath*>(JS_GetOpaque2(ctx, v, Self(ctx)->pathClass_));
}

JSValue FillPathFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    CanvasPath* path = PathOf(ctx, a.At(0));
    if (!path) return JS_EXCEPTION;
    CanvasPaint p;
    if (!ReadPaint(ctx, a.At(1), DefaultInk(ctx), p)) return JS_EXCEPTION;
    const std::wstring rule = a.S(2);
    if (a.failed) return JS_EXCEPTION;
    c->FillPath(*path, p, rule == L"evenodd");
    return JS_UNDEFINED;
}

JSValue StrokePathFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    CanvasPath* path = PathOf(ctx, a.At(0));
    if (!path) return JS_EXCEPTION;
    CanvasPaint p;
    CanvasStroke s;
    if (!ReadPaint(ctx, a.At(1), DefaultInk(ctx), p) || !ReadStroke(ctx, a.At(2), s)) return JS_EXCEPTION;
    c->StrokePath(*path, p, s);
    return JS_UNDEFINED;
}

JSValue TextFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    const std::wstring text = a.S(0);
    const float x = a.F(1), y = a.F(2);
    if (a.failed) return JS_EXCEPTION;
    CanvasTextStyle st;
    JSValue color = JS_UNDEFINED;
    if (!ReadTextStyle(ctx, a.At(3), st, &color)) return JS_EXCEPTION;
    CanvasPaint p;
    const bool ok = ReadPaint(ctx, color, DefaultInk(ctx), p);
    JS_FreeValue(ctx, color);
    if (!ok) return JS_EXCEPTION;
    c->Text(text, x, y, st, p);
    return JS_UNDEFINED;
}

JSValue MeasureTextFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    const std::wstring text = a.S(0);
    if (a.failed) return JS_EXCEPTION;
    CanvasTextStyle st;
    JSValue color = JS_UNDEFINED;
    if (!ReadTextStyle(ctx, a.At(1), st, &color)) return JS_EXCEPTION;
    JS_FreeValue(ctx, color);
    const CanvasTextMetrics m = c->MeasureText(text, st);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, m.width));
    JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, m.height));
    JS_SetPropertyStr(ctx, o, "baseline", JS_NewFloat64(ctx, m.baseline));
    JS_SetPropertyStr(ctx, o, "lines", JS_NewInt32(ctx, m.lines));
    return o;
}

bool AssetPath(JSContext* ctx, const std::wstring& given, std::string& path) {
    // media.art()'s token: the current media session's thumbnail (permission "media").
    if (given.rfind(L"media:art/", 0) == 0) {
        if (!Self(ctx)->host_.Permitted(ScriptHost::Permission::Media)) {
            JS_ThrowTypeError(ctx, "image: media art needs the \"media\" permission");
            return false;
        }
        path = WideToUtf8(given);
        return true;
    }
    if (given.rfind(L"weather:", 0) == 0) {  // weather.icon(): the dock's vector glyphs (no data in them)
        path = WideToUtf8(given);
        return true;
    }
    // Round 17 phase 17A: a file anywhere on disk ("C:\...", "\\server\...", "%USERPROFILE%\...")
    // with the "shell" permission (which already opens files and lists folders). The host reads it
    // under the "file:" token (ScriptHost::ReadFile; a module name never carries it: ResolvePath
    // refuses ':' and '\'), and the canvas decodes it photo-sized (WidgetCanvas::FindImage).
    const bool absolute = given.size() >= 3 && ((std::iswalpha(given[0]) && given[1] == L':' && (given[2] == L'\\' || given[2] == L'/')) ||
                                                 (given[0] == L'\\' && given[1] == L'\\') || given[0] == L'%');
    if (absolute) {
        if (!Self(ctx)->host_.Permitted(ScriptHost::Permission::Shell)) {
            JS_ThrowTypeError(ctx, "image: a file outside the widget folder needs the \"shell\" permission");
            return false;
        }
        if (given.size() > 2048) {
            JS_ThrowTypeError(ctx, "image: the path is too long");
            return false;
        }
        path = "file:" + WideToUtf8(given);
        return true;
    }
    if (!Widgets::ResolvePath("", WideToUtf8(given), path)) {
        JS_ThrowTypeError(ctx, "image: '%s' is not a path inside the widget folder", WideToUtf8(given).c_str());
        return false;
    }
    return true;
}

JSValue ImageFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    const std::wstring given = a.S(0);
    const float x = a.F(1), y = a.F(2), w = a.F(3), h = a.F(4), opacity = a.F(5, 1.0f);
    if (a.failed) return JS_EXCEPTION;
    std::string path;
    if (!AssetPath(ctx, given, path)) return JS_EXCEPTION;
    std::wstring error;
    if (!c->Image(path, x, y, w, h, opacity, error)) return JS_ThrowTypeError(ctx, "image: %s", WideToUtf8(error).c_str());
    return JS_UNDEFINED;
}

JSValue ImageSizeFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    const std::wstring given = a.S(0);
    if (a.failed) return JS_EXCEPTION;
    std::string path;
    if (!AssetPath(ctx, given, path)) return JS_EXCEPTION;
    float w = 0, h = 0;
    std::wstring error;
    if (!c->ImageSize(path, w, h, error)) return JS_ThrowTypeError(ctx, "imageSize: %s", WideToUtf8(error).c_str());
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, w));
    JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, h));
    return o;
}

JSValue SaveFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    WidgetCanvas* c = CanvasOf(ctx);
    if (!c) return NoCanvas(ctx);
    c->Save();
    return JS_UNDEFINED;
}
JSValue RestoreFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    WidgetCanvas* c = CanvasOf(ctx);
    if (!c) return NoCanvas(ctx);
    return JS_NewBool(ctx, c->Restore());
}
JSValue TranslateFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    const float x = a.F(0), y = a.F(1);
    if (a.failed) return JS_EXCEPTION;
    c->Translate(x, y);
    return JS_UNDEFINED;
}
JSValue RotateFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    const float r = a.F(0), cx = a.F(1), cy = a.F(2);
    if (a.failed) return JS_EXCEPTION;
    c->Rotate(r, cx, cy);
    return JS_UNDEFINED;
}
JSValue ScaleFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    const float sx = a.F(0, 1.0f);
    const float sy = a.F(1, sx);
    if (a.failed) return JS_EXCEPTION;
    c->ScaleBy(sx, sy);
    return JS_UNDEFINED;
}
JSValue OpacityFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    const float o = a.F(0, 1.0f);
    if (a.failed) return JS_EXCEPTION;
    c->Opacity(o);
    return JS_UNDEFINED;
}
JSValue ClipFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    const float x = a.F(0), y = a.F(1), w = a.F(2), h = a.F(3);
    if (a.failed) return JS_EXCEPTION;
    c->ClipRect(x, y, w, h);
    return JS_UNDEFINED;
}
// Round 16 phase 16F: canvas.layer(id, x, y, w, h, { parent, clip, opacity, tracks }) and
// canvas.endLayer() (docs/widgets.md "canvas"). A track: { prop: "x" | "y" | "rotate" |
// "opacity", center: [cx, cy], duration, begin, once, keys: [[seconds, value], ...], easing:
// [[x1, y1, x2, y2], ...] }.
bool ReadTrack(JSContext* ctx, JSValueConst v, CanvasTrack& t) {
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "layer: a track must be an object");
        return false;
    }
    float duration = 0.0f, begin = 0.0f;
    bool once = false;
    if (!PropS(ctx, v, "prop", t.prop) || !PropF(ctx, v, "duration", duration) || !PropF(ctx, v, "begin", begin) ||
        !PropB(ctx, v, "once", once)) {
        return false;
    }
    if (t.prop != "x" && t.prop != "y" && t.prop != "rotate" && t.prop != "opacity") {
        JS_ThrowTypeError(ctx, "layer: a track's prop is \"x\", \"y\", \"rotate\" or \"opacity\"");
        return false;
    }
    if (!(duration > 0.0f) || !std::isfinite(duration) || !std::isfinite(begin)) {
        JS_ThrowTypeError(ctx, "layer: a track needs a duration above 0 (seconds)");
        return false;
    }
    t.duration = duration;
    t.begin = begin;
    t.repeat = !once;
    JSValue centre = JS_GetPropertyStr(ctx, v, "center");
    if (JS_IsException(centre)) return false;
    if (JS_IsArray(centre)) {
        JSValue cx = JS_GetPropertyUint32(ctx, centre, 0), cy = JS_GetPropertyUint32(ctx, centre, 1);
        double dx = 0, dy = 0;
        JS_ToFloat64(ctx, &dx, cx);
        JS_ToFloat64(ctx, &dy, cy);
        t.cx = static_cast<float>(dx);
        t.cy = static_cast<float>(dy);
        JS_FreeValue(ctx, cx);
        JS_FreeValue(ctx, cy);
    }
    JS_FreeValue(ctx, centre);
    JSValue keys = JS_GetPropertyStr(ctx, v, "keys");
    if (JS_IsException(keys)) return false;
    int64_t n = 0;
    if (!JS_IsArray(keys) || JS_GetLength(ctx, keys, &n) < 0 || n < 2 || n > 64) {
        JS_FreeValue(ctx, keys);
        JS_ThrowTypeError(ctx, "layer: a track's keys are 2 - 64 [seconds, value] pairs");
        return false;
    }
    for (int64_t i = 0; i < n; ++i) {
        JSValue k = JS_GetPropertyUint32(ctx, keys, static_cast<uint32_t>(i));
        bool ok = JS_IsArray(k);
        double time = 0, value = 0;
        if (ok) {
            JSValue a0 = JS_GetPropertyUint32(ctx, k, 0), a1 = JS_GetPropertyUint32(ctx, k, 1);
            ok = !JS_ToFloat64(ctx, &time, a0) && !JS_ToFloat64(ctx, &value, a1) && std::isfinite(time) && std::isfinite(value);
            JS_FreeValue(ctx, a0);
            JS_FreeValue(ctx, a1);
        }
        JS_FreeValue(ctx, k);
        ok = ok && time >= 0.0 && time <= t.duration + 1e-6 && (t.keys.empty() || time >= t.keys.back().first);
        if (!ok) {
            JS_FreeValue(ctx, keys);
            if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "layer: key %d: [seconds, value], in order, within the duration", static_cast<int>(i));
            return false;
        }
        t.keys.emplace_back(time, static_cast<float>(value));
    }
    JS_FreeValue(ctx, keys);
    JSValue easing = JS_GetPropertyStr(ctx, v, "easing");
    if (JS_IsException(easing)) return false;
    if (JS_IsArray(easing)) {
        int64_t m = 0;
        if (JS_GetLength(ctx, easing, &m) < 0 || m != n - 1) {
            JS_FreeValue(ctx, easing);
            JS_ThrowTypeError(ctx, "layer: easing needs one [x1, y1, x2, y2] per key span");
            return false;
        }
        for (int64_t i = 0; i < m; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, easing, static_cast<uint32_t>(i));
            std::array<float, 4> b{0.0f, 0.0f, 1.0f, 1.0f};
            if (JS_IsArray(e)) {
                for (uint32_t j = 0; j < 4; ++j) {
                    JSValue x = JS_GetPropertyUint32(ctx, e, j);
                    double d = 0;
                    if (!JS_ToFloat64(ctx, &d, x) && std::isfinite(d)) b[j] = static_cast<float>(d);
                    JS_FreeValue(ctx, x);
                }
            }
            JS_FreeValue(ctx, e);
            b[0] = std::clamp(b[0], 0.0f, 1.0f);
            b[2] = std::clamp(b[2], 0.0f, 1.0f);
            t.easing.push_back(b);
        }
    }
    JS_FreeValue(ctx, easing);
    return true;
}

JSValue LayerFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    CanvasLayerSpec spec;
    spec.id = WideToUtf8(a.S(0));
    spec.x = a.F(1);
    spec.y = a.F(2);
    spec.w = a.F(3);
    spec.h = a.F(4);
    if (a.failed) return JS_EXCEPTION;
    if (spec.id.empty() || spec.id.size() > 64) return JS_ThrowTypeError(ctx, "layer: an id of 1 - 64 characters is needed");
    JSValueConst o = a.At(5);
    if (JS_IsObject(o)) {
        if (!PropS(ctx, o, "parent", spec.parent) || !PropB(ctx, o, "clip", spec.clip) || !PropF(ctx, o, "opacity", spec.opacity)) {
            return JS_EXCEPTION;
        }
        spec.opacity = std::clamp(spec.opacity, 0.0f, 1.0f);
        JSValue tracks = JS_GetPropertyStr(ctx, o, "tracks");
        if (JS_IsException(tracks)) return JS_EXCEPTION;
        if (!JS_IsUndefined(tracks) && !JS_IsNull(tracks)) {
            int64_t n = 0;
            if (!JS_IsArray(tracks) || JS_GetLength(ctx, tracks, &n) < 0 || n > 32) {
                JS_FreeValue(ctx, tracks);
                return JS_ThrowTypeError(ctx, "layer: tracks must be an array of up to 32 track objects");
            }
            for (int64_t i = 0; i < n; ++i) {
                JSValue tv = JS_GetPropertyUint32(ctx, tracks, static_cast<uint32_t>(i));
                CanvasTrack t;
                const bool ok = ReadTrack(ctx, tv, t);
                JS_FreeValue(ctx, tv);
                if (!ok) {
                    JS_FreeValue(ctx, tracks);
                    return JS_EXCEPTION;
                }
                spec.tracks.push_back(std::move(t));
            }
        }
        JS_FreeValue(ctx, tracks);
        // The options as text: an unchanged animation keeps running across redraws.
        JSValue json = JS_JSONStringify(ctx, o, JS_UNDEFINED, JS_UNDEFINED);
        if (JS_IsException(json)) return JS_EXCEPTION;
        spec.key = ToStd(ctx, json);
        JS_FreeValue(ctx, json);
    }
    std::wstring error;
    const bool live = c->BeginLayer(spec, error);
    if (!live && !error.empty()) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_NewBool(ctx, live);
}

JSValue EndLayerFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    WidgetCanvas* c = CanvasOf(ctx);
    if (!c) return NoCanvas(ctx);
    if (!c->EndLayer()) return JS_ThrowTypeError(ctx, "endLayer: no layer is open");
    return JS_UNDEFINED;
}

JSValue ClipPathFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    CanvasPath* path = PathOf(ctx, a.At(0));
    if (!path) return JS_EXCEPTION;
    c->ClipPath(*path);
    return JS_UNDEFINED;
}

JSValue NewPath(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(Self(ctx)->pathClass_));
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, new CanvasPath());
    return obj;
}

JSValue CanvasGet(JSContext* ctx, JSValueConst, int magic) {
    WidgetCanvas* c = CanvasOf(ctx);
    if (!c) return JS_NewFloat64(ctx, 0);
    switch (magic) {
        case 0: return JS_NewFloat64(ctx, c->Width());
        case 1: return JS_NewFloat64(ctx, c->Height());
        case 2: return JS_NewFloat64(ctx, c->Scale());
        default: return JS_NewBool(ctx, c->Drawing());
    }
}

// --- Path methods (return the path, so calls chain) ---
JSValue PathOp(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int op) {
    CanvasPath* p = PathOf(ctx, self);
    if (!p) return JS_EXCEPTION;
    Args a{ctx, argc, argv};
    float v[7];
    for (int i = 0; i < 7; ++i) v[i] = a.F(i);
    const bool ccw = a.B(5);
    if (a.failed) return JS_EXCEPTION;
    if (p->Size() > 100000) return JS_ThrowRangeError(ctx, "path too long");
    switch (op) {
        case 0: p->MoveTo(v[0], v[1]); break;
        case 1: p->LineTo(v[0], v[1]); break;
        case 2: p->QuadTo(v[0], v[1], v[2], v[3]); break;
        case 3: p->BezierTo(v[0], v[1], v[2], v[3], v[4], v[5]); break;
        case 4: p->Arc(v[0], v[1], v[2], v[3], v[4], ccw); break;
        case 5: p->Rect(v[0], v[1], v[2], v[3]); break;
        default: p->Close(); break;
    }
    return JS_DupValue(ctx, self);
}
JSValue PathMoveTo(JSContext* ctx, JSValueConst s, int argc, JSValueConst* argv) { return PathOp(ctx, s, argc, argv, 0); }
JSValue PathLineTo(JSContext* ctx, JSValueConst s, int argc, JSValueConst* argv) { return PathOp(ctx, s, argc, argv, 1); }
JSValue PathQuadTo(JSContext* ctx, JSValueConst s, int argc, JSValueConst* argv) { return PathOp(ctx, s, argc, argv, 2); }
JSValue PathBezierTo(JSContext* ctx, JSValueConst s, int argc, JSValueConst* argv) { return PathOp(ctx, s, argc, argv, 3); }
JSValue PathArc(JSContext* ctx, JSValueConst s, int argc, JSValueConst* argv) { return PathOp(ctx, s, argc, argv, 4); }
JSValue PathRect(JSContext* ctx, JSValueConst s, int argc, JSValueConst* argv) { return PathOp(ctx, s, argc, argv, 5); }
JSValue PathClose(JSContext* ctx, JSValueConst s, int argc, JSValueConst* argv) { return PathOp(ctx, s, argc, argv, 6); }

// --- dock ----------------------------------------------------------------------------------------

JSValue DockGet(JSContext* ctx, JSValueConst, int magic) {
    const ScriptDockInfo& d = Self(ctx)->host_.Dock();
    switch (magic) {
        case 0: return NewString(ctx, d.axis);
        case 1: return NewString(ctx, d.edge);
        case 2: return JS_NewFloat64(ctx, d.width);
        case 3: return JS_NewFloat64(ctx, d.height);
        case 4: return JS_NewFloat64(ctx, d.main);
        case 5: return JS_NewFloat64(ctx, d.cross);
        case 6: return JS_NewFloat64(ctx, d.scale);
        case 7: return JS_NewFloat64(ctx, d.iconSize);
        case 8: return JS_NewBool(ctx, d.hovered);
        case 9: return JS_NewBool(ctx, d.magnified);
        case 10: return JS_NewBool(ctx, d.reduceMotion);
        case 11: return JS_NewBool(ctx, d.locked);
        case 12: return NewString(ctx, d.dockId);
        case 13: return NewString(ctx, d.widgetId);
        case 14: return JS_NewBool(ctx, d.visible);
        case 15: return JS_NewBool(ctx, d.clockDate);
        case 16: return JS_NewBool(ctx, d.standalone);
        case 17: return NewString(ctx, d.itemKey);
        case 18: return JS_NewBool(ctx, d.preview);
        default: return JS_UNDEFINED;
    }
}

JSValue ThemeGet(JSContext* ctx, JSValueConst, int magic) {
    const ScriptDockInfo& d = Self(ctx)->host_.Dock();
    const uint32_t v[] = {d.text, d.dateText, d.accent, d.background, d.labelBackground, d.labelText, d.separator,
                          d.border};
    return magic >= 0 && magic < static_cast<int>(std::size(v)) ? NewString(ctx, ArgbText(v[magic])) : JS_UNDEFINED;
}

// --- timers, console, redraw ---------------------------------------------------------------------

uint32_t Ms(Args& a, int i) {
    const float ms = a.F(i, 0.0f);
    if (!std::isfinite(ms) || ms < 0.0f) return 0;
    return static_cast<uint32_t>(std::min(ms, 2.0e9f));
}

JSValue SetTimeoutFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const uint32_t ms = Ms(a, 1);
    if (a.failed) return JS_EXCEPTION;
    return Self(ctx)->AddTimer(a.At(0), ms, false);
}
JSValue SetIntervalFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const uint32_t ms = Ms(a, 1);
    if (a.failed) return JS_EXCEPTION;
    return Self(ctx)->AddTimer(a.At(0), ms, true);
}
JSValue ClearTimerFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const float id = a.F(0, 0.0f);
    if (a.failed) return JS_EXCEPTION;
    if (id >= 1.0f && id < 1.0e9f) Self(ctx)->ClearTimer(static_cast<int>(id));
    return JS_UNDEFINED;
}
// A time zone argument (round 5 phase D): a Windows zone key name; absent / null / "" = local.
std::wstring ZoneArg(Args& a, int i) {
    if (!a.Has(i) || JS_IsNull(a.At(i))) return {};
    std::wstring z = a.S(i);
    return z.size() > 128 ? std::wstring() : z;
}
JSValue SetRefreshFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const uint32_t ms = Ms(a, 0);
    // timer.setRefresh(ms, { timeZone }) (or the key as a string): aligned to that zone's time of day.
    std::wstring zone;
    if (a.Has(1) && JS_IsObject(a.At(1))) {
        std::string z;
        if (!PropS(ctx, a.At(1), "timeZone", z)) return JS_EXCEPTION;
        zone = z.size() > 256 ? std::wstring() : Utf8ToWide(z);
    } else {
        zone = ZoneArg(a, 1);
    }
    if (a.failed) return JS_EXCEPTION;
    Self(ctx)->host_.SetRefresh(ms, zone);
    return JS_UNDEFINED;
}
JSValue RequestRedrawFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Self(ctx)->host_.RequestRedraw();
    return JS_UNDEFINED;
}
JSValue LogFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const float level = a.F(0, 0.0f);
    const std::wstring text = a.S(1);
    if (a.failed) return JS_EXCEPTION;
    Self(ctx)->host_.Log(level >= 2.0f ? ScriptLogLevel::Error : level >= 1.0f ? ScriptLogLevel::Warn : ScriptLogLevel::Info,
                         text);
    return JS_UNDEFINED;
}

// --- system ----------------------------------------------------------------------------------------

// system.now() / system.localTime(timeZone): the same object (round 5 phase D: + timeZone).
JSValue NowFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::wstring zone = ZoneArg(a, 0);
    if (a.failed) return JS_EXCEPTION;
    const WidgetSystem::LocalTime t = WidgetSystem::Now(zone);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "year", JS_NewInt32(ctx, t.year));
    JS_SetPropertyStr(ctx, o, "month", JS_NewInt32(ctx, t.month));
    JS_SetPropertyStr(ctx, o, "day", JS_NewInt32(ctx, t.day));
    JS_SetPropertyStr(ctx, o, "hour", JS_NewInt32(ctx, t.hour));
    JS_SetPropertyStr(ctx, o, "minute", JS_NewInt32(ctx, t.minute));
    JS_SetPropertyStr(ctx, o, "second", JS_NewInt32(ctx, t.second));
    JS_SetPropertyStr(ctx, o, "millisecond", JS_NewInt32(ctx, t.millisecond));
    JS_SetPropertyStr(ctx, o, "weekday", JS_NewInt32(ctx, t.weekday));
    JS_SetPropertyStr(ctx, o, "yearDay", JS_NewInt32(ctx, t.yearDay));
    JS_SetPropertyStr(ctx, o, "dst", JS_NewBool(ctx, t.dst));
    JS_SetPropertyStr(ctx, o, "utcOffset", JS_NewInt32(ctx, t.utcOffset));
    JS_SetPropertyStr(ctx, o, "timeZone", NewString(ctx, t.timeZone));
    return o;
}
JSValue TimeZonesFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const std::vector<WidgetSystem::Zone> zones = WidgetSystem::TimeZones();
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < zones.size(); ++i) {
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "key", NewString(ctx, zones[i].key));
        JS_SetPropertyStr(ctx, o, "name", NewString(ctx, zones[i].name));
        JS_SetPropertyStr(ctx, o, "standardOffset", JS_NewInt32(ctx, zones[i].standardOffset));
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), o);
    }
    return arr;
}
JSValue FormatFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::wstring f = a.S(0);
    const std::wstring zone = ZoneArg(a, 1);
    if (a.failed) return JS_EXCEPTION;
    return NewString(ctx, WidgetSystem::Format(f.size() > 256 ? f.substr(0, 256) : f, zone));
}
JSValue FormatDateFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::wstring style = a.S(0);
    const std::wstring zone = ZoneArg(a, 1);
    if (a.failed) return JS_EXCEPTION;
    return NewString(ctx, WidgetSystem::FormatDate(style != L"short", zone));
}
JSValue FormatTimeFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const bool seconds = a.B(0, false);
    const std::wstring zone = ZoneArg(a, 1);
    if (a.failed) return JS_EXCEPTION;
    return NewString(ctx, WidgetSystem::FormatTime(seconds, zone));
}
JSValue Names(JSContext* ctx, const std::vector<std::wstring>& names) {
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < names.size(); ++i) JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), NewString(ctx, names[i]));
    return arr;
}
JSValue SystemGet(JSContext* ctx, JSValueConst, int magic) {
    const ScriptDockInfo& d = Self(ctx)->host_.Dock();
    switch (magic) {
        case 0: return NewString(ctx, WidgetSystem::Locale());
        case 1: return JS_NewBool(ctx, WidgetSystem::Hour12());
        case 2: return NewString(ctx, std::string(d.metric ? "metric" : "imperial"));
        case 3: return NewString(ctx, std::string(d.metric ? "C" : "F"));
        case 4: return JS_NewInt32(ctx, WidgetSystem::FirstDayOfWeek());
        case 5: return Names(ctx, WidgetSystem::DayNames(false));
        case 6: return Names(ctx, WidgetSystem::DayNames(true));
        case 7: return Names(ctx, WidgetSystem::MonthNames(false));
        case 8: return Names(ctx, WidgetSystem::MonthNames(true));
        default: return JS_UNDEFINED;
    }
}

const JSCFunctionListEntry kCanvasFuncs[] = {
    JS_CFUNC_DEF("fillRect", 5, Guard<CvFillRect>),
    JS_CFUNC_DEF("strokeRect", 6, Guard<CvStrokeRect>),
    JS_CFUNC_DEF("fillRoundRect", 6, Guard<CvFillRoundRect>),
    JS_CFUNC_DEF("strokeRoundRect", 7, Guard<CvStrokeRoundRect>),
    JS_CFUNC_DEF("fillEllipse", 5, Guard<CvFillEllipse>),
    JS_CFUNC_DEF("strokeEllipse", 6, Guard<CvStrokeEllipse>),
    JS_CFUNC_DEF("fillCircle", 4, Guard<CvFillCircle>),
    JS_CFUNC_DEF("strokeCircle", 5, Guard<CvStrokeCircle>),
    JS_CFUNC_DEF("line", 6, Guard<LineFn>),
    JS_CFUNC_DEF("path", 0, Guard<NewPath>),
    JS_CFUNC_DEF("fill", 3, Guard<FillPathFn>),
    JS_CFUNC_DEF("stroke", 3, Guard<StrokePathFn>),
    JS_CFUNC_DEF("text", 4, Guard<TextFn>),
    JS_CFUNC_DEF("measureText", 2, Guard<MeasureTextFn>),
    JS_CFUNC_DEF("image", 6, Guard<ImageFn>),
    JS_CFUNC_DEF("imageSize", 1, Guard<ImageSizeFn>),
    JS_CFUNC_DEF("save", 0, Guard<SaveFn>),
    JS_CFUNC_DEF("restore", 0, Guard<RestoreFn>),
    JS_CFUNC_DEF("translate", 2, Guard<TranslateFn>),
    JS_CFUNC_DEF("rotate", 3, Guard<RotateFn>),
    // "scale" is the DPI property below (a method of that name would be shadowed by it).
    JS_CFUNC_DEF("scaleBy", 2, Guard<ScaleFn>),
    JS_CFUNC_DEF("opacity", 1, Guard<OpacityFn>),
    JS_CFUNC_DEF("clip", 4, Guard<ClipFn>),
    JS_CFUNC_DEF("clipPath", 1, Guard<ClipPathFn>),
    JS_CFUNC_DEF("layer", 6, Guard<LayerFn>),  // round 16 phase 16F
    JS_CFUNC_DEF("endLayer", 0, Guard<EndLayerFn>),
    JS_CFUNC_DEF("fileIcon", 4, Guard<FileIconFn>),
    JS_CGETSET_MAGIC_DEF("width", GuardGet<CanvasGet>, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("height", GuardGet<CanvasGet>, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("scale", GuardGet<CanvasGet>, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("drawing", GuardGet<CanvasGet>, nullptr, 3),
};

const JSCFunctionListEntry kPathFuncs[] = {
    JS_CFUNC_DEF("moveTo", 2, Guard<PathMoveTo>),
    JS_CFUNC_DEF("lineTo", 2, Guard<PathLineTo>),
    JS_CFUNC_DEF("quadTo", 4, Guard<PathQuadTo>),
    JS_CFUNC_DEF("bezierTo", 6, Guard<PathBezierTo>),
    JS_CFUNC_DEF("arc", 6, Guard<PathArc>),
    JS_CFUNC_DEF("rect", 4, Guard<PathRect>),
    JS_CFUNC_DEF("close", 0, Guard<PathClose>),
};

const JSCFunctionListEntry kThemeFuncs[] = {
    JS_CGETSET_MAGIC_DEF("text", GuardGet<ThemeGet>, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("dateText", GuardGet<ThemeGet>, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("accent", GuardGet<ThemeGet>, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("background", GuardGet<ThemeGet>, nullptr, 3),
    JS_CGETSET_MAGIC_DEF("labelBackground", GuardGet<ThemeGet>, nullptr, 4),
    JS_CGETSET_MAGIC_DEF("labelText", GuardGet<ThemeGet>, nullptr, 5),
    JS_CGETSET_MAGIC_DEF("separator", GuardGet<ThemeGet>, nullptr, 6),
    JS_CGETSET_MAGIC_DEF("border", GuardGet<ThemeGet>, nullptr, 7),
};

const JSCFunctionListEntry kDockFuncs[] = {
    JS_CGETSET_MAGIC_DEF("axis", GuardGet<DockGet>, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("edge", GuardGet<DockGet>, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("width", GuardGet<DockGet>, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("height", GuardGet<DockGet>, nullptr, 3),
    JS_CGETSET_MAGIC_DEF("main", GuardGet<DockGet>, nullptr, 4),
    JS_CGETSET_MAGIC_DEF("cross", GuardGet<DockGet>, nullptr, 5),
    JS_CGETSET_MAGIC_DEF("scale", GuardGet<DockGet>, nullptr, 6),
    JS_CGETSET_MAGIC_DEF("iconSize", GuardGet<DockGet>, nullptr, 7),
    JS_CGETSET_MAGIC_DEF("hovered", GuardGet<DockGet>, nullptr, 8),
    JS_CGETSET_MAGIC_DEF("magnified", GuardGet<DockGet>, nullptr, 9),
    JS_CGETSET_MAGIC_DEF("reduceMotion", GuardGet<DockGet>, nullptr, 10),
    JS_CGETSET_MAGIC_DEF("locked", GuardGet<DockGet>, nullptr, 11),
    JS_CGETSET_MAGIC_DEF("id", GuardGet<DockGet>, nullptr, 12),
    JS_CGETSET_MAGIC_DEF("widgetId", GuardGet<DockGet>, nullptr, 13),
    JS_CGETSET_MAGIC_DEF("visible", GuardGet<DockGet>, nullptr, 14),
    JS_CGETSET_MAGIC_DEF("clockDate", GuardGet<DockGet>, nullptr, 15),
    JS_CGETSET_MAGIC_DEF("standalone", GuardGet<DockGet>, nullptr, 16),
    JS_CGETSET_MAGIC_DEF("itemKey", GuardGet<DockGet>, nullptr, 17),
    JS_CGETSET_MAGIC_DEF("preview", GuardGet<DockGet>, nullptr, 18),
    JS_CFUNC_DEF("requestRedraw", 0, Guard<RequestRedrawFn>),
};

const JSCFunctionListEntry kTimerFuncs[] = {
    JS_CFUNC_DEF("setTimeout", 2, Guard<SetTimeoutFn>),
    JS_CFUNC_DEF("setInterval", 2, Guard<SetIntervalFn>),
    JS_CFUNC_DEF("clear", 1, Guard<ClearTimerFn>),
    JS_CFUNC_DEF("setRefresh", 2, Guard<SetRefreshFn>),
    JS_CFUNC_DEF("requestRedraw", 0, Guard<RequestRedrawFn>),
};

const JSCFunctionListEntry kGlobalFuncs[] = {
    JS_CFUNC_DEF("setTimeout", 2, Guard<SetTimeoutFn>),
    JS_CFUNC_DEF("setInterval", 2, Guard<SetIntervalFn>),
    JS_CFUNC_DEF("clearTimeout", 1, Guard<ClearTimerFn>),
    JS_CFUNC_DEF("clearInterval", 1, Guard<ClearTimerFn>),
    JS_CFUNC_DEF("requestRedraw", 0, Guard<RequestRedrawFn>),
};

const JSCFunctionListEntry kSystemFuncs[] = {
    JS_CFUNC_DEF("now", 0, Guard<NowFn>),
    JS_CFUNC_DEF("localTime", 1, Guard<NowFn>),
    JS_CFUNC_DEF("timeZones", 0, Guard<TimeZonesFn>),
    JS_CFUNC_DEF("format", 2, Guard<FormatFn>),
    JS_CFUNC_DEF("formatDate", 2, Guard<FormatDateFn>),
    JS_CFUNC_DEF("formatTime", 2, Guard<FormatTimeFn>),
    JS_CGETSET_MAGIC_DEF("locale", GuardGet<SystemGet>, nullptr, 0),
    JS_CGETSET_MAGIC_DEF("hour12", GuardGet<SystemGet>, nullptr, 1),
    JS_CGETSET_MAGIC_DEF("units", GuardGet<SystemGet>, nullptr, 2),
    JS_CGETSET_MAGIC_DEF("temperatureUnit", GuardGet<SystemGet>, nullptr, 3),
    JS_CGETSET_MAGIC_DEF("firstDayOfWeek", GuardGet<SystemGet>, nullptr, 4),
    JS_CGETSET_MAGIC_DEF("dayNames", GuardGet<SystemGet>, nullptr, 5),
    JS_CGETSET_MAGIC_DEF("shortDayNames", GuardGet<SystemGet>, nullptr, 6),
    JS_CGETSET_MAGIC_DEF("monthNames", GuardGet<SystemGet>, nullptr, 7),
    JS_CGETSET_MAGIC_DEF("shortMonthNames", GuardGet<SystemGet>, nullptr, 8),
};

// --- storage ---------------------------------------------------------------------------------------

JSValue StorageGetFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::string key = WideToUtf8(a.S(0));
    if (a.failed) return JS_EXCEPTION;
    std::string json;
    if (!Self(ctx)->host_.StorageGet(key, json)) return a.Has(1) ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
    JSValue v = JS_ParseJSON(ctx, json.data(), json.size(), "<storage>");
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    return v;
}
JSValue StorageSetFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::string key = WideToUtf8(a.S(0));
    if (a.failed) return JS_EXCEPTION;
    JSValue s = JS_JSONStringify(ctx, a.At(1), JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(s)) return s;
    if (JS_IsUndefined(s)) {  // undefined, a function: nothing to keep
        Self(ctx)->host_.StorageRemove(key);
        return JS_UNDEFINED;
    }
    const std::string json = ToStd(ctx, s);
    JS_FreeValue(ctx, s);
    std::wstring error;
    if (!Self(ctx)->host_.StorageSet(key, json, error)) return JS_ThrowRangeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_UNDEFINED;
}
JSValue StorageRemoveFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::string key = WideToUtf8(a.S(0));
    if (a.failed) return JS_EXCEPTION;
    Self(ctx)->host_.StorageRemove(key);
    return JS_UNDEFINED;
}
JSValue StorageKeysFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const std::vector<std::string> keys = Self(ctx)->host_.StorageKeys();
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < keys.size(); ++i) JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), NewString(ctx, keys[i]));
    return arr;
}
JSValue StorageClearFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    ScriptHost& h = Self(ctx)->host_;
    for (const std::string& k : h.StorageKeys()) h.StorageRemove(k);
    return JS_UNDEFINED;
}

// --- http (native half; http.get / post / request are in the prelude) ---------------------------

JSValue HttpFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::wstring method = a.S(0), url = a.S(1), headers = a.S(2);
    const std::string body = a.Has(3) ? ToStd(ctx, argv[3]) : std::string();
    const float timeout = a.F(4, 10000.0f);
    const bool wantJson = a.B(5);
    if (a.failed) return JS_EXCEPTION;
    if (method != L"GET" && method != L"POST" && method != L"PUT" && method != L"DELETE" && method != L"PATCH" &&
        method != L"HEAD") {
        return JS_ThrowTypeError(ctx, "http: method %s is not supported", WideToUtf8(method).c_str());
    }
    std::wstring error;
    const int id = Self(ctx)->host_.HttpStart(method, url, headers, body,
                                              static_cast<uint32_t>(std::clamp(timeout, 0.0f, 60000.0f)), error);
    // A preview (round 6 phase 6E) makes no requests: a rejected promise, never a throw.
    if (!id && Self(ctx)->host_.Dock().preview) return Self(ctx)->Rejected(Self(ctx)->MakeError(WideToUtf8(error)));
    if (!id) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return Self(ctx)->AddRequest(id, wantJson);
}

// --- shell -----------------------------------------------------------------------------------------

JSValue ShellOpenFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::wstring target = a.S(0), args = a.S(1);
    if (a.failed) return JS_EXCEPTION;
    std::wstring error;
    if (!Self(ctx)->host_.ShellOpen(target, args, error)) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_NewBool(ctx, !Self(ctx)->host_.Dock().preview);  // a preview opens nothing (false)
}
JSValue ShellListFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::wstring folder = a.S(0);
    if (a.failed) return JS_EXCEPTION;
    std::string json;
    std::wstring error;
    if (!Self(ctx)->host_.ShellList(folder, json, error)) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_ParseJSON(ctx, json.data(), json.size(), "<shell.list>");
}
JSValue SendKeysFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::wstring name = a.S(0);
    if (a.failed) return JS_EXCEPTION;
    std::wstring error;
    if (!Self(ctx)->host_.SendKeys(name, error)) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_NewBool(ctx, !Self(ctx)->host_.Dock().preview);  // a preview presses nothing (false)
}

// --- providers (native half; audio / media are in the prelude) -------------------------------------

JSValue JsonResult(JSContext* ctx, bool ok, const std::string& json, const std::wstring& error, const char* what) {
    if (!ok) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_ParseJSON(ctx, json.data(), json.size(), what);
}
JSValue AudioGetFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    std::string json;
    std::wstring error;
    const bool ok = Self(ctx)->host_.AudioGet(json, error);
    return JsonResult(ctx, ok, json, error, "<audio>");
}
// audioSet(what, number, text)
JSValue AudioSetFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::string what = WideToUtf8(a.S(0));
    const float number = a.F(1, 0.0f);
    const std::wstring text = a.S(2);
    if (a.failed) return JS_EXCEPTION;
    std::wstring error;
    if (!Self(ctx)->host_.AudioSet(what, number, text, error)) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_UNDEFINED;
}
JSValue MediaGetFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    std::string json;
    std::wstring error;
    const bool ok = Self(ctx)->host_.MediaGet(json, error);
    return JsonResult(ctx, ok, json, error, "<media>");
}
JSValue MediaCmdFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::string cmd = WideToUtf8(a.S(0));
    if (a.failed) return JS_EXCEPTION;
    std::wstring error;
    if (!Self(ctx)->host_.MediaCommand(cmd, error)) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_UNDEFINED;
}
JSValue MediaArtFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    std::string token;
    std::wstring error;
    if (!Self(ctx)->host_.MediaArt(token, error)) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return token.empty() ? JS_NULL : NewString(ctx, token);
}
// Round 23 phase 23A: mediaSeek(seconds).
JSValue MediaSeekFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const float seconds = a.F(0, 0.0f);
    if (a.failed) return JS_EXCEPTION;
    if (!std::isfinite(seconds)) return JS_ThrowTypeError(ctx, "media.seek: the position must be a number of seconds");
    std::wstring error;
    if (!Self(ctx)->host_.MediaSeek(seconds, error)) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_UNDEFINED;
}
JSValue NotifGetFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    std::string json;
    std::wstring error;
    const bool ok = Self(ctx)->host_.NotificationsGet(json, error);
    return JsonResult(ctx, ok, json, error, "<notifications>");
}
// notifCmd(command, id)
JSValue NotifCmdFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::string cmd = WideToUtf8(a.S(0));
    double id = 0.0;
    if (argc > 1 && JS_ToFloat64(ctx, &id, argv[1]) < 0) return JS_EXCEPTION;
    if (a.failed) return JS_EXCEPTION;
    std::wstring error;
    if (!Self(ctx)->host_.NotificationsCommand(cmd, id, error)) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_UNDEFINED;
}

// cal(what, argsJson): the calendar provider (permission "calendar").
JSValue CalFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::string what = WideToUtf8(a.S(0));
    const std::string args = a.Has(1) ? WideToUtf8(a.S(1)) : std::string("{}");
    if (a.failed) return JS_EXCEPTION;
    std::string json;
    std::wstring error;
    const bool ok = Self(ctx)->host_.CalendarCall(what, args, json, error);
    return JsonResult(ctx, ok, json, error, "<calendar>");
}

// weather(what, argsJson): the weather provider (permission "weather").
JSValue WeatherFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::string what = WideToUtf8(a.S(0));
    const std::string args = a.Has(1) ? WideToUtf8(a.S(1)) : std::string("{}");
    if (a.failed) return JS_EXCEPTION;
    std::string json;
    std::wstring error;
    const bool ok = Self(ctx)->host_.WeatherCall(what, args, json, error);
    return JsonResult(ctx, ok, json, error, "<weather>");
}

// perf(what, number): the perf provider (permission "perf", round 6 phase 6D).
JSValue PerfFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::string what = WideToUtf8(a.S(0));
    double number = 0.0;
    if (a.Has(1) && JS_ToFloat64(ctx, &number, argv[1]) < 0) return JS_EXCEPTION;
    if (a.failed) return JS_EXCEPTION;
    std::string json;
    std::wstring error;
    const bool ok = Self(ctx)->host_.PerfCall(what, number, json, error);
    return JsonResult(ctx, ok, json, error, "<perf>");
}
// perfSeries(key, n): up to n (0 - 600) newest values of a metric, oldest first, as numbers (NaN
// for a sample without the key).
JSValue PerfSeriesFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::string key = WideToUtf8(a.S(0));
    double n = 600.0;
    if (a.Has(1) && JS_ToFloat64(ctx, &n, argv[1]) < 0) return JS_EXCEPTION;
    if (a.failed) return JS_EXCEPTION;
    if (!std::isfinite(n) || n < 0.0) n = 0.0;
    std::vector<double> values;
    std::wstring error;
    if (!Self(ctx)->host_.PerfSeries(key, static_cast<size_t>(std::min(n, 600.0)), values, error)) {
        return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    }
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < values.size(); ++i) JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), JS_NewFloat64(ctx, values[i]));
    return arr;
}

// canvas.fileIcon(path, x, y, size): the shell's icon of a file or folder (permission "shell").
JSValue FileIconFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CANVAS_OR_THROW();
    if (!Self(ctx)->host_.Permitted(ScriptHost::Permission::Shell)) {
        return JS_ThrowTypeError(ctx, "canvas.fileIcon needs the \"shell\" permission");
    }
    const std::wstring path = a.S(0);
    const float x = a.F(1), y = a.F(2), size = a.F(3, 32.0f);
    if (a.failed) return JS_EXCEPTION;
    wchar_t expanded[MAX_PATH * 2]{};
    ExpandEnvironmentStringsW(path.c_str(), expanded, static_cast<DWORD>(std::size(expanded)));
    std::wstring error;
    if (!c->FileIcon(expanded, x, y, size, error)) return JS_ThrowTypeError(ctx, "fileIcon: %s", WideToUtf8(error).c_str());
    return JS_UNDEFINED;
}

// --- popup -----------------------------------------------------------------------------------------

JSValue PopupOpenFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    float w = 0, h = 0;
    bool passive = false;  // { passive: true }: the passive kind (no focus, mouse-transparent)
    if (JS_IsObject(a.At(0))) {
        if (!PropF(ctx, a.At(0), "width", w) || !PropF(ctx, a.At(0), "height", h) ||
            !PropB(ctx, a.At(0), "passive", passive)) {
            return JS_EXCEPTION;
        }
    } else {
        w = a.F(0);
        h = a.F(1);
        if (a.failed) return JS_EXCEPTION;
        if (JS_IsObject(a.At(2)) && !PropB(ctx, a.At(2), "passive", passive)) return JS_EXCEPTION;
    }
    std::wstring error;
    if (!Self(ctx)->host_.PopupOpen(w, h, passive, error)) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_UNDEFINED;
}
JSValue PopupCloseFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Self(ctx)->host_.PopupClose();
    return JS_UNDEFINED;
}
JSValue PopupRedrawFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Self(ctx)->host_.PopupRedraw();
    return JS_UNDEFINED;
}
JSValue PopupFolderFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::wstring path = a.S(0);
    const std::wstring view = a.S(1);  // optional: "auto" / "grid" / "list"
    if (a.failed) return JS_EXCEPTION;
    std::wstring error;
    if (!Self(ctx)->host_.PopupFolder(path, view, error)) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_UNDEFINED;
}
JSValue PopupPreviewsFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::wstring app = a.S(0);
    if (a.failed) return JS_EXCEPTION;
    std::wstring error;
    if (!Self(ctx)->host_.PopupPreviews(app, error)) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_UNDEFINED;
}
JSValue PopupCalendarFn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    std::wstring error;
    if (!Self(ctx)->host_.PopupCalendar(INT64_MIN, error)) return JS_ThrowTypeError(ctx, "%s", WideToUtf8(error).c_str());
    return JS_UNDEFINED;
}
JSValue PopupGet(JSContext* ctx, JSValueConst, int) { return JS_NewBool(ctx, Self(ctx)->host_.PopupShown()); }

// popup.editText({ title, text, multiline, maxLength, placeholder }) (round 6 phase 6C): a promise of
// the edited text, or null when the editor was cancelled. A bad argument, no editor here or an
// editor already open: a rejected promise.
JSValue EditTextFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    QuickJsEngine* self = Self(ctx);
    // type: 'T' TypeError, 'R' RangeError, anything else Error.
    auto reject = [&](char type, const std::string& message) {
        const std::string text = "popup.editText: " + message;
        JSValue e = JS_UNDEFINED;
        if (type == 'T' || type == 'R') {
            if (type == 'T') {
                JS_ThrowTypeError(ctx, "%s", text.c_str());
            } else {
                JS_ThrowRangeError(ctx, "%s", text.c_str());
            }
            e = JS_GetException(ctx);
        } else {
            e = self->MakeError(text);
        }
        return self->Rejected(e);
    };
    const JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    ScriptHost::EditTextRequest r;
    if (!JS_IsUndefined(o) && !JS_IsNull(o)) {
        if (!JS_IsObject(o) || JS_IsFunction(ctx, o) || JS_IsArray(o)) {
            return reject('T', "the argument must be an options object { title, text, multiline, maxLength, placeholder }");
        }
        // A string option: a string (absent / null: the default); anything else is refused.
        auto str = [&](const char* key, std::wstring& out, bool& bad) {
            JSValue v = JS_GetPropertyStr(ctx, o, key);
            if (JS_IsException(v)) return false;
            if (JS_IsString(v)) {
                out = ToWide(ctx, v);
            } else if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
                bad = true;
            }
            JS_FreeValue(ctx, v);
            return true;
        };
        bool bad = false;
        std::wstring title;
        if (!str("title", title, bad)) return JS_EXCEPTION;
        if (bad) return reject('T', "title must be a string");
        if (!title.empty()) r.title = title;
        if (!str("text", r.text, bad)) return JS_EXCEPTION;
        if (bad) return reject('T', "text must be a string");
        if (!str("placeholder", r.placeholder, bad)) return JS_EXCEPTION;
        if (bad) return reject('T', "placeholder must be a string");
        if (!PropB(ctx, o, "multiline", r.multiline)) return JS_EXCEPTION;
        JSValue ml = JS_GetPropertyStr(ctx, o, "maxLength");
        if (JS_IsException(ml)) return JS_EXCEPTION;
        if (!JS_IsUndefined(ml) && !JS_IsNull(ml)) {
            double d = 0;
            const bool number = JS_IsNumber(ml) && JS_ToFloat64(ctx, &d, ml) == 0;
            JS_FreeValue(ctx, ml);
            if (!number || !std::isfinite(d) || d < 1.0 || d > 1000000.0) {
                return reject('R', "maxLength must be a number from 1 to 1000000");
            }
            r.maxLength = static_cast<uint32_t>(std::floor(d));
        }
    }
    // One newline form ("\n"); a single-line editor gets spaces instead.
    std::wstring text;
    text.reserve(r.text.size());
    for (size_t i = 0; i < r.text.size(); ++i) {
        wchar_t c = r.text[i];
        if (c == L'\r') {
            if (i + 1 < r.text.size() && r.text[i + 1] == L'\n') ++i;
            c = L'\n';
        }
        if (c == L'\n' && !r.multiline) c = L' ';
        text.push_back(c);
    }
    r.text = std::move(text);
    if (r.text.size() > r.maxLength) return reject('R', "text is longer than maxLength");
    if (r.title.size() > 200) r.title.resize(200);
    if (r.placeholder.size() > 200) r.placeholder.resize(200);
    std::wstring error;
    const int id = self->host_.EditText(r, error);
    if (!id) return reject('E', WideToUtf8(error));
    return self->AddSettle(id);
}

// --- widget: the widget's own package files --------------------------------------------------------

// widget.files(pattern): the package's files matching a glob ("faces/*.json"; '*' and '?' stay
// inside one folder level), all of them without a pattern.
JSValue WidgetFilesFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::string pattern = a.Has(0) ? WideToUtf8(a.S(0)) : std::string();
    if (a.failed) return JS_EXCEPTION;
    JSValue arr = JS_NewArray(ctx);
    uint32_t n = 0;
    for (const std::string& f : Self(ctx)->host_.PackageFiles()) {
        if (pattern.empty() || Widgets::GlobMatch(pattern, f)) JS_SetPropertyUint32(ctx, arr, n++, NewString(ctx, f));
    }
    return arr;
}
// widget.read(path): a package file as text (UTF-8).
JSValue WidgetReadFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const std::wstring given = a.S(0);
    if (a.failed) return JS_EXCEPTION;
    std::string path;
    if (!Widgets::ResolvePath("", WideToUtf8(given), path)) {
        return JS_ThrowTypeError(ctx, "widget.read: '%s' is not a path inside the widget folder", WideToUtf8(given).c_str());
    }
    std::string bytes;
    std::wstring error;
    if (!Self(ctx)->host_.ReadFile(path, bytes, error)) {
        return JS_ThrowTypeError(ctx, "widget.read: %s", WideToUtf8(error).c_str());
    }
    return NewString(ctx, bytes);
}
// Round 23 phase 23A, widget.resize(w, h): a standalone widget's window size in DIPs (true: it
// will be applied; false: not standalone, or a preview).
JSValue WidgetResizeFn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Args a{ctx, argc, argv};
    const float w = a.F(0, 0.0f), h = a.F(1, 0.0f);
    if (a.failed) return JS_EXCEPTION;
    if (!std::isfinite(w) || !std::isfinite(h) || w <= 0.0f || h <= 0.0f) {
        return JS_ThrowRangeError(ctx, "widget.resize: the width and height must be positive numbers of DIPs");
    }
    return JS_NewBool(ctx, Self(ctx)->host_.ResizeWindow(w, h));
}

const JSCFunctionListEntry kStorageFuncs[] = {
    JS_CFUNC_DEF("get", 2, Guard<StorageGetFn>),
    JS_CFUNC_DEF("set", 2, Guard<StorageSetFn>),
    JS_CFUNC_DEF("remove", 1, Guard<StorageRemoveFn>),
    JS_CFUNC_DEF("keys", 0, Guard<StorageKeysFn>),
    JS_CFUNC_DEF("clear", 0, Guard<StorageClearFn>),
};

const JSCFunctionListEntry kShellFuncs[] = {
    JS_CFUNC_DEF("open", 2, Guard<ShellOpenFn>),
    JS_CFUNC_DEF("list", 1, Guard<ShellListFn>),
    JS_CFUNC_DEF("sendKeys", 1, Guard<SendKeysFn>),
};

const JSCFunctionListEntry kProviderFuncs[] = {
    JS_CFUNC_DEF("audioGet", 0, Guard<AudioGetFn>),
    JS_CFUNC_DEF("audioSet", 3, Guard<AudioSetFn>),
    JS_CFUNC_DEF("mediaGet", 0, Guard<MediaGetFn>),
    JS_CFUNC_DEF("mediaCmd", 1, Guard<MediaCmdFn>),
    JS_CFUNC_DEF("mediaArt", 0, Guard<MediaArtFn>),
    JS_CFUNC_DEF("mediaSeek", 1, Guard<MediaSeekFn>),
    JS_CFUNC_DEF("notifGet", 0, Guard<NotifGetFn>),
    JS_CFUNC_DEF("notifCmd", 2, Guard<NotifCmdFn>),
    JS_CFUNC_DEF("cal", 2, Guard<CalFn>),
    JS_CFUNC_DEF("weather", 2, Guard<WeatherFn>),
    JS_CFUNC_DEF("perf", 2, Guard<PerfFn>),
    JS_CFUNC_DEF("perfSeries", 2, Guard<PerfSeriesFn>),
};

const JSCFunctionListEntry kPopupFuncs[] = {
    JS_CFUNC_DEF("open", 2, Guard<PopupOpenFn>),
    JS_CFUNC_DEF("close", 0, Guard<PopupCloseFn>),
    JS_CFUNC_DEF("redraw", 0, Guard<PopupRedrawFn>),
    JS_CFUNC_DEF("folder", 2, Guard<PopupFolderFn>),
    JS_CFUNC_DEF("previews", 1, Guard<PopupPreviewsFn>),
    JS_CFUNC_DEF("calendar", 0, Guard<PopupCalendarFn>),
    JS_CFUNC_DEF("editText", 1, Guard<EditTextFn>),
    JS_CGETSET_MAGIC_DEF("isOpen", GuardGet<PopupGet>, nullptr, 0),
};

const JSCFunctionListEntry kWidgetFuncs[] = {
    JS_CFUNC_DEF("files", 1, Guard<WidgetFilesFn>),
    JS_CFUNC_DEF("read", 1, Guard<WidgetReadFn>),
    JS_CFUNC_DEF("resize", 2, Guard<WidgetResizeFn>),
};

const JSCFunctionListEntry kNativeFuncs[] = {
    JS_CFUNC_DEF("log", 2, Guard<LogFn>),
    JS_CFUNC_DEF("http", 6, Guard<HttpFn>),
};

bool QuickJsEngine::InstallApi() {
    JSValue global = JS_GetGlobalObject(ctx_);
    auto object = [&](const JSCFunctionListEntry* list, size_t n) {
        JSValue o = JS_NewObject(ctx_);
        JS_SetPropertyFunctionList(ctx_, o, list, static_cast<int>(n));
        return o;
    };
    // Path: methods on the class prototype.
    JSValue pathProto = object(kPathFuncs, std::size(kPathFuncs));
    JS_SetClassProto(ctx_, pathClass_, pathProto);  // takes it

    canvas_ = object(kCanvasFuncs, std::size(kCanvasFuncs));
    dock_ = object(kDockFuncs, std::size(kDockFuncs));
    JS_SetPropertyStr(ctx_, dock_, "theme", object(kThemeFuncs, std::size(kThemeFuncs)));
    JS_FreezeObject(ctx_, dock_);
    settings_ = ParseSettings();

    JS_SetPropertyFunctionList(ctx_, global, kGlobalFuncs, static_cast<int>(std::size(kGlobalFuncs)));
    JS_SetPropertyStr(ctx_, global, "timer", object(kTimerFuncs, std::size(kTimerFuncs)));
    JS_SetPropertyStr(ctx_, global, "system", object(kSystemFuncs, std::size(kSystemFuncs)));
    JS_SetPropertyStr(ctx_, global, "storage", object(kStorageFuncs, std::size(kStorageFuncs)));
    JS_SetPropertyStr(ctx_, global, "shell", object(kShellFuncs, std::size(kShellFuncs)));
    JS_SetPropertyStr(ctx_, global, "popup", object(kPopupFuncs, std::size(kPopupFuncs)));
    JS_SetPropertyStr(ctx_, global, "dock", JS_DupValue(ctx_, dock_));
    JS_SetPropertyStr(ctx_, global, "canvas", JS_DupValue(ctx_, canvas_));
    JS_SetPropertyStr(ctx_, global, "settings", JS_DupValue(ctx_, settings_));
    JSValue native = object(kNativeFuncs, std::size(kNativeFuncs));
    JS_SetPropertyStr(ctx_, native, "canvas", JS_DupValue(ctx_, canvas_));
    JS_SetPropertyStr(ctx_, native, "widget", object(kWidgetFuncs, std::size(kWidgetFuncs)));
    JS_SetPropertyStr(ctx_, native, "providers", object(kProviderFuncs, std::size(kProviderFuncs)));
    JS_SetPropertyStr(ctx_, global, "__windock", native);
    JS_FreeValue(ctx_, global);

    Enter(1000);
    JSValue r = JS_Eval(ctx_, kPrelude, sizeof(kPrelude) - 1, "<windock>", JS_EVAL_TYPE_GLOBAL);
    Leave();
    if (JS_IsException(r)) {
        host_.Log(ScriptLogLevel::Error, L"prelude: " + TakeException().error);
        return false;
    }
    // The prelude's value is the providers' emit(channel); the native object goes again.
    if (JS_IsFunction(ctx_, r)) {
        emit_ = r;
    } else {
        JS_FreeValue(ctx_, r);
    }
    JSValue g = JS_GetGlobalObject(ctx_);
    const JSAtom atom = JS_NewAtom(ctx_, "__windock");
    JS_DeleteProperty(ctx_, g, atom, 0);
    JS_FreeAtom(ctx_, atom);
    JS_FreeValue(ctx_, g);
    return true;
}

}  // namespace

std::unique_ptr<ScriptEngine> CreateQuickJsEngine(ScriptHost& host, const ScriptLimits& limits) {
    try {
        auto engine = std::make_unique<QuickJsEngine>(host, limits);
        if (!engine->Init()) return nullptr;
        return engine;
    } catch (...) {
        return nullptr;
    }
}
