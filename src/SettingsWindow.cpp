#include "SettingsWindow.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <iterator>

#include "Backdrop.h"
#include "SettingsIds.h"
#include "SettingsPreview.h"
#include "resource.h"

namespace {

bool FontFamilyExists(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    EnumFontFamiliesExW(
        dc, &lf,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) -> int {
            *reinterpret_cast<bool*>(lp) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

std::wstring WindowText(HWND hwnd) {
    const int n = GetWindowTextLengthW(hwnd);
    if (n <= 0) return {};
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    s.resize(static_cast<size_t>(GetWindowTextW(hwnd, s.data(), n + 1)));
    return s;
}

// An editable combo box selects all of its edit text on WM_SETTEXT, on resizing and on a theme
// change, and the selection stays highlighted while it does not have the focus: clear it.
void UnselectComboText(const Ctl& c) {
    if (c.kind != CtlKind::Combo || !c.hwnd || (GetWindowLongW(c.hwnd, GWL_STYLE) & 0x3) != CBS_DROPDOWN) return;
    HWND focus = GetFocus();
    if (focus == c.hwnd || (focus && IsChild(c.hwnd, focus))) return;
    SendMessageW(c.hwnd, CB_SETEDITSEL, 0, MAKELPARAM(-1, 0));
}

}  // namespace

// --- SettingsPage ----------------------------------------------------------------------------

SettingsPage::SettingsPage(SettingsWindow& window, const wchar_t* name, wchar_t glyph)
    : window_(window), name_(name), glyph_(glyph) {}

HWND SettingsPage::Item(int id) const {
    const Ctl* c = Find(id);
    return c ? c->hwnd : nullptr;
}

const Ctl* SettingsPage::Find(int id) const {
    for (const Ctl& c : table_) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

const Ctl* SettingsPage::Find(HWND hwnd) const {
    if (!hwnd) return nullptr;
    for (const Ctl& c : table_) {
        if (c.hwnd == hwnd) return &c;
    }
    return nullptr;
}

int SettingsPage::Width() const { return SettingsWindow::ContentWidthDip(); }

Ctl& SettingsPage::Add(int id, CtlKind kind, const wchar_t* cls, std::wstring text, DWORD style, RECT dip) {
    Ctl c;
    c.id = id;
    c.kind = kind;
    c.cls = cls;
    c.text = std::move(text);
    c.style = style | WS_GROUP;  // every control is its own group except radios 2..n (Radios)
    c.dip = dip;
    table_.push_back(std::move(c));
    return table_.back();
}

void SettingsPage::Title(const wchar_t* text) {
    Add(SettingsIds::kPageTitle, CtlKind::Title, WC_STATICW, text, SS_LEFT | SS_NOPREFIX,
        RECT{kMargin, 14, Width() - kMargin, 56});
    y_ = 64;
}

void SettingsPage::Section(int id, const wchar_t* text) {
    if (y_ > 64) y_ += 10;
    Add(id, CtlKind::Section, WC_STATICW, text, SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE,
        RECT{kMargin, y_, Width() - kMargin, y_ + 24});
    y_ += 28;
}

void SettingsPage::Label(int id, const wchar_t* text) {
    Add(id, CtlKind::Label, WC_STATICW, text, SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS,
        RECT{kMargin, y_, kMargin + kLabelW, y_ + kRowH});
}

Ctl& SettingsPage::Text(int id, CtlKind kind, std::wstring text, int x, int w, int h) {
    const DWORD style = SS_LEFT | SS_NOPREFIX | (h == kRowH ? SS_CENTERIMAGE | SS_ENDELLIPSIS : 0);
    return Add(id, kind, WC_STATICW, std::move(text), style, RECT{x, y_, x + w, y_ + h});
}

void SettingsPage::Trackbar(int id, int x, int w, int lo, int hi, int page) {
    Ctl& c = Add(id, CtlKind::Trackbar, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_BOTH | TBS_NOTICKS,
                 RECT{x, y_ + 2, x + w, y_ + kRowH - 2});
    c.lo = lo;
    c.hi = hi;
    c.step = page;
}

void SettingsPage::Spin(int editId, int spinId, int x, int w, int lo, int hi, int step) {
    constexpr int kSpinW = 18;
    Ctl& e = Add(editId, CtlKind::Edit, WC_EDITW, L"", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                 RECT{x, y_ + 4, x + w - kSpinW, y_ + kRowH - 4});
    e.exStyle = WS_EX_CLIENTEDGE;
    e.buddy = spinId;
    e.hi = 6;  // text limit
    Ctl& s = Add(spinId, CtlKind::Spin, UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_NOTHOUSANDS | UDS_HOTTRACK,
                 RECT{x + w - kSpinW, y_ + 4, x + w, y_ + kRowH - 4});
    s.buddy = editId;
    s.lo = lo;
    s.hi = hi;
    s.step = step;
}

void SettingsPage::Combo(int id, int x, int w, bool editable) {
    const DWORD style = WS_TABSTOP | WS_VSCROLL | (editable ? CBS_DROPDOWN | CBS_AUTOHSCROLL : CBS_DROPDOWNLIST);
    // The height is the drop-down's; the selection field takes its height from the font.
    Add(id, CtlKind::Combo, WC_COMBOBOXW, L"", style, RECT{x, y_ + 4, x + w, y_ + 4 + 220});
}

void SettingsPage::Check(int id, int captionId, const wchar_t* text, int x) {
    Ctl& b = Add(id, CtlKind::Check, WC_BUTTONW, text, WS_TABSTOP | BS_AUTOCHECKBOX | BS_NOTIFY,
                 RECT{x, y_, x + kCaptionDx, y_ + kRowH});
    b.buddy = captionId;
    Ctl& c = Add(captionId, CtlKind::Caption, WC_STATICW, text, SS_OWNERDRAW | SS_NOTIFY,
                 RECT{x + kCaptionDx, y_, Width() - kMargin, y_ + kRowH});
    c.buddy = id;
}

void SettingsPage::Radios(std::initializer_list<RadioDef> radios, int x) {
    const int top = y_;
    bool first = true;
    for (const RadioDef& r : radios) {
        Ctl& b = Add(r.id, CtlKind::Radio, WC_BUTTONW, r.text, WS_TABSTOP | BS_AUTORADIOBUTTON | BS_NOTIFY,
                     RECT{x, y_, x + kCaptionDx, y_ + kRowH});
        if (!first) b.style &= ~static_cast<DWORD>(WS_GROUP);
        b.buddy = r.captionId;
        first = false;
        y_ += kRowH;
    }
    y_ = top;
    for (const RadioDef& r : radios) {
        Ctl& c = Add(r.captionId, CtlKind::Caption, WC_STATICW, r.text, SS_OWNERDRAW | SS_NOTIFY,
                     RECT{x + kCaptionDx, y_, Width() - kMargin, y_ + kRowH});
        c.buddy = r.id;
        y_ += kRowH;
    }
    y_ += kRowGap;
}

void SettingsPage::Button(int id, const wchar_t* text, int x, int w) {
    Add(id, CtlKind::Button, WC_BUTTONW, text, WS_TABSTOP | BS_PUSHBUTTON, RECT{x, y_ + 2, x + w, y_ + kRowH - 2});
}

void SettingsPage::Link(int id, std::wstring markup, int x, int w) {
    Add(id, CtlKind::Link, WC_LINK, std::move(markup), WS_TABSTOP, RECT{x, y_ + 7, x + w, y_ + kRowH - 3});
}

Config& SettingsPage::Cfg() { return *window_.Host().config; }
SettingsHost& SettingsPage::Host() { return window_.Host(); }
bool SettingsPage::Loading() const { return window_.Loading(); }

void SettingsPage::Changed(SettingsScope scope) {
    if (window_.Loading()) return;
    if (window_.Host().changed) window_.Host().changed(scope);
    window_.ConfigApplied();
}

void SettingsPage::SetText(int id, const std::wstring& text) {
    const Ctl* c = Find(id);
    if (!c || !c->hwnd || WindowText(c->hwnd) == text) return;
    SetWindowTextW(c->hwnd, text.c_str());
    UnselectComboText(*c);
}

std::wstring SettingsPage::GetText(int id) const { return WindowText(Item(id)); }

void SettingsPage::SetCheck(int id, bool checked) {
    if (HWND h = Item(id)) SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool SettingsPage::GetCheck(int id) const {
    HWND h = Item(id);
    return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void SettingsPage::SetSpin(int spinId, int value) {
    if (HWND h = Item(spinId)) SendMessageW(h, UDM_SETPOS32, 0, value);
}

int SettingsPage::GetSpin(int spinId) const {
    HWND h = Item(spinId);
    return h ? static_cast<int>(SendMessageW(h, UDM_GETPOS32, 0, 0)) : 0;
}

void SettingsPage::SetTrack(int id, int pos) {
    if (HWND h = Item(id)) SendMessageW(h, TBM_SETPOS, TRUE, pos);
}

int SettingsPage::GetTrack(int id) const {
    HWND h = Item(id);
    return h ? static_cast<int>(SendMessageW(h, TBM_GETPOS, 0, 0)) : 0;
}

void SettingsPage::Enable(int id, bool enable) {
    const Ctl* c = Find(id);
    if (!c || !c->hwnd) return;
    EnableWindow(c->hwnd, enable);
    if (c->buddy) {
        if (HWND b = Item(c->buddy)) {
            if (c->kind == CtlKind::Edit) EnableWindow(b, enable);
            InvalidateRect(b, nullptr, TRUE);  // a caption greys out with its button
        }
    }
}

void SettingsPage::Show(int id, bool show) {
    HWND h = Item(id);
    // The style bit, not IsWindowVisible: the page itself may be hidden.
    if (!h || ((GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0) == show) return;
    if (!show && GetFocus() == h) {
        // Keep the keyboard focus on the page: the next control in the tab order.
        if (HWND next = GetNextDlgTabItem(hwnd_, h, FALSE); next && next != h) SetFocus(next);
    }
    ShowWindow(h, show ? SW_SHOWNA : SW_HIDE);
}

HWND SettingsPage::Owner() const { return GetAncestor(hwnd_, GA_ROOT); }

bool SettingsPage::ReadInt(int editId, int lo, int hi, int& value) {
    const std::wstring text = GetText(editId);
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    const long v = std::wcstol(text.c_str(), &end, 10);
    if (end == text.c_str()) return false;
    value = static_cast<int>(std::clamp<long>(v, lo, hi));
    if (const Ctl* e = Find(editId); e && e->buddy) {
        SetSpin(e->buddy, value);  // also rewrites the (clamped) text
    } else {
        SetText(editId, std::to_wstring(value));
    }
    return true;
}

// --- SettingsWindow: creation ----------------------------------------------------------------

SettingsWindow::SettingsWindow(SettingsHost host) : host_(std::move(host)) {}

bool SettingsWindow::FocusCuesHidden(HWND hwnd) {
    return (SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
}

SettingsWindow::~SettingsWindow() {
    if (hwnd_) DestroyWindow(hwnd_);  // WM_DESTROY / WM_NCDESTROY release everything
}

bool SettingsWindow::RegisterClasses(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW nav{sizeof(nav)};
    nav.lpfnWndProc = NavProcThunk;
    nav.hInstance = instance;
    nav.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    nav.lpszClassName = kNavClassName;
    if (!RegisterClassExW(&nav) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW page{sizeof(page)};
    page.lpfnWndProc = PageProcThunk;
    page.hInstance = instance;
    page.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    page.lpszClassName = kPageClassName;
    if (!RegisterClassExW(&page) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    registered = true;
    return true;
}

SIZE SettingsWindow::WindowSizeFor(UINT dpi) const {
    RECT r{0, 0, MulDiv(kWidthDip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
           MulDiv(kHeightDip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)};
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, dpi);
    return SIZE{r.right - r.left, r.bottom - r.top};
}

RECT SettingsWindow::InitialRect() const {
    const SIZE s = WindowSizeFor(dpi_);
    RECT mon = host_.dockMonitor ? host_.dockMonitor() : RECT{};
    HMONITOR hmon = IsRectEmpty(&mon) ? MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)
                                      : MonitorFromRect(&mon, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hmon, &mi);
    const RECT& work = mi.rcWork;  // the full monitor while the dock hides the taskbar
    int x = work.left + (work.right - work.left - s.cx) / 2;
    int y = work.top + (work.bottom - work.top - s.cy) / 2;
    // Never over the dock: the bottom stays kAboveBarDip above the bar at rest.
    if (host_.dockBar) {
        const RECT bar = host_.dockBar();
        if (!IsRectEmpty(&bar) && bar.top > work.top) y = std::min(y, static_cast<int>(bar.top - s.cy) - Px(kAboveBarDip));
    }
    y = std::max(y, static_cast<int>(work.top));
    x = std::max(x, static_cast<int>(work.left));
    return RECT{x, y, x + s.cx, y + s.cy};
}

bool SettingsWindow::Create(HINSTANCE instance) {
    instance_ = instance;
    static bool commonControls = false;
    if (!commonControls) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS |
                                                  ICC_LINK_CLASS | ICC_LISTVIEW_CLASSES};
        commonControls = InitCommonControlsEx(&icc) != FALSE;
    }
    if (!RegisterClasses(instance)) return false;

    // Process-wide (undocumented, optional): popups of windows allowed dark follow the app mode.
    darkPopups_ = DarkMode::TryEnableDarkPopups(true);

    dpi_ = host_.dockDpi ? std::max<UINT>(host_.dockDpi(), USER_DEFAULT_SCREEN_DPI / 2) : USER_DEFAULT_SCREEN_DPI;
    RECT r = InitialRect();
    // No owner: an owned window of the topmost, non-activating dock would inherit topmost.
    CreateWindowExW(0, kClassName, L"WinDock Settings", kStyle, r.left, r.top, r.right - r.left, r.bottom - r.top,
                    nullptr, nullptr, instance, this);
    if (!hwnd_) {
        if (darkPopups_) DarkMode::TryEnableDarkPopups(false);
        darkPopups_ = false;
        return false;
    }
    // The monitor the window landed on decides its DPI; normally the dock's.
    if (const UINT actual = GetDpiForWindow(hwnd_); actual != dpi_) {
        dpi_ = actual;
        r = InitialRect();
        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    palette_ = DarkMode::CurrentPalette();
    CreateBrushes();
    UpdateIcons();

    nav_ = CreateWindowExW(0, kNavClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP, 0, 0, 0, 0, hwnd_,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kNav)), instance, this);
    pages_ = CreateSettingsPages(*this);
    for (size_t i = 0; i < pages_.size(); ++i) {
        if (!CreatePage(*pages_[i], static_cast<int>(i))) {
            DestroyWindow(hwnd_);
            return false;
        }
    }
    preview_ = std::make_unique<SettingsPreview>();
    if (!preview_->Create(instance, hwnd_, SettingsIds::kPreview, host_.config, dpi_)) {
        preview_.reset();  // the window works without it
    }
    CreateFonts();  // also hands them to the controls
    ApplyTheme();
    Layout();
    Refresh();
    active_ = -1;
    SelectPage(0);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    if (!lastFocus_) SetFocus(nav_);
    return true;
}

bool SettingsWindow::CreatePage(SettingsPage& page, int index) {
    CreateWindowExW(WS_EX_CONTROLPARENT, kPageClassName, page.Name().c_str(), WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0,
                    hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kPageBase + index)), instance_,
                    &page);
    if (!page.hwnd_) return false;
    page.Build();
    CreateControls(page);
    page.Created();
    return true;
}

void SettingsWindow::CreateControls(SettingsPage& page) {
    for (Ctl& c : page.table_) {
        c.hwnd = CreateWindowExW(c.exStyle, c.cls, c.text.c_str(), WS_CHILD | WS_VISIBLE | c.style, 0, 0, 0, 0,
                                 page.hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(c.id)), instance_, nullptr);
        if (!c.hwnd) continue;
        switch (c.kind) {
            case CtlKind::List:
                ListView_SetExtendedListViewStyle(c.hwnd, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
                SetWindowSubclass(c.hwnd, ListSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
                break;
            case CtlKind::Trackbar:
                SendMessageW(c.hwnd, TBM_SETRANGEMIN, FALSE, c.lo);
                SendMessageW(c.hwnd, TBM_SETRANGEMAX, FALSE, c.hi);
                SendMessageW(c.hwnd, TBM_SETPAGESIZE, 0, c.step);
                SendMessageW(c.hwnd, TBM_SETLINESIZE, 0, 1);
                break;
            case CtlKind::Spin: {
                SendMessageW(c.hwnd, UDM_SETBUDDY, reinterpret_cast<WPARAM>(page.Item(c.buddy)), 0);
                SendMessageW(c.hwnd, UDM_SETRANGE32, static_cast<WPARAM>(c.lo), c.hi);
                UDACCEL accel[] = {{0, static_cast<UINT>(c.step)}, {2, static_cast<UINT>(c.step * 5)}};
                SendMessageW(c.hwnd, UDM_SETACCEL, std::size(accel), reinterpret_cast<LPARAM>(accel));
                break;
            }
            case CtlKind::Edit:
                if (c.hi > 0) SendMessageW(c.hwnd, EM_SETLIMITTEXT, static_cast<WPARAM>(c.hi), 0);
                break;
            case CtlKind::Link: {
                // Links in the WM_CTLCOLORSTATIC text colour (the palette's link colour) instead
                // of the fixed dark blue, which is unreadable on the dark layer.
                LITEM item{};
                item.mask = LIF_ITEMINDEX | LIF_STATE;
                item.iLink = 0;
                item.state = item.stateMask = LIS_DEFAULTCOLORS;
                SendMessageW(c.hwnd, LM_SETITEM, 0, reinterpret_cast<LPARAM>(&item));
                break;
            }
            default:
                break;
        }
    }
}

void SettingsWindow::Activate() {
    if (!hwnd_) return;
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

void SettingsWindow::Refresh() {
    if (!hwnd_) return;
    loading_ = true;
    for (auto& page : pages_) page->Load();
    loading_ = false;
    if (preview_) preview_->Invalidate();
}

void SettingsWindow::ShowPage(int index) {
    if (!hwnd_ || index < 0 || index >= static_cast<int>(pages_.size())) return;
    SelectPage(index);
}

void SettingsWindow::WeatherChanged() {
    if (!hwnd_) return;
    loading_ = true;
    for (auto& page : pages_) page->OnWeatherChanged();
    loading_ = false;
}

void SettingsWindow::ConfigApplied() {
    if (preview_) preview_->Invalidate();
}

void SettingsWindow::StyleChanged() {
    loading_ = true;
    for (auto& page : pages_) page->OnStyleChanged();
    loading_ = false;
}

// --- resources -------------------------------------------------------------------------------

void SettingsWindow::CreateFonts() {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    LOGFONTW lf{};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_)) {
        lf = ncm.lfMessageFont;
    } else {
        lf.lfHeight = -MulDiv(9, static_cast<int>(dpi_), 72);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
    }
    HFONT oldFont = font_, oldSemibold = fontSemibold_, oldTitle = fontTitle_, oldIcons = fontIcons_;
    font_ = CreateFontIndirectW(&lf);
    LOGFONTW semibold = lf;
    semibold.lfWeight = FW_SEMIBOLD;
    fontSemibold_ = CreateFontIndirectW(&semibold);
    LOGFONTW title = semibold;
    title.lfHeight = lf.lfHeight < 0 ? lf.lfHeight * 2 : -Px(24);
    fontTitle_ = CreateFontIndirectW(&title);
    static const wchar_t* iconFace = FontFamilyExists(L"Segoe Fluent Icons")  ? L"Segoe Fluent Icons"
                                     : FontFamilyExists(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets"
                                                                              : nullptr;
    fontIcons_ = nullptr;
    if (iconFace) {
        LOGFONTW icons{};
        icons.lfHeight = -Px(16);
        icons.lfWeight = FW_NORMAL;
        icons.lfCharSet = DEFAULT_CHARSET;
        icons.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(icons.lfFaceName, iconFace);
        fontIcons_ = CreateFontIndirectW(&icons);
    }
    for (auto& page : pages_) {
        for (const Ctl& c : page->table_) {
            if (!c.hwnd) continue;
            HFONT f = c.kind == CtlKind::Title ? fontTitle_ : c.kind == CtlKind::Section ? fontSemibold_ : font_;
            SendMessageW(c.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
        }
    }
    for (HFONT f : {oldFont, oldSemibold, oldTitle, oldIcons}) {
        if (f) DeleteObject(f);
    }
}

void SettingsWindow::DestroyFonts() {
    for (HFONT* f : {&font_, &fontSemibold_, &fontTitle_, &fontIcons_}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

void SettingsWindow::CreateBrushes() {
    DestroyBrushes();
    layerBrush_ = CreateSolidBrush(palette_.layer);
    baseBrush_ = CreateSolidBrush(palette_.base);
    controlBrush_ = CreateSolidBrush(palette_.control);
}

void SettingsWindow::DestroyBrushes() {
    for (HBRUSH* b : {&layerBrush_, &baseBrush_, &controlBrush_}) {
        if (*b) DeleteObject(*b);
        *b = nullptr;
    }
}

void SettingsWindow::UpdateIcons() {
    HICON iconSmall = nullptr, iconBig = nullptr;
    LoadIconWithScaleDown(instance_, MAKEINTRESOURCEW(IDI_APP), GetSystemMetricsForDpi(SM_CXSMICON, dpi_),
                          GetSystemMetricsForDpi(SM_CYSMICON, dpi_), &iconSmall);
    LoadIconWithScaleDown(instance_, MAKEINTRESOURCEW(IDI_APP), GetSystemMetricsForDpi(SM_CXICON, dpi_),
                          GetSystemMetricsForDpi(SM_CYICON, dpi_), &iconBig);
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall));
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig));
    DestroyIcons();
    iconSmall_ = iconSmall;
    iconBig_ = iconBig;
}

void SettingsWindow::DestroyIcons() {
    if (iconSmall_) DestroyIcon(iconSmall_);
    if (iconBig_) DestroyIcon(iconBig_);
    iconSmall_ = iconBig_ = nullptr;
}

void SettingsWindow::ApplyTheme() {
    palette_ = DarkMode::CurrentPalette();
    const bool dark = palette_.dark;
    CreateBrushes();
    if (darkPopups_) {
        DarkMode::TryEnableDarkPopups(true);  // flushes the menu themes
        DarkMode::AllowDarkForWindow(hwnd_, dark);
    }
    DarkMode::SetDarkCaption(hwnd_, dark);
    ApplyBackdrop();
    for (auto& page : pages_) {
        if (!page->hwnd_) continue;
        DarkMode::ThemeScrollBars(page->hwnd_, dark);
        for (const Ctl& c : page->table_) {
            if (!c.hwnd) continue;
            switch (c.kind) {
                case CtlKind::Button:
                case CtlKind::Check:
                case CtlKind::Radio:
                case CtlKind::Edit:
                case CtlKind::Spin:
                case CtlKind::Combo:
                case CtlKind::Link:
                    DarkMode::ThemeControl(c.hwnd, dark);
                    break;
                case CtlKind::Trackbar:
                    // No dark theme (custom-drawn in dark mode: DrawTrackbar), but a trackbar
                    // caches its background brush: a theme reset makes it ask WM_CTLCOLORSTATIC again.
                    SetWindowTheme(c.hwnd, nullptr, nullptr);
                    break;
                case CtlKind::List:
                    ThemeList(c.hwnd);
                    break;
                default:
                    break;
            }
            UnselectComboText(c);
        }
    }
    if (preview_) {
        // The strip sits on the content layer, with a hairline between it and the page.
        const COLORREF a = palette_.layer, b = palette_.text;
        auto mix = [](BYTE x, BYTE y) { return static_cast<BYTE>((x * 88 + y * 12) / 100); };
        preview_->SetColors(a, RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
                                   mix(GetBValue(a), GetBValue(b))));
    }
    StyleChanged();
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void SettingsWindow::ThemeList(HWND list) {
    const bool dark = palette_.dark;
    DarkMode::AllowDarkForWindow(list, dark);
    SetWindowTheme(list, dark ? L"DarkMode_ItemsView" : L"Explorer", nullptr);
    if (HWND header = ListView_GetHeader(list)) {
        DarkMode::AllowDarkForWindow(header, dark);
        SetWindowTheme(header, dark ? L"DarkMode_ItemsView" : L"Explorer", nullptr);
    }
    ListView_SetBkColor(list, palette_.control);
    ListView_SetTextBkColor(list, CLR_NONE);  // rows on the list background (no lighter bands)
    ListView_SetTextColor(list, palette_.text);
    InvalidateRect(list, nullptr, TRUE);
}

LRESULT CALLBACK SettingsWindow::ListSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                                  DWORD_PTR data) {
    auto* self = reinterpret_cast<SettingsWindow*>(data);
    if (msg == WM_NOTIFY && self && self->palette_.dark) {
        // The header's dark theme keeps dark item text: draw it in the palette's text colour.
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == ListView_GetHeader(hwnd)) {
            auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(cd->hdc, self->palette_.text);
                return CDRF_DODEFAULT;
            }
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, ListSubclassProc, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT SettingsWindow::DrawTrackbar(NMCUSTOMDRAW& cd) {
    // Trackbars have no dark visual style: paint the channel and the thumb (Windows 11 shapes).
    if (!palette_.dark || palette_.highContrast) return CDRF_DODEFAULT;
    if (cd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    HWND tb = cd.hdr.hwndFrom;
    const bool enabled = IsWindowEnabled(tb) != FALSE;
    const COLORREF accent = enabled ? palette_.accent : palette_.textDisabled;
    auto fill = [&](const RECT& r, COLORREF color, int radius) {
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ oldBrush = SelectObject(cd.hdc, brush);
        HGDIOBJ oldPen = SelectObject(cd.hdc, pen);
        if (radius > 0) {
            RoundRect(cd.hdc, r.left, r.top, r.right, r.bottom, radius, radius);
        } else {
            Ellipse(cd.hdc, r.left, r.top, r.right, r.bottom);
        }
        SelectObject(cd.hdc, oldBrush);
        SelectObject(cd.hdc, oldPen);
        DeleteObject(pen);
        DeleteObject(brush);
    };
    RECT thumb{};
    SendMessageW(tb, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    const int thumbX = (thumb.left + thumb.right) / 2;
    switch (cd.dwItemSpec) {
        case TBCD_CHANNEL: {
            const int h = std::max(2, Px(4));
            const int cy = (cd.rc.top + cd.rc.bottom) / 2;
            const RECT left{cd.rc.left, cy - h / 2, std::max(static_cast<int>(cd.rc.left) + h, thumbX), cy - h / 2 + h};
            const RECT right{std::min(static_cast<int>(cd.rc.right) - h, thumbX), left.top, cd.rc.right, left.bottom};
            fill(right, RGB(0x9A, 0x9A, 0x9A), h);
            fill(left, accent, h);
            return CDRF_SKIPDEFAULT;
        }
        case TBCD_THUMB: {
            RECT client{};
            GetClientRect(tb, &client);
            const int d = std::min({Px(18), static_cast<int>(client.bottom - client.top),
                                    static_cast<int>(thumb.right - thumb.left) + Px(8)});
            const int cy = (thumb.top + thumb.bottom) / 2;
            const RECT outer{thumbX - d / 2, cy - d / 2, thumbX - d / 2 + d, cy - d / 2 + d};
            fill(outer, RGB(0x45, 0x45, 0x45), 0);
            const bool hot = (cd.uItemState & (CDIS_HOT | CDIS_SELECTED)) != 0;
            const int inner = std::max(4, d * (hot ? 6 : 5) / 10);
            const RECT in{thumbX - inner / 2, cy - inner / 2, thumbX - inner / 2 + inner, cy - inner / 2 + inner};
            fill(in, accent, 0);
            return CDRF_SKIPDEFAULT;
        }
        case TBCD_TICS:
            return CDRF_SKIPDEFAULT;
        default:
            return CDRF_DODEFAULT;
    }
}

void SettingsWindow::ApplyBackdrop() {
    // Mica shows only where the client area is transparent (frame extended), and GDI controls
    // cannot paint alpha: so Mica behind the caption and the nav column only.
    const bool want = kNavMica && !palette_.highContrast && Backdrop::SystemAllowsTransparency();
    bool mica = want && DarkMode::SetMicaBackdrop(hwnd_, true);
    if (!mica) DarkMode::SetMicaBackdrop(hwnd_, false);
    MARGINS margins{mica ? Px(kNavWidthDip) : 0, 0, 0, 0};
    if (FAILED(DwmExtendFrameIntoClientArea(hwnd_, &margins)) && mica) {
        mica = false;
        DarkMode::SetMicaBackdrop(hwnd_, false);
    }
    mica_ = mica;
    if (nav_) InvalidateRect(nav_, nullptr, TRUE);
}

int SettingsWindow::CheckGlyphPx(bool radio) const {
    SIZE s{};
    if (HTHEME theme = OpenThemeDataForDpi(hwnd_, L"BUTTON", dpi_)) {
        GetThemePartSize(theme, nullptr, radio ? BP_RADIOBUTTON : BP_CHECKBOX, 1, nullptr, TS_DRAW, &s);
        CloseThemeData(theme);
    }
    if (s.cx <= 0) s.cx = GetSystemMetricsForDpi(SM_CXMENUCHECK, dpi_);
    return s.cx;
}

// --- layout ----------------------------------------------------------------------------------

void SettingsWindow::Layout() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int navW = Px(kNavWidthDip);
    SetWindowPos(nav_, nullptr, 0, 0, navW, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    if (preview_) {
        const int footer = std::min(Px(kFooterDip), static_cast<int>(rc.bottom));
        SetWindowPos(preview_->Hwnd(), nullptr, navW, rc.bottom - footer, std::max(0, static_cast<int>(rc.right) - navW),
                     footer, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (auto& page : pages_) {
        if (!page->hwnd_) continue;
        SetWindowPos(page->hwnd_, nullptr, navW, 0, rc.right - navW, std::max(0, static_cast<int>(rc.bottom) - Px(kFooterDip)),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutPage(*page);
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void SettingsWindow::LayoutPage(SettingsPage& page) {
    int bottom = 0;
    for (const Ctl& c : page.table_) {
        const int b = c.kind == CtlKind::Combo ? c.dip.top + SettingsPage::kRowH - 4 : c.dip.bottom;
        bottom = std::max(bottom, Px(b));
    }
    page.contentPx_ = bottom + Px(SettingsPage::kMargin);
    RECT pr{};
    GetWindowRect(page.hwnd_, &pr);
    const int view = pr.bottom - pr.top;
    page.scrollPx_ = std::clamp(page.scrollPx_, 0, std::max(0, page.contentPx_ - view));
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, page.contentPx_ - 1),
                  static_cast<UINT>(std::max(0, view)), page.scrollPx_, 0};
    SetScrollInfo(page.hwnd_, SB_VERT, &si, TRUE);

    const int checkPx = CheckGlyphPx(false) + 1;
    const int radioPx = CheckGlyphPx(true) + 1;
    HDC dc = GetDC(page.hwnd_);
    HGDIOBJ oldFont = dc ? SelectObject(dc, font_) : nullptr;
    HDWP dwp = BeginDeferWindowPos(static_cast<int>(page.table_.size()));
    for (const Ctl& c : page.table_) {
        if (!c.hwnd) continue;
        RECT r{Px(c.dip.left), Px(c.dip.top) - page.scrollPx_, Px(c.dip.right), Px(c.dip.bottom) - page.scrollPx_};
        if (c.kind == CtlKind::Check) r.right = r.left + checkPx;
        if (c.kind == CtlKind::Radio) r.right = r.left + radioPx;
        if (c.kind == CtlKind::Caption && dc) {
            // Clickable only over its text (plus a little), like a real check box caption.
            SIZE ext{};
            GetTextExtentPoint32W(dc, c.text.c_str(), static_cast<int>(c.text.size()), &ext);
            r.right = std::min(r.right, r.left + ext.cx + Px(6));
        }
        if (dwp) {
            dwp = DeferWindowPos(dwp, c.hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPos(c.hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (dwp) EndDeferWindowPos(dwp);
    for (const Ctl& c : page.table_) UnselectComboText(c);
    if (dc) {
        SelectObject(dc, oldFont);
        ReleaseDC(page.hwnd_, dc);
    }
}

void SettingsWindow::ScrollPage(SettingsPage& page, int px) {
    RECT pr{};
    GetWindowRect(page.hwnd_, &pr);
    px = std::clamp(px, 0, std::max(0, page.contentPx_ - static_cast<int>(pr.bottom - pr.top)));
    if (px == page.scrollPx_) return;
    page.scrollPx_ = px;
    LayoutPage(page);
    RedrawWindow(page.hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void SettingsWindow::EnsureVisible(HWND control) {
    SettingsPage* page = PageOf(control);
    if (!page) return;
    const Ctl* c = page->Find(control);
    if (!c) c = page->Find(GetParent(control));  // a combo box's edit
    if (!c) return;
    RECT pr{};
    GetWindowRect(page->hwnd_, &pr);
    const int view = pr.bottom - pr.top;
    const int top = Px(c->dip.top) - Px(8);
    const int bottom = Px(c->kind == CtlKind::Combo ? c->dip.top + SettingsPage::kRowH : c->dip.bottom) + Px(8);
    if (top < page->scrollPx_) {
        ScrollPage(*page, top);
    } else if (bottom > page->scrollPx_ + view) {
        ScrollPage(*page, bottom - view);
    }
}

SettingsPage* SettingsWindow::ActivePage() const {
    return active_ >= 0 && active_ < static_cast<int>(pages_.size()) ? pages_[static_cast<size_t>(active_)].get() : nullptr;
}

SettingsPage* SettingsWindow::PageOf(HWND control) const {
    for (HWND h = control; h && h != hwnd_; h = GetParent(h)) {
        for (const auto& page : pages_) {
            if (page->hwnd_ == h) return page.get();
        }
    }
    return nullptr;
}

void SettingsWindow::SelectPage(int index) {
    index = std::clamp(index, 0, static_cast<int>(pages_.size()) - 1);
    if (index == active_) return;
    HWND focus = GetFocus();
    SettingsPage* old = ActivePage();
    const bool focusInOld = old && focus && IsChild(old->hwnd_, focus);
    active_ = index;
    SettingsPage* page = ActivePage();
    ShowWindow(page->hwnd_, SW_SHOWNA);
    if (old) ShowWindow(old->hwnd_, SW_HIDE);
    if (focusInOld) SetFocus(nav_);
    SetWindowTextW(nav_, page->Name().c_str());  // accessibility: the nav's name is the selection
    InvalidateRect(nav_, nullptr, TRUE);
}

void SettingsWindow::OnEnterKey() {
    HWND focus = GetFocus();
    SettingsPage* page = PageOf(focus);
    if (!page || loading_) return;
    const Ctl* c = page->Find(focus);
    if (!c) c = page->Find(GetParent(focus));  // the edit inside an editable combo box
    if (c && (c->kind == CtlKind::Edit || c->kind == CtlKind::Combo)) page->OnCommit(c->id);
}

bool SettingsWindow::PreTranslateMessage(MSG& msg) {
    if (!hwnd_ || !msg.hwnd || (msg.hwnd != hwnd_ && !IsChild(hwnd_, msg.hwnd))) return false;
    if (!IsDialogMessageW(hwnd_, &msg)) return false;
    if (hwnd_ && msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) EnsureVisible(GetFocus());
    return true;
}

// --- window procedures -----------------------------------------------------------------------

LRESULT CALLBACK SettingsWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(msg, wp, lp);
}

void SettingsWindow::RequestRebuild(const SettingsPage& page) {
    for (size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].get() == &page && hwnd_) PostMessageW(hwnd_, kRebuildMessage, i, 0);
    }
}

void SettingsWindow::ShowWidgetSettings(int item) {
    if (!hwnd_ || SettingsIds::kPageWidgetSettings >= static_cast<int>(pages_.size())) return;
    pages_[SettingsIds::kPageWidgetSettings]->SelectItem(item);
    RebuildPage(SettingsIds::kPageWidgetSettings);
    SelectPage(SettingsIds::kPageWidgetSettings);
}

void SettingsWindow::ShowItemSettings(int item) {
    if (!hwnd_ || SettingsIds::kPageItemSettings >= static_cast<int>(pages_.size())) return;
    pages_[SettingsIds::kPageItemSettings]->SelectItem(item);
    RebuildPage(SettingsIds::kPageItemSettings);
    SelectPage(SettingsIds::kPageItemSettings);
}

void SettingsWindow::RebuildPage(int index) {
    if (!hwnd_ || index < 0 || index >= static_cast<int>(pages_.size())) return;
    SettingsPage& page = *pages_[static_cast<size_t>(index)];
    if (!page.hwnd_) return;
    if (HWND f = GetFocus(); f && IsChild(page.hwnd_, f)) SetFocus(nav_);
    if (lastFocus_ && IsChild(page.hwnd_, lastFocus_)) lastFocus_ = nullptr;
    for (Ctl& c : page.table_) {
        if (c.hwnd) DestroyWindow(c.hwnd);
    }
    page.table_.clear();
    page.y_ = 0;
    page.scrollPx_ = 0;
    page.Build();
    CreateControls(page);
    page.Created();
    const bool dark = palette_.dark;
    DarkMode::ThemeScrollBars(page.hwnd_, dark);
    for (const Ctl& c : page.table_) {
        if (!c.hwnd) continue;
        HFONT f = c.kind == CtlKind::Title ? fontTitle_ : c.kind == CtlKind::Section ? fontSemibold_ : font_;
        SendMessageW(c.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
        switch (c.kind) {
            case CtlKind::Button:
            case CtlKind::Check:
            case CtlKind::Radio:
            case CtlKind::Edit:
            case CtlKind::Spin:
            case CtlKind::Combo:
            case CtlKind::Link:
                DarkMode::ThemeControl(c.hwnd, dark);
                break;
            case CtlKind::Trackbar:
                SetWindowTheme(c.hwnd, nullptr, nullptr);
                break;
            case CtlKind::List:
                ThemeList(c.hwnd);
                break;
            default:
                break;
        }
    }
    LayoutPage(page);
    loading_ = true;
    page.Load();
    loading_ = false;
    page.OnStyleChanged();
    RedrawWindow(page.hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    InvalidateRect(nav_, nullptr, TRUE);  // a page name may have changed
}

LRESULT SettingsWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    HWND hwnd = hwnd_;
    if (msg == kRebuildMessage) {
        RebuildPage(static_cast<int>(wp));
        return 0;
    }
    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                if (HWND f = GetFocus(); f && IsChild(hwnd_, f)) lastFocus_ = f;
                if (host_.keepDockShown) host_.keepDockShown(false);
            } else {
                // Active: the dock stays shown so changes are visible (auto-hide resumes once
                // another window is activated).
                if (host_.keepDockShown) host_.keepDockShown(!HIWORD(wp));  // not while minimized
                if (!HIWORD(wp)) {
                    HWND f = lastFocus_ && IsWindow(lastFocus_) && IsWindowVisible(lastFocus_) &&
                                     IsWindowEnabled(lastFocus_)
                                 ? lastFocus_
                                 : nav_;
                    SetFocus(f);
                }
            }
            return 0;

        case WM_SETFOCUS:
            SetFocus(lastFocus_ && IsWindow(lastFocus_) && IsWindowVisible(lastFocus_) ? lastFocus_ : nav_);
            return 0;

        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL) {  // Escape (IsDialogMessage)
                PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                return 0;
            }
            if (LOWORD(wp) == IDOK) {  // Enter outside a push button: commit the focused edit
                OnEnterKey();
                return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        case WM_DPICHANGED: {
            const UINT old = dpi_;
            dpi_ = HIWORD(wp);
            for (auto& page : pages_) page->scrollPx_ = MulDiv(page->scrollPx_, static_cast<int>(dpi_), static_cast<int>(old));
            CreateFonts();
            UpdateIcons();
            if (preview_) preview_->SetDpi(dpi_);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyBackdrop();  // the nav width in pixels changed
            Layout();
            StyleChanged();  // image lists at the new DPI
            RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
        }

        case WM_GETDPISCALEDSIZE: {
            // Exact: the client stays kWidthDip x kHeightDip (the caption does not scale linearly).
            *reinterpret_cast<SIZE*>(lp) = WindowSizeFor(static_cast<UINT>(wp));
            return TRUE;
        }

        case WM_SETTINGCHANGE:
            if (wp == SPI_SETHIGHCONTRAST ||
                (lp && CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lp), -1, L"ImmersiveColorSet", -1,
                                            TRUE) == CSTR_EQUAL)) {
                ApplyTheme();  // app mode, accent, transparency or high contrast changed
            } else if (wp == SPI_SETNONCLIENTMETRICS) {
                CreateFonts();
                Layout();
            }
            loading_ = true;
            for (auto& page : pages_) page->OnSettingChange(wp, lp);
            loading_ = false;
            return 0;

        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            ApplyTheme();
            return 0;

        case WM_DWMCOMPOSITIONCHANGED:
            ApplyBackdrop();
            return 0;

        case WM_DISPLAYCHANGE:
            loading_ = true;
            for (auto& page : pages_) page->OnDisplayChange();
            loading_ = false;
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            RECT nav{0, 0, Px(kNavWidthDip), rc.bottom};
            FillRect(dc, &nav, mica_ ? static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)) : baseBrush_);
            RECT content{nav.right, 0, rc.right, rc.bottom};
            FillRect(dc, &content, layerBrush_);
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_DESTROY:
            if (host_.keepDockShown) host_.keepDockShown(false);
            return 0;

        case WM_NCDESTROY: {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            hwnd_ = nullptr;
            nav_ = nullptr;
            lastFocus_ = nullptr;
            for (auto& page : pages_) {
                page->hwnd_ = nullptr;
                for (Ctl& c : page->table_) c.hwnd = nullptr;
            }
            preview_.reset();  // its window (a child) is already gone, with its D2D objects
            DestroyFonts();
            DestroyBrushes();
            DestroyIcons();
            if (darkPopups_) DarkMode::TryEnableDarkPopups(false);  // the dock's menus as before
            darkPopups_ = false;
            if (host_.destroyed) host_.destroyed();
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --- pages -----------------------------------------------------------------------------------

LRESULT CALLBACK SettingsWindow::PageProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsPage* page = nullptr;
    if (msg == WM_NCCREATE) {
        page = static_cast<SettingsPage*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        page->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(page));
    } else {
        page = reinterpret_cast<SettingsPage*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!page) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        page->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return page->window_.PageProc(*page, msg, wp, lp);
}

LRESULT SettingsWindow::CtlColor(UINT msg, HDC hdc, HWND control, const SettingsPage& page) {
    const Ctl* c = page.Find(control);
    COLORREF text = palette_.text;
    COLORREF back = palette_.layer;
    HBRUSH brush = layerBrush_;
    const bool enabled = IsWindowEnabled(control) != FALSE;
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX || (c && c->kind == CtlKind::Edit) ||
        (!c && msg == WM_CTLCOLORSTATIC)) {
        // Edits (read-only / disabled ones send CTLCOLORSTATIC), combo boxes' edits and lists.
        back = palette_.control;
        brush = controlBrush_;
    } else if (c && c->kind == CtlKind::Note) {
        text = palette_.textSecondary;
    } else if (c && c->kind == CtlKind::Link) {
        text = palette_.link;
    }
    if (!enabled) text = palette_.textDisabled;
    SetTextColor(hdc, text);
    SetBkColor(hdc, back);
    return reinterpret_cast<LRESULT>(brush);
}

void SettingsWindow::DrawCaption(const SettingsPage& page, const DRAWITEMSTRUCT& di) {
    const Ctl* c = page.Find(di.hwndItem);
    HWND button = c ? page.Item(c->buddy) : nullptr;
    const bool enabled = !button || IsWindowEnabled(button);
    FillRect(di.hDC, &di.rcItem, layerBrush_);
    SetBkMode(di.hDC, TRANSPARENT);
    SetTextColor(di.hDC, enabled ? palette_.text : palette_.textDisabled);
    HGDIOBJ old = SelectObject(di.hDC, font_);
    const std::wstring text = WindowText(di.hwndItem);
    RECT r = di.rcItem;
    DrawTextW(di.hDC, text.c_str(), static_cast<int>(text.size()), &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (button && GetFocus() == button && !FocusCuesHidden(button)) {
        RECT fr = di.rcItem;
        DrawTextW(di.hDC, text.c_str(), static_cast<int>(text.size()), &fr,
                  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        const int h = fr.bottom - fr.top;
        fr.top = (di.rcItem.top + di.rcItem.bottom - h) / 2 - 1;
        fr.bottom = fr.top + h + 2;
        fr.left -= 2;
        fr.right = std::min<LONG>(fr.right + 2, di.rcItem.right);
        SetTextColor(di.hDC, palette_.text);
        SetBkColor(di.hDC, palette_.layer);
        DrawFocusRect(di.hDC, &fr);
    }
    SelectObject(di.hDC, old);
}

LRESULT SettingsWindow::PageProc(SettingsPage& page, UINT msg, WPARAM wp, LPARAM lp) {
    HWND hwnd = page.hwnd_;
    switch (msg) {
        case WM_COMMAND: {
            HWND ctl = reinterpret_cast<HWND>(lp);
            const Ctl* c = page.Find(ctl);
            if (!c) break;
            const int code = HIWORD(wp);
            if (c->kind == CtlKind::Caption) {
                if (code == STN_CLICKED || code == STN_DBLCLK) {
                    HWND button = page.Item(c->buddy);
                    if (button && IsWindowEnabled(button)) SendMessageW(button, BM_CLICK, 0, 0);
                }
                return 0;
            }
            if ((c->kind == CtlKind::Check || c->kind == CtlKind::Radio) && (code == BN_SETFOCUS || code == BN_KILLFOCUS)) {
                if (HWND caption = page.Item(c->buddy)) InvalidateRect(caption, nullptr, TRUE);
                return 0;
            }
            if (loading_) return 0;
            if (c->kind == CtlKind::Edit && code == EN_KILLFOCUS) {
                page.OnCommit(c->id);
                return 0;
            }
            if (c->kind == CtlKind::Combo && code == CBN_KILLFOCUS &&
                (GetWindowLongW(ctl, GWL_STYLE) & 0x3) == CBS_DROPDOWN) {
                page.OnCommit(c->id);
                return 0;
            }
            page.OnCommand(c->id, code, ctl);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->code == NM_CUSTOMDRAW) {
                if (const Ctl* c = page.Find(nm->hwndFrom); c && c->kind == CtlKind::Trackbar) {
                    return DrawTrackbar(*reinterpret_cast<NMCUSTOMDRAW*>(lp));
                }
            }
            if (loading_) return 0;
            return page.OnNotify(*nm);
        }
        case WM_HSCROLL:
        case WM_VSCROLL:
            if (lp) {
                if (const Ctl* c = page.Find(reinterpret_cast<HWND>(lp)); c && !loading_) page.OnScroll(c->id, LOWORD(wp));
                return 0;
            }
            if (msg == WM_VSCROLL) {
                SCROLLINFO si{sizeof(si), SIF_ALL};
                GetScrollInfo(hwnd, SB_VERT, &si);
                int pos = page.scrollPx_;
                switch (LOWORD(wp)) {
                    case SB_LINEUP: pos -= Px(32); break;
                    case SB_LINEDOWN: pos += Px(32); break;
                    case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
                    case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
                    case SB_THUMBTRACK:
                    case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                    case SB_TOP: pos = 0; break;
                    case SB_BOTTOM: pos = page.contentPx_; break;
                    default: break;
                }
                ScrollPage(page, pos);
            }
            return 0;
        case WM_MOUSEWHEEL:
            ScrollPage(page, page.scrollPx_ - MulDiv(GET_WHEEL_DELTA_WPARAM(wp), Px(48), WHEEL_DELTA));
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
            return CtlColor(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), page);
        case WM_DRAWITEM: {
            const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (di->CtlType == ODT_STATIC) {
                DrawCaption(page, *di);
                return TRUE;
            }
            if (di->CtlType == ODT_BUTTON && page.OnDrawItem(*di)) return TRUE;
            break;
        }
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, layerBrush_);
            return 1;
        }
        case WM_PRINTCLIENT: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, layerBrush_);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
