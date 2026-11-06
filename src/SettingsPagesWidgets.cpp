// The Widgets page (installed widgets, Console, export) and the Widget settings page (one widget,
// clock or weather item's settings, generated from its schema; round 5: the Place and TimeZone
// fields, the weather place search of the retired Weather page).

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "Clock.h"
#include "ContextMenu.h"
#include "JsonUtil.h"
#include "SettingsForm.h"
#include "SettingsIds.h"
#include "Weather.h"
#include "WidgetPackage.h"

namespace {

using namespace SettingsIds;
namespace json = winrt::Windows::Data::Json;

constexpr int kListH = 180;    // round 8 phase 8D: 220 until then (the page fits 760 x 560)
constexpr int kButtonW = 140;  // round 8 phase 8A: three in a row fit the minimum width
constexpr int kButtonStep = 146;
constexpr int kMoreW = 96;     // round 8 phase 8D: "More ▾"
constexpr int kDefaultItemW = 600;  // an Item window's default width (DIPs): what a field's hint is measured for
constexpr UINT_PTR kPlaceTimer = 1; // round 8 phase 8C: the place search's typing pause
constexpr UINT kPlacePauseMs = 400;

// --- the library's preview panel (round 6 phase 6E) ------------------------------------------------
// WinDock.WidgetPreviewPanel: the selected widget's live preview (SettingsWindow::Preview, fitted into
// 240 x 120 DIPs over the theme-coloured panel) and its name and description beside it. GDI,
// double-buffered, palette colours, no timers.

class PreviewPanel {
public:
    static constexpr wchar_t kClass[] = L"WinDock.WidgetPreviewPanel";
    static constexpr int kPreviewW = 240, kPreviewH = 120;  // DIPs
    static constexpr float kZoom = 2.0f;  // a small widget is drawn up to twice its dock size

