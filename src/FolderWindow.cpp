#include "FolderWindow.h"

#include <commctrl.h>
#include <commoncontrols.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "DesktopLayer.h"
#include "Theme.h"

namespace {

using ShellFiles::Join;
using ShellFiles::LastComponent;
using ShellFiles::Parent;
using ShellFiles::SamePath;

// Layout (DIPs).
constexpr float kTitleH = 30.0f;
constexpr float kPad = 6.0f;          // panel edge -> view
constexpr float kTitlePadX = 12.0f;
constexpr float kChevronW = 26.0f;
constexpr float kGrabEdge = 6.0f;     // resize band inside the edge
constexpr float kGrabCorner = 14.0f;
constexpr float kSnapDip = 8.0f;
constexpr float kMinW = 160.0f;
constexpr float kMinH = 80.0f;
constexpr float kRadius = 8.0f;
constexpr float kTitleFont = 13.0f;
constexpr float kMessageFont = 12.0f;
constexpr float kLabelLines = 2.0f;
constexpr float kLineH = 16.0f;       // ItemGridView's name line
constexpr size_t kAutoGridMax = 48;   // auto view: a grid up to this many entries (as the folder popup)
constexpr size_t kMaxEntries = 20000;
constexpr size_t kMaxOpen = 16;       // Enter / double click on a selection opens at most this many
constexpr UINT kRefreshMs = 250;
constexpr UINT kEditMessage = WM_APP + 13;  // posted by the rename box: wParam 1 commit, 0 cancel
constexpr UINT kCmdFirst = 1, kCmdLast = 0x7FFF;

D2D1_RECT_F ToRectF(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                       static_cast<float>(r.bottom));
}

bool Empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

uint32_t Fnv(const std::wstring& s) {
    uint32_t h = 2166136261u;
    for (wchar_t c : s) {
        h ^= static_cast<uint32_t>(c);
        h *= 16777619u;
    }
    return h;
}

struct Pidl {
    PIDLIST_ABSOLUTE p = nullptr;
    Pidl() = default;
    Pidl(const Pidl&) = delete;
    Pidl& operator=(const Pidl&) = delete;
    Pidl(Pidl&& o) noexcept : p(o.p) { o.p = nullptr; }
    ~Pidl() {
        if (p) CoTaskMemFree(p);
    }
};

}  // namespace

// --- lifetime ------------------------------------------------------------------------------------

HRESULT FolderWindow::Create(HINSTANCE instance, Config* config, size_t cfg, HMONITOR monitor, Host host) {
    instance_ = instance;
    config_ = config;
    cfg_ = cfg;
    monitor_ = monitor;
    host_ = std::move(host);
    if (!host_.renderer || !config_ || cfg_ >= config_->docks.size()) return E_INVALIDARG;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &FolderWindow::WndProcThunk;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return HRESULT_FROM_WIN32(GetLastError());

    UINT dx = 96, dy = 96;
    if (monitor_ && SUCCEEDED(GetDpiForMonitor(monitor_, MDT_EFFECTIVE_DPI, &dx, &dy))) dpi_ = dx;
    scale_ = static_cast<float>(dpi_) / 96.0f;
    applied_ = Cfg();
    folder_ = FolderPath();
    rect_ = RectFromConfig();
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kClassName, TitleText().c_str(), WS_POPUP,
                            rect_.left, rect_.top, rect_.right - rect_.left, rect_.bottom - rect_.top, nullptr, nullptr,
                            instance_, this);
    if (!hwnd_) return HRESULT_FROM_WIN32(GetLastError());
    if (const UINT d = GetDpiForWindow(hwnd_); d && d != dpi_) {
        dpi_ = d;
        scale_ = static_cast<float>(dpi_) / 96.0f;
        rect_ = RectFromConfig();
        SetWindowPos(hwnd_, nullptr, rect_.left, rect_.top, rect_.right - rect_.left, rect_.bottom - rect_.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    const BOOL off = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));

    const HRESULT hr = EnsureTarget();
    if (FAILED(hr)) {
        Destroy();
        return hr;
    }
    Load();
    // Placed at the bottom before it is shown (no flash above the other windows).
    DesktopLayer::Register(hwnd_, nullptr);
    UpdateBackdrop();
    Relayout();
    Watch();
    RegisterDrop();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    DesktopLayer::Place(hwnd_);
    return S_OK;
}

void FolderWindow::Destroy() {
    CancelRename();
    Unwatch();
    if (dropTarget_) {
        if (hwnd_) RevokeDragDrop(hwnd_);
        dropTarget_->Detach();
        dropTarget_->Release();
        dropTarget_ = nullptr;
    }
    if (refreshPending_ && hwnd_) KillTimer(hwnd_, kRefreshTimer);
    refreshPending_ = false;
    if (hwnd_) DesktopLayer::Unregister(hwnd_);
    view_.Detach();
    viewReady_ = false;
    panel_.Reset();
    root_.Reset();
    if (target_) target_->SetRoot(nullptr);
    target_ = nullptr;
    iconByIndex_.clear();
    icons_.Shutdown();
    wic_ = nullptr;
    backdrop_.Destroy();
    acrylic_ = false;
    titleFormat_ = messageFormat_ = nullptr;
    titleEllipsis_ = nullptr;
    formatScale_ = 0.0f;
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
    if (editFont_) DeleteObject(editFont_);
    editFont_ = nullptr;
}

void FolderWindow::ReleaseDevice() {
    view_.Detach();
    viewReady_ = false;
    panel_.Reset();
    root_.Reset();
    if (target_) target_->SetRoot(nullptr);
    target_ = nullptr;
    iconByIndex_.clear();
    icons_.Clear();
    // The entries keep their place; their bitmaps were on the old device.
    for (size_t i = 0; i < view_.Count(); ++i) {
        view_.At(i).icon = nullptr;
        view_.At(i).resolved = false;
    }
}

HRESULT FolderWindow::RestoreDevice() {
    if (!hwnd_) return E_UNEXPECTED;
    const HRESULT hr = EnsureTarget();
    if (SUCCEEDED(hr)) Relayout();
    return hr;
}

HRESULT FolderWindow::EnsureTarget() {
    if (!target_) {
        HRESULT hr = host_.renderer->CreateWindowTarget(hwnd_, target_, root_);
        if (SUCCEEDED(hr)) hr = host_.renderer->CreateLayer(panel_, &root_);
        if (FAILED(hr)) {
            target_ = nullptr;
            root_.Reset();
            panel_.Reset();
            return hr;
        }
    }
    if (!viewReady_) {
        view_.Init(host_.renderer, hwnd_, kViewTimerBase,
                   ItemGridView::Callbacks{
                       [this](size_t i, GridItem& item) { ResolveItem(i, item); },
                       [this](size_t i, ItemGridView::Button b) {
                           if (b == ItemGridView::Button::Left) Activate(i);
                       },
                       [this](int i, POINT screen) { ContextMenu(i, screen); },
                       [this](size_t i, POINT screen) { BeginDrag(i, screen); },
                   });
        view_.SetMultiSelect(true);
        view_.SetDragOut(true);
        viewReady_ = true;
    }
    if (!Cfg().folder.collapsed && !view_.Attached()) view_.Attach(root_);
    return S_OK;
}

// --- placement -----------------------------------------------------------------------------------

RECT FolderWindow::WorkArea() const {
    MONITORINFO mi{sizeof(mi)};
    if (monitor_ && GetMonitorInfoW(monitor_, &mi)) return mi.rcWork;
    if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi)) return mi.rcWork;
    return RECT{0, 0, 1024, 768};
}

