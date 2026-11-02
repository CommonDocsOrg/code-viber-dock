#include "PreviewPopup.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace {

constexpr DWORD kExStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
constexpr DWORD kStyle = WS_POPUP;
constexpr UINT_PTR kFadeTimerId = 1;

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
constexpr double kFadeS = 0.12;

D2D1_RECT_F ToRectF(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                       static_cast<float>(r.bottom));
}

LONG Px(float dip, float scale) { return static_cast<LONG>(std::lround(dip * scale)); }

}  // namespace

void PreviewPopup::Init(HINSTANCE instance, Renderer* renderer, Callbacks callbacks) {
    instance_ = instance;
    renderer_ = renderer;
    callbacks_ = std::move(callbacks);
}

bool PreviewPopup::EnsureWindow() {
    if (hwnd_ && target_ && content_.visual) return true;
    if (!renderer_ || !instance_) return false;
    if (!hwnd_) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &PreviewPopup::WndProcThunk;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        hwnd_ = CreateWindowExW(kExStyle, kClassName, L"WinDock preview", kStyle, 0, 0, 1, 1, nullptr, nullptr,
                                instance_, this);
        if (!hwnd_) return false;
        // Shown and hidden instantly; the fade is ours.
        const BOOL off = TRUE;
        DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    }
    HRESULT hr = renderer_->CreateWindowTarget(hwnd_, target_, root_);
    if (SUCCEEDED(hr)) hr = renderer_->CreateLayer(content_, &root_);
    if (FAILED(hr)) {
        wchar_t buf[96];
        swprintf_s(buf, L"WinDock: preview target failed (0x%08X)\n", static_cast<unsigned>(hr));
        OutputDebugStringW(buf);
        Destroy();
        return false;
    }
    fade_.valid = false;
    return true;
}

void PreviewPopup::Destroy() {
    if (fadePending_ && hwnd_) KillTimer(hwnd_, kFadeTimerId);
    fadePending_ = false;
    ReleaseThumbs();
    cards_.clear();
    contentData_ = Content{};
    content_.Reset();
    root_.Reset();
    if (target_) target_->SetRoot(nullptr);
    target_ = nullptr;
    backdrop_.Destroy();
    titleFormat_ = nullptr;
    moreFormat_ = nullptr;
    ellipsis_ = nullptr;
    formatScale_ = 0.0f;
    state_ = State::Hidden;
    hot_ = pressed_ = pressedMiddle_ = -1;
    tracking_ = inside_ = false;
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
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
            if (FAILED(DwmRegisterThumbnail(hwnd_, w, &card.thumb))) card.thumb = nullptr;
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

void PreviewPopup::LayoutCards() {
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
    size_ = SIZE{w, cardH + 2 * pad};
    if (hot_ >= static_cast<int>(cards_.size())) {
        hot_ = -1;
        hotClose_ = false;
    }
}

HRESULT PreviewPopup::Draw() {
    if (!content_.visual || !renderer_) return E_UNEXPECTED;
    IDWriteFactory* dw = renderer_->DWrite();
    const float s = style_.scale;
    if (dw && (!titleFormat_ || formatScale_ != s)) {
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
        if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, kMoreFont * s, L"",
                                           moreFormat_.put()))) {
            moreFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            moreFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            moreFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        } else {
            moreFormat_ = nullptr;
        }
        formatScale_ = s;
    }

    const float radius =
        std::round((style_.acrylic ? style_.cornerRadiusDip : std::min(style_.cornerRadiusDip, 12.0f)) * s);
    const float stroke = std::max(1.0f, std::round(s));
    const float cardRadius = std::round(kCardRadius * s);
    const D2D1_COLOR_F tint = Theme::ToColor(style_.tint);
    const UINT w = static_cast<UINT>(size_.cx), h = static_cast<UINT>(size_.cy);

    return renderer_->DrawLayer(content_, w, h, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(tint, brush.put()))) return;
        const float fw = static_cast<float>(w), fh = static_cast<float>(h);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, fw, fh), radius, radius), brush.get());
        const float inset = stroke / 2.0f;
        brush->SetColor(Theme::ToColor(style_.border));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(inset, inset, fw - inset, fh - inset),
                                                   std::max(0.0f, radius - inset), std::max(0.0f, radius - inset)),
                                 brush.get(), stroke);

        for (size_t i = 0; i < cards_.size(); ++i) {
            const Card& c = cards_[i];
            const bool hot = static_cast<int>(i) == hot_;
            const D2D1_RECT_F card = ToRectF(c.rect);
            if (hot) {
                brush->SetColor(Theme::ToColor(style_.hotBackground));
                dc->FillRoundedRectangle(D2D1::RoundedRect(card, cardRadius, cardRadius), brush.get());
                brush->SetColor(Theme::ToColor(style_.hotBorder));
                const D2D1_RECT_F edge = D2D1::RectF(card.left + inset, card.top + inset, card.right - inset,
                                                     card.bottom - inset);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(edge, cardRadius - inset, cardRadius - inset), brush.get(),
                                         stroke);
            }

            // Title (ellipsized), left of the close glyph.
            if (titleFormat_ && !c.title.empty()) {
                const float left = static_cast<float>(c.boxRect.left);
                const float right = static_cast<float>(c.closeRect.left) - std::round(4.0f * s);
                const float top = static_cast<float>(c.rect.top) + std::round(kCardPad * s);
                const float bottom = top + std::round(kTitleH * s);
                winrt::com_ptr<IDWriteTextLayout> layout;
                if (right > left &&
                    SUCCEEDED(dw->CreateTextLayout(c.title.c_str(), static_cast<UINT32>(c.title.size()),
                                                   titleFormat_.get(), right - left, bottom - top, layout.put()))) {
                    brush->SetColor(Theme::ToColor(style_.text, hot ? 1.0f : 0.85f));
                    dc->DrawTextLayout(D2D1::Point2F(left, top), layout.get(), brush.get(),
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }

            // Close glyph: a red rounded square behind it while hovered.
            const D2D1_RECT_F close = ToRectF(c.closeRect);
            if (hot && hotClose_) {
                brush->SetColor(Theme::ToColor(style_.closeHover));
                const float r = std::round(4.0f * s);
                dc->FillRoundedRectangle(D2D1::RoundedRect(close, r, r), brush.get());
            }
            const float cx = (close.left + close.right) / 2.0f, cy = (close.top + close.bottom) / 2.0f;
            const float g = std::round(kCloseGlyph * s) / 2.0f;
            brush->SetColor(Theme::ToColor(style_.text, hot ? 1.0f : 0.55f));
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
    });
}

