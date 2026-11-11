#include "WidgetCanvas.h"

#include <shobjidl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <numbers>
#include <utility>

#include "JsonUtil.h"
#include "WeatherGlyphs.h"

namespace {

constexpr size_t kMaxImages = 32;
constexpr size_t kMaxImageBytes = 8u << 20;
constexpr UINT kMaxImagePx = 4096;
// Round 17 phase 17A: "file:" images (a photo anywhere on disk, canvas.image with an absolute path
// and the "shell" permission; the host caps the file at 64 MB). A photo is decoded at most
// kMaxPhotoPx on its long side (WIC's scaler; the JPEG decoder scales while decoding), after its
// EXIF orientation, and the decoded photos are kept within kPhotoBudget, least recently used first:
// a slideshow's next picture can be decoded ahead (imageSize) without the cache growing.
constexpr char kFileToken[] = "file:";
constexpr UINT kMaxPhotoPx = 2048;
constexpr UINT kMaxPhotoSourcePx = 24000;  // a raw size above this is refused (a 600 MP scan)
constexpr size_t kPhotoBudget = 64u << 20;  // 4 full-size photos (2048 x 2048 x 4)
constexpr size_t kMaxFormats = 16;
constexpr size_t kMaxStrokes = 16;
constexpr float kPi = std::numbers::pi_v<float>;

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool Finite(float v) { return std::isfinite(v); }

D2D1_COLOR_F ToD2D(const CanvasColor& c, float alpha) {
    return D2D1::ColorF(c.r, c.g, c.b, std::clamp(c.a * alpha, 0.0f, 1.0f));
}

}  // namespace

CanvasColor CanvasColorFromArgb(uint32_t argb) {
    CanvasColor c;
    c.a = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
    c.r = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
    c.g = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
    c.b = static_cast<float>(argb & 0xFF) / 255.0f;
    return c;
}

bool ParseCanvasColor(std::string_view text, CanvasColor& out) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text == "transparent") {
        out = CanvasColor{0, 0, 0, 0};
        return true;
    }
    if (!text.empty() && text[0] == '#') {
        text.remove_prefix(1);
        uint32_t v = 0;
        for (char c : text) {
            const int h = Hex(c);
            if (h < 0) return false;
            v = (v << 4) | static_cast<uint32_t>(h);
        }
        switch (text.size()) {
            case 3: {
                const uint32_t r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
                out = CanvasColorFromArgb(0xFF000000u | (r * 0x11u) << 16 | (g * 0x11u) << 8 | (b * 0x11u));
                return true;
            }
            case 6: out = CanvasColorFromArgb(0xFF000000u | v); return true;
            case 8: out = CanvasColorFromArgb(v); return true;
            default: return false;
        }
    }
    // rgb(r, g, b) / rgba(r, g, b, a)
    const bool rgba = text.rfind("rgba(", 0) == 0;
    if (!rgba && text.rfind("rgb(", 0) != 0) return false;
    if (text.back() != ')') return false;
    std::string inner(text.substr(rgba ? 5 : 4, text.size() - (rgba ? 6 : 5)));
    float v[4] = {0, 0, 0, 1};
    const int want = rgba ? 4 : 3;
    const char* p = inner.c_str();
    for (int i = 0; i < want; ++i) {
        char* end = nullptr;
        const double d = std::strtod(p, &end);
        if (end == p) return false;
        v[i] = static_cast<float>(d);
        p = end;
        while (*p == ' ') ++p;
        if (i + 1 < want) {
            if (*p != ',') return false;
            ++p;
        }
    }
    while (*p == ' ') ++p;
    if (*p) return false;
    out = CanvasColor{std::clamp(v[0], 0.0f, 255.0f) / 255.0f, std::clamp(v[1], 0.0f, 255.0f) / 255.0f,
                      std::clamp(v[2], 0.0f, 255.0f) / 255.0f, std::clamp(v[3], 0.0f, 1.0f)};
    return true;
}

// --- CanvasPath --------------------------------------------------------------------------------

void CanvasPath::Ensure() {
    if (open_) return;
    ops_.push_back(Cmd{Op::Move, {curX_, curY_}});
    startX_ = curX_;
    startY_ = curY_;
    open_ = true;
    hasPoint_ = true;
}

void CanvasPath::MoveTo(float x, float y) {
    ops_.push_back(Cmd{Op::Move, {x, y}});
    curX_ = startX_ = x;
    curY_ = startY_ = y;
    hasPoint_ = open_ = true;
    geometry_ = nullptr;
}

void CanvasPath::LineTo(float x, float y) {
    if (!hasPoint_) {
        MoveTo(x, y);
        return;
    }
    Ensure();
    ops_.push_back(Cmd{Op::Line, {x, y}});
    curX_ = x;
    curY_ = y;
    geometry_ = nullptr;
}

void CanvasPath::QuadTo(float cx, float cy, float x, float y) {
    if (!hasPoint_) MoveTo(cx, cy);
    Ensure();
    ops_.push_back(Cmd{Op::Quad, {cx, cy, x, y}});
    curX_ = x;
    curY_ = y;
    geometry_ = nullptr;
}