    static void Register() {
        static bool registered = false;
        if (registered) return;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &PreviewPanel::Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    explicit PreviewPanel(SettingsWindow& window) : window_(window) {}
    ~PreviewPanel() {
        if (bold_) DeleteObject(bold_);
    }
    PreviewPanel(const PreviewPanel&) = delete;
    PreviewPanel& operator=(const PreviewPanel&) = delete;

    void Attach(HWND hwnd) {
        hwnd_ = hwnd;
        if (hwnd) SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    // The widget shown (id "": none).
    void Set(const std::wstring& id, const std::wstring& name, const std::wstring& description) {
        if (id == id_ && name == name_ && description == description_) return;
        id_ = id;
        name_ = name;
        description_ = description;
        if (hwnd_) {
            SetWindowTextW(hwnd_, (L"Preview: " + name_).c_str());
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    static SIZE Fit(const SettingsWindow& w) {
        return SIZE{w.Px(kPreviewW) - 2 * std::max(1, w.Px(3)), w.Px(kPreviewH) - 2 * std::max(1, w.Px(3))};
    }
    // Automation: the shown preview's size (MAKELONG(w, h) px; 0: none / failed).
    LRESULT ShownSize() {
        if (id_.empty()) return 0;
        const SettingsWindow::PreviewImage* img = window_.Preview(id_, Fit(window_), true, kZoom);
        return img ? static_cast<LRESULT>(MAKELONG(static_cast<WORD>(img->px.cx), static_cast<WORD>(img->px.cy))) : 0;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<PreviewPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
            case WM_NCDESTROY:
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                self->hwnd_ = nullptr;
                break;
            case WM_SETFONT: {
                self->font_ = reinterpret_cast<HFONT>(wp);
                if (self->bold_) DeleteObject(self->bold_);
                self->bold_ = nullptr;
                LOGFONTW lf{};
                if (self->font_ && GetObjectW(self->font_, sizeof(lf), &lf)) {
                    lf.lfWeight = FW_SEMIBOLD;
                    self->bold_ = CreateFontIndirectW(&lf);
                }
                if (lp) InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            case WM_GETFONT:
                return reinterpret_cast<LRESULT>(self->font_);
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_GETDLGCODE:
                return DLGC_STATIC;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT rc{};
                GetClientRect(hwnd, &rc);
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
                HGDIOBJ old = SelectObject(mem, bmp);
                self->Paint(mem, rc);
                BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
                EndPaint(hwnd, &ps);
                return 0;
            }
            default:
                break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void Paint(HDC dc, const RECT& rc) {
        const DarkMode::Palette& p = window_.Colors();
        const COLORREF under = window_.SurfaceColor(hwnd_);  // a card's fill (round 8 phase 8A)
        HBRUSH back = CreateSolidBrush(under);
        FillRect(dc, &rc, back);
        DeleteObject(back);
        SetBkMode(dc, TRANSPARENT);
        HFONT base = font_ ? font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const int pw = window_.Px(kPreviewW), ph = window_.Px(kPreviewH);
        const int top = std::max(0, static_cast<int>(rc.bottom - ph) / 2);
        const RECT area{0, top, pw, top + ph};
        if (id_.empty()) {
            HGDIOBJ of = SelectObject(dc, base);
            SetTextColor(dc, p.textSecondary);
            RECT t = rc;
            DrawTextW(dc, L"Select a widget to see its preview", -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, of);
            return;
        }
        if (const SettingsWindow::PreviewImage* img = window_.Preview(id_, Fit(window_), true, kZoom)) {
            window_.PaintPreview(dc, area, *img, under);
        } else {
            // No preview (the widget failed): a dotted frame with a word in it.
            HPEN pen = CreatePen(PS_DOT, 1, p.textSecondary);
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            RoundRect(dc, area.left, area.top, area.right, area.bottom, window_.Px(12), window_.Px(12));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(pen);
            HGDIOBJ of = SelectObject(dc, base);
            SetTextColor(dc, p.textSecondary);
            RECT t = area;
            DrawTextW(dc, L"No preview", -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, of);
        }
        // The name and the description to the right.
        const int x = pw + window_.Px(16);
        RECT name{x, top, rc.right, top + window_.Px(22)};
        HGDIOBJ of = SelectObject(dc, bold_ ? bold_ : base);
        SetTextColor(dc, p.text);
        DrawTextW(dc, name_.c_str(), static_cast<int>(name_.size()), &name, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, base);
        SetTextColor(dc, p.textSecondary);
        RECT desc{x, top + window_.Px(26), rc.right, top + ph};
        DrawTextW(dc, description_.c_str(), static_cast<int>(description_.size()), &desc,
                  DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
        SelectObject(dc, of);
    }

    SettingsWindow& window_;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;  // the window's (not owned)
    HFONT bold_ = nullptr;
    std::wstring id_, name_, description_;
};

// --- Widgets -------------------------------------------------------------------------------------

class WidgetsPage : public FormPage {
public:
    explicit WidgetsPage(SettingsWindow& w) : FormPage(w, L"Widget library", L'\xEA86'), panel_(w) {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Widget library");
        HelpTopic(L"guide-settings.md#widget-library");
        Add(kWidgetsList, CtlKind::List, WC_LISTVIEWW, L"Installed widgets",
            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
            RECT{kMargin, y_, right, y_ + kListH});
        Tip(kWidgetsList, L"Every widget WinDock has: the built-in ones and those in your widgets folder. Select one to see its "
                          L"preview and description below, then add it to a dock or the desktop.");
        NextRow(kListH + 8);
        // Round 6 phase 6E: the selected widget's live preview and description.
        PreviewPanel::Register();
        Add(kWidgetsPreview, CtlKind::Custom, PreviewPanel::kClass, L"Preview", 0, RECT{kMargin, y_, right, y_ + 124});
        Tip(kWidgetsPreview, L"A live preview of the selected widget with sample data, and what it does.");
        NextRow(124 + 10);
        // Round 8 phase 8D: the adds, then "More ▾" (Add from folder…, Export built-in to folder,
        // Reload widgets, Open widgets folder, Console…: the old buttons' ids as its commands).
        Button(kWidgetsAddToDock, L"Add to dock", kMargin, kButtonW);
        Button(kWidgetsAddToDesktop, L"Add to the desktop", kMargin + kButtonStep, kButtonW);  // round 6 phase 6B
        // Round 9 phase 9A: greyed only with no widget selected; the tooltips say so.
        Tip(kWidgetsAddToDock, L"Adds the selected widget to a dock (with several docks: a menu of them). Select a widget "
                               L"in the list first.");
        Tip(kWidgetsAddToDesktop, L"Puts the selected widget on the desktop in a window of its own. Select a widget in the "
                                  L"list first.");
        Button(kWidgetsMore, L"More \x25BE", kMargin + 2 * kButtonStep, kMoreW);
        NextRow(kRowH + 4);
        Text(kWidgetsStatus, CtlKind::Value, L"", kMargin, right - kMargin);
        NextRow(kRowH);
        // Round 8 phase 8A: one line, the whole explanation as its tooltip.
        Text(kWidgetsNote, CtlKind::Note, L"A widget is a folder with a widget.json and a main.js (see docs/widgets.md).",
             kMargin, right - kMargin, 20);
        HelpButton(L"guide-settings.md#widget-library");  // round 9 phase 9D: the card's "?" on the note's line
        Tip(kWidgetsNote, L"A widget is a folder in " + Widgets::Directory() +
                              L" with a widget.json and a main.js (see docs/widgets.md). A folder with a built-in's id "
                              L"replaces the built-in: export one to edit it. Each widget item has its own settings window "
                              L"(right-click it on the dock > <name> settings…, or select it on its dock's Contents tab).");
        NextRow(22);
    }

    void OnResized() override { OnStyleChanged(); }  // round 8 phase 8A: the list's columns

    void Created() override {
        panel_.Attach(Item(kWidgetsPreview));
        HWND list = Item(kWidgetsList);
        if (!list) return;
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        const wchar_t* names[] = {L"Name", L"Id", L"Source", L"Version"};
        for (int i = 0; i < 4; ++i) {
            col.pszText = const_cast<wchar_t*>(names[i]);
            col.cx = 100;
            col.iSubItem = i;
            ListView_InsertColumn(list, i, &col);
        }
    }

    void OnStyleChanged() override {
        HWND list = Item(kWidgetsList);
        if (!list) return;
        const Ctl* c = Find(kWidgetsList);
        const int total = Window().Px(c->dip.right - c->dip.left) - GetSystemMetricsForDpi(SM_CXVSCROLL, Window().Dpi()) - 4;
        int widths[] = {Window().Px(170), Window().Px(130), Window().Px(110)};
        // Round 8 phase 8D: a narrow window shares the room (no horizontal scroll bar).
        if (const int room = total - Window().Px(64), fixed = widths[0] + widths[1] + widths[2]; room > 0 && fixed > room) {
            for (int& w : widths) w = w * room / fixed;
        }
        for (int i = 0; i < 3; ++i) ListView_SetColumnWidth(list, i, widths[i]);
        ListView_SetColumnWidth(list, 3, std::max(Window().Px(60), total - widths[0] - widths[1] - widths[2]));
    }

    void Load() override { Fill(); }

    bool Query(int code, LPARAM, LRESULT& out) override {
        if (code != 269) return false;  // round 6 phase 6E: the preview panel's bitmap size (MAKELONG px; 0 none)
        out = panel_.ShownSize();
        return true;
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) == kWidgetsList && nm.code == LVN_ITEMCHANGED) UpdateButtons();
        return 0;
    }

    void OnCommand(int id, int code, HWND) override {
        if (code != BN_CLICKED) return;
        const Widgets::CatalogEntry* sel = Selected();
        switch (id) {
            case kWidgetsMore:
                ShowMoreMenu();
                break;
            case kWidgetsAddToDock: {
                if (!sel) return;
                const std::wstring wid = sel->id, name = sel->name;  // Refresh refills the catalog
                const size_t dock = PickDock();
                if (dock == SIZE_MAX) return;  // cancelled
                Cfg().docks[dock].AddScriptWidget(wid);
                const std::wstring dockId = Cfg().docks[dock].id;
                ChangedFor(SettingsScope::Pins, dock);
                Window().Refresh();
                SetText(kWidgetsStatus, L"Added " + name + L" to the dock " + dockId + L".");
                break;
            }
            case kWidgetsAddToDesktop: {
                // Round 6 phase 6B: a standalone widget on the primary display (its window opens).
                if (!sel) return;
                const std::wstring wid = sel->id, name = sel->name;
                const size_t added = Host().addWidgetWindow ? Host().addWidgetWindow(wid, SIZE_MAX, true) : SIZE_MAX;
                if (added == SIZE_MAX) {
                    MessageBeep(MB_ICONWARNING);
                    SetText(kWidgetsStatus, L"Not added: " + name + L" could not be loaded.");
                    return;
                }
                Fill(wid);
                SetText(kWidgetsStatus, L"Added " + name + L" to the desktop as " + Cfg().docks[added].id + L".");
                break;
            }
            case kWidgetsAddFolder:
                AddFromFolder();
                break;
            case kWidgetsExport: {
                if (!ExportAllowed(sel)) return;
                std::wstring folder, error;
                const std::wstring name = sel->name, wid = sel->id;
                if (Widgets::ExportBuiltIn(wid, folder, error)) {
                    Host().command(ContextMenu::kReloadWidgets);  // the folder copy replaces the built-in
                    Fill(wid);
                    SetText(kWidgetsStatus, L"Exported " + name + L" to " + folder + L".");
                } else {
                    SetText(kWidgetsStatus, L"Not exported: " + error);
                    MessageBeep(MB_ICONWARNING);
                }
                break;
            }
            case kWidgetsReload:
                Host().command(ContextMenu::kReloadWidgets);
                SetText(kWidgetsStatus, L"Every widget was reloaded.");
                break;
            case kWidgetsOpenFolder: {
                const std::wstring dir = Widgets::Directory();
                CreateDirectoryW(dir.c_str(), nullptr);
                ShellExecuteW(Owner(), L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            }
            case kWidgetsConsole:
                Host().command(ContextMenu::kWidgetConsole);
                break;
            default:
                break;
        }
    }

private:
    // Export built-in to folder: a built-in that no folder replaces yet.
    static bool ExportAllowed(const Widgets::CatalogEntry* sel) {
        return sel && sel->builtIn && sel->source == Widgets::Package::Source::BuiltIn;
    }

    // Round 8 phase 8D: "More ▾" under its button (the old buttons' ids as the commands).
    void ShowMoreMenu() {
        HWND button = Item(kWidgetsMore);
        HMENU menu = CreatePopupMenu();
        if (!button || !menu) {
            if (menu) DestroyMenu(menu);
            return;
        }
        AppendMenuW(menu, MF_STRING, kWidgetsAddFolder, L"Add from folder\x2026");
        AppendMenuW(menu, MF_STRING | (ExportAllowed(Selected()) ? 0 : MF_GRAYED), kWidgetsExport, L"Export built-in to folder");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kWidgetsReload, L"Reload widgets");
        AppendMenuW(menu, MF_STRING, kWidgetsOpenFolder, L"Open widgets folder");
        AppendMenuW(menu, MF_STRING, kWidgetsConsole, L"Console\x2026");
        RECT r{};
        GetWindowRect(button, &r);
        const int cmd = static_cast<int>(
            TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, Owner(), nullptr));
        DestroyMenu(menu);
        if (cmd > 0) OnCommand(cmd, BN_CLICKED, nullptr);
    }

    // The plain docks (not folder windows or standalone widgets): "Add to dock" adds to the only
    // one, or shows a menu of them ("Add to dock ▾").
    size_t PlainDocks() {
        const auto& docks = Cfg().docks;
        return static_cast<size_t>(std::count_if(docks.begin(), docks.end(), [](const DockConfig& d) { return d.IsDock(); }));
    }

    // "Add to dock": the only dock, or (with several) the one chosen in a small menu of dock ids
    // under the button. SIZE_MAX: cancelled / none.
    size_t PickDock() {
        const auto& docks = Cfg().docks;
        std::vector<size_t> plain;
        for (size_t i = 0; i < docks.size(); ++i) {
            if (docks[i].IsDock()) plain.push_back(i);
        }
        if (plain.size() <= 1) return plain.empty() ? SIZE_MAX : plain.front();
        HWND button = Item(kWidgetsAddToDock);
        HMENU menu = CreatePopupMenu();
        if (!button || !menu) return SIZE_MAX;
        for (size_t k = 0; k < plain.size() && k < 200; ++k) {
            const DockConfig& d = docks[plain[k]];
            const std::wstring text = d.id + (d.title.empty() ? L"" : L" (" + d.title + L")");
            AppendMenuW(menu, MF_STRING, 1 + static_cast<UINT>(k), text.c_str());
        }
        RECT r{};
        GetWindowRect(button, &r);
        const UINT cmd = static_cast<UINT>(
            TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, Owner(), nullptr));
        DestroyMenu(menu);
        return cmd >= 1 && cmd - 1 < plain.size() && plain[cmd - 1] < Cfg().docks.size() ? plain[cmd - 1] : SIZE_MAX;
    }

    const Widgets::CatalogEntry* Selected() const {
        HWND list = Item(kWidgetsList);
        const int sel = list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
        return sel >= 0 && sel < static_cast<int>(catalog_.size()) ? &catalog_[static_cast<size_t>(sel)] : nullptr;
    }

    void UpdateButtons() {
        const Widgets::CatalogEntry* sel = Selected();
        // The preview panel follows the selection (the description read once per id per window).
        if (sel) {
            auto it = descriptions_.find(sel->id);
            if (it == descriptions_.end()) {
                std::wstring error, d;
                if (const auto pkg = Widgets::Load(sel->id, error)) d = pkg->manifest.description;
                it = descriptions_.emplace(sel->id, d).first;
            }
            panel_.Set(sel->id, sel->name, it->second);
        } else {
            panel_.Set(L"", L"", L"");
        }
        Enable(kWidgetsAddToDock, sel != nullptr);
        Enable(kWidgetsAddToDesktop, sel != nullptr);
        SetText(kWidgetsAddToDock, PlainDocks() > 1 ? L"Add to dock \x25BE" : L"Add to dock");  // round 8 phase 8D
        if (HWND f = GetFocus(); f && IsChild(Hwnd(), f) && !IsWindowEnabled(f)) SetFocus(Item(kWidgetsList));
    }

    // Lists the catalog; selects `select` (an id) or keeps the selection.
    void Fill(const std::wstring& select = {}) {
        HWND list = Item(kWidgetsList);
        if (!list) return;
        const Widgets::CatalogEntry* old = Selected();
        const std::wstring keep = !select.empty() ? select : old ? old->id : std::wstring();
        catalog_ = Widgets::Catalog();
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        int sel = catalog_.empty() ? -1 : 0;
        for (size_t i = 0; i < catalog_.size(); ++i) {
            const Widgets::CatalogEntry& e = catalog_[i];
            std::wstring error, version;
            if (const auto pkg = Widgets::Load(e.id, error)) version = pkg->manifest.version;
            std::wstring source = e.source == Widgets::Package::Source::Folder ? (e.builtIn ? L"folder (replaces built-in)" : L"folder")
                                                                             : L"built-in";
            std::wstring name = e.name, wid = e.id;
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = name.data();
            const int at = ListView_InsertItem(list, &item);
            if (at >= 0) {
                ListView_SetItemText(list, at, 1, wid.data());
                ListView_SetItemText(list, at, 2, source.data());
                ListView_SetItemText(list, at, 3, version.data());
            }
            if (e.id == keep) sel = static_cast<int>(i);
        }
        if (sel >= 0) ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        UpdateButtons();
    }

    void AddFromFolder() {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        dialog->SetTitle(L"Add a widget folder (it must contain a widget.json)");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        if (FAILED(dialog->Show(Owner()))) return;
        winrt::com_ptr<IShellItem> item;
        PWSTR raw = nullptr;
        if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return;
        const std::wstring folder = raw;
        CoTaskMemFree(raw);
        std::wstring wid, error;
        if (Widgets::InstallFolder(folder, wid, error)) {
            Fill(wid);
            SetText(kWidgetsStatus, L"Installed " + wid + L" (" + Widgets::Directory() + L"\\" + wid + L").");
        } else {
            SetText(kWidgetsStatus, L"Not added: " + error);
            MessageBeep(MB_ICONWARNING);
        }
    }

    std::vector<Widgets::CatalogEntry> catalog_;
    PreviewPanel panel_;
    std::map<std::wstring, std::wstring> descriptions_;  // widget id -> its manifest's description
};

// --- Widget settings -----------------------------------------------------------------------------
// The controls are generated from the selected item's schema: a script widget's widget.json
// "settings", or (round 5 phase A) the built-in schema of a clock / weather item
// (Widgets::BuiltInSchema). The page builds its table again (SettingsWindow::RequestRebuild) when
// another item is picked or the item's widget changes. Values are written to the item's
// `settings` object (only the keys the user changed; a clock / weather item has all of its keys
// since it was made) and applied at once (SettingsScope::Pins: the dock re-reads the item, a
// widget gets onSettingsChanged, a weather item moves to its new place's service).

// 4 decimals (~11 m), trailing zeros dropped.
std::wstring FormatCoord(double v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.4f", v);
    std::wstring s = buf;
    while (!s.empty() && s.back() == L'0') s.pop_back();
    if (!s.empty() && s.back() == L'.') s.pop_back();
    if (s == L"-0") s = L"0";
    return s;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::iswspace(s[i])) ++i;
    return s.substr(i);
}

double RoundCoord(double v) { return std::round(v * 1e4) / 1e4; }

// "47.6", "-122,33" (a decimal comma is accepted) -> value in [lo, hi]. False if not a number.
bool ParseCoord(std::wstring text, double lo, double hi, double& value) {
    text = Trim(std::move(text));
    for (wchar_t& c : text) {
        if (c == L',') c = L'.';
    }
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    const double v = std::wcstod(text.c_str(), &end);
    if (!end || *end != L'\0' || !std::isfinite(v) || v < lo || v > hi) return false;
    value = RoundCoord(v);
    return true;
}

bool HasSettings(ItemKind k) { return k == ItemKind::Widget || k == ItemKind::Clock || k == ItemKind::Weather; }

// visibleIf: every named field has that value now (compared as JSON). Round 10 phase 10B: shared
// with the Contents tab's inspector (SettingFieldVisible).
bool FieldVisible(const Widgets::SettingField& f, const json::JsonObject& values) {
    if (f.visibleIf.empty()) return true;
    if (!values) return true;
    for (const auto& [key, want] : f.visibleIf) {
        const json::IJsonValue v = JsonUtil::Find(values, key.c_str());
        if (!v) return false;
        try {
            if (std::wstring(v.Stringify()) != std::wstring(json::JsonValue::Parse(want).Stringify())) return false;
        } catch (...) {
            return false;
        }
    }
    return true;
}

// Round 10 phase 10C: which fields a generated page shows: every field (the window's only page of
// the widget's settings), or one section's (the Content / Appearance tabs of a schema with both).
enum class WidgetPart { All, Content, Appearance };

class WidgetSettingsPage : public FormPage {
public:
    // `standaloneItem` >= 0: a widget tab of a standalone widget's Dock window (round 8 phase 8C).
    // Round 10 phase 10C: `name` is the tab's ("Content" / "Appearance"), `part` its fields.
    WidgetSettingsPage(SettingsWindow& w, int standaloneItem, WidgetPart part, const wchar_t* name)
        : FormPage(w, name, part == WidgetPart::Appearance ? L'\xE790' : L'\xE713'), part_(part), standalone_(standaloneItem >= 0) {
        if (standalone_) asked_ = standaloneItem;
    }