void PreviewPopup::UpdateThumbs(BYTE opacity, bool full) {
    for (const Card& c : cards_) {
        if (!c.thumb) continue;
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_OPACITY | DWM_TNP_VISIBLE;
        p.fVisible = c.hasThumb ? TRUE : FALSE;
        p.opacity = opacity;
        if (full) {
            p.dwFlags |= DWM_TNP_RECTDESTINATION | DWM_TNP_SOURCECLIENTAREAONLY;
            p.rcDestination = c.thumbRect;
            p.fSourceClientAreaOnly = TRUE;
        }
        DwmUpdateThumbnailProperties(c.thumb, &p);
    }
}

void PreviewPopup::Commit() {
    if (!renderer_) return;
    const HRESULT hr = renderer_->Commit();
    if (FAILED(hr)) {
        wchar_t buf[96];
        swprintf_s(buf, L"WinDock: preview commit failed (0x%08X)\n", static_cast<unsigned>(hr));
        OutputDebugStringW(buf);
    }
}

bool PreviewPopup::Show(const Content& content, const Style& style, int anchorX, int bottom) {
    if (content.windows.empty() || !EnsureWindow()) return false;
    const bool wasHidden = state_ == State::Hidden;
    style_ = style;
    if (style_.acrylic && !backdrop_.Exists() && !backdrop_.Create(instance_)) style_.acrylic = false;
    if (style_.acrylic) {
        style_.cornerRadiusDip = backdrop_.SetCornerRadius(8.0f);
    } else {
        backdrop_.Destroy();
    }
    contentData_ = content;
    LayoutCards();
    if (FAILED(Draw())) return false;

    const RECT& m = style_.monitor;
    const LONG margin = Px(kMonitorMargin, style_.scale);
    LONG x = anchorX - size_.cx / 2;
    // Shrunk while the mouse is in it (a window closed): keep the cursor inside instead of
    // re-centring away from it, so the popup does not dismiss itself under a still mouse.
    POINT cursor{};
    if (inside_ && !wasHidden && GetCursorPos(&cursor)) {
        const LONG keep = Px(kCloseSize, style_.scale);
        if (cursor.x >= x + size_.cx) x = cursor.x - size_.cx + keep;
        if (cursor.x < x) x = cursor.x - keep;
    }
    x = std::clamp(x, m.left + margin, std::max(m.left + margin, m.right - margin - size_.cx));
    LONG y = std::max(bottom - size_.cy, m.top + margin);
    rect_ = RECT{x, y, x + size_.cx, y + size_.cy};

    const double now = Renderer::Now();
    if (wasHidden) {
        fade_ = Tween{0.0f, 0.0f, now, 0.0, true};
        winrt::com_ptr<IDCompositionVisual3> root3;
        if (root_.visual && SUCCEEDED(root_.visual->QueryInterface(IID_PPV_ARGS(root3.put())))) {
            root3->SetOpacity(0.0f);
        } else {
            fade_ = Tween{1.0f, 1.0f, now, 0.0, true};  // no opacity support: no fade
        }
    }
    state_ = State::Shown;
    UpdateThumbs(static_cast<BYTE>(std::lround(std::clamp(fade_.Value(now), 0.0f, 1.0f) * 255.0f)), true);
    StartFade(1.0f);
    Commit();
    SetWindowPos(hwnd_, HWND_TOPMOST, rect_.left, rect_.top, size_.cx, size_.cy,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | (wasHidden ? SWP_SHOWWINDOW : 0));
    if (style_.acrylic) backdrop_.Place(hwnd_, rect_);

    // Resized away from under the cursor: that is a leave (no WM_MOUSELEAVE may come).
    if (inside_ && !ContainsCursor()) {
        if (tracking_) {
            TRACKMOUSEEVENT cancel{sizeof(cancel), TME_LEAVE | TME_CANCEL, hwnd_, 0};
            TrackMouseEvent(&cancel);
            tracking_ = false;
        }
        inside_ = false;
        hot_ = -1;
        if (callbacks_.hover) callbacks_.hover(false);
    }
    return true;
}

