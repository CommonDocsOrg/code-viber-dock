#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The drawing surface a widget script gets in draw(canvas, dock) (docs/widgets.md, "canvas").
// Engine-neutral: the script engine binding (QuickJsEngine.cpp) converts its arguments and calls
// these methods. Every call draws straight into the slot's DirectComposition surface through the
// Direct2D device context of the current draw (Renderer::DrawLayer): nothing is rasterised into an
// intermediate bitmap, and nothing is drawn unless the widget asked for a redraw (or the dock
// relaid it out). Coordinates are DIPs from the top-left of the widget's box, upright on every
// edge; the canvas scales them by the dock's DPI scale. Outside draw() (measure()) only the text
// metrics work; the drawing calls do nothing.

struct CanvasColor {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};
// "#RGB", "#RRGGBB", "#AARRGGBB" (as in config.json), "transparent", or "rgb(r, g, b)" /
// "rgba(r, g, b, a)" (0 - 255, alpha 0 - 1). False for anything else.
bool ParseCanvasColor(std::string_view text, CanvasColor& out);
// 0xAARRGGBB.
CanvasColor CanvasColorFromArgb(uint32_t argb);

struct CanvasGradientStop {
    float offset = 0.0f;  // 0 - 1
    CanvasColor color;
};

// A fill or stroke: a solid colour, a linear gradient (x0, y0) -> (x1, y1), or a radial one
// centred on (x0, y0) with radii (rx, ry) and the gradient origin offset by (x1, y1).
struct CanvasPaint {
    enum class Kind { Solid, Linear, Radial };
    Kind kind = Kind::Solid;
    CanvasColor color;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, rx = 0, ry = 0;
    std::vector<CanvasGradientStop> stops;
};

struct CanvasStroke {
    float width = 1.0f;
    D2D1_CAP_STYLE cap = D2D1_CAP_STYLE_FLAT;
    D2D1_LINE_JOIN join = D2D1_LINE_JOIN_MITER;
    std::vector<float> dashes;  // in stroke widths, like D2D custom dashes
};

struct CanvasTextStyle {
    std::wstring family = L"Segoe UI";
    float size = 12.0f;  // DIPs
    int weight = 400;    // 100 - 900
    bool italic = false;
    int align = 0;       // 0 left, 1 center, 2 right (of x; with maxWidth: inside [x, x + maxWidth] ...)
    int valign = 0;      // 0 top, 1 middle, 2 bottom, 3 baseline (of y)
    float maxWidth = 0;  // > 0: wraps at this width (and align positions the lines inside it)
};

struct CanvasTextMetrics {
    float width = 0, height = 0, baseline = 0;  // DIPs; baseline: of the first line, from the top
    int lines = 0;
};

// A path built by a script (canvas.path()): commands in DIPs, turned into a Direct2D path geometry
// (device independent, cached until the path changes) when it is filled or stroked.
class CanvasPath {
public:
    void MoveTo(float x, float y);
    void LineTo(float x, float y);
    void QuadTo(float cx, float cy, float x, float y);
    void BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
    // As the HTML canvas: radians, 0 = +x, increasing clockwise (y points down); a line joins the
    // current point to the arc's start.
    void Arc(float cx, float cy, float r, float start, float end, bool counterClockwise);
    void Rect(float x, float y, float w, float h);
    void Close();
    size_t Size() const { return ops_.size(); }
    // The geometry (null on failure or when empty).
    ID2D1PathGeometry* Geometry(ID2D1Factory1* factory, bool evenOdd);

private:
    enum class Op : uint8_t { Move, Line, Quad, Bezier, ArcSeg, Close };
    struct Cmd {
        Op op;
        float v[7];
    };
    void Ensure();  // a figure is open at the current point
    std::vector<Cmd> ops_;
    float curX_ = 0, curY_ = 0, startX_ = 0, startY_ = 0;
    bool hasPoint_ = false, open_ = false;
    winrt::com_ptr<ID2D1PathGeometry> geometry_;
    bool geometryEvenOdd_ = false;
};

class WidgetCanvas {
public:
    // Reads a package file (an image under the widget's folder): false if it is not there or not
    // allowed. `error` explains.
    using FileReader = std::function<bool(const std::string& path, std::string& bytes, std::wstring& error)>;

    WidgetCanvas(ID2D1Factory1* d2d, IDWriteFactory* dwrite, FileReader reader);
    WidgetCanvas(const WidgetCanvas&) = delete;
    WidgetCanvas& operator=(const WidgetCanvas&) = delete;

