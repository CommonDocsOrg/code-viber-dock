#include "SettingsPreview.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>

#include "Backdrop.h"
#include "IconEffects.h"
#include "Presets.h"
#include "Theme.h"
#include "WeatherGlyphs.h"

namespace {

// The miniature dock: six placeholder apps showing the three dot states.
enum class Dot { None, Running, Active, Attention };
struct PreviewApp {
    const wchar_t* name;
    Dot dot;
    bool stacked = false;  // round 11 phase 11A: two windows (the slot button's stacked edge)
};
constexpr PreviewApp kApps[] = {
    {L"File Explorer", Dot::Active}, {L"Browser", Dot::Running, true}, {L"Calendar", Dot::Running},
    {L"Music", Dot::Attention},      {L"Photos", Dot::None},           {L"Terminal", Dot::Running},
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
    const DockConfig* shown = dock_ ? dock_() : nullptr;
    const DockConfig& dk = shown ? *shown : c.Main();
    // The global theme (Global > Themes / Animation) or the dock's own (a Dock window).
    const ThemeConfig& t = globalTheme_ ? c.theme : c.ThemeOf(dk);
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
    dc->FillRectangle(dividerBottom_ ? D2D1::RectF(0, H - stroke, W, H) : D2D1::RectF(0, 0, W, stroke), brush.get());

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
    // `sheen`: the bar (round 11 phase 11A: theme.sheen over the fill, under the border, as the dock draws it).
    auto fillPanel = [&](const D2D1_ROUNDED_RECT& shape, bool sheen) {
        if (acrylic) {
            brush->SetColor(frost);
            dc->FillRoundedRectangle(shape, brush.get());
            brush->SetColor(Theme::ToColor(t.acrylicBackground));
        } else {
            brush->SetColor(Theme::ToColor(t.background));
        }
        dc->FillRoundedRectangle(shape, brush.get());
        if (sheen) Theme::DrawSheen(dc, shape.rect, shape.radiusX, t, stroke);
        const float inset = stroke / 2.0f;
        brush->SetColor(Theme::ToColor(t.barBorder));
        D2D1_ROUNDED_RECT edge = shape;
        edge.rect = Inset(shape.rect, inset);
        edge.radiusX = edge.radiusY = std::max(0.0f, shape.radiusX - inset);
        dc->DrawRoundedRectangle(edge, brush.get(), stroke);
    };

    // --- geometry (pixels) ---
    const bool clockShown = dk.Has(ItemKind::Clock);
    const bool weatherShown = std::any_of(dk.items.begin(), dk.items.end(), [](const DockItemEntry& e) {
        return e.Kind() == ItemKind::Weather || (e.Kind() == ItemKind::Widget && e.id == L"weather");
    });
    const float mag = static_cast<float>(std::clamp(dk.magnification, 1.0, 2.5));
    const float wallH = (wall.bottom - wall.top) / s;
    const float reflFrac = t.reflection ? t.reflectionHeight : 0.0f;  // the bar grows by the reflections
    const float b = std::clamp((wallH - kBarBottom - kTopRoom - kLabelGap - kLabelH) / (11.0f + 48.0f * (mag + reflFrac)),
                               kMinUnit, kMaxUnit);
    const float u = b * s;  // dock DIP -> preview pixel
    const float iconPx = std::round(48.0f * u);
    // Round 11 phase 11A: the theme's gap and bar padding (8 / 8: the numbers of before) and slot
    // buttons instead of the dots (the icons centred on the row then, as on the dock).
    const float gap = std::max(std::round(t.iconGap * u), std::min(2.0f, t.iconGap));
    const float barPad = std::clamp(t.barPadding, 0.0f, 24.0f);
    const bool buttons = SlotStyleOf(t) == SlotStyle::Buttons;
    const float half = std::floor(gap / 2.0f);
    const float padding = std::round(12.0f * u);
    const float reflH = t.reflection ? std::ceil(iconPx * t.reflectionHeight) : 0.0f;
    const float rowH = std::round(iconPx + 2.0f * barPad * u);  // the icon row (the bar without reflections)
    const float barH = rowH + reflH;
    const float stride = iconPx + gap;
    const float lead = padding - half;
    const float itemsW = static_cast<float>(kAppCount) * stride;

    D2D1_SIZE_U clockSize{};
    if (clockShown && dwrite_) {
        // The dock's first clock item (round 5: each clock has its own settings).
        ClockConfig cc = c.clock;
        for (const DockItemEntry& e : dk.items) {
            if (e.Kind() != ItemKind::Clock) continue;
            cc = ClockOf(c, e);
            break;
        }
        const std::wstring format = Clock::EffectiveFormat(cc.format, cc.showSeconds);
        if (!clockConfigured_ || clockFormat_ != format || clockDate_ != cc.showDate || clockZone_ != cc.timeZone ||
            clockLabel_ != cc.label) {
            clock_.Configure(format, cc.showDate);
            clock_.SetTimeZone(cc.timeZone);
            clock_.SetLabel(cc.label);
            clockFormat_ = format;
            clockDate_ = cc.showDate;
            clockZone_ = cc.timeZone;
            clockLabel_ = cc.label;
            clockConfigured_ = true;
        }
        clock_.SetColors(t.clockText, t.clockDateText);
        clock_.UpdateText();
        clockSize = clock_.BuildLayout(dwrite_.get(), u);
    }
    const float clockW = static_cast<float>(clockSize.width), clockH = static_cast<float>(clockSize.height);
    // The weather widget: a sun glyph standing in for it (no data here), left of the clock.
    const float weatherW = weatherShown ? std::round(22.0f * u) : 0.0f;
    const float widgetGap = weatherShown && clockShown ? std::round(10.0f * u) : 0.0f;
    const bool widgets = clockShown || weatherShown;
    const float sepW = widgets ? std::round(17.0f * u) : 0.0f;
    float barW = widgets ? lead + itemsW + sepW + weatherW + widgetGap + clockW + padding : lead + itemsW + lead;
    barW = std::round(barW);

    const float cardW = px(kCardW), cardH = std::min(px(kCardH), wall.bottom - wall.top - px(8.0f));
    const float cardRight = wall.right - px(kCardRight);
    const float cardLeft = cardRight - cardW;
    const float regionLeft = wall.left, regionRight = showPopup_ ? cardLeft - px(12.0f) : wall.right;
    const float barLeft = std::round(regionLeft + std::max(0.0f, (regionRight - regionLeft - barW) / 2.0f));
    const float barBottom = wall.bottom - px(kBarBottom);
    const float barTop = barBottom - barH;
    const float itemsLeft = barLeft + lead;
    const float itemTop = barTop + std::round((buttons ? barPad : barPad * 0.625f) * u);

    // Magnification about the focus (the cursor, else the default app), as DockWindow::ComputeTargets.
    const float focusX = mouseX_ >= 0 ? static_cast<float>(mouseX_)
                                      : itemsLeft + (static_cast<float>(kDefaultFocus) + 0.5f) * stride;
    const float range = static_cast<float>(dk.magnifyRange) * stride;
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
        fillPanel(Rounded(bar, std::round(radiusDip * u)), true);
    }
    if (widgets) {
        if (!shelf) {
            const float x = separatorX + stroke / 2.0f;
            const float vInset = std::round(rowH * 0.22f);
            brush->SetColor(Theme::ToColor(t.separator));
            dc->DrawLine(D2D1::Point2F(x, barTop + vInset), D2D1::Point2F(x, barTop + rowH - vInset), brush.get(),
                         stroke);
        }
        if (clockShown) {
            clock_.Draw(dc, barLeft + barW - padding - clockW + growRight, std::round(barTop + (rowH - clockH) / 2.0f));
        }
        if (weatherShown) {
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
    const float dotFromEdge = std::round(barPad * 0.625f * u);
    const float dotY = reflH > 0.0f ? itemTop + iconPx + dotFromEdge : barBottom - dotFromEdge;
    // Slot buttons: the slot inset 1 DIP along the bar, the bar inset max(1, barPadding / 2) across.
    const float buttonInset = std::max(1.0f, std::round(u));
    const float buttonCross = std::round(std::max(1.0f, barPad / 2.0f) * u);
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
        if (buttons) {
            // Round 11 phase 11A: the slot's button by the sample's state (Browser has two windows:
            // the stacked edge), hovered with the light at the focus, as the dock draws it.
            Theme::SlotButton sb;
            sb.state = kApps[i].dot == Dot::Attention ? 3 : kApps[i].dot == Dot::Active ? 2 : kApps[i].dot == Dot::Running ? 1 : 0;
            sb.hovered = i == hovered;
            sb.stacked = kApps[i].stacked && sb.state != 0 ? 1 : 0;
            const D2D1_RECT_F br = D2D1::RectF(std::round(slotLeft + buttonInset), barTop + buttonCross,
                                               std::round(slotLeft + w + gap - buttonInset), barBottom - buttonCross);
            sb.hot = sb.hovered && t.slotHotTrack;
            sb.hotAt = D2D1::Point2F(focusX - br.left, (br.bottom - br.top) / 2.0f);
            sb.hotColor = t.accent;  // the placeholders' colour, as on the dock
            dc->SetTransform(D2D1::Matrix3x2F::Translation(br.left, br.top));
            Theme::DrawSlotButton(dc, D2D1::RectF(0.0f, 0.0f, br.right - br.left, br.bottom - br.top), t, u, sb);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
        }
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
        if (kApps[i].dot != Dot::None && !buttons) {  // round 11 phase 11A: the buttons replace the dots
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
    if (!showPopup_) {
        dc->PopLayer();
        return;
    }
    const float cardTop = std::round((wall.top + wall.bottom - cardH) / 2.0f);
    const D2D1_RECT_F popup = D2D1::RectF(cardLeft, cardTop, cardRight, cardTop + cardH);
    const float popupRadius = std::round((acrylic ? t.cornerRadius : std::min(t.cornerRadius, 12.0f)) * 0.6f * s);
    fillPanel(Rounded(popup, popupRadius), false);
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

// --- round 12 phase 12B: the theme gallery ---

void DrawMiniDock(ID2D1DeviceContext* dc, const D2D1_RECT_F& box, const ThemeConfig& t, float s) {
    if (box.right - box.left < 20.0f || box.bottom - box.top < 12.0f) return;
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (!factory || FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush.put()))) return;
    const float stroke = std::max(1.0f, std::round(s));
    // The wallpaper of the preview strip, clipped to a rounded box.
    winrt::com_ptr<ID2D1RoundedRectangleGeometry> clip;
    if (FAILED(factory->CreateRoundedRectangleGeometry(Rounded(box, std::round(4.0f * s)), clip.put()))) return;
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.get()), nullptr);
    const D2D1_GRADIENT_STOP stops[] = {
        {0.0f, D2D1::ColorF(0x3B6E8F)}, {0.55f, D2D1::ColorF(0x7A5C99)}, {1.0f, D2D1::ColorF(0xC9798A)}};
    winrt::com_ptr<ID2D1GradientStopCollection> stopCollection;
    winrt::com_ptr<ID2D1LinearGradientBrush> gradient;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, static_cast<UINT32>(std::size(stops)), stopCollection.put())) &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(box.left, box.top), D2D1::Point2F(box.right, box.bottom)),
            stopCollection.get(), gradient.put()))) {
        dc->FillRectangle(box, gradient.get());
    }

    // The dock at a third of its size: four icons of 48 DIPs with the theme's gap and padding.
    constexpr int kIcons = 4;
    constexpr uint32_t kIconColors[kIcons] = {0xFF3A86FF, 0xFF34C759, 0xFFFF9F0A, 0xFFAF52DE};
    constexpr int kStates[kIcons] = {2, 1, 0, 3};  // active, running, pinned only, attention
    const float u = 0.34f * s;
    const float icon = std::max(6.0f, std::round(48.0f * u));
    const float gap = std::max(1.0f, std::round(t.iconGap * u));
    const float padDip = std::clamp(t.barPadding, 0.0f, 24.0f);
    const float pad = std::round(padDip * u);
    const float lead = std::max(std::round(8.0f * u), std::round(gap / 2.0f));
    const bool buttons = SlotStyleOf(t) == SlotStyle::Buttons;
    const float rowH = icon + 2.0f * pad;
    const float reflH = t.reflection && !t.reflectionClip ? std::ceil(icon * t.reflectionHeight) : 0.0f;
    const float barH = rowH + reflH;
    const float barW = std::min(box.right - box.left - 2.0f * stroke, kIcons * icon + (kIcons - 1) * gap + 2.0f * lead);
    const float barLeft = std::round((box.left + box.right - barW) / 2.0f);
    const float barBottom = box.bottom - std::round(4.0f * s);
    const D2D1_RECT_F bar = D2D1::RectF(barLeft, barBottom - barH, barLeft + barW, barBottom);
    const bool shelf = t.shelf || t.perspective;
    const bool acrylic = t.acrylic && !shelf && Backdrop::SystemAllowsTransparency();
    if (shelf) {
        Theme::DrawShelf(dc, bar, t, u, stroke, -1.0f, reflH);
    } else {
        const float radius = std::round((acrylic ? SnapAcrylicRadius(t.cornerRadius) : t.cornerRadius) * u);
        const D2D1_ROUNDED_RECT shape = Rounded(bar, radius);
        if (acrylic) {
            brush->SetColor(D2D1::ColorF(0x6E6A8E, 0.85f));  // the blurred wallpaper, as the strip draws it
            dc->FillRoundedRectangle(shape, brush.get());
            brush->SetColor(Theme::ToColor(t.acrylicBackground));
        } else {
            brush->SetColor(Theme::ToColor(t.background));
        }
        dc->FillRoundedRectangle(shape, brush.get());
        Theme::DrawSheen(dc, shape.rect, shape.radiusX, t, stroke);
        brush->SetColor(Theme::ToColor(t.barBorder));
        D2D1_ROUNDED_RECT edge = shape;
        edge.rect = Inset(shape.rect, stroke / 2.0f);
        edge.radiusX = edge.radiusY = std::max(0.0f, shape.radiusX - stroke / 2.0f);
        dc->DrawRoundedRectangle(edge, brush.get(), stroke);
    }
    const float itemTop = bar.top + (buttons ? pad : std::round(padDip * 0.625f * u));
    const float dotR = std::max(1.0f, 2.0f * u);
    const float dotY = reflH > 0.0f ? itemTop + icon + std::round(pad * 0.625f)
                                    : barBottom - std::max(dotR + 1.0f, std::round(pad * 0.625f));
    const D2D1_COLOR_F dots[3] = {Theme::ToColor(t.dotRunning), Theme::ToColor(t.dotActive), Theme::ToColor(t.dotAttention)};
    const GlowMode glow = GlowOf(t);
    for (int i = 0; i < kIcons; ++i) {
        const float left = barLeft + lead + static_cast<float>(i) * (icon + gap);
        if (buttons && kStates[i] != 0) {
            Theme::SlotButton sb;
            sb.state = kStates[i];
            const float inset = std::max(1.0f, std::round(u));
            const float cross = std::round(std::max(1.0f, padDip / 2.0f) * u);
            const D2D1_RECT_F br = D2D1::RectF(std::round(left - gap / 2.0f + inset), bar.top + cross,
                                               std::round(left + icon + gap / 2.0f - inset), bar.bottom - cross);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(br.left, br.top));
            Theme::DrawSlotButton(dc, D2D1::RectF(0.0f, 0.0f, br.right - br.left, br.bottom - br.top), t, u, sb);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        const uint32_t color = kIconColors[i];
        const D2D1_RECT_F ir = D2D1::RectF(left, itemTop, left + icon, itemTop + icon);
        // The glow: a soft disc behind the icon (the second icon stands for the hovered app, the first the active one).
        const bool glowShown = glow == GlowMode::Always || (glow == GlowMode::Hover && i == 1) || (glow == GlowMode::Active && i == 0);
        if (glowShown && t.glowStrength > 0.0f) {
            const uint32_t gc = ((t.glowColor >> 24) ? t.glowColor : color) | 0xFF000000u;
            const D2D1_GRADIENT_STOP gs[] = {{0.0f, Theme::ToColor(gc, t.glowStrength)}, {1.0f, Theme::ToColor(gc, 0.0f)}};
            winrt::com_ptr<ID2D1GradientStopCollection> gsc;
            winrt::com_ptr<ID2D1RadialGradientBrush> rb;
            const D2D1_POINT_2F c = D2D1::Point2F((ir.left + ir.right) / 2.0f, (ir.top + ir.bottom) / 2.0f);
            const float r = icon * 0.5f + t.glowRadius * u;
            if (SUCCEEDED(dc->CreateGradientStopCollection(gs, 2, gsc.put())) &&
                SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, {}, r, r), gsc.get(), rb.put()))) {
                dc->FillEllipse(D2D1::Ellipse(c, r, r), rb.get());
            }
        }
        brush->SetColor(Theme::ToColor(color));
        dc->FillRoundedRectangle(D2D1::RoundedRect(ir, icon * 0.22f, icon * 0.22f), brush.get());
        if (t.reflection) {
            // The mirrored icon fading out under it (a gradient stands in for the mirrored pixels).
            const float h = reflH > 0.0f ? reflH : std::max(0.0f, bar.bottom - ir.bottom - stroke);
            const D2D1_GRADIENT_STOP rs[] = {{0.0f, Theme::ToColor(color, t.reflectionOpacity)}, {1.0f, Theme::ToColor(color, 0.0f)}};
            winrt::com_ptr<ID2D1GradientStopCollection> rsc;
            winrt::com_ptr<ID2D1LinearGradientBrush> lb;
            if (h > 0.0f && SUCCEEDED(dc->CreateGradientStopCollection(rs, 2, rsc.put())) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, ir.bottom + 1.0f), D2D1::Point2F(0, ir.bottom + 1.0f + h)),
                    rsc.get(), lb.put()))) {
                dc->FillRectangle(D2D1::RectF(ir.left, ir.bottom + 1.0f, ir.right, ir.bottom + 1.0f + h), lb.get());
            }
        }
        if (!buttons && kStates[i] != 0) {
            brush->SetColor(dots[kStates[i] == 2 ? 1 : kStates[i] == 3 ? 2 : 0]);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((ir.left + ir.right) / 2.0f, dotY), dotR, dotR), brush.get());
        }
    }
    dc->PopLayer();
}