void CanvasPath::BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    if (!hasPoint_) MoveTo(c1x, c1y);
    Ensure();
    ops_.push_back(Cmd{Op::Bezier, {c1x, c1y, c2x, c2y, x, y}});
    curX_ = x;
    curY_ = y;
    geometry_ = nullptr;
}

void CanvasPath::Arc(float cx, float cy, float r, float start, float end, bool ccw) {
    r = std::fabs(r);
    float sweep = end - start;
    const float full = 2.0f * kPi;
    if (!ccw) {
        if (sweep >= full) sweep = full;
        else sweep = std::fmod(std::fmod(sweep, full) + full, full);
    } else {
        sweep = -sweep;
        if (sweep >= full) sweep = full;
        else sweep = std::fmod(std::fmod(sweep, full) + full, full);
        sweep = -sweep;
    }
    const float sx = cx + r * std::cos(start), sy = cy + r * std::sin(start);
    if (hasPoint_) {
        LineTo(sx, sy);
    } else {
        MoveTo(sx, sy);
    }
    if (r <= 0.0f || sweep == 0.0f) return;
    // Direct2D arcs cover less than a full turn: split into halves.
    const int parts = std::fabs(sweep) > kPi ? 2 : 1;
    const float step = sweep / static_cast<float>(parts);
    float a = start;
    for (int i = 0; i < parts; ++i) {
        a += step;
        const float x = cx + r * std::cos(a), y = cy + r * std::sin(a);
        // v: end x, end y, radius, clockwise, large arc
        ops_.push_back(Cmd{Op::ArcSeg, {x, y, r, step > 0 ? 1.0f : 0.0f, std::fabs(step) > kPi ? 1.0f : 0.0f}});
        curX_ = x;
        curY_ = y;
    }
    geometry_ = nullptr;
}

void CanvasPath::Rect(float x, float y, float w, float h) {
    MoveTo(x, y);
    LineTo(x + w, y);
    LineTo(x + w, y + h);
    LineTo(x, y + h);
    Close();
}

void CanvasPath::Close() {
    if (!open_) return;
    ops_.push_back(Cmd{Op::Close, {}});
    open_ = false;
    curX_ = startX_;
    curY_ = startY_;
    geometry_ = nullptr;
}

ID2D1PathGeometry* CanvasPath::Geometry(ID2D1Factory1* factory, bool evenOdd) {
    if (ops_.empty() || !factory) return nullptr;
    if (geometry_ && geometryEvenOdd_ == evenOdd) return geometry_.get();
    geometry_ = nullptr;
    winrt::com_ptr<ID2D1PathGeometry> g;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(g.put())) || FAILED(g->Open(sink.put()))) return nullptr;
    sink->SetFillMode(evenOdd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
    bool inFigure = false;
    for (const Cmd& c : ops_) {
        switch (c.op) {
            case Op::Move:
                if (inFigure) sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->BeginFigure(D2D1::Point2F(c.v[0], c.v[1]), D2D1_FIGURE_BEGIN_FILLED);
                inFigure = true;
                break;
            case Op::Line:
                if (inFigure) sink->AddLine(D2D1::Point2F(c.v[0], c.v[1]));
                break;
            case Op::Quad:
                if (inFigure) {
                    sink->AddQuadraticBezier(
                        D2D1::QuadraticBezierSegment(D2D1::Point2F(c.v[0], c.v[1]), D2D1::Point2F(c.v[2], c.v[3])));
                }
                break;
            case Op::Bezier:
                if (inFigure) {
                    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(c.v[0], c.v[1]), D2D1::Point2F(c.v[2], c.v[3]),
                                                        D2D1::Point2F(c.v[4], c.v[5])));
                }
                break;
            case Op::ArcSeg:
                if (inFigure) {
                    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.v[0], c.v[1]), D2D1::SizeF(c.v[2], c.v[2]), 0.0f,
                                                  c.v[3] > 0.5f ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                                                                : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                                  c.v[4] > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                }
                break;
            case Op::Close:
                if (inFigure) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                inFigure = false;
                break;
        }
    }
    if (inFigure) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return nullptr;
    geometry_ = std::move(g);
    geometryEvenOdd_ = evenOdd;
    return geometry_.get();
}

// --- WidgetCanvas ------------------------------------------------------------------------------

WidgetCanvas::WidgetCanvas(ID2D1Factory1* d2d, IDWriteFactory* dwrite, FileReader reader)
    : d2d_(d2d), dwrite_(dwrite), reader_(std::move(reader)) {}

void WidgetCanvas::Begin(ID2D1DeviceContext* dc, float scale, float width, float height) {
    layerOpen_ = layerLive_ = false;  // round 16 phase 16F: a new draw starts with no layer open
    inlineParents_.clear();
    Bind(dc, scale, width, height);
}

