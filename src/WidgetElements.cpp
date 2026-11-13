// The clock and weather widgets: one non-magnifying slot each, sized by its content and drawn into
// the slot's surface. Clicking opens their click popup (the month calendar / the forecast),
// hosted by the dock.

#include <algorithm>
#include <cmath>
#include <string>

#include "Clock.h"
#include "DockElement.h"
#include "JsonUtil.h"
#include "Theme.h"
#include "Weather.h"
#include "WeatherGlyphs.h"
#include "WidgetInstance.h"

namespace {

constexpr float kWidgetGap = 10.0f;         // between two widgets (DockWindow's layout), DIPs
constexpr float kWeatherGlyph = 26.0f;      // weather widget glyph (24 with the date under the time)
constexpr float kWeatherTextGap = 3.0f;     // glyph -> temperature
constexpr float kWeatherStaleAlpha = 0.6f;  // stale / missing weather data

// A widget: one slot keyed "kind:<kind>" ("#n" for the second one of a kind and on), never
// magnified or labelled. It can be dragged to another place (unless the dock is locked); a right
// click opens its own item menu (round 10 phase 10A; App::OnDockMessage; it was the dock menu).
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

// The dock has a clock whose time is drawn smaller over a second line (its date or its label):
// the weather element and script widgets (dock.clockDate) use the smaller sizes too.
bool ClockSecondLine(const ElementHost& host) {
    for (const DockItemEntry& e : host.DockCfg().items) {
        if (e.Kind() != ItemKind::Clock) continue;
        const ClockConfig c = ClockOf(host.Cfg(), e);
        if (c.showDate || !c.label.empty()) return true;
    }
    return false;
}

// clock: the time (its item's format, strftime, with showSeconds; round 5: in its time zone) and
// optionally the date and a label, from Clock. A one-shot timer aligned to the next minute (or
// second) boundary redraws it. A click opens the month calendar, or the item's clickTarget.
class ClockElement final : public WidgetElement {
public:
    ClockElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal)
        : WidgetElement(host, entry, DockItem::Kind::Clock, L"clock", ordinal) {
        ClockElement::Configure();
    }

