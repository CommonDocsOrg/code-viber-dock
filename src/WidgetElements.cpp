// The clock and weather widgets: one non-magnifying slot each, sized by its content and drawn into
// the slot's surface. Clicking opens their click popup (the month calendar / the forecast),
// hosted by the dock.

#include <algorithm>
#include <cmath>
#include <string>

#include "Clock.h"
#include "DockElement.h"
#include "Theme.h"
#include "Weather.h"
#include "WeatherGlyphs.h"

namespace {

constexpr float kWidgetGap = 10.0f;         // between two widgets (DockWindow's layout), DIPs
constexpr float kWeatherGlyph = 26.0f;      // weather widget glyph (24 with the date under the time)
constexpr float kWeatherTextGap = 3.0f;     // glyph -> temperature
constexpr float kWeatherStaleAlpha = 0.6f;  // stale / missing weather data

// A widget: one slot keyed "kind:<kind>" ("#n" for the second one of a kind and on), never
// magnified or labelled. It can be dragged to another place (unless the dock is locked); a right
// click opens the dock menu.
class WidgetElement : public DockElement {
public:
    WidgetElement(ElementHost& host, const DockItemEntry& entry, DockItem::Kind slotKind, const wchar_t* name,
                  size_t ordinal)
        : DockElement(host, entry), slotKind_(slotKind) {
        key_.id = std::wstring(L"kind:") + name + (ordinal ? L"#" + std::to_wstring(ordinal) : std::wstring());
    }

    void AppendSlots(SlotBuilder& out) override {
        DockItem& item = out.Add(key_);
        if (item.kind != slotKind_) {
            item.kind = slotKind_;
            item.dirty = true;
        }
        item.pinned = false;
    }

    // Half the gap to a neighbouring widget on each side counts as the widget.
    float HitSlack(const LayoutContext& ctx) const override { return std::round(kWidgetGap * ctx.scale / 2.0f); }
    RightClickAction OnRightClick(int) const override { return RightClickAction::DockMenu; }
    // The rest layout: with the cursor beyond the icons the bar grows away from it, so a widget at
    // either end of the dock does not move under it.
    float PopupAnchor(const DockItem& slot, const LayoutContext&) const override { return slot.x + slot.slotW / 2.0f; }

private:
    DockItem::Kind slotKind_;
    AppKey key_;
};

// clock: the time (clock.format, strftime, with clock.showSeconds) and optionally the date, from
// Clock. A one-shot timer aligned to the next minute (or second) boundary redraws it. A click
// opens the month calendar, or clock.clickTarget (clock.click).
class ClockElement final : public WidgetElement {
public:
    ClockElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal)
        : WidgetElement(host, entry, DockItem::Kind::Clock, L"clock", ordinal) {
        ClockElement::Configure();
    }

    void Configure() override {
        const Config& c = host_.Cfg();
        clock_.Configure(Clock::EffectiveFormat(c.clock.format, c.clock.showSeconds), c.clock.showDate);
        clock_.SetColors(c.theme.clockText, c.theme.clockDateText);
    }
    void Start() override {
        if (host_.Hwnd()) clock_.ScheduleNextTick(host_.Hwnd(), ElementHost::kClockTimerId);
    }
    bool Update() override { return clock_.UpdateText(); }
    // Main / cross: width / height on a horizontal dock. On a vertical dock the lines stack one
    // word per line (the time over its AM / PM, the date's words under it) and the block's height
    // runs along the bar.
    ElementSize Measure(const LayoutContext& ctx) override {
        const bool vertical = ctx.axis == DockAxis::Vertical;
        clock_.SetStacked(vertical);
        size_ = clock_.BuildLayout(ctx.dwrite, ctx.scale);
        const float w = static_cast<float>(size_.width), h = static_cast<float>(size_.height);
        return vertical ? ElementSize{h, w} : ElementSize{w, h};
    }
    HRESULT Render(Renderer& r, Layer& content, const LayoutContext&) override {
        return r.DrawLayer(content, size_.width, size_.height, [&](ID2D1DeviceContext* dc) { clock_.Draw(dc, 0.0f, 0.0f); });
    }
    bool OnEvent(DockEvent e) override {
        if (e != DockEvent::ClockTick && e != DockEvent::TimeChanged) return false;
        Start();  // the next boundary
        return clock_.UpdateText();
    }