void WidgetCanvas::Bind(ID2D1DeviceContext* dc, float scale, float width, float height) {
    dc_ = dc;
    scale_ = scale > 0.0f ? scale : 1.0f;
    width_ = width;
    height_ = height;
    cur_ = State{};
    stack_.clear();
    clips_.clear();
    gradient_ = nullptr;
    if (!dc_) return;
    dc_->GetTransform(&dcBase_);
    base_ = D2D1::Matrix3x2F::Scale(scale_, scale_) * dcBase_;
    // The surface may sit in an atlas: never draw outside the widget's own box.
    dc_->SetTransform(base_);
    dc_->PushAxisAlignedClip(D2D1::RectF(0, 0, width_, height_), D2D1_ANTIALIAS_MODE_ALIASED);
    // Brushes belong to a device (it may have been recreated since the last draw).
    solid_ = nullptr;
}

void WidgetCanvas::End() {
    if (layerOpen_) EndLayer();  // the script forgot endLayer(): close it (the slot's state comes back)
    if (!dc_) return;
    while (!clips_.empty()) {
        if (clips_.back()) {
            dc_->PopLayer();
        } else {
            dc_->PopAxisAlignedClip();
        }
        clips_.pop_back();
    }
    dc_->SetTransform(base_);
    dc_->PopAxisAlignedClip();
    dc_->SetTransform(dcBase_);
    solid_ = nullptr;
    gradient_ = nullptr;
    dc_ = nullptr;
}

void WidgetCanvas::Apply() {
    if (dc_) dc_->SetTransform(cur_.m * base_);
}

// --- layers (round 16 phase 16F) ------------------------------------------------------------------

bool WidgetCanvas::BeginLayer(const CanvasLayerSpec& spec, std::wstring& error) {
    if (layerOpen_) {
        error = L"layer: a layer is already open (call endLayer() first; layers do not nest, use `parent`)";
        return false;
    }
    if (!Finite(spec.x + spec.y + spec.w + spec.h) || spec.w <= 0.0f || spec.h <= 0.0f) {
        error = L"layer: the box needs a positive width and height";
        return false;
    }
    layerOpen_ = true;
    layerLive_ = false;
    ID2D1DeviceContext* ldc = dc_ && sink_ ? sink_->BeginLayer(spec, scale_) : nullptr;
    if (ldc) {
        // The slot's state waits; the layer's recording context gets a fresh one of its own box.
        main_ = Main{dc_, dcBase_, base_, width_, height_, cur_, std::move(stack_), std::move(clips_)};
        Bind(ldc, scale_, spec.w, spec.h);
        layerLive_ = true;
        return true;
    }
    // Inline (no sink: a popup, a preview, measure(); or the host declined): the content is drawn
    // into the current surface at the tracks' first keys, inside the parent's box, with the
    // parent's opacity (an inline parent's clip is not inherited: a still picture, good enough).
    Save();
    InlineParent me{spec.x, spec.y, spec.opacity};
    if (!spec.parent.empty()) {
        const auto it = inlineParents_.find(spec.parent);
        if (it != inlineParents_.end()) {
            me.x += it->second.x;
            me.y += it->second.y;
            me.alpha *= it->second.alpha;
        }
    }
    inlineParents_[spec.id] = me;
    Translate(me.x, me.y);
    if (spec.clip) ClipRect(0.0f, 0.0f, spec.w, spec.h);
    Opacity(me.alpha);
    for (const CanvasTrack& t : spec.tracks) {
        if (t.keys.empty()) continue;
        const float v = t.keys.front().second;
        if (t.prop == "x") {
            Translate(v, 0.0f);
        } else if (t.prop == "y") {
            Translate(0.0f, v);
        } else if (t.prop == "rotate") {
            Rotate(v * kPi / 180.0f, t.cx, t.cy);
        } else if (t.prop == "opacity") {
            Opacity(v);
        }
    }
    return false;
}

bool WidgetCanvas::EndLayer() {
    if (!layerOpen_) return false;
    layerOpen_ = false;
    if (!layerLive_) {
        Restore();
        return true;
    }
    layerLive_ = false;
    End();  // the layer's context: its clips popped, its transform put back
    if (sink_) sink_->EndLayer();
    dc_ = main_.dc;
    dcBase_ = main_.dcBase;
    base_ = main_.base;
    width_ = main_.width;
    height_ = main_.height;
    cur_ = main_.cur;
    stack_ = std::move(main_.stack);
    clips_ = std::move(main_.clips);
    main_ = Main{};
    solid_ = nullptr;
    gradient_ = nullptr;
    Apply();
    return true;
}