    void Configure() override {
        cfg_ = ClockOf(host_.Cfg(), Entry());
        const ClockConfig& c = cfg_;
        clock_.Configure(Clock::EffectiveFormat(c.format, c.showSeconds), c.showDate);
        clock_.SetTimeZone(c.timeZone);
        clock_.SetLabel(c.label);
        clock_.SetColors(host_.DockTheme().clockText, host_.DockTheme().clockDateText);
        // A custom face (clock.face, phase 14); "digital" or a face that cannot be read: the
        // classic rendering.
        std::wstring error;
        std::shared_ptr<const ClockFace> face = ClockFaces::Load(c.face, error);
        if (!error.empty()) OutputDebugStringW((L"WinDock: clock face: " + error + L"\n").c_str());
        clock_.SetFace(face, face ? ClockFaces::Resolve(*face, host_.DockTheme()) : ClockFaceColors{}, c.format,
                       c.showSeconds);
        // Round 20 phase 20A: the item's look; the fit (Measure) starts over from its scale.
        fitRoom_ = -1.0f;
        drawnScale_ = c.scale;
        clock_.SetLook(LookOf(drawnScale_));
        dot_ = WantDot();
    }
    bool ClockOffset(int& minutes) const override {
        minutes = clock_.OffsetMinutes();
        return true;
    }
    bool ClockLook(int& scalePercent, D2D1_SIZE_U& block) const override {
        scalePercent = static_cast<int>(std::lround(drawnScale_ * 100.0f));
        block = text_;
        return true;
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
        // Round 20 phase 20A: the look's scale is capped so the block fits across the bar (the
        // widget row's room; a widget taller than the row would stick out of the bar, which does
        // not grow for widgets). Only the part above 1 is given up: at scale 1 the clock fits as
        // it always did (or did not, on a tiny dock). The fit starts over from the item's scale
        // when the room changes (a thicker dock, another icon size) and after Configure; a block
        // that gets narrower on a vertical dock keeps the smaller scale until then.
        const float room = ctx.rowRoom > 0.0f ? ctx.rowRoom : ctx.iconPx;
        if (room != fitRoom_) {
            fitRoom_ = room;
            drawnScale_ = cfg_.scale;
            clock_.SetLook(LookOf(drawnScale_));
        }
        text_ = clock_.BuildLayout(ctx.dwrite, ctx.scale);
        for (int pass = 0; pass < 2 && room > 0.0f && drawnScale_ > 1.0f; ++pass) {
            const float cross = static_cast<float>(vertical ? text_.width : text_.height);
            if (cross <= room + 0.5f) break;
            const float fit = std::max(1.0f, drawnScale_ * room / cross);
            if (fit >= drawnScale_ - 0.001f) break;
            drawnScale_ = fit;
            clock_.SetLook(LookOf(drawnScale_));
            text_ = clock_.BuildLayout(ctx.dwrite, ctx.scale);
        }
        size_ = text_;
        vertical_ = vertical;
        // With events today (calendars.showDots): a dot after the last line (under it when stacked).
        dotSpace_ = dot_ && text_.width > 0 ? static_cast<UINT32>(std::lround((kDotGap + kDotSize) * ctx.scale)) : 0;
        if (vertical) {
            size_.height += dotSpace_;
        } else {
            size_.width += dotSpace_;
        }
        const float w = static_cast<float>(size_.width), h = static_cast<float>(size_.height);
        return vertical ? ElementSize{h, w} : ElementSize{w, h};
    }
    HRESULT Render(Renderer& r, Layer& content, const LayoutContext& ctx) override {
        return r.DrawLayer(content, size_.width, size_.height, [&](ID2D1DeviceContext* dc) {
            clock_.Draw(dc, 0.0f, 0.0f);
            if (!dotSpace_) return;
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(host_.DockTheme().accent), brush.put()))) return;
            const float radius = kDotSize * ctx.scale / 2.0f, gap = kDotGap * ctx.scale;
            const D2D1_POINT_2F c = vertical_ ? D2D1::Point2F(static_cast<float>(text_.width) / 2.0f, static_cast<float>(text_.height) + gap + radius)
                                              : D2D1::Point2F(static_cast<float>(text_.width) + gap + radius, clock_.LastLineCentre());
            dc->FillEllipse(D2D1::Ellipse(c, radius, radius), brush.get());
        });
    }
    bool OnEvent(DockEvent e) override {
        if (e == DockEvent::Calendar) {
            const bool want = WantDot();
            if (want == dot_) return false;
            dot_ = want;
            return true;
        }
        if (e != DockEvent::ClockTick && e != DockEvent::TimeChanged) return false;
        Start();  // the next boundary
        const bool want = WantDot();  // a new day may have events (or none)
        const bool dotChanged = want != dot_;
        dot_ = want;
        return clock_.UpdateText() || dotChanged;
    }

    // The calendar popup (also when a click launches clock.clickTarget instead: DockCommand 114
    // opens it, and a press on the clock closes it).
    ClickPopup Popup() const override { return ClickPopup::Calendar; }
    void OnClick(int slot, bool pressClosedPopup) override {
        if (pressClosedPopup) return;
        const ClockConfig c = cfg_;  // copied: a launch may change the config
        switch (c.click) {
            case ClockClick::Calendar: host_.ShowClickPopup(slot); break;
            case ClockClick::Launch: host_.OpenTarget(c.clickTarget); break;
            case ClockClick::None:
            default: break;
        }
    }

private:
    static constexpr float kDotSize = 5.0f;  // DIPs
    static constexpr float kDotGap = 4.0f;
    bool WantDot() const { return host_.Cfg().calendars.showDots && host_.CalendarEventsToday() > 0; }
    // Round 20 phase 20A: the item's look at scale `scale` (ClockConfig -> Clock::Look).
    Clock::Look LookOf(float scale) const {
        Clock::Look k;
        k.scale = scale;
        k.font = cfg_.font;
        k.weight = ClockWeightOf(cfg_.weight);
        ParseArgbColor(cfg_.timeColor, k.timeColor);  // "" (not set): stays 0
        ParseArgbColor(cfg_.dateColor, k.dateColor);
        return k;
    }

    ClockConfig cfg_;  // ClockOf the item (Configure)
    Clock clock_;
    D2D1_SIZE_U text_{};  // the text block
    D2D1_SIZE_U size_{};  // the slot's surface (the text block and the dot)
    UINT32 dotSpace_ = 0;
    bool dot_ = false;
    bool vertical_ = false;
    float fitRoom_ = -1.0f;    // round 20 phase 20A: the room the fit was made for (px; -1 none yet)
    float drawnScale_ = 1.0f;  // the scale drawn: cfg_.scale, or less to fit (QueryState 340)
};