// --- round 18 phase 18A: a layout's picture (the Add a dock gallery) ---

void DrawMiniLayout(ID2D1DeviceContext* dc, const D2D1_RECT_F& box, const DockConfig& layout, const ThemeConfig& t, float s) {
    if (box.right - box.left < 30.0f || box.bottom - box.top < 24.0f) return;
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush.put()))) return;
    // The display: a 16 : 9 screen in a dark bezel on a small stand, as big as the box allows.
    const float standH = std::round(7.0f * s);
    const float availW = box.right - box.left, availH = box.bottom - box.top - standH;
    float sw = availW, sh = std::round(sw * 9.0f / 16.0f);
    if (sh > availH) {
        sh = availH;
        sw = std::round(sh * 16.0f / 9.0f);
    }
    const float sx = std::round((box.left + box.right - sw) / 2.0f), sy = box.top;
    const D2D1_RECT_F screen = D2D1::RectF(sx, sy, sx + sw, sy + sh);
    const float bezel = std::max(2.0f, std::round(3.0f * s));
    brush->SetColor(D2D1::ColorF(0x1C1C1E));
    dc->FillRoundedRectangle(Rounded(screen, std::round(5.0f * s)), brush.get());
    const float mid = (screen.left + screen.right) / 2.0f;
    dc->FillRectangle(D2D1::RectF(mid - std::round(6.0f * s), screen.bottom, mid + std::round(6.0f * s), screen.bottom + standH - std::round(2.0f * s)),
                      brush.get());
    dc->FillRoundedRectangle(Rounded(D2D1::RectF(mid - std::round(16.0f * s), screen.bottom + standH - std::round(3.0f * s),
                                                 mid + std::round(16.0f * s), screen.bottom + standH),
                                     std::round(1.5f * s)),
                             brush.get());
    const D2D1_RECT_F in = Inset(screen, bezel);
    // The wallpaper of DrawMiniDock's tiles.
    const D2D1_GRADIENT_STOP stops[] = {
        {0.0f, D2D1::ColorF(0x3B6E8F)}, {0.55f, D2D1::ColorF(0x7A5C99)}, {1.0f, D2D1::ColorF(0xC9798A)}};
    winrt::com_ptr<ID2D1GradientStopCollection> stopCollection;
    winrt::com_ptr<ID2D1LinearGradientBrush> gradient;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, static_cast<UINT32>(std::size(stops)), stopCollection.put())) &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(in.left, in.top), D2D1::Point2F(in.right, in.bottom)), stopCollection.get(),
            gradient.put()))) {
        dc->FillRoundedRectangle(Rounded(in, std::round(2.0f * s)), gradient.get());
    }
    // The bar where the layout puts it (the chooser's silhouette geometry), a little off the edge
    // unless it is full width (as the dock's margin).
    const float thick = std::max(6.0f, std::round(std::min(in.right - in.left, in.bottom - in.top) * 0.13f));
    const float gap = layout.fullWidth || layout.Floating() ? 0.0f : std::max(1.0f, std::round(1.5f * s));
    const RECT inner{static_cast<LONG>(in.left + gap), static_cast<LONG>(in.top + gap), static_cast<LONG>(in.right - gap),
                     static_cast<LONG>(in.bottom - gap)};
    const RECT b = Presets::SilhouetteBar(layout, inner, static_cast<int>(thick));
    D2D1_RECT_F bar = D2D1::RectF(static_cast<float>(b.left), static_cast<float>(b.top), static_cast<float>(b.right), static_cast<float>(b.bottom));
    const bool vertical = IsVertical(layout.edge);
    const float radius = layout.fullWidth ? 0.0f : std::min(thick / 2.0f, std::round(t.cornerRadius * 0.25f * s));
    const bool acrylic = t.acrylic && !(t.shelf || t.perspective) && Backdrop::SystemAllowsTransparency();
    if (acrylic) {
        brush->SetColor(D2D1::ColorF(0x6E6A8E, 0.85f));  // the blurred wallpaper, as DrawMiniDock
        dc->FillRoundedRectangle(Rounded(bar, radius), brush.get());
        brush->SetColor(Theme::ToColor(t.acrylicBackground));
    } else {
        brush->SetColor(Theme::ToColor(t.background));
    }
    dc->FillRoundedRectangle(Rounded(bar, radius), brush.get());
    brush->SetColor(Theme::ToColor(t.barBorder));
    const float stroke = std::max(1.0f, std::round(s * 0.75f));
    dc->DrawRoundedRectangle(Rounded(Inset(bar, stroke / 2.0f), std::max(0.0f, radius - stroke / 2.0f)), brush.get(), stroke);
    // The items: icons (pins, running apps), a thin bar (a separator), a pill (a clock, a widget,
    // the tray, the notification centre), one after the other along the bar until it is full.
    constexpr uint32_t kIconColors[] = {0xFF3A86FF, 0xFF34C759, 0xFFFF9F0A, 0xFFAF52DE, 0xFFFF375F};
    const float pad = std::max(1.0f, std::round(thick * 0.18f));
    const float g = std::max(2.0f, thick - 2.0f * pad);  // a glyph's size across the bar
    const float step = std::max(1.0f, std::round(g * 0.3f));
    const float mainStart = (vertical ? bar.top : bar.left) + pad + (radius > g / 2.0f ? radius - g / 2.0f : 0.0f);
    const float mainEnd = (vertical ? bar.bottom : bar.right) - pad;
    const float cross = (vertical ? bar.left : bar.top) + pad;
    float at = mainStart;
    int color = 0;
    auto place = [&](float length) -> D2D1_RECT_F {
        return vertical ? D2D1::RectF(cross, at, cross + g, at + length) : D2D1::RectF(at, cross, at + length, cross + g);
    };
    auto icon = [&]() {
        if (at + g > mainEnd) return false;
        brush->SetColor(Theme::ToColor(kIconColors[color++ % std::size(kIconColors)]));
        dc->FillRoundedRectangle(Rounded(place(g), g * 0.24f), brush.get());
        at += g + step;
        return true;
    };
    for (const DockItemEntry& e : layout.items) {
        bool more = true;
        if (CompareStringOrdinal(e.kind.c_str(), -1, L"pins", -1, TRUE) == CSTR_EQUAL) {
            for (int k = 0; k < 3 && more; ++k) more = icon();
        } else {
            switch (e.Kind()) {
                case ItemKind::Running: more = icon() && icon(); break;
                case ItemKind::Separator: {
                    const float w = std::max(1.0f, std::round(s * 0.75f));
                    if (at + w > mainEnd) {
                        more = false;
                        break;
                    }
                    brush->SetColor(Theme::ToColor(t.separator));
                    const D2D1_RECT_F r = place(w);
                    dc->FillRectangle(vertical ? D2D1::RectF(r.left, r.top, r.right, r.top + w) : D2D1::RectF(r.left, r.top, r.left + w, r.bottom),
                                      brush.get());
                    at += w + step;
                    break;
                }
                case ItemKind::App:
                case ItemKind::Folder:
                case ItemKind::Group:
                case ItemKind::RecycleBin: more = icon(); break;
                default: {
                    // A widget-like item: a pill twice as long as an icon, in the theme's text colour.
                    const float len = std::round(g * 2.0f);
                    if (at + len > mainEnd) {
                        more = false;
                        break;
                    }
                    brush->SetColor(Theme::ToColor(t.clockText, 0.6f));
                    dc->FillRoundedRectangle(Rounded(place(len), g / 2.0f), brush.get());
                    at += len + step;
                    break;
                }
            }
        }
        if (!more) break;
    }
}

