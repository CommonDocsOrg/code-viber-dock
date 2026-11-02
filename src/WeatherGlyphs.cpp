#include "WeatherGlyphs.h"

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <iterator>

#include "Theme.h"

namespace WeatherGlyphs {

namespace {

constexpr float kPi = 3.14159265358979f;

struct Circle {
    float x, y, r;
};

// The upper (smaller y) intersection of two circles (they must intersect).
D2D1_POINT_2F UpperIntersection(const Circle& a, const Circle& b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float d = std::max(1e-4f, std::sqrt(dx * dx + dy * dy));
    const float along = (a.r * a.r - b.r * b.r + d * d) / (2.0f * d);
    const float h = std::sqrt(std::max(0.0f, a.r * a.r - along * along));
    const float mx = a.x + along * dx / d, my = a.y + along * dy / d;
    const D2D1_POINT_2F p1{mx + h * dy / d, my - h * dx / d};
    const D2D1_POINT_2F p2{mx - h * dy / d, my + h * dx / d};
    return p1.y < p2.y ? p1 : p2;
}

// An arc of circle `c` from `from` to `to`, visually counter-clockwise (y points down).
void ArcCcw(ID2D1GeometrySink* sink, const Circle& c, D2D1_POINT_2F from, D2D1_POINT_2F to) {
    const float a0 = std::atan2(from.y - c.y, from.x - c.x);
    const float a1 = std::atan2(to.y - c.y, to.x - c.x);
    float sweep = a0 - a1;  // counter-clockwise on screen = decreasing angle
    while (sweep < 0.0f) sweep += 2.0f * kPi;
    sink->AddArc(D2D1::ArcSegment(to, D2D1::SizeF(c.r, c.r), 0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                  sweep > kPi ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
}

class Painter {
public:
    Painter(ID2D1RenderTarget* rt, const D2D1_RECT_F& box, const Colors& colors) : rt_(rt), colors_(colors) {
        x0_ = box.left;
        y0_ = box.top;
        w_ = std::min(box.right - box.left, box.bottom - box.top);
        x0_ += ((box.right - box.left) - w_) / 2.0f;
        y0_ += ((box.bottom - box.top) - w_) / 2.0f;
        rt->GetFactory(factory_.put());
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), brush_.put());
        if (factory_) {
            const D2D1_STROKE_STYLE_PROPERTIES round = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
            factory_->CreateStrokeStyle(round, nullptr, 0, round_.put());
        }
    }
    bool Ok() const { return factory_ && brush_; }
    float W() const { return w_; }
    D2D1_POINT_2F P(float x, float y) const { return D2D1::Point2F(x0_ + x * w_, y0_ + y * w_); }
    void Color(uint32_t argb, float alpha = 1.0f) { brush_->SetColor(Theme::ToColor(argb, alpha * colors_.opacity)); }
    const Colors& Palette() const { return colors_; }

    void Line(float x1, float y1, float x2, float y2, float stroke) {
        rt_->DrawLine(P(x1, y1), P(x2, y2), brush_.get(), stroke * w_, round_.get());
    }
    void Dot(float x, float y, float r) { rt_->FillEllipse(D2D1::Ellipse(P(x, y), r * w_, r * w_), brush_.get()); }

    // Sun: a disc and 8 rays (unit coordinates).
    void Sun(float cx, float cy, float r) {
        Color(colors_.sun);
        Dot(cx, cy, r);
        const float stroke = std::max(0.055f, 1.0f / std::max(1.0f, w_));
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * kPi / 4.0f;
            const float c = std::cos(a), s = std::sin(a);
            Line(cx + c * r * 1.38f, cy + s * r * 1.38f, cx + c * r * 1.78f, cy + s * r * 1.78f, stroke);
        }
    }