// weather: a condition glyph and the temperature from the WeatherService of its item's place
// (Open-Meteo; round 5: one service per place), the item's label under the temperature; stale /
// missing data dimmed; unconfigured, a "set location" glyph that opens the item's settings.
class WeatherElement final : public WidgetElement {
public:
    WeatherElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal)
        : WidgetElement(host, entry, DockItem::Kind::Weather, L"weather", ordinal) {
        WeatherElement::Configure();
    }

    void Configure() override {
        cfg_ = WeatherOf(host_.Cfg(), Entry());
        small_ = ClockSecondLine(host_) || !cfg_.label.empty();
        format_ = nullptr;  // the clock's font size may have changed (date on / off)
        layout_ = nullptr;
        labelFormat_ = nullptr;
        labelLayout_ = nullptr;
    }
    bool OnEvent(DockEvent e) override { return e == DockEvent::WeatherData; }
    bool WeatherPlace(WeatherConfig& place) const override {
        place = cfg_;
        return true;
    }

    ElementSize Measure(const LayoutContext& ctx) override {
        // The temperature in the clock's time font, the label under it in the date's; unconfigured:
        // the "set location" glyph alone.
        const float scale = ctx.scale;
        const float glyph = std::round((small_ ? kWeatherGlyph - 2.0f : kWeatherGlyph) * scale);
        std::wstring text;
        std::wstring label;
        if (cfg_.Configured()) {
            const WeatherService* service = host_.WeatherFor(cfg_);
            const WeatherService::Forecast* data = service ? &service->Data() : nullptr;
            if (data && data->valid && std::isfinite(data->temp)) {
                long t = std::lround(data->temp);
                if (t == 0) t = 0;
                text = std::to_wstring(t) + L"°";
            } else {
                text = L"--°";
            }
            label = cfg_.label;
        }
        IDWriteFactory* dw = ctx.dwrite;
        if (scale != textScale_) {
            format_ = nullptr;
            layout_ = nullptr;
            labelFormat_ = nullptr;
            labelLayout_ = nullptr;
        }
        if (!format_ && dw &&
            FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, (small_ ? 14.0f : 16.0f) * scale, L"",
                                        format_.put()))) {
            format_ = nullptr;
        }
        if (!label.empty() && !labelFormat_ && dw &&
            FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, 11.0f * scale, L"", labelFormat_.put()))) {
            labelFormat_ = nullptr;
        }
        textScale_ = scale;
        if (text != text_) layout_ = nullptr;
        if (label != label_) labelLayout_ = nullptr;
        text_ = text;
        label_ = label;
        auto measure = [&](const std::wstring& s, IDWriteTextFormat* format, winrt::com_ptr<IDWriteTextLayout>& layout,
                           float& w, float& h) {
            w = h = 0.0f;
            if (s.empty() || !format || !dw) return;
            if (!layout && FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), format, 4096.0f, 4096.0f,
                                                       layout.put()))) {
                layout = nullptr;
            }
            DWRITE_TEXT_METRICS m{};
            if (layout && SUCCEEDED(layout->GetMetrics(&m))) {
                w = std::ceil(m.widthIncludingTrailingWhitespace);
                h = std::ceil(m.height);
            }
        };
        measure(text, format_.get(), layout_, tempW_, tempH_);
        measure(label, labelFormat_.get(), labelLayout_, labelW_, labelH_);
        const float textW = std::max(tempW_, labelW_), textH = tempH_ + labelH_;
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
        const ThemeConfig& theme = host_.DockTheme();
        const float h = static_cast<float>(size_.height);
        const float glyph = std::round((small_ ? kWeatherGlyph - 2.0f : kWeatherGlyph) * ctx.scale);
        const WeatherService* weather = host_.WeatherFor(cfg_);
        static const WeatherService::Forecast kNone;
        const WeatherService::Forecast& data = weather ? weather->Data() : kNone;
        const bool configured = cfg_.Configured();
        // Stale data (and none yet) are drawn dimmed.
        const bool dim = configured && (!data.valid || !weather || weather->State() != WeatherService::Status::Fresh);
        WeatherGlyphs::Colors colors;
        colors.ink = theme.clockText;
        colors.water = theme.accent;
        colors.opacity = dim ? kWeatherStaleAlpha : 1.0f;
        const WeatherGlyphs::Glyph g = !configured  ? WeatherGlyphs::Glyph::SetLocation
                                       : data.valid ? WeatherGlyphs::FromWmo(data.code)
                                                    : WeatherGlyphs::Glyph::Overcast;
        const bool isDay = !data.valid || data.isDay;
        const float scale = ctx.scale;
        const float gap = std::round(kWeatherTextGap * scale);
        // The temperature (dimmed with the glyph) and the label under it (the date's colour).
        auto texts = [&](ID2D1DeviceContext* dc, float x, float y, bool centre, float w) {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(theme.clockText, colors.opacity), brush.put()))) return;
            if (layout_) {
                dc->DrawTextLayout(D2D1::Point2F(centre ? std::round((w - tempW_) / 2.0f) : x, y), layout_.get(), brush.get());
            }
            if (labelLayout_) {
                brush->SetColor(Theme::ToColor(theme.clockDateText));
                dc->DrawTextLayout(D2D1::Point2F(centre ? std::round((w - labelW_) / 2.0f) : x, y + tempH_),
                                   labelLayout_.get(), brush.get());
            }
        };
        if (vertical_) {
            const float w = static_cast<float>(size_.width);
            return r.DrawLayer(content, size_.width, size_.height, [&](ID2D1DeviceContext* dc) {
                const float left = std::round((w - glyph) / 2.0f);
                WeatherGlyphs::Draw(dc, g, isDay, D2D1::RectF(left, 0.0f, left + glyph, glyph), colors);
                texts(dc, 0.0f, glyph + gap, true, w);
            });
        }
        return r.DrawLayer(content, size_.width, size_.height, [&](ID2D1DeviceContext* dc) {
            const float top = std::round((h - glyph) / 2.0f);
            WeatherGlyphs::Draw(dc, g, isDay, D2D1::RectF(0.0f, top, glyph, top + glyph), colors);
            texts(dc, glyph + gap, std::round((h - tempH_ - labelH_) / 2.0f), false, 0.0f);
        });
    }

    ClickPopup Popup() const override { return ClickPopup::Weather; }
    void OnClick(int slot, bool pressClosedPopup) override {
        if (pressClosedPopup) return;
        if (cfg_.Configured()) {
            host_.ShowClickPopup(slot);
        } else {
            host_.OpenItemSettings(slot);  // nothing to show yet: choose a place
        }
    }

