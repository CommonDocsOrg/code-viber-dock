#include "PreviewPopup.h"

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace {

// Layout in DIPs.
constexpr float kPad = 8.0f;          // popup edge -> cards
constexpr float kCardPad = 8.0f;      // card edge -> title / thumbnail box
constexpr float kTitleH = 20.0f;      // title row (also the close glyph's row)
constexpr float kTitleGap = 6.0f;     // title row -> thumbnail box
constexpr float kThumbW = 200.0f;     // thumbnail box
constexpr float kThumbH = 130.0f;
constexpr float kCardGap = 4.0f;      // between cards
constexpr float kCloseSize = 20.0f;   // close glyph hit box
constexpr float kCloseGlyph = 8.0f;   // the "x" itself
constexpr float kMoreW = 64.0f;       // "+N more" column
constexpr float kTitleFont = 12.0f;
constexpr float kMoreFont = 13.0f;
constexpr float kCardRadius = 6.0f;
constexpr float kIconDip = 48.0f;     // app icon in a card without a thumbnail
constexpr float kMonitorMargin = 8.0f;

D2D1_RECT_F ToRectF(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                       static_cast<float>(r.bottom));
}

LONG Px(float dip, float scale) { return static_cast<LONG>(std::lround(dip * scale)); }

}  // namespace

void PreviewPopup::Init(HINSTANCE instance, Renderer* renderer, Callbacks callbacks) {
    PopupWindow::Init(instance, renderer);
    callbacks_ = std::move(callbacks);
    SetHoverCallback(callbacks_.hover);
}

void PreviewPopup::OnDestroying() {
    ReleaseThumbs();
    cards_.clear();
    contentData_ = Content{};
    hiddenCount_ = 0;
    titleFormat_ = nullptr;
    moreFormat_ = nullptr;
    ellipsis_ = nullptr;
    formatScale_ = 0.0f;
}

void PreviewPopup::OnHidden() {
    ReleaseThumbs();
    cards_.clear();
    contentData_ = Content{};
    hiddenCount_ = 0;
}

void PreviewPopup::ReleaseThumbs() {
    for (Card& c : cards_) {
        if (c.thumb) DwmUnregisterThumbnail(c.thumb);
        c.thumb = nullptr;
        c.hasThumb = false;
    }
}

void PreviewPopup::SyncCards(const std::vector<HWND>& windows) {
    // Keep the thumbnails of windows that stay; unregister the others; register new ones.
    std::vector<Card> next;
    next.reserve(windows.size());
    for (HWND w : windows) {
        Card card;
        auto it = std::find_if(cards_.begin(), cards_.end(), [&](const Card& c) { return c.window == w; });
        if (it != cards_.end()) {
            card = std::move(*it);
            it->thumb = nullptr;
            it->window = nullptr;
        } else {
            card.window = w;
            if (FAILED(DwmRegisterThumbnail(Window(), w, &card.thumb))) card.thumb = nullptr;
        }
        wchar_t buf[256]{};
        GetWindowTextW(w, buf, static_cast<int>(std::size(buf)));  // cached caption, never blocks
        card.title = buf;
        next.push_back(std::move(card));
    }
    for (Card& c : cards_) {
        if (c.thumb) DwmUnregisterThumbnail(c.thumb);
    }
    cards_ = std::move(next);
}

bool PreviewPopup::Show(const Content& content, const Style& style, const PopupAnchor& anchor) {
    if (content.windows.empty()) return false;
    style_ = style;
    contentData_ = content;
    Frame frame;
    frame.scale = style.scale;
    frame.acrylic = style.acrylic;
    frame.cornerRadiusDip = style.cornerRadiusDip;
    frame.monitor = style.monitor;
    return ShowAt(frame, anchor);
}