RECT FolderWindow::RectFromConfig() const {
    const DockConfig& d = Cfg();
    const RECT work = WorkArea();
    const LONG workW = work.right - work.left, workH = work.bottom - work.top;
    const LONG w = std::min<LONG>(static_cast<LONG>(Px(static_cast<float>(d.folder.w))), std::max<LONG>(1, workW));
    const LONG titleH = static_cast<LONG>(Px(kTitleH));
    const LONG h = d.folder.collapsed ? titleH
                                      : std::min<LONG>(static_cast<LONG>(Px(static_cast<float>(d.folder.h))), std::max<LONG>(titleH, workH));
    LONG x = d.x == DockConfig::kCentred ? work.left + (workW - w) / 2 : work.left + static_cast<LONG>(Px(static_cast<float>(d.x)));
    LONG y = d.y == DockConfig::kCentred ? work.top + (workH - h) / 2 : work.top + static_cast<LONG>(Px(static_cast<float>(d.y)));
    // Kept reachable: the title bar on the work area.
    const LONG keep = static_cast<LONG>(Px(48.0f));
    x = std::clamp(x, work.left - w + keep, std::max(work.left - w + keep, work.right - keep));
    y = std::clamp(y, work.top, std::max(work.top, work.bottom - titleH));
    return RECT{x, y, x + w, y + h};
}

void FolderWindow::StoreRect() {
    DockConfig& d = Cfg();
    const RECT work = WorkArea();
    d.x = static_cast<int>(std::lround(static_cast<float>(rect_.left - work.left) / scale_));
    d.y = static_cast<int>(std::lround(static_cast<float>(rect_.top - work.top) / scale_));
    d.folder.w = std::max(static_cast<int>(kMinW), static_cast<int>(std::lround(static_cast<float>(rect_.right - rect_.left) / scale_)));
    if (!d.folder.collapsed) {
        d.folder.h = std::max(static_cast<int>(kMinH), static_cast<int>(std::lround(static_cast<float>(rect_.bottom - rect_.top) / scale_)));
    }
    applied_ = d;
}

void FolderWindow::Place(const RECT& r, bool relayout) {
    const bool resized = r.right - r.left != rect_.right - rect_.left || r.bottom - r.top != rect_.bottom - rect_.top;
    rect_ = r;
    if (!hwnd_) return;
    SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (acrylic_) backdrop_.Place(hwnd_, rect_);
    if (resized || relayout) Relayout();
}

void FolderWindow::SetMonitor(HMONITOR monitor) {
    if (!hwnd_) return;
    if (monitor != monitor_) {
        monitor_ = monitor;
        UINT dx = 96, dy = 96;
        if (monitor_ && SUCCEEDED(GetDpiForMonitor(monitor_, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx != dpi_) {
            dpi_ = dx;
            scale_ = static_cast<float>(dpi_) / 96.0f;
            formatScale_ = 0.0f;
        }
    }
    const RECT r = RectFromConfig();
    if (!EqualRect(&r, &rect_)) Place(r, false);
}

FolderWindow::Grab FolderWindow::GrabAt(POINT pt) const {
    const LONG w = rect_.right - rect_.left, h = rect_.bottom - rect_.top;
    const LONG e = static_cast<LONG>(Px(kGrabEdge)), c = static_cast<LONG>(Px(kGrabCorner));
    const bool collapsed = Cfg().folder.collapsed;
    const bool left = pt.x < e, right = pt.x >= w - e;
    const bool top = !collapsed && pt.y < e, bottom = !collapsed && pt.y >= h - e;
    const bool nearL = pt.x < c, nearR = pt.x >= w - c, nearT = pt.y < c, nearB = pt.y >= h - c;
    if (!collapsed) {
        if ((top && nearL) || (left && nearT)) return Grab::TopLeft;
        if ((top && nearR) || (right && nearT)) return Grab::TopRight;
        if ((bottom && nearL) || (left && nearB)) return Grab::BottomLeft;
        if ((bottom && nearR) || (right && nearB)) return Grab::BottomRight;
        if (top) return Grab::Top;
        if (bottom) return Grab::Bottom;
    }
    if (left) return Grab::Left;
    if (right) return Grab::Right;
    if (PtInRect(&titleRect_, pt)) return Grab::Move;
    return Grab::None;
}

void FolderWindow::SnapMove(RECT& r) const {
    const RECT work = WorkArea();
    const LONG thr = static_cast<LONG>(Px(kSnapDip));
    std::vector<LONG> xs{work.left, work.right}, ys{work.top, work.bottom};
    if (host_.neighbours) {
        for (const RECT& n : host_.neighbours(this)) {
            xs.push_back(n.left);
            xs.push_back(n.right);
            ys.push_back(n.top);
            ys.push_back(n.bottom);
        }
    }
    auto best = [&](LONG a, LONG b, const std::vector<LONG>& cs) {
        LONG shift = 0, dist = thr + 1;
        for (LONG c : cs) {
            if (std::labs(c - a) < dist) {
                dist = std::labs(c - a);
                shift = c - a;
            }
            if (std::labs(c - b) < dist) {
                dist = std::labs(c - b);
                shift = c - b;
            }
        }
        return dist <= thr ? shift : 0;
    };
    const LONG dx = best(r.left, r.right, xs), dy = best(r.top, r.bottom, ys);
    OffsetRect(&r, dx, dy);
}

void FolderWindow::SnapEdges(RECT& r, Grab g) const {
    const RECT work = WorkArea();
    const LONG thr = static_cast<LONG>(Px(kSnapDip));
    std::vector<LONG> xs{work.left, work.right}, ys{work.top, work.bottom};
    if (host_.neighbours) {
        for (const RECT& n : host_.neighbours(this)) {
            xs.push_back(n.left);
            xs.push_back(n.right);
            ys.push_back(n.top);
            ys.push_back(n.bottom);
        }
    }
    auto snap = [&](LONG& v, const std::vector<LONG>& cs) {
        LONG dist = thr + 1, to = v;
        for (LONG c : cs) {
            if (std::labs(c - v) < dist) {
                dist = std::labs(c - v);
                to = c;
            }
        }
        if (dist <= thr) v = to;
    };
    if (g == Grab::Left || g == Grab::TopLeft || g == Grab::BottomLeft) snap(r.left, xs);
    if (g == Grab::Right || g == Grab::TopRight || g == Grab::BottomRight) snap(r.right, xs);
    if (g == Grab::Top || g == Grab::TopLeft || g == Grab::TopRight) snap(r.top, ys);
    if (g == Grab::Bottom || g == Grab::BottomLeft || g == Grab::BottomRight) snap(r.bottom, ys);
}

void FolderWindow::SetCollapsed(bool collapsed) {
    DockConfig& d = Cfg();
    if (d.folder.collapsed == collapsed) return;
    if (!collapsed) {
        d.folder.collapsed = false;
    } else {
        StoreRect();  // the expanded height is kept
        d.folder.collapsed = true;
    }
    applied_ = d;
    if (collapsed) {
        view_.Detach();
    } else if (target_ && !view_.Attached()) {
        view_.Attach(root_);
    }
    RECT r = rect_;
    r.bottom = r.top + (collapsed ? static_cast<LONG>(Px(kTitleH)) : static_cast<LONG>(Px(static_cast<float>(d.folder.h))));
    Place(r, true);
    if (host_.changed) host_.changed();
}

void FolderWindow::ApplyConfig() {
    if (!hwnd_ || cfg_ >= config_->docks.size()) return;
    const DockConfig& d = Cfg();
    const FolderWindowConfig& a = applied_.folder;
    const FolderWindowConfig& f = d.folder;
    const bool pathChanged = f.path != a.path;
    const bool listChanged = pathChanged || f.showHidden != a.showHidden || f.sort != a.sort || f.view != a.view ||
                             f.iconSize != a.iconSize;
    const bool collapsedChanged = f.collapsed != a.collapsed;
    const bool rectChanged = collapsedChanged || f.w != a.w || f.h != a.h || d.x != applied_.x || d.y != applied_.y ||
                             d.monitor != applied_.monitor;
    applied_ = d;
    if (pathChanged) {
        Unwatch();
        folder_ = FolderPath();
        Watch();
    }
    if (collapsedChanged) {
        if (f.collapsed) {
            view_.Detach();
        } else if (target_ && !view_.Attached()) {
            view_.Attach(root_);
        }
    }
    UpdateBackdrop();  // the theme may have changed
    if (listChanged) Load();
    SetWindowTextW(hwnd_, TitleText().c_str());
    if (rectChanged) {
        Place(RectFromConfig(), true);
    } else {
        Relayout();
    }
}

void FolderWindow::Relayout() {
    if (!hwnd_ || !target_) return;
    const LONG w = rect_.right - rect_.left, h = rect_.bottom - rect_.top;
    const LONG titleH = static_cast<LONG>(Px(kTitleH)), pad = static_cast<LONG>(Px(kPad));
    titleRect_ = RECT{0, 0, w, titleH};
    chevronRect_ = RECT{w - static_cast<LONG>(Px(kChevronW + 4.0f)), 0, w - static_cast<LONG>(Px(4.0f)), titleH};
    const bool collapsed = Cfg().folder.collapsed;
    contentRect_ = collapsed ? RECT{} : RECT{pad, titleH + static_cast<LONG>(Px(2.0f)), w - pad, std::max(titleH, h - pad)};
    if (!collapsed && view_.Attached()) {
        const ItemGridView::Style style = ViewStyle();
        view_.SetStyle(style);
        const FolderWindowConfig& f = Cfg().folder;
        const bool list = f.view == ItemView::List || (f.view == ItemView::Auto && entries_.size() > kAutoGridMax);
        view_.SetMode(list ? ItemGridView::Mode::List : ItemGridView::Mode::Grid);
        const LONG cellW = static_cast<LONG>(Px(style.cellW));
        view_.SetColumns(static_cast<size_t>(std::max<LONG>(1, (contentRect_.right - contentRect_.left) / std::max<LONG>(1, cellW))));
        view_.SetBounds(entries_.empty() ? RECT{} : contentRect_);
        view_.Refresh();
    }
    DrawPanel();
    Commit();
}

// --- drawing -------------------------------------------------------------------------------------

std::wstring FolderWindow::TitleText() const {
    const DockConfig& d = Cfg();
    if (!d.title.empty()) return d.title;
    const std::wstring name = LastComponent(FolderPath());
    return name.empty() ? std::wstring(L"Folder") : name;
}

void FolderWindow::EnsureFormats() {
    IDWriteFactory* dw = host_.renderer ? host_.renderer->DWrite() : nullptr;
    if (!dw || (titleFormat_ && formatScale_ == scale_)) return;
    titleFormat_ = messageFormat_ = nullptr;
    titleEllipsis_ = nullptr;
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, kTitleFont * scale_, L"", titleFormat_.put()))) {
        titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(titleFormat_.get(), titleEllipsis_.put()))) {
            titleFormat_->SetTrimming(&trimming, titleEllipsis_.get());
        }
    } else {
        titleFormat_ = nullptr;
    }
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, kMessageFont * scale_, L"", messageFormat_.put()))) {
        messageFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        messageFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
        messageFormat_ = nullptr;
    }
    formatScale_ = scale_;
}