void PreviewPopup::StartFade(float to) {
    if (!root_.visual || !renderer_) return;
    winrt::com_ptr<IDCompositionVisual3> root3;
    if (FAILED(root_.visual->QueryInterface(IID_PPV_ARGS(root3.put())))) {
        fade_ = Tween{to, to, Renderer::Now(), 0.0, true};
        return;
    }
    const double now = Renderer::Now();
    const float remaining = std::fabs(to - fade_.Value(now));
    renderer_->Drive(fade_, to, std::max(0.03, kFadeS * remaining), now,
                     [&](auto v) { return root3->SetOpacity(v); }, 0.001f);
    if (fade_.Running(now) && !fadePending_) {
        SetTimer(hwnd_, kFadeTimerId, 16, nullptr);
        fadePending_ = true;
    }
}

void PreviewPopup::OnFadeTimer() {
    KillTimer(hwnd_, kFadeTimerId);
    fadePending_ = false;
    const double now = Renderer::Now();
    const float v = std::clamp(fade_.Value(now), 0.0f, 1.0f);
    if (fade_.Running(now)) {
        UpdateThumbs(static_cast<BYTE>(std::lround(v * 255.0f)), false);
        SetTimer(hwnd_, kFadeTimerId, 16, nullptr);
        fadePending_ = true;
        return;
    }
    if (state_ == State::FadingOut) {
        FinishHide();
        return;
    }
    UpdateThumbs(static_cast<BYTE>(std::lround(v * 255.0f)), false);
}

void PreviewPopup::Hide(bool animate) {
    if (state_ == State::Hidden || !hwnd_) return;
    if (!animate) {
        FinishHide();
        return;
    }
    if (state_ == State::FadingOut) return;
    state_ = State::FadingOut;
    pressed_ = pressedMiddle_ = -1;
    StartFade(0.0f);
    Commit();
    if (!fadePending_) FinishHide();  // nothing to animate
}

