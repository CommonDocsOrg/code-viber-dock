#include "ItemGridView.h"

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace {

// Grid cell layout (DIPs), the folder popup's look since round 3.
constexpr float kIconTop = 6.0f;    // cell top -> icon
constexpr float kTextGap = 4.0f;    // icon -> name
constexpr float kTextPadX = 4.0f;   // cell edge -> name
constexpr float kLineH = 16.0f;     // name line height (two lines)
constexpr float kCellRadius = 6.0f;
// List rows (DIPs).
constexpr float kListPadX = 8.0f;
constexpr float kListIconGap = 10.0f;
constexpr float kListSecondaryW = 150.0f;  // at most; never more than 40 % of the width
constexpr float kListSecondaryGap = 8.0f;
// Scrollbar (DIPs).
constexpr float kThumbW = 4.0f;
constexpr float kThumbInset = 2.0f;
constexpr float kThumbMinH = 24.0f;
constexpr float kScrollbarHitW = 12.0f;    // the strip at the right edge that takes thumb presses
constexpr UINT kScrollbarHoldMs = 900;     // shown this long after the last scroll
constexpr double kScrollbarFadeInS = 0.08;
constexpr double kScrollbarFadeOutS = 0.3;
constexpr ULONGLONG kTypeAheadMs = 1000;   // type-to-select: keys within this time extend the prefix

enum TimerSlot : UINT_PTR { kAnimTimer = 0, kFadeTimer = 1 };

bool StartsWith(const std::wstring& text, const std::wstring& prefix) {
    if (prefix.empty() || text.size() < prefix.size()) return false;
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC, text.c_str(),
                           static_cast<int>(prefix.size()), prefix.c_str(), static_cast<int>(prefix.size()), nullptr,
                           nullptr, 0) == CSTR_EQUAL;
}

UINT TimerMs(double seconds) { return static_cast<UINT>(std::max(1.0, std::ceil(seconds * 1000.0))); }

}  // namespace

void ItemGridView::Init(Renderer* renderer, HWND host, UINT_PTR timerBase, Callbacks callbacks) {
    renderer_ = renderer;
    host_ = host;
    timerBase_ = timerBase;
    callbacks_ = std::move(callbacks);
}

HRESULT ItemGridView::Attach(Layer& parent) {
    if (!renderer_) return E_UNEXPECTED;
    if (viewport_.visual && parent_ == &parent && viewport_.parent == parent.visual) return S_OK;
    Detach();
    HRESULT hr = renderer_->CreateLayer(viewport_, &parent);
    if (SUCCEEDED(hr)) hr = renderer_->CreateLayer(content_, &viewport_);
    if (SUCCEEDED(hr)) hr = renderer_->CreateLayer(thumb_, &viewport_);  // in front of the rows
    if (FAILED(hr)) {
        Detach();
        return hr;
    }
    parent_ = &parent;
    scrollY_.valid = thumbY_.valid = thumbOpacity_.valid = false;
    thumbH_ = 0.0f;
    FadeScrollbar(0.0f, 0.0);
    return S_OK;
}

void ItemGridView::Detach() {
    StopTimer(kAnimTimer);
    StopTimer(kFadeTimer);
    if (press_.active && host_ && GetCapture() == host_) {
        press_ = Press{};
        ReleaseCapture();
    }
    press_ = Press{};
    rows_.clear();  // their visuals go with the viewport's subtree
    if (renderer_ && viewport_.visual) renderer_->DestroyLayer(viewport_);
    viewport_.Reset();
    content_.Reset();
    thumb_.Reset();
    parent_ = nullptr;
    scrollY_.valid = thumbY_.valid = thumbOpacity_.valid = false;
    thumbShown_ = false;
    thumbH_ = 0.0f;
    hot_ = -1;
}

void ItemGridView::SetStyle(const Style& style) {
    style_ = style;
    InvalidateAll();
}

void ItemGridView::SetItems(std::vector<GridItem> items) {
    items_ = std::move(items);
    hot_ = selected_ = drop_ = -1;
    typed_.clear();
    target_ = 0.0f;
    scrollY_.valid = thumbY_.valid = false;
    for (Row& r : rows_) {
        if (renderer_) renderer_->DestroyLayer(r.layer);
    }
    rows_.clear();
    thumbH_ = 0.0f;
}

void ItemGridView::InvalidateItem(size_t index) {
    InvalidateRowOf(static_cast<int>(index));
}

