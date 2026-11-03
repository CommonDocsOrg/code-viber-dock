#include "CalendarPopup.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace {

// Layout in DIPs.
constexpr float kPad = 14.0f;
constexpr float kCell = 36.0f;        // grid cell (square)
constexpr float kRowH = 32.0f;        // grid row
constexpr int kRows = 6;
constexpr float kTitleTop = 12.0f, kTitleH = 22.0f;
constexpr float kMonthTop = 40.0f, kMonthH = 28.0f;
constexpr float kArrow = 28.0f;
constexpr float kHeadTop = 74.0f, kHeadH = 20.0f;
constexpr float kGridTop = 98.0f;
constexpr float kBottomPad = 10.0f;
constexpr float kFooterGap = 6.0f;    // grid -> footer button
constexpr float kFooterH = 30.0f;
constexpr float kTodayDiameter = 28.0f;
constexpr float kRadius = 6.0f;
// Events (phase 13).
constexpr float kDot = 4.0f;          // dot diameter
constexpr float kDotStep = 6.0f;      // centre to centre
constexpr float kDotY = 27.0f;        // dot centres, from the cell's top
constexpr float kListGap = 6.0f;      // grid -> the date header
constexpr float kDateH = 24.0f;
constexpr float kEventRowH = 30.0f;
constexpr float kBarIconDip = 18.0f;  // the colour bar's square (the list's icon)
constexpr float kBarW = 4.0f;
constexpr float kEmptyH = 28.0f;
constexpr float kDetailGap = 4.0f;
constexpr float kDetailH = 38.0f;     // two lines
constexpr size_t kMaxRows = 6;
constexpr float kMonitorMargin = 8.0f;

LONG Px(float dip, float scale) { return static_cast<LONG>(std::lround(dip * scale)); }

D2D1_RECT_F ToRectF(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                       static_cast<float>(r.bottom));
}

int DaysInMonth(int y, int m) { return Ics::DaysInMonth(y, m); }

// 0 = Monday ... 6 = Sunday (Zeller-free: Sakamoto's method gives 0 = Sunday).
int WeekdayMonday0(int y, int m, int d) {
    static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    const int sunday0 = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return (sunday0 + 6) % 7;
}

std::wstring FormatDate(DWORD flags, const SYSTEMTIME& st) {
    wchar_t buf[128]{};
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, flags, &st, nullptr, buf, 128, nullptr)) return {};
    return buf;
}

SYSTEMTIME DayTime(int64_t day) {
    int y = 0, m = 0, d = 0;
    Ics::CivilFromDays(day, y, m, d);
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(std::clamp(y, 1601, 30827));
    st.wMonth = static_cast<WORD>(m);
    st.wDay = static_cast<WORD>(d);
    st.wDayOfWeek = static_cast<WORD>((Ics::WeekdayOf(day) + 1) % 7);
    return st;
}

// Black or white, whichever reads better on `argb`.
uint32_t OnColor(uint32_t argb) {
    const float r = static_cast<float>((argb >> 16) & 0xFF), g = static_cast<float>((argb >> 8) & 0xFF),
                b = static_cast<float>(argb & 0xFF);
    return 0.299f * r + 0.587f * g + 0.114f * b > 150.0f ? 0xFF000000u : 0xFFFFFFFFu;
}

bool IsWebUrl(const std::wstring& url) {
    return _wcsnicmp(url.c_str(), L"https://", 8) == 0 || _wcsnicmp(url.c_str(), L"http://", 7) == 0;
}

std::wstring FirstLine(const std::wstring& s) {
    const size_t cut = s.find_first_of(L"\r\n");
    return cut == std::wstring::npos ? s : s.substr(0, cut);
}

}  // namespace

void CalendarPopup::Init(HINSTANCE instance, Renderer* renderer, Callbacks callbacks) {
    PopupWindow::Init(instance, renderer);
    callbacks_ = std::move(callbacks);
    SetClosedCallback([this] {
        if (callbacks_.closed) callbacks_.closed();
    });
}

void CalendarPopup::OnHidden() {
    view_.Detach();
    view_.SetItems({});
    rows_.clear();
    service_ = nullptr;
}