ItemGridView::Style FolderWindow::ViewStyle() const {
    const ThemeConfig& t = config_->ThemeOf(Cfg());
    const FolderWindowConfig& f = Cfg().folder;
    ItemGridView::Style v;
    v.scale = scale_;
    v.text = t.labelText;
    v.secondary = t.clockDateText;
    v.hotBackground = t.previewHotBackground;
    v.hotBorder = t.previewHotBorder;
    v.accent = t.accent;
    v.scrollbar = (t.labelText & 0x00FFFFFF) | 0x80000000;
    v.iconDip = static_cast<float>(f.iconSize);
    v.cellW = std::max(80.0f, v.iconDip + 32.0f);
    v.cellH = v.iconDip + 6.0f + 4.0f + kLabelLines * kLineH + 4.0f;
    v.listIconDip = std::clamp(std::round(v.iconDip / 2.0f), 16.0f, 32.0f);
    v.rowH = v.listIconDip + 12.0f;
    v.scrollSeconds = 0.16;
    v.clickActivates = f.singleClick;
    return v;
}

void FolderWindow::DrawPanel() {
    if (!panel_.visual || !host_.renderer) return;
    EnsureFormats();
    const UINT w = static_cast<UINT>(std::max<LONG>(1, rect_.right - rect_.left));
    const UINT h = static_cast<UINT>(std::max<LONG>(1, rect_.bottom - rect_.top));
    const ThemeConfig& t = config_->ThemeOf(Cfg());
    const bool collapsed = Cfg().folder.collapsed;
    const std::wstring title = TitleText();
    IDWriteFactory* dw = host_.renderer->DWrite();
    host_.renderer->DrawLayer(panel_, w, h, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(acrylic_ ? t.acrylicBackground : t.background), brush.put()))) return;
        const float fw = static_cast<float>(w), fh = static_cast<float>(h);
        const float radius = Px(kRadius);
        const float stroke = std::max(1.0f, std::round(scale_));
        const float inset = stroke / 2.0f;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, fw, fh), radius, radius), brush.get());
        brush->SetColor(Theme::ToColor(t.barBorder));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(inset, inset, fw - inset, fh - inset), radius - inset,
                                                   radius - inset),
                                 brush.get(), stroke);
        // Title.
        const float left = Px(kTitlePadX), right = static_cast<float>(chevronRect_.left) - Px(2.0f);
        winrt::com_ptr<IDWriteTextLayout> layout;
        if (dw && titleFormat_ && right > left &&
            SUCCEEDED(dw->CreateTextLayout(title.c_str(), static_cast<UINT32>(title.size()), titleFormat_.get(), right - left,
                                           static_cast<float>(titleRect_.bottom), layout.put()))) {
            brush->SetColor(Theme::ToColor(t.clockText));
            dc->DrawTextLayout(D2D1::Point2F(left, 0.0f), layout.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        // Chevron: down while expanded, right while collapsed.
        const float cx = (chevronRect_.left + chevronRect_.right) / 2.0f, cy = titleRect_.bottom / 2.0f;
        const float g = Px(4.0f);
        brush->SetColor(Theme::ToColor(t.clockText, 0.7f));
        const float gs = std::max(1.0f, 1.5f * scale_);
        if (collapsed) {
            dc->DrawLine(D2D1::Point2F(cx - g / 2.0f, cy - g), D2D1::Point2F(cx + g / 2.0f, cy), brush.get(), gs);
            dc->DrawLine(D2D1::Point2F(cx + g / 2.0f, cy), D2D1::Point2F(cx - g / 2.0f, cy + g), brush.get(), gs);
        } else {
            dc->DrawLine(D2D1::Point2F(cx - g, cy - g / 2.0f), D2D1::Point2F(cx, cy + g / 2.0f), brush.get(), gs);
            dc->DrawLine(D2D1::Point2F(cx, cy + g / 2.0f), D2D1::Point2F(cx + g, cy - g / 2.0f), brush.get(), gs);
            // Hairline under the title.
            brush->SetColor(Theme::ToColor(t.barBorder));
            const float y = static_cast<float>(titleRect_.bottom) - inset;
            dc->DrawLine(D2D1::Point2F(Px(kPad), y), D2D1::Point2F(fw - Px(kPad), y), brush.get(), stroke);
            if (entries_.empty() && messageFormat_) {
                const wchar_t* text = readable_ ? L"This folder is empty" : L"Cannot open this folder";
                brush->SetColor(Theme::ToColor(t.labelText, 0.6f));
                dc->DrawText(text, static_cast<UINT32>(wcslen(text)), messageFormat_.get(), ToRectF(contentRect_), brush.get());
            }
        }
    });
}

void FolderWindow::Commit() {
    if (!host_.renderer) return;
    const HRESULT hr = host_.renderer->Commit();
    if (FAILED(hr) && Renderer::IsDeviceLost(hr) && host_.deviceLost) host_.deviceLost();
}

void FolderWindow::UpdateBackdrop() {
    if (!hwnd_) return;
    const bool want = config_->ThemeOf(Cfg()).acrylic && Backdrop::SystemAllowsTransparency();
    acrylic_ = want && (backdrop_.Exists() || backdrop_.Create(instance_));
    if (acrylic_) {
        backdrop_.SetCornerRadius(kRadius);
        backdrop_.Place(hwnd_, rect_);
    } else {
        backdrop_.Destroy();
    }
    // The blur window directly beneath this one (the layer keeps the pair together).
    DesktopLayer::SetUnder(hwnd_, acrylic_ ? backdrop_.Hwnd() : nullptr);
}

