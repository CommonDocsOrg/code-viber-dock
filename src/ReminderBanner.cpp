#include "ReminderBanner.h"

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace {

constexpr float kWidth = 340.0f;
constexpr float kPad = 12.0f;
constexpr float kBarW = 4.0f;
constexpr float kTitleH = 22.0f;
constexpr float kLineH = 18.0f;
constexpr float kButtonsGap = 8.0f;
constexpr float kButtonH = 28.0f;
constexpr float kButtonW = 100.0f;
constexpr float kRadius = 6.0f;

LONG Px(float dip, float s) { return static_cast<LONG>(std::lround(dip * s)); }
D2D1_RECT_F ToRectF(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                       static_cast<float>(r.bottom));
}

}  // namespace

void ReminderBanner::Init(HINSTANCE instance, Renderer* renderer, Callbacks callbacks) {
    PopupWindow::Init(instance, renderer);
    callbacks_ = std::move(callbacks);
}

void ReminderBanner::OnDestroying() {
    titleFormat_ = textFormat_ = buttonFormat_ = nullptr;
    formatScale_ = 0.0f;
}

bool ReminderBanner::Show(const Style& style, const PopupAnchor& anchor, const CalendarService::ReminderInfo& reminder,
                          size_t more) {
    style_ = style;
    frame_ = Frame{};
    frame_.scale = style.scale;
    frame_.acrylic = style.acrylic;
    frame_.cornerRadiusDip = style.cornerRadiusDip;
    frame_.monitor = style.monitor;
    info_ = reminder;
    more_ = more;
    // Written once: the banner does not tick ("In 15 min" is when it appeared).
    const int64_t left = reminder.start - CalendarService::Now();
    if (left <= 30) {
        when_ = left < -60 ? L"Started at " + reminder.when : L"Now \x00B7 " + reminder.when;
    } else {
        const int64_t minutes = (left + 59) / 60;
        when_ = minutes < 60 ? L"In " + std::to_wstring(minutes) + L" min \x00B7 " + reminder.when
                             : L"In " + std::to_wstring(minutes / 60) + L" h " + std::to_wstring(minutes % 60) + L" min \x00B7 " + reminder.when;
    }
    return ShowAt(frame_, anchor);
}

void ReminderBanner::EnsureFormats() {
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    const float s = style_.scale;
    if (!dw || (titleFormat_ && formatScale_ == s)) return;
    titleFormat_ = textFormat_ = buttonFormat_ = nullptr;
    auto make = [&](winrt::com_ptr<IDWriteTextFormat>& f, DWRITE_FONT_WEIGHT weight, float size, DWRITE_TEXT_ALIGNMENT align) {
        if (FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size * s, L"", f.put()))) {
            f = nullptr;
            return;
        }
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        winrt::com_ptr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(f.get(), ellipsis.put()))) f->SetTrimming(&trim, ellipsis.get());
    };
    make(titleFormat_, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
    make(textFormat_, DWRITE_FONT_WEIGHT_NORMAL, 12.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
    make(buttonFormat_, DWRITE_FONT_WEIGHT_NORMAL, 12.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    formatScale_ = s;
}

SIZE ReminderBanner::Measure() {
    EnsureFormats();
    const float s = style_.scale;
    const LONG w = Px(kWidth, s), pad = Px(kPad, s);
    LONG y = pad + Px(kTitleH + kLineH, s);
    if (!info_.location.empty()) y += Px(kLineH, s);
    y += Px(kButtonsGap, s);
    const LONG bw = Px(kButtonW, s), bh = Px(kButtonH, s), gap = Px(8.0f, s);
    dismissRect_ = RECT{w - pad - bw, y, w - pad, y + bh};
    snoozeRect_ = RECT{dismissRect_.left - gap - bw, y, dismissRect_.left - gap, y + bh};
    return SIZE{w, y + bh + pad};
}

void ReminderBanner::Paint(ID2D1DeviceContext* dc) {
    EnsureFormats();
    const float s = style_.scale;
    const float w = static_cast<float>(ClientSize().cx), pad = std::round(kPad * s);
    PaintPanel(dc, style_.tint, style_.border);
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(info_.color), brush.put()))) return;
    float y = pad;
    const float textX = pad + std::round((kBarW + 8.0f) * s);
    float barBottom = y + std::round((kTitleH + kLineH) * s) + (info_.location.empty() ? 0.0f : std::round(kLineH * s));
    dc->FillRectangle(D2D1::RectF(pad, y + 2.0f * s, pad + std::round(kBarW * s), barBottom - 2.0f * s), brush.get());
    auto text = [&](IDWriteTextFormat* f, const std::wstring& t, const D2D1_RECT_F& r, uint32_t color) {
        if (!f || t.empty()) return;
        brush->SetColor(Theme::ToColor(color));
        dc->DrawText(t.c_str(), static_cast<UINT32>(t.size()), f, r, brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const std::wstring more = more_ ? L"+" + std::to_wstring(more_) + L" more" : std::wstring();
    const float moreW = more_ ? std::round(64.0f * s) : 0.0f;
    text(titleFormat_.get(), info_.title, D2D1::RectF(textX, y, w - pad - moreW, y + kTitleH * s), style_.text);
    text(textFormat_.get(), more, D2D1::RectF(w - pad - moreW, y, w - pad, y + kTitleH * s), style_.secondary);
    y += std::round(kTitleH * s);
    text(textFormat_.get(), when_, D2D1::RectF(textX, y, w - pad, y + kLineH * s), style_.text);
    y += std::round(kLineH * s);
    if (!info_.location.empty()) text(textFormat_.get(), info_.location, D2D1::RectF(textX, y, w - pad, y + kLineH * s), style_.secondary);
    const float stroke = std::max(1.0f, std::round(s)), inset = stroke / 2.0f, radius = std::round(kRadius * s);
    for (const int which : {kHitSnooze, kHitDismiss}) {
        const RECT& rc = which == kHitSnooze ? snoozeRect_ : dismissRect_;
        const D2D1_RECT_F f = ToRectF(rc);
        const bool hot = Hot() == which;
        brush->SetColor(Theme::ToColor(hot ? style_.hotBorder : style_.hotBackground));
        dc->FillRoundedRectangle(D2D1::RoundedRect(f, radius, radius), brush.get());
        brush->SetColor(Theme::ToColor(hot ? style_.hotBorder : style_.border));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(f.left + inset, f.top + inset, f.right - inset, f.bottom - inset),
                                                   radius - inset, radius - inset),
                                 brush.get(), stroke);
        text(buttonFormat_.get(), which == kHitSnooze ? L"Snooze 5 min" : L"Dismiss", f, style_.text);
    }
}

int ReminderBanner::HitAt(POINT pt, int* part) const {
    if (part) *part = 0;
    if (PtInRect(&snoozeRect_, pt)) return kHitSnooze;
    if (PtInRect(&dismissRect_, pt)) return kHitDismiss;
    return kHitBody;
}

void ReminderBanner::OnClick(int index, int, Button button) {
    if (button != Button::Left) return;
    if (index == kHitSnooze && callbacks_.snooze) callbacks_.snooze();
    if (index == kHitDismiss && callbacks_.dismiss) callbacks_.dismiss();
    if (index == kHitBody && callbacks_.open) callbacks_.open();
}