void ItemGridView::SetMode(Mode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    for (Row& r : rows_) {
        if (renderer_) renderer_->DestroyLayer(r.layer);
    }
    rows_.clear();
}

void ItemGridView::SetColumns(size_t columns) {
    columns = std::max<size_t>(1, columns);
    if (columns == columns_) return;
    columns_ = columns;
    if (mode_ == Mode::Grid) {
        for (Row& r : rows_) {
            if (renderer_) renderer_->DestroyLayer(r.layer);
        }
        rows_.clear();
    }
}

void ItemGridView::SetBounds(const RECT& bounds) {
    if (EqualRect(&bounds, &bounds_)) return;
    const bool resized = bounds.right - bounds.left != bounds_.right - bounds_.left ||
                         bounds.bottom - bounds.top != bounds_.bottom - bounds_.top;
    bounds_ = bounds;
    if (resized) {
        InvalidateAll();
        thumbH_ = 0.0f;
    }
}

void ItemGridView::Refresh() {
    if (!viewport_.visual || !renderer_) return;
    renderer_->SetOffset(viewport_, static_cast<float>(bounds_.left), static_cast<float>(bounds_.top));
    renderer_->SetClip(viewport_, D2D1::RectF(0.0f, 0.0f, ViewWidth(), ViewHeight()));
    // Back inside the range (fewer entries, a taller view), statically.
    target_ = std::clamp(target_, 0.0f, static_cast<float>(MaxScroll()));
    StopTimer(kAnimTimer);
    const double now = Renderer::Now();
    scrollY_.valid = false;  // a static set
    renderer_->Drive(scrollY_, -target_, 0.0, now, [&](auto v) { return content_.visual->SetOffsetY(v); });
    TrimRows(target_, target_ + ViewHeight());
    EnsureRows(target_, target_ + ViewHeight());
    RedrawDirty();
    if (Scrollable()) {
        DrawThumb();
        float y = 0.0f, h = 0.0f;
        ThumbGeometry(target_, y, h);
        thumbY_.valid = false;
        renderer_->Drive(thumbY_, y, 0.0, now, [&](auto v) { return thumb_.visual->SetOffsetY(v); });
    } else {
        FadeScrollbar(0.0f, 0.0);
        StopTimer(kFadeTimer);
    }
}

LONG ItemGridView::RowHeight() const {
    return static_cast<LONG>(Px(mode_ == Mode::Grid ? style_.cellH : style_.rowH));
}

LONG ItemGridView::CellWidth() const {
    return mode_ == Mode::Grid ? static_cast<LONG>(Px(style_.cellW)) : bounds_.right - bounds_.left;
}

size_t ItemGridView::RowCount() const {
    const size_t cols = Columns();
    return items_.empty() ? 0 : (items_.size() + cols - 1) / cols;
}

LONG ItemGridView::MaxScroll() const {
    return std::max<LONG>(0, ContentHeight() - (bounds_.bottom - bounds_.top));
}

float ItemGridView::CurrentScroll() const {
    if (!scrollY_.valid) return target_;
    return -scrollY_.Value(Renderer::Now());
}

D2D1_RECT_F ItemGridView::ContentRect(size_t index) const {
    const float rowH = static_cast<float>(RowHeight());
    if (mode_ == Mode::List) {
        const float top = static_cast<float>(index) * rowH;
        return D2D1::RectF(0.0f, top, ViewWidth(), top + rowH);
    }
    const size_t cols = Columns();
    const LONG cellW = CellWidth();
    const LONG gridW = static_cast<LONG>(cols) * cellW;
    const LONG left = (static_cast<LONG>(ViewWidth()) - gridW) / 2;
    const float x = static_cast<float>(left + static_cast<LONG>(index % cols) * cellW);
    const float y = static_cast<float>(index / cols) * rowH;
    return D2D1::RectF(x, y, x + static_cast<float>(cellW), y + rowH);
}

RECT ItemGridView::ItemRect(size_t index) const {
    if (index >= items_.size()) return RECT{};
    const D2D1_RECT_F r = ContentRect(index);
    const float dy = static_cast<float>(bounds_.top) - CurrentScroll();
    return RECT{bounds_.left + static_cast<LONG>(r.left), static_cast<LONG>(std::lround(r.top + dy)),
                bounds_.left + static_cast<LONG>(r.right), static_cast<LONG>(std::lround(r.bottom + dy))};
}