void CalendarPopup::OnDestroying() {
    view_.Detach();
    view_.SetItems({});
    viewReady_ = false;
    rows_.clear();
    bars_.clear();
    service_ = nullptr;
    titleFormat_ = monthFormat_ = dayFormat_ = headFormat_ = noteFormat_ = nullptr;
    formatScale_ = 0.0f;
}

bool CalendarPopup::Show(const Style& style, const PopupAnchor& anchor, CalendarService* service, int64_t day) {
    style_ = style;
    frame_ = Frame{};
    frame_.scale = style.scale;
    frame_.acrylic = style.acrylic;
    frame_.cornerRadiusDip = style.cornerRadiusDip;
    frame_.monitor = style.monitor;
    anchor_ = anchor;
    service_ = service;
    GetLocalTime(&today_);
    const int64_t today = Ics::DaysFromCivil(today_.wYear, today_.wMonth, today_.wDay);
    if (day == INT64_MIN) day = today;
    int y = 0, m = 0, d = 0;
    Ics::CivilFromDays(day, y, m, d);
    year_ = std::clamp(y, 1601, 9999);
    month_ = m;
    selected_ = day;
    DWORD first = 0;  // 0 = Monday ... 6 = Sunday
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER,
                        reinterpret_cast<LPWSTR>(&first), sizeof(first) / sizeof(wchar_t))) {
        firstDay_ = static_cast<int>(first % 7);
    }
    if (Events()) service_->EnsureDays(GridFirst(), GridFirst() + kRows * 7 - 1);
    Fill();
    lastDetail_ = -1;
    if (!ShowAt(frame_, anchor_)) return false;
    view_.ScrollTo(0.0f, false);
    return true;
}

void CalendarPopup::Update() {
    if (!Visible()) return;  // hidden or fading out: ShowAt would show it again
    if (Events()) service_->EnsureDays(GridFirst(), GridFirst() + kRows * 7 - 1);
    const LONG scroll = view_.ScrollOffset();
    Fill();
    Relayout();
    view_.ScrollTo(static_cast<float>(scroll), false);
}

int CalendarPopup::MonthOffset() const {
    if (!OnScreen()) return 0;
    return (year_ - today_.wYear) * 12 + (month_ - today_.wMonth);
}

int64_t CalendarPopup::GridFirst() const {
    const int lead = (WeekdayMonday0(year_, month_, 1) - firstDay_ + 7) % 7;
    return Ics::DaysFromCivil(year_, month_, 1) - lead;
}

bool CalendarPopup::SelectDay(int64_t day) {
    if (!Visible()) return false;
    int y = 0, m = 0, d = 0;
    Ics::CivilFromDays(day, y, m, d);
    if (y < 1601 || y > 9999) return false;
    year_ = y;
    month_ = m;
    selected_ = day;
    if (Events()) service_->EnsureDays(GridFirst(), GridFirst() + kRows * 7 - 1);
    Fill();
    lastDetail_ = -1;
    Relayout();
    view_.ScrollTo(0.0f, false);
    return true;
}

void CalendarPopup::Shift(int months) {
    if (months == 0) {
        GetLocalTime(&today_);
        year_ = today_.wYear;
        month_ = today_.wMonth;
        selected_ = Ics::DaysFromCivil(today_.wYear, today_.wMonth, today_.wDay);
    } else {
        int index = year_ * 12 + (month_ - 1) + months;
        index = std::clamp(index, 1601 * 12, 9999 * 12 + 11);  // SYSTEMTIME's range
        year_ = index / 12;
        month_ = index % 12 + 1;
    }
    if (!Events()) {
        if (SUCCEEDED(Redraw())) Commit();  // fixed size: no re-layout
        return;
    }
    // The grid's days must be expanded (the service may re-expand: the rows are refilled).
    service_->EnsureDays(GridFirst(), GridFirst() + kRows * 7 - 1);
    Fill();
    Relayout();
}