// --- contents ------------------------------------------------------------------------------------

std::wstring FolderWindow::FolderPath() const {
    const std::wstring& path = Cfg().folder.path;
    if (path.empty()) return {};
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = ExpandEnvironmentStringsW(path.c_str(), buf, static_cast<DWORD>(std::size(buf)));
    return ShellFiles::Normalize(n > 0 && n <= std::size(buf) ? std::wstring(buf) : path);
}

void FolderWindow::Load() {
    entries_.clear();
    iconByIndex_.clear();
    resolved_ = 0;
    readable_ = !folder_.empty();
    const FolderWindowConfig& f = Cfg().folder;
    if (readable_) {
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileExW(Join(folder_, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            readable_ = false;
        } else {
            do {
                const wchar_t* n = fd.cFileName;
                if ((n[0] == L'.' && n[1] == 0) || (n[0] == L'.' && n[1] == L'.' && n[2] == 0)) continue;
                const DWORD a = fd.dwFileAttributes;
                if ((a & FILE_ATTRIBUTE_HIDDEN) && (a & FILE_ATTRIBUTE_SYSTEM)) continue;  // protected (desktop.ini)
                if ((a & FILE_ATTRIBUTE_HIDDEN) && !f.showHidden) continue;
                Entry e;
                e.name = n;
                e.folder = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e.size = (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                e.modified = fd.ftLastWriteTime;
                entries_.push_back(std::move(e));
            } while (entries_.size() < kMaxEntries && FindNextFileW(find, &fd));
            FindClose(find);
        }
    }
    auto byName = [](const Entry& a, const Entry& b) { return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0; };
    const FolderSort sort = f.sort;
    std::sort(entries_.begin(), entries_.end(), [&](const Entry& a, const Entry& b) {
        if (a.folder != b.folder) return a.folder;
        switch (sort) {
            case FolderSort::Date: {
                const LONG c = CompareFileTime(&a.modified, &b.modified);
                if (c != 0) return c > 0;  // newest first
                return byName(a, b);
            }
            case FolderSort::Type: {
                if (!a.folder) {
                    const std::wstring ea = ShellFiles::Extension(a.name), eb = ShellFiles::Extension(b.name);
                    const int c = CompareStringOrdinal(ea.c_str(), -1, eb.c_str(), -1, TRUE);
                    if (c != CSTR_EQUAL) return c == CSTR_LESS_THAN;
                }
                return byName(a, b);
            }
            case FolderSort::Size:
                if (!a.folder && a.size != b.size) return a.size > b.size;  // largest first
                return byName(a, b);
            case FolderSort::Name:
            default:
                return byName(a, b);
        }
    });
    const bool list = f.view == ItemView::List || (f.view == ItemView::Auto && entries_.size() > kAutoGridMax);
    std::vector<GridItem> items;
    items.reserve(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        e.path = Join(folder_, e.name);
        GridItem item;
        item.label = e.name;  // the shell's display name replaces it when the row is drawn
        item.id = i;
        item.flags = e.folder ? 1u : 0u;
        if (list) item.secondary = ShellFiles::SizeAndDate(e.folder, e.size, e.modified);
        items.push_back(std::move(item));
    }
    view_.SetItems(std::move(items));
    view_.SetMode(list ? ItemGridView::Mode::List : ItemGridView::Mode::Grid);
}

void FolderWindow::Reload() {
    std::vector<std::wstring> keep = SelectedPaths();
    const int caret = view_.Selected();
    const std::wstring caretPath = caret >= 0 && static_cast<size_t>(caret) < entries_.size() ? entries_[static_cast<size_t>(caret)].path : L"";
    const LONG scroll = view_.ScrollOffset();
    if (!selectAfterLoad_.empty()) {
        // Renamed / dropped entries: selected once they are there.
        const bool any = std::any_of(selectAfterLoad_.begin(), selectAfterLoad_.end(), [](const std::wstring& p) {
            return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
        });
        if (any) {
            keep = std::move(selectAfterLoad_);
            selectAfterLoad_.clear();
        }
    }
    Load();
    Relayout();
    view_.ScrollTo(static_cast<float>(scroll), false);
    bool first = true;
    auto indexOf = [&](const std::wstring& p) -> int {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (SamePath(entries_[i].path, p)) return static_cast<int>(i);
        }
        return -1;
    };
    const int caretIndex = caretPath.empty() ? -1 : indexOf(caretPath);
    for (const std::wstring& p : keep) {
        const int i = indexOf(p);
        if (i < 0) continue;
        view_.SelectWith(i, !first, false, false);
        first = false;
    }
    if (caretIndex >= 0 && view_.Marked(static_cast<size_t>(caretIndex)) && view_.Selected() != caretIndex) {
        // The caret where it was (toggle twice keeps the marks).
        view_.SelectWith(caretIndex, true, false, false);
        view_.SelectWith(caretIndex, true, false, false);
    }
    Commit();
}

void FolderWindow::Watch() {
    Unwatch();
    if (!hwnd_ || folder_.empty()) return;
    Pidl pidl;
    if (FAILED(SHParseDisplayName(folder_.c_str(), nullptr, &pidl.p, 0, nullptr)) || !pidl.p) return;
    SHChangeNotifyEntry entry{pidl.p, FALSE};
    constexpr LONG kEvents = SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEITEM |
                             SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM | SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES;
    notifyId_ = SHChangeNotifyRegister(hwnd_, SHCNRF_InterruptLevel | SHCNRF_ShellLevel | SHCNRF_NewDelivery, kEvents,
                                       kChangeMessage, 1, &entry);
}

void FolderWindow::Unwatch() {
    if (notifyId_) SHChangeNotifyDeregister(notifyId_);
    notifyId_ = 0;
}

winrt::com_ptr<ID2D1Bitmap> FolderWindow::IconFor(const std::wstring& path, int sysIndex) {
    if (auto it = iconByIndex_.find(sysIndex); it != iconByIndex_.end()) return it->second;
    winrt::com_ptr<ID2D1Bitmap> bitmap;
    ID2D1DeviceContext* dc = host_.renderer ? host_.renderer->ResourceContext() : nullptr;
    const ItemGridView::Style style = ViewStyle();
    const bool list = view_.GetMode() == ItemGridView::Mode::List;
    const int px = static_cast<int>(std::lround((list ? style.listIconDip : style.iconDip) * scale_));
    if (dc && px <= 48) {
        winrt::com_ptr<IImageList> images;
        const int which = px <= 16 ? SHIL_SMALL : px <= 32 ? SHIL_LARGE : SHIL_EXTRALARGE;
        if (SUCCEEDED(SHGetImageList(which, IID_PPV_ARGS(images.put())))) {
            HICON icon = nullptr;
            if (SUCCEEDED(images->GetIcon(sysIndex, ILD_TRANSPARENT, &icon)) && icon) {
                bitmap = icons_.FromHicon(icon, dc);
                DestroyIcon(icon);
            }
        }
    } else if (dc) {
        // Above 48 px: the shell scales the icon to the size (the jumbo image list would show a
        // small icon in the corner of a 256 px square for files without large icons).
        winrt::com_ptr<IShellItemImageFactory> factory;
        HBITMAP hbmp = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(factory.put()))) &&
            SUCCEEDED(factory->GetImage(SIZE{px, px}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp)) && hbmp) {
            if (!wic_) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.put()));
            winrt::com_ptr<IWICBitmap> wb;
            winrt::com_ptr<IWICFormatConverter> conv;
            if (wic_ && SUCCEEDED(wic_->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapUsePremultipliedAlpha, wb.put())) &&
                SUCCEEDED(wic_->CreateFormatConverter(conv.put())) &&
                SUCCEEDED(conv->Initialize(wb.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                           WICBitmapPaletteTypeCustom))) {
                dc->CreateBitmapFromWicBitmap(conv.get(), nullptr, bitmap.put());
            }
            DeleteObject(hbmp);
        }
    }
    iconByIndex_.emplace(sysIndex, bitmap);
    return bitmap;
}