int ItemGridView::HitTest(POINT pt) const {
    if (!Contains(pt) || items_.empty()) return -1;
    const float x = static_cast<float>(pt.x - bounds_.left);
    const float y = static_cast<float>(pt.y - bounds_.top) + CurrentScroll();
    const float rowH = static_cast<float>(RowHeight());
    if (y < 0.0f || rowH <= 0.0f) return -1;
    const size_t row = static_cast<size_t>(y / rowH);
    size_t index = row;
    if (mode_ == Mode::Grid) {
        const size_t cols = Columns();
        const LONG cellW = CellWidth();
        const LONG left = (static_cast<LONG>(ViewWidth()) - static_cast<LONG>(cols) * cellW) / 2;
        if (x < static_cast<float>(left) || cellW <= 0) return -1;
        const size_t col = static_cast<size_t>((x - static_cast<float>(left)) / static_cast<float>(cellW));
        if (col >= cols) return -1;
        index = row * cols + col;
    }
    return index < items_.size() ? static_cast<int>(index) : -1;
}

bool ItemGridView::RowRange(float top, float bottom, size_t& first, size_t& last) const {
    const size_t rows = RowCount();
    const float rowH = static_cast<float>(RowHeight());
    if (rows == 0 || rowH <= 0.0f || bottom <= top) return false;
    const float t = std::max(0.0f, top);
    first = static_cast<size_t>(t / rowH);
    last = std::min(rows - 1, static_cast<size_t>(std::max(0.0f, std::ceil(bottom / rowH) - 1.0f)));
    return first <= last && first < rows;
}

ItemGridView::Row* ItemGridView::FindRow(size_t row) {
    for (Row& r : rows_) {
        if (r.index == row) return &r;
    }
    return nullptr;
}

void ItemGridView::EnsureRows(float top, float bottom) {
    if (!content_.visual || !renderer_) return;
    size_t first = 0, last = 0;
    if (!RowRange(top, bottom, first, last)) return;
    const float rowH = static_cast<float>(RowHeight());
    for (size_t r = first; r <= last; ++r) {
        Row* row = FindRow(r);
        if (!row) {
            Row fresh;
            fresh.index = r;
            if (FAILED(renderer_->CreateLayer(fresh.layer, &content_))) return;
            renderer_->SetOffset(fresh.layer, 0.0f, static_cast<float>(r) * rowH);
            rows_.push_back(std::move(fresh));
            row = &rows_.back();
        }
        if (row->dirty) DrawRow(*row);
    }
}

void ItemGridView::TrimRows(float top, float bottom) {
    size_t first = 0, last = 0;
    const bool any = RowRange(top, bottom, first, last);
    for (size_t i = rows_.size(); i-- > 0;) {
        const size_t r = rows_[i].index;
        if (any && r >= first && r <= last) continue;
        if (renderer_) renderer_->DestroyLayer(rows_[i].layer);
        rows_.erase(rows_.begin() + static_cast<ptrdiff_t>(i));
    }
}

void ItemGridView::InvalidateRowOf(int index) {
    if (index < 0 || static_cast<size_t>(index) >= items_.size()) return;
    if (Row* r = FindRow(static_cast<size_t>(index) / Columns())) r->dirty = true;
}

void ItemGridView::InvalidateAll() {
    for (Row& r : rows_) r.dirty = true;
}

void ItemGridView::RedrawDirty() {
    for (Row& r : rows_) {
        if (r.dirty) DrawRow(r);
    }
}

void ItemGridView::EnsureFormats() {
    IDWriteFactory* dw = renderer_ ? renderer_->DWrite() : nullptr;
    const float s = style_.scale;
    if (!dw || (gridFormat_ && formatScale_ == s && formatFont_ == style_.fontDip)) return;
    gridFormat_ = listFormat_ = secondaryFormat_ = nullptr;
    gridEllipsis_ = listEllipsis_ = nullptr;
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    const float size = style_.fontDip * s;
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, size, L"", gridFormat_.put()))) {
        gridFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        gridFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        gridFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        // Fixed line height: exactly two lines fit the name box; the second one is ellipsized.
        gridFormat_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, kLineH * s, 13.0f * s);
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(gridFormat_.get(), gridEllipsis_.put()))) {
            gridFormat_->SetTrimming(&trimming, gridEllipsis_.get());
        }
    } else {
        gridFormat_ = nullptr;
    }
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, size, L"", listFormat_.put()))) {
        listFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        listFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(listFormat_.get(), listEllipsis_.put()))) {
            listFormat_->SetTrimming(&trimming, listEllipsis_.get());
        }
    } else {
        listFormat_ = nullptr;
    }
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, size - std::round(1.0f * s), L"",
                                       secondaryFormat_.put()))) {
        secondaryFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        secondaryFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        secondaryFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (listEllipsis_) secondaryFormat_->SetTrimming(&trimming, listEllipsis_.get());
    } else {
        secondaryFormat_ = nullptr;
    }
    formatScale_ = s;
    formatFont_ = style_.fontDip;
}