void CalendarPopup::Fill() {
    rows_.clear();
    std::vector<GridItem> items;
    if (Events() && selected_ != INT64_MIN) {
        rows_ = service_->On(selected_);
        const auto& all = service_->Items();
        for (size_t index : rows_) {
            const CalendarService::Item& it = all[index];
            const Ics::Event& ev = service_->EventOf(it);
            GridItem g;
            g.label = ev.summary.empty() ? std::wstring(L"(No title)") : FirstLine(ev.summary);
            const Ics::Occurrence& o = it.occ;
            g.secondary = o.allDay || o.firstDay < selected_ ? (o.allDay || o.lastDay > selected_ ? std::wstring(L"All day")
                                                                                                  : L"Until " + service_->TimeText(o.end))
                                                             : service_->TimeText(o.start);
            g.id = index;
            g.flags = service_->ColorOf(it);
            items.push_back(std::move(g));
        }
    }
    view_.SetItems(std::move(items));
}

void CalendarPopup::Relayout() {
    if (!Visible()) return;
    ShowAt(frame_, anchor_);  // measures again (the list's height) and keeps the anchor
}

ItemGridView::Style CalendarPopup::ViewStyle() const {
    ItemGridView::Style v;
    v.scale = style_.scale;
    v.text = style_.text;
    v.secondary = style_.secondary;
    v.hotBackground = style_.hotBackground;
    v.hotBorder = style_.hotBorder;
    v.accent = style_.accent;
    v.scrollbar = (style_.text & 0x00FFFFFF) | 0x80000000;
    v.rowH = kEventRowH;
    v.listIconDip = kBarIconDip;
    v.fontDip = 12.0f;
    v.reduceMotion = style_.reduceMotion;
    return v;
}

ID2D1Bitmap* CalendarPopup::BarBitmap(uint32_t color) {
    auto it = bars_.find(color);
    if (it != bars_.end()) return it->second.get();
    ID2D1DeviceContext* dc = Render() ? Render()->ResourceContext() : nullptr;
    if (!dc) return nullptr;
    const float s = style_.scale;
    const UINT px = static_cast<UINT>(std::max<LONG>(2, Px(kBarIconDip, s)));
    const UINT bw = static_cast<UINT>(std::max<LONG>(2, Px(kBarW, s)));
    const UINT x0 = (px - bw) / 2;
    const uint32_t a = color >> 24;
    auto pre = [&](uint32_t c) { return (c * a + 127) / 255; };
    const uint32_t pixel = (a << 24) | (pre((color >> 16) & 0xFF) << 16) | (pre((color >> 8) & 0xFF) << 8) | pre(color & 0xFF);
    std::vector<uint32_t> pixels(static_cast<size_t>(px) * px, 0u);
    for (UINT y = 0; y < px; ++y) {
        for (UINT x = x0; x < x0 + bw && x < px; ++x) pixels[static_cast<size_t>(y) * px + x] = pixel;
    }
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    winrt::com_ptr<ID2D1Bitmap1> bitmap;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(px, px), pixels.data(), px * 4, props, bitmap.put()))) return nullptr;
    winrt::com_ptr<ID2D1Bitmap> plain = bitmap.as<ID2D1Bitmap>();
    if (bars_.size() > 64) bars_.clear();
    return bars_.emplace(color, std::move(plain)).first->second.get();
}