ID2D1Brush* WidgetCanvas::Brush(const CanvasPaint& p) {
    if (!dc_) return nullptr;
    if (p.kind == CanvasPaint::Kind::Solid || p.stops.empty()) {
        const CanvasColor& c = p.kind == CanvasPaint::Kind::Solid || p.stops.empty() ? p.color : p.stops.front().color;
        if (!solid_) {
            if (FAILED(dc_->CreateSolidColorBrush(ToD2D(c, cur_.alpha), solid_.put()))) return nullptr;
        } else {
            solid_->SetColor(ToD2D(c, cur_.alpha));
        }
        return solid_.get();
    }
    std::vector<D2D1_GRADIENT_STOP> stops;
    stops.reserve(p.stops.size());
    for (const CanvasGradientStop& s : p.stops) {
        stops.push_back(D2D1::GradientStop(std::clamp(s.offset, 0.0f, 1.0f), ToD2D(s.color, cur_.alpha)));
    }
    winrt::com_ptr<ID2D1GradientStopCollection> collection;
    if (FAILED(dc_->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put()))) {
        return nullptr;
    }
    gradient_ = nullptr;
    if (p.kind == CanvasPaint::Kind::Linear) {
        winrt::com_ptr<ID2D1LinearGradientBrush> b;
        if (FAILED(dc_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(p.x0, p.y0), D2D1::Point2F(p.x1, p.y1)),
                collection.get(), b.put()))) {
            return nullptr;
        }
        gradient_ = b;
    } else {
        winrt::com_ptr<ID2D1RadialGradientBrush> b;
        if (FAILED(dc_->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(p.x0, p.y0), D2D1::Point2F(p.x1, p.y1), p.rx, p.ry),
                collection.get(), b.put()))) {
            return nullptr;
        }
        gradient_ = b;
    }
    return gradient_.get();
}

ID2D1StrokeStyle* WidgetCanvas::StrokeStyle(const CanvasStroke& s) {
    if (s.cap == D2D1_CAP_STYLE_FLAT && s.join == D2D1_LINE_JOIN_MITER && s.dashes.empty()) return nullptr;
    std::wstring key = std::to_wstring(static_cast<int>(s.cap)) + L"|" + std::to_wstring(static_cast<int>(s.join));
    for (float d : s.dashes) key += L"|" + std::to_wstring(d);
    if (auto it = strokes_.find(key); it != strokes_.end()) return it->second.get();
    if (strokes_.size() >= kMaxStrokes) strokes_.clear();
    D2D1_STROKE_STYLE_PROPERTIES props =
        D2D1::StrokeStyleProperties(s.cap, s.cap, s.cap, s.join, 10.0f,
                                    s.dashes.empty() ? D2D1_DASH_STYLE_SOLID : D2D1_DASH_STYLE_CUSTOM, 0.0f);
    winrt::com_ptr<ID2D1StrokeStyle> style;
    if (FAILED(d2d_->CreateStrokeStyle(props, s.dashes.empty() ? nullptr : s.dashes.data(),
                                       static_cast<UINT32>(s.dashes.size()), style.put()))) {
        return nullptr;
    }
    ID2D1StrokeStyle* raw = style.get();
    strokes_.emplace(std::move(key), std::move(style));
    return raw;
}

void WidgetCanvas::FillRect(float x, float y, float w, float h, const CanvasPaint& p) {
    if (!dc_ || !Finite(x + y + w + h)) return;
    Apply();
    if (ID2D1Brush* b = Brush(p)) dc_->FillRectangle(D2D1::RectF(x, y, x + w, y + h), b);
}

void WidgetCanvas::StrokeRect(float x, float y, float w, float h, const CanvasPaint& p, const CanvasStroke& s) {
    if (!dc_ || !Finite(x + y + w + h)) return;
    Apply();
    if (ID2D1Brush* b = Brush(p)) dc_->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), b, s.width, StrokeStyle(s));
}

void WidgetCanvas::FillRoundRect(float x, float y, float w, float h, float r, const CanvasPaint& p) {
    if (!dc_ || !Finite(x + y + w + h + r)) return;
    Apply();
    if (ID2D1Brush* b = Brush(p)) dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), b);
}

void WidgetCanvas::StrokeRoundRect(float x, float y, float w, float h, float r, const CanvasPaint& p,
                                   const CanvasStroke& s) {
    if (!dc_ || !Finite(x + y + w + h + r)) return;
    Apply();
    if (ID2D1Brush* b = Brush(p)) {
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), b, s.width, StrokeStyle(s));
    }
}

void WidgetCanvas::FillEllipse(float cx, float cy, float rx, float ry, const CanvasPaint& p) {
    if (!dc_ || !Finite(cx + cy + rx + ry)) return;
    Apply();
    if (ID2D1Brush* b = Brush(p)) dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), b);
}

void WidgetCanvas::StrokeEllipse(float cx, float cy, float rx, float ry, const CanvasPaint& p, const CanvasStroke& s) {
    if (!dc_ || !Finite(cx + cy + rx + ry)) return;
    Apply();
    if (ID2D1Brush* b = Brush(p)) {
        dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), b, s.width, StrokeStyle(s));
    }
}

void WidgetCanvas::Line(float x0, float y0, float x1, float y1, const CanvasPaint& p, const CanvasStroke& s) {
    if (!dc_ || !Finite(x0 + y0 + x1 + y1)) return;
    Apply();
    if (ID2D1Brush* b = Brush(p)) dc_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), b, s.width, StrokeStyle(s));
}

void WidgetCanvas::FillPath(CanvasPath& path, const CanvasPaint& p, bool evenOdd) {
    if (!dc_) return;
    ID2D1PathGeometry* g = path.Geometry(d2d_, evenOdd);
    if (!g) return;
    Apply();
    if (ID2D1Brush* b = Brush(p)) dc_->FillGeometry(g, b);
}