void FolderWindow::ResolveItem(size_t index, GridItem& item) {
    if (index >= entries_.size()) return;
    const Entry& e = entries_[index];
    ++resolved_;
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(e.path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_DISPLAYNAME)) return;
    if (sfi.szDisplayName[0]) item.label = sfi.szDisplayName;
    item.icon = IconFor(e.path, sfi.iIcon);
}

std::vector<std::wstring> FolderWindow::SelectedPaths() const {
    std::vector<std::wstring> paths;
    for (size_t i : view_.MarkedIndexes()) {
        if (i < entries_.size()) paths.push_back(entries_[i].path);
    }
    return paths;
}

void FolderWindow::Open(const std::wstring& path, const wchar_t* verb) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = hwnd_;
    sei.lpVerb = verb;
    sei.lpFile = path.c_str();
    sei.lpDirectory = folder_.empty() ? nullptr : folder_.c_str();
    sei.nShow = SW_SHOWNORMAL;
    AllowSetForegroundWindow(ASFW_ANY);  // what starts may take the foreground
    if (ShellExecuteExW(&sei)) return;
    if (!verb && GetLastError() == ERROR_NO_ASSOCIATION) {
        sei.lpVerb = L"openas";
        if (ShellExecuteExW(&sei)) return;
    }
    MessageBeep(MB_ICONWARNING);
}

void FolderWindow::Activate(size_t index) {
    if (index >= entries_.size()) return;
    std::vector<std::wstring> paths;
    if (view_.Marked(index)) paths = SelectedPaths();
    if (paths.empty()) paths.push_back(entries_[index].path);
    if (paths.size() > kMaxOpen) paths.resize(kMaxOpen);
    for (const std::wstring& p : paths) Open(p, nullptr);  // the default verb (folders: Explorer)
}

// --- menus ---------------------------------------------------------------------------------------

void FolderWindow::ContextMenu(int index, POINT screen) {
    if (edit_) CommitRename(nullptr);
    if (index >= 0 && static_cast<size_t>(index) < entries_.size()) {
        std::vector<std::wstring> paths = SelectedPaths();
        if (paths.empty()) paths.push_back(entries_[static_cast<size_t>(index)].path);
        ShellMenu(paths, index, screen);
        return;
    }
    BackgroundMenu(screen);
}

void FolderWindow::ShellMenu(const std::vector<std::wstring>& paths, int index, POINT screen) {
    Pidl folderPidl;
    winrt::com_ptr<IShellFolder> folder;
    if (FAILED(SHParseDisplayName(folder_.c_str(), nullptr, &folderPidl.p, 0, nullptr)) || !folderPidl.p ||
        FAILED(SHBindToObject(nullptr, folderPidl.p, nullptr, IID_PPV_ARGS(folder.put())))) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    std::vector<Pidl> absolute;
    std::vector<PCUITEMID_CHILD> children;
    for (const std::wstring& p : paths) {
        Pidl a;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &a.p, 0, nullptr)) && a.p) {
            children.push_back(ILFindLastID(a.p));
            absolute.push_back(std::move(a));
        }
    }
    winrt::com_ptr<IContextMenu> cm;
    if (children.empty() || FAILED(folder->GetUIObjectOf(hwnd_, static_cast<UINT>(children.size()), children.data(),
                                                        IID_IContextMenu, nullptr, cm.put_void()))) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    UINT flags = CMF_NORMAL | CMF_CANRENAME;
    if (GetKeyState(VK_SHIFT) < 0) flags |= CMF_EXTENDEDVERBS;
    if (FAILED(cm->QueryContextMenu(menu, 0, kCmdFirst, kCmdLast, flags))) {
        DestroyMenu(menu);
        return;
    }
    menu2_ = cm.try_as<IContextMenu2>();
    menu3_ = cm.try_as<IContextMenu3>();
    menuOpen_ = true;
    SetForegroundWindow(hwnd_);  // so the menu closes on a click elsewhere (KB135788); the window does not rise
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    menuOpen_ = false;
    menu2_ = nullptr;
    menu3_ = nullptr;
    if (cmd >= kCmdFirst) {
        wchar_t verb[64]{};
        if (FAILED(cm->GetCommandString(cmd - kCmdFirst, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb),
                                        static_cast<UINT>(std::size(verb))))) {
            verb[0] = 0;
        }
        if (_wcsicmp(verb, L"rename") == 0) {
            BeginRename(index);  // inline, like Explorer
        } else {
            CMINVOKECOMMANDINFOEX ci{};
            ci.cbSize = sizeof(ci);
            ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            if (GetKeyState(VK_CONTROL) < 0) ci.fMask |= CMIC_MASK_CONTROL_DOWN;
            if (GetKeyState(VK_SHIFT) < 0) ci.fMask |= CMIC_MASK_SHIFT_DOWN;
            ci.hwnd = hwnd_;
            ci.lpVerb = MAKEINTRESOURCEA(cmd - kCmdFirst);
            ci.lpVerbW = MAKEINTRESOURCEW(cmd - kCmdFirst);
            ci.lpDirectoryW = folder_.c_str();
            ci.nShow = SW_SHOWNORMAL;
            ci.ptInvoke = screen;
            AllowSetForegroundWindow(ASFW_ANY);
            cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ci));
        }
    }
    DestroyMenu(menu);
}

void FolderWindow::BackgroundMenu(POINT screen) {
    enum : UINT {
        kOpen = 1, kRefresh, kViewAuto = 10, kViewGrid, kViewList, kSortName = 20, kSortDate, kSortType, kSortSize,
        kHidden = 30, kSingle, kCollapse = 40, kRemove = 50, kNew,
    };
    const FolderWindowConfig f = Cfg().folder;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | (readable_ ? 0 : MF_GRAYED), kOpen, L"Open in Explorer");
    AppendMenuW(menu, MF_STRING, kRefresh, L"Refresh");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (HMENU sub = CreatePopupMenu()) {
        AppendMenuW(sub, MF_STRING, kViewAuto, L"Automatic");
        AppendMenuW(sub, MF_STRING, kViewGrid, L"Grid");
        AppendMenuW(sub, MF_STRING, kViewList, L"List");
        CheckMenuRadioItem(sub, kViewAuto, kViewList, kViewAuto + static_cast<UINT>(f.view), MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), L"View");
    }
    if (HMENU sub = CreatePopupMenu()) {
        AppendMenuW(sub, MF_STRING, kSortName, L"Name");
        AppendMenuW(sub, MF_STRING, kSortDate, L"Date modified");
        AppendMenuW(sub, MF_STRING, kSortType, L"Type");
        AppendMenuW(sub, MF_STRING, kSortSize, L"Size");
        CheckMenuRadioItem(sub, kSortName, kSortSize, kSortName + static_cast<UINT>(f.sort), MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), L"Sort by");
    }
    AppendMenuW(menu, MF_STRING | (f.showHidden ? MF_CHECKED : 0), kHidden, L"Show hidden items");
    AppendMenuW(menu, MF_STRING | (f.singleClick ? MF_CHECKED : 0), kSingle, L"Open items with a single click");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCollapse, f.collapsed ? L"Expand" : L"Collapse to the title bar");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kNew, L"New folder window…");
    AppendMenuW(menu, MF_STRING, kRemove, L"Remove this folder window");
    menuOpen_ = true;
    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    menuOpen_ = false;
    DestroyMenu(menu);
    if (!hwnd_ || cmd == 0) return;
    FolderWindowConfig& c = Cfg().folder;
    bool changed = true;
    switch (cmd) {
        case kOpen: Open(folder_, L"open"); changed = false; break;
        case kRefresh: Reload(); changed = false; break;
        case kViewAuto: case kViewGrid: case kViewList: c.view = static_cast<ItemView>(cmd - kViewAuto); break;
        case kSortName: case kSortDate: case kSortType: case kSortSize: c.sort = static_cast<FolderSort>(cmd - kSortName); break;
        case kHidden: c.showHidden = !c.showHidden; break;
        case kSingle: c.singleClick = !c.singleClick; break;
        case kCollapse: SetCollapsed(!c.collapsed); changed = false; break;
        case kNew: changed = false; if (host_.newWindow) host_.newWindow(); break;
        case kRemove: changed = false; if (host_.remove) host_.remove(cfg_); break;
        default: changed = false; break;
    }
    if (changed) {
        ApplyConfig();
        if (host_.changed) host_.changed();
    }
}

