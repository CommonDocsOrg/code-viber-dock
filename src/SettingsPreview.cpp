#include "SettingsPreview.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>

#include "Backdrop.h"
#include "IconEffects.h"
#include "Theme.h"
#include "WeatherGlyphs.h"

namespace {

// The miniature dock: six placeholder apps showing the three dot states.
enum class Dot { None, Running, Active, Attention };
struct PreviewApp {
    const wchar_t* name;
    Dot dot;
};
constexpr PreviewApp kApps[] = {
    {L"File Explorer", Dot::Active}, {L"Browser", Dot::Running}, {L"Calendar", Dot::Running},
    {L"Music", Dot::Attention},      {L"Photos", Dot::None},     {L"Terminal", Dot::Running},
};
constexpr int kAppCount = static_cast<int>(std::size(kApps));
constexpr int kDefaultFocus = 2;  // the magnified app while the mouse is elsewhere

// Strip layout, DIPs. The dock's own metrics (DockWindow.cpp) are multiplied by a unit `b` chosen
// so the magnified icon and its label fit the strip at every magnification.
constexpr float kWallInsetX = 16.0f;
constexpr float kWallInsetY = 6.0f;
constexpr float kWallRadius = 6.0f;
constexpr float kBarBottom = 5.0f;   // wallpaper bottom -> bar bottom
constexpr float kTopRoom = 3.0f;     // wallpaper top -> label top (at full magnification)
constexpr float kLabelGap = 3.0f;
constexpr float kLabelFont = 11.0f;
constexpr float kLabelPadX = 7.0f;
constexpr float kLabelPadY = 3.0f;
constexpr float kLabelH = 21.0f;     // kLabelFont line + 2 * kLabelPadY (rounded up)
constexpr float kMaxUnit = 0.75f;
constexpr float kMinUnit = 0.3f;
constexpr float kCardW = 150.0f;     // the preview popup sample
constexpr float kCardH = 64.0f;
constexpr float kCardRight = 14.0f;
constexpr float kCardPad = 5.0f;
constexpr float kTitleFont = 9.0f;
constexpr float kPi = 3.14159265358979f;

D2D1_COLOR_F FromColorRef(COLORREF c, float a = 1.0f) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a);
}

// Backdrop::SetCornerRadius: DWM rounds the acrylic window with 0, 4 or 8 DIPs.
float SnapAcrylicRadius(float dip) { return dip < 4.0f ? 0.0f : dip < 8.0f ? 4.0f : 8.0f; }

D2D1_ROUNDED_RECT Rounded(const D2D1_RECT_F& r, float radius) {
    const float max = std::min(r.right - r.left, r.bottom - r.top) / 2.0f;
    const float rr = std::clamp(radius, 0.0f, std::max(0.0f, max));
    return D2D1::RoundedRect(r, rr, rr);
}

D2D1_RECT_F Inset(const D2D1_RECT_F& r, float d) { return D2D1::RectF(r.left + d, r.top + d, r.right - d, r.bottom - d); }

}  // namespace

SettingsPreview::~SettingsPreview() {
    if (hwnd_) DestroyWindow(hwnd_);
    ReleaseAll();
}

bool SettingsPreview::Create(HINSTANCE instance, HWND parent, int id, const Config* config, UINT dpi) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProcThunk;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        registered = true;
    }
    config_ = config;
    dpi_ = dpi;
    CreateWindowExW(0, kClassName, L"Dock preview", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, this);
    return hwnd_ != nullptr;
}

void SettingsPreview::SetColors(COLORREF layer, COLORREF divider) {
    layer_ = layer;
    divider_ = divider;
    Invalidate();
}

void SettingsPreview::SetDpi(UINT dpi) {
    if (dpi == dpi_) return;
    dpi_ = dpi;
    ReleaseTarget();  // re-created at the new size / DPI on the next paint
    labelFormat_ = letterFormat_ = titleFormat_ = nullptr;
    labelSize_ = letterSize_ = titleSize_ = 0.0f;
    clock_.ReleaseLayout();
    Invalidate();
}

