#include "WeatherPopup.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

#include "Theme.h"

namespace {

// Layout in DIPs.
constexpr float kPad = 14.0f;
constexpr int kHours = 24;
constexpr int kDays = 7;
constexpr float kColumnW = 21.0f;                      // one hourly column
constexpr float kWidth = 2.0f * kPad + kHours * kColumnW;  // 532
constexpr float kMessageWidth = 320.0f;
constexpr float kButton = 28.0f;                       // refresh button
constexpr float kTitleTop = 12.0f, kTitleH = 22.0f;
constexpr float kMainTop = 38.0f;
constexpr float kBigGlyph = 50.0f;
constexpr float kRightColumnW = 180.0f;
constexpr float kRule1 = 100.0f;                       // hairline under the header
constexpr float kHourlyTop = 108.0f;
constexpr float kHourlyH = 92.0f;
constexpr float kRuleGap = 6.0f;
constexpr float kRowH = 24.0f;                         // one daily row
constexpr float kBottomPad = 12.0f;
constexpr float kRadius = 6.0f;                        // hot boxes
constexpr wchar_t kCredit[] = L"Weather data by Open-Meteo";
constexpr wchar_t kCreditUrl[] = L"https://open-meteo.com/";
constexpr wchar_t kSettingsText[] = L"Weather settings…";

LONG Px(float dip, float scale) { return static_cast<LONG>(std::lround(dip * scale)); }

D2D1_RECT_F ToRectF(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                       static_cast<float>(r.bottom));
}

bool Empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

}  // namespace

const WeatherService* WeatherPopup::W() const {
    static const WeatherService kNone;  // no service (yet) for the place: "No location" / no data
    const WeatherService* w = source_ ? source_() : nullptr;
    return w ? w : &kNone;
}

void WeatherPopup::Init(HINSTANCE instance, Renderer* renderer, std::function<const WeatherService*()> source,
                        Callbacks callbacks) {
    PopupWindow::Init(instance, renderer);
    source_ = std::move(source);
    callbacks_ = std::move(callbacks);
    SetClosedCallback(callbacks_.closed);
}

void WeatherPopup::OnDestroying() {
    titleFormat_ = bigFormat_ = bodyFormat_ = smallFormat_ = tinyFormat_ = microFormat_ = nullptr;
    formatScale_ = 0.0f;
}

bool WeatherPopup::Show(const Style& style, const PopupAnchor& anchor) {
    if (!source_) return false;
    style_ = style;
    frame_ = Frame{};
    frame_.scale = style.scale;
    frame_.acrylic = style.acrylic;
    frame_.cornerRadiusDip = style.cornerRadiusDip;
    frame_.monitor = style.monitor;
    anchor_ = anchor;
    return ShowAt(frame_, anchor_);
}

void WeatherPopup::Update() {
    if (!Visible()) return;
    const SIZE before = ClientSize();
    const bool wasFull = full_;
    const SIZE after = Measure();
    if (after.cx == before.cx && after.cy == before.cy && wasFull == full_) {
        if (SUCCEEDED(Redraw())) Commit();  // same size: no re-placement (and no activation)
        return;
    }
    ShowAt(frame_, anchor_);
}

void WeatherPopup::EnsureFormats() {
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    const float s = style_.scale;
    if (!dw || (titleFormat_ && formatScale_ == s)) return;
    titleFormat_ = bigFormat_ = bodyFormat_ = smallFormat_ = tinyFormat_ = microFormat_ = nullptr;
    auto make = [&](winrt::com_ptr<IDWriteTextFormat>& f, DWRITE_FONT_WEIGHT weight, float size) {
        if (FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size * s, L"", f.put()))) {
            f = nullptr;
            return;
        }
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    };
    make(titleFormat_, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f);
    make(bigFormat_, DWRITE_FONT_WEIGHT_LIGHT, 36.0f);
    make(bodyFormat_, DWRITE_FONT_WEIGHT_NORMAL, 13.0f);
    make(smallFormat_, DWRITE_FONT_WEIGHT_NORMAL, 12.0f);
    make(tinyFormat_, DWRITE_FONT_WEIGHT_NORMAL, 10.5f);
    make(microFormat_, DWRITE_FONT_WEIGHT_NORMAL, 9.5f);
    // 24-hour clock unless the locale's time format has an AM/PM marker.
    wchar_t fmt[80]{};
    hours24_ = !GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STIMEFORMAT, fmt, 80) || !wcschr(fmt, L't');
    formatScale_ = s;
}