HRESULT ItemGridView::DrawRow(Row& row) {
    if (!renderer_ || !row.layer.visual) return E_UNEXPECTED;
    const UINT w = static_cast<UINT>(std::max(0.0f, ViewWidth()));
    const UINT h = static_cast<UINT>(RowHeight());
    const size_t cols = Columns();
    const size_t first = row.index * cols;
    const size_t end = std::min(items_.size(), first + cols);
    // Fill in what the host left for later (icons of the entries scrolled into view).
    for (size_t i = first; i < end; ++i) {
        if (!items_[i].resolved) {
            items_[i].resolved = true;
            if (callbacks_.resolve) callbacks_.resolve(i, items_[i]);
        }
    }
    EnsureFormats();
    IDWriteFactory* dw = renderer_->DWrite();
    const float s = style_.scale;
    const float top = static_cast<float>(row.index) * static_cast<float>(h);
    const HRESULT hr = renderer_->DrawLayer(row.layer, w, h, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(style_.text), brush.put()))) return;
        const float stroke = std::max(1.0f, std::round(s));
        const float inset = stroke / 2.0f;
        const float radius = std::round(kCellRadius * s);
        auto box = [&](D2D1_RECT_F f, uint32_t fill, float fillFactor, uint32_t outline, bool withOutline) {
            brush->SetColor(Theme::ToColor(fill, fillFactor));
            dc->FillRoundedRectangle(D2D1::RoundedRect(f, radius, radius), brush.get());
            if (!withOutline) return;
            brush->SetColor(Theme::ToColor(outline));
            const D2D1_RECT_F edge = D2D1::RectF(f.left + inset, f.top + inset, f.right - inset, f.bottom - inset);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(edge, radius - inset, radius - inset), brush.get(), stroke);
        };
        for (size_t i = first; i < end; ++i) {
            const GridItem& item = items_[i];
            D2D1_RECT_F cell = ContentRect(i);
            cell.top -= top;
            cell.bottom -= top;
            const int index = static_cast<int>(i);
            const bool isHot = index == hot_;
            D2D1_RECT_F hl = cell;
            if (mode_ == Mode::List) {
                hl = D2D1::RectF(cell.left + std::round(2.0f * s), cell.top + std::round(1.0f * s),
                                 cell.right - std::round(2.0f * s), cell.bottom - std::round(1.0f * s));
            }
            if (index == drop_) {
                box(hl, style_.accent, 0.25f, style_.accent, true);
            } else if (isHot) {
                box(hl, style_.hotBackground, 1.0f, style_.hotBorder, true);
            } else if (index == selected_) {
                box(hl, style_.hotBackground, 1.0f, style_.accent, true);
            }
            const bool lit = isHot || index == selected_ || index == drop_;
            if (mode_ == Mode::Grid) {
                const float iconPx = std::round(style_.iconDip * s);
                const float ix = std::round((cell.left + cell.right - iconPx) / 2.0f);
                const float iy = cell.top + std::round(kIconTop * s);
                if (item.icon) {
                    const D2D1_RECT_F dst = D2D1::RectF(ix, iy, ix + iconPx, iy + iconPx);
                    dc->DrawBitmap(item.icon.get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr,
                                   nullptr);
                }
                if (dw && gridFormat_ && !item.label.empty()) {
                    const float textW = (cell.right - cell.left) - 2.0f * std::round(kTextPadX * s);
                    const float textH = std::round(2.0f * kLineH * s);
                    winrt::com_ptr<IDWriteTextLayout> layout;
                    if (SUCCEEDED(dw->CreateTextLayout(item.label.c_str(), static_cast<UINT32>(item.label.size()),
                                                       gridFormat_.get(), textW, textH, layout.put()))) {
                        brush->SetColor(Theme::ToColor(style_.text, lit ? 1.0f : 0.9f));
                        dc->DrawTextLayout(D2D1::Point2F(cell.left + std::round(kTextPadX * s),
                                                         iy + iconPx + std::round(kTextGap * s)),
                                           layout.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
                continue;
            }
            // List: icon, name, secondary text at the right.
            const float padX = std::round(kListPadX * s);
            const float iconPx = std::round(style_.listIconDip * s);
            const float iy = std::round(cell.top + (cell.bottom - cell.top - iconPx) / 2.0f);
            if (item.icon) {
                const D2D1_RECT_F dst = D2D1::RectF(cell.left + padX, iy, cell.left + padX + iconPx, iy + iconPx);
                dc->DrawBitmap(item.icon.get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr,
                               nullptr);
            }
            const float nameX = cell.left + padX + iconPx + std::round(kListIconGap * s);
            float nameRight = cell.right - padX;
            if (!item.secondary.empty() && dw && secondaryFormat_) {
                const float secW = std::min(std::round(kListSecondaryW * s), std::round((cell.right - cell.left) * 0.4f));
                const float secLeft = cell.right - padX - secW;
                winrt::com_ptr<IDWriteTextLayout> layout;
                if (secW > 0.0f &&
                    SUCCEEDED(dw->CreateTextLayout(item.secondary.c_str(), static_cast<UINT32>(item.secondary.size()),
                                                   secondaryFormat_.get(), secW, cell.bottom - cell.top, layout.put()))) {
                    brush->SetColor(Theme::ToColor(style_.secondary));
                    dc->DrawTextLayout(D2D1::Point2F(secLeft, cell.top), layout.get(), brush.get(),
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                nameRight = secLeft - std::round(kListSecondaryGap * s);
            }
            if (dw && listFormat_ && !item.label.empty() && nameRight > nameX) {
                winrt::com_ptr<IDWriteTextLayout> layout;
                if (SUCCEEDED(dw->CreateTextLayout(item.label.c_str(), static_cast<UINT32>(item.label.size()),
                                                   listFormat_.get(), nameRight - nameX, cell.bottom - cell.top,
                                                   layout.put()))) {
                    brush->SetColor(Theme::ToColor(style_.text, lit ? 1.0f : 0.92f));
                    dc->DrawTextLayout(D2D1::Point2F(nameX, cell.top), layout.get(), brush.get(),
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
    });
    if (SUCCEEDED(hr)) row.dirty = false;
    return hr;
}

void ItemGridView::ThumbGeometry(float scroll, float& y, float& h) const {
    const float inset = Px(kThumbInset);
    const float track = std::max(1.0f, ViewHeight() - 2.0f * inset);
    const float content = static_cast<float>(std::max<LONG>(1, ContentHeight()));
    h = std::min(track, std::max(Px(kThumbMinH), std::round(track * ViewHeight() / content)));
    const float max = static_cast<float>(MaxScroll());
    y = inset + (max > 0.0f ? std::round((track - h) * std::clamp(scroll, 0.0f, max) / max) : 0.0f);
}

HRESULT ItemGridView::DrawThumb() {
    if (!thumb_.visual || !renderer_) return E_UNEXPECTED;
    float y = 0.0f, h = 0.0f;
    ThumbGeometry(target_, y, h);
    const float w = Px(kThumbW);
    renderer_->SetOffset(thumb_, ViewWidth() - w - Px(kThumbInset), y);
    thumbY_.valid = false;
    if (h == thumbH_ && thumb_.width) return S_OK;
    thumbH_ = h;
    return renderer_->DrawLayer(thumb_, static_cast<UINT>(w), static_cast<UINT>(h), [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(style_.scrollbar), brush.put()))) return;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), w / 2.0f, w / 2.0f), brush.get());
    });
}