    void SelectItem(int item) override { asked_ = item; }

    // Round 10 phase 10C: the window's other tab (the other section, a standalone widget's Placement)
    // changed the config: this tab's values follow (and its table, when a visibleIf flipped).
    void OnPeerChanged() override { Load(); }

protected:
    void Build() override {
        const int right = Width() - kMargin;
        ResolveItem();
        builtFor_ = Signature();
        std::wstring error;
        pkg_ = nullptr;
        builtIn_ = Widgets::Manifest{};
        const ItemKind kind = Kind();
        if (kind == ItemKind::Widget) {
            pkg_ = Widgets::Load(Entry().id, error);
        } else if (kind == ItemKind::Clock || kind == ItemKind::Weather) {
            builtIn_.name = kind == ItemKind::Clock ? L"Clock" : L"Weather";
            builtIn_.description = BuiltInItemDescription(kind);
            builtIn_.settings = Widgets::BuiltInSchema(kind);
        }
        const Widgets::Manifest* m = Schema();
        Title(L"Widget settings");  // a no-op in Dock and Item windows (round 8 phase 8A)
        // Round 9 phase 9D: the cards' "?" open the item's guide section; F1 there too (a standalone
        // widget's Widget tab: the window's guide section).
        cardTopic_ = kind == ItemKind::Clock     ? L"guide-items.md#the-clock"
                     : kind == ItemKind::Weather ? L"guide-items.md#weather"
                                                 : L"guide-items.md#widget-settings";
        HelpTopic(standalone_ ? L"guide-settings.md#content-and-appearance-tabs" : cardTopic_);
        // Round 8 phase 8C: the description is the Item window header's third line (the page has
        // no title or description note); only the cases without fields say so here. A standalone
        // widget's compact header has no such line: its Widget tab keeps the description as one
        // line with the whole text as its tooltip.
        std::wstring note;
        int lines = 2;
        if (item_ < 0) {
            note = L"This item has no settings.";
        } else if (!m) {
            note = L"Not installed: " + error;
        } else if (m->settings.empty()) {
            note = m->name + L" has no settings.";
        } else if (standalone_) {
            note = m->description;
            lines = 1;
        }
        if (part_ == WidgetPart::Appearance) note.clear();  // round 10 phase 10C: the Content tab says it
        if (!note.empty()) {
            UntitledCard(cardTopic_);
            Ctl& n = Text(kWidgetSettingsNote, CtlKind::Note, note, kMargin, right - kMargin, 20 * lines);
            n.maxLines = lines;
            Tip(kWidgetSettingsNote, note);
            NextRow(20 * lines + 2);
        }
        placeField_ = -1;
        places_.clear();  // the new search box has an empty list
        searched_.clear();
        KillTimer(Hwnd(), kPlaceTimer);
        builtVisible_ = VisibleSignature();
        if (m) BuildFields(*m, right);
        if (standalone_ && item_ >= 0) {
            // The Item window's footer buttons (same ids) as the last card of the Widget tab.
            UntitledCard(cardTopic_);
            Button(kFooterReset, L"Reset to defaults", kMargin, 150);
            if (kind == ItemKind::Widget) Button(kFooterConsole, L"Console…", kMargin + 158, 110);
            NextRow();
        }
    }