void PreviewPopup::FinishHide() {
    if (fadePending_) KillTimer(hwnd_, kFadeTimerId);
    fadePending_ = false;
    state_ = State::Hidden;
    ReleaseThumbs();
    cards_.clear();
    contentData_ = Content{};
    hiddenCount_ = 0;
    hot_ = pressed_ = pressedMiddle_ = -1;
    hotClose_ = false;
    if (tracking_ && hwnd_) {
        TRACKMOUSEEVENT cancel{sizeof(cancel), TME_LEAVE | TME_CANCEL, hwnd_, 0};
        TrackMouseEvent(&cancel);
    }
    tracking_ = inside_ = false;
    if (hwnd_) {
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOSIZE);
    }
    backdrop_.Hide();
    if (renderer_ && content_.visual) {
        renderer_->DrawLayer(content_, 0, 0, {});  // frees the surface
        Commit();
    }
    fade_.valid = false;
}

bool PreviewPopup::ContainsCursor() const {
    if (state_ == State::Hidden) return false;
    POINT pt{};
    return GetCursorPos(&pt) && PtInRect(&rect_, pt);
}

int PreviewPopup::CardAt(POINT pt, bool* onClose) const {
    if (onClose) *onClose = false;
    for (size_t i = 0; i < cards_.size(); ++i) {
        if (!PtInRect(&cards_[i].rect, pt)) continue;
        if (onClose) *onClose = PtInRect(&cards_[i].closeRect, pt) != FALSE;
        return static_cast<int>(i);
    }
    return -1;
}

void PreviewPopup::SetHot(int card, bool close) {
    if (card < 0) close = false;
    if (card == hot_ && close == hotClose_) return;
    hot_ = card;
    hotClose_ = close;
    if (state_ == State::Hidden) return;
    if (SUCCEEDED(Draw())) Commit();
}

LRESULT CALLBACK PreviewPopup::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PreviewPopup* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<PreviewPopup*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PreviewPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self || !self->hwnd_ || self->hwnd_ != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(msg, wp, lp);
}

LRESULT PreviewPopup::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_MOUSEMOVE: {
            if (state_ != State::Shown) return 0;
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                tracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            if (!inside_) {
                inside_ = true;
                if (callbacks_.hover) callbacks_.hover(true);
            }
            bool close = false;
            const int card = CardAt(pt, &close);
            SetHot(card, close);
            return 0;
        }

        case WM_MOUSELEAVE:
            tracking_ = false;
            pressed_ = pressedMiddle_ = -1;
            SetHot(-1, false);
            if (inside_) {
                inside_ = false;
                if (callbacks_.hover) callbacks_.hover(false);
            }
            return 0;

        case WM_LBUTTONDOWN:
            pressed_ = state_ == State::Shown ? CardAt(pt, &pressedClose_) : -1;
            return 0;

        case WM_LBUTTONUP: {
            bool close = false;
            const int card = CardAt(pt, &close);
            const bool clicked = state_ == State::Shown && card >= 0 && card == pressed_ && close == pressedClose_;
            pressed_ = -1;
            if (!clicked) return 0;
            HWND target = cards_[static_cast<size_t>(card)].window;  // the callbacks may hide the popup
            if (close) {
                if (callbacks_.close) callbacks_.close(target);
            } else if (callbacks_.activate) {
                callbacks_.activate(target);
            }
            return 0;
        }

        case WM_MBUTTONDOWN:
            pressedMiddle_ = state_ == State::Shown ? CardAt(pt, nullptr) : -1;
            return 0;

        case WM_MBUTTONUP: {
            const int card = CardAt(pt, nullptr);
            const bool clicked = state_ == State::Shown && card >= 0 && card == pressedMiddle_;
            pressedMiddle_ = -1;
            if (clicked && callbacks_.close) callbacks_.close(cards_[static_cast<size_t>(card)].window);
            return 0;
        }

        case WM_TIMER:
            if (wp == kFadeTimerId) {
                OnFadeTimer();
                return 0;
            }
            break;

        case WM_WINDOWPOSCHANGED:
            if (!(reinterpret_cast<const WINDOWPOS*>(lp)->flags & SWP_NOZORDER)) backdrop_.Restack(hwnd_);
            break;

        case WM_DPICHANGED:
            return 0;  // the dock places and scales the popup

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            ValidateRect(hwnd_, nullptr);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