void CalendarPopup::EnsureFormats() {
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    const float s = style_.scale;
    if (!dw || (titleFormat_ && formatScale_ == s)) return;
    titleFormat_ = monthFormat_ = dayFormat_ = headFormat_ = noteFormat_ = nullptr;
    auto make = [&](winrt::com_ptr<IDWriteTextFormat>& f, DWRITE_FONT_WEIGHT weight, float size,
                    DWRITE_TEXT_ALIGNMENT align) {
        if (FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size * s, L"", f.put()))) {
            f = nullptr;
            return;
        }
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    };
    make(titleFormat_, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
    make(monthFormat_, DWRITE_FONT_WEIGHT_NORMAL, 13.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
    make(dayFormat_, DWRITE_FONT_WEIGHT_NORMAL, 13.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    make(headFormat_, DWRITE_FONT_WEIGHT_NORMAL, 11.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    make(noteFormat_, DWRITE_FONT_WEIGHT_NORMAL, 12.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
    formatScale_ = s;
}

SIZE CalendarPopup::Measure() {
    EnsureFormats();
    const float s = style_.scale;
    const LONG pad = Px(kPad, s);
    const LONG w = 2 * pad + 7 * Px(kCell, s);
    const LONG arrow = Px(kArrow, s);
    const LONG top = Px(kMonthTop, s);
    nextRect_ = RECT{w - pad - arrow, top, w - pad, top + arrow};
    prevRect_ = RECT{nextRect_.left - arrow - Px(4.0f, s), top, nextRect_.left - Px(4.0f, s), top + arrow};
    monthRect_ = RECT{pad - Px(6.0f, s), top, prevRect_.left - Px(8.0f, s), top + Px(kMonthH, s)};
    dateRect_ = viewRect_ = emptyRect_ = detailRect_ = RECT{};
    footerRect_ = RECT{};
    if (!Events()) {  // the month grid alone (as before phase 13)
        if (view_.Attached()) view_.Detach();
        LONG h = Px(kGridTop + kRows * kRowH + kBottomPad, s);
        if (!style_.footer.empty()) {
            const LONG footerTop = Px(kGridTop + kRows * kRowH + kFooterGap, s);
            footerRect_ = RECT{pad, footerTop, w - pad, footerTop + Px(kFooterH, s)};
            h = footerRect_.bottom + Px(kBottomPad, s);
        }
        return SIZE{w, h};
    }
    if (!viewReady_ && Window()) {
        ItemGridView::Callbacks cb;
        cb.resolve = [this](size_t, GridItem& item) {
            if (ID2D1Bitmap* b = BarBitmap(item.flags)) item.icon.copy_from(b);
        };
        cb.activate = [this](size_t i, ItemGridView::Button b) {
            if (b != ItemGridView::Button::Left || !Events() || i >= rows_.size()) return;
            const auto& all = service_->Items();
            if (rows_[i] >= all.size()) return;
            const std::wstring url = service_->EventOf(all[rows_[i]]).url;
            if (IsWebUrl(url) && callbacks_.openUrl) callbacks_.openUrl(url);  // may hide the popup
        };
        view_.Init(Render(), Window(), kViewTimerBase, std::move(cb));
        viewReady_ = true;
    }
    if (!view_.Attached()) view_.Attach(ContentLayer());
    view_.SetStyle(ViewStyle());
    view_.SetMode(ItemGridView::Mode::List);

    LONG y = Px(kGridTop + kRows * kRowH + kListGap, s);
    dateRect_ = RECT{pad, y, w - pad, y + Px(kDateH, s)};
    y = dateRect_.bottom;
    const LONG footerH = style_.footer.empty() ? 0 : Px(kFooterGap + kFooterH, s);
    if (rows_.empty()) {
        emptyRect_ = RECT{pad, y, w - pad, y + Px(kEmptyH, s)};
        y = emptyRect_.bottom;
    } else {
        // As many rows as fit between the dock and the monitor's far edge (at most kMaxRows).
        const RECT& m = style_.monitor;
        const LONG margin = Px(kMonitorMargin, s);
        LONG room = (anchor_.side - m.top) - margin;
        if (anchor_.edge == DockEdge::Top) room = (m.bottom - anchor_.side) - margin;
        if (IsVertical(anchor_.edge)) room = (m.bottom - m.top) - 2 * margin;
        const LONG rowH = Px(kEventRowH, s);
        const LONG fixed = y + Px(kDetailGap + kDetailH + kBottomPad, s) + footerH;
        const size_t fit = static_cast<size_t>(std::max<LONG>(1, (room - fixed) / std::max<LONG>(1, rowH)));
        const size_t visible = std::min({rows_.size(), kMaxRows, fit});
        viewRect_ = RECT{pad / 2, y, w - pad / 2, y + static_cast<LONG>(visible) * rowH};
        y = viewRect_.bottom + Px(kDetailGap, s);
        detailRect_ = RECT{pad, y, w - pad, y + Px(kDetailH, s)};
        y = detailRect_.bottom;
    }
    view_.SetBounds(viewRect_);
    view_.Refresh();
    if (!style_.footer.empty()) {
        const LONG footerTop = y + Px(kFooterGap, s);
        footerRect_ = RECT{pad, footerTop, w - pad, footerTop + Px(kFooterH, s)};
        y = footerRect_.bottom;
    }
    if (Hot() >= kHitDayBase + kRows * 7) ClearHot();
    return SIZE{w, y + Px(kBottomPad, s)};
}

int CalendarPopup::DetailRow() const {
    if (rows_.empty()) return -1;
    const int hot = view_.Hot();
    if (hot >= 0 && static_cast<size_t>(hot) < rows_.size()) return hot;
    const int sel = view_.Selected();
    return sel >= 0 && static_cast<size_t>(sel) < rows_.size() ? sel : -1;
}

void CalendarPopup::Paint(ID2D1DeviceContext* dc) {
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
    auto text = [&](IDWriteTextFormat* format, const std::wstring& t, const D2D1_RECT_F& r, uint32_t color,
                    float alpha) {
        if (!format || t.empty()) return;
        brush->SetColor(Theme::ToColor(color, alpha));
        dc->DrawText(t.c_str(), static_cast<UINT32>(t.size()), format, r, brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    auto hotBox = [&](const RECT& r) {
        const float radius = std::round(kRadius * s);
        const D2D1_RECT_F f = ToRectF(r);
        brush->SetColor(Theme::ToColor(style_.hotBackground));
        dc->FillRoundedRectangle(D2D1::RoundedRect(f, radius, radius), brush.get());
        brush->SetColor(Theme::ToColor(style_.hotBorder));
        const D2D1_RECT_F edge = D2D1::RectF(f.left + inset, f.top + inset, f.right - inset, f.bottom - inset);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(edge, radius - inset, radius - inset), brush.get(), stroke);
    };

    // Today's long date, then the month shown with its arrows.
    text(titleFormat_.get(), FormatDate(DATE_LONGDATE, today_),
         D2D1::RectF(pad, kTitleTop * s, w - pad, (kTitleTop + kTitleH) * s), style_.text, 1.0f);
    SYSTEMTIME first{};
    first.wYear = static_cast<WORD>(year_);
    first.wMonth = static_cast<WORD>(month_);
    first.wDay = 1;
    if (hot == kHitMonth) hotBox(monthRect_);
    const D2D1_RECT_F monthF = ToRectF(monthRect_);
    text(monthFormat_.get(), FormatDate(DATE_YEARMONTH, first),
         D2D1::RectF(monthF.left + 6.0f * s, monthF.top, monthF.right, monthF.bottom), style_.text, 1.0f);
    for (const int which : {kHitPrev, kHitNext}) {
        const RECT& r = which == kHitPrev ? prevRect_ : nextRect_;
        if (hot == which) hotBox(r);
        const D2D1_RECT_F f = ToRectF(r);
        const float cx = (f.left + f.right) / 2.0f, cy = (f.top + f.bottom) / 2.0f;
        const float g = std::round(4.5f * s);
        const float dir = which == kHitPrev ? 1.0f : -1.0f;  // the chevron's open side
        brush->SetColor(Theme::ToColor(style_.text, hot == which ? 1.0f : 0.8f));
        const float glyphStroke = std::max(1.0f, 1.5f * s);
        dc->DrawLine(D2D1::Point2F(cx + dir * g / 2.0f, cy - g), D2D1::Point2F(cx - dir * g / 2.0f, cy), brush.get(),
                     glyphStroke);
        dc->DrawLine(D2D1::Point2F(cx - dir * g / 2.0f, cy), D2D1::Point2F(cx + dir * g / 2.0f, cy + g), brush.get(),
                     glyphStroke);
    }

    // Weekday headers.
    const float cell = std::round(kCell * s);
    const float headTop = kHeadTop * s;
    for (int c = 0; c < 7; ++c) {
        const int dow = (firstDay_ + c) % 7;  // 0 = Monday = LOCALE_SABBREVDAYNAME1
        wchar_t name[40]{};
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, static_cast<LCTYPE>(LOCALE_SABBREVDAYNAME1 + dow), name, 40);
        const float x = pad + static_cast<float>(c) * cell;
        text(headFormat_.get(), name, D2D1::RectF(x, headTop, x + cell, headTop + kHeadH * s), style_.secondary, 1.0f);
    }

    // The grid: 6 weeks from the week containing the 1st.
    const int lead = (WeekdayMonday0(year_, month_, 1) - firstDay_ + 7) % 7;
    const int days = DaysInMonth(year_, month_);
    const int prevMonth = month_ == 1 ? 12 : month_ - 1;
    const int prevDays = DaysInMonth(month_ == 1 ? year_ - 1 : year_, prevMonth);
    const bool thisMonth = year_ == today_.wYear && month_ == today_.wMonth;
    const float rowH = std::round(kRowH * s);
    const float gridTop = kGridTop * s;
    const float disc = kTodayDiameter * s / 2.0f;
    const bool events = Events();
    const bool dots = events && style_.dots;
    const int64_t gridFirst = GridFirst();
    const auto* items = events ? &service_->Items() : nullptr;
    dotsDrawn_ = 0;
    for (int i = 0; i < kRows * 7; ++i) {
        int day = i - lead + 1;
        bool inMonth = true;
        if (day < 1) {
            day += prevDays;
            inMonth = false;
        } else if (day > days) {
            day -= days;
            inMonth = false;
        }
        const float x = pad + static_cast<float>(i % 7) * cell;
        const float y = gridTop + static_cast<float>(i / 7) * rowH;
        const D2D1_RECT_F r = D2D1::RectF(x, y, x + cell, y + rowH);
        const D2D1_POINT_2F centre = D2D1::Point2F((r.left + r.right) / 2.0f, (r.top + r.bottom) / 2.0f);
        const int64_t cellDay = gridFirst + i;
        uint32_t color = style_.text;
        float alpha = inMonth ? 1.0f : 0.35f;
        if (events && hot == kHitDayBase + i) {
            const float radius = std::round(kRadius * s);
            brush->SetColor(Theme::ToColor(style_.hotBackground));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + inset, r.top + inset, r.right - inset, r.bottom - inset),
                                                       radius, radius),
                                     brush.get());
        }
        const bool isToday = inMonth && thisMonth && day == today_.wDay;
        if (isToday) {
            brush->SetColor(Theme::ToColor(style_.accent));
            dc->FillEllipse(D2D1::Ellipse(centre, disc, disc), brush.get());
            color = OnColor(style_.accent);
            alpha = 1.0f;
        } else if (events && cellDay == selected_) {
            brush->SetColor(Theme::ToColor(style_.accent));
            dc->DrawEllipse(D2D1::Ellipse(centre, disc - inset, disc - inset), brush.get(), std::max(1.0f, 1.5f * s));
        }
        text(dayFormat_.get(), std::to_wstring(day), r, color, alpha);
        if (dots) {
            const std::vector<size_t> on = service_->On(cellDay);
            const size_t n = std::min<size_t>(on.size(), 3);
            const float step = std::round(kDotStep * s), radius = kDot * s / 2.0f;
            const float x0 = centre.x - step * static_cast<float>(n - 1) / 2.0f;
            for (size_t k = 0; k < n; ++k) {
                const uint32_t c = isToday ? OnColor(style_.accent) : service_->ColorOf((*items)[on[k]]);
                brush->SetColor(Theme::ToColor(c, inMonth ? 1.0f : 0.45f));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + step * static_cast<float>(k), r.top + std::round(kDotY * s)), radius, radius),
                                brush.get());
                ++dotsDrawn_;
            }
        }
    }

    if (events) {
        // The selected day's header, then its list (its own visuals), "No events", or the strip.
        if (selected_ != INT64_MIN) {
            const SYSTEMTIME st = DayTime(selected_);
            text(noteFormat_.get(), FormatDate(DATE_LONGDATE, st), ToRectF(dateRect_), style_.secondary, 1.0f);
        }
        if (!IsRectEmpty(&emptyRect_)) {
            text(noteFormat_.get(), L"No events", ToRectF(emptyRect_), style_.secondary, 0.8f);
        }
        const int row = DetailRow();
        if (!IsRectEmpty(&detailRect_) && row >= 0 && rows_[static_cast<size_t>(row)] < items->size()) {
            const CalendarService::Item& it = (*items)[rows_[static_cast<size_t>(row)]];
            const Ics::Event& ev = service_->EventOf(it);
            const auto& feeds = service_->Feeds();
            std::wstring line1 = service_->RangeText(it, selected_);
            if (it.feed < feeds.size() && !feeds[it.feed].cfg.name.empty()) line1 += L"  \x00B7  " + feeds[it.feed].cfg.name;
            std::wstring line2 = FirstLine(ev.location);
            const std::wstring desc = FirstLine(ev.description);
            if (!desc.empty()) line2 = line2.empty() ? desc : line2 + L"  \x2014  " + desc;
            const D2D1_RECT_F f = ToRectF(detailRect_);
            const float mid = (f.top + f.bottom) / 2.0f;
            text(noteFormat_.get(), line1, D2D1::RectF(f.left, f.top, f.right, mid), style_.text, 0.9f);
            text(noteFormat_.get(), line2, D2D1::RectF(f.left, mid, f.right, f.bottom), style_.secondary, 1.0f);
        }
        lastDetail_ = row;
    }

    // The footer button (clock.clickTarget): a quiet box, outlined while hovered.
    if (!IsRectEmpty(&footerRect_)) {
        const float radius = std::round(kRadius * s);
        const D2D1_RECT_F f = ToRectF(footerRect_);
        if (hot == kHitFooter) {
            hotBox(footerRect_);
        } else {
            brush->SetColor(Theme::ToColor(style_.hotBackground, 0.6f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(f, radius, radius), brush.get());
        }
        text(dayFormat_.get(), style_.footer, f, style_.text, 1.0f);
    }
}