    // Round 8 phase 8C: one card per run of fields of one `group` (a group that comes back later
    // gets another card; fields without a group: a card named after the widget); a field's
    // description is its row's one-line hint when it fits the hint's width at the default size,
    // else its first sentence is, and the whole description is the tooltip of the label, the
    // control and the hint.
    void BuildFields(const Widgets::Manifest& m, int right) {
        const auto& fields = m.settings;
        const ItemKind kind = Kind();
        // The values now: `visibleIf` hides the fields whose condition does not hold (the page is
        // generated again when a field others depend on changes).
        const json::JsonObject values = Values();
        const std::wstring untitled = m.name.empty() ? std::wstring(L"Settings") : m.name;
        // The hints' widths at the default 600 DIP window (less a scroll bar): a check box's under
        // its caption, the others' in the label column.
        const int defaultRight = std::min(right, kDefaultItemW - kMargin - 18);
        const int checkHintW = std::max(40, defaultRight - kMargin - kCaptionDx);
        std::wstring group;
        bool first = true;
        for (size_t k = 0; k < fields.size() && k < static_cast<size_t>(kWidgetFieldMax); ++k) {
            const Widgets::SettingField& f = fields[k];
            const int base = kWidgetFieldBase + 10 * static_cast<int>(k);
            if (!OnPage(f) || !Visible(f, values)) continue;  // round 10 phase 10C: the other tab's
            if (first || f.group != group) Section(base + 4, (f.group.empty() ? untitled : f.group).c_str(), cardTopic_);  // the card
            first = false;
            group = f.group;
            using T = Widgets::SettingField::Type;
            const std::wstring hint = f.type == T::Place ? std::wstring() : HintOf(f.description, f.type == T::Bool ? checkHintW : labelW_);
            auto label = [&] {
                if (hint.empty()) {
                    Label(base, f.label.c_str());
                } else {
                    Label(base, f.label.c_str(), hint.c_str(), base + 3);
                }
            };
            switch (f.type) {
                case T::Bool:
                    if (hint.empty()) {
                        Check(base + 1, base + 2, f.label.c_str());
                    } else {
                        Check(base + 1, base + 2, f.label.c_str(), hint.c_str(), base + 3);
                    }
                    NextRow();
                    break;
                case T::Int: {
                    label();
                    const int lo = static_cast<int>(std::max(-1e9, f.min)), hi = static_cast<int>(std::min(1e9, f.max));
                    Spin(base + 1, base + 2, kColX, 120, lo, hi, std::max(1, static_cast<int>(f.step)));
                    if (!f.unit.empty()) Text(base + 5, CtlKind::Note, f.unit, kColX + 128, 120);  // "DIPs", "%"
                    NextRow();
                    break;
                }
                case T::Choice:
                case T::TimeZone:
                    label();
                    Combo(base + 1, kColX, right - kColX, false);
                    NextRow();
                    break;
                case T::Place:
                    if (placeField_ < 0) {
                        placeField_ = static_cast<int>(k);
                        BuildPlace(right);
                    }
                    break;
                case T::Color:
                case T::Number:
                    label();
                    EditBox(base + 1, kColX, 160, 32);
                    if (!f.unit.empty()) Text(base + 5, CtlKind::Note, f.unit, kColX + 168, 120);
                    NextRow();
                    break;
                case T::Font:
                case T::String:
                default:
                    label();
                    EditBox(base + 1, kColX, right - kColX, 1024);
                    NextRow();
                    break;
            }
            if (f.type != T::Place) {
                // The whole description: the label's (and so its hint's), the control's tooltip.
                // Round 9 phase 9D: a field without one (another author's widget) still gets a
                // tooltip: its name and its default.
                const std::wstring tip = !f.description.empty() ? f.description : f.label + L". Default: " + DefaultText(f) + L".";
                Tip(f.type == T::Bool ? base + 2 : base, tip);
                Tip(base + 1, tip);
                if (f.type == T::Bool && !hint.empty()) Tip(base + 3, tip);
            }
            if (kind == ItemKind::Clock && f.key == L"face") {
                Button(kClockFacesFolder, L"Open faces folder", kColX, 150);  // in the face's card
                NextRow();
            }
        }
    }

    // Round 9 phase 9D: a field's default as the page shows it ("On", a choice's label, the text;
    // "empty").
    static std::wstring DefaultText(const Widgets::SettingField& f) {
        std::wstring d = f.defaultJson;
        if (f.type == Widgets::SettingField::Type::Bool) return d == L"true" ? L"on" : L"off";
        if (d.size() >= 2 && d.front() == L'"' && d.back() == L'"') d = d.substr(1, d.size() - 2);
        for (const auto& [value, label] : f.choices) {
            if (value == d) return label;
        }
        return d.empty() || d == L"null" ? std::wstring(L"empty") : d;
    }

    // The row hint for a description: all of it when it is one line in `widthDip`, else its first
    // sentence (cut with "…" by the static when that is still too long).
    std::wstring HintOf(const std::wstring& description, int widthDip) {
        if (description.empty()) return L"";
        const int line = Window().MeasureTextDip(L"Ag", 1000);
        if (Window().MeasureTextDip(description, widthDip) <= line) return description;
        const size_t dot = description.find(L". ");
        return dot == std::wstring::npos ? description : description.substr(0, dot + 1);
    }