    // The calendar popup (also when a click launches clock.clickTarget instead: DockCommand 114
    // opens it, and a press on the clock closes it).
    ClickPopup Popup() const override { return ClickPopup::Calendar; }
    void OnClick(int slot, bool pressClosedPopup) override {
        if (pressClosedPopup) return;
        const ClockConfig& c = host_.Cfg().clock;
        switch (c.click) {
            case ClockClick::Calendar: host_.ShowClickPopup(slot); break;
            case ClockClick::Launch: host_.OpenTarget(c.clickTarget); break;
            case ClockClick::None:
            default: break;
        }
    }

private:
    Clock clock_;
    D2D1_SIZE_U size_{};
};

// weather: a condition glyph and the temperature from the dock's WeatherService (Open-Meteo);
// stale / missing data dimmed; unconfigured, a "set location" glyph that opens Settings > Weather.
class WeatherElement final : public WidgetElement {
public:
    WeatherElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal)
        : WidgetElement(host, entry, DockItem::Kind::Weather, L"weather", ordinal) {}

    void Configure() override {
        format_ = nullptr;  // the clock's font size may have changed (date on / off)
        layout_ = nullptr;
    }
    bool OnEvent(DockEvent e) override { return e == DockEvent::WeatherData; }

    ElementSize Measure(const LayoutContext& ctx) override {
        // The temperature in the clock's time font; unconfigured: the "set location" glyph alone.
        const Config& c = host_.Cfg();
        const float scale = ctx.scale;
        const float glyph = std::round((DateShown() ? kWeatherGlyph - 2.0f : kWeatherGlyph) * scale);
        std::wstring text;
        if (c.weather.Configured()) {
            const WeatherService::Forecast& data = host_.Weather().Data();
            if (data.valid && std::isfinite(data.temp)) {
                long t = std::lround(data.temp);
                if (t == 0) t = 0;
                text = std::to_wstring(t) + L"°";
            } else {
                text = L"--°";
            }
        }
        IDWriteFactory* dw = ctx.dwrite;
        if (scale != textScale_) {
            format_ = nullptr;
            layout_ = nullptr;
        }
        if (!format_ && dw &&
            FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, (DateShown() ? 14.0f : 16.0f) * scale, L"",
                                        format_.put()))) {
            format_ = nullptr;
        }
        textScale_ = scale;
        if (text != text_) layout_ = nullptr;
        text_ = text;
        float textW = 0.0f, textH = 0.0f;
        if (!text.empty() && format_ && dw) {
            if (!layout_ && FAILED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format_.get(),
                                                        4096.0f, 4096.0f, layout_.put()))) {
                layout_ = nullptr;
            }
            DWRITE_TEXT_METRICS m{};
            if (layout_ && SUCCEEDED(layout_->GetMetrics(&m))) {
                textW = std::ceil(m.widthIncludingTrailingWhitespace);
                textH = std::ceil(m.height);
            }
        }
        const float gap = textW > 0.0f ? std::round(kWeatherTextGap * scale) : 0.0f;
        // Horizontal docks: the glyph left of the temperature; vertical docks: above it.
        vertical_ = ctx.axis == DockAxis::Vertical;
        if (vertical_) {
            size_ = D2D1::SizeU(static_cast<UINT32>(std::max(glyph, textW)), static_cast<UINT32>(glyph + gap + textH));
            return {static_cast<float>(size_.height), static_cast<float>(size_.width)};
        }
        size_ = D2D1::SizeU(static_cast<UINT32>(glyph + gap + textW), static_cast<UINT32>(std::max(glyph, textH)));
        return {static_cast<float>(size_.width), static_cast<float>(size_.height)};
    }

    HRESULT Render(Renderer& r, Layer& content, const LayoutContext& ctx) override {
        if (size_.width == 0 || size_.height == 0) return r.DrawLayer(content, 0, 0, {});
        const Config& c = host_.Cfg();
        const ThemeConfig& theme = c.theme;
        const float h = static_cast<float>(size_.height);
        const float glyph = std::round((DateShown() ? kWeatherGlyph - 2.0f : kWeatherGlyph) * ctx.scale);
        const WeatherService& weather = host_.Weather();
        const WeatherService::Forecast& data = weather.Data();
        const bool configured = c.weather.Configured();
        // Stale data (and none yet) are drawn dimmed.
        const bool dim = configured && (!data.valid || weather.State() != WeatherService::Status::Fresh);
        WeatherGlyphs::Colors colors;
        colors.ink = theme.clockText;
        colors.water = theme.accent;
        colors.opacity = dim ? kWeatherStaleAlpha : 1.0f;
        const WeatherGlyphs::Glyph g = !configured  ? WeatherGlyphs::Glyph::SetLocation
                                       : data.valid ? WeatherGlyphs::FromWmo(data.code)
                                                    : WeatherGlyphs::Glyph::Overcast;
        const bool isDay = !data.valid || data.isDay;
        const float scale = ctx.scale;
        if (vertical_) {
            const float w = static_cast<float>(size_.width);
            return r.DrawLayer(content, size_.width, size_.height, [&](ID2D1DeviceContext* dc) {
                const float left = std::round((w - glyph) / 2.0f);
                WeatherGlyphs::Draw(dc, g, isDay, D2D1::RectF(left, 0.0f, left + glyph, glyph), colors);
                if (!layout_) return;
                winrt::com_ptr<ID2D1SolidColorBrush> brush;
                if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(theme.clockText, colors.opacity), brush.put()))) return;
                DWRITE_TEXT_METRICS m{};
                layout_->GetMetrics(&m);
                const float y = glyph + std::round(kWeatherTextGap * scale);
                dc->DrawTextLayout(D2D1::Point2F(std::round((w - m.widthIncludingTrailingWhitespace) / 2.0f), y),
                                   layout_.get(), brush.get());
            });
        }
        return r.DrawLayer(content, size_.width, size_.height, [&](ID2D1DeviceContext* dc) {
            const float top = std::round((h - glyph) / 2.0f);
            WeatherGlyphs::Draw(dc, g, isDay, D2D1::RectF(0.0f, top, glyph, top + glyph), colors);
            if (!layout_) return;
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(theme.clockText, colors.opacity), brush.put()))) return;
            DWRITE_TEXT_METRICS m{};
            layout_->GetMetrics(&m);
            const float x = glyph + std::round(kWeatherTextGap * scale);
            dc->DrawTextLayout(D2D1::Point2F(x, std::round((h - m.height) / 2.0f)), layout_.get(), brush.get());
        });
    }

    ClickPopup Popup() const override { return ClickPopup::Weather; }
    void OnClick(int slot, bool pressClosedPopup) override {
        if (pressClosedPopup) return;
        if (host_.Cfg().weather.Configured()) {
            host_.ShowClickPopup(slot);
        } else {
            host_.OpenWeatherSettings();  // nothing to show yet: choose a place
        }
    }

private:
    // The clock shows its date under the time: the widget uses the smaller glyph and font.
    bool DateShown() const {
        const Config& c = host_.Cfg();
        return c.Main().Has(ItemKind::Clock) && c.clock.showDate;
    }

    bool vertical_ = false;  // measured for a vertical dock: the glyph above the temperature
    std::wstring text_;  // "12°" (empty: glyph only)
    float textScale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> format_;
    winrt::com_ptr<IDWriteTextLayout> layout_;
    D2D1_SIZE_U size_{};
};

}  // namespace

std::unique_ptr<DockElement> MakeClockElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal) {
    return std::make_unique<ClockElement>(host, entry, ordinal);
}

std::unique_ptr<DockElement> MakeWeatherElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal) {
    return std::make_unique<WeatherElement>(host, entry, ordinal);
}