void SettingsPreview::Invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsPreview::ReleaseTarget() { target_ = nullptr; }

void SettingsPreview::ReleaseAll() {
    clock_.ReleaseLayout();
    labelFormat_ = letterFormat_ = titleFormat_ = nullptr;
    labelSize_ = letterSize_ = titleSize_ = 0.0f;
    target_ = nullptr;
    dwrite_ = nullptr;
    factory_ = nullptr;
}

HRESULT SettingsPreview::EnsureTarget() {
    if (target_) return S_OK;
    HRESULT hr = S_OK;
    if (!factory_) {
        D2D1_FACTORY_OPTIONS options{};
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, factory_.put_void());
        if (FAILED(hr)) return hr;
    }
    if (!dwrite_) {
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwrite_.put()));
        if (FAILED(hr)) return hr;
    }
    // A DC render target bound to the WM_PAINT DC on every paint: the strip lands in the same GDI
    // redirection surface as the controls around it, so PrintWindow / BitBlt captures and
    // WM_PRINTCLIENT see it, and resizing needs nothing. (An ID2D1HwndRenderTarget presents through
    // its own swap chain; GDI captures of the window showed the strip blank.) Pixel coordinates
    // (96 "DPI"): the drawing scales itself, like the dock's renderer.
    // Software rendering: the strip is small and repaints rarely, and a hardware target would
    // create a second Direct3D device (measured: ~18 MB of private bytes that stay after the window
    // closes, ~30 driver threads while it is open, and growth on every open / close).
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    return factory_->CreateDCRenderTarget(&props, target_.put());
}

IDWriteTextFormat* SettingsPreview::Format(winrt::com_ptr<IDWriteTextFormat>& slot, float& slotSize, float size,
                                           DWRITE_FONT_WEIGHT weight) {
    if (slot && slotSize == size) return slot.get();
    slot = nullptr;
    slotSize = 0.0f;
    if (!dwrite_ || size <= 0.0f ||
        FAILED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"", slot.put()))) {
        slot = nullptr;
        return nullptr;
    }
    slot->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    slotSize = size;
    return slot.get();
}