private:
    WeatherConfig cfg_;      // WeatherOf the item (Configure)
    bool small_ = false;     // a clock draws a second line, or a label: the smaller glyph and font
    bool vertical_ = false;  // measured for a vertical dock: the glyph above the temperature
    std::wstring text_;      // "12°" (empty: glyph only)
    std::wstring label_;     // the item's label (empty: none)
    float textScale_ = 0.0f;
    float tempW_ = 0.0f, tempH_ = 0.0f, labelW_ = 0.0f, labelH_ = 0.0f;  // px
    winrt::com_ptr<IDWriteTextFormat> format_;
    winrt::com_ptr<IDWriteTextLayout> layout_;
    winrt::com_ptr<IDWriteTextFormat> labelFormat_;
    winrt::com_ptr<IDWriteTextLayout> labelLayout_;
    D2D1_SIZE_U size_{};
};

// widget: a script widget (docs/widgets.md). The instance (its script runtime, timers, settings,
// log) belongs to the dock and outlives this element, which is rebuilt with every item-list
// change; the element forwards layout, drawing and input to it. Its slot key is
// "kind:widget:<id>" (+ "#n" for the n-th item with the same id).
class ScriptWidgetElement final : public WidgetElement {
public:
    ScriptWidgetElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal)
        : WidgetElement(host, entry, DockItem::Kind::Widget, (L"widget:" + entry.id).c_str(), ordinal),
          widget_(host.AcquireWidget(entry, ordinal)) {
        ScriptWidgetElement::Configure();
    }

    void Configure() override {
        if (widget_) widget_->SetDock(WidgetDockInfo(host_));
    }
    ElementSize Measure(const LayoutContext& ctx) override { return widget_ ? widget_->Measure(ctx) : ElementSize{}; }
    HRESULT Render(Renderer& r, Layer& content, const LayoutContext& ctx) override {
        if (!widget_) return r.DrawLayer(content, 0, 0, {});
        widget_->SetMagnified(host_.Magnifying());
        return widget_->Render(r, content, ctx);
    }
    bool OnEvent(DockEvent e) override {
        if (!widget_) return false;
        if (e == DockEvent::TimeChanged) {
            widget_->TimeChanged();  // asks for its own redraw (posted)
            return false;
        }
        return e == DockEvent::WidgetRedraw && widget_->TakeChanged();
    }
    WidgetInstance* Widget() const override { return widget_; }

    // A press: onPress; on a widget whose popup is open the dock closes it and the click that
    // follows says so (closedPopup), so a click toggles the popup.
    bool OnPress(int slot) override {
        if (!widget_) return false;
        const bool open = widget_->PopupShownNow();
        float x = 0, y = 0;
        if (Local(slot, x, y)) widget_->Press(x, y);
        return open;
    }
    void OnClick(int slot, bool pressClosedPopup) override {
        float x = 0, y = 0;
        if (widget_ && Local(slot, x, y)) widget_->Click(x, y, pressClosedPopup);
    }
    void OnMiddleClick(int slot) override {
        float x = 0, y = 0;
        if (widget_ && Local(slot, x, y)) widget_->MiddleClick(x, y);
    }
    bool OnWheel(int slot, int delta, bool horizontal) override {
        float x = 0, y = 0;
        return widget_ && Local(slot, x, y) && widget_->Wheel(x, y, delta, horizontal);
    }
    void OnHover(int slot, bool inside) override {
        float x = 0, y = 0;
        if (!widget_) return;
        Local(slot, x, y);
        widget_->SetMagnified(host_.Magnifying());
        widget_->Hover(inside, x, y);
    }
    // Round 23 phase 23A: the press onPress claimed (WidgetInstance::Press returned true).
    bool PressClaimed(int) const override { return widget_ && widget_->Capturing(); }
    void OnDrag(int slot) override {
        float x = 0, y = 0;
        if (widget_ && Local(slot, x, y)) widget_->Drag(x, y);
    }
    void OnRelease(int slot, bool cancelled) override {
        float x = 0, y = 0;
        if (widget_ && Local(slot, x, y)) widget_->Release(x, y, cancelled);
    }