SIZE PreviewPopup::Measure() {
    const float s = style_.scale;
    const LONG pad = Px(kPad, s), cardPad = Px(kCardPad, s), titleH = Px(kTitleH, s), titleGap = Px(kTitleGap, s);
    const LONG boxW = Px(kThumbW, s), boxH = Px(kThumbH, s), gap = Px(kCardGap, s), moreW = Px(kMoreW, s);
    const LONG cardW = boxW + 2 * cardPad;
    const LONG cardH = cardPad + titleH + titleGap + boxH + cardPad;
    const LONG closeSize = Px(kCloseSize, s);

    // As many cards as fit on the monitor (at most kMaxCards); the rest are counted as "+N more".
    const LONG avail = (style_.monitor.right - style_.monitor.left) - 2 * Px(kMonitorMargin, s) - 2 * pad;
    const std::vector<HWND>& all = contentData_.windows;
    size_t n = std::min(all.size(), kMaxCards);
    auto width = [&](size_t count, bool more) {
        LONG w = static_cast<LONG>(count) * cardW + static_cast<LONG>(count ? count - 1 : 0) * gap;
        if (more) w += gap + moreW;
        return w;
    };
    while (n > 1 && width(n, n < all.size()) > avail) --n;
    hiddenCount_ = all.size() - n;
    SyncCards(std::vector<HWND>(all.begin(), all.begin() + static_cast<ptrdiff_t>(n)));

    LONG x = pad;
    for (Card& c : cards_) {
        c.rect = RECT{x, pad, x + cardW, pad + cardH};
        c.boxRect = RECT{x + cardPad, pad + cardPad + titleH + titleGap, x + cardPad + boxW,
                         pad + cardPad + titleH + titleGap + boxH};
        const LONG closeTop = pad + cardPad + (titleH - closeSize) / 2;
        c.closeRect = RECT{c.rect.right - cardPad - closeSize + Px(2.0f, s), closeTop,
                           c.rect.right - cardPad + Px(2.0f, s), closeTop + closeSize};

        // Fit the source's aspect ratio into the box (never enlarged). A minimized window may
        // report no size: then the card shows the app icon instead.
        c.hasThumb = false;
        c.thumbRect = c.boxRect;
        if (c.thumb) {
            SIZE src{};
            if (SUCCEEDED(DwmQueryThumbnailSourceSize(c.thumb, &src)) && src.cx > 0 && src.cy > 0) {
                // DWM_TNP_SOURCECLIENTAREAONLY shows the client area: use its aspect when known.
                RECT client{};
                if (!IsIconic(c.window) && GetClientRect(c.window, &client) && client.right > 0 && client.bottom > 0) {
                    src = SIZE{client.right, client.bottom};
                }
                const double fit = std::min({static_cast<double>(boxW) / src.cx, static_cast<double>(boxH) / src.cy, 1.0});
                const LONG w = std::max<LONG>(1, std::lround(src.cx * fit));
                const LONG h = std::max<LONG>(1, std::lround(src.cy * fit));
                const LONG l = c.boxRect.left + (boxW - w) / 2;
                const LONG t = c.boxRect.top + (boxH - h) / 2;
                c.thumbRect = RECT{l, t, l + w, t + h};
                c.hasThumb = true;
            }
        }
        x += cardW + gap;
    }
    LONG w = width(cards_.size(), hiddenCount_ > 0) + 2 * pad;
    moreRect_ = hiddenCount_ ? RECT{w - pad - moreW, pad, w - pad, pad + cardH} : RECT{};
    if (Hot() >= static_cast<int>(cards_.size())) ClearHot();
    return SIZE{w, cardH + 2 * pad};
}

void PreviewPopup::EnsureFormats() {
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    const float s = style_.scale;
    if (!dw || (titleFormat_ && formatScale_ == s)) return;
    titleFormat_ = nullptr;
    moreFormat_ = nullptr;
    ellipsis_ = nullptr;
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, kTitleFont * s, L"", titleFormat_.put()))) {
        titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(titleFormat_.get(), ellipsis_.put()))) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            titleFormat_->SetTrimming(&trimming, ellipsis_.get());
        }
    } else {
        titleFormat_ = nullptr;
    }
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, kMoreFont * s, L"", moreFormat_.put()))) {
        moreFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        moreFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        moreFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    } else {
        moreFormat_ = nullptr;
    }
    formatScale_ = s;
}

