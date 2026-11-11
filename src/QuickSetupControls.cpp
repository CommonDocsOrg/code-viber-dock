// Round 13 phase 13H: Quick setup's painting kit and its choice control (see QuickSetupControls.h).

#include "QuickSetupControls.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>

namespace QsUi {

COLORREF Mix(COLORREF a, COLORREF b, int percentA) {
    percentA = std::clamp(percentA, 0, 100);
    return RGB((GetRValue(a) * percentA + GetRValue(b) * (100 - percentA)) / 100,
               (GetGValue(a) * percentA + GetGValue(b) * (100 - percentA)) / 100,
               (GetBValue(a) * percentA + GetBValue(b) * (100 - percentA)) / 100);
}

Colors ColorsFor(const DarkMode::Palette& p) {
    Colors k;
    k.dark = p.dark;
    k.highContrast = p.highContrast;
    k.text = p.text;
    k.muted = p.textSecondary;
    k.disabled = p.textDisabled;
    if (p.highContrast) {
        // One window colour, everything framed in the text colour, the selection in the highlight.
        k.base = k.rail = k.card = k.well = k.raised = p.layer;
        k.faint = p.text;
        k.accent = GetSysColor(COLOR_HIGHLIGHT);
        k.accentText = GetSysColor(COLOR_HIGHLIGHTTEXT);
        k.border = k.hair = p.text;
        k.knob = k.accentText;
        return k;
    }
    k.base = p.base;
    // The rail is the base a little toward black: #202020 -> #1A1A1A in dark (18 %); light keeps it
    // subtle (#F3 -> #E9) or the rail turns grey.
    k.rail = Mix(RGB(0, 0, 0), p.base, p.dark ? 18 : 4);
    k.card = p.layer;
    k.well = Mix(RGB(0, 0, 0), p.base, p.dark ? 3 : 4);
    k.raised = p.control;
    k.faint = Mix(p.text, p.base, p.dark ? 48 : 52);  // the prototype's #7A7A7A on #202020
    k.accent = p.accent;
    const int lum = (GetRValue(p.accent) * 299 + GetGValue(p.accent) * 587 + GetBValue(p.accent) * 114) / 1000;
    // Dark text on a light accent is the accent nearly black (the prototype's #06202C on #60CDFF).
    k.accentText = lum > 150 ? Mix(RGB(0, 0, 0), p.accent, 88) : RGB(0xFF, 0xFF, 0xFF);
    k.border = Mix(p.text, p.layer, p.dark ? 8 : 10);
    k.hair = Mix(p.text, p.base, 7);
    k.knob = p.dark ? p.text : p.control;
    return k;
}

// --- Buffer ---------------------------------------------------------------------------------------

Buffer::Buffer(HDC target, const RECT& rc, bool copyIn) : target_(target), rc_(rc) {
    w_ = std::max<int>(1, rc.right - rc.left);
    h_ = std::max<int>(1, rc.bottom - rc.top);
    dc_ = CreateCompatibleDC(target);
    bmp_ = dc_ ? CreateCompatibleBitmap(target, w_, h_) : nullptr;
    if (!dc_ || !bmp_) {
        // No memory: draw straight on the target (it flickers, nothing is lost).
        if (bmp_) DeleteObject(bmp_);
        if (dc_) DeleteDC(dc_);
        bmp_ = nullptr;
        dc_ = nullptr;
        return;
    }
    old_ = SelectObject(dc_, bmp_);
    if (copyIn) BitBlt(dc_, 0, 0, w_, h_, target, rc.left, rc.top, SRCCOPY);
}

Buffer::~Buffer() {
    if (!dc_) return;
    BitBlt(target_, rc_.left, rc_.top, w_, h_, dc_, 0, 0, SRCCOPY);
    SelectObject(dc_, old_);
    DeleteObject(bmp_);
    DeleteDC(dc_);
}

// --- Canvas ---------------------------------------------------------------------------------------

namespace {

struct D2DState {
    winrt::com_ptr<ID2D1Factory1> factory;
    winrt::com_ptr<ID2D1DCRenderTarget> target;
    winrt::com_ptr<ID2D1StrokeStyle> round, dash;
    bool busy = false;  // one canvas at a time (the target is shared)
};

D2DState& State() {
    static D2DState s;
    return s;
}

D2D1_COLOR_F Color(COLORREF c, float a) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a);
}

D2D1_COLOR_F Argb(uint32_t v) {
    return D2D1::ColorF(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, ((v >> 24) & 0xFF) / 255.0f);
}

D2D1_ROUNDED_RECT RR(const D2D1_RECT_F& r, float radius) {
    const float m = std::max(0.0f, std::min({radius, (r.right - r.left) / 2.0f, (r.bottom - r.top) / 2.0f}));
    return D2D1::RoundedRect(r, m, m);
}

}  // namespace