private:
    // The pointer (of the message being handled) in DIPs from the box's top-left.
    bool Local(int slot, float& x, float& y) const {
        const D2D1_RECT_F box = host_.SlotScreenBox(slot);
        const DWORD pos = GetMessagePos();
        const float s = std::max(0.01f, Scale());
        x = (static_cast<float>(static_cast<short>(LOWORD(pos))) - box.left) / s;
        y = (static_cast<float>(static_cast<short>(HIWORD(pos))) - box.top) / s;
        return true;
    }
    float Scale() const {
        const UINT dpi = GetDpiForWindow(host_.Hwnd());
        return dpi ? static_cast<float>(dpi) / 96.0f : 1.0f;
    }

    WidgetInstance* widget_;
};

}  // namespace

ScriptDockInfo WidgetDockInfo(const ElementHost& host) {
    const DockConfig& d = host.DockCfg();
    const ThemeConfig& t = host.DockTheme();
    ScriptDockInfo info;
    info.edge = JsonUtil::WideToUtf8(ToString(d.edge));
    info.axis = IsVertical(d.edge) ? "vertical" : "horizontal";
    info.dockId = JsonUtil::WideToUtf8(d.id);
    info.locked = d.locked;
    info.reduceMotion = host.ReducedMotion();
    info.metric = host.Cfg().weather.units == Units::Metric;
    info.clockDate = ClockSecondLine(host);  // the weather element's smaller size
    info.text = t.clockText;
    info.dateText = t.clockDateText;
    info.accent = t.accent;
    info.background = t.background;
    info.labelBackground = t.labelBackground;
    info.labelText = t.labelText;
    info.separator = t.separator;
    info.border = t.barBorder;
    info.iconSize = static_cast<float>(d.iconSize);
    return info;
}

std::unique_ptr<DockElement> MakeScriptWidgetElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal) {
    return std::make_unique<ScriptWidgetElement>(host, entry, ordinal);
}

std::unique_ptr<DockElement> MakeClockElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal) {
    return std::make_unique<ClockElement>(host, entry, ordinal);
}

std::unique_ptr<DockElement> MakeWeatherElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal) {
    return std::make_unique<WeatherElement>(host, entry, ordinal);
}