void WidgetCanvas::StrokePath(CanvasPath& path, const CanvasPaint& p, const CanvasStroke& s) {
    if (!dc_) return;
    ID2D1PathGeometry* g = path.Geometry(d2d_, false);
    if (!g) return;
    Apply();
    if (ID2D1Brush* b = Brush(p)) dc_->DrawGeometry(g, b, s.width, StrokeStyle(s));
}

IDWriteTextFormat* WidgetCanvas::Format(const CanvasTextStyle& st) {
    if (!dwrite_) return nullptr;
    const float size = std::clamp(st.size, 1.0f, 512.0f);
    const int weight = std::clamp(st.weight, 1, 999);
    std::wstring key = st.family + L"|" + std::to_wstring(size) + L"|" + std::to_wstring(weight) + (st.italic ? L"|i" : L"");
    if (auto it = formats_.find(key); it != formats_.end()) return it->second.get();
    if (formats_.size() >= kMaxFormats) formats_.clear();
    winrt::com_ptr<IDWriteTextFormat> format;
    if (FAILED(dwrite_->CreateTextFormat(st.family.empty() ? L"Segoe UI" : st.family.c_str(), nullptr,
                                         static_cast<DWRITE_FONT_WEIGHT>(weight),
                                         st.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"", format.put()))) {
        return nullptr;
    }
    IDWriteTextFormat* raw = format.get();
    formats_.emplace(std::move(key), std::move(format));
    return raw;
}

winrt::com_ptr<IDWriteTextLayout> WidgetCanvas::Layout(const std::wstring& text, const CanvasTextStyle& st,
                                                       CanvasTextMetrics& m) {
    m = {};
    IDWriteTextFormat* format = Format(st);
    if (!format || text.empty()) return nullptr;
    const bool wrap = st.maxWidth > 0.0f;
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(std::min<size_t>(text.size(), 4096)), format,
                                         wrap ? st.maxWidth : 100000.0f, 100000.0f, layout.put()))) {
        return nullptr;
    }
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    const DWRITE_TEXT_ALIGNMENT ta = st.align == 1   ? DWRITE_TEXT_ALIGNMENT_CENTER
                                     : st.align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                     : DWRITE_TEXT_ALIGNMENT_LEADING;
    if (wrap) layout->SetTextAlignment(ta);
    DWRITE_TEXT_METRICS tm{};
    if (FAILED(layout->GetMetrics(&tm))) return nullptr;
    m.width = tm.widthIncludingTrailingWhitespace;
    m.height = tm.height;
    m.lines = static_cast<int>(tm.lineCount);
    std::vector<DWRITE_LINE_METRICS> lines(std::max<UINT32>(1, tm.lineCount));
    UINT32 count = 0;
    if (SUCCEEDED(layout->GetLineMetrics(lines.data(), static_cast<UINT32>(lines.size()), &count)) && count >= 1) {
        m.baseline = lines[0].baseline;
    }
    if (!wrap && st.align != 0 && tm.lineCount > 1) {
        // Several lines ("\n") without a wrap width: align them inside the block.
        layout->SetMaxWidth(std::ceil(m.width));
        layout->SetTextAlignment(ta);
    }
    return layout;
}

CanvasTextMetrics WidgetCanvas::MeasureText(const std::wstring& text, const CanvasTextStyle& style) {
    CanvasTextMetrics m;
    Layout(text, style, m);
    return m;
}

void WidgetCanvas::Text(const std::wstring& text, float x, float y, const CanvasTextStyle& st, const CanvasPaint& p) {
    if (!dc_ || !Finite(x + y)) return;
    CanvasTextMetrics m;
    winrt::com_ptr<IDWriteTextLayout> layout = Layout(text, st, m);
    if (!layout) return;
    const float boxW = st.maxWidth > 0.0f ? st.maxWidth : m.width;
    float left = x;
    if (st.align == 1) left = x - boxW / 2.0f;
    if (st.align == 2) left = x - boxW;
    float top = y;
    switch (st.valign) {
        case 1: top = y - m.height / 2.0f; break;
        case 2: top = y - m.height; break;
        case 3: top = y - m.baseline; break;
        default: break;
    }
    Apply();
    if (ID2D1Brush* b = Brush(p)) dc_->DrawTextLayout(D2D1::Point2F(left, top), layout.get(), b);
}

IWICImagingFactory* WidgetCanvas::Wic() {
    if (!wic_ &&
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.put())))) {
        wic_ = nullptr;
    }
    return wic_.get();
}

namespace {

// Round 17 phase 17A: the EXIF orientation tag (274) of a decoded frame, 1 - 8; 1 (upright) when
// there is none. JPEG keeps it under /app1/ifd, TIFF under /ifd (WIC's metadata query paths).
UINT ExifOrientation(IWICBitmapFrameDecode* frame) {
    winrt::com_ptr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(reader.put())) || !reader) return 1;
    for (const wchar_t* query : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        PROPVARIANT v;
        PropVariantInit(&v);
        UINT result = 1;
        if (SUCCEEDED(reader->GetMetadataByName(query, &v))) {
            if (v.vt == VT_UI2) result = v.uiVal;
            else if (v.vt == VT_UI4) result = v.ulVal;
            else if (v.vt == VT_I2) result = static_cast<UINT>(v.iVal);
            else if (v.vt == VT_I4) result = static_cast<UINT>(v.lVal);
            PropVariantClear(&v);
            return result >= 1 && result <= 8 ? result : 1;
        }
        PropVariantClear(&v);
    }
    return 1;
}

}  // namespace