void WeatherPopup::Text(ID2D1DeviceContext* dc, IDWriteTextFormat* format, const std::wstring& text,
                        const D2D1_RECT_F& rect, uint32_t color, float alpha, DWRITE_TEXT_ALIGNMENT align) const {
    if (!format || text.empty() || rect.right <= rect.left) return;
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(color, alpha), brush.put()))) return;
    format->SetTextAlignment(align);
    dc->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float WeatherPopup::TextWidth(IDWriteTextFormat* format, const std::wstring& text) const {
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (!dw || !format || text.empty() ||
        FAILED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 4096.0f, 1024.0f,
                                    layout.put()))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS m{};
    return SUCCEEDED(layout->GetMetrics(&m)) ? m.widthIncludingTrailingWhitespace : 0.0f;
}

std::wstring WeatherPopup::Temp(double value, bool unit) const {
    if (!std::isfinite(value)) return L"–";
    long v = std::lround(value);
    if (v == 0) v = 0;  // no "-0"
    std::wstring s = std::to_wstring(v) + L"°";
    if (unit) s += W()->Settings().units == Units::Imperial ? L"F" : L"C";
    return s;
}

std::wstring WeatherPopup::HourLabel(int hour) const {
    if (hours24_) return std::to_wstring(hour);
    const int h = hour % 12 == 0 ? 12 : hour % 12;
    return std::to_wstring(h) + (hour < 12 ? L"a" : L"p");
}

std::wstring WeatherPopup::Message() const {
    if (!W()->Settings().Configured()) return L"No location is set.";
    if (W()->Busy()) return L"Loading the forecast…";
    if (!W()->LastError().empty()) return L"Weather unavailable: " + W()->LastError();
    return L"No weather data yet.";
}

SIZE WeatherPopup::Measure() {
    EnsureFormats();
    const float s = style_.scale;
    const WeatherService::Forecast& data = W()->Data();
    full_ = data.valid;
    const LONG pad = Px(kPad, s);
    if (!full_) {
        const LONG w = Px(kMessageWidth, s);
        LONG h = Px(kMainTop + 40.0f, s);
        refreshRect_ = W()->Settings().Configured()
                           ? RECT{w - pad - Px(kButton, s), Px(9.0f, s), w - pad, Px(9.0f, s) + Px(kButton, s)}
                           : RECT{};
        creditRect_ = RECT{};
        if (!W()->Settings().Configured()) {
            const LONG bw = static_cast<LONG>(std::ceil(TextWidth(bodyFormat_.get(), kSettingsText))) + Px(24.0f, s);
            settingsRect_ = RECT{(w - bw) / 2, h, (w + bw) / 2, h + Px(28.0f, s)};
            h = settingsRect_.bottom + Px(kBottomPad, s);
        } else {
            settingsRect_ = RECT{};
            h += Px(kBottomPad, s);
        }
        return SIZE{w, h};
    }
    settingsRect_ = RECT{};
    const LONG w = Px(kWidth, s);
    refreshRect_ = RECT{w - pad - Px(kButton, s), Px(9.0f, s), w - pad, Px(9.0f, s) + Px(kButton, s)};
    const LONG creditW = static_cast<LONG>(std::ceil(TextWidth(smallFormat_.get(), kCredit)));
    const LONG creditTop = Px(kMainTop + 28.0f, s);
    creditRect_ = RECT{w - pad - creditW, creditTop, w - pad, creditTop + Px(18.0f, s)};
    const size_t days = std::min<size_t>(kDays, W()->DailyWindow(kDays).size());
    const float dailyTop = kHourlyTop + kHourlyH + 2.0f * kRuleGap;
    const LONG h = Px(dailyTop + static_cast<float>(days) * kRowH + kBottomPad, s);
    return SIZE{w, h};
}