Canvas::Canvas(HDC dc, int w, int h) : hdc_(dc) {
    D2DState& st = State();
    if (st.busy || !dc || w <= 0 || h <= 0) return;
    if (!st.factory) {
        D2D1_FACTORY_OPTIONS options{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, st.factory.put_void()))) {
            st.factory = nullptr;
            return;
        }
        const D2D1_STROKE_STYLE_PROPERTIES round = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                               D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        if (FAILED(st.factory->CreateStrokeStyle(round, nullptr, 0, st.round.put()))) st.round = nullptr;
        const float dashes[] = {3.0f, 2.5f};  // in stroke widths: the prototype's 1.5 px dashed border
        const D2D1_STROKE_STYLE_PROPERTIES dash = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_CUSTOM);
        if (FAILED(st.factory->CreateStrokeStyle(dash, dashes, 2, st.dash.put()))) st.dash = nullptr;
    }
    if (!st.target) {
        // Software, alpha ignored: what GDI drew first is read back as opaque and the shapes blend
        // over it (a premultiplied target reads a GDI bitmap's zero alpha as transparent).
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        if (FAILED(st.factory->CreateDCRenderTarget(&props, st.target.put()))) {
            st.target = nullptr;
            return;
        }
    }
    const RECT rc{0, 0, w, h};
    if (FAILED(st.target->BindDC(dc, &rc))) return;
    winrt::com_ptr<ID2D1DeviceContext> d = st.target.try_as<ID2D1DeviceContext>();
    if (!d || FAILED(d->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush_.put()))) return;
    st.target->BeginDraw();
    st.busy = begun_ = true;
    d->SetTransform(D2D1::Matrix3x2F::Identity());
    d->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc_ = std::move(d);
}

void Canvas::End() {
    if (!begun_) return;
    D2DState& st = State();
    begun_ = st.busy = false;
    brush_ = nullptr;
    dc_ = nullptr;
    if (st.target && st.target->EndDraw() == D2DERR_RECREATE_TARGET) st.target = nullptr;  // made again on the next paint
}

void Canvas::FillRound(const D2D1_RECT_F& r, float radius, COLORREF c, float alpha) {
    if (!dc_) {
        // GDI's fallback: opaque, aliased; enough to keep the cards and buttons visible.
        if (!hdc_ || alpha < 0.5f) return;
        HBRUSH b = CreateSolidBrush(c);
        HPEN p = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ ob = SelectObject(hdc_, b), op = SelectObject(hdc_, p);
        const int d = static_cast<int>(radius * 2.0f);
        RoundRect(hdc_, static_cast<int>(r.left), static_cast<int>(r.top), static_cast<int>(r.right), static_cast<int>(r.bottom), d, d);
        SelectObject(hdc_, ob);
        SelectObject(hdc_, op);
        DeleteObject(p);
        DeleteObject(b);
        return;
    }
    brush_->SetColor(Color(c, alpha));
    dc_->FillRoundedRectangle(RR(r, radius), brush_.get());
}

void Canvas::StrokeRound(const D2D1_RECT_F& r, float radius, COLORREF c, float width, float alpha, bool dashed) {
    if (!dc_) return;
    brush_->SetColor(Color(c, alpha));
    // The stroke inside the rectangle (CSS borders are): inset by half its width.
    const float h = width / 2.0f;
    const D2D1_RECT_F in = D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h);
    dc_->DrawRoundedRectangle(RR(in, std::max(0.0f, radius - h)), brush_.get(), width, dashed ? State().dash.get() : nullptr);
}

void Canvas::FillArgb(const D2D1_RECT_F& r, float radius, uint32_t argb) {
    if (!dc_ || (argb >> 24) == 0) return;
    brush_->SetColor(Argb(argb));
    dc_->FillRoundedRectangle(RR(r, radius), brush_.get());
}

void Canvas::StrokeArgb(const D2D1_RECT_F& r, float radius, uint32_t argb, float width) {
    if (!dc_ || (argb >> 24) == 0 || width <= 0.0f) return;
    brush_->SetColor(Argb(argb));
    const float h = width / 2.0f;
    dc_->DrawRoundedRectangle(RR(D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h), std::max(0.0f, radius - h)), brush_.get(), width);
}

void Canvas::FillCircle(float cx, float cy, float radius, COLORREF c, float alpha) {
    if (!dc_) {
        FillRound(D2D1::RectF(cx - radius, cy - radius, cx + radius, cy + radius), radius, c, alpha);
        return;
    }
    brush_->SetColor(Color(c, alpha));
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush_.get());
}

void Canvas::StrokeCircle(float cx, float cy, float radius, COLORREF c, float width, float alpha) {
    if (!dc_) return;
    brush_->SetColor(Color(c, alpha));
    dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush_.get(), width);
}

void Canvas::Line(float x0, float y0, float x1, float y1, COLORREF c, float width, float alpha) {
    if (!dc_) return;
    brush_->SetColor(Color(c, alpha));
    dc_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush_.get(), width, State().round.get());
}