    // The place block (moved from the Weather page of round 4). Round 8 phase 8C: an autocomplete
    // search (the editable drop-down kPlaceResults: typing, then Enter or a 400 ms pause, searches
    // Open-Meteo's geocoder; the results fill its list as they arrive; choosing one sets the place),
    // the chosen place as one summary row, the name and the coordinates behind "Edit coordinates",
    // the status with Refresh now on one row and the credit.
    void BuildPlace(int right) {
        Label(kPlaceSearchLabel, L"Find a place", L"Type a city's name", kPlaceSearchNote);
        Tip(kPlaceSearchLabel, L"Type the name of a city or a place: the places that match appear in the list as you type "
                               L"(or when you press Enter). Choose one to set the place, its name and its coordinates.");
        Combo(kPlaceResults, kColX, right - kColX, true);
        Tip(kPlaceResults, L"Type a place's name; choose one of the places found in the list.");
        NextRow();
        if (Kind() == ItemKind::Widget) {
            // A script widget may show another item's place: the hint says whose (LoadPlace).
            Label(kPlaceSummaryLabel, L"Place", L"", kPlaceNote);
            Tip(kPlaceSummaryLabel, L"A widget without a place of its own shows the place of its dock's first Weather item, "
                                    L"else the default place (config.json \"weather\"). Choosing a place here gives it its own.");
        } else {
            Label(kPlaceSummaryLabel, L"Place");
        }
        Text(kPlaceSummary, CtlKind::Value, L"", kColX, right - kColX);
        NextRow();
        Expander(kPlaceCoordsExpander, L"Edit coordinates");
        Label(kPlaceLocationLabel, L"Name shown");
        Tip(kPlaceLocationLabel, L"The place's name as the forecast popup shows it. Choosing a place above fills it in.");
        EditBox(kPlaceLocationEdit, kColX, right - kColX, 128);
        NextRow();
        Label(kPlaceLatLabel, L"Latitude");
        Tip(kPlaceLatLabel, L"The place's latitude in degrees (-90 south to 90 north). Choosing a place above fills it in.");
        EditBox(kPlaceLatEdit, kColX, 120, 16);
        Text(kPlaceLatUnit, CtlKind::Note, L"-90 to 90", kColX + 128, 120);
        NextRow();
        Label(kPlaceLonLabel, L"Longitude");
        Tip(kPlaceLonLabel, L"The place's longitude in degrees (-180 west to 180 east). Choosing a place above fills it in.");
        EditBox(kPlaceLonEdit, kColX, 120, 16);
        Text(kPlaceLonUnit, CtlKind::Note, L"-180 to 180", kColX + 128, 120);
        NextRow();
        EndExpander();
        Label(kPlaceStatusLabel, L"Status");
        Text(kPlaceStatus, CtlKind::Value, L"", kColX, right - kColX - 138);
        Button(kPlaceRefreshNow, L"Refresh now", right - 130, 130);
        NextRow();
        Link(kPlaceCredit, L"Weather data by <a>Open-Meteo.com</a> (CC BY 4.0)", kMargin, right - kMargin);
        NextRow();
    }

    void Created() override {
        const Widgets::Manifest* m = Schema();
        if (!m) return;
        const auto& fields = m->settings;
        dynChoices_.clear();
        for (size_t k = 0; k < fields.size() && k < static_cast<size_t>(kWidgetFieldMax); ++k) {
            const int id = kWidgetFieldBase + 10 * static_cast<int>(k) + 1;
            if (fields[k].type == Widgets::SettingField::Type::Choice && fields[k].ChoiceFromPerf() && Item(id)) {
                BuildPerfChoices(k);
                for (const auto& c : dynChoices_[k]) ComboAdd(id, c.second.c_str());
            } else if (fields[k].type == Widgets::SettingField::Type::Choice) {
                for (const auto& c : fields[k].choices) ComboAdd(id, c.second.c_str());
            } else if (fields[k].type == Widgets::SettingField::Type::TimeZone && Item(id)) {
                // "Local time", then every zone of this Windows by display name, sorted by UTC offset.
                if (zones_.empty()) {
                    zones_.push_back({L"", L"Local time"});
                    for (const Clock::TimeZoneInfo& z : Clock::TimeZones()) zones_.push_back({z.key, z.display});
                }
                for (const auto& z : zones_) ComboAdd(id, z.second.c_str());
            }
        }
    }

    void Load() override {
        // The dock's items may have changed (another dock picked, items added / removed).
        // Round 10 phase 10B: also when another editor (the Contents tab) changed a value that
        // other fields' visibleIf name.
        if (Signature() != builtFor_ || VisibleSignature() != builtVisible_) {
            Window().RequestRebuild(*this);
            return;
        }
        const Widgets::Manifest* m = Schema();
        if (!m || item_ < 0) return;
        const json::JsonObject values = Values();
        if (!values) return;
        const auto& fields = m->settings;
        for (size_t k = 0; k < fields.size() && k < static_cast<size_t>(kWidgetFieldMax); ++k) {
            const Widgets::SettingField& f = fields[k];
            const int base = kWidgetFieldBase + 10 * static_cast<int>(k);
            using T = Widgets::SettingField::Type;
            if (f.type == T::Place) continue;  // LoadPlace
            const json::IJsonValue v = JsonUtil::Find(values, f.key.c_str());
            if (!v) continue;
            switch (f.type) {
                case T::Bool:
                    SetCheck(base + 1, v.ValueType() == json::JsonValueType::Boolean && v.GetBoolean());
                    break;
                case T::Int:
                    if (v.ValueType() == json::JsonValueType::Number) SetSpin(base + 2, static_cast<int>(std::lround(v.GetNumber())));
                    break;
                case T::Number:
                    if (v.ValueType() == json::JsonValueType::Number) SetText(base + 1, JsonUtil::Num(v.GetNumber()));
                    break;
                case T::Choice: {
                    if (v.ValueType() != json::JsonValueType::String) break;
                    const std::wstring s(v.GetString());
                    const auto& choices = ChoicesOf(k);
                    for (size_t c = 0; c < choices.size(); ++c) {
                        if (choices[c].first == s) SetComboSel(base + 1, static_cast<int>(c));
                    }
                    break;
                }
                case T::TimeZone: {
                    const std::wstring s = v.ValueType() == json::JsonValueType::String ? std::wstring(v.GetString()) : std::wstring();
                    int sel = 0;  // local (also for a zone this Windows does not know: the clock shows local time)
                    for (size_t z = 0; z < zones_.size(); ++z) {
                        if (_wcsicmp(zones_[z].first.c_str(), s.c_str()) == 0) sel = static_cast<int>(z);
                    }
                    SetComboSel(base + 1, sel);
                    break;
                }
                default:
                    if (v.ValueType() == json::JsonValueType::String) SetText(base + 1, std::wstring(v.GetString()));
                    break;
            }
        }
        if (placeField_ >= 0) LoadPlace(values);
    }

    void OnWeatherChanged() override { UpdatePlaceStatus(); }