int CalendarPopup::HitAt(POINT pt, int* part) const {
    if (part) *part = 0;
    if (PtInRect(&prevRect_, pt)) return kHitPrev;
    if (PtInRect(&nextRect_, pt)) return kHitNext;
    if (PtInRect(&monthRect_, pt)) return kHitMonth;
    if (!IsRectEmpty(&footerRect_) && PtInRect(&footerRect_, pt)) return kHitFooter;
    if (Events()) {
        const float s = style_.scale;
        const float pad = std::round(kPad * s), cell = std::round(kCell * s), rowH = std::round(kRowH * s);
        const float gx = static_cast<float>(pt.x) - pad, gy = static_cast<float>(pt.y) - kGridTop * s;
        if (gx >= 0.0f && gy >= 0.0f && gx < 7.0f * cell && gy < kRows * rowH) {
            return kHitDayBase + static_cast<int>(gy / rowH) * 7 + static_cast<int>(gx / cell);
        }
    }
    return -1;
}

void CalendarPopup::OnClick(int index, int, Button button) {
    if (button != Button::Left) return;
    if (index == kHitPrev) Shift(-1);
    if (index == kHitNext) Shift(1);
    if (index == kHitMonth) Shift(0);
    if (index == kHitFooter && callbacks_.footer) callbacks_.footer();
    if (index >= kHitDayBase && index < kHitDayBase + kRows * 7 && Events()) SelectDay(GridFirst() + (index - kHitDayBase));
}