void Canvas::Polyline(const D2D1_POINT_2F* pts, int n, COLORREF c, float width, float alpha) {
    if (!dc_ || n < 2) return;
    D2DState& st = State();
    winrt::com_ptr<ID2D1PathGeometry> path;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(st.factory->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return;
    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
    for (int i = 1; i < n; ++i) sink->AddLine(pts[i]);
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return;
    brush_->SetColor(Color(c, alpha));
    dc_->DrawGeometry(path.get(), brush_.get(), width, st.round.get());
}

void Canvas::FillWallpaper(const D2D1_RECT_F& r, float radius) {
    if (!dc_) {
        FillRound(r, radius, RGB(0x1D, 0x2D, 0x4F));
        return;
    }
    // CSS radial-gradient(circle at 30% 25%, #3F74AB, #1D2D4F 55%, #121A2C): the circle reaches the
    // farthest corner (bottom right).
    const float w = r.right - r.left, h = r.bottom - r.top;
    const D2D1_POINT_2F c = D2D1::Point2F(r.left + 0.3f * w, r.top + 0.25f * h);
    const float rad = std::sqrt(0.49f * w * w + 0.5625f * h * h);
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(0x3F74AB)}, {0.55f, D2D1::ColorF(0x1D2D4F)}, {1.0f, D2D1::ColorF(0x121A2C)}};
    winrt::com_ptr<ID2D1GradientStopCollection> sc;
    winrt::com_ptr<ID2D1RadialGradientBrush> rb;
    if (SUCCEEDED(dc_->CreateGradientStopCollection(stops, static_cast<UINT32>(std::size(stops)), sc.put())) &&
        SUCCEEDED(dc_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, D2D1::Point2F(), rad, rad), sc.get(), rb.put()))) {
        dc_->FillRoundedRectangle(RR(r, radius), rb.get());
    }
}

void Canvas::FillGradient(const D2D1_RECT_F& r, float radius, COLORREF from, COLORREF to) {
    if (!dc_) {
        FillRound(r, radius, from);
        return;
    }
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, Color(from, 1.0f)}, {1.0f, Color(to, 1.0f)}};
    winrt::com_ptr<ID2D1GradientStopCollection> sc;
    winrt::com_ptr<ID2D1LinearGradientBrush> lb;
    if (SUCCEEDED(dc_->CreateGradientStopCollection(stops, 2, sc.put())) &&
        SUCCEEDED(dc_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)), sc.get(), lb.put()))) {
        dc_->FillRoundedRectangle(RR(r, radius), lb.get());
    }
}

// --- glyphs ---------------------------------------------------------------------------------------