    void OnTimer(UINT_PTR id) override {
        if (id != kPlaceTimer) return;
        KillTimer(Hwnd(), kPlaceTimer);  // one pause, one search: nothing runs while idle
        StartSearch(false);
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) == kPlaceCredit && (nm.code == NM_CLICK || nm.code == NM_RETURN)) {
            AllowSetForegroundWindow(ASFW_ANY);
            ShellExecuteW(nullptr, L"open", L"https://open-meteo.com/", nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }

    void OnCommand(int id, int code, HWND) override {
        if (standalone_ && id == kFooterConsole && code == BN_CLICKED) {  // the Widget tab's Console…
            if (Host().command) Host().command(ContextMenu::kWidgetConsole);
            return;
        }
        if (standalone_ && id == kFooterReset) id = kWidgetReset;  // the Widget tab's Reset to defaults
        if (id == kWidgetReset && code == BN_CLICKED) {  // the footer's Reset to defaults
            if (item_ < 0) return;
            // A clock / weather item: the defaults' values (config.clock / config.weather); a
            // widget: no settings (its schema's defaults).
            DockItemEntry& e = Entry();
            e.settings = Kind() == ItemKind::Clock     ? ClockSettingsJson(Cfg().clock)
                         : Kind() == ItemKind::Weather ? WeatherSettingsJson(Cfg().weather)
                                                       : std::wstring();
            Changed(SettingsScope::Pins);
            Window().RequestRebuild(*this);  // the defaults may show / hide visibleIf fields
            return;
        }
        if (id == kClockFacesFolder && code == BN_CLICKED) {
            const std::wstring dir = ClockFaces::Directory();
            CreateDirectoryW(Config::Directory().c_str(), nullptr);
            CreateDirectoryW(dir.c_str(), nullptr);
            ShellExecuteW(Owner(), L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (id == kPlaceResults) {
            // Round 8 phase 8C: the autocomplete. Typing (re)starts the pause; a choice in the
            // dropped list counts when the list closes on it (arrowing through it does not), one
            // made with the list closed (arrow keys, automation) at once.
            if (code == CBN_EDITCHANGE) {
                SetTimer(Hwnd(), kPlaceTimer, kPlacePauseMs, nullptr);
            } else if (code == CBN_SELENDOK) {
                ChoosePlace(ComboSel(id));
            } else if (code == CBN_SELCHANGE && !SendMessageW(Item(id), CB_GETDROPPEDSTATE, 0, 0)) {
                ChoosePlace(ComboSel(id));
            }
            return;
        }
        if (id == kPlaceRefreshNow && code == BN_CLICKED) {
            if (Host().refreshWeather) Host().refreshWeather(PlaceNow());
            UpdatePlaceStatus();
            return;
        }
        const Widgets::SettingField* f = FieldOf(id);
        if (!f) return;
        using T = Widgets::SettingField::Type;
        if (f->type == T::Bool && code == BN_CLICKED) {
            Store(*f, json::JsonValue::CreateBooleanValue(GetCheck(id)));
        } else if (f->type == T::Choice && code == CBN_SELCHANGE) {
            const int sel = ComboSel(id);
            const auto& choices = ChoicesOf(static_cast<size_t>((id - kWidgetFieldBase) / 10));
            if (sel >= 0 && sel < static_cast<int>(choices.size())) {
                Store(*f, json::JsonValue::CreateStringValue(choices[static_cast<size_t>(sel)].first));
            }
        } else if (f->type == T::TimeZone && code == CBN_SELCHANGE) {
            const int sel = ComboSel(id);
            if (sel >= 0 && sel < static_cast<int>(zones_.size())) {
                Store(*f, json::JsonValue::CreateStringValue(zones_[static_cast<size_t>(sel)].first));
            }
        }
    }

    void Spun(int spin, int pos) override {
        const Widgets::SettingField* f = FieldOf(spin - 1);
        if (f && f->type == Widgets::SettingField::Type::Int) Store(*f, json::JsonValue::CreateNumberValue(pos));
    }

    void Commit(int id) override {
        switch (id) {
            case kPlaceResults:
                // Enter, or leaving the box: search now for text not searched yet (a chosen
                // place's name was).
                StartSearch(GetKeyState(VK_RETURN) < 0);
                return;
            case kPlaceLocationEdit: {
                std::wstring name = Trim(GetText(id));
                if (name.size() > 128) name.resize(128);
                SetText(id, name);
                const WeatherConfig now = PlaceNow();
                if (name != now.location) StoreKeys({{L"location", json::JsonValue::CreateStringValue(name)}});
                return;
            }
            case kPlaceLatEdit:
            case kPlaceLonEdit: {
                const bool lat = id == kPlaceLatEdit;
                const WeatherConfig now = PlaceNow();
                const double current = lat ? now.latitude : now.longitude;
                double v = current;
                if (!ParseCoord(GetText(id), lat ? -90.0 : -180.0, lat ? 90.0 : 180.0, v)) {
                    SetText(id, FormatCoord(current));  // not a valid coordinate: back to the current one
                    MessageBeep(MB_ICONWARNING);
                    return;
                }
                SetText(id, FormatCoord(v));
                // Both coordinates go into the item (a widget then has a place of its own).
                const double other = lat ? now.longitude : now.latitude;
                if (std::fabs(v - current) > 1e-9 || !SettingsHavePlace(Entry().settings)) {
                    StoreKeys({{lat ? L"latitude" : L"longitude", json::JsonValue::CreateNumberValue(v)},
                               {lat ? L"longitude" : L"latitude", json::JsonValue::CreateNumberValue(other)}});
                }
                UpdatePlaceStatus();
                return;
            }
            default:
                break;
        }
        const Widgets::SettingField* f = FieldOf(id);
        if (!f) return;
        const std::wstring text = GetText(id);
        using T = Widgets::SettingField::Type;
        switch (f->type) {
            case T::Int: {
                int v = 0;
                if (ReadInt(id, static_cast<int>(std::max(-1e9, f->min)), static_cast<int>(std::min(1e9, f->max)), v)) {
                    Store(*f, json::JsonValue::CreateNumberValue(v));
                } else {
                    Load();
                }
                break;
            }
            case T::Number: {
                wchar_t* end = nullptr;
                const double d = std::wcstod(text.c_str(), &end);
                if (end == text.c_str() || !std::isfinite(d)) {
                    Load();
                    break;
                }
                Store(*f, json::JsonValue::CreateNumberValue(std::clamp(d, f->min, f->max)));
                break;
            }
            case T::Color: {
                uint32_t argb = 0;
                if (!ParseArgbColor(text, argb)) {
                    MessageBeep(MB_ICONWARNING);
                    Load();
                    break;
                }
                Store(*f, json::JsonValue::CreateStringValue(FormatArgbColor(argb)));
                break;
            }
            default:
                // A clock's format may not be empty (it would hide the clock): back to the current one.
                if (Kind() == ItemKind::Clock && f->key == L"format" && Trim(text).empty()) {
                    Load();
                    break;
                }
                Store(*f, json::JsonValue::CreateStringValue(text));
                break;
        }
    }

private:
    DockItemEntry& Entry() { return Dock().items[static_cast<size_t>(item_)]; }
    ItemKind Kind() {
        const auto& items = Dock().items;
        return item_ >= 0 && item_ < static_cast<int>(items.size()) ? items[static_cast<size_t>(item_)].Kind() : ItemKind::Unknown;
    }

    // The schema shown: the widget's manifest, the built-in kind's, or none.
    const Widgets::Manifest* Schema() const {
        if (pkg_) return &pkg_->manifest;
        return builtIn_.settings.empty() ? nullptr : &builtIn_;
    }

    // item_ (the Item window's item, SelectItem) names a clock, weather or widget item of the
    // window's dock, else -1.
    void ResolveItem() {
        item_ = asked_;
        const auto& items = Dock().items;
        if (item_ < 0 || item_ >= static_cast<int>(items.size()) || !HasSettings(items[static_cast<size_t>(item_)].Kind())) {
            item_ = -1;
        }
    }

    // What the table was generated for: the dock, the item and its kind / widget id.
    std::wstring Signature() {
        ResolveItem();
        std::wstring s = Window().DockId() + L"|" + std::to_wstring(item_);
        if (item_ >= 0) s += L"|" + Entry().kind + L":" + Entry().id;
        return s;
    }

    // The values the page shows: a clock / weather item's effective settings (its own over the
    // defaults); a widget's merged settings, with the place it shows when it has none of its own.
    json::JsonObject Values() {
        json::JsonObject values{nullptr};
        const Widgets::Manifest* m = Schema();
        if (!m || item_ < 0) return values;
        const DockItemEntry& e = Entry();
        const std::wstring text = ItemSettingsValues(Cfg(), e, m);  // round 10 phase 10B: shared with the inspector
        try {
            if (!json::JsonObject::TryParse(text, values)) values = nullptr;
            if (values && e.Kind() == ItemKind::Widget && HasPlaceField()) {
                // The place keys it does not set itself: what its weather provider uses.
                const WeatherConfig place = ScriptWeatherOf(Cfg(), Dock(), e);
                json::JsonObject own{nullptr};
                if (e.settings.empty() || !json::JsonObject::TryParse(e.settings, own) || !own) own = json::JsonObject();
                const bool hasPlace = SettingsHavePlace(e.settings);
                auto fill = [&](const wchar_t* key, const json::IJsonValue& v) {
                    if (!hasPlace || !own.HasKey(key)) values.SetNamedValue(key, v);
                };
                fill(L"latitude", json::JsonValue::CreateNumberValue(place.latitude));
                fill(L"longitude", json::JsonValue::CreateNumberValue(place.longitude));
                fill(L"location", json::JsonValue::CreateStringValue(place.location));
                if (!own.HasKey(L"units")) values.SetNamedValue(L"units", json::JsonValue::CreateStringValue(ToString(place.units)));
                if (!own.HasKey(L"refreshMinutes")) values.SetNamedValue(L"refreshMinutes", json::JsonValue::CreateNumberValue(place.refreshMinutes));
            }
        } catch (...) {
            values = nullptr;
        }
        return values;
    }

    bool HasPlaceField() const {
        const Widgets::Manifest* m = Schema();
        return m && std::any_of(m->settings.begin(), m->settings.end(),
                                [](const Widgets::SettingField& f) { return f.type == Widgets::SettingField::Type::Place; });
    }

    // The place of the item now: a weather item's own; a widget's (its own, else its dock's
    // weather item's, else the defaults).
    WeatherConfig PlaceNow() {
        if (item_ < 0) return Cfg().weather;
        const DockItemEntry& e = Entry();
        return e.Kind() == ItemKind::Weather ? WeatherOf(Cfg(), e) : ScriptWeatherOf(Cfg(), Dock(), e);
    }

    void LoadPlace(const json::JsonObject& values) {
        double lat = 0.0, lon = 0.0;
        std::wstring location;
        JsonUtil::ReadNumber(values, L"latitude", lat);
        JsonUtil::ReadNumber(values, L"longitude", lon);
        JsonUtil::ReadString(values, L"location", location);
        SetText(kPlaceLocationEdit, location);
        SetText(kPlaceLatEdit, FormatCoord(lat));
        SetText(kPlaceLonEdit, FormatCoord(lon));
        if (Kind() == ItemKind::Widget) {
            // The Place label's hint: whose place it shows (the label's tooltip explains).
            const DockItemEntry& e = Entry();
            const bool own = SettingsHavePlace(e.settings);
            const bool dockItem = std::any_of(Dock().items.begin(), Dock().items.end(),
                                              [](const DockItemEntry& i) { return i.Kind() == ItemKind::Weather; });
            SetText(kPlaceNote, own ? L"Its own place" : dockItem ? L"This dock's Weather item's" : L"The default place");
        }
        UpdatePlaceStatus();
    }

    void UpdatePlaceStatus() {
        if (placeField_ < 0 || item_ < 0 || !Item(kPlaceStatus)) return;
        if (Host().weatherStatus) SetText(kPlaceStatus, Host().weatherStatus(PlaceNow()));
        UpdateSummary();
    }

    // "Colorado Springs · 38.83, −104.82" (the place shown; the coordinates to 2 decimals).
    void UpdateSummary() {
        if (!Item(kPlaceSummary)) return;
        const WeatherConfig p = PlaceNow();
        if (!p.Configured()) {
            SetText(kPlaceSummary, L"None yet: find a place above");
            return;
        }
        auto coord = [](double v) {
            wchar_t buf[32];
            swprintf_s(buf, L"%.2f", std::fabs(v));
            std::wstring s = buf;
            return (v < 0 && s != L"0.00" ? std::wstring(L"\x2212") : std::wstring()) + s;  // a real minus sign
        };
        const std::wstring name = !p.location.empty() ? p.location : std::wstring(L"Unnamed place");
        SetText(kPlaceSummary, name + L" · " + coord(p.latitude) + L", " + coord(p.longitude));
    }

    // Searches for the box's text unless it was the last search (or a chosen place's name) and
    // `force` is false; `force` (Enter): also an empty box says so, and the same text shows the
    // list again.
    void StartSearch(bool force) {
        KillTimer(Hwnd(), kPlaceTimer);
        HWND combo = Item(kPlaceResults);
        if (!combo) return;
        const std::wstring query = Trim(GetText(kPlaceResults));
        if (query.empty()) {
            if (force) SetText(kPlaceSearchNote, L"Type a place name first");
            return;
        }
        if (query == searched_) {
            if (force && !places_.empty() && GetFocus() && IsChild(combo, GetFocus())) SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
            return;
        }
        if (!Host().geocode) return;
        searched_ = query;
        const bool started = Host().geocode(query, [this](const std::vector<WeatherService::Place>& places,
                                                          const std::wstring& error) { OnPlaces(places, error); });
        SetText(kPlaceSearchNote, started ? L"Searching…" : L"Could not start the search");
        if (!started) searched_.clear();
    }

    void OnPlaces(const std::vector<WeatherService::Place>& places, const std::wstring& error) {
        places_ = places;
        HWND combo = Item(kPlaceResults);
        if (!combo) return;  // the page was generated for another item meanwhile
        // The list's new rows; the typed text and its caret stay (CB_RESETCONTENT clears the edit).
        const std::wstring typed = GetText(kPlaceResults);
        DWORD from = 0, to = 0;
        SendMessageW(combo, CB_GETEDITSEL, reinterpret_cast<WPARAM>(&from), reinterpret_cast<LPARAM>(&to));
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (const WeatherService::Place& p : places_) ComboAdd(kPlaceResults, p.Label().c_str());
        auto restore = [&] {
            SetWindowTextW(combo, typed.c_str());
            SendMessageW(combo, CB_SETEDITSEL, 0, MAKELPARAM(from, to));
        };
        restore();
        if (!error.empty()) {
            SetText(kPlaceSearchNote, L"Search failed: " + error);
        } else if (places_.empty()) {
            SetText(kPlaceSearchNote, L"No places found");
        } else {
            SetText(kPlaceSearchNote, std::to_wstring(places_.size()) + (places_.size() == 1 ? L" place found: choose it" : L" places found: choose one"));
            // Typing in the box (the window in front): its list drops down with the places (the
            // text stays as typed).
            if (HWND f = GetFocus(); f && (f == combo || IsChild(combo, f)) && GetForegroundWindow() == Owner() &&
                                     !SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0)) {
                SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
                restore();
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));  // a drop-down shown while typing hides the pointer
            }
        }
    }

