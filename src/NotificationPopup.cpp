#include "NotificationPopup.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace {

// Layout (DIPs).
constexpr float kWidth = 380.0f;
constexpr float kMinWidth = 260.0f;
constexpr float kPad = 10.0f;
constexpr float kHeaderH = 30.0f;
constexpr float kHeaderGap = 6.0f;
constexpr float kButtonH = 26.0f;
constexpr float kButtonPadX = 10.0f;
constexpr float kRowH = 68.0f;
constexpr float kIconDip = 28.0f;
constexpr float kMessageH = 64.0f;
constexpr size_t kMaxRows = 6;        // visible at once; the list scrolls past them
constexpr float kMonitorMargin = 8.0f;
constexpr float kRadius = 6.0f;
constexpr double kScrollSeconds = 0.16;

LONG Px(float dip, float s) { return static_cast<LONG>(std::lround(dip * s)); }
D2D1_RECT_F ToRectF(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                       static_cast<float>(r.bottom));
}

float TextWidth(IDWriteFactory* dw, IDWriteTextFormat* f, const std::wstring& text) {
    if (!dw || !f || text.empty()) return 0.0f;
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), f, 4000.0f, 100.0f, layout.put()))) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    return SUCCEEDED(layout->GetMetrics(&m)) ? m.widthIncludingTrailingWhitespace : 0.0f;
}

constexpr wchar_t kClearText[] = L"Clear all";
constexpr wchar_t kSettingsText[] = L"Open Windows settings";

}  // namespace

void NotificationPopup::Init(HINSTANCE instance, Renderer* renderer, Callbacks callbacks) {
    PopupWindow::Init(instance, renderer);
    callbacks_ = std::move(callbacks);
    SetClosedCallback([this] {
        if (callbacks_.closed) callbacks_.closed();
    });
}

bool NotificationPopup::Show(const Style& style, const PopupAnchor& anchor, const NotificationService& service) {
    style_ = style;
    frame_ = Frame{};
    frame_.scale = style.scale;
    frame_.acrylic = style.acrylic;
    frame_.cornerRadiusDip = style.cornerRadiusDip;
    frame_.monitor = style.monitor;
    anchor_ = anchor;
    Fill(service);
    if (!ShowAt(frame_, anchor_)) return false;
    view_.ScrollTo(0.0f, false);
    return true;
}

void NotificationPopup::Update(const NotificationService& service) {
    if (!Visible()) return;  // hidden or fading out: ShowAt would show it again
    const LONG scroll = view_.ScrollOffset();
    Fill(service);
    if (ShowAt(frame_, anchor_)) view_.ScrollTo(static_cast<float>(scroll), false);
}

std::wstring NotificationPopup::RelativeTime(int64_t unixMs) {
    if (unixMs <= 0) return {};
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const int64_t now = (((static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000LL) / 10000;
    const int64_t s = std::max<int64_t>(0, (now - unixMs) / 1000);
    if (s < 60) return L"now";
    if (s < 3600) return std::to_wstring(s / 60) + L" min";
    if (s < 86400) return std::to_wstring(s / 3600) + L" h";
    if (s < 2 * 86400) return L"yesterday";
    // An older one: its date (the locale's short "month day").
    const int64_t ticks = unixMs * 10000 + 116444736000000000LL;
    FILETIME utc{static_cast<DWORD>(ticks & 0xFFFFFFFF), static_cast<DWORD>(ticks >> 32)};
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return {};
    wchar_t buf[64];
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_MONTHDAY, &st, nullptr, buf, static_cast<int>(std::size(buf)), nullptr)) {
        return buf;
    }
    return std::to_wstring(st.wMonth) + L"/" + std::to_wstring(st.wDay);
}