void DrawGlyph(Canvas& c, Glyph g, const D2D1_RECT_F& box, COLORREF color, COLORREF cutout, float alpha) {
    // Each glyph in the prototype's SVG view box (`vb` units square), scaled into `box`.
    float vb = 16.0f;
    switch (g) {
        case Glyph::Check: vb = 13.0f; break;
        case Glyph::Close:
        case Glyph::Add: vb = 14.0f; break;
        case Glyph::DockMark:
        case Glyph::StartUp:
        case Glyph::Taskbar: vb = 18.0f; break;
        case Glyph::HideNever:
        case Glyph::HideAlways:
        case Glyph::HideOverlap: vb = 20.0f; break;
        default: break;
    }
    const float k = std::min(box.right - box.left, box.bottom - box.top) / vb;
    const float ox = (box.left + box.right) / 2.0f - vb * k / 2.0f, oy = (box.top + box.bottom) / 2.0f - vb * k / 2.0f;
    auto R = [&](float x, float y, float w, float h) { return D2D1::RectF(ox + x * k, oy + y * k, ox + (x + w) * k, oy + (y + h) * k); };
    auto P = [&](float x, float y) { return D2D1::Point2F(ox + x * k, oy + y * k); };
    auto line = [&](float x0, float y0, float x1, float y1, float w, float a = 1.0f) {
        c.Line(ox + x0 * k, oy + y0 * k, ox + x1 * k, oy + y1 * k, color, w * k, alpha * a);
    };
    auto monitor = [&](float rx) { c.StrokeRound(R(1.0f, 2.0f, 14.0f, 12.0f), rx * k, color, 1.2f * k, alpha); };
    switch (g) {
        case Glyph::Check: {
            const D2D1_POINT_2F pts[] = {P(2.5f, 7.0f), P(5.5f, 10.0f), P(10.5f, 4.0f)};
            c.Polyline(pts, 3, color, 2.0f * k, alpha);
            break;
        }
        case Glyph::Close:
            line(2, 2, 12, 12, 1.3f);
            line(12, 2, 2, 12, 1.3f);
            break;
        case Glyph::Add:
            line(7, 1, 7, 13, 1.6f);
            line(1, 7, 13, 7, 1.6f);
            break;
        case Glyph::Info:
            c.StrokeCircle(ox + 8 * k, oy + 8 * k, 6.5f * k, color, 1.3f * k, alpha);
            line(8, 7, 8, 11, 1.4f);
            line(8, 5, 8, 5.5f, 1.4f);
            break;
        case Glyph::DockMark:
            c.FillRound(R(2, 11, 14, 4), 2 * k, color, alpha);
            for (float x : {3.5f, 7.5f, 11.5f}) c.FillRound(R(x, 4, 3, 5), 1 * k, color, alpha);
            break;
        // The monitor: the SVG's rect (1.5, 2.5, 13 x 11) stroked 1.2 centred; StrokeRound strokes
        // inside its rectangle, so the outer edge is given (1.5 - 0.6 = 0.9 ~ 1).
        case Glyph::EdgeTop:
            monitor(2.1f);
            c.FillRound(R(3.5f, 4, 9, 2), 1 * k, color, alpha);
            break;
        case Glyph::EdgeBottom:
            monitor(2.1f);
            c.FillRound(R(3.5f, 10, 9, 2), 1 * k, color, alpha);
            break;
        case Glyph::EdgeLeft:
            monitor(2.1f);
            c.FillRound(R(3, 4.5f, 2, 7), 1 * k, color, alpha);
            break;
        case Glyph::EdgeRight:
            monitor(2.1f);
            c.FillRound(R(11, 4.5f, 2, 7), 1 * k, color, alpha);
            break;
        case Glyph::StartUp: {
            c.StrokeRound(R(1.35f, 2.35f, 15.3f, 11.3f), 2.1f * k, color, 1.3f * k, alpha);
            line(6, 16, 12, 16, 1.3f);
            line(9, 13, 9, 16, 1.3f);
            const D2D1_POINT_2F pts[] = {P(7, 8), P(8.5f, 9.5f), P(11, 6.5f)};
            c.Polyline(pts, 3, color, 1.4f * k, alpha);
            break;
        }
        case Glyph::Taskbar:
            c.StrokeRound(R(1.35f, 1.85f, 15.3f, 14.3f), 2.1f * k, color, 1.3f * k, alpha);
            line(2, 12.5f, 16, 12.5f, 1.3f);
            line(4, 10, 14, 4, 1.3f, 0.6f);
            break;
        case Glyph::HideNever:
        case Glyph::HideAlways:
        case Glyph::HideOverlap:
            c.StrokeRound(R(1.35f, 2.35f, 17.3f, 15.3f), 2.65f * k, color, 1.3f * k, alpha);
            if (g == Glyph::HideNever) {
                c.FillRound(R(5, 13, 10, 2.2f), 1 * k, color, alpha);
            } else if (g == Glyph::HideAlways) {
                c.FillRound(R(5, 16, 10, 2.2f), 1 * k, color, alpha);
                line(10, 9, 10, 13, 1.3f);
                const D2D1_POINT_2F pts[] = {P(8, 11), P(10, 13), P(12, 11)};
                c.Polyline(pts, 3, color, 1.3f * k, alpha);
            } else {
                c.FillRound(R(5, 13, 10, 2.2f), 1 * k, color, alpha * 0.45f);
                c.FillRound(R(6.35f, 5.35f, 10.3f, 9.3f), 1.65f * k, cutout, 1.0f);
                c.StrokeRound(R(6.35f, 5.35f, 10.3f, 9.3f), 1.65f * k, color, 1.3f * k, alpha);
            }
            break;
        default:
            break;
    }
}

// --- buttons --------------------------------------------------------------------------------------