    void ChoosePlace(int index) {
        if (index < 0 || index >= static_cast<int>(places_.size()) || item_ < 0) return;
        const WeatherService::Place& p = places_[static_cast<size_t>(index)];
        const std::wstring name = p.name.size() > 128 ? p.name.substr(0, 128) : p.name;
        KillTimer(Hwnd(), kPlaceTimer);
        searched_ = Trim(p.Label());  // the box shows the chosen row: no search for it
        StoreKeys({{L"latitude", json::JsonValue::CreateNumberValue(RoundCoord(p.latitude))},
                   {L"longitude", json::JsonValue::CreateNumberValue(RoundCoord(p.longitude))},
                   {L"location", json::JsonValue::CreateStringValue(name)}});
        SetText(kPlaceLocationEdit, name);
        SetText(kPlaceLatEdit, FormatCoord(RoundCoord(p.latitude)));
        SetText(kPlaceLonEdit, FormatCoord(RoundCoord(p.longitude)));
        if (Kind() == ItemKind::Widget) SetText(kPlaceNote, L"Its own place");
        SetText(kPlaceSearchNote, L"Place set");
        UpdatePlaceStatus();
    }

    const Widgets::SettingField* FieldOf(int id) const {
        const Widgets::Manifest* m = Schema();
        if (!m || id < kWidgetFieldBase) return nullptr;
        const int k = (id - kWidgetFieldBase) / 10, part = (id - kWidgetFieldBase) % 10;
        if (part != 1 || k >= static_cast<int>(m->settings.size())) return nullptr;
        return &m->settings[static_cast<size_t>(k)];
    }

    // Writes keys into the item's settings object and applies them. False if nothing changed.
    bool StoreKeys(std::initializer_list<std::pair<const wchar_t*, json::IJsonValue>> keys) {
        if (item_ < 0 || Loading()) return false;
        // Round 10 phase 10B: the write is shared with the Contents tab's inspector.
        std::vector<std::pair<std::wstring, std::wstring>> texts;
        try {
            for (const auto& [key, value] : keys) texts.emplace_back(key, std::wstring(value.Stringify()));
        } catch (...) {
            return false;
        }
        if (!StoreItemSettings(Cfg(), Entry(), texts)) return false;
        Changed(SettingsScope::Pins);  // the dock re-reads the item: onSettingsChanged / a new place
        return true;
    }

    // Writes one field's value and applies it.
    void Store(const Widgets::SettingField& f, const json::IJsonValue& value) {
        if (!StoreKeys({{f.key.c_str(), value}})) return;
        // Fields shown only for some value of this one: generate the page again.
        const Widgets::Manifest* m = Schema();
        if (m && std::any_of(m->settings.begin(), m->settings.end(), [&](const Widgets::SettingField& o) {
                return std::any_of(o.visibleIf.begin(), o.visibleIf.end(), [&](const auto& c) { return c.first == f.key; });
            })) {
            Window().RequestRebuild(*this);
        }
        if (f.key == L"units" || f.key == L"refreshMinutes") UpdatePlaceStatus();
    }

    static bool Visible(const Widgets::SettingField& f, const json::JsonObject& values) { return FieldVisible(f, values); }