// --- rename / delete / drag ------------------------------------------------------------------------

LRESULT CALLBACK FolderWindow::EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<FolderWindow*>(data);
    switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
            if (wp == VK_RETURN || wp == VK_ESCAPE) {
                if (self->hwnd_) PostMessageW(self->hwnd_, kEditMessage, wp == VK_RETURN ? 1 : 0, 0);
                return 0;
            }
            break;
        case WM_CHAR:
            if (wp == L'\r' || wp == 27) return 0;  // no beep
            break;
        case WM_KILLFOCUS:
            // Explorer commits a rename when the box loses the focus.
            if (self->hwnd_ && self->edit_ == hwnd) PostMessageW(self->hwnd_, kEditMessage, 1, 0);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, &FolderWindow::EditProc, 1);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void FolderWindow::BeginRename(int index) {
    CancelRename();
    if (index < 0 || static_cast<size_t>(index) >= entries_.size() || Cfg().folder.collapsed || !view_.Attached()) return;
    view_.Select(index, true);
    const Entry& e = entries_[static_cast<size_t>(index)];
    const ItemGridView::Style style = ViewStyle();
    const RECT r = view_.ItemRect(static_cast<size_t>(index));
    const LONG boxH = static_cast<LONG>(Px(22.0f));
    RECT box{};
    if (view_.GetMode() == ItemGridView::Mode::Grid) {
        const LONG top = r.top + static_cast<LONG>(Px(6.0f) + Px(style.iconDip) + Px(2.0f));
        box = RECT{r.left + static_cast<LONG>(Px(2.0f)), top, r.right - static_cast<LONG>(Px(2.0f)), top + boxH};
    } else {
        const LONG left = r.left + static_cast<LONG>(Px(8.0f) + Px(style.listIconDip) + Px(8.0f));
        const LONG top = r.top + (r.bottom - r.top - boxH) / 2;
        box = RECT{left, top, r.right - static_cast<LONG>(Px(8.0f)), top + boxH};
    }
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&box), 2);
    if (!editFont_) {
        editFont_ = CreateFontW(-static_cast<int>(Px(12.0f)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
    edit_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", e.name.c_str(),
                            WS_POPUP | WS_BORDER | ES_AUTOHSCROLL |
                                (view_.GetMode() == ItemGridView::Mode::Grid ? ES_CENTER : ES_LEFT),
                            box.left, box.top, box.right - box.left, box.bottom - box.top, hwnd_, nullptr, instance_, nullptr);
    if (!edit_) return;
    if (editFont_) SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), FALSE);
    SetWindowSubclass(edit_, &FolderWindow::EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    // The name without its extension is selected (folders: all of it), as in Explorer.
    const size_t sel = e.folder ? e.name.size() : ShellFiles::Stem(e.name).size();
    SendMessageW(edit_, EM_SETSEL, 0, static_cast<LPARAM>(sel));
    renameIndex_ = index;
    renamePath_ = e.path;
    ShowWindow(edit_, SW_SHOW);
    SetFocus(edit_);
}

void FolderWindow::CommitRename(const std::wstring* text) {
    if (!edit_) return;
    std::wstring name;
    if (text) {
        name = *text;
    } else {
        const int n = GetWindowTextLengthW(edit_);
        name.resize(static_cast<size_t>(n) + 1);
        name.resize(static_cast<size_t>(GetWindowTextW(edit_, name.data(), n + 1)));
    }
    CancelRename();  // the box goes first (its focus loss posts nothing any more)
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    const std::wstring old = LastComponent(renamePath_);
    if (name.empty() || name == old || renamePath_.empty()) return;
    if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    selectAfterLoad_ = {Join(Parent(renamePath_), name)};
    StartJob(ShellFiles::Op::Rename, {renamePath_}, name);
}

void FolderWindow::CancelRename() {
    if (!edit_) return;
    HWND e = edit_;
    edit_ = nullptr;
    DestroyWindow(e);
    renameIndex_ = -1;
}

void FolderWindow::RecycleSelection() {
    std::vector<std::wstring> paths = SelectedPaths();
    if (paths.empty()) return;
    StartJob(ShellFiles::Op::Recycle, std::move(paths), {});
}