SettingsGallery::~SettingsGallery() {
    if (hwnd_ && IsWindow(hwnd_)) SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
}

bool SettingsGallery::Register(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &SettingsGallery::Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

void SettingsGallery::Attach(HWND hwnd) {
    if (hwnd == hwnd_) return;
    hwnd_ = hwnd;
    if (hwnd) SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}

void SettingsGallery::SetTiles(std::vector<Tile> tiles, int selected) {
    tiles_ = std::move(tiles);
    selected_ = tiles_.empty() ? 0 : std::clamp(selected, 0, static_cast<int>(tiles_.size()) - 1);
    hot_ = -1;
    notifiedHot_ = Hot();  // round 18 phase 18A: the page reads Hot() after new tiles itself
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsGallery::SetColors(COLORREF back, COLORREF text, COLORREF muted, COLORREF accent) {
    back_ = back;
    text_ = text;
    muted_ = muted;
    accent_ = accent;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

int SettingsGallery::HeightDip(int count, int widthDip) {
    const int cols = std::max(1, (widthDip + kGapX) / (kTileW + kGapX));
    const int rows = std::max(1, (count + cols - 1) / cols);
    return rows * kTileH + (rows - 1) * kGapY;
}

float SettingsGallery::Scale() const {
    return static_cast<float>(hwnd_ ? GetDpiForWindow(hwnd_) : USER_DEFAULT_SCREEN_DPI) / USER_DEFAULT_SCREEN_DPI;
}

void SettingsGallery::SetWide(int columns, int tileHDip) {
    columns = std::max(0, columns);
    if (columns == wideColumns_ && tileHDip == wideTileH_) return;
    wideColumns_ = columns;
    wideTileH_ = std::max(48, tileHDip);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

int SettingsGallery::WideHeightDip(int columns, int tileHDip) const {
    // Round 18 phase 18A: TileRect's walk in DIPs (rows of `columns`, a caption starts a row).
    columns = std::max(1, columns);
    tileHDip = std::max(48, tileHDip);
    if (tiles_.empty()) return tileHDip;
    int col = 0, y = 0;
    for (size_t i = 0; i < tiles_.size(); ++i) {
        const bool caption = !tiles_[i].caption.empty();
        if (i > 0 && (col == columns || (caption && col > 0))) {
            col = 0;
            y += tileHDip + kWideGap;
        }
        if (caption) y += kCaptionH;
        ++col;
    }
    return y + tileHDip;
}

int SettingsGallery::Hot() const {
    if (hot_ >= 0) return hot_;
    return hwnd_ && GetFocus() == hwnd_ && !tiles_.empty() ? selected_ : -1;
}

void SettingsGallery::NotifyHot() {
    if (!notifyHot_ || !hwnd_) return;
    const int now = Hot();
    if (now == notifiedHot_) return;
    notifiedHot_ = now;
    SendMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd_), kHot), reinterpret_cast<LPARAM>(hwnd_));
}

int SettingsGallery::Columns() const {
    if (wideColumns_ > 0) return wideColumns_;
    RECT rc{};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    const int widthDip = static_cast<int>(std::lround(static_cast<float>(rc.right) / Scale()));
    return std::max(1, (widthDip + kGapX) / (kTileW + kGapX));
}

D2D1_RECT_F SettingsGallery::TileRect(int index) const {
    const float s = Scale();
    const int cols = Columns();
    if (wideColumns_ > 0) {
        // Round 13 phase 13H: the width shared out between the columns (whole pixels, the gaps equal).
        RECT rc{};
        if (hwnd_) GetClientRect(hwnd_, &rc);
        const float gap = std::round(kWideGap * s);
        const float w = (static_cast<float>(rc.right) - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        // Round 18 phase 18A: a tile with a caption starts a new row under its caption.
        const float tileH = std::round(wideTileH_ * s), captionH = std::round(kCaptionH * s);
        int col = 0;
        float y = 0.0f;
        for (int i = 0; i <= index && i < Count(); ++i) {
            const bool caption = !tiles_[static_cast<size_t>(i)].caption.empty();
            if (i > 0 && (col == cols || (caption && col > 0))) {
                col = 0;
                y += tileH + gap;
            }
            if (caption) y += captionH;
            if (i < index) ++col;
        }
        if (index >= Count()) col = index % cols;  // past the end (never drawn)
        const float x = std::round(static_cast<float>(col) * (w + gap));
        y = std::round(y);
        return D2D1::RectF(x, y, std::round(static_cast<float>(col) * (w + gap) + w), y + tileH);
    }
    const float x = std::round(static_cast<float>((index % cols) * (kTileW + kGapX)) * s);
    const float y = std::round(static_cast<float>((index / cols) * (kTileH + kGapY)) * s);
    return D2D1::RectF(x, y, x + std::round(kTileW * s), y + std::round(kTileH * s));
}

int SettingsGallery::HitTest(POINT pt) const {
    for (int i = 0; i < Count(); ++i) {
        const D2D1_RECT_F r = TileRect(i);
        const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

void SettingsGallery::Select(int index) {
    if (tiles_.empty()) return;
    index = std::clamp(index, 0, Count() - 1);
    if (index == selected_) return;
    selected_ = index;
    InvalidateRect(hwnd_, nullptr, FALSE);
    NotifyHot();  // round 18 phase 18A (the keyboard's selection is the hot tile while focused)
}

void SettingsGallery::Choose(int index) {
    if (index < 0 || index >= Count()) return;
    selected_ = index;
    InvalidateRect(hwnd_, nullptr, FALSE);
    const int id = GetDlgCtrlID(hwnd_);
    SendMessageW(GetParent(hwnd_), WM_COMMAND, MAKEWPARAM(id, kChosen), reinterpret_cast<LPARAM>(hwnd_));
}

IDWriteTextFormat* SettingsGallery::Format(winrt::com_ptr<IDWriteTextFormat>& slot, float& size, float want,
                                           DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
    if (slot && size == want) return slot.get();
    slot = nullptr;
    size = 0.0f;
    if (!dwrite_ || FAILED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                     DWRITE_FONT_STRETCH_NORMAL, want, L"", slot.put()))) {
        slot = nullptr;
        return nullptr;
    }
    slot->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    slot->SetTextAlignment(align);
    const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    winrt::com_ptr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(slot.get(), ellipsis.put()))) slot->SetTrimming(&trim, ellipsis.get());
    size = want;
    return slot.get();
}