    // Moon: a crescent (outer arc of radius r, inner arc of the same radius moved right).
    void Moon(float cx, float cy, float r) {
        winrt::com_ptr<ID2D1PathGeometry> path;
        winrt::com_ptr<ID2D1GeometrySink> sink;
        if (FAILED(factory_->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return;
        const float rp = r * w_;
        const D2D1_POINT_2F top = P(cx + 0.5f * r, cy - 0.866f * r), bottom = P(cx + 0.5f * r, cy + 0.866f * r);
        sink->BeginFigure(top, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddArc(D2D1::ArcSegment(bottom, D2D1::SizeF(rp, rp), 0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                      D2D1_ARC_SIZE_LARGE));
        sink->AddArc(D2D1::ArcSegment(top, D2D1::SizeF(rp, rp), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) return;
        Color(colors_.ink, 0.95f);
        rt_->FillGeometry(path.get(), brush_.get());
    }

    // Cloud: one path, three arcs over a flat base. (ox, oy) shifts and `k` scales the unit cloud
    // (which spans x 0.14 - 0.92, y 0.24 - 0.78) about its bottom centre (0.53, 0.78).
    void Cloud(float ox, float oy, float k, uint32_t argb, float alpha) {
        auto map = [&](float x, float y, float r) {
            return Circle{(0.53f + (x - 0.53f) * k + ox) * w_ + x0_, (0.78f + (y - 0.78f) * k + oy) * w_ + y0_, r * k * w_};
        };
        const Circle a = map(0.30f, 0.62f, 0.16f);
        const Circle b = map(0.52f, 0.46f, 0.22f);
        const Circle c = map(0.74f, 0.60f, 0.18f);
        winrt::com_ptr<ID2D1PathGeometry> path;
        winrt::com_ptr<ID2D1GeometrySink> sink;
        if (FAILED(factory_->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return;
        const D2D1_POINT_2F start{a.x, a.y + a.r};
        const D2D1_POINT_2F cBottom{c.x, c.y + c.r};
        const D2D1_POINT_2F bc = UpperIntersection(b, c);
        const D2D1_POINT_2F ab = UpperIntersection(a, b);
        sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(cBottom);
        ArcCcw(sink.get(), c, cBottom, bc);
        ArcCcw(sink.get(), b, bc, ab);
        ArcCcw(sink.get(), a, ab, start);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) return;
        Color(argb, alpha);
        rt_->FillGeometry(path.get(), brush_.get());
    }

    void Polygon(const D2D1_POINT_2F* pts, size_t n) {
        winrt::com_ptr<ID2D1PathGeometry> path;
        winrt::com_ptr<ID2D1GeometrySink> sink;
        if (n < 3 || FAILED(factory_->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return;
        sink->BeginFigure(P(pts[0].x, pts[0].y), D2D1_FIGURE_BEGIN_FILLED);
        for (size_t i = 1; i < n; ++i) sink->AddLine(P(pts[i].x, pts[i].y));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) return;
        rt_->FillGeometry(path.get(), brush_.get());
    }

    // Map pin outline with a centre dot ("set a location").
    void Pin() {
        winrt::com_ptr<ID2D1PathGeometry> path;
        winrt::com_ptr<ID2D1GeometrySink> sink;
        if (FAILED(factory_->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return;
        const float r = 0.25f * w_;
        sink->BeginFigure(P(0.5f, 0.90f), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(P(0.25f, 0.62f), P(0.25f, 0.40f)));
        sink->AddArc(D2D1::ArcSegment(P(0.75f, 0.40f), D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL));
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(P(0.75f, 0.62f), P(0.5f, 0.90f)));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) return;
        Color(colors_.ink, 0.9f);
        rt_->DrawGeometry(path.get(), brush_.get(), std::max(1.0f, 0.075f * w_), round_.get());
        Dot(0.5f, 0.40f, 0.085f);
    }

    void Arrow(float cx, float cy, float r, uint32_t argb) {
        winrt::com_ptr<ID2D1PathGeometry> path;
        winrt::com_ptr<ID2D1GeometrySink> sink;
        if (FAILED(factory_->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return;
        const float a0 = -50.0f * kPi / 180.0f, a1 = a0 + 290.0f * kPi / 180.0f;  // clockwise sweep
        const float rp = r * w_;
        const D2D1_POINT_2F p0 = P(cx + r * std::cos(a0), cy + r * std::sin(a0));
        const D2D1_POINT_2F p1 = P(cx + r * std::cos(a1), cy + r * std::sin(a1));
        sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(p1, D2D1::SizeF(rp, rp), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_LARGE));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close())) return;
        Color(argb);
        rt_->DrawGeometry(path.get(), brush_.get(), std::max(1.0f, 0.09f * w_), round_.get());
        // Arrowhead at the end, pointing along the sweep.
        const float tx = -std::sin(a1), ty = std::cos(a1);  // tangent (clockwise)
        const float nx = std::cos(a1), ny = std::sin(a1);   // outward normal
        const float ex = cx + r * nx, ey = cy + r * ny;
        const D2D1_POINT_2F head[3] = {
            D2D1::Point2F(ex + tx * 0.20f, ey + ty * 0.20f),
            D2D1::Point2F(ex + nx * 0.16f - tx * 0.02f, ey + ny * 0.16f - ty * 0.02f),
            D2D1::Point2F(ex - nx * 0.16f - tx * 0.02f, ey - ny * 0.16f - ty * 0.02f),
        };
        Polygon(head, 3);
    }

private:
    ID2D1RenderTarget* rt_;
    const Colors& colors_;
    float x0_ = 0.0f, y0_ = 0.0f, w_ = 0.0f;
    winrt::com_ptr<ID2D1Factory> factory_;
    winrt::com_ptr<ID2D1SolidColorBrush> brush_;
    winrt::com_ptr<ID2D1StrokeStyle> round_;
};

}  // namespace

Glyph FromWmo(int code) {
    switch (code) {
        case 0:
            return Glyph::Clear;
        case 1:
        case 2:
            return Glyph::PartlyCloudy;
        case 3:
            return Glyph::Overcast;
        case 45:
        case 48:
            return Glyph::Fog;
        case 51: case 53: case 55: case 56: case 57:
            return Glyph::Drizzle;
        case 61: case 63: case 65: case 66: case 67: case 80: case 81: case 82:
            return Glyph::Rain;
        case 71: case 73: case 75: case 77: case 85: case 86:
            return Glyph::Snow;
        case 95: case 96: case 99:
            return Glyph::Thunder;
        default:
            return Glyph::Overcast;
    }
}

const wchar_t* Describe(int code) {
    switch (code) {
        case 0: return L"Clear sky";
        case 1: return L"Mainly clear";
        case 2: return L"Partly cloudy";
        case 3: return L"Overcast";
        case 45: return L"Fog";
        case 48: return L"Freezing fog";
        case 51: return L"Light drizzle";
        case 53: return L"Drizzle";
        case 55: return L"Dense drizzle";
        case 56: return L"Light freezing drizzle";
        case 57: return L"Freezing drizzle";
        case 61: return L"Light rain";
        case 63: return L"Rain";
        case 65: return L"Heavy rain";
        case 66: return L"Light freezing rain";
        case 67: return L"Freezing rain";
        case 71: return L"Light snow";
        case 73: return L"Snow";
        case 75: return L"Heavy snow";
        case 77: return L"Snow grains";
        case 80: return L"Light showers";
        case 81: return L"Showers";
        case 82: return L"Violent showers";
        case 85: return L"Snow showers";
        case 86: return L"Heavy snow showers";
        case 95: return L"Thunderstorm";
        case 96: return L"Thunderstorm with hail";
        case 99: return L"Thunderstorm with heavy hail";
        default: return L"Unknown";
    }
}

void Draw(ID2D1RenderTarget* rt, Glyph glyph, bool isDay, const D2D1_RECT_F& box, const Colors& colors) {
    if (!rt) return;
    Painter p(rt, box, colors);
    if (!p.Ok() || p.W() < 2.0f) return;
    const uint32_t cloud = colors.ink;
    switch (glyph) {
        case Glyph::Clear:
            if (isDay) {
                p.Sun(0.5f, 0.5f, 0.21f);
            } else {
                p.Moon(0.47f, 0.5f, 0.30f);
            }
            break;
        case Glyph::PartlyCloudy:
            if (isDay) {
                p.Sun(0.36f, 0.36f, 0.16f);
            } else {
                p.Moon(0.34f, 0.34f, 0.22f);
            }
            p.Cloud(0.06f, 0.06f, 0.82f, cloud, 0.95f);
            break;
        case Glyph::Overcast:
            p.Cloud(-0.12f, -0.14f, 0.70f, cloud, 0.5f);  // a second cloud behind
            p.Cloud(0.03f, 0.06f, 0.98f, cloud, 0.95f);
            break;
        case Glyph::Fog:
            p.Cloud(0.0f, -0.16f, 0.80f, cloud, 0.85f);
            p.Color(colors.ink, 0.85f);
            p.Line(0.20f, 0.76f, 0.82f, 0.76f, 0.065f);
            p.Line(0.30f, 0.89f, 0.72f, 0.89f, 0.065f);
            break;
        case Glyph::Drizzle:
        case Glyph::Rain: {
            p.Cloud(0.0f, -0.17f, 0.88f, cloud, 0.95f);
            p.Color(colors.water);
            const bool heavy = glyph == Glyph::Rain;
            for (int i = 0; i < 3; ++i) {
                const float x = 0.36f + 0.16f * static_cast<float>(i);
                if (heavy) {
                    p.Line(x, 0.72f, x - 0.06f, 0.90f, 0.07f);
                } else {
                    p.Dot(x - 0.02f, 0.78f + (i == 1 ? 0.08f : 0.0f), 0.045f);
                }
            }
            break;
        }
        case Glyph::Snow: {
            p.Cloud(0.0f, -0.17f, 0.88f, cloud, 0.95f);
            p.Color(colors.ink);
            for (int i = 0; i < 3; ++i) {
                const float x = 0.34f + 0.16f * static_cast<float>(i);
                const float y = i == 1 ? 0.86f : 0.78f;
                for (int k = 0; k < 3; ++k) {  // a small six-pointed flake
                    const float a = static_cast<float>(k) * kPi / 3.0f;
                    const float dx = 0.055f * std::cos(a), dy = 0.055f * std::sin(a);
                    p.Line(x - dx, y - dy, x + dx, y + dy, 0.04f);
                }
            }
            break;
        }
        case Glyph::Thunder: {
            p.Cloud(0.0f, -0.17f, 0.88f, cloud, 0.95f);
            p.Color(colors.bolt);
            const D2D1_POINT_2F bolt[] = {{0.55f, 0.58f}, {0.38f, 0.80f}, {0.50f, 0.80f}, {0.44f, 0.98f},
                                          {0.64f, 0.72f}, {0.52f, 0.72f}, {0.60f, 0.58f}};
            p.Polygon(bolt, std::size(bolt));
            break;
        }
        case Glyph::SetLocation:
            p.Pin();
            break;
    }
}

void DrawRefresh(ID2D1RenderTarget* rt, const D2D1_RECT_F& box, uint32_t color) {
    if (!rt) return;
    const Colors colors{};
    Painter p(rt, box, colors);
    if (!p.Ok() || p.W() < 2.0f) return;
    p.Arrow(0.5f, 0.5f, 0.30f, color);
}

}  // namespace WeatherGlyphs