WidgetCanvas::ImageEntry* WidgetCanvas::FindImage(const std::string& path, std::wstring& error) {
    if (auto it = images_.find(path); it != images_.end()) {
        it->second.stamp = ++stamp_;
        return &it->second;
    }
    if (!reader_) {
        error = L"images are not available";
        return nullptr;
    }
    const bool file = path.rfind(kFileToken, 0) == 0;  // round 17 phase 17A: a photo from disk
    const std::wstring shown = JsonUtil::Utf8ToWide(file ? path.substr(std::strlen(kFileToken)) : path);
    std::string bytes;
    if (!reader_(path, bytes, error)) return nullptr;
    if (bytes.empty() || (!file && bytes.size() > kMaxImageBytes)) {
        error = L"image is empty or larger than 8 MB: " + shown;
        return nullptr;
    }
    IWICImagingFactory* wic = Wic();
    if (!wic) {
        error = L"WIC is not available";
        return nullptr;
    }
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(wic->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put()))) {
        error = L"cannot decode image: " + shown;
        return nullptr;
    }
    winrt::com_ptr<IWICBitmapSource> source = frame;
    UINT w = 0, h = 0;
    source->GetSize(&w, &h);
    if (w == 0 || h == 0 || (!file && (w > kMaxImagePx || h > kMaxImagePx)) || (file && (w > kMaxPhotoSourcePx || h > kMaxPhotoSourcePx))) {
        error = (file ? L"image is larger than 24000 x 24000: " : L"image is larger than 4096 x 4096: ") + shown;
        return nullptr;
    }
    if (file) {
        // Scale first (the JPEG decoder scales while it decodes: cheap), then turn the way the
        // camera held it, flip first then rotate (two rotators: the combined flags' order is not
        // documented, and the transposed orientations 5 and 7 depend on it).
        if (w > kMaxPhotoPx || h > kMaxPhotoPx) {
            const double k = static_cast<double>(kMaxPhotoPx) / std::max(w, h);
            const UINT sw = std::max<UINT>(1, static_cast<UINT>(std::lround(w * k)));
            const UINT sh = std::max<UINT>(1, static_cast<UINT>(std::lround(h * k)));
            winrt::com_ptr<IWICBitmapScaler> scaler;
            if (FAILED(wic->CreateBitmapScaler(scaler.put())) ||
                FAILED(scaler->Initialize(source.get(), sw, sh, WICBitmapInterpolationModeFant))) {
                error = L"cannot scale image: " + shown;
                return nullptr;
            }
            source = scaler;
            w = sw;
            h = sh;
        }
        const UINT orientation = ExifOrientation(frame.get());
        const WICBitmapTransformOptions flip = orientation == 2 || orientation == 5 || orientation == 7 ? WICBitmapTransformFlipHorizontal
                                               : orientation == 4                                       ? WICBitmapTransformFlipVertical
                                                                                                        : WICBitmapTransformRotate0;
        const WICBitmapTransformOptions rotate = orientation == 3                      ? WICBitmapTransformRotate180
                                                 : orientation == 6 || orientation == 7 ? WICBitmapTransformRotate90
                                                 : orientation == 5 || orientation == 8 ? WICBitmapTransformRotate270
                                                                                        : WICBitmapTransformRotate0;
        for (const WICBitmapTransformOptions t : {flip, rotate}) {
            if (t == WICBitmapTransformRotate0) continue;
            winrt::com_ptr<IWICBitmapFlipRotator> rotator;
            if (FAILED(wic->CreateBitmapFlipRotator(rotator.put())) || FAILED(rotator->Initialize(source.get(), t))) {
                error = L"cannot rotate image: " + shown;
                return nullptr;
            }
            source = rotator;
        }
        source->GetSize(&w, &h);
    }
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(source.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        error = L"cannot decode image: " + shown;
        return nullptr;
    }
    // Decode now (the memory stream's bytes go away with this call).
    winrt::com_ptr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmapFromSource(converter.get(), WICBitmapCacheOnLoad, bitmap.put()))) {
        error = L"cannot decode image: " + shown;
        return nullptr;
    }
    ImageEntry entry;
    entry.source = bitmap;
    entry.w = w;
    entry.h = h;
    entry.file = file;
    entry.bytes = static_cast<size_t>(w) * h * 4;
    entry.stamp = ++stamp_;
    if (file) {
        // Make room: the least recently used photos go first, package images stay.
        while (fileBytes_ + entry.bytes > kPhotoBudget) {
            auto victim = images_.end();
            for (auto it = images_.begin(); it != images_.end(); ++it) {
                if (it->second.file && (victim == images_.end() || it->second.stamp < victim->second.stamp)) victim = it;
            }
            if (victim == images_.end()) break;
            fileBytes_ -= victim->second.bytes;
            images_.erase(victim);
        }
        fileBytes_ += entry.bytes;
    } else if (images_.size() >= kMaxImages) {
        images_.clear();
        fileBytes_ = 0;
    }
    return &images_.emplace(path, std::move(entry)).first->second;
}

