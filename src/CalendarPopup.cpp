#include "CalendarPopup.h"

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

LONG Px(float dip, float scale) { return static_cast<LONG>(std::lround(dip * scale)); }

D2D1_RECT_F ToRectF(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                       static_cast<float>(r.bottom));
}

int DaysInMonth(int y, int m) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return kDays[(m - 1) % 12];
}

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

// Black or white, whichever reads better on `argb`.
uint32_t OnColor(uint32_t argb) {
    const float r = static_cast<float>((argb >> 16) & 0xFF), g = static_cast<float>((argb >> 8) & 0xFF),
                b = static_cast<float>(argb & 0xFF);
    return 0.299f * r + 0.587f * g + 0.114f * b > 150.0f ? 0xFF000000u : 0xFFFFFFFFu;
}

}  // namespace

void CalendarPopup::Init(HINSTANCE instance, Renderer* renderer, std::function<void()> closed,
                         std::function<void()> footer) {
    PopupWindow::Init(instance, renderer);
    SetClosedCallback(std::move(closed));
    footer_ = std::move(footer);
}

void CalendarPopup::OnDestroying() {
    titleFormat_ = monthFormat_ = dayFormat_ = headFormat_ = nullptr;
    formatScale_ = 0.0f;
}

bool CalendarPopup::Show(const Style& style, const PopupAnchor& anchor) {
    style_ = style;
    frame_ = Frame{};
    frame_.scale = style.scale;
    frame_.acrylic = style.acrylic;
    frame_.cornerRadiusDip = style.cornerRadiusDip;
    frame_.monitor = style.monitor;
    GetLocalTime(&today_);
    year_ = today_.wYear;
    month_ = today_.wMonth;
    DWORD first = 0;  // 0 = Monday ... 6 = Sunday
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER,
                        reinterpret_cast<LPWSTR>(&first), sizeof(first) / sizeof(wchar_t))) {
        firstDay_ = static_cast<int>(first % 7);
    }
    return ShowAt(frame_, anchor);
}

int CalendarPopup::MonthOffset() const {
    if (!OnScreen()) return 0;
    return (year_ - today_.wYear) * 12 + (month_ - today_.wMonth);
}

void CalendarPopup::Shift(int months) {
    if (months == 0) {
        GetLocalTime(&today_);
        year_ = today_.wYear;
        month_ = today_.wMonth;
    } else {
        int index = year_ * 12 + (month_ - 1) + months;
        index = std::clamp(index, 1601 * 12, 9999 * 12 + 11);  // SYSTEMTIME's range
        year_ = index / 12;
        month_ = index % 12 + 1;
    }
    if (SUCCEEDED(Redraw())) Commit();  // fixed size: no re-layout
}

void CalendarPopup::EnsureFormats() {
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    const float s = style_.scale;
    if (!dw || (titleFormat_ && formatScale_ == s)) return;
    titleFormat_ = monthFormat_ = dayFormat_ = headFormat_ = nullptr;
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
    LONG h = Px(kGridTop + kRows * kRowH + kBottomPad, s);
    footerRect_ = RECT{};
    if (!style_.footer.empty()) {
        const LONG footerTop = Px(kGridTop + kRows * kRowH + kFooterGap, s);
        footerRect_ = RECT{pad, footerTop, w - pad, footerTop + Px(kFooterH, s)};
        h = footerRect_.bottom + Px(kBottomPad, s);
    }
    return SIZE{w, h};
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
        uint32_t color = style_.text;
        float alpha = inMonth ? 1.0f : 0.35f;
        if (inMonth && thisMonth && day == today_.wDay) {
            brush->SetColor(Theme::ToColor(style_.accent));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((r.left + r.right) / 2.0f, (r.top + r.bottom) / 2.0f), disc, disc),
                            brush.get());
            color = OnColor(style_.accent);
            alpha = 1.0f;
        }
        text(dayFormat_.get(), std::to_wstring(day), r, color, alpha);
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
    return -1;
}

void CalendarPopup::OnClick(int index, int, Button button) {
    if (button != Button::Left) return;
    if (index == kHitPrev) Shift(-1);
    if (index == kHitNext) Shift(1);
    if (index == kHitMonth) Shift(0);
    if (index == kHitFooter && footer_) footer_();
}

bool CalendarPopup::OnMessage(UINT msg, WPARAM wp, LPARAM, LRESULT& result) {
    if (msg != WM_KEYDOWN) return false;
    switch (wp) {
        case VK_LEFT:
        case VK_PRIOR:
            Shift(-1);
            break;
        case VK_RIGHT:
        case VK_NEXT:
            Shift(1);
            break;
        case VK_HOME:
            Shift(0);
            break;
        default:
            return false;
    }
    result = 0;
    return true;
}