bool CalendarPopup::OnMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    result = 0;
    const bool list = Events() && view_.Attached();
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    auto detail = [&] {
        if (DetailRow() != lastDetail_ && SUCCEEDED(Redraw())) Commit();
    };
    switch (msg) {
        case WM_KEYDOWN:
            switch (wp) {
                case VK_LEFT:
                case VK_PRIOR:
                    Shift(-1);
                    return true;
                case VK_RIGHT:
                case VK_NEXT:
                    Shift(1);
                    return true;
                case VK_HOME:
                    Shift(0);
                    return true;
                case VK_UP:
                case VK_DOWN:
                case VK_RETURN:
                    if (list && !rows_.empty()) {
                        view_.KeyDown(static_cast<UINT>(wp));  // Enter may open a URL (and hide the popup)
                        if (Visible()) detail();
                        return true;
                    }
                    return false;
                default:
                    return false;
            }
        case WM_MOUSEMOVE:
            if (list) {
                view_.MouseMove(pt);
                detail();
            }
            return false;  // the base tracks the buttons, the days and the leave
        case WM_MOUSELEAVE:
            if (list) {
                view_.MouseLeave();
                detail();
            }
            return false;
        case WM_LBUTTONDOWN:
            return list && view_.ButtonDown(ItemGridView::Button::Left, pt);
        case WM_LBUTTONUP:
            return list && view_.ButtonUp(ItemGridView::Button::Left, pt);
        case WM_MOUSEWHEEL:
            if (list) {
                POINT client = pt;  // screen coordinates
                if (Window()) ScreenToClient(Window(), &client);
                view_.Wheel(GET_WHEEL_DELTA_WPARAM(wp), client);
            }
            return true;
        case WM_CAPTURECHANGED:
            if (list) view_.CaptureLost();
            return false;
        case WM_TIMER:
            return view_.Timer(wp);
        default:
            return false;
    }
}