void NotificationPopup::Fill(const NotificationService& service) {
    items_ = service.Items();
    unread_ = service.Unread();
    ids_.clear();
    std::vector<GridItem> rows;
    rows.reserve(items_.size());
    for (const NotificationItem& n : items_) {
        GridItem g;
        g.label = n.title.empty() ? (n.app.empty() ? std::wstring(L"Notification") : n.app) : n.title;
        g.detail = n.body;
        if (g.detail.empty() && !n.title.empty() && !n.app.empty()) g.detail = n.app;
        g.secondary = RelativeTime(n.time);
        g.id = n.id;
        rows.push_back(std::move(g));
        ids_.push_back(n.id);
    }
    view_.SetItems(std::move(rows));
    const bool denied = service.GetState() == NotificationService::State::Denied;
    message_ = !ids_.empty() ? 0 : denied ? 2 : 1;
    reason_ = denied ? service.StateReason() : std::wstring();
}

ItemGridView::Style NotificationPopup::ViewStyle() const {
    ItemGridView::Style v;
    v.scale = style_.scale;
    v.text = style_.text;
    v.secondary = style_.secondary;
    v.hotBackground = style_.hotBackground;
    v.hotBorder = style_.hotBorder;
    v.accent = style_.accent;
    v.scrollbar = (style_.text & 0x00FFFFFF) | 0x80000000;
    v.rowH = kRowH;
    v.listIconDip = kIconDip;
    v.fontDip = 12.0f;
    v.scrollSeconds = kScrollSeconds;
    v.reduceMotion = style_.reduceMotion;
    v.detailRows = true;
    v.closeButton = true;
    return v;
}

void NotificationPopup::EnsureFormats() {
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    const float s = style_.scale;
    if (!dw || (titleFormat_ && formatScale_ == s)) return;
    titleFormat_ = buttonFormat_ = messageFormat_ = nullptr;
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, 14.0f * s, L"", titleFormat_.put()))) {
        titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
        titleFormat_ = nullptr;
    }
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, 12.0f * s, L"", buttonFormat_.put()))) {
        buttonFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        buttonFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        buttonFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
        buttonFormat_ = nullptr;
    }
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, 12.0f * s, L"", messageFormat_.put()))) {
        messageFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        messageFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        messageFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
        messageFormat_ = nullptr;
    }
    clearTextW_ = TextWidth(dw, buttonFormat_.get(), kClearText);
    settingsTextW_ = TextWidth(dw, buttonFormat_.get(), kSettingsText);
    formatScale_ = s;
}