bool WidgetCanvas::ImageSize(const std::string& path, float& w, float& h, std::wstring& error) {
    if (path.rfind("weather:", 0) == 0) {  // weather.icon(): vector glyphs, 24 DIPs nominal
        w = h = 24.0f;
        return true;
    }
    ImageEntry* img = FindImage(path, error);
    if (!img) return false;
    w = static_cast<float>(img->w);
    h = static_cast<float>(img->h);
    return true;
}

namespace {

constexpr char kWeatherToken[] = "weather:";

// A weather.icon() token: "weather:icon/<glyph>/<d|n>/<ink ARGB hex>/<water ARGB hex>" or
// "weather:refresh/<ARGB hex>". The dock's own vector glyphs (WeatherGlyphs), drawn into the rect.
bool DrawWeatherToken(ID2D1DeviceContext* dc, const std::string& token, const D2D1_RECT_F& box, float opacity,
                      std::wstring& error) {
    std::vector<std::string> parts;
    size_t start = std::strlen(kWeatherToken);
    while (start <= token.size()) {
        size_t slash = token.find('/', start);
        if (slash == std::string::npos) slash = token.size();
        parts.push_back(token.substr(start, slash - start));
        start = slash + 1;
    }
    auto hex = [](const std::string& s, uint32_t& v) {
        if (s.size() != 8) return false;
        char* end = nullptr;
        v = static_cast<uint32_t>(std::strtoul(s.c_str(), &end, 16));
        return end && *end == 0;
    };
    if (parts.size() == 2 && parts[0] == "refresh") {
        uint32_t c = 0;
        if (!hex(parts[1], c)) {
            error = L"not a weather.icon() token";
            return false;
        }
        const uint32_t a = static_cast<uint32_t>(std::lround(((c >> 24) & 0xFF) * std::clamp(opacity, 0.0f, 1.0f)));
        if (dc) WeatherGlyphs::DrawRefresh(dc, box, (c & 0x00FFFFFFu) | (a << 24));
        return true;
    }
    using G = WeatherGlyphs::Glyph;
    static constexpr std::pair<const char*, G> kNames[] = {
        {"clear", G::Clear}, {"partly-cloudy", G::PartlyCloudy}, {"overcast", G::Overcast}, {"fog", G::Fog},
        {"drizzle", G::Drizzle}, {"rain", G::Rain}, {"snow", G::Snow}, {"thunder", G::Thunder},
        {"set-location", G::SetLocation}};
    WeatherGlyphs::Colors colors;
    const auto* name = parts.size() == 5 && parts[0] == "icon"
                           ? std::find_if(std::begin(kNames), std::end(kNames), [&](const auto& n) { return parts[1] == n.first; })
                           : std::end(kNames);
    if (name == std::end(kNames) || (parts[2] != "d" && parts[2] != "n") || !hex(parts[3], colors.ink) ||
        !hex(parts[4], colors.water)) {
        error = L"not a weather.icon() token";
        return false;
    }
    colors.opacity = std::clamp(opacity, 0.0f, 1.0f);
    if (dc) WeatherGlyphs::Draw(dc, name->second, parts[2] == "d", box, colors);
    return true;
}

}  // namespace