void ItemGridView::FadeScrollbar(float to, double seconds) {
    if (!thumb_.visual || !renderer_) return;
    winrt::com_ptr<IDCompositionVisual3> v3;
    if (FAILED(thumb_.visual->QueryInterface(IID_PPV_ARGS(v3.put())))) return;
    if (style_.reduceMotion) seconds = 0.0;
    renderer_->Drive(thumbOpacity_, to, seconds, Renderer::Now(), [&](auto v) { return v3->SetOpacity(v); }, 0.001f,
                     Easing::EaseOut);
    thumbShown_ = to > 0.0f;
}

void ItemGridView::ShowScrollbar() {
    if (!Scrollable() || !thumb_.visual) return;
    if (!thumb_.width || thumbH_ <= 0.0f) {
        DrawThumb();
        float y = 0.0f, h = 0.0f;
        ThumbGeometry(CurrentScroll(), y, h);
        renderer_->Drive(thumbY_, y, 0.0, Renderer::Now(), [&](auto v) { return thumb_.visual->SetOffsetY(v); });
    }
    if (!thumbShown_) FadeScrollbar(1.0f, kScrollbarFadeInS);
    StartTimer(kFadeTimer, kScrollbarHoldMs);
}

bool ItemGridView::InScrollbar(POINT pt) const {
    return Scrollable() && Contains(pt) && pt.x >= bounds_.right - static_cast<LONG>(Px(kScrollbarHitW));
}