void PreviewPopup::Paint(ID2D1DeviceContext* dc) {
    EnsureFormats();
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    const float s = style_.scale;
    const float stroke = std::max(1.0f, std::round(s));
    const float cardRadius = std::round(kCardRadius * s);
    const int hot = Hot();
    const bool hotClose = HotPart() == kClose;

    PaintPanel(dc, style_.tint, style_.border);
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(style_.text), brush.put()))) return;
    const float inset = stroke / 2.0f;

    for (size_t i = 0; i < cards_.size(); ++i) {
        const Card& c = cards_[i];
        const bool isHot = static_cast<int>(i) == hot;
        const D2D1_RECT_F card = ToRectF(c.rect);
        if (isHot) {
            brush->SetColor(Theme::ToColor(style_.hotBackground));
            dc->FillRoundedRectangle(D2D1::RoundedRect(card, cardRadius, cardRadius), brush.get());
            brush->SetColor(Theme::ToColor(style_.hotBorder));
            const D2D1_RECT_F edge =
                D2D1::RectF(card.left + inset, card.top + inset, card.right - inset, card.bottom - inset);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(edge, cardRadius - inset, cardRadius - inset), brush.get(),
                                     stroke);
        }

        // Title (ellipsized), left of the close glyph.
        if (dw && titleFormat_ && !c.title.empty()) {
            const float left = static_cast<float>(c.boxRect.left);
            const float right = static_cast<float>(c.closeRect.left) - std::round(4.0f * s);
            const float top = static_cast<float>(c.rect.top) + std::round(kCardPad * s);
            const float bottom = top + std::round(kTitleH * s);
            winrt::com_ptr<IDWriteTextLayout> layout;
            if (right > left &&
                SUCCEEDED(dw->CreateTextLayout(c.title.c_str(), static_cast<UINT32>(c.title.size()), titleFormat_.get(),
                                               right - left, bottom - top, layout.put()))) {
                brush->SetColor(Theme::ToColor(style_.text, isHot ? 1.0f : 0.85f));
                dc->DrawTextLayout(D2D1::Point2F(left, top), layout.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        // Close glyph: a red rounded square behind it while hovered.
        const D2D1_RECT_F close = ToRectF(c.closeRect);
        if (isHot && hotClose) {
            brush->SetColor(Theme::ToColor(style_.closeHover));
            const float r = std::round(4.0f * s);
            dc->FillRoundedRectangle(D2D1::RoundedRect(close, r, r), brush.get());
        }
        const float cx = (close.left + close.right) / 2.0f, cy = (close.top + close.bottom) / 2.0f;
        const float g = std::round(kCloseGlyph * s) / 2.0f;
        brush->SetColor(Theme::ToColor(style_.text, isHot ? 1.0f : 0.55f));
        const float glyphStroke = std::max(1.0f, 1.25f * s);
        dc->DrawLine(D2D1::Point2F(cx - g, cy - g), D2D1::Point2F(cx + g, cy + g), brush.get(), glyphStroke);
        dc->DrawLine(D2D1::Point2F(cx - g, cy + g), D2D1::Point2F(cx + g, cy - g), brush.get(), glyphStroke);

        if (c.hasThumb) {
            // DWM renders the thumbnail into this rect: leave it transparent.
            dc->PushAxisAlignedClip(ToRectF(c.thumbRect), D2D1_ANTIALIAS_MODE_ALIASED);
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            dc->PopAxisAlignedClip();
        } else {
            // No thumbnail (e.g. a minimized window without a cached frame): the app icon.
            const D2D1_RECT_F box = ToRectF(c.boxRect);
            brush->SetColor(Theme::ToColor(style_.hotBackground, 0.5f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(box, cardRadius, cardRadius), brush.get());
            if (ID2D1Bitmap* icon = contentData_.icon.get()) {
                const float size = std::round(kIconDip * s);
                const float bx = std::round((box.left + box.right - size) / 2.0f);
                const float by = std::round((box.top + box.bottom - size) / 2.0f);
                const D2D1_RECT_F dst = D2D1::RectF(bx, by, bx + size, by + size);
                dc->DrawBitmap(icon, &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            }
        }
    }

    if (hiddenCount_ && moreFormat_) {
        const std::wstring more = L"+" + std::to_wstring(hiddenCount_) + L" more";
        brush->SetColor(Theme::ToColor(style_.text, 0.75f));
        dc->DrawText(more.c_str(), static_cast<UINT32>(more.size()), moreFormat_.get(), ToRectF(moreRect_),
                     brush.get());
    }
}

void PreviewPopup::OnFade(BYTE opacity, bool placed) {
    for (const Card& c : cards_) {
        if (!c.thumb) continue;
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_OPACITY | DWM_TNP_VISIBLE;
        p.fVisible = c.hasThumb ? TRUE : FALSE;
        p.opacity = opacity;
        if (placed) {
            p.dwFlags |= DWM_TNP_RECTDESTINATION | DWM_TNP_SOURCECLIENTAREAONLY;
            p.rcDestination = c.thumbRect;
            p.fSourceClientAreaOnly = TRUE;
        }
        DwmUpdateThumbnailProperties(c.thumb, &p);
    }
}

int PreviewPopup::HitAt(POINT pt, int* part) const {
    if (part) *part = kCard;
    for (size_t i = 0; i < cards_.size(); ++i) {
        if (!PtInRect(&cards_[i].rect, pt)) continue;
        if (part && PtInRect(&cards_[i].closeRect, pt)) *part = kClose;
        return static_cast<int>(i);
    }
    return -1;
}

void PreviewPopup::OnClick(int index, int part, Button button) {
    if (index < 0 || static_cast<size_t>(index) >= cards_.size()) return;
    HWND target = cards_[static_cast<size_t>(index)].window;  // the callbacks may hide the popup
    if (button == Button::Middle || (button == Button::Left && part == kClose)) {
        if (callbacks_.close) callbacks_.close(target);
    } else if (button == Button::Left && callbacks_.activate) {
        callbacks_.activate(target);
    }
}