bool WidgetCanvas::Image(const std::string& path, float x, float y, float w, float h, float opacity,
                         std::wstring& error) {
    if (path.rfind(kWeatherToken, 0) == 0) {
        // Vector glyphs: no size of their own (0 / omitted: 24 DIPs square).
        if (w <= 0.0f || h <= 0.0f) w = h = 24.0f;
        if (!Finite(x + y + w + h)) return true;
        if (dc_) Apply();
        return DrawWeatherToken(dc_, path, D2D1::RectF(x, y, x + w, y + h), opacity * cur_.alpha, error);
    }
    ImageEntry* img = FindImage(path, error);
    if (!img) return false;
    if (!dc_) return true;
    winrt::com_ptr<ID2D1Device> device;
    dc_->GetDevice(device.put());
    if (!img->bitmap || img->device != device.get()) {
        img->bitmap = nullptr;
        if (FAILED(dc_->CreateBitmapFromWicBitmap(img->source.get(), nullptr, img->bitmap.put()))) {
            error = L"cannot create the image bitmap";
            return false;
        }
        img->device = device.get();
    }
    if (w <= 0.0f || h <= 0.0f) {
        w = static_cast<float>(img->w);
        h = static_cast<float>(img->h);
    }
    if (!Finite(x + y + w + h)) return true;
    Apply();
    const D2D1_RECT_F dst = D2D1::RectF(x, y, x + w, y + h);
    dc_->DrawBitmap(img->bitmap.get(), &dst, std::clamp(opacity * cur_.alpha, 0.0f, 1.0f),
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    return true;
}

bool WidgetCanvas::FileIcon(const std::wstring& path, float x, float y, float size, std::wstring& error) {
    if (!dc_) return true;
    if (!Finite(x + y + size) || size <= 0.0f) return true;
    const UINT px = static_cast<UINT>(std::clamp(std::lround(size * scale_), 8L, 256L));
    const std::string key = "icon|" + JsonUtil::WideToUtf8(path) + "|" + std::to_string(px);
    ImageEntry* img = nullptr;
    if (auto it = images_.find(key); it != images_.end()) img = &it->second;
    if (!img) {
        winrt::com_ptr<IShellItemImageFactory> factory;
        if (path.empty() || FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(factory.put())))) {
            error = L"no such file or folder: " + path;
            return false;
        }
        HBITMAP hbm = nullptr;
        if (FAILED(factory->GetImage(SIZE{static_cast<LONG>(px), static_cast<LONG>(px)},
                                     SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm)) ||
            !hbm) {
            error = L"no icon for " + path;
            return false;
        }
        IWICImagingFactory* wic = Wic();
        winrt::com_ptr<IWICBitmap> bitmap;
        const bool ok =
            wic && SUCCEEDED(wic->CreateBitmapFromHBITMAP(hbm, nullptr, WICBitmapUsePremultipliedAlpha, bitmap.put()));
        DeleteObject(hbm);
        winrt::com_ptr<IWICFormatConverter> converter;
        if (!ok || FAILED(wic->CreateFormatConverter(converter.put())) ||
            FAILED(converter->Initialize(bitmap.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                         0.0, WICBitmapPaletteTypeCustom))) {
            error = L"cannot convert the icon of " + path;
            return false;
        }
        winrt::com_ptr<IWICBitmap> converted;
        if (FAILED(wic->CreateBitmapFromSource(converter.get(), WICBitmapCacheOnLoad, converted.put()))) {
            error = L"cannot convert the icon of " + path;
            return false;
        }
        if (images_.size() >= kMaxImages) images_.clear();
        ImageEntry entry;
        entry.source = converted;
        converted->GetSize(&entry.w, &entry.h);
        img = &images_.emplace(key, std::move(entry)).first->second;
    }
    winrt::com_ptr<ID2D1Device> device;
    dc_->GetDevice(device.put());
    if (!img->bitmap || img->device != device.get()) {
        img->bitmap = nullptr;
        if (FAILED(dc_->CreateBitmapFromWicBitmap(img->source.get(), nullptr, img->bitmap.put()))) {
            error = L"cannot create the icon bitmap";
            return false;
        }
        img->device = device.get();
    }
    Apply();
    const D2D1_RECT_F dst = D2D1::RectF(x, y, x + size, y + size);
    dc_->DrawBitmap(img->bitmap.get(), &dst, std::clamp(cur_.alpha, 0.0f, 1.0f),
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    return true;
}

void WidgetCanvas::Save() {
    if (stack_.size() >= 64) return;  // a runaway save loop: ignore past 64 levels
    State s = cur_;
    s.clips = clips_.size();
    stack_.push_back(s);
}

bool WidgetCanvas::Restore() {
    if (stack_.empty()) return false;
    const State s = stack_.back();
    stack_.pop_back();
    while (dc_ && clips_.size() > s.clips) {
        if (clips_.back()) {
            dc_->PopLayer();
        } else {
            dc_->PopAxisAlignedClip();
        }
        clips_.pop_back();
    }
    cur_.m = s.m;
    cur_.alpha = s.alpha;
    return true;
}

void WidgetCanvas::Translate(float x, float y) {
    if (Finite(x + y)) cur_.m = D2D1::Matrix3x2F::Translation(x, y) * cur_.m;
}

void WidgetCanvas::Rotate(float radians, float cx, float cy) {
    if (!Finite(radians + cx + cy)) return;
    cur_.m = D2D1::Matrix3x2F::Rotation(radians * 180.0f / kPi, D2D1::Point2F(cx, cy)) * cur_.m;
}

void WidgetCanvas::ScaleBy(float sx, float sy) {
    if (Finite(sx + sy)) cur_.m = D2D1::Matrix3x2F::Scale(sx, sy) * cur_.m;
}

void WidgetCanvas::Opacity(float alpha) {
    if (Finite(alpha)) cur_.alpha = std::clamp(cur_.alpha * alpha, 0.0f, 1.0f);
}

void WidgetCanvas::ClipRect(float x, float y, float w, float h) {
    if (!dc_ || !Finite(x + y + w + h) || clips_.size() >= 64) return;
    Apply();
    // Axis-aligned in the current transform (rotated: its bounds).
    dc_->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    clips_.push_back(false);
}

void WidgetCanvas::ClipPath(CanvasPath& path) {
    if (!dc_ || clips_.size() >= 64) return;
    ID2D1PathGeometry* g = path.Geometry(d2d_, false);
    if (!g) return;
    Apply();
    dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), g, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                   nullptr);
    clips_.push_back(true);
}