namespace {

void Text(HDC dc, const std::wstring& text, RECT r, HFONT font, COLORREF color, UINT flags) {
    if (text.empty()) return;
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

int TextWidth(HDC dc, const std::wstring& text, HFONT font) {
    if (text.empty()) return 0;
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

void Focus(HDC dc, RECT r, int inset) {
    InflateRect(&r, -inset, -inset);
    SetTextColor(dc, RGB(0, 0, 0));
    SetBkColor(dc, RGB(0xFF, 0xFF, 0xFF));
    DrawFocusRect(dc, &r);
}

void FillBack(HDC dc, int w, int h, COLORREF back) {
    HBRUSH b = CreateSolidBrush(back);
    const RECT r{0, 0, w, h};
    FillRect(dc, &r, b);
    DeleteObject(b);
}

}  // namespace

void DrawButton(HDC target, const RECT& rc, ButtonKind kind, const std::wstring& text, Glyph glyph, UINT state, HFONT font,
                const Colors& k, COLORREF back, float s) {
    Buffer buf(target, rc);
    HDC dc = buf.Dc() ? buf.Dc() : target;
    const int w = buf.W(), h = buf.H();
    FillBack(dc, w, h, back);
    const bool hot = (state & CDIS_HOT) != 0, pressed = (state & CDIS_SELECTED) != 0, disabled = (state & CDIS_DISABLED) != 0;
    const bool hc = k.highContrast;
    const D2D1_RECT_F box = D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h));
    COLORREF ink = k.text;
    {
        Canvas c(dc, w, h);
        switch (kind) {
            case ButtonKind::Standard:
                c.FillRound(box, 6 * s, hc ? back : Tint(k.text, back, pressed ? 4 : hot ? 10 : 6));
                c.StrokeRound(box, 6 * s, hc && hot ? k.accent : hc ? k.text : Tint(k.text, back, 10), std::max(1.0f, std::round(s)));
                ink = disabled ? k.disabled : k.text;
                break;
            case ButtonKind::Primary:
                if (disabled) {
                    c.FillRound(box, 6 * s, hc ? back : Tint(k.text, back, 12));
                    if (hc) c.StrokeRound(box, 6 * s, k.disabled, std::max(1.0f, std::round(s)));
                    ink = k.disabled;
                } else {
                    // Hover and press as Windows 11's accent button: the accent at 90 / 80 % over the window.
                    c.FillRound(box, 6 * s, hc ? k.accent : Mix(k.accent, back, pressed ? 80 : hot ? 90 : 100));
                    if (hc && hot) c.StrokeRound(box, 6 * s, k.text, 2 * std::max(1.0f, std::round(s)));
                    ink = k.accentText;
                }
                break;
            case ButtonKind::Ghost:
                if (hot && !disabled) c.FillRound(box, 6 * s, hc ? back : Tint(k.text, back, pressed ? 4 : 6));
                if (hc && hot) c.StrokeRound(box, 6 * s, k.accent, std::max(1.0f, std::round(s)));
                ink = disabled ? k.disabled : hot ? k.text : k.muted;
                break;
            case ButtonKind::Dashed: {
                const bool lit = hot && !disabled;
                if (lit && !hc) c.FillRound(box, 10 * s, k.accent, 0.06f);
                c.StrokeRound(box, 10 * s, lit ? k.accent : hc ? k.text : Tint(k.text, back, disabled ? 9 : 18), 1.5f * s, 1.0f, true);
                ink = disabled ? k.disabled : lit ? k.text : k.muted;
                break;
            }
            case ButtonKind::Icon:
                if (hot && !disabled) c.FillRound(box, 6 * s, hc ? back : Tint(k.text, back, pressed ? 5 : 8));
                if (hc && hot) c.StrokeRound(box, 6 * s, k.accent, std::max(1.0f, std::round(s)));
                ink = disabled ? k.disabled : hot ? k.text : k.muted;
                break;
            case ButtonKind::Link:
                if (hot && !disabled) c.FillRound(box, 5 * s, k.accent, hc ? 1.0f : 0.10f);
                ink = disabled ? k.disabled : hc && hot ? k.accentText : k.accent;
                break;
        }
        // The glyph: alone (Icon), or before the text, the two centred together (Dashed).
        if (glyph != Glyph::None) {
            const float g = std::round(14.0f * s);
            const int tw = TextWidth(dc, text, font);
            const float total = g + (tw ? 8.0f * s + static_cast<float>(tw) : 0.0f);
            const float x = std::round((static_cast<float>(w) - total) / 2.0f), y = std::round((static_cast<float>(h) - g) / 2.0f);
            DrawGlyph(c, glyph, D2D1::RectF(x, y, x + g, y + g), ink);
        }
    }
    if (!text.empty()) {
        RECT t{0, 0, w, h};
        if (glyph != Glyph::None) {
            const int g = static_cast<int>(std::round(14.0f * s)), tw = TextWidth(dc, text, font);
            t.left = (w - (g + static_cast<int>(8.0f * s) + tw)) / 2 + g + static_cast<int>(8.0f * s);
            Text(dc, text, t, font, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            InflateRect(&t, -static_cast<int>(6 * s), 0);
            Text(dc, text, t, font, ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    if ((state & CDIS_FOCUS) && (state & CDIS_SHOWKEYBOARDCUES)) Focus(dc, RECT{0, 0, w, h}, std::max(2, static_cast<int>(3 * s)));
}

void DrawSwitch(HDC target, const RECT& rc, bool on, UINT state, const Colors& k, COLORREF back, float s) {
    Buffer buf(target, rc);
    HDC dc = buf.Dc() ? buf.Dc() : target;
    const int w = buf.W(), h = buf.H();
    FillBack(dc, w, h, back);
    const bool hot = (state & (CDIS_HOT | CDIS_SELECTED)) != 0, disabled = (state & CDIS_DISABLED) != 0;
    const float tw = std::round(44.0f * s), th = std::round(24.0f * s);
    const float x = std::round((static_cast<float>(w) - tw) / 2.0f), y = std::round((static_cast<float>(h) - th) / 2.0f);
    const D2D1_RECT_F track = D2D1::RectF(x, y, x + tw, y + th);
    {
        Canvas c(dc, w, h);
        const float one = std::max(1.0f, std::round(s));
        const float knobR = (hot && !disabled ? 8.0f : 7.0f) * s;
        if (on) {
            c.FillRound(track, th / 2.0f, disabled ? k.disabled : k.accent);
            c.FillCircle(x + tw - 4.0f * s - 7.0f * s, y + th / 2.0f, knobR, disabled ? back : k.accentText);
        } else {
            // Off: a ring and a grey knob (the prototype's white 30 % / #CFCFCF on dark).
            const COLORREF ring = disabled ? k.disabled : k.highContrast ? k.text : Mix(k.text, back, k.dark ? 30 : 55);
            if (hot && !disabled && !k.highContrast) c.FillRound(track, th / 2.0f, Tint(k.text, back, 5));
            c.StrokeRound(track, th / 2.0f, ring, one);
            const COLORREF knob = disabled ? k.disabled : k.highContrast ? k.text : Mix(k.text, back, k.dark ? 80 : 62);
            c.FillCircle(x + 4.0f * s + 7.0f * s, y + th / 2.0f, knobR, knob);
        }
    }
    if ((state & CDIS_FOCUS) && (state & CDIS_SHOWKEYBOARDCUES)) {
        Focus(dc, RECT{static_cast<int>(track.left) - 3, static_cast<int>(track.top) - 3, static_cast<int>(track.right) + 3,
                       static_cast<int>(track.bottom) + 3},
              0);
    }
}

LRESULT DrawSlider(NMCUSTOMDRAW& cd, const Colors& k, COLORREF back, float s) {
    // Everything at the channel's turn (the first item the trackbar draws), the thumb's and the
    // ticks' turns skipped: the groove and the thumb overlap, and one buffer keeps them together.
    if (cd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    if (cd.dwItemSpec != TBCD_CHANNEL) return CDRF_SKIPDEFAULT;
    HWND tb = cd.hdr.hwndFrom;
    RECT client{}, channel{}, thumb{};
    GetClientRect(tb, &client);
    SendMessageW(tb, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    SendMessageW(tb, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    const bool enabled = IsWindowEnabled(tb) != FALSE;
    Buffer buf(cd.hdc, client);
    HDC dc = buf.Dc() ? buf.Dc() : cd.hdc;
    FillBack(dc, buf.W(), buf.H(), back);
    {
        Canvas c(dc, buf.W(), buf.H());
        const float cy = (thumb.top + thumb.bottom) / 2.0f, tx = (thumb.left + thumb.right) / 2.0f;
        const float gh = std::max(2.0f, std::round(4.0f * s));
        const COLORREF fill = enabled ? k.accent : k.disabled;
        const float left = static_cast<float>(channel.left), right = static_cast<float>(channel.right);
        c.FillRound(D2D1::RectF(left, cy - gh / 2, right, cy + gh / 2), gh / 2, k.highContrast ? k.text : Tint(k.text, back, 14));
        c.FillRound(D2D1::RectF(left, cy - gh / 2, std::max(left + gh, tx), cy + gh / 2), gh / 2, fill);
        const bool hot = (cd.uItemState & (CDIS_HOT | CDIS_SELECTED)) != 0 || GetCapture() == tb;
        c.FillCircle(tx, cy, 9.0f * s, fill);
        c.FillCircle(tx, cy, (hot ? 4.0f : 5.0f) * s, k.knob);
    }
    if (GetFocus() == tb && !(SendMessageW(tb, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) Focus(dc, RECT{0, 0, buf.W(), buf.H()}, 0);
    return CDRF_SKIPDEFAULT;
}

// --- Choice ---------------------------------------------------------------------------------------

namespace {

struct ChoiceState {
    Choice::Style style = Choice::Style::Segments;
    std::vector<Choice::Option> options;
    int selected = -1;
    int hot = -1;
    bool tracking = false;
    bool hasLook = false;
    Choice::Look look;
};

ChoiceState* StateOf(HWND hwnd) { return reinterpret_cast<ChoiceState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); }

float ScaleOf(HWND hwnd) { return static_cast<float>(GetDpiForWindow(hwnd)) / USER_DEFAULT_SCREEN_DPI; }

RECT OptionRect(HWND hwnd, const ChoiceState& st, int i) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int n = std::max(1, static_cast<int>(st.options.size()));
    const float s = ScaleOf(hwnd);
    if (st.style == Choice::Style::Segments) {
        const int pad = static_cast<int>(std::round(3 * s)), gap = static_cast<int>(std::round(2 * s));
        const int inner = rc.right - 2 * pad - (n - 1) * gap;
        const int x = pad + i * inner / n + i * gap, x1 = pad + (i + 1) * inner / n + i * gap;
        return RECT{x, pad, x1, rc.bottom - pad};
    }
    const int gap = static_cast<int>(std::round(10 * s));
    const int inner = rc.right - (n - 1) * gap;
    return RECT{i * inner / n + i * gap, 0, (i + 1) * inner / n + i * gap, rc.bottom};
}

int HitOption(HWND hwnd, const ChoiceState& st, POINT pt) {
    for (int i = 0; i < static_cast<int>(st.options.size()); ++i) {
        const RECT r = OptionRect(hwnd, st, i);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

void ChooseOption(HWND hwnd, ChoiceState& st, int i) {
    if (i < 0 || i >= static_cast<int>(st.options.size()) || !IsWindowEnabled(hwnd)) return;
    if (i == st.selected) return;
    st.selected = i;
    InvalidateRect(hwnd, nullptr, FALSE);
    SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), Choice::kChosen), reinterpret_cast<LPARAM>(hwnd));
}

void PaintChoice(HWND hwnd, const ChoiceState& st, HDC target) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    Buffer buf(target, rc);
    HDC dc = buf.Dc() ? buf.Dc() : target;
    const Colors& k = st.look.colors;
    const COLORREF back = st.look.back;
    FillBack(dc, buf.W(), buf.H(), back);
    if (!st.hasLook) return;
    const float s = ScaleOf(hwnd);
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const bool focused = GetFocus() == hwnd && !(SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    const bool hc = k.highContrast;
    const int n = static_cast<int>(st.options.size());
    std::vector<COLORREF> fills(static_cast<size_t>(n), back);
    std::vector<COLORREF> inks(static_cast<size_t>(n), k.muted);
    {
        Canvas c(dc, buf.W(), buf.H());
        if (st.style == Choice::Style::Segments) {
            const D2D1_RECT_F outer = F(rc);
            c.FillRound(outer, 7 * s, k.well);
            c.StrokeRound(outer, 7 * s, hc ? k.text : Tint(k.text, back, 10), std::max(1.0f, std::round(s)));
        }
        for (int i = 0; i < n; ++i) {
            const RECT r = OptionRect(hwnd, st, i);
            const bool on = i == st.selected, hot = i == st.hot && enabled;
            const Choice::Option& o = st.options[static_cast<size_t>(i)];
            if (st.style == Choice::Style::Segments) {
                COLORREF fill = k.well;
                if (on) {
                    fill = hc ? k.accent : k.raised;
                    c.FillRound(F(r), 5 * s, fill);
                    if (!hc) c.StrokeRound(F(r), 5 * s, Tint(k.text, fill, k.dark ? 6 : 12), std::max(1.0f, std::round(s)));
                } else if (hot) {
                    fill = hc ? k.well : Tint(k.text, k.well, 6);
                    c.FillRound(F(r), 5 * s, fill);
                    if (hc) c.StrokeRound(F(r), 5 * s, k.accent, std::max(1.0f, std::round(s)));
                }
                const COLORREF ink = !enabled ? k.disabled : on && hc ? k.accentText : on || hot ? k.text : k.muted;
                inks[static_cast<size_t>(i)] = ink;
                fills[static_cast<size_t>(i)] = fill;
                if (o.glyph != Glyph::None) {
                    const float g = std::round(16.0f * s);
                    float x = std::round((r.left + r.right - g) / 2.0f);
                    if (!o.text.empty()) x = static_cast<float>(r.left) + std::round(8.0f * s);
                    const float y = std::round((r.top + r.bottom - g) / 2.0f);
                    DrawGlyph(c, o.glyph, D2D1::RectF(x, y, x + g, y + g), !enabled ? k.disabled : on && !hc ? k.accent : ink, fill);
                }
            } else {
                const COLORREF fill = on ? (hc ? back : Mix(k.accent, k.card, 8)) : hot && !hc ? Tint(k.text, k.card, 4) : k.card;
                fills[static_cast<size_t>(i)] = fill;
                c.FillRound(F(r), 10 * s, fill);
                const COLORREF frame = on ? k.accent : hot && hc ? k.accent : hc ? k.text : Tint(k.text, k.card, 10);
                c.StrokeRound(F(r), 10 * s, enabled ? frame : Tint(k.text, k.card, 8), (on && hc ? 3.0f : 1.5f) * s);
                const float g = std::round(20.0f * s);
                const float x = static_cast<float>(r.left) + std::round(14.0f * s), y = static_cast<float>(r.top) + std::round(14.0f * s);
                DrawGlyph(c, o.glyph, D2D1::RectF(x, y, x + g, y + g), !enabled ? k.disabled : on ? k.accent : k.muted, fill);
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        const RECT r = OptionRect(hwnd, st, i);
        const Choice::Option& o = st.options[static_cast<size_t>(i)];
        const bool on = i == st.selected;
        if (st.style == Choice::Style::Segments) {
            if (!o.text.empty()) {
                RECT t = r;
                if (o.glyph != Glyph::None) t.left += static_cast<LONG>(std::round(30.0f * s));
                Text(dc, o.text, t, st.look.font, inks[static_cast<size_t>(i)],
                     (o.glyph != Glyph::None ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        } else {
            const int pad = static_cast<int>(std::round(14.0f * s)), g = static_cast<int>(std::round(20.0f * s));
            const COLORREF title = enabled ? k.text : k.disabled, detail = enabled ? k.muted : k.disabled;
            TEXTMETRICW tm{};
            HGDIOBJ old = SelectObject(dc, st.look.bold);
            GetTextMetricsW(dc, &tm);
            SelectObject(dc, old);
            int y;
            if (st.look.compactTiles) {
                RECT t{r.left + pad + g + static_cast<int>(8 * s), r.top + pad, r.right - pad, r.top + pad + g};
                Text(dc, o.text, t, st.look.bold, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                y = r.top + pad + g + static_cast<int>(6 * s);
            } else {
                RECT t{r.left + pad, r.top + pad + g + static_cast<int>(6 * s), r.right - pad, 0};
                t.bottom = t.top + tm.tmHeight;
                Text(dc, o.text, t, st.look.bold, title, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
                y = t.bottom + static_cast<int>(4 * s);
            }
            RECT d{r.left + pad, y, r.right - pad, r.bottom - pad / 2};
            Text(dc, o.detail, d, st.look.smallFont, detail, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
        }
        if (focused && (on || (st.selected < 0 && i == 0))) Focus(dc, r, st.style == Choice::Style::Segments ? 2 : static_cast<int>(4 * s));
    }
}

LRESULT CALLBACK ChoiceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* st = new ChoiceState();
        if (const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp); cs->lpCreateParams) {
            st->style = *static_cast<const Choice::Style*>(cs->lpCreateParams);
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    ChoiceState* st = StateOf(hwnd);
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete st;
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            PaintChoice(hwnd, *st, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintChoice(hwnd, *st, reinterpret_cast<HDC>(wp));
            return 0;
        case WM_SIZE:
        case WM_ENABLE:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETFOCUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), Choice::kFocused), reinterpret_cast<LPARAM>(hwnd));
            return 0;
        case WM_UPDATEUISTATE: {
            const LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
            InvalidateRect(hwnd, nullptr, FALSE);
            return r;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN: {
            const int n = static_cast<int>(st->options.size());
            if (n == 0) return 0;
            int next = st->selected;
            switch (wp) {
                case VK_LEFT:
                case VK_UP: next = st->selected < 0 ? 0 : std::max(0, st->selected - 1); break;
                case VK_RIGHT:
                case VK_DOWN: next = st->selected < 0 ? 0 : std::min(n - 1, st->selected + 1); break;
                case VK_HOME: next = 0; break;
                case VK_END: next = n - 1; break;
                default: return DefWindowProcW(hwnd, msg, wp, lp);
            }
            // The keyboard focus shows from now on (a click alone keeps it hidden, as dialogs do).
            SendMessageW(hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
            ChooseOption(hwnd, *st, next);
            return 0;
        }
        case WM_CHAR:
            if (wp == L' ' && st->selected < 0) ChooseOption(hwnd, *st, 0);
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            ChooseOption(hwnd, *st, HitOption(hwnd, *st, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}));
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!st->tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                st->tracking = TrackMouseEvent(&tme) != FALSE;
            }
            const int hit = HitOption(hwnd, *st, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hit != st->hot) {
                st->hot = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            st->tracking = false;
            if (st->hot >= 0) {
                st->hot = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case Choice::kMsgChoose:
            if (static_cast<int>(wp) < 0 || static_cast<int>(wp) >= static_cast<int>(st->options.size())) return 0;
            ChooseOption(hwnd, *st, static_cast<int>(wp));
            return 1;
        case Choice::kMsgSelected:
            return st->selected;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool Choice::Register(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ChoiceProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND Choice::Create(HWND parent, int id, Style style, const wchar_t* name, HINSTANCE instance) {
    return CreateWindowExW(0, kClassName, name, WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, &style);
}

void Choice::SetOptions(HWND hwnd, std::vector<Option> options) {
    ChoiceState* st = hwnd ? StateOf(hwnd) : nullptr;
    if (!st) return;
    st->options = std::move(options);
    st->selected = std::min(st->selected, static_cast<int>(st->options.size()) - 1);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void Choice::SetSelected(HWND hwnd, int index) {
    ChoiceState* st = hwnd ? StateOf(hwnd) : nullptr;
    if (!st) return;
    index = index < 0 || index >= static_cast<int>(st->options.size()) ? -1 : index;
    if (index == st->selected) return;
    st->selected = index;
    InvalidateRect(hwnd, nullptr, FALSE);
}

int Choice::Selected(HWND hwnd) {
    const ChoiceState* st = hwnd ? StateOf(hwnd) : nullptr;
    return st ? st->selected : -1;
}

void Choice::SetLook(HWND hwnd, const Look& look) {
    ChoiceState* st = hwnd ? StateOf(hwnd) : nullptr;
    if (!st) return;
    st->look = look;
    st->hasLook = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

int Choice::TilesHeight(HWND hwnd) {
    const ChoiceState* st = hwnd ? StateOf(hwnd) : nullptr;
    if (!st || st->options.empty() || !st->hasLook) return 0;
    const float s = ScaleOf(hwnd);
    const int pad = static_cast<int>(std::round(14.0f * s)), g = static_cast<int>(std::round(20.0f * s));
    HDC dc = GetDC(hwnd);
    TEXTMETRICW tm{};
    HGDIOBJ old = SelectObject(dc, st->look.bold);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, st->look.smallFont);
    int detail = 0;
    for (int i = 0; i < static_cast<int>(st->options.size()); ++i) {
        const RECT r = OptionRect(hwnd, *st, i);
        RECT m{0, 0, std::max<LONG>(1, r.right - r.left - 2 * pad), 0};
        const std::wstring& t = st->options[static_cast<size_t>(i)].detail;
        if (!t.empty()) DrawTextW(dc, t.c_str(), static_cast<int>(t.size()), &m, DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        detail = std::max(detail, static_cast<int>(m.bottom));
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    const int head = st->look.compactTiles ? g + static_cast<int>(6 * s) : g + static_cast<int>(6 * s) + tm.tmHeight + static_cast<int>(4 * s);
    return pad + head + detail + pad;
}

}  // namespace QsUi