SIZE NotificationPopup::Measure() {
    EnsureFormats();
    if (!viewReady_ && Window()) {
        ItemGridView::Callbacks cb;
        cb.resolve = [this](size_t i, GridItem& item) {
            if (i >= items_.size() || !callbacks_.icon) return;
            const UINT px = static_cast<UINT>(Px(kIconDip, style_.scale));
            if (ID2D1Bitmap* b = callbacks_.icon(items_[i], px)) item.icon.copy_from(b);
        };
        cb.activate = [this](size_t i, ItemGridView::Button b) {
            if (b == ItemGridView::Button::Left) ActivateRow(i);
        };
        cb.close = [this](size_t i) { DismissRow(i); };
        view_.Init(Render(), Window(), kViewTimerBase, std::move(cb));
        viewReady_ = true;
    }
    if (!view_.Attached()) view_.Attach(ContentLayer());
    view_.SetStyle(ViewStyle());
    view_.SetMode(ItemGridView::Mode::List);

    const float s = style_.scale;
    const LONG pad = Px(kPad, s), headerH = Px(kHeaderH, s), rowH = Px(kRowH, s), margin = Px(kMonitorMargin, s);
    const RECT& m = style_.monitor;
    LONG roomW = (m.right - m.left) - 2 * margin;
    LONG roomH = (anchor_.side - m.top) - margin;
    switch (anchor_.edge) {
        case DockEdge::Top: roomH = (m.bottom - anchor_.side) - margin; break;
        case DockEdge::Left:
            roomW = (m.right - anchor_.side) - margin;
            roomH = (m.bottom - m.top) - 2 * margin;
            break;
        case DockEdge::Right:
            roomW = (anchor_.side - m.left) - margin;
            roomH = (m.bottom - m.top) - 2 * margin;
            break;
        case DockEdge::Bottom:
        default: break;
    }
    const LONG w = std::max(Px(kMinWidth, s), std::min(Px(kWidth, s), roomW));
    LONG y = pad;
    headerRect_ = RECT{pad, y, w - pad, y + headerH};
    const LONG buttonW = static_cast<LONG>(std::ceil(clearTextW_)) + 2 * Px(kButtonPadX, s);
    const LONG buttonH = Px(kButtonH, s);
    clearRect_ = ids_.empty() ? RECT{}
                              : RECT{w - pad - buttonW, y + (headerH - buttonH) / 2, w - pad, y + (headerH - buttonH) / 2 + buttonH};
    y += headerH + Px(kHeaderGap, s);
    settingsRect_ = RECT{};
    if (ids_.empty()) {
        viewRect_ = RECT{};
        const LONG mh = Px(kMessageH, s) + (message_ == 2 ? buttonH + Px(8.0f, s) : 0);
        messageRect_ = RECT{pad, y, w - pad, y + Px(kMessageH, s)};
        if (message_ == 2) {
            const LONG sw = static_cast<LONG>(std::ceil(settingsTextW_)) + 2 * Px(kButtonPadX, s);
            const LONG sx = (w - sw) / 2, sy = messageRect_.bottom;
            settingsRect_ = RECT{sx, sy, sx + sw, sy + buttonH};
        }
        y += mh;
    } else {
        messageRect_ = RECT{};
        const LONG availH = roomH - 2 * pad - headerH - Px(kHeaderGap, s);
        const size_t fit = static_cast<size_t>(std::max<LONG>(1, availH / std::max<LONG>(1, rowH)));
        const size_t visible = std::min({ids_.size(), kMaxRows, fit});
        viewRect_ = RECT{pad / 2, y, w - pad / 2, y + static_cast<LONG>(visible) * rowH};
        y = viewRect_.bottom;
    }
    y += pad;
    view_.SetBounds(viewRect_);
    view_.Refresh();
    if (Hot() > kHitSettings) ClearHot();
    return SIZE{w, y};
}