void ItemGridView::ScrollTo(float px, bool animate) {
    ScrollToInternal(px, animate);
}

void ItemGridView::ScrollToInternal(float px, bool animate) {
    px = std::clamp(std::round(px), 0.0f, static_cast<float>(MaxScroll()));
    if (!content_.visual || !renderer_) {
        target_ = px;
        return;
    }
    const double now = Renderer::Now();
    float current = CurrentScroll();
    const float viewH = ViewHeight();
    double duration = animate && !style_.reduceMotion ? style_.scrollSeconds : 0.0;
    if (std::fabs(px - current) < 0.5f && std::fabs(px - target_) < 0.5f) return;
    auto setY = [&](auto v) { return content_.visual->SetOffsetY(v); };
    auto setThumb = [&](auto v) { return thumb_.visual->SetOffsetY(v); };
    if (duration > 0.0 && std::fabs(px - current) > viewH) {
        // A long way (End, a far page): jump to one screen before the target, animate the rest.
        current = px - (px > current ? viewH : -viewH);
        scrollY_.valid = false;
        renderer_->Drive(scrollY_, -current, 0.0, now, setY);
        float ty = 0.0f, th = 0.0f;
        ThumbGeometry(current, ty, th);
        thumbY_.valid = false;
        renderer_->Drive(thumbY_, ty, 0.0, now, setThumb);
    }
    // The rows of both ends exist while the content moves; the ones that scroll away go when it ends.
    EnsureRows(std::min(current, px), std::max(current, px) + viewH);
    renderer_->Drive(scrollY_, -px, duration, now, setY, 0.25f, Easing::EaseOut);
    target_ = px;
    if (Scrollable()) {
        DrawThumb();  // static at the current position first (it may have been resized)
        float ty = 0.0f, th = 0.0f;
        ThumbGeometry(current, ty, th);
        renderer_->Drive(thumbY_, ty, 0.0, now, setThumb);
        ThumbGeometry(px, ty, th);
        renderer_->Drive(thumbY_, ty, duration, now, setThumb, 0.25f, Easing::EaseOut);
        ShowScrollbar();
    }
    if (duration > 0.0) {
        StartTimer(kAnimTimer, TimerMs(duration + 0.03));
    } else {
        StopTimer(kAnimTimer);
        TrimRows(px, px + viewH);
    }
    Commit();
}

void ItemGridView::Reveal(size_t index) {
    if (index >= items_.size()) return;
    const D2D1_RECT_F r = ContentRect(index);
    if (r.top < target_) {
        ScrollToInternal(r.top, true);
    } else if (r.bottom > target_ + ViewHeight()) {
        ScrollToInternal(r.bottom - ViewHeight(), true);
    }
}

void ItemGridView::Select(int index, bool reveal) {
    if (index >= static_cast<int>(items_.size())) index = static_cast<int>(items_.size()) - 1;
    if (index < -1) index = -1;
    const int old = selected_;
    selected_ = index;
    InvalidateRowOf(old);
    InvalidateRowOf(index);
    if (reveal && index >= 0) Reveal(static_cast<size_t>(index));
    RedrawDirty();
    Commit();
}

void ItemGridView::SetHot(int index) {
    if (index == hot_) return;
    const int old = hot_;
    hot_ = index;
    InvalidateRowOf(old);
    InvalidateRowOf(index);
    RedrawDirty();
    Commit();
}

void ItemGridView::SetDropHighlight(int index) {
    if (index == drop_) return;
    const int old = drop_;
    drop_ = index;
    InvalidateRowOf(old);
    InvalidateRowOf(index);
    RedrawDirty();
    Commit();
}