void WeatherPopup::Paint(ID2D1DeviceContext* dc) {
    EnsureFormats();
    const float s = style_.scale;
    const float w = static_cast<float>(ClientSize().cx);
    const float pad = std::round(kPad * s);
    const float stroke = std::max(1.0f, std::round(s));
    const float inset = stroke / 2.0f;
    const int hot = Hot();
    PaintPanel(dc, style_.tint, style_.border);
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(style_.text), brush.put()))) return;
    const WeatherService::Forecast& data = W()->Data();
    const bool stale = W()->State() == WeatherService::Status::Stale;
    WeatherGlyphs::Colors glyphs = style_.glyphs;

    auto hotBox = [&](const RECT& r, bool isHot) {
        if (Empty(r)) return;
        const float radius = std::round(kRadius * s);
        const D2D1_RECT_F f = ToRectF(r);
        brush->SetColor(Theme::ToColor(style_.hotBackground, isHot ? 1.0f : 0.6f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(f, radius, radius), brush.get());
        if (!isHot) return;
        brush->SetColor(Theme::ToColor(style_.hotBorder));
        const D2D1_RECT_F edge = D2D1::RectF(f.left + inset, f.top + inset, f.right - inset, f.bottom - inset);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(edge, radius - inset, radius - inset), brush.get(), stroke);
    };
    auto rule = [&](float yDip) {
        const float y = std::round(yDip * s) - inset;
        brush->SetColor(Theme::ToColor(style_.border));
        dc->DrawLine(D2D1::Point2F(pad, y), D2D1::Point2F(w - pad, y), brush.get(), stroke);
    };

    // Title row: the location and the refresh button.
    std::wstring title = !style_.title.empty() ? style_.title : W()->Settings().location;
    if (title.empty()) title = L"Weather";
    const float titleRight = Empty(refreshRect_) ? w - pad : static_cast<float>(refreshRect_.left) - 8.0f * s;
    Text(dc, titleFormat_.get(), title, D2D1::RectF(pad, kTitleTop * s, titleRight, (kTitleTop + kTitleH) * s),
         style_.text, 1.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!Empty(refreshRect_)) {
        const bool isHot = hot == kHitRefresh;
        if (isHot) hotBox(refreshRect_, true);
        const D2D1_RECT_F r = ToRectF(refreshRect_);
        const float g = std::round(16.0f * s);
        const float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f;
        WeatherGlyphs::DrawRefresh(dc, D2D1::RectF(cx - g / 2.0f, cy - g / 2.0f, cx + g / 2.0f, cy + g / 2.0f),
                                   W()->Busy() ? (style_.text & 0x00FFFFFFu) | 0x66000000u
                                                    : (isHot ? style_.text : style_.secondary));
    }

    if (!full_) {
        const float top = kMainTop * s;
        Text(dc, bodyFormat_.get(), Message(), D2D1::RectF(pad, top, w - pad, top + 28.0f * s), style_.secondary, 1.0f,
             DWRITE_TEXT_ALIGNMENT_LEADING);
        if (!Empty(settingsRect_)) {
            hotBox(settingsRect_, hot == kHitSettings);
            Text(dc, bodyFormat_.get(), kSettingsText, ToRectF(settingsRect_), style_.text, 1.0f,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        return;
    }

    // Header: glyph, temperature, condition, details; updated + credit on the right.
    glyphs.opacity = stale ? 0.6f : 1.0f;
    const float glyphPx = std::round(kBigGlyph * s);
    const float mainTop = kMainTop * s;
    WeatherGlyphs::Draw(dc, WeatherGlyphs::FromWmo(data.code), data.isDay,
                        D2D1::RectF(pad, mainTop + 2.0f * s, pad + glyphPx, mainTop + 2.0f * s + glyphPx), glyphs);
    const std::wstring temp = Temp(data.temp, true);
    const float tempX = pad + glyphPx + 8.0f * s;
    const float tempW = std::ceil(TextWidth(bigFormat_.get(), temp));
    Text(dc, bigFormat_.get(), temp, D2D1::RectF(tempX, mainTop, tempX + tempW + 2.0f, mainTop + 54.0f * s),
         style_.text, stale ? 0.6f : 1.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
    const float rightCol = w - pad - std::round(kRightColumnW * s);
    const float condX = tempX + tempW + 14.0f * s;
    Text(dc, bodyFormat_.get(), WeatherGlyphs::Describe(data.code),
         D2D1::RectF(condX, mainTop + 6.0f * s, rightCol - 8.0f * s, mainTop + 26.0f * s), style_.text, 1.0f,
         DWRITE_TEXT_ALIGNMENT_LEADING);
    std::wstring details;
    if (std::isfinite(data.humidity)) details = L"Humidity " + std::to_wstring(std::lround(data.humidity)) + L"%";
    if (std::isfinite(data.wind)) {
        if (!details.empty()) details += L" · ";
        details += L"Wind " + std::to_wstring(std::lround(data.wind)) +
                   (W()->Settings().units == Units::Imperial ? L" mph" : L" km/h");
    }
    Text(dc, smallFormat_.get(), details, D2D1::RectF(condX, mainTop + 28.0f * s, rightCol - 8.0f * s, mainTop + 46.0f * s),
         style_.secondary, 1.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
    std::wstring updated = L"Updated " + WeatherService::FormatTime(data.fetchedAt);
    if (stale) updated += L" (stale)";
    Text(dc, smallFormat_.get(), updated, D2D1::RectF(rightCol, mainTop + 6.0f * s, w - pad, mainTop + 24.0f * s),
         style_.secondary, 1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);
    Text(dc, smallFormat_.get(), kCredit, ToRectF(creditRect_), hot == kHitCredit ? style_.text : style_.secondary,
         1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (hot == kHitCredit) {  // underline while hovered: it is a link
        const float y = static_cast<float>(creditRect_.bottom) - 2.0f * s;
        brush->SetColor(Theme::ToColor(style_.text, 0.7f));
        dc->DrawLine(D2D1::Point2F(static_cast<float>(creditRect_.left), y),
                     D2D1::Point2F(static_cast<float>(creditRect_.right), y), brush.get(), stroke);
    }
    rule(kRule1);

    // Hourly strip.
    glyphs.opacity = 1.0f;
    const std::vector<WeatherService::Hour> hours = W()->HourlyWindow(kHours);
    const float colW = (w - 2.0f * pad) / static_cast<float>(kHours);
    const float top = kHourlyTop * s;
    double lo = 1e9, hi = -1e9;
    for (const auto& h : hours) {
        if (!std::isfinite(h.temp)) continue;
        lo = std::min(lo, h.temp);
        hi = std::max(hi, h.temp);
    }
    const float bandTop = top + 54.0f * s, bandBottom = top + 74.0f * s;
    auto bandY = [&](double t) {
        if (!(hi > lo)) return (bandTop + bandBottom) / 2.0f;
        return bandBottom - static_cast<float>((t - lo) / (hi - lo)) * (bandBottom - bandTop);
    };
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    winrt::com_ptr<ID2D1PathGeometry> line;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    bool figure = false;
    if (factory && SUCCEEDED(factory->CreatePathGeometry(line.put())) && SUCCEEDED(line->Open(sink.put()))) {
        for (size_t i = 0; i < hours.size(); ++i) {
            const auto& h = hours[i];
            if (!std::isfinite(h.temp)) continue;
            const D2D1_POINT_2F p{pad + (static_cast<float>(i) + 0.5f) * colW, bandY(h.temp)};
            if (!figure) {
                sink->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW);
                figure = true;
            } else {
                sink->AddLine(p);
            }
        }
        if (figure) sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close())) figure = false;
    }
    if (figure) {
        brush->SetColor(Theme::ToColor(style_.glyphs.water, 0.9f));
        dc->DrawGeometry(line.get(), brush.get(), std::max(1.0f, 1.5f * s));
    }
    const float glyphSize = std::round(16.0f * s);
    for (size_t i = 0; i < hours.size(); ++i) {
        const auto& h = hours[i];
        const float x = pad + static_cast<float>(i) * colW;
        const float cx = x + colW / 2.0f;
        Text(dc, tinyFormat_.get(), i == 0 ? std::wstring(L"Now") : HourLabel(h.hour),
             D2D1::RectF(x - 4.0f * s, top, x + colW + 4.0f * s, top + 14.0f * s), i == 0 ? style_.text : style_.secondary,
             1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (h.code >= 0) {
            WeatherGlyphs::Draw(dc, WeatherGlyphs::FromWmo(h.code), h.isDay,
                                D2D1::RectF(cx - glyphSize / 2.0f, top + 17.0f * s, cx + glyphSize / 2.0f,
                                            top + 17.0f * s + glyphSize),
                                glyphs);
        }
        Text(dc, tinyFormat_.get(), Temp(h.temp, false),
             D2D1::RectF(x - 4.0f * s, top + 35.0f * s, x + colW + 4.0f * s, top + 50.0f * s), style_.text, 1.0f,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        if (std::isfinite(h.temp)) {
            brush->SetColor(Theme::ToColor(style_.glyphs.water));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, bandY(h.temp)), 1.8f * s, 1.8f * s), brush.get());
        }
        if (std::isfinite(h.precip) && h.precip >= 1.0) {
            Text(dc, microFormat_.get(), std::to_wstring(std::lround(h.precip)) + L"%",
                 D2D1::RectF(x - 6.0f * s, top + 77.0f * s, x + colW + 6.0f * s, top + 90.0f * s), style_.glyphs.water,
                 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    rule(kHourlyTop + kHourlyH + kRuleGap / 2.0f);

    // Daily rows.
    const std::vector<WeatherService::Day> days = W()->DailyWindow(kDays);
    double weekLo = 1e9, weekHi = -1e9;
    for (const auto& d : days) {
        if (std::isfinite(d.tmin)) weekLo = std::min(weekLo, d.tmin);
        if (std::isfinite(d.tmax)) weekHi = std::max(weekHi, d.tmax);
    }
    const float dailyTop = (kHourlyTop + kHourlyH + 2.0f * kRuleGap) * s;
    const float rowH = kRowH * s;
    const float trackL = pad + 158.0f * s, trackR = w - pad - 58.0f * s;
    const float trackH = std::max(2.0f, std::round(4.0f * s));
    winrt::com_ptr<ID2D1LinearGradientBrush> gradient;
    {
        const D2D1_GRADIENT_STOP stops[2] = {{0.0f, Theme::ToColor(style_.glyphs.water)},
                                             {1.0f, Theme::ToColor(style_.glyphs.sun)}};
        winrt::com_ptr<ID2D1GradientStopCollection> collection;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, collection.put()))) {
            dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(trackL, 0), D2D1::Point2F(trackR, 0)),
                collection.get(), gradient.put());
        }
    }
    const float dayGlyph = std::round(18.0f * s);
    for (size_t i = 0; i < days.size(); ++i) {
        const auto& d = days[i];
        const float y = dailyTop + static_cast<float>(i) * rowH;
        const float cy = y + rowH / 2.0f;
        std::wstring name = L"Today";
        if (i > 0) {
            wchar_t buf[40]{};
            GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, static_cast<LCTYPE>(LOCALE_SABBREVDAYNAME1 + d.weekday), buf, 40);
            name = buf;
        }
        Text(dc, bodyFormat_.get(), name, D2D1::RectF(pad, y, pad + 70.0f * s, y + rowH), style_.text, 1.0f,
             DWRITE_TEXT_ALIGNMENT_LEADING);
        if (d.code >= 0) {
            const float gx = pad + 74.0f * s;
            WeatherGlyphs::Draw(dc, WeatherGlyphs::FromWmo(d.code), true,
                                D2D1::RectF(gx, cy - dayGlyph / 2.0f, gx + dayGlyph, cy + dayGlyph / 2.0f), glyphs);
        }
        Text(dc, bodyFormat_.get(), Temp(d.tmin, false), D2D1::RectF(pad + 98.0f * s, y, pad + 148.0f * s, y + rowH),
             style_.secondary, 1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);
        Text(dc, bodyFormat_.get(), Temp(d.tmax, false), D2D1::RectF(w - pad - 52.0f * s, y, w - pad, y + rowH),
             style_.text, 1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);
        // Range bar: the week's span as the track, this day's low - high on it.
        const float r = trackH / 2.0f;
        D2D1_COLOR_F track = Theme::ToColor(style_.border);
        track.a = std::min(1.0f, track.a * 1.6f);  // a little stronger than the hairlines
        brush->SetColor(track);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(trackL, cy - r, trackR, cy + r), r, r), brush.get());
        if (std::isfinite(d.tmin) && std::isfinite(d.tmax) && weekHi > weekLo) {
            const float a = trackL + static_cast<float>((d.tmin - weekLo) / (weekHi - weekLo)) * (trackR - trackL);
            const float b = trackL + static_cast<float>((d.tmax - weekLo) / (weekHi - weekLo)) * (trackR - trackL);
            const D2D1_ROUNDED_RECT seg =
                D2D1::RoundedRect(D2D1::RectF(a, cy - r, std::max(b, a + trackH), cy + r), r, r);
            if (gradient) {
                dc->FillRoundedRectangle(seg, gradient.get());
            } else {
                brush->SetColor(Theme::ToColor(style_.glyphs.water));
                dc->FillRoundedRectangle(seg, brush.get());
            }
        }
    }
}

int WeatherPopup::HitAt(POINT pt, int* part) const {
    if (part) *part = 0;
    if (!Empty(refreshRect_) && PtInRect(&refreshRect_, pt)) return kHitRefresh;
    if (!Empty(creditRect_) && PtInRect(&creditRect_, pt)) return kHitCredit;
    if (!Empty(settingsRect_) && PtInRect(&settingsRect_, pt)) return kHitSettings;
    return -1;
}

void WeatherPopup::OnClick(int index, int, Button button) {
    if (button != Button::Left) return;
    switch (index) {
        case kHitRefresh:
            if (callbacks_.refresh) callbacks_.refresh();
            break;
        case kHitCredit:
            AllowSetForegroundWindow(ASFW_ANY);
            ShellExecuteW(nullptr, L"open", kCreditUrl, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case kHitSettings:
            if (callbacks_.openSettings) callbacks_.openSettings();
            break;
        default:
            break;
    }
}

bool WeatherPopup::OnMessage(UINT msg, WPARAM wp, LPARAM, LRESULT& result) {
    if (msg == WM_KEYDOWN && wp == VK_F5) {
        if (callbacks_.refresh) callbacks_.refresh();
        result = 0;
        return true;
    }
    return false;
}