void NotificationPopup::Paint(ID2D1DeviceContext* dc) {
    EnsureFormats();
    const float s = style_.scale;
    const float stroke = std::max(1.0f, std::round(s));
    const float inset = stroke / 2.0f;
    const float radius = std::round(kRadius * s);
    PaintPanel(dc, style_.tint, style_.border);
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(style_.text), brush.put()))) return;
    // Header: the title and the unread count.
    if (titleFormat_) {
        std::wstring title = L"Notifications";
        if (unread_ > 0) title += L"  (" + std::to_wstring(unread_) + L")";
        const D2D1_RECT_F r = ToRectF(headerRect_);
        dc->DrawText(title.c_str(), static_cast<UINT32>(title.size()), titleFormat_.get(),
                     D2D1::RectF(r.left + std::round(4.0f * s), r.top, clearRect_.left ? static_cast<float>(clearRect_.left) : r.right, r.bottom),
                     brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    auto button = [&](const RECT& rc, const wchar_t* text, bool hot) {
        if (rc.right <= rc.left) return;
        const D2D1_RECT_F f = ToRectF(rc);
        brush->SetColor(Theme::ToColor(hot ? style_.hotBorder : style_.hotBackground));
        dc->FillRoundedRectangle(D2D1::RoundedRect(f, radius, radius), brush.get());
        brush->SetColor(Theme::ToColor(hot ? style_.hotBorder : style_.border));
        const D2D1_RECT_F e = D2D1::RectF(f.left + inset, f.top + inset, f.right - inset, f.bottom - inset);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(e, radius - inset, radius - inset), brush.get(), stroke);
        brush->SetColor(Theme::ToColor(style_.text));
        if (buttonFormat_) dc->DrawText(text, static_cast<UINT32>(wcslen(text)), buttonFormat_.get(), f, brush.get());
    };
    button(clearRect_, kClearText, Hot() == kHitClear);
    if (!ids_.empty()) return;
    // Nothing listed: why.
    if (messageFormat_) {
        const std::wstring text = message_ == 2 ? (reason_.empty() ? std::wstring(L"Access to notifications is denied.") : reason_)
                                                : std::wstring(L"No notifications");
        brush->SetColor(Theme::ToColor(style_.secondary));
        dc->DrawText(text.c_str(), static_cast<UINT32>(text.size()), messageFormat_.get(), ToRectF(messageRect_), brush.get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    button(settingsRect_, kSettingsText, Hot() == kHitSettings);
}

int NotificationPopup::HitAt(POINT pt, int* part) const {
    if (part) *part = 0;
    if (PtInRect(&clearRect_, pt)) return kHitClear;
    if (PtInRect(&settingsRect_, pt)) return kHitSettings;
    return -1;
}

void NotificationPopup::OnClick(int index, int, Button button) {
    if (button != Button::Left) return;
    if (index == kHitClear) {
        ClearAll();
    } else if (index == kHitSettings && callbacks_.openSettings) {
        callbacks_.openSettings();
    }
}

bool NotificationPopup::DismissRow(size_t row) {
    if (!OnScreen() || row >= ids_.size()) return false;
    const uint32_t id = ids_[row];
    if (callbacks_.dismiss) callbacks_.dismiss(id);  // the service changes: Update follows
    return true;
}

bool NotificationPopup::ActivateRow(size_t row) {
    if (!OnScreen() || row >= ids_.size()) return false;
    const uint32_t id = ids_[row];
    if (callbacks_.activate) callbacks_.activate(id);  // may hide the popup
    return true;
}

bool NotificationPopup::ClearAll() {
    if (!OnScreen() || ids_.empty()) return false;
    if (callbacks_.clearAll) callbacks_.clearAll();
    return true;
}

void NotificationPopup::OnHidden() {
    view_.Detach();
    view_.SetItems({});
    items_.clear();
    ids_.clear();
}

void NotificationPopup::OnDestroying() {
    view_.Detach();
    view_.SetItems({});
    viewReady_ = false;
    items_.clear();
    ids_.clear();
    titleFormat_ = buttonFormat_ = messageFormat_ = nullptr;
    formatScale_ = 0.0f;
}

bool NotificationPopup::OnMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    result = 0;
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_DELETE) {
                const int sel = view_.Selected() >= 0 ? view_.Selected() : view_.Hot();
                if (sel >= 0) DismissRow(static_cast<size_t>(sel));
                return true;
            }
            return view_.KeyDown(static_cast<UINT>(wp));
        case WM_CHAR:
            return view_.Char(static_cast<wchar_t>(wp));
        case WM_MOUSEMOVE:
            view_.MouseMove(pt);
            return false;  // the base tracks the header button and the leave
        case WM_MOUSELEAVE:
            view_.MouseLeave();
            return false;
        case WM_LBUTTONDOWN:
            return view_.ButtonDown(ItemGridView::Button::Left, pt);
        case WM_LBUTTONUP:
            return view_.ButtonUp(ItemGridView::Button::Left, pt);
        case WM_RBUTTONDOWN:
            return view_.ButtonDown(ItemGridView::Button::Right, pt);
        case WM_RBUTTONUP:
            return view_.ButtonUp(ItemGridView::Button::Right, pt);
        case WM_MOUSEWHEEL: {
            POINT client = pt;  // screen coordinates
            if (Window()) ScreenToClient(Window(), &client);
            view_.Wheel(GET_WHEEL_DELTA_WPARAM(wp), client);
            return true;
        }
        case WM_CAPTURECHANGED:
            view_.CaptureLost();
            return false;
        case WM_TIMER:
            return view_.Timer(wp);
        default:
            return false;
    }
}