bool FolderWindow::StartJob(ShellFiles::Op op, std::vector<std::wstring> files, const std::wstring& dest) {
    ShellFiles::Job job;
    job.op = op;
    job.files = std::move(files);
    job.dest = dest;
    job.notify = hwnd_;
    job.message = kJobDoneMessage;
    job.owner = hwnd_;
    if (!ShellFiles::Start(std::move(job))) {
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    ++jobs_;
    return true;
}

void FolderWindow::BeginDrag(size_t index, POINT) {
    if (index >= entries_.size()) return;
    if (!view_.Marked(index)) view_.SelectWith(static_cast<int>(index), false, false, false);
    const std::vector<std::wstring> paths = SelectedPaths();
    winrt::com_ptr<IDataObject> data = ShellFiles::DataObjectFor(paths);
    if (!data) return;
    dragging_ = true;
    DWORD effect = DROPEFFECT_NONE;
    // The shell's drag (drag image, the drop source); a move done by the target shows up through
    // the change notifications.
    SHDoDragDrop(hwnd_, data.get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    dragging_ = false;
}

// --- drops ---------------------------------------------------------------------------------------

void FolderWindow::RegisterDrop() {
    if (dropTarget_ || !hwnd_) return;
    dropTarget_ = new ShellFiles::DropTarget(ShellFiles::DropTarget::Sink{
        [this](IDataObject* d, POINT s, DWORD k, DWORD a) { return DragEnter(d, s, k, a); },
        [this](POINT s, DWORD k, DWORD a) { return DragOver(s, k, a); },
        [this] { DragLeave(); },
        [this](IDataObject* d, POINT s, DWORD k, DWORD a) { return Drop(d, s, k, a); },
    });
    if (FAILED(RegisterDragDrop(hwnd_, dropTarget_))) {
        dropTarget_->Detach();
        dropTarget_->Release();
        dropTarget_ = nullptr;
    }
}

std::wstring FolderWindow::DropFolderAt(POINT screen, int* entry) const {
    if (entry) *entry = -1;
    POINT client = screen;
    if (hwnd_) ScreenToClient(hwnd_, &client);
    const int i = view_.Attached() ? view_.HitTest(client) : -1;
    if (i >= 0 && static_cast<size_t>(i) < entries_.size() && entries_[static_cast<size_t>(i)].folder) {
        const std::wstring& dest = entries_[static_cast<size_t>(i)].path;
        const bool self = std::any_of(dragFiles_.begin(), dragFiles_.end(),
                                      [&](const std::wstring& f) { return ShellFiles::IsInside(dest, f); });
        if (!self) {
            if (entry) *entry = i;
            return dest;
        }
    }
    return folder_;
}

DWORD FolderWindow::DragEnter(IDataObject* data, POINT screen, DWORD keys, DWORD allowed) {
    dragFiles_ = ShellFiles::FilesOf(data);
    return DragOver(screen, keys, allowed);
}

DWORD FolderWindow::DragOver(POINT screen, DWORD keys, DWORD allowed) {
    if (dragFiles_.empty() || !readable_) return DROPEFFECT_NONE;
    int entry = -1;
    const std::wstring dest = DropFolderAt(screen, &entry);
    const DWORD effect = ShellFiles::DropEffect(dragFiles_, dest, keys, allowed);
    if (view_.Attached()) view_.SetDropHighlight(effect != DROPEFFECT_NONE ? entry : -1);
    return effect;
}

void FolderWindow::DragLeave() {
    dragFiles_.clear();
    if (view_.Attached()) view_.SetDropHighlight(-1);
}

DWORD FolderWindow::Drop(IDataObject* data, POINT screen, DWORD keys, DWORD allowed) {
    dragFiles_ = ShellFiles::FilesOf(data);
    int entry = -1;
    const std::wstring dest = DropFolderAt(screen, &entry);
    if (view_.Attached()) view_.SetDropHighlight(-1);
    std::vector<std::wstring> files = std::move(dragFiles_);
    dragFiles_.clear();
    return DropInto(std::move(files), dest, keys, allowed);
}

DWORD FolderWindow::DropInto(std::vector<std::wstring> files, const std::wstring& dest, DWORD keys, DWORD allowed) {
    if (!readable_ || dest.empty()) return DROPEFFECT_NONE;
    const DWORD effect = ShellFiles::DropEffect(files, dest, keys, allowed);
    if (effect == DROPEFFECT_NONE) return DROPEFFECT_NONE;
    const bool move = effect == DROPEFFECT_MOVE;
    if (move) std::erase_if(files, [&](const std::wstring& f) { return SamePath(Parent(f), dest); });
    if (SamePath(dest, folder_)) {
        selectAfterLoad_.clear();
        for (const std::wstring& f : files) selectAfterLoad_.push_back(Join(folder_, LastComponent(f)));
    }
    if (!StartJob(move ? ShellFiles::Op::Move : ShellFiles::Op::Copy, std::move(files), dest)) return DROPEFFECT_NONE;
    // Done by us (also a move): the source is told "copied", so it never deletes anything itself.
    return (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : effect;
}

DWORD FolderWindow::TestDrop(const std::vector<std::wstring>& files, int entry, DWORD keys) {
    std::wstring dest = folder_;
    if (entry >= 0 && static_cast<size_t>(entry) < entries_.size() && entries_[static_cast<size_t>(entry)].folder) {
        dest = entries_[static_cast<size_t>(entry)].path;
    }
    return DropInto(files, dest, keys, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
}

// --- automation ----------------------------------------------------------------------------------

LRESULT FolderWindow::QueryState(WPARAM what, LPARAM arg) const {
    switch (what) {
        case 185: return rect_.left;
        case 186: return rect_.top;
        case 187: return rect_.right - rect_.left;
        case 188: return rect_.bottom - rect_.top;
        case 189: return static_cast<LRESULT>(entries_.size());
        case 190: return Cfg().folder.collapsed ? 1 : 0;
        case 191: return view_.Selected();
        case 192: return view_.ScrollOffset();
        case 193: return static_cast<LRESULT>(view_.LiveRows());
        case 194: return static_cast<LRESULT>(refreshes_);
        case 195: return (refreshPending_ ? 1 : 0) + (view_.TimerPending() ? 1 : 0);
        case 196: return static_cast<LRESULT>(resolved_);
        case 197: return edit_ ? 1 : 0;
        case 198: return view_.GetMode() == ItemGridView::Mode::List ? 1 : 0;
        case 199: return reinterpret_cast<LRESULT>(hwnd_);
        case 200:
            if (arg < 0 || static_cast<size_t>(arg) >= entries_.size()) return -1;
            return static_cast<LRESULT>(Fnv(entries_[static_cast<size_t>(arg)].name) & 0x7FFFFFFFu);
        case 201: return static_cast<LRESULT>(notifications_);
        case 202: return DesktopLayer::AboveDesktop(hwnd_) ? 1 : 0;
        case 203: return view_.Attached() ? static_cast<LRESULT>(view_.Columns()) : 0;
        case 204: return jobs_;
        case 205: return static_cast<LRESULT>(view_.MarkedCount());
        default: return -1;
    }
}

LRESULT FolderWindow::Command(UINT cmd, int arg, int high) {
    const int n = static_cast<int>(entries_.size());
    switch (cmd) {
        case 839:  // select (HIWORD(wParam): 1 Ctrl, 2 Shift)
            if (arg == 0xFFFF) arg = -1;
            if (arg >= n || !view_.Attached()) return 0;
            view_.SelectWith(arg, (high & 1) != 0, (high & 2) != 0, true);
            return 1;
        case 840:
            if (arg < 0 || arg >= n) return 0;
            Activate(static_cast<size_t>(arg));
            return 1;
        case 841:
            if (!view_.Attached()) return 0;
            view_.ScrollTo(static_cast<float>(high), true);
            return 1;
        case 842:
            SetCollapsed(!Cfg().folder.collapsed);
            return 1;
        case 843:
            BeginRename(arg);
            return edit_ ? 1 : 0;
        case 844: {
            if (!edit_) return 0;
            if (high == 0) {
                CommitRename(nullptr);
                return 1;
            }
            wchar_t buf[256]{};
            if (!GlobalGetAtomNameW(static_cast<ATOM>(high), buf, static_cast<int>(std::size(buf)))) return 0;
            const std::wstring name(buf);
            CommitRename(&name);
            return 1;
        }
        case 845:
            if (view_.MarkedCount() == 0) return 0;
            RecycleSelection();
            return 1;
        case 846:
            if (arg != 0xFFFF && (arg < 0 || arg >= n)) return 0;
            if (arg != 0xFFFF && view_.Attached() && !view_.Marked(static_cast<size_t>(arg))) view_.Select(arg, true);
            PostMessageW(hwnd_, kMenuMessage, static_cast<WPARAM>(arg), 0);  // the menu is modal
            return 1;
        case 847:
            view_.SelectAll();
            return 1;
        case 849:
        case 850: {
            wchar_t buf[256]{};
            if (!GlobalGetAtomNameW(static_cast<ATOM>(high), buf, static_cast<int>(std::size(buf)))) return 0;
            const DWORD effect = TestDrop({std::wstring(buf)}, arg == 0xFFFF ? -1 : arg, cmd == 850 ? MK_CONTROL : 0);
            return static_cast<LRESULT>(effect);
        }
        default:
            return -1;
    }
}

// --- window --------------------------------------------------------------------------------------

LRESULT CALLBACK FolderWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    FolderWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<FolderWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<FolderWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self || self->hwnd_ != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(msg, wp, lp);
}

LRESULT FolderWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    // The shell menu's owner-drawn parts (Send to, Open with ...).
    if (menuOpen_ && (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM || msg == WM_MENUCHAR)) {
        LRESULT result = 0;
        if (menu3_ && SUCCEEDED(menu3_->HandleMenuMsg2(msg, wp, lp, &result))) return result;
        if (menu2_ && SUCCEEDED(menu2_->HandleMenuMsg(msg, wp, lp))) return msg == WM_INITMENUPOPUP ? 0 : TRUE;
    }
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    const bool collapsed = cfg_ < config_->docks.size() && Cfg().folder.collapsed;
    switch (msg) {
        case WM_WINDOWPOSCHANGING:
            DesktopLayer::FilterPosChanging(hwnd_, reinterpret_cast<WINDOWPOS*>(lp));
            return 0;
        case WM_WINDOWPOSCHANGED:
            if (acrylic_ && !(reinterpret_cast<const WINDOWPOS*>(lp)->flags & SWP_NOZORDER)) backdrop_.Restack(hwnd_);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;  // the keyboard comes here; the z-order stays (FilterPosChanging)
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT c{};
                GetCursorPos(&c);
                ScreenToClient(hwnd_, &c);
                const wchar_t* cursor = IDC_ARROW;
                switch (grab_ != Grab::None ? grab_ : GrabAt(c)) {
                    case Grab::Left: case Grab::Right: cursor = IDC_SIZEWE; break;
                    case Grab::Top: case Grab::Bottom: cursor = IDC_SIZENS; break;
                    case Grab::TopLeft: case Grab::BottomRight: cursor = IDC_SIZENWSE; break;
                    case Grab::TopRight: case Grab::BottomLeft: cursor = IDC_SIZENESW; break;
                    default: break;
                }
                SetCursor(LoadCursorW(nullptr, cursor));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: {
            const Grab g = GrabAt(pt);
            if (g != Grab::None) {
                grab_ = g;
                grabPt_ = pt;
                ClientToScreen(hwnd_, &grabPt_);
                grabRect_ = rect_;
                grabMoved_ = false;
                SetCapture(hwnd_);
                return 0;
            }
            if (!collapsed) view_.ButtonDown(ItemGridView::Button::Left, pt);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                tracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            if (grab_ != Grab::None) {
                POINT s = pt;
                ClientToScreen(hwnd_, &s);
                const LONG dx = s.x - grabPt_.x, dy = s.y - grabPt_.y;
                if (!grabMoved_ && std::labs(dx) < GetSystemMetrics(SM_CXDRAG) && std::labs(dy) < GetSystemMetrics(SM_CYDRAG)) {
                    return 0;
                }
                grabMoved_ = true;
                RECT r = grabRect_;
                const LONG minW = static_cast<LONG>(Px(kMinW)), minH = static_cast<LONG>(Px(kMinH));
                switch (grab_) {
                    case Grab::Move:
                        OffsetRect(&r, dx, dy);
                        SnapMove(r);
                        break;
                    default: {
                        if (grab_ == Grab::Left || grab_ == Grab::TopLeft || grab_ == Grab::BottomLeft) r.left += dx;
                        if (grab_ == Grab::Right || grab_ == Grab::TopRight || grab_ == Grab::BottomRight) r.right += dx;
                        if (grab_ == Grab::Top || grab_ == Grab::TopLeft || grab_ == Grab::TopRight) r.top += dy;
                        if (grab_ == Grab::Bottom || grab_ == Grab::BottomLeft || grab_ == Grab::BottomRight) r.bottom += dy;
                        SnapEdges(r, grab_);
                        if (r.right - r.left < minW) {
                            if (r.left != grabRect_.left) r.left = r.right - minW; else r.right = r.left + minW;
                        }
                        if (!collapsed && r.bottom - r.top < minH) {
                            if (r.top != grabRect_.top) r.top = r.bottom - minH; else r.bottom = r.top + minH;
                        }
                        break;
                    }
                }
                Place(r, false);
                return 0;
            }
            if (!collapsed) view_.MouseMove(pt);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (grab_ != Grab::None) {
                const Grab g = grab_;
                grab_ = Grab::None;
                if (GetCapture() == hwnd_) ReleaseCapture();
                if (grabMoved_) {
                    StoreRect();
                    if (host_.changed) host_.changed();
                } else if (g == Grab::Move && PtInRect(&chevronRect_, pt)) {
                    SetCollapsed(!collapsed);
                }
                grabMoved_ = false;
                return 0;
            }
            if (!collapsed) view_.ButtonUp(ItemGridView::Button::Left, pt);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            if (PtInRect(&titleRect_, pt) && GrabAt(pt) == Grab::Move) {
                if (!PtInRect(&chevronRect_, pt)) SetCollapsed(!collapsed);  // a chevron click toggles on its own
                return 0;
            }
            if (!collapsed && !Cfg().folder.singleClick && !view_.DoubleClick(pt)) {
                view_.ButtonDown(ItemGridView::Button::Left, pt);  // the background: a plain press
            }
            return 0;
        case WM_RBUTTONDOWN:
            if (!collapsed && view_.Contains(pt)) view_.ButtonDown(ItemGridView::Button::Right, pt);
            return 0;
        case WM_RBUTTONUP: {
            if (!collapsed && view_.Pressed(ItemGridView::Button::Right)) {
                view_.ButtonUp(ItemGridView::Button::Right, pt);
                return 0;
            }
            POINT s = pt;
            ClientToScreen(hwnd_, &s);
            BackgroundMenu(s);
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (!collapsed) view_.MouseLeave();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT c = pt;
            ScreenToClient(hwnd_, &c);
            if (!collapsed) view_.Wheel(GET_WHEEL_DELTA_WPARAM(wp), c);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (grab_ != Grab::None && reinterpret_cast<HWND>(lp) != hwnd_) {
                grab_ = Grab::None;
                if (grabMoved_) {
                    StoreRect();
                    if (host_.changed) host_.changed();
                }
                grabMoved_ = false;
            }
            view_.CaptureLost();
            return 0;
        case WM_KEYDOWN:
            if (collapsed) return 0;
            switch (wp) {
                case VK_F2: BeginRename(view_.Selected()); return 0;
                case VK_DELETE: RecycleSelection(); return 0;
                case VK_F5: Reload(); return 0;
                case VK_ESCAPE: view_.SelectWith(-1, false, false, false); return 0;
                default: view_.KeyDown(static_cast<UINT>(wp)); return 0;
            }
        case WM_SYSKEYDOWN:
            if (!collapsed && wp == VK_F10 && GetKeyState(VK_SHIFT) < 0) {
                view_.KeyDown(VK_F10);
                return 0;
            }
            if (!collapsed && wp == VK_RETURN) {  // Alt+Enter: Properties
                const std::vector<std::wstring> paths = SelectedPaths();
                if (!paths.empty()) SHObjectProperties(hwnd_, SHOP_FILEPATH, paths.front().c_str(), nullptr);
                return 0;
            }
            break;
        case WM_CHAR:
            if (!collapsed) view_.Char(static_cast<wchar_t>(wp));
            return 0;
        case WM_TIMER:
            if (wp == kRefreshTimer) {
                KillTimer(hwnd_, kRefreshTimer);  // one-shot
                refreshPending_ = false;
                ++refreshes_;
                Reload();
                return 0;
            }
            view_.Timer(wp);
            return 0;
        case kChangeMessage: {
            PIDLIST_ABSOLUTE* pidls = nullptr;
            LONG event = 0;
            if (HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wp), static_cast<DWORD>(lp), &pidls, &event)) {
                SHChangeNotification_Unlock(lock);
            }
            ++notifications_;
            if (!refreshPending_) {
                refreshPending_ = SetTimer(hwnd_, kRefreshTimer, kRefreshMs, nullptr) != 0;  // coalesced
            }
            return 0;
        }
        case kJobDoneMessage:
            if (jobs_ > 0) --jobs_;
            // The change notifications usually come first; a late one is coalesced with this.
            if (!refreshPending_) refreshPending_ = SetTimer(hwnd_, kRefreshTimer, kRefreshMs, nullptr) != 0;
            return 0;
        case kMenuMessage: {
            const int index = static_cast<int>(wp) == 0xFFFF ? -1 : static_cast<int>(wp);
            POINT s{rect_.left + (rect_.right - rect_.left) / 2, rect_.top + static_cast<LONG>(Px(kTitleH)) * 2};
            if (index >= 0 && view_.Attached()) {
                const RECT r = view_.ItemRect(static_cast<size_t>(index));
                s = POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
                ClientToScreen(hwnd_, &s);
            }
            ContextMenu(index, s);
            return 0;
        }
        case kEditMessage:
            if (wp) {
                CommitRename(nullptr);
            } else {
                CancelRename();
            }
            return 0;
        case WM_DPICHANGED:
            dpi_ = HIWORD(wp);
            scale_ = static_cast<float>(dpi_) / 96.0f;
            formatScale_ = 0.0f;
            Load();
            Place(RectFromConfig(), true);
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            if (msg == WM_DISPLAYCHANGE || wp == SPI_SETWORKAREA) {
                const RECT r = RectFromConfig();
                if (!EqualRect(&r, &rect_)) Place(r, false);
            }
            return 0;
        case WM_SYSCOMMAND:
            // Never minimized, maximized or closed by the system menu / Alt+F4 (the window belongs
            // to the config; "Remove this folder window" removes it).
            if ((wp & 0xFFF0) == SC_CLOSE || (wp & 0xFFF0) == SC_MINIMIZE || (wp & 0xFFF0) == SC_MAXIMIZE) return 0;
            break;
        case WM_CLOSE:
            return 0;
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