    // Round 10 phase 10B: which fields the table was generated with (visibleIf), so a change made
    // elsewhere (the Contents tab's inspector) that shows / hides fields generates it again.
    std::wstring VisibleSignature() {
        const Widgets::Manifest* m = Schema();
        if (!m || item_ < 0) return L"";
        const json::JsonObject values = Values();
        std::wstring s;
        for (size_t k = 0; k < m->settings.size() && k < static_cast<size_t>(kWidgetFieldMax); ++k) {
            s += !OnPage(m->settings[k]) ? L'-' : Visible(m->settings[k], values) ? L'1' : L'0';
        }
        return s;
    }
    std::wstring builtVisible_;

    // Round 10 phase 10C: field `f` belongs on this tab.
    bool OnPage(const Widgets::SettingField& f) const {
        using S = Widgets::SettingField::Section;
        return part_ == WidgetPart::All || (f.section == S::Appearance) == (part_ == WidgetPart::Appearance);
    }
    WidgetPart part_ = WidgetPart::All;

    int asked_ = -1;  // the Item window's item (SelectItem)
    int item_ = -1;   // asked_ when it is a clock, weather or widget item, else -1
    std::wstring builtFor_;
    std::shared_ptr<const Widgets::Package> pkg_;
    Widgets::Manifest builtIn_;  // a clock / weather item: Widgets::BuiltInSchema
    int placeField_ = -1;        // the schema's (first) Place field, -1 none
    std::vector<WeatherService::Place> places_;  // the last search's results
    std::wstring searched_;      // the last search's text (or the chosen place's row): not searched again
    bool standalone_ = false;    // a standalone widget's Widget tab (round 8 phase 8C)
    const wchar_t* cardTopic_ = L"guide-items.md#widget-settings";  // round 9 phase 9D
    std::vector<std::pair<std::wstring, std::wstring>> zones_;  // time zone rows: key, display name

    // Round 6 phase 6D: the rows of `"choiceFrom": "perf"` fields (field index -> value, label): the
    // fixed choices, the perf provider's metrics (built-in keys first, then the HWiNFO readings with
    // their sensor names), and the stored key when it is not listed now (HWiNFO closed), so opening
    // the page never changes it.
    std::map<size_t, std::vector<std::pair<std::wstring, std::wstring>>> dynChoices_;
    const std::vector<std::pair<std::wstring, std::wstring>>& ChoicesOf(size_t k) const {
        auto it = dynChoices_.find(k);
        if (it != dynChoices_.end()) return it->second;
        return Schema()->settings[k].choices;
    }
    void BuildPerfChoices(size_t k) {
        const Widgets::SettingField& f = Schema()->settings[k];
        auto& rows = dynChoices_[k];
        rows = f.choices;
        std::vector<SettingsPerfMetric> metrics = Host().perfMetrics ? Host().perfMetrics() : std::vector<SettingsPerfMetric>{};
        std::stable_partition(metrics.begin(), metrics.end(), [](const SettingsPerfMetric& m) { return !m.hwinfo; });
        auto listed = [&](const std::wstring& key) {
            return std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return r.first == key; });
        };
        for (const SettingsPerfMetric& m : metrics) {
            if (listed(m.key)) continue;
            rows.emplace_back(m.key, m.hwinfo ? L"HWiNFO: " + m.name : m.name);
        }
        std::wstring stored;
        const json::JsonObject values = Values();
        if (const json::IJsonValue v = values ? JsonUtil::Find(values, f.key.c_str()) : nullptr;
            v && v.ValueType() == json::JsonValueType::String) {
            stored = v.GetString();
        }
        if (!stored.empty() && !listed(stored)) rows.emplace_back(stored, stored + L" (not available now)");
    }
};

}  // namespace

std::wstring ClockDetail(const ClockConfig& c) {
    std::wstring s = Clock::EffectiveFormat(c.format, c.showSeconds);
    if (c.showDate) s += L", date";
    if (!c.timeZone.empty()) s += L", " + c.timeZone;
    if (!c.label.empty()) s += L", “" + c.label + L"”";
    return s;
}

std::wstring WeatherDetail(const WeatherConfig& w) {
    if (!w.Configured()) return L"No location yet";
    std::wstring s = !w.label.empty() ? w.label : !w.location.empty() ? w.location : std::wstring(L"Location set");
    return s + (w.units == Units::Imperial ? L", °F" : L", °C");
}

std::wstring ItemSettingsValues(const Config& config, const DockItemEntry& e, const Widgets::Manifest* schema) {
    if (e.Kind() == ItemKind::Clock) return ClockSettingsJson(ClockOf(config, e));
    if (e.Kind() == ItemKind::Weather) return WeatherSettingsJson(WeatherOf(config, e));
    if (schema) return JsonUtil::Utf8ToWide(Widgets::MergeSettings(*schema, e.settings));
    return e.settings.empty() ? std::wstring(L"{}") : e.settings;
}

bool StoreItemSettings(const Config& config, DockItemEntry& e, const std::vector<std::pair<std::wstring, std::wstring>>& keys) {
    json::JsonObject o{nullptr};
    try {
        std::wstring base = e.settings;
        // A clock / weather item without settings (written by hand): its effective values first.
        if (base.empty() && e.Kind() == ItemKind::Clock) base = ClockSettingsJson(ClockOf(config, e));
        if (base.empty() && e.Kind() == ItemKind::Weather) base = WeatherSettingsJson(WeatherOf(config, e));
        if (base.empty() || !json::JsonObject::TryParse(base, o) || !o) o = json::JsonObject();
        for (const auto& [key, value] : keys) o.SetNamedValue(key, json::JsonValue::Parse(value));
        const std::wstring text(o.Stringify());
        if (text == e.settings) return false;
        e.settings = text;
    } catch (...) {
        return false;
    }
    return true;
}

bool SettingFieldVisible(const Widgets::SettingField& field, const std::wstring& valuesJson) {
    if (field.visibleIf.empty()) return true;
    json::JsonObject values{nullptr};
    try {
        if (!json::JsonObject::TryParse(valuesJson, values)) values = nullptr;
    } catch (...) {
        values = nullptr;
    }
    return FieldVisible(field, values);
}

std::unique_ptr<SettingsPage> MakeWidgetsPage(SettingsWindow& window) { return std::make_unique<WidgetsPage>(window); }

std::vector<std::unique_ptr<SettingsPage>> MakeWidgetSettingsPages(SettingsWindow& window, int standaloneItem) {
    // Round 10 phase 10C: the schema's sections decide the tabs, once, when the window is made (a
    // widget whose manifest changes while its window is open keeps the window's tabs; a section
    // that is not used gets none: a schema with only Content fields, the weather item's, has one
    // page and no tab strip).
    const DockItemEntry* e = nullptr;
    if (standaloneItem >= 0) {
        const DockConfig& d = window.Dock();
        if (static_cast<size_t>(standaloneItem) < d.items.size()) e = &d.items[static_cast<size_t>(standaloneItem)];
    } else {
        e = window.TargetItem();
    }
    unsigned mask = 0;
    if (e && (e->Kind() == ItemKind::Clock || e->Kind() == ItemKind::Weather)) {
        mask = Widgets::SectionMask(Widgets::BuiltInSchema(e->Kind()), static_cast<size_t>(SettingsIds::kWidgetFieldMax));
    } else if (e && e->Kind() == ItemKind::Widget) {
        std::wstring error;
        if (const auto pkg = Widgets::Load(e->id, error)) mask = Widgets::SectionMask(pkg->manifest.settings, static_cast<size_t>(SettingsIds::kWidgetFieldMax));
    }
    std::vector<std::unique_ptr<SettingsPage>> pages;
    if (mask == 3) {
        pages.push_back(std::make_unique<WidgetSettingsPage>(window, standaloneItem, WidgetPart::Content, L"Content"));
        pages.push_back(std::make_unique<WidgetSettingsPage>(window, standaloneItem, WidgetPart::Appearance, L"Appearance"));
    } else {
        pages.push_back(std::make_unique<WidgetSettingsPage>(window, standaloneItem, WidgetPart::All, mask == 2 ? L"Appearance" : L"Content"));
    }
    return pages;
}

std::wstring BuiltInItemDescription(ItemKind kind) {
    switch (kind) {
        case ItemKind::Clock: return L"This clock's own settings: other clocks keep theirs.";
        case ItemKind::Weather: return L"This weather item's own place and units: other weather items keep theirs.";
        default: return L"";
    }
}
