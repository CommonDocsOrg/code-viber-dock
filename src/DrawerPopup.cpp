// Round 12 phase 12D: the drawer popup of customize mode (DrawerPopup.h).

#include "DrawerPopup.h"

#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>

#include "SettingsIds.h"

namespace {

constexpr int kWidthDip = 600;   // the popup's width (clamped to the display)
constexpr int kPad = 12;         // DIPs around the content
constexpr int kSearchTop = 44, kSearchH = 28;
constexpr int kChipsTop = 80, kChipsH = 26;
constexpr int kGridTop = 114;
constexpr int kMinGridH = 120;   // DIPs: at least about one row of tiles

struct Chip {
    int id;
    const wchar_t* text;
    int w;
};
constexpr Chip kChips[] = {{SettingsIds::kDrawerChipAll, L"All", 48},
                           {SettingsIds::kDrawerChipBuiltIn, L"Built in", 76},
                           {SettingsIds::kDrawerChipWidgets, L"Widgets", 76},
                           {SettingsIds::kDrawerChipApps, L"Apps and folders", 124}};

std::wstring PreviewKey(const std::wstring& id, SIZE fit, float zoom) {
    return id + L"|" + std::to_wstring(fit.cx) + L"x" + std::to_wstring(fit.cy) + L"z" + std::to_wstring(static_cast<int>(zoom * 100.0f));
}

COLORREF MixColor(COLORREF a, COLORREF b, int pa) {
    auto ch = [&](BYTE x, BYTE y) { return static_cast<BYTE>((x * pa + y * (100 - pa)) / 100); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

}  // namespace

DrawerPopup::~DrawerPopup() {
    if (hwnd_) DestroyWindow(hwnd_);
    grid_.reset();
    ClearPreviews();
    for (HGDIOBJ o : {static_cast<HGDIOBJ>(font_), static_cast<HGDIOBJ>(bold_), static_cast<HGDIOBJ>(brush_), static_cast<HGDIOBJ>(editBrush_)}) {
        if (o) DeleteObject(o);
    }
}

bool DrawerPopup::EnsureWindow(HINSTANCE instance) {
    if (hwnd_) return true;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &DrawerPopup::WndProcThunk;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        if (!registered) return false;
    }
    StripUi::SettingsStrip::Register();
    // Topmost like the dock, a tool window (not in the taskbar / Alt+Tab), activatable (the search
    // box takes typing).
    CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, kClassName, L"Add to the dock", WS_POPUP | WS_CLIPCHILDREN, 0, 0,
                    0, 0, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    // Windows 11: rounded corners (no effect before; ignored).
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    search_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kDrawerPopupSearch)), instance, nullptr);
    if (search_) {
        SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search widgets"));
        SendMessageW(search_, EM_LIMITTEXT, 80, 0);
    }
    for (const Chip& c : kChips) {
        CreateWindowExW(0, WC_BUTTONW, c.text,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE | (c.id == SettingsIds::kDrawerChipAll ? WS_GROUP : 0),
                        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(c.id)), instance, nullptr);
    }
    CheckDlgButton(hwnd_, SettingsIds::kDrawerChipAll, BST_CHECKED);
    // (Its host is this popup's private base: converted here, where that is accessible.)
    StripUi::StripHost& host = *this;
    grid_ = std::make_unique<StripUi::SettingsStrip>(host, StripUi::SettingsStrip::Mode::Drawer, L"Add to the dock");
    HWND grid = CreateWindowExW(0, StripUi::SettingsStrip::kClass, L"Add to the dock", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 0, 0,
                                0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kDrawerPopupGrid)), instance, nullptr);
    grid_->Attach(grid);
    grid_->onActivate = [this](int ref) {
        if (cb_.add) cb_.add(ref);
    };
    grid_->onDesktop = [this](int ref) {
        if (cb_.desktop) cb_.desktop(ref);
    };
    grid_->onDragOutside = [this](POINT screen, int ref) { return cb_.drag ? cb_.drag(0, screen, ref) : 0; };
    grid_->onDragOutsideLeave = [this] {
        if (cb_.drag) cb_.drag(1, POINT{}, -1);
    };
    grid_->onDropOutside = [this](POINT screen, int ref) { return cb_.drag && cb_.drag(2, screen, ref) != 0; };
    fontDpi_ = 0;
    ApplyStyle();
    return true;
}