    // A draw into `dc` (whose current transform maps the surface's pixel origin) of a box
    // width x height DIPs at `scale` px per DIP. dc null: measuring only.
    void Begin(ID2D1DeviceContext* dc, float scale, float width, float height);
    // Pops every clip / layer the script left pushed and restores the device context's transform.
    void End();
    bool Drawing() const { return dc_ != nullptr; }
    float Width() const { return width_; }
    float Height() const { return height_; }
    float Scale() const { return scale_; }

    void FillRect(float x, float y, float w, float h, const CanvasPaint& p);
    void StrokeRect(float x, float y, float w, float h, const CanvasPaint& p, const CanvasStroke& s);
    void FillRoundRect(float x, float y, float w, float h, float r, const CanvasPaint& p);
    void StrokeRoundRect(float x, float y, float w, float h, float r, const CanvasPaint& p, const CanvasStroke& s);
    void FillEllipse(float cx, float cy, float rx, float ry, const CanvasPaint& p);
    void StrokeEllipse(float cx, float cy, float rx, float ry, const CanvasPaint& p, const CanvasStroke& s);
    void Line(float x0, float y0, float x1, float y1, const CanvasPaint& p, const CanvasStroke& s);
    void FillPath(CanvasPath& path, const CanvasPaint& p, bool evenOdd);
    void StrokePath(CanvasPath& path, const CanvasPaint& p, const CanvasStroke& s);
    void Text(const std::wstring& text, float x, float y, const CanvasTextStyle& style, const CanvasPaint& p);
    CanvasTextMetrics MeasureText(const std::wstring& text, const CanvasTextStyle& style);
    // An image from the widget's package (PNG, JPEG, BMP, GIF, ICO via WIC), into the rect (w or h
    // <= 0: the image's own size in DIPs at 96 DPI). False (with `error`) if it cannot be loaded.
    bool Image(const std::string& path, float x, float y, float w, float h, float opacity, std::wstring& error);
    bool ImageSize(const std::string& path, float& w, float& h, std::wstring& error);

    void Save();
    bool Restore();  // false: nothing saved
    void Translate(float x, float y);
    void Rotate(float radians, float cx, float cy);
    void ScaleBy(float sx, float sy);
    // Multiplies the opacity of everything drawn from now on (until Restore).
    void Opacity(float alpha);
    void ClipRect(float x, float y, float w, float h);
    void ClipPath(CanvasPath& path);

    // Drops the cached images (a device change: their Direct2D bitmaps belong to the old device).
    void ReleaseImages() { images_.clear(); }

private:
    struct State {
        D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Identity();
        float alpha = 1.0f;
        size_t clips = 0;  // clips_ size when saved
    };
    struct ImageEntry {
        winrt::com_ptr<IWICBitmapSource> source;  // converted to 32bpp PBGRA
        winrt::com_ptr<ID2D1Bitmap> bitmap;
        ID2D1Device* device = nullptr;  // the bitmap's device (not owned)
        UINT w = 0, h = 0;
    };

    void Apply();  // sets the dc transform for the current state
    ID2D1Brush* Brush(const CanvasPaint& p);
    ID2D1StrokeStyle* StrokeStyle(const CanvasStroke& s);
    IDWriteTextFormat* Format(const CanvasTextStyle& style);
    winrt::com_ptr<IDWriteTextLayout> Layout(const std::wstring& text, const CanvasTextStyle& style,
                                             CanvasTextMetrics& m);
    ImageEntry* FindImage(const std::string& path, std::wstring& error);
    IWICImagingFactory* Wic();

    ID2D1Factory1* d2d_;
    IDWriteFactory* dwrite_;
    FileReader reader_;
    ID2D1DeviceContext* dc_ = nullptr;
    D2D1_MATRIX_3X2_F dcBase_ = D2D1::Matrix3x2F::Identity();  // the dc's transform at Begin
    D2D1_MATRIX_3X2_F base_ = D2D1::Matrix3x2F::Identity();    // DIPs -> surface pixels
    float scale_ = 1.0f, width_ = 0.0f, height_ = 0.0f;
    State cur_;
    std::vector<State> stack_;
    std::vector<bool> clips_;  // pushed clips: true = a layer, false = an axis-aligned clip
    winrt::com_ptr<ID2D1SolidColorBrush> solid_;
    winrt::com_ptr<ID2D1Brush> gradient_;  // the last gradient brush (kept alive for the call)
    std::map<std::wstring, winrt::com_ptr<IDWriteTextFormat>> formats_;
    std::map<std::wstring, winrt::com_ptr<ID2D1StrokeStyle>> strokes_;
    std::map<std::string, ImageEntry> images_;
    winrt::com_ptr<IWICImagingFactory> wic_;
};