void SettingsPreview::Paint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);
    bool drawn = false;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (hdc && config_ && rc.right > 0 && rc.bottom > 0 && SUCCEEDED(EnsureTarget()) &&
        SUCCEEDED(target_->BindDC(hdc, &rc))) {
        target_->BeginDraw();
        winrt::com_ptr<ID2D1DeviceContext> dc = target_.try_as<ID2D1DeviceContext>();
        if (dc) Draw(dc.get());
        const HRESULT hr = target_->EndDraw();
        drawn = SUCCEEDED(hr) && dc;
        if (hr == D2DERR_RECREATE_TARGET) {
            // Device lost: a new target on the next paint (one retry per loss, no loop).
            ReleaseTarget();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    if (!drawn && hdc) {
        HBRUSH b = CreateSolidBrush(layer_);
        FillRect(hdc, &ps.rcPaint, b);
        DeleteObject(b);
    }
    EndPaint(hwnd_, &ps);
}

void SettingsPreview::Draw(ID2D1DeviceContext* dc) {
    const Config& c = *config_;
    const ThemeConfig& t = c.theme;
    const float s = static_cast<float>(dpi_) / USER_DEFAULT_SCREEN_DPI;
    auto px = [s](float dip) { return std::round(dip * s); };
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float W = static_cast<float>(rc.right), H = static_cast<float>(rc.bottom);
    const float stroke = std::max(1.0f, std::round(s));

    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->Clear(FromColorRef(layer_));
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(FromColorRef(divider_), brush.put()))) return;
    dc->FillRectangle(D2D1::RectF(0, 0, W, stroke), brush.get());

    // --- wallpaper ---
    const D2D1_RECT_F wall = D2D1::RectF(px(kWallInsetX), px(kWallInsetY), W - px(kWallInsetX), H - px(kWallInsetY));
    if (wall.right - wall.left < 40.0f || wall.bottom - wall.top < 20.0f) return;
    winrt::com_ptr<ID2D1RoundedRectangleGeometry> wallShape;
    if (FAILED(factory_->CreateRoundedRectangleGeometry(Rounded(wall, px(kWallRadius)), wallShape.put()))) return;
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), wallShape.get()), nullptr);

    const D2D1_GRADIENT_STOP stops[] = {
        {0.0f, D2D1::ColorF(0x3B6E8F)},
        {0.55f, D2D1::ColorF(0x7A5C99)},
        {1.0f, D2D1::ColorF(0xC9798A)},
    };
    winrt::com_ptr<ID2D1GradientStopCollection> stopCollection;
    winrt::com_ptr<ID2D1LinearGradientBrush> gradient;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, static_cast<UINT32>(std::size(stops)), stopCollection.put())) &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(wall.left, wall.top), D2D1::Point2F(wall.right, wall.bottom)),
            stopCollection.get(), gradient.put()))) {
        dc->FillRectangle(wall, gradient.get());
    }
    // A soft light blob so a translucent bar has something to show through.
    const D2D1_GRADIENT_STOP glow[] = {{0.0f, D2D1::ColorF(1, 1, 1, 0.28f)}, {1.0f, D2D1::ColorF(1, 1, 1, 0.0f)}};
    winrt::com_ptr<ID2D1GradientStopCollection> glowStops;
    winrt::com_ptr<ID2D1RadialGradientBrush> glowBrush;
    const D2D1_POINT_2F glowCentre = D2D1::Point2F(wall.left + (wall.right - wall.left) * 0.3f, wall.top);
    const float glowR = (wall.bottom - wall.top) * 1.4f;
    if (SUCCEEDED(dc->CreateGradientStopCollection(glow, 2, glowStops.put())) &&
        SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(glowCentre, {}, glowR * 1.6f, glowR),
                                                glowStops.get(), glowBrush.put()))) {
        dc->FillRectangle(wall, glowBrush.get());
    }

    const bool acrylic = t.acrylic && !t.shelf && !t.perspective && Backdrop::SystemAllowsTransparency();
    const D2D1_COLOR_F frost = D2D1::ColorF(0x6E6A8E, 0.85f);  // the blurred wallpaper under acrylic
    auto fillPanel = [&](const D2D1_ROUNDED_RECT& shape) {
        if (acrylic) {
            brush->SetColor(frost);
            dc->FillRoundedRectangle(shape, brush.get());
            brush->SetColor(Theme::ToColor(t.acrylicBackground));
        } else {
            brush->SetColor(Theme::ToColor(t.background));
        }
        dc->FillRoundedRectangle(shape, brush.get());
        const float inset = stroke / 2.0f;
        brush->SetColor(Theme::ToColor(t.barBorder));
        D2D1_ROUNDED_RECT edge = shape;
        edge.rect = Inset(shape.rect, inset);
        edge.radiusX = edge.radiusY = std::max(0.0f, shape.radiusX - inset);
        dc->DrawRoundedRectangle(edge, brush.get(), stroke);
    };

    // --- geometry (pixels) ---
    const float mag = static_cast<float>(std::clamp(c.magnification, 1.0, 2.5));
    const float wallH = (wall.bottom - wall.top) / s;
    const float reflFrac = t.reflection ? t.reflectionHeight : 0.0f;  // the bar grows by the reflections
    const float b = std::clamp((wallH - kBarBottom - kTopRoom - kLabelGap - kLabelH) / (11.0f + 48.0f * (mag + reflFrac)),
                               kMinUnit, kMaxUnit);
    const float u = b * s;  // dock DIP -> preview pixel
    const float iconPx = std::round(48.0f * u);
    const float gap = std::max(2.0f, std::round(8.0f * u));
    const float half = std::floor(gap / 2.0f);
    const float padding = std::round(12.0f * u);
    const float reflH = t.reflection ? std::ceil(iconPx * t.reflectionHeight) : 0.0f;
    const float rowH = std::round(iconPx + 16.0f * u);  // the icon row (the bar without reflections)
    const float barH = rowH + reflH;
    const float stride = iconPx + gap;
    const float lead = padding - half;
    const float itemsW = static_cast<float>(kAppCount) * stride;

    D2D1_SIZE_U clockSize{};
    if (c.clock.show && dwrite_) {
        if (!clockConfigured_ || clockFormat_ != c.clock.format || clockDate_ != c.clock.showDate) {
            clock_.Configure(c.clock.format, c.clock.showDate);
            clockFormat_ = c.clock.format;
            clockDate_ = c.clock.showDate;
            clockConfigured_ = true;
        }
        clock_.SetColors(t.clockText, t.clockDateText);
        clock_.UpdateText();
        clockSize = clock_.BuildLayout(dwrite_.get(), u);
    }
    const float clockW = static_cast<float>(clockSize.width), clockH = static_cast<float>(clockSize.height);
    // The weather widget: a sun glyph standing in for it (no data here), left of the clock.
    const float weatherW = c.weather.show ? std::round(22.0f * u) : 0.0f;
    const float widgetGap = c.weather.show && c.clock.show ? std::round(10.0f * u) : 0.0f;
    const bool widgets = c.clock.show || c.weather.show;
    const float sepW = widgets ? std::round(17.0f * u) : 0.0f;
    float barW = widgets ? lead + itemsW + sepW + weatherW + widgetGap + clockW + padding : lead + itemsW + lead;
    barW = std::round(barW);

    const float cardW = px(kCardW), cardH = std::min(px(kCardH), wall.bottom - wall.top - px(8.0f));
    const float cardRight = wall.right - px(kCardRight);
    const float cardLeft = cardRight - cardW;
    const float regionLeft = wall.left, regionRight = cardLeft - px(12.0f);
    const float barLeft = std::round(regionLeft + std::max(0.0f, (regionRight - regionLeft - barW) / 2.0f));
    const float barBottom = wall.bottom - px(kBarBottom);
    const float barTop = barBottom - barH;
    const float itemsLeft = barLeft + lead;
    const float itemTop = barTop + std::round(5.0f * u);

    // Magnification about the focus (the cursor, else the default app), as DockWindow::ComputeTargets.
    const float focusX = mouseX_ >= 0 ? static_cast<float>(mouseX_)
                                      : itemsLeft + (static_cast<float>(kDefaultFocus) + 0.5f) * stride;
    const float range = static_cast<float>(c.magnifyRange) * stride;
    float scales[kAppCount];
    float delta = 0.0f;
    for (int i = 0; i < kAppCount; ++i) {
        float scale = 1.0f;
        if (mag > 1.001f) {
            const float d = std::fabs(focusX - (itemsLeft + (static_cast<float>(i) + 0.5f) * stride));
            if (d < range) {
                const float f = c.animation.magnifyFalloff == Falloff::Linear ? 1.0f - d / range
                                                                             : 0.5f * (1.0f + std::cos(kPi * d / range));
                scale = 1.0f + (mag - 1.0f) * f;
            }
        }
        scales[i] = scale;
        delta += iconPx * (scale - 1.0f);
    }
    float growLeft = 0.0f;
    if (delta > 0.0f) {
        const float x = focusX - barLeft;
        float mapped;
        if (x <= lead) {
            mapped = x;
        } else if (x >= lead + itemsW) {
            mapped = x + delta;
        } else {
            const int k = std::min(kAppCount - 1, static_cast<int>((x - lead) / stride));
            const float frac = (x - lead - static_cast<float>(k) * stride) / stride;
            mapped = lead;
            for (int j = 0; j < k; ++j) mapped += iconPx * scales[j] + gap;
            mapped += frac * (iconPx * scales[k] + gap);
        }
        growLeft = std::clamp(mapped - x, 0.0f, delta);
    }
    const float growRight = delta - growLeft;
    int hovered = -1;
    if (focusX >= itemsLeft && focusX < itemsLeft + itemsW) {
        hovered = std::min(kAppCount - 1, static_cast<int>((focusX - itemsLeft) / stride));
    }

    // --- bar ---
    const float radiusDip = acrylic ? SnapAcrylicRadius(t.cornerRadius) : t.cornerRadius;
    const D2D1_RECT_F bar = D2D1::RectF(barLeft - growLeft, barTop, barLeft + barW + growRight, barBottom);
    const float separatorX = std::round(barLeft + lead + itemsW + sepW / 2.0f) + growRight;
    // The perspective tilt is shown as the shelf's trapezoid (with icons a little shorter).
    const bool shelf = t.shelf || t.perspective;
    if (shelf) {
        Theme::DrawShelf(dc, bar, t, u, stroke, widgets ? separatorX : -1.0f, reflH);
    } else {
        fillPanel(Rounded(bar, std::round(radiusDip * u)));
    }
    if (widgets) {
        if (!shelf) {
            const float x = separatorX + stroke / 2.0f;
            const float vInset = std::round(rowH * 0.22f);
            brush->SetColor(Theme::ToColor(t.separator));
            dc->DrawLine(D2D1::Point2F(x, barTop + vInset), D2D1::Point2F(x, barTop + rowH - vInset), brush.get(),
                         stroke);
        }
        if (c.clock.show) {
            clock_.Draw(dc, barLeft + barW - padding - clockW + growRight, std::round(barTop + (rowH - clockH) / 2.0f));
        }
        if (c.weather.show) {
            const float x = barLeft + barW - padding - clockW - widgetGap - weatherW + growRight;
            const float y = std::round(barTop + (rowH - weatherW) / 2.0f);
            WeatherGlyphs::Colors colors;
            colors.ink = t.clockText;
            colors.water = t.accent;
            WeatherGlyphs::Draw(dc, WeatherGlyphs::Glyph::Clear, true, D2D1::RectF(x, y, x + weatherW, y + weatherW),
                                colors);
        }
    }

    // --- icons and dots ---
    IDWriteTextFormat* letter =
        Format(letterFormat_, letterSize_, std::max(1.0f, iconPx * 0.5f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (letter) {
        letter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        letter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    const D2D1_COLOR_F dotColors[3] = {Theme::ToColor(t.dotRunning), Theme::ToColor(t.dotActive),
                                       Theme::ToColor(t.dotAttention)};
    const float dotR = std::max(1.5f, 2.0f * u);
    const float dotY = reflH > 0.0f ? itemTop + iconPx + std::round(5.0f * u) : barBottom - std::round(5.0f * u);
    // Icon effects, as the dock bakes them into its icon surfaces (here at rest size, every paint).
    const GlowMode glowMode = GlowOf(t);
    const float glowRadius = t.glowRadius * u;
    const uint32_t glowColor = IconEffects::GlowColor(t, 0, t.accent);  // placeholders: like the dock's
    const float squash = t.perspective ? 0.9f : 1.0f;
    float slotLeft = itemsLeft - growLeft;
    float hoveredCx = 0.0f, hoveredTop = 0.0f;
    for (int i = 0; i < kAppCount; ++i) {
        const float w = iconPx * scales[i];
        const float cx = slotLeft + half + w / 2.0f;
        slotLeft += w + gap;
        const float bottom = itemTop + iconPx;
        if (i == hovered) {
            hoveredCx = cx;
            hoveredTop = bottom - w;
        }
        // Drawn at rest size, scaled about the bottom centre (as the dock's layer transform).
        const float left = cx - iconPx / 2.0f, top = itemTop;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(scales[i], scales[i] * squash, D2D1::Point2F(cx, bottom)));
        const wchar_t ch[2] = {static_cast<wchar_t>(std::towupper(kApps[i].name[0])), 0};
        // The placeholder square at (0, 0) - (size, size) of whatever context draws it.
        auto drawIcon = [&](ID2D1DeviceContext* target, float size) {
            winrt::com_ptr<ID2D1SolidColorBrush> fill;
            if (FAILED(target->CreateSolidColorBrush(Theme::ToColor(t.placeholderFill), fill.put()))) return;
            const float inset = std::round(size * 0.06f);
            const float r = size * 0.22f;
            target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(inset, inset, size - inset, size - inset), r, r), fill.get());
            if (letter) {
                fill->SetColor(Theme::ToColor(t.placeholderText));
                target->DrawText(ch, 1, letter, D2D1::RectF(0, 0, size, size), fill.get());
            }
        };
        const bool glowShown = glowMode == GlowMode::Always || (glowMode == GlowMode::Hover && i == hovered) ||
                               (glowMode == GlowMode::Active && kApps[i].dot == Dot::Active);
        winrt::com_ptr<ID2D1CommandList> recorded;
        if ((glowShown && t.glowStrength > 0.0f) || reflH > 0.0f) recorded = IconEffects::Record(dc, iconPx, drawIcon);
        if (recorded && glowShown) {
            IconEffects::DrawGlow(dc, recorded.get(), glowRadius, glowColor, t.glowStrength, D2D1::Point2F(left, top));
        }
        if (recorded && reflH > 0.0f) {
            IconEffects::DrawReflection(dc, recorded.get(), iconPx, reflH, t.reflectionOpacity,
                                        D2D1::Point2F(left, top + iconPx + 1.0f));
        }
        const D2D1_MATRIX_3X2_F placed = D2D1::Matrix3x2F::Translation(left, top) *
                                         D2D1::Matrix3x2F::Scale(scales[i], scales[i] * squash, D2D1::Point2F(cx, bottom));
        dc->SetTransform(placed);
        drawIcon(dc, iconPx);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        if (kApps[i].dot != Dot::None) {
            brush->SetColor(dotColors[static_cast<int>(kApps[i].dot) - 1]);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, dotY), dotR, dotR), brush.get());
        }
    }

    // --- hover label ---
    if (hovered >= 0) {
        if (IDWriteTextFormat* fmt = Format(labelFormat_, labelSize_, kLabelFont * s, DWRITE_FONT_WEIGHT_NORMAL)) {
            const wchar_t* name = kApps[hovered].name;
            winrt::com_ptr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwrite_->CreateTextLayout(name, static_cast<UINT32>(wcslen(name)), fmt, 1000.0f, 100.0f,
                                                    layout.put()))) {
                DWRITE_TEXT_METRICS m{};
                layout->GetMetrics(&m);
                const float lw = std::ceil(m.widthIncludingTrailingWhitespace) + 2.0f * px(kLabelPadX);
                const float lh = std::ceil(m.height) + 2.0f * px(kLabelPadY);
                const float lx = std::round(std::clamp(hoveredCx - lw / 2.0f, wall.left + 2.0f, wall.right - lw - 2.0f));
                const float ly = std::round(std::max(wall.top + px(1.0f), hoveredTop - px(kLabelGap) - lh));
                const D2D1_RECT_F lr = D2D1::RectF(lx, ly, lx + lw, ly + lh);
                const float radius = std::round(t.labelCornerRadius * s);
                const D2D1_ROUNDED_RECT shape = Rounded(lr, radius);
                brush->SetColor(Theme::ToColor(t.labelBackground));
                dc->FillRoundedRectangle(shape, brush.get());
                const float inset = stroke / 2.0f;
                brush->SetColor(Theme::ToColor(t.labelBorder));
                dc->DrawRoundedRectangle(Rounded(Inset(lr, inset), std::max(0.0f, shape.radiusX - inset)), brush.get(),
                                         stroke);
                brush->SetColor(Theme::ToColor(t.labelText));
                dc->DrawTextLayout(D2D1::Point2F(lx + px(kLabelPadX), ly + px(kLabelPadY)), layout.get(), brush.get());
            }
        }
    }

    // --- preview popup sample: a hot card (close button hovered) and a plain one ---
    const float cardTop = std::round((wall.top + wall.bottom - cardH) / 2.0f);
    const D2D1_RECT_F popup = D2D1::RectF(cardLeft, cardTop, cardRight, cardTop + cardH);
    const float popupRadius = std::round((acrylic ? t.cornerRadius : std::min(t.cornerRadius, 12.0f)) * 0.6f * s);
    fillPanel(Rounded(popup, popupRadius));
    const float pad = px(kCardPad);
    const float cw = std::floor((cardW - 3.0f * pad) / 2.0f);
    const float cardRadius = px(4.0f * 0.75f);
    IDWriteTextFormat* title = Format(titleFormat_, titleSize_, kTitleFont * s, DWRITE_FONT_WEIGHT_NORMAL);
    if (title) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        title->SetTrimming(&trim, nullptr);
    }
    for (int i = 0; i < 2; ++i) {
        const bool hot = i == 0;
        const float l = popup.left + pad + static_cast<float>(i) * (cw + pad);
        const D2D1_RECT_F card = D2D1::RectF(l, popup.top + pad, l + cw, popup.bottom - pad);
        if (hot) {
            brush->SetColor(Theme::ToColor(t.previewHotBackground));
            dc->FillRoundedRectangle(Rounded(card, cardRadius), brush.get());
            brush->SetColor(Theme::ToColor(t.previewHotBorder));
            dc->DrawRoundedRectangle(Rounded(Inset(card, stroke / 2.0f), cardRadius - stroke / 2.0f), brush.get(), stroke);
        }
        const float in = px(3.0f);
        const float titleH = px(12.0f);
        const float closeSize = px(11.0f);
        const D2D1_RECT_F close =
            D2D1::RectF(card.right - in - closeSize, card.top + in, card.right - in, card.top + in + closeSize);
        if (title) {
            const wchar_t* text = hot ? L"Window 1" : L"Window 2";
            brush->SetColor(Theme::ToColor(t.labelText, hot ? 1.0f : 0.85f));
            dc->DrawText(text, static_cast<UINT32>(wcslen(text)), title,
                         D2D1::RectF(card.left + in, card.top + in - px(1.0f), close.left - px(2.0f),
                                     card.top + in + titleH),
                         brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (hot) {
            brush->SetColor(Theme::ToColor(t.closeHover));
            dc->FillRoundedRectangle(Rounded(close, px(3.0f)), brush.get());
        }
        const float gx = (close.left + close.right) / 2.0f, gy = (close.top + close.bottom) / 2.0f;
        const float g = std::round(3.5f * s);
        brush->SetColor(Theme::ToColor(t.labelText, hot ? 1.0f : 0.55f));
        const float glyphStroke = std::max(1.0f, 1.1f * s);
        dc->DrawLine(D2D1::Point2F(gx - g, gy - g), D2D1::Point2F(gx + g, gy + g), brush.get(), glyphStroke);
        dc->DrawLine(D2D1::Point2F(gx - g, gy + g), D2D1::Point2F(gx + g, gy - g), brush.get(), glyphStroke);
        // The thumbnail area (a window without a live thumbnail: the half-alpha icon box).
        const D2D1_RECT_F box =
            D2D1::RectF(card.left + in, card.top + in + titleH + px(2.0f), card.right - in, card.bottom - in);
        brush->SetColor(Theme::ToColor(t.previewHotBackground, 0.5f));
        dc->FillRoundedRectangle(Rounded(box, cardRadius), brush.get());
    }

    dc->PopLayer();
}

LRESULT CALLBACK SettingsPreview::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsPreview* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<SettingsPreview*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsPreview*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->WndProc(msg, wp, lp);
}

LRESULT SettingsPreview::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:  // the target is bound to the paint DC with the client size on every paint
            Invalidate();
            return 0;
        case WM_MOUSEMOVE: {
            const int x = GET_X_LPARAM(lp);
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                tracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            if (x != mouseX_) {
                mouseX_ = x;
                Invalidate();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            mouseX_ = -1;
            Invalidate();
            return 0;
        case WM_DESTROY:
            ReleaseAll();  // nothing of the preview outlives its window
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