void DrawerPopup::ApplyStyle() {
    palette_ = DarkMode::CurrentPalette();
    if (brush_) DeleteObject(brush_);
    brush_ = CreateSolidBrush(palette_.layer);
    if (editBrush_) DeleteObject(editBrush_);
    editBrush_ = CreateSolidBrush(palette_.control);
    if (fontDpi_ != dpi_ || !font_) {
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
        if (font_) DeleteObject(font_);
        if (bold_) DeleteObject(bold_);
        font_ = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW b = ncm.lfMessageFont;
        b.lfWeight = FW_SEMIBOLD;
        b.lfHeight = b.lfHeight * 7 / 6;
        bold_ = CreateFontIndirectW(&b);
        fontDpi_ = dpi_;
    }
    if (!hwnd_) return;
    for (HWND c = GetWindow(hwnd_, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        DarkMode::ThemeControl(c, palette_.dark);
    }
    if (grid_) grid_->StyleChanged();
    // The border in the dock's colour (Windows 11; ignored before).
    const COLORREF border = palette_.highContrast ? palette_.text : dockColor_;
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

void DrawerPopup::Fill() {
    if (!grid_) return;
    int select = -1;
    std::vector<StripUi::StripTile> tiles = catalog_.Tiles(dock_, filter_, chip_, grid_->SelectedRef(), Px(44), &select, nullptr);
    grid_->SetTiles(std::move(tiles), select);
}

int DrawerPopup::ContentHeight(int w) const {
    const int gridW = std::max(1, w - 2 * Px(kPad));
    const int gridH = grid_ ? std::max(grid_->GridHeight(gridW), Px(kMinGridH)) : Px(kMinGridH);
    return Px(kGridTop) + gridH + Px(kPad);
}

bool DrawerPopup::Show(HINSTANCE instance, const DockConfig& dock, const std::wstring& name, uint32_t argb, const RECT& avail, DockEdge edge,
                       int endAlong, UINT dpi) {
    const bool wasShown = Shown();
    dpi_ = std::max<UINT>(dpi, USER_DEFAULT_SCREEN_DPI / 2);
    dockColor_ = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    if (!EnsureWindow(instance)) return false;
    if (fontDpi_ != dpi_) ApplyStyle();
    const COLORREF border = palette_.highContrast ? palette_.text : dockColor_;
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    if (!wasShown) {
        dock_ = dock;
    } else {
        Refresh(dock);
    }
    name_ = name;
    SetWindowTextW(hwnd_, (L"Add to " + name).c_str());
    if (!wasShown) {
        catalog_.Build();  // widgets installed meanwhile
        Fill();
    }  // shown already: the tiles stay (a drag in progress is not cancelled); Refresh follows the items
    // The size: 600 DIPs (the room's width at most), as tall as the content within the room.
    const int roomW = std::max<int>(Px(240), avail.right - avail.left), roomH = std::max<int>(Px(160), avail.bottom - avail.top);
    const int w = std::min(Px(kWidthDip), roomW);
    const int h = std::min(ContentHeight(w), roomH);
    int x = endAlong - w, y = avail.bottom - h;  // a bottom dock: on the room's bottom side (toward the dock), its end at endAlong
    switch (edge) {
        case DockEdge::Top: y = avail.top; break;
        case DockEdge::Left:
            x = avail.left;
            y = endAlong - h;
            break;
        case DockEdge::Right:
            x = avail.right - w;
            y = endAlong - h;
            break;
        case DockEdge::Bottom:
        default: break;
    }
    x = std::clamp(x, static_cast<int>(avail.left), std::max(static_cast<int>(avail.left), static_cast<int>(avail.right) - w));
    y = std::clamp(y, static_cast<int>(avail.top), std::max(static_cast<int>(avail.top), static_cast<int>(avail.bottom) - h));
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW | (wasShown ? SWP_NOACTIVATE : 0));
    Layout();
    if (!wasShown) {
        SetForegroundWindow(hwnd_);
        if (search_) SetFocus(search_);
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

void DrawerPopup::Hide() {
    if (!hwnd_ || !IsWindowVisible(hwnd_)) return;
    hiding_ = true;
    // The foreground goes back to the dock first: hiding the active window would activate whatever
    // is next in the z-order (another app: customize mode would end).
    if (GetForegroundWindow() == hwnd_ && returnFocus_ && IsWindow(returnFocus_)) SetForegroundWindow(returnFocus_);
    ShowWindow(hwnd_, SW_HIDE);
    hiding_ = false;
    // The previews belong to this showing (they render again when it is shown next).
    ClearPreviews();
}

void DrawerPopup::Refresh(const DockConfig& dock) {
    // Only a change of the kinds on the dock changes a tile ("On the dock"); refilling cancels a drag.
    auto kinds = [](const DockConfig& d) {
        std::wstring s;
        for (const DockItemEntry& e : d.items) s += e.kind + L'|';
        return s;
    };
    const bool same = kinds(dock) == kinds(dock_);
    dock_ = dock;
    if (Shown() && !same) Fill();
}

void DrawerPopup::CloseSelf() {
    if (!Shown()) return;
    Hide();
    if (cb_.closed) cb_.closed();
}

void DrawerPopup::Layout() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int pad = Px(kPad), w = rc.right - 2 * pad;
    if (search_) SetWindowPos(search_, nullptr, pad, Px(kSearchTop), w, Px(kSearchH), SWP_NOZORDER | SWP_NOACTIVATE);
    int x = pad;
    for (const Chip& c : kChips) {
        if (HWND b = GetDlgItem(hwnd_, c.id)) SetWindowPos(b, nullptr, x, Px(kChipsTop), Px(c.w), Px(kChipsH), SWP_NOZORDER | SWP_NOACTIVATE);
        x += Px(c.w) + Px(6);
    }
    if (grid_ && grid_->Hwnd()) {
        SetWindowPos(grid_->Hwnd(), nullptr, pad, Px(kGridTop), w, std::max<int>(0, rc.bottom - Px(kGridTop) - pad),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void DrawerPopup::ClearPreviews() {
    for (auto& [key, img] : previews_) {
        if (img.bitmap) DeleteObject(img.bitmap);
    }
    previews_.clear();
}

void DrawerPopup::InvalidatePreviews() {
    ClearPreviews();
    if (hwnd_) RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

const DrawerPopup::Image* DrawerPopup::PreviewEntry(const std::wstring& id, SIZE fit, float zoom) {
    const uint64_t stamp = cb_.previewStamp ? cb_.previewStamp() : 0;
    if (stamp != previewStamp_ || dpi_ != previewDpi_) {
        ClearPreviews();
        previewStamp_ = stamp;
        previewDpi_ = dpi_;
    }
    const auto it = previews_.find(PreviewKey(id, fit, zoom));
    return it == previews_.end() ? nullptr : &it->second;
}

const DrawerPopup::Image* DrawerPopup::Preview(const std::wstring& id, SIZE fit, bool render, float zoom) {
    if (const Image* cached = PreviewEntry(id, fit, zoom)) return cached->failed ? nullptr : cached;
    if (!render || !cb_.renderPreview || !hwnd_) return nullptr;
    const SettingsWidgetPreview r = cb_.renderPreview(id, static_cast<float>(dpi_) / 96.0f, fit, zoom);
    Image img;
    img.bitmap = r.bitmap;
    img.px = r.px;
    img.failed = r.bitmap == nullptr;
    img.ms = r.ms;
    const auto it = previews_.emplace(PreviewKey(id, fit, zoom), img).first;
    return it->second.failed ? nullptr : &it->second;
}

void DrawerPopup::PaintPreview(HDC dc, const RECT& box, const Image& img, COLORREF under) const {
    // As SettingsWindow::PaintPreview: a rounded panel in the theme's background, the bitmap fitted
    // inside (aspect kept, never enlarged past 1:1).
    const int bw = box.right - box.left, bh = box.bottom - box.top;
    if (!img.bitmap || bw <= 0 || bh <= 0 || img.px.cx <= 0 || img.px.cy <= 0) return;
    const uint32_t argb = cb_.previewPanel ? cb_.previewPanel() : 0xFF1E1E1E;
    const COLORREF theme = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    HBRUSH brush = CreateSolidBrush(MixColor(theme, under, 60));
    HPEN pen = CreatePen(PS_SOLID, 1, MixColor(theme, under, 75));
    HGDIOBJ ob = SelectObject(dc, brush);
    HGDIOBJ op = SelectObject(dc, pen);
    const int radius = Px(6) * 2;
    RoundRect(dc, box.left, box.top, box.right, box.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    DeleteObject(brush);
    const int pad = std::max(1, Px(3));
    const int aw = std::max(1, bw - 2 * pad), ah = std::max(1, bh - 2 * pad);
    double k = std::min(static_cast<double>(aw) / img.px.cx, static_cast<double>(ah) / img.px.cy);
    k = std::min(k, 1.0);
    const int w = std::max(1, static_cast<int>(std::lround(img.px.cx * k))), h = std::max(1, static_cast<int>(std::lround(img.px.cy * k)));
    const int x = box.left + (bw - w) / 2, y = box.top + (bh - h) / 2;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, img.bitmap);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    GdiAlphaBlend(dc, x, y, w, h, mem, 0, 0, img.px.cx, img.px.cy, blend);
    SelectObject(mem, old);
    DeleteDC(mem);
}

LRESULT CALLBACK DrawerPopup::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DrawerPopup* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<DrawerPopup*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DrawerPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self->hwnd_ == hwnd) self->hwnd_ = nullptr;
        self->search_ = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->WndProc(msg, wp, lp);
}

LRESULT DrawerPopup::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ACTIVATE:
            // The click model: losing the activation (a click elsewhere) hides it, unless one of
            // its own dialogs took it.
            if (LOWORD(wp) == WA_INACTIVE && !modal_ && !hiding_) {
                const HWND to = reinterpret_cast<HWND>(lp);
                if (!to || GetAncestor(to, GA_ROOTOWNER) != hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);  // (checked again there)
            }
            return 0;
        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wp);
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            FillRect(dc, &rc, brush_);
            // The title: "Add to <dock>", and what a click / drag does.
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ old = SelectObject(dc, bold_);
            SetTextColor(dc, palette_.text);
            RECT t{Px(kPad), Px(8), rc.right - Px(kPad), Px(36)};
            const std::wstring title = L"Add to " + name_;
            DrawTextW(dc, title.c_str(), static_cast<int>(title.size()), &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SIZE ext{};
            GetTextExtentPoint32W(dc, title.c_str(), static_cast<int>(title.size()), &ext);
            SelectObject(dc, font_);
            SetTextColor(dc, palette_.textSecondary);
            RECT hint{t.left + ext.cx + Px(12), t.top, t.right, t.bottom};
            const wchar_t* text = L"Click a tile to add it, or drag it onto the dock";
            DrawTextW(dc, text, -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, old);
            return 1;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, palette_.text);
            SetBkColor(dc, palette_.control);
            return reinterpret_cast<LRESULT>(editBrush_);
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, palette_.text);
            SetBkColor(dc, palette_.layer);
            return reinterpret_cast<LRESULT>(brush_);
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDCANCEL) {  // Escape (IsDialogMessage): customize mode ends (the owner's escape), else it hides
                if (modal_) return 0;
                if (cb_.escape) {
                    cb_.escape();
                } else {
                    CloseSelf();
                }
                return 0;
            }
            if (id == SettingsIds::kDrawerPopupSearch && code == EN_CHANGE && search_) {
                wchar_t buf[96]{};
                GetWindowTextW(search_, buf, static_cast<int>(std::size(buf)));
                filter_ = buf;
                Fill();
                return 0;
            }
            if (id >= SettingsIds::kDrawerChipAll && id <= SettingsIds::kDrawerChipApps && code == BN_CLICKED) {
                chip_ = static_cast<StripUi::DrawerFilter>(id - SettingsIds::kDrawerChipAll);
                for (int c = SettingsIds::kDrawerChipAll; c <= SettingsIds::kDrawerChipApps; ++c) {
                    CheckDlgButton(hwnd_, c, c == id ? BST_CHECKED : BST_UNCHECKED);
                }
                Fill();
                return 0;
            }
            return 0;
        }
        case WM_SIZE:
            Layout();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            ApplyStyle();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        case WM_DPICHANGED:
            dpi_ = HIWORD(wp);
            ApplyStyle();
            Layout();
            return 0;
        case WM_CLOSE:  // the deactivation above (a dialog may have opened since it was posted)
            if (!modal_) CloseSelf();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