void ItemGridView::MouseMove(POINT pt) {
    if (!press_.active) {
        const bool inside = Contains(pt);
        if (inside && hot_ < 0 && !thumbShown_) ShowScrollbar();  // entering: show that it scrolls
        SetHot(HitTest(pt));
        if (InScrollbar(pt) && thumbShown_) StartTimer(kFadeTimer, kScrollbarHoldMs);
        return;
    }
    if (press_.button != Button::Left) return;
    const LONG dx = pt.x - press_.pt.x, dy = pt.y - press_.pt.y;
    if (press_.thumb) {
        float y = 0.0f, h = 0.0f;
        ThumbGeometry(press_.scroll, y, h);
        const float track = ViewHeight() - 2.0f * Px(kThumbInset) - h;
        if (track > 0.0f) {
            ScrollToInternal(press_.scroll + static_cast<float>(dy) * static_cast<float>(MaxScroll()) / track, false);
        }
        return;
    }
    if (press_.scrolling) {
        ScrollToInternal(press_.scroll - static_cast<float>(dy), false);
        return;
    }
    const LONG cx = GetSystemMetrics(SM_CXDRAG), cy = GetSystemMetrics(SM_CYDRAG);
    const bool moved = std::labs(dx) >= cx || std::labs(dy) >= cy;
    if (!moved) return;
    // An entry pressed in a view whose entries can be dragged out: that drag. Otherwise a vertical
    // drag scrolls the content.
    if (dragOut_ && callbacks_.beginDrag && press_.item >= 0) {
        const size_t index = static_cast<size_t>(press_.item);
        press_ = Press{};
        if (host_ && GetCapture() == host_) ReleaseCapture();
        SetHot(-1);
        POINT screen = pt;
        if (host_) ClientToScreen(host_, &screen);
        callbacks_.beginDrag(index, screen);  // may run a modal drag loop
        return;
    }
    if (Scrollable() && std::labs(dy) >= std::labs(dx)) {
        press_.scrolling = true;
        SetHot(-1);
        ScrollToInternal(press_.scroll - static_cast<float>(dy), false);
    }
}

void ItemGridView::MouseLeave() {
    if (!press_.active) SetHot(-1);
}

bool ItemGridView::ButtonDown(Button button, POINT pt) {
    if (!Contains(pt)) return false;
    press_ = Press{};
    press_.active = true;
    press_.button = button;
    press_.pt = pt;
    press_.item = HitTest(pt);
    press_.scroll = target_;
    if (button == Button::Left && InScrollbar(pt)) {
        float y = 0.0f, h = 0.0f;
        ThumbGeometry(CurrentScroll(), y, h);
        const LONG top = bounds_.top + static_cast<LONG>(y);
        if (pt.y >= top && pt.y < top + static_cast<LONG>(h)) {
            press_.thumb = true;
            press_.item = -1;
        } else {
            // Page toward the click.
            const float page = std::max(1.0f, ViewHeight() - static_cast<float>(RowHeight()));
            ScrollToInternal(target_ + (pt.y < top ? -page : page), true);
            press_ = Press{};
            return true;
        }
    }
    if (host_) SetCapture(host_);
    return true;
}

bool ItemGridView::ButtonUp(Button button, POINT pt) {
    if (!press_.active || press_.button != button) return false;
    const Press p = press_;
    press_ = Press{};
    if (host_ && GetCapture() == host_) ReleaseCapture();
    if (p.scrolling || p.thumb) {
        SetHot(HitTest(pt));
        return true;
    }
    const int index = HitTest(pt);
    if (index != p.item) return true;
    POINT screen = pt;
    if (host_) ClientToScreen(host_, &screen);
    if (button == Button::Right) {
        if (index >= 0) Select(index, false);
        if (callbacks_.contextMenu) callbacks_.contextMenu(index, screen);
        return true;
    }
    if (index < 0) return true;
    if (button == Button::Left) Select(index, false);
    if (callbacks_.activate) callbacks_.activate(static_cast<size_t>(index), button);  // may replace the items
    return true;
}

void ItemGridView::CaptureLost() {
    if (!press_.active) return;
    press_ = Press{};
}

bool ItemGridView::Wheel(int delta, POINT) {
    if (!Scrollable() || delta == 0) return false;
    const float step = static_cast<float>(RowHeight()) * (mode_ == Mode::List ? 3.0f : 1.0f);
    ScrollToInternal(target_ - static_cast<float>(delta) * step / static_cast<float>(WHEEL_DELTA), true);
    return true;
}