void SettingsGallery::Paint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    bool drawn = false;
    if (hdc && rc.right > 0 && rc.bottom > 0) {
        if (!factory_) {
            D2D1_FACTORY_OPTIONS options{};
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, factory_.put_void()))) {
                factory_ = nullptr;
            }
        }
        if (!dwrite_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                   reinterpret_cast<IUnknown**>(dwrite_.put())))) {
            dwrite_ = nullptr;
        }
        if (factory_ && !target_) {
            // Software, bound to the paint DC, as the preview strip: no second D3D device, and
            // PrintWindow / WM_PRINTCLIENT see the tiles.
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            if (FAILED(factory_->CreateDCRenderTarget(&props, target_.put()))) target_ = nullptr;
        }
        winrt::com_ptr<ID2D1DeviceContext> dc = target_ ? target_.try_as<ID2D1DeviceContext>() : nullptr;
        if (dc && SUCCEEDED(target_->BindDC(hdc, &rc))) {
            const float s = Scale();
            target_->BeginDraw();
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->Clear(FromColorRef(back_));
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(FromColorRef(text_), brush.put()))) brush = nullptr;
            IDWriteTextFormat* name = Format(nameFormat_, nameSize_, std::round(12.0f * s), DWRITE_FONT_WEIGHT_NORMAL);
            IDWriteTextFormat* detail = Format(detailFormat_, detailSize_, std::round(11.0f * s), DWRITE_FONT_WEIGHT_NORMAL);
            const bool focused = GetFocus() == hwnd_;
            const float stroke = std::max(1.0f, std::round(s));
            if (wideColumns_ > 0 && brush) {
                PaintWide(dc.get(), brush.get(), ps.rcPaint, focused, s, stroke);
            }
            for (int i = 0; i < Count() && brush && wideColumns_ == 0; ++i) {
                const Tile& tile = tiles_[static_cast<size_t>(i)];
                const D2D1_RECT_F r = TileRect(i);
                if (r.top > static_cast<float>(ps.rcPaint.bottom) || r.bottom < static_cast<float>(ps.rcPaint.top)) continue;
                const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(Inset(r, stroke), 6.0f * s, 6.0f * s);
                if (i == hot_ || tile.current) {
                    brush->SetColor(FromColorRef(tile.current ? accent_ : text_, tile.current ? 0.14f : 0.07f));
                    dc->FillRoundedRectangle(shape, brush.get());
                }
                if (tile.current) {
                    brush->SetColor(FromColorRef(accent_));
                    dc->DrawRoundedRectangle(shape, brush.get(), 2.0f * stroke);
                }
                if (focused && i == selected_) {
                    // The keyboard focus: a dotted ring inside the tile.
                    winrt::com_ptr<ID2D1StrokeStyle> dash;
                    const D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
                        D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.0f,
                        D2D1_DASH_STYLE_DOT);
                    if (FAILED(factory_->CreateStrokeStyle(sp, nullptr, 0, dash.put()))) dash = nullptr;
                    brush->SetColor(FromColorRef(text_));
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(Inset(r, 3.0f * stroke), 4.0f * s, 4.0f * s), brush.get(),
                                             stroke, dash.get());
                }
                const D2D1_RECT_F pic = D2D1::RectF(r.left + std::round(6.0f * s), r.top + std::round(6.0f * s),
                                                    r.right - std::round(6.0f * s), r.top + std::round(50.0f * s));
                if (tile.layout) {
                    DrawMiniLayout(dc.get(), pic, tile.dock, tile.theme, s);  // round 18 phase 18A
                } else {
                    DrawMiniDock(dc.get(), pic, tile.theme, s);
                }
                dc->SetTransform(D2D1::Matrix3x2F::Identity());
                if (name) {
                    brush->SetColor(FromColorRef(text_));
                    dc->DrawText(tile.name.c_str(), static_cast<UINT32>(tile.name.size()), name,
                                 D2D1::RectF(r.left + 4.0f * s, pic.bottom + 4.0f * s, r.right - 4.0f * s, pic.bottom + 21.0f * s),
                                 brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                if (detail && !tile.detail.empty()) {
                    brush->SetColor(FromColorRef(muted_));
                    dc->DrawText(tile.detail.c_str(), static_cast<UINT32>(tile.detail.size()), detail,
                                 D2D1::RectF(r.left + 4.0f * s, pic.bottom + 21.0f * s, r.right - 4.0f * s, r.bottom - 2.0f * s),
                                 brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
            const HRESULT hr = target_->EndDraw();
            drawn = SUCCEEDED(hr);
            if (hr == D2DERR_RECREATE_TARGET) {
                target_ = nullptr;  // a new target on the next paint (one retry per loss)
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
    }
    if (!drawn && hdc) {
        HBRUSH b = CreateSolidBrush(back_);
        FillRect(hdc, &ps.rcPaint, b);
        DeleteObject(b);
    }
    EndPaint(hwnd_, &ps);
}

void SettingsGallery::PaintWide(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, const RECT& paint, bool focused, float s,
                                float stroke) {
    // Round 13 phase 13H (Quick setup's look grid; the prototype's .look tiles): picture, name,
    // tagline, the current tile ringed with a check badge in its corner.
    IDWriteTextFormat* name = Format(nameLeft_, nameLeftSize_, std::round(13.0f * s), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    IDWriteTextFormat* detail = Format(detailLeft_, detailLeftSize_, std::round(11.5f * s), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    const int lum = (GetRValue(accent_) * 299 + GetGValue(accent_) * 587 + GetBValue(accent_) * 114) / 1000;
    // The badge's check: the accent nearly black on a light accent (as Quick setup's other marks).
    const D2D1_COLOR_F mark = lum > 150 ? FromColorRef(RGB(GetRValue(accent_) * 12 / 100, GetGValue(accent_) * 12 / 100, GetBValue(accent_) * 12 / 100))
                                        : FromColorRef(RGB(0xFF, 0xFF, 0xFF));
    winrt::com_ptr<ID2D1StrokeStyle> roundCaps;
    const D2D1_STROKE_STYLE_PROPERTIES rp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                        D2D1_LINE_JOIN_ROUND);
    if (FAILED(factory_->CreateStrokeStyle(rp, nullptr, 0, roundCaps.put()))) roundCaps = nullptr;
    for (int i = 0; i < Count(); ++i) {
        const Tile& tile = tiles_[static_cast<size_t>(i)];
        const D2D1_RECT_F r = TileRect(i);
        if (r.top > static_cast<float>(paint.bottom) || r.bottom < static_cast<float>(paint.top)) continue;
        const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(Inset(r, stroke), 10.0f * s, 10.0f * s);
        if (i == hot_ && !tile.current) {
            brush->SetColor(FromColorRef(text_, 0.05f));
            dc->FillRoundedRectangle(shape, brush);
        }
        if (tile.current) {
            brush->SetColor(FromColorRef(accent_));
            dc->DrawRoundedRectangle(shape, brush, 2.0f * stroke);
        }
        const float pad = std::round(6.0f * s), textH = std::round(40.0f * s);
        const D2D1_RECT_F pic = D2D1::RectF(r.left + pad, r.top + pad, r.right - pad, std::max(r.top + pad + 20.0f * s, r.bottom - pad - textH));
        if (tile.layout) {
            DrawMiniLayout(dc, pic, tile.dock, tile.theme, s);  // round 18 phase 18A
        } else {
            DrawMiniDock(dc, pic, tile.theme, s);
        }
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        if (!tile.caption.empty()) {
            // Round 18 phase 18A: the caption over the row this tile starts ("Mine").
            if (IDWriteTextFormat* cf = Format(captionFormat_, captionSize_, std::round(12.0f * s), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                               DWRITE_TEXT_ALIGNMENT_LEADING)) {
                brush->SetColor(FromColorRef(muted_));
                dc->DrawText(tile.caption.c_str(), static_cast<UINT32>(tile.caption.size()), cf,
                             D2D1::RectF(r.left + 2.0f * s, r.top - std::round(kCaptionH * s), r.right, r.top - 2.0f * s), brush,
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
        if (name) {
            brush->SetColor(FromColorRef(text_));
            dc->DrawText(tile.name.c_str(), static_cast<UINT32>(tile.name.size()), name,
                         D2D1::RectF(pic.left + 4.0f * s, pic.bottom + 5.0f * s, r.right - pad, pic.bottom + 23.0f * s), brush,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (detail && !tile.detail.empty()) {
            brush->SetColor(FromColorRef(muted_));
            dc->DrawText(tile.detail.c_str(), static_cast<UINT32>(tile.detail.size()), detail,
                         D2D1::RectF(pic.left + 4.0f * s, pic.bottom + 23.0f * s, r.right - pad, pic.bottom + 39.0f * s), brush,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (tile.current) {
            const float cx = r.right - std::round(20.0f * s), cy = r.top + std::round(20.0f * s), rad = 10.0f * s;
            brush->SetColor(FromColorRef(accent_));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), brush);
            // The prototype's check (13-unit box) at 11 px.
            const float k = 11.0f * s / 13.0f, ox = cx - 5.5f * s, oy = cy - 5.5f * s;
            winrt::com_ptr<ID2D1PathGeometry> path;
            winrt::com_ptr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(factory_->CreatePathGeometry(path.put())) && SUCCEEDED(path->Open(sink.put()))) {
                sink->BeginFigure(D2D1::Point2F(ox + 2.5f * k, oy + 7.0f * k), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(ox + 5.5f * k, oy + 10.0f * k));
                sink->AddLine(D2D1::Point2F(ox + 10.5f * k, oy + 4.0f * k));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                if (SUCCEEDED(sink->Close())) {
                    brush->SetColor(mark);
                    dc->DrawGeometry(path.get(), brush, 2.2f * k, roundCaps.get());
                }
            }
        }
        if (focused && i == selected_) {
            winrt::com_ptr<ID2D1StrokeStyle> dash;
            const D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                                D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_DOT);
            if (FAILED(factory_->CreateStrokeStyle(sp, nullptr, 0, dash.put()))) dash = nullptr;
            brush->SetColor(FromColorRef(text_));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(Inset(r, 4.0f * stroke), 7.0f * s, 7.0f * s), brush, stroke, dash.get());
        }
    }
}

LRESULT CALLBACK SettingsGallery::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (auto* self = reinterpret_cast<SettingsGallery*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        return self->OnMessage(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SettingsGallery::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE: {
            LRESULT code = DLGC_WANTARROWS | DLGC_WANTCHARS;
            if (const MSG* m = reinterpret_cast<const MSG*>(lp); m && m->message == WM_KEYDOWN && m->wParam == VK_RETURN) {
                code |= DLGC_WANTMESSAGE;  // Enter chooses the selected tile
            }
            return code;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd_, nullptr, FALSE);
            NotifyHot();
            return 0;
        case WM_KEYDOWN: {
            const int cols = Columns();
            switch (wp) {
                case VK_LEFT: Select(selected_ - 1); return 0;
                case VK_RIGHT: Select(selected_ + 1); return 0;
                case VK_UP:
                    if (selected_ - cols >= 0) Select(selected_ - cols);
                    return 0;
                case VK_DOWN:
                    if (selected_ + cols < Count()) Select(selected_ + cols);
                    return 0;
                case VK_HOME: Select(0); return 0;
                case VK_END: Select(Count() - 1); return 0;
                case VK_RETURN: Choose(selected_); return 0;
                default: break;
            }
            break;
        }
        case WM_CHAR:
            if (wp == L' ') Choose(selected_);
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            const int hit = HitTest(POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
            if (hit >= 0) Choose(hit);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                tracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            const int hit = HitTest(POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
            if (hit != hot_) {
                hot_ = hit;
                InvalidateRect(hwnd_, nullptr, FALSE);
                NotifyHot();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (hot_ >= 0) {
                hot_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
                NotifyHot();
            }
            return 0;
        case kMsgSelect:
            Select(static_cast<int>(wp));
            return selected_;
        case kMsgChoose:
            if (static_cast<int>(wp) < 0 || static_cast<int>(wp) >= Count()) return 0;
            Choose(static_cast<int>(wp));
            return 1;
        case WM_DESTROY: {
            target_ = nullptr;
            nameFormat_ = detailFormat_ = nameLeft_ = detailLeft_ = captionFormat_ = nullptr;
            nameSize_ = detailSize_ = nameLeftSize_ = detailLeftSize_ = captionSize_ = 0.0f;
            dwrite_ = nullptr;
            factory_ = nullptr;
            HWND self = hwnd_;
            SetWindowLongPtrW(self, GWLP_USERDATA, 0);
            hwnd_ = nullptr;
            return DefWindowProcW(self, msg, wp, lp);
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