bool ItemGridView::KeyDown(UINT vk) {
    const int n = static_cast<int>(items_.size());
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    if (vk == VK_APPS || (vk == VK_F10 && shift)) {
        if (!callbacks_.contextMenu) return false;
        POINT screen{bounds_.left + 8, bounds_.top + 8};
        if (selected_ >= 0) {
            const RECT r = ItemRect(static_cast<size_t>(selected_));
            screen = POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        }
        if (host_) ClientToScreen(host_, &screen);
        callbacks_.contextMenu(selected_, screen);
        return true;
    }
    if (n == 0) return false;
    const int cols = static_cast<int>(Columns());
    const int pageRows = std::max(1, static_cast<int>(ViewHeight() / std::max<LONG>(1, RowHeight())));
    const int cur = selected_;
    int next = cur;
    switch (vk) {
        case VK_RETURN:
            if (cur >= 0 && callbacks_.activate) callbacks_.activate(static_cast<size_t>(cur), Button::Left);
            return true;
        case VK_LEFT:
            if (mode_ == Mode::List) return true;
            next = cur < 0 ? n - 1 : std::max(0, cur - 1);
            break;
        case VK_RIGHT:
            if (mode_ == Mode::List) return true;
            next = cur < 0 ? 0 : std::min(n - 1, cur + 1);
            break;
        case VK_UP:
            next = cur < 0 ? n - 1 : (cur - cols >= 0 ? cur - cols : cur);
            break;
        case VK_DOWN:
            next = cur < 0 ? 0 : (cur + cols < n ? cur + cols : cur);
            break;
        case VK_PRIOR:
            next = cur < 0 ? 0 : std::max(cur % cols, cur - cols * pageRows);
            break;
        case VK_NEXT:
            next = cur < 0 ? 0 : std::min(n - 1, cur + cols * pageRows);
            break;
        case VK_HOME:
            next = 0;
            break;
        case VK_END:
            next = n - 1;
            break;
        default:
            return false;
    }
    Select(next, true);
    return true;
}

bool ItemGridView::Char(wchar_t ch) {
    if (ch < 0x20 || ch == 0x7F || items_.empty()) return false;
    const ULONGLONG now = GetTickCount64();
    if (now - typedAt_ > kTypeAheadMs) typed_.clear();
    typedAt_ = now;
    typed_ += ch;
    // The same key again and again cycles through the entries starting with it; a longer prefix
    // keeps the current entry while it still matches.
    const bool repeat = std::all_of(typed_.begin(), typed_.end(), [&](wchar_t c) { return c == typed_[0]; });
    const std::wstring prefix = repeat ? typed_.substr(0, 1) : typed_;
    const size_t n = items_.size();
    const size_t start = selected_ < 0 ? 0 : static_cast<size_t>(selected_) + (repeat || typed_.size() == 1 ? 1 : 0);
    for (size_t k = 0; k < n; ++k) {
        const size_t i = (start + k) % n;
        if (StartsWith(items_[i].label, prefix)) {
            Select(static_cast<int>(i), true);
            return true;
        }
    }
    return true;
}

bool ItemGridView::Timer(UINT_PTR id) {
    if (id == timerBase_ + kAnimTimer) {
        StopTimer(kAnimTimer);
        TrimRows(target_, target_ + ViewHeight());
        Commit();
        return true;
    }
    if (id == timerBase_ + kFadeTimer) {
        StopTimer(kFadeTimer);
        POINT cursor{};
        bool overBar = false;
        if (host_ && GetCursorPos(&cursor) && ScreenToClient(host_, &cursor)) overBar = InScrollbar(cursor);
        if (press_.thumb || press_.scrolling || overBar) {
            StartTimer(kFadeTimer, kScrollbarHoldMs);  // still in use
            return true;
        }
        FadeScrollbar(0.0f, kScrollbarFadeOutS);
        Commit();
        return true;
    }
    return false;
}

void ItemGridView::Commit() {
    if (renderer_) renderer_->Commit();
}

void ItemGridView::StartTimer(UINT_PTR which, UINT ms) {
    if (!host_) return;
    SetTimer(host_, timerBase_ + which, ms, nullptr);
    (which == kAnimTimer ? animTimer_ : fadeTimer_) = true;
}

void ItemGridView::StopTimer(UINT_PTR which) {
    bool& flag = which == kAnimTimer ? animTimer_ : fadeTimer_;
    if (flag && host_) KillTimer(host_, timerBase_ + which);
    flag = false;
}
