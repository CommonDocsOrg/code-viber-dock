// The Docks and displays page of the Global window (round 5 phase B: the Docks list and the Layout
// canvas of round 4 on one page). The canvas: a scaled drawing of every monitor (its work area
// shaded) with every dock on its edge in the dock's colour, labelled with its id (and title).
// Click a dock to select it (and its row), double-click it to open its own window; drag it along
// its edge (it snaps to the start / centre / end like Ctrl+drag on the dock), onto another edge or
// onto another monitor. A drop applies at once and saves (edge, align, offset, monitor).
// Owner-drawn with GDI in the palette colours; it repaints only when something changed.

#include <windows.h>
#include <commctrl.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <vector>

#include "ContextMenu.h"
#include "SettingsForm.h"
#include "SettingsIds.h"
#include "WidgetPackage.h"

namespace {

using namespace SettingsIds;

constexpr wchar_t kCanvasClass[] = L"WinDock.LayoutCanvas";
constexpr int kCanvasH = 200;          // DIPs (round 8 phase 8D: 230 until then; the page fits 760 x 560)
constexpr int kListH = 120;            // the list of docks
constexpr int kIdColDip = 170;
constexpr int kEdgeColDip = 110;
constexpr int kDisplayColDip = 130;
constexpr int kButtonW = 114;  // round 8 phase 8A: four in a row fit beside the scroll bar
constexpr int kButtonStep = 120;
constexpr int kAddW = 96;      // round 8 phase 8D: the toolbar's "Add ▾"
constexpr int kLockW = 122;    // ... and "Lock all docks" (its box and caption) at its right
// The Add ▾ menu's "Standalone widget ▸" entries: kWidgetCmd + the catalog index.
constexpr UINT kWidgetCmd = 0x7000;
constexpr float kMarginDip = 8.0f;     // the dock's distance from the monitor edge (DockWindow kBottomMargin)
constexpr float kSnapDip = 24.0f;      // snaps to the centre / an end this close (DockWindow kDockSnapDip)

BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(mon);
    return TRUE;
}

// DockManager's order: the primary first, then the others in enumeration order.
std::vector<HMONITOR> Monitors() {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    std::erase(monitors, primary);
    monitors.insert(monitors.begin(), primary);
    return monitors;
}

const wchar_t* EdgeWord(DockEdge e) {
    switch (e) {
        case DockEdge::Top: return L"top";
        case DockEdge::Left: return L"left";
        case DockEdge::Right: return L"right";
        case DockEdge::Bottom:
        default: return L"bottom";
    }
}
const wchar_t* AlignWord(DockAlign a) { return a == DockAlign::Start ? L"start" : a == DockAlign::End ? L"end" : L"centre"; }

class LayoutPage;

// The canvas control (window class WinDock.LayoutCanvas, created by the page's control table).
class LayoutCanvas {
public:
    explicit LayoutCanvas(LayoutPage& page) : page_(page) {}
    void Attach(HWND hwnd) {
        hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    // Monitors and dock rects again (config loaded, a dock moved, displays changed).
    void Reload();
    void Repaint() {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);

private:
    struct Mon {
        RECT rc{}, work{};
        UINT dpi = 96;
    };
    struct Dock {
        size_t cfg = 0;
        int monitor = -1;
        bool folder = false;  // a desktop folder window (its rect, moved freely)
        bool widget = false;  // a standalone widget (round 6 phase 6B; its window's rect, moved freely)
        RECT bar{};     // screen
        RECT canvas{};  // canvas px (inflated to at least 4 px thick)
    };
    // Where a drop would put the dragged dock.
    struct Target {
        int monitor = 0;
        DockEdge edge = DockEdge::Bottom;
        DockAlign align = DockAlign::Center;
        int offsetDip = 0;
        // A circle (docks[].shape): its centre in DIPs from the work area's top-left instead.
        bool circle = false;
        // A folder window: its top-left in DIPs from the work area's top-left (xDip / yDip).
        bool folder = false;
        int xDip = 0, yDip = 0;
        RECT ghost{};  // screen
    };

    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);
    void Paint(HDC hdc);
    void Layout();  // the canvas mapping for the client size
    POINT ToCanvas(POINT screen) const;
    POINT ToScreen(POINT canvas) const;
    RECT ToCanvas(const RECT& r) const;
    int HitDock(POINT canvas) const;
    Target TargetFor(POINT canvasPt) const;
    void Drop(const Target& t);
    void Publish();  // window text (automation) and the selection line

    LayoutPage& page_;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<Mon> mons_;
    std::vector<Dock> docks_;
    RECT unionRc_{};
    float scale_ = 1.0f;
    float ox_ = 0.0f, oy_ = 0.0f;
    // Dragging
    int pressed_ = -1;      // dock index under the press
    POINT pressPt_{};       // canvas
    POINT grab_{};          // screen: pointer - bar centre at the press
    bool dragging_ = false;
    Target target_;
};

// Global > Docks and displays (round 5 phase B: the Docks list of round 4 phase 3 and the Layout
// canvas of 3b on one page). The canvas on top, the list of docks (with each dock's colour dot)
// under it. The selected row (= the dock clicked on the canvas; kept as SettingsWindow::SelectDock)
// is what this page's Duplicate / Remove / Open dock settings act on; no other page reads it (round
// 5 phase D). A double click on the canvas or the list opens that dock's own window.
class LayoutPage : public FormPage {
public:
    explicit LayoutPage(SettingsWindow& w) : FormPage(w, L"Docks and displays", L'\xE7F4'), canvas_(*this) {}
    ~LayoutPage() override {
        if (images_) ImageList_Destroy(images_);  // LVS_SHAREIMAGELISTS: the list never frees it
    }

    Config& Config_() { return Cfg(); }
    SettingsWindow& Win() { return Window(); }
    SettingsHost& HostRef() { return Host(); }
    void Apply() { Changed(SettingsScope::Appearance); }
    void SetSelectedText(const std::wstring& text) { SetText(kLayoutSelected, text); }
    // The selected config dock (the window's dock).
    size_t SelectedCfg() const { return const_cast<LayoutPage*>(this)->DockIndex(); }
    // Selects config dock `cfg` (this page's list and canvas follow when `refresh`).
    void SelectCfg(size_t cfg, bool refresh = true) {
        Config& c = Cfg();
        if (cfg >= c.docks.size() || cfg == SelectedCfg()) return;
        Window().SelectDock(c.docks[cfg].id);
        if (refresh) {
            Window().Refresh();  // this list and canvas
            Window().ConfigApplied();
        }
    }
    // Its own window (a dock's or a folder window's).
    void OpenCfg(size_t cfg) {
        if (Host().openDock && cfg < Cfg().docks.size()) Host().openDock(cfg, -1);
    }

protected:
    void Build() override {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = &LayoutCanvas::Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kCanvasClass;
            wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
            registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        const int right = Width() - kMargin;
        Title(L"Docks and displays");
        Add(kLayoutCanvas, CtlKind::Custom, kCanvasClass, L"Docks and displays", WS_TABSTOP, RECT{kMargin, y_, right, y_ + kCanvasH});
        NextRow(kCanvasH + 4);
        // Round 8 phase 8A: one line; round 8 phase 8D: the whole explanation is the canvas's tooltip.
        Ctl& hint = Text(kDocksNote, CtlKind::Note, L"Click a dock to select it, double-click to open it, drag to move it.",
                         kMargin, right - kMargin, 20);
        hint.maxLines = 2;
        NextRow(24);
        // Round 8 phase 8D: one toolbar: "Add ▾" (Dock, Floating dock, Folder window…, Standalone
        // widget ▸), the selection / drop line, "Lock all docks" at the right.
        Button(kDockAddMenu, L"Add \x25BE", kMargin, kAddW);
        Text(kLayoutSelected, CtlKind::Value, L"", kMargin + kAddW + 12, right - kLockW - 8 - (kMargin + kAddW + 12));
        Check(kLockAllDocks, kLockAllDocksCaption, L"Lock all docks", right - kLockW);
        NextRow(kRowH + 4);
        Add(kDocksList, CtlKind::List, WC_LISTVIEWW, L"Docks",
            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | LVS_SHAREIMAGELISTS,
            RECT{kMargin, y_, right, y_ + kListH});
        NextRow(kListH + 8);
        // The selected row's actions.
        Button(kDocksOpen, L"Open settings", kMargin, kButtonW);
        Button(kDockDuplicate, L"Duplicate", kMargin + kButtonStep, kButtonW);
        Button(kDockRemove, L"Remove", kMargin + 2 * kButtonStep, kButtonW);
        // Round 9 phase 9A: why Remove may be greyed.
        Tip(kDockRemove, L"Removes the selected dock, folder window or standalone widget. Greyed for the last dock: "
                         L"there is always at least one.");
        NextRow(kRowH + 4);
        static constexpr wchar_t kDocksTip[] =
            L"Every display with its docks (the shaded part is its work area). Click a dock to select it, double-click "
            L"it (or its row) to open its own settings window; drag it along its edge (it snaps to the centre and the "
            L"ends), onto another edge or onto another display: it moves when you let go. A new dock starts at the "
            L"bottom of the primary display with the running apps and the clock. A dock whose display is not "
            L"connected is parked until it is; \"Every display\" shows one copy on each. A folder window shows a "
            L"folder on the desktop, under your windows. A floating dock sits anywhere on its display (on the desktop "
            L"until you choose another layer): drag it where you want it. A standalone widget is one widget in a "
            L"window of its own on the desktop (under your windows unless you choose another layer).";
        Tip(kLayoutCanvas, kDocksTip);
        Tip(kDocksNote, kDocksTip);
        static constexpr wchar_t kLockTip[] =
            L"Checked while every dock and standalone widget is locked (folder windows have no lock). Check it to lock "
            L"them all, clear it to unlock them all; each one's own Lock is in its window's header.";
        Tip(kLockAllDocks, kLockTip);
        Tip(kLockAllDocksCaption, kLockTip);
    }

    void OnResized() override { OnStyleChanged(); }  // round 8 phase 8A: the list's columns

    void Created() override {
        if (HWND c = Item(kLayoutCanvas)) canvas_.Attach(c);
        HWND list = Item(kDocksList);
        if (!list) return;
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        const wchar_t* names[] = {L"Dock", L"Edge", L"Display", L"Windows"};
        for (int i = 0; i < 4; ++i) {
            col.pszText = const_cast<wchar_t*>(names[i]);
            col.cx = 100;
            col.iSubItem = i;
            ListView_InsertColumn(list, i, &col);
        }
    }

    void Load() override {
        FillList();
        canvas_.Reload();
    }
    void OnDisplayChange() override {
        FillList();
        canvas_.Reload();
    }

    void OnStyleChanged() override {
        canvas_.Reload();
        HWND list = Item(kDocksList);
        if (!list) return;
        const Ctl* c = Find(kDocksList);
        const int scrollBar = GetSystemMetricsForDpi(SM_CXVSCROLL, Window().Dpi());
        const int total = Window().Px(c->dip.right - c->dip.left) - scrollBar - 4;
        int id = Window().Px(kIdColDip), edge = Window().Px(kEdgeColDip), display = Window().Px(kDisplayColDip);
        // Round 8 phase 8D: a narrow window shares the room (no horizontal scroll bar).
        if (const int room = total - Window().Px(80); room > 0 && id + edge + display > room) {
            const int fixed = id + edge + display;
            id = id * room / fixed;
            edge = edge * room / fixed;
            display = display * room / fixed;
        }
        ListView_SetColumnWidth(list, 0, id);
        ListView_SetColumnWidth(list, 1, edge);
        ListView_SetColumnWidth(list, 2, display);
        ListView_SetColumnWidth(list, 3, std::max(Window().Px(60), total - id - edge - display));
        // The colour dots at the current DPI.
        const int px = GetSystemMetricsForDpi(SM_CXSMICON, Window().Dpi());
        if (images_ && px == dotPx_) return;
        HIMAGELIST old = images_;
        dotPx_ = px;
        images_ = ImageList_Create(px, px, ILC_COLOR32 | ILC_MASK, 4, 4);
        ListView_SetImageList(list, images_, LVSIL_SMALL);
        if (old) ImageList_Destroy(old);
        FillList();
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) != kDocksList) return 0;
        if (nm.code == NM_DBLCLK) {
            const auto& item = reinterpret_cast<const NMITEMACTIVATE&>(nm);
            if (item.iItem >= 0) OpenCfg(static_cast<size_t>(item.iItem));
            return 0;
        }
        if (nm.code != LVN_ITEMCHANGED || Loading() || filling_) return 0;
        const auto& change = reinterpret_cast<const NMLISTVIEW&>(nm);
        if (!(change.uChanged & LVIF_STATE) || !(change.uNewState & LVIS_SELECTED)) return 0;
        if (change.iItem >= 0) SelectCfg(static_cast<size_t>(change.iItem));
        return 0;
    }

    void OnCommand(int id, int code, HWND ctl) override {
        if (code != BN_CLICKED) return;
        Config& c = Cfg();
        const size_t sel = SelectedCfg();
        switch (id) {
            case kDockAddMenu:
                ShowAddMenu();
                break;
            case kDockAdd:
            case kDockAddFloating:
            case kDockDuplicate:
            case kDockRemove: {
                const SettingsDockEdit edit = id == kDockAdd           ? SettingsDockEdit::Add
                                              : id == kDockAddFloating ? SettingsDockEdit::AddFloating
                                              : id == kDockDuplicate   ? SettingsDockEdit::Duplicate
                                                                       : SettingsDockEdit::Remove;
                if (!Host().editDocks || !Host().editDocks(edit, sel)) {
                    MessageBeep(MB_ICONWARNING);
                    return;
                }
                // The new dock (appended / right after the copied one), or the removed one's neighbour.
                const size_t next = edit == SettingsDockEdit::Add || edit == SettingsDockEdit::AddFloating ? c.docks.size() - 1
                                    : edit == SettingsDockEdit::Duplicate ? sel + 1
                                                                          : std::min(sel, c.docks.size() - 1);
                if (next < c.docks.size()) Window().SelectDock(c.docks[next].id);
                Window().Refresh();
                Window().ConfigApplied();
                break;
            }
            case kDockAddFolder:
                if (Host().command) Host().command(ContextMenu::kNewFolderWindow);  // the picker, then a refresh
                break;
            case kDockAddWidget: {
                // Round 6 phase 6B: which widget (round 8 phase 8D: as a command, the catalog in a
                // menu under Add ▾; the Add ▾ menu has it as a submenu).
                const std::vector<Widgets::CatalogEntry> catalog = Widgets::Catalog();
                HMENU menu = CreatePopupMenu();
                if (!menu) return;
                AppendWidgets(menu, catalog);
                const UINT cmd = TrackUnderAdd(menu);
                DestroyMenu(menu);
                if (cmd >= kWidgetCmd && cmd - kWidgetCmd < catalog.size()) AddWidget(catalog[cmd - kWidgetCmd].id);
                break;
            }
            case kDocksOpen:
                OpenCfg(sel);
                break;
            case kLockAllDocks:
            case kUnlockAllDocks: {
                // Round 8 phase 8D: the check box (its state says which) or a command (lParam 0:
                // 2806 locks, 2807 unlocks every entry).
                const bool lock = ctl ? GetCheck(kLockAllDocks) : id == kLockAllDocks;
                if (Host().command) Host().command(lock ? ContextMenu::kLockAllDocks : ContextMenu::kUnlockAllDocks);
                break;
            }
            default:
                break;
        }
    }

private:
    // The installed widgets as menu items kWidgetCmd + k.
    static void AppendWidgets(HMENU menu, const std::vector<Widgets::CatalogEntry>& catalog) {
        for (size_t k = 0; k < catalog.size() && k < 400; ++k) {
            AppendMenuW(menu, MF_STRING, kWidgetCmd + static_cast<UINT>(k), catalog[k].name.c_str());
        }
        if (catalog.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No widgets installed");
    }

    // A menu under the Add ▾ button; the chosen command (0: none).
    UINT TrackUnderAdd(HMENU menu) {
        HWND button = Item(kDockAddMenu);
        RECT r{};
        if (button) {
            GetWindowRect(button, &r);
        } else {
            POINT pt{};
            GetCursorPos(&pt);
            r = RECT{pt.x, pt.y, pt.x, pt.y};
        }
        return static_cast<UINT>(
            TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, Owner(), nullptr));
    }

    // Round 8 phase 8D: "Add ▾": Dock, Floating dock, Folder window…, Standalone widget ▸ (the
    // old buttons' ids as the commands).
    void ShowAddMenu() {
        const std::vector<Widgets::CatalogEntry> catalog = Widgets::Catalog();
        HMENU menu = CreatePopupMenu();
        HMENU widgets = CreatePopupMenu();
        if (!menu || !widgets) {
            if (menu) DestroyMenu(menu);
            if (widgets) DestroyMenu(widgets);
            return;
        }
        AppendWidgets(widgets, catalog);
        AppendMenuW(menu, MF_STRING, kDockAdd, L"Dock");
        AppendMenuW(menu, MF_STRING, kDockAddFloating, L"Floating dock");
        AppendMenuW(menu, MF_STRING, kDockAddFolder, L"Folder window\x2026");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(widgets), L"Standalone widget");  // destroyed with its parent
        const UINT cmd = TrackUnderAdd(menu);
        DestroyMenu(menu);
        if (cmd >= kWidgetCmd && cmd - kWidgetCmd < catalog.size()) {
            AddWidget(catalog[cmd - kWidgetCmd].id);
        } else if (cmd == kDockAdd || cmd == kDockAddFloating || cmd == kDockAddFolder) {
            OnCommand(static_cast<int>(cmd), BN_CLICKED, nullptr);
        }
    }

    // A standalone widget of widget `wid` on the primary display (its window opens).
    void AddWidget(const std::wstring& wid) {
        Config& c = Cfg();
        const size_t added = Host().addWidgetWindow ? Host().addWidgetWindow(wid, SIZE_MAX, true) : SIZE_MAX;
        if (added == SIZE_MAX) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        if (added < c.docks.size()) Window().SelectDock(c.docks[added].id);
        Window().Refresh();
    }

    // A filled circle in the dock's colour (the list's dot) as an image list entry.
    void AddDot(COLORREF color) {
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, dotPx_, dotPx_);
        ReleaseDC(nullptr, screen);
        HGDIOBJ old = SelectObject(dc, bmp);
        const COLORREF key = RGB(255, 0, 255);
        RECT r{0, 0, dotPx_, dotPx_};
        HBRUSH back = CreateSolidBrush(key);
        FillRect(dc, &r, back);
        DeleteObject(back);
        HBRUSH fill = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ ob = SelectObject(dc, fill);
        HGDIOBJ op = SelectObject(dc, pen);
        const int inset = std::max(1, dotPx_ / 5);
        Ellipse(dc, inset, inset, dotPx_ - inset, dotPx_ - inset);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
        DeleteObject(fill);
        SelectObject(dc, old);
        DeleteDC(dc);
        ImageList_AddMasked(images_, bmp, key);
        DeleteObject(bmp);
    }

    void FillList() {
        HWND list = Item(kDocksList);
        if (!list) return;
        const Config& c = Cfg();
        const int n = static_cast<int>(c.docks.size());
        filling_ = true;
        // Updated in place while the row count stays (a selection change refreshes every page,
        // this one included, from inside the list's own notification).
        const bool rebuild = ListView_GetItemCount(list) != n;
        if (rebuild) {
            SendMessageW(list, WM_SETREDRAW, FALSE, 0);
            ListView_DeleteAllItems(list);
        }
        if (images_) {
            ImageList_RemoveAll(images_);
            for (int i = 0; i < n; ++i) {
                const uint32_t argb = c.DockColor(static_cast<size_t>(i));
                AddDot(RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF));
            }
        }
        for (int i = 0; i < n; ++i) {
            const DockConfig& d = c.docks[static_cast<size_t>(i)];
            std::wstring id = d.id + (d.title.empty() ? L"" : L" (" + d.title + L")");
            const std::wstring edge = d.IsFolderWindow()   ? std::wstring(L"Folder window")
                                  : d.IsWidgetWindow() ? std::wstring(L"Standalone widget")
                                      : d.Floating()     ? L"Floating, faces " + std::wstring(SettingsEdgeName(d.edge))
                                                         : std::wstring(SettingsEdgeName(d.edge)) + (d.fullWidth ? L", full" : L"");
            const std::wstring display = SettingsDisplayName(d.monitor);
            const int windows = Host().dockWindows ? Host().dockWindows(static_cast<size_t>(i)) : 1;
            std::wstring state = windows == 0 ? L"Parked (display not connected)"
                                 : d.Each()   ? std::to_wstring(windows) + L" (one per display)"
                                              : L"Shown";
            if (d.locked && !d.IsFolderWindow()) state += L", locked";
            if (d.IsFolderWindow()) state += L": " + d.folder.path;
            if (const DockItemEntry* w = d.WidgetItem()) {
                state += L": " + Widgets::DisplayName(w->id) + L", " + std::to_wstring(d.widgetWindow.w) + L" × " +
                         std::to_wstring(d.widgetWindow.h);
            }
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_IMAGE;
            item.iItem = i;
            item.pszText = id.data();
            item.iImage = images_ ? i : -1;
            if (rebuild) {
                ListView_InsertItem(list, &item);
            } else {
                ListView_SetItem(list, &item);
            }
            ListView_SetItemText(list, i, 1, const_cast<wchar_t*>(edge.c_str()));
            ListView_SetItemText(list, i, 2, const_cast<wchar_t*>(display.c_str()));
            ListView_SetItemText(list, i, 3, state.data());
        }
        const int sel = std::clamp(static_cast<int>(SelectedCfg() == SIZE_MAX ? 0 : SelectedCfg()), 0, std::max(0, n - 1));
        if (n > 0 && ListView_GetNextItem(list, -1, LVNI_SELECTED) != sel) {
            ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        if (n > 0) ListView_EnsureVisible(list, sel, FALSE);
        if (rebuild) {
            SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        }
        InvalidateRect(list, nullptr, TRUE);
        filling_ = false;
        const bool folderSel = n > 0 && !c.docks[static_cast<size_t>(sel)].IsDock();  // a folder window or a standalone widget
        const auto plainDocks = std::count_if(c.docks.begin(), c.docks.end(), [](const DockConfig& d) { return d.IsDock(); });
        Enable(kDockRemove, n > 1 && (folderSel || plainDocks > 1));
        // Round 8 phase 8D: one check box for Lock all / Unlock all (mixed: unchecked). A folder
        // window has no lock of its own (its `locked` is never saved): it does not count.
        const bool lockable = std::any_of(c.docks.begin(), c.docks.end(), [](const DockConfig& d) { return !d.IsFolderWindow(); });
        SetCheck(kLockAllDocks, lockable && std::all_of(c.docks.begin(), c.docks.end(), [](const DockConfig& d) {
                                    return d.IsFolderWindow() || d.locked;
                                }));
        if (HWND f = GetFocus(); f && IsChild(Hwnd(), f) && !IsWindowEnabled(f)) SetFocus(list);
    }

    LayoutCanvas canvas_;
    HIMAGELIST images_ = nullptr;
    int dotPx_ = 0;
    bool filling_ = false;
};

LRESULT CALLBACK LayoutCanvas::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (auto* self = reinterpret_cast<LayoutCanvas*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        return self->OnMessage(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void LayoutCanvas::Reload() {
    if (!hwnd_) return;
    mons_.clear();
    for (HMONITOR m : Monitors()) {
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(m, &mi)) continue;
        Mon mon;
        mon.rc = mi.rcMonitor;
        mon.work = mi.rcWork;
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy))) mon.dpi = dx;
        mons_.push_back(mon);
    }
    docks_.clear();
    if (page_.HostRef().dockRects) {
        for (const SettingsDockRect& r : page_.HostRef().dockRects()) {
            Dock d;
            d.cfg = r.cfg;
            d.monitor = r.monitor;
            d.bar = r.bar;
            d.folder = r.folderWindow;
            d.widget = r.widgetWindow;
            docks_.push_back(d);
        }
    }
    Layout();
    Publish();
    Repaint();
}

void LayoutCanvas::Layout() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    unionRc_ = RECT{};
    for (const Mon& m : mons_) UnionRect(&unionRc_, &unionRc_, &m.rc);
    const float pad = static_cast<float>(page_.Win().Px(10));
    const float uw = static_cast<float>(std::max<LONG>(1, unionRc_.right - unionRc_.left));
    const float uh = static_cast<float>(std::max<LONG>(1, unionRc_.bottom - unionRc_.top));
    const float cw = std::max(1.0f, static_cast<float>(client.right) - 2.0f * pad);
    const float ch = std::max(1.0f, static_cast<float>(client.bottom) - 2.0f * pad);
    scale_ = std::min(cw / uw, ch / uh);
    ox_ = pad + (cw - uw * scale_) / 2.0f;
    oy_ = pad + (ch - uh * scale_) / 2.0f;
    for (Dock& d : docks_) {
        if (d.monitor < 0) continue;
        d.canvas = ToCanvas(d.bar);
        // At least 4 px thick so a dock stays visible and grabbable on a small drawing.
        if (d.canvas.bottom - d.canvas.top < 4) {
            const LONG mid = (d.canvas.top + d.canvas.bottom) / 2;
            d.canvas.top = mid - 2;
            d.canvas.bottom = mid + 2;
        }
        if (d.canvas.right - d.canvas.left < 4) {
            const LONG mid = (d.canvas.left + d.canvas.right) / 2;
            d.canvas.left = mid - 2;
            d.canvas.right = mid + 2;
        }
    }
}

POINT LayoutCanvas::ToCanvas(POINT s) const {
    return POINT{static_cast<LONG>(std::lround(ox_ + (s.x - unionRc_.left) * scale_)),
                 static_cast<LONG>(std::lround(oy_ + (s.y - unionRc_.top) * scale_))};
}

POINT LayoutCanvas::ToScreen(POINT c) const {
    return POINT{static_cast<LONG>(std::lround((c.x - ox_) / scale_)) + unionRc_.left,
                 static_cast<LONG>(std::lround((c.y - oy_) / scale_)) + unionRc_.top};
}

RECT LayoutCanvas::ToCanvas(const RECT& r) const {
    const POINT a = ToCanvas(POINT{r.left, r.top}), b = ToCanvas(POINT{r.right, r.bottom});
    return RECT{a.x, a.y, b.x, b.y};
}

int LayoutCanvas::HitDock(POINT c) const {
    for (size_t i = docks_.size(); i-- > 0;) {
        if (docks_[i].monitor < 0) continue;
        RECT r = docks_[i].canvas;
        InflateRect(&r, 4, 4);
        if (PtInRect(&r, c)) return static_cast<int>(i);
    }
    return -1;
}

LayoutCanvas::Target LayoutCanvas::TargetFor(POINT canvasPt) const {
    Target t;
    const Dock& d = docks_[static_cast<size_t>(pressed_)];
    const DockConfig& dc = page_.Config_().docks[d.cfg];
    const POINT p = ToScreen(canvasPt);
    // The dock's centre follows the pointer (as grabbed).
    const POINT c{p.x - grab_.x, p.y - grab_.y};
    // The monitor under the centre (nearest when it is off every monitor).
    long best = LONG_MAX;
    for (size_t i = 0; i < mons_.size(); ++i) {
        const RECT& r = mons_[i].rc;
        const long dx = c.x < r.left ? r.left - c.x : c.x >= r.right ? c.x - r.right + 1 : 0;
        const long dy = c.y < r.top ? r.top - c.y : c.y >= r.bottom ? c.y - r.bottom + 1 : 0;
        if (dx + dy < best) {
            best = dx + dy;
            t.monitor = static_cast<int>(i);
        }
    }
    if (mons_.empty()) return t;
    const Mon& m = mons_[static_cast<size_t>(t.monitor)];
    const RECT& r = m.rc;
    if (d.folder || d.widget || dc.Floating()) {
        // A folder window (and a floating dock, round 6 phase 6A: its bar's top-left; a standalone
        // widget, 6B: its window's) goes where it is dragged (kept on that monitor's work area); no
        // snapping.
        const float s = static_cast<float>(m.dpi) / 96.0f;
        const LONG w = d.bar.right - d.bar.left, h = d.bar.bottom - d.bar.top;
        const LONG left = std::clamp(c.x - w / 2, m.work.left, std::max(m.work.left, m.work.right - w));
        const LONG top = std::clamp(c.y - h / 2, m.work.top, std::max(m.work.top, m.work.bottom - h));
        t.folder = true;
        t.xDip = static_cast<int>(std::lround(static_cast<float>(left - m.work.left) / s));
        t.yDip = static_cast<int>(std::lround(static_cast<float>(top - m.work.top) / s));
        t.edge = dc.edge;
        t.align = dc.align;
        t.offsetDip = dc.offset;
        t.ghost = RECT{left, top, left + w, top + h};
        return t;
    }
    if (dc.shape == DockShape::Circle) {
        // A circle floats: its centre goes where the dragged centre is (on that monitor).
        const float s = static_cast<float>(m.dpi) / 96.0f;
        const LONG w = d.bar.right - d.bar.left, h = d.bar.bottom - d.bar.top;
        const LONG cx = std::clamp(c.x, r.left + w / 2, std::max(r.left + w / 2, r.right - w / 2));
        const LONG cy = std::clamp(c.y, r.top + h / 2, std::max(r.top + h / 2, r.bottom - h / 2));
        t.circle = true;
        t.xDip = static_cast<int>(std::lround(static_cast<float>(cx - m.work.left) / s));
        t.yDip = static_cast<int>(std::lround(static_cast<float>(cy - m.work.top) / s));
        t.edge = dc.edge;
        t.align = dc.align;
        t.offsetDip = dc.offset;
        t.ghost = RECT{cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h};
        return t;
    }
    const LONG cx = std::clamp(c.x, r.left, r.right - 1), cy = std::clamp(c.y, r.top, r.bottom - 1);
    // The edge nearest to the centre.
    const LONG dist[4] = {r.bottom - cy, cy - r.top, cx - r.left, r.right - cx};  // DockEdge order
    int e = 0;
    for (int i = 1; i < 4; ++i) {
        if (dist[i] < dist[e]) e = i;
    }
    t.edge = static_cast<DockEdge>(e);
    const bool wasVertical = IsVertical(dc.edge), vertical = IsVertical(t.edge);
    const LONG length = wasVertical ? d.bar.bottom - d.bar.top : d.bar.right - d.bar.left;  // along the bar
    const LONG thick = wasVertical ? d.bar.right - d.bar.left : d.bar.bottom - d.bar.top;
    const float s = static_cast<float>(m.dpi) / 96.0f;
    const LONG margin = dc.fullWidth ? 0 : static_cast<LONG>(std::lround(kMarginDip * s));
    const LONG mStart = vertical ? r.top : r.left, mEnd = vertical ? r.bottom : r.right;
    const LONG along = vertical ? cy : cx;
    LONG barStart = 0;
    if (dc.fullWidth) {
        // Full width: only the edge and the monitor change.
        t.align = dc.align;
        t.offsetDip = dc.offset;
        barStart = mStart;
    } else {
        // The nearest of centre / start / end, snapped to it within kSnapDip (as Ctrl+drag does).
        const LONG offCenter = along - (mStart + mEnd) / 2;
        const LONG offStart = (along - length / 2) - (mStart + margin);
        const LONG offEnd = (along + length / 2) - (mEnd - margin);
        LONG off = offCenter;
        t.align = DockAlign::Center;
        if (std::labs(offStart) < std::labs(off)) {
            off = offStart;
            t.align = DockAlign::Start;
        }
        if (std::labs(offEnd) < std::labs(off)) {
            off = offEnd;
            t.align = DockAlign::End;
        }
        if (std::labs(off) <= std::lround(kSnapDip * s)) off = 0;
        t.offsetDip = static_cast<int>(std::lround(static_cast<float>(off) / s));
        const LONG offPx = static_cast<LONG>(std::lround(t.offsetDip * s));
        switch (t.align) {
            case DockAlign::Start: barStart = mStart + margin + offPx; break;
            case DockAlign::End: barStart = mEnd - margin - length + offPx; break;
            default: barStart = (mStart + mEnd) / 2 - length / 2 + offPx; break;
        }
        barStart = std::clamp(barStart, mStart, std::max(mStart, mEnd - length));
    }
    const LONG barLen = dc.fullWidth ? mEnd - mStart : length;
    switch (t.edge) {
        case DockEdge::Top: t.ghost = RECT{barStart, r.top + margin, barStart + barLen, r.top + margin + thick}; break;
        case DockEdge::Left: t.ghost = RECT{r.left + margin, barStart, r.left + margin + thick, barStart + barLen}; break;
        case DockEdge::Right: t.ghost = RECT{r.right - margin - thick, barStart, r.right - margin, barStart + barLen}; break;
        case DockEdge::Bottom:
        default: t.ghost = RECT{barStart, r.bottom - margin - thick, barStart + barLen, r.bottom - margin}; break;
    }
    return t;
}

void LayoutCanvas::Drop(const Target& t) {
    Config& c = page_.Config_();
    const Dock& d = docks_[static_cast<size_t>(pressed_)];
    DockConfig& dc = c.docks[d.cfg];
    // "each" stays on every monitor; "primary" stays "primary" on the primary.
    int monitor = dc.monitor;
    if (!dc.Each() && !(dc.monitor == DockConfig::kMonitorPrimary && t.monitor == 0)) monitor = t.monitor;
    if (t.folder) {
        // A folder window or a floating dock ("each" stays on every monitor).
        const bool own = d.folder || d.widget;  // a window of its own: the monitor it is dropped on
        if (dc.x == t.xDip && dc.y == t.yDip && dc.monitor == (own ? t.monitor : monitor)) return;
        dc.x = t.xDip;
        dc.y = t.yDip;
        dc.monitor = own ? t.monitor : monitor;
        page_.SelectCfg(d.cfg, false);
        page_.Apply();  // Sync re-places it (and saves)
        page_.Win().Refresh();
        return;
    }
    if (t.circle) {
        if (dc.x == t.xDip && dc.y == t.yDip && dc.monitor == monitor) return;
        dc.x = t.xDip;
        dc.y = t.yDip;
        dc.monitor = monitor;
        page_.SelectCfg(d.cfg, false);
        page_.Apply();
        page_.Win().Refresh();
        return;
    }
    if (dc.edge == t.edge && dc.align == t.align && dc.offset == t.offsetDip && dc.monitor == monitor) return;
    dc.edge = t.edge;
    dc.align = t.align;
    dc.offset = t.offsetDip;
    dc.monitor = monitor;
    page_.SelectCfg(d.cfg, false);
    page_.Apply();        // the dock moves (and is saved)
    page_.Win().Refresh();  // the other pages show the new position (and this canvas reloads)
}

void LayoutCanvas::Publish() {
    std::wstring text;
    for (size_t i = 0; i < docks_.size(); ++i) {
        const Dock& d = docks_[i];
        text += std::to_wstring(i) + L":" + std::to_wstring(d.cfg) + L":" + std::to_wstring(d.monitor) + L":";
        if (d.monitor >= 0) {
            text += std::to_wstring(d.canvas.left) + L"," + std::to_wstring(d.canvas.top) + L"," +
                    std::to_wstring(d.canvas.right) + L"," + std::to_wstring(d.canvas.bottom);
        } else {
            text += L"-";
        }
        text += L";";
    }
    SetWindowTextW(hwnd_, text.c_str());
    const Config& c = page_.Config_();
    std::wstring line;
    if (dragging_ && pressed_ >= 0 && target_.folder) {
        line = L"Drop: " + std::to_wstring(target_.xDip) + L", " + std::to_wstring(target_.yDip) + L" on display " +
               std::to_wstring(target_.monitor + 1);
    } else if (dragging_ && pressed_ >= 0) {
        line = L"Drop: " + std::wstring(EdgeWord(target_.edge)) + L", " + AlignWord(target_.align) +
               (target_.offsetDip ? (target_.offsetDip > 0 ? L" +" : L" ") + std::to_wstring(target_.offsetDip) : L"") +
               L", display " + std::to_wstring(target_.monitor + 1);
    } else if (const size_t selCfg = page_.SelectedCfg(); selCfg < c.docks.size() && c.docks[selCfg].IsFolderWindow()) {
        const DockConfig& d = c.docks[selCfg];
        line = L"Selected: the folder window " + d.id + L" (" + d.folder.path + L")";
    } else if (const size_t wCfg = page_.SelectedCfg(); wCfg < c.docks.size() && c.docks[wCfg].IsWidgetWindow()) {
        const DockConfig& d = c.docks[wCfg];
        const DockItemEntry* w = d.WidgetItem();
        auto coord = [](int v) { return v == DockConfig::kCentred ? std::wstring(L"centred") : std::to_wstring(v); };
        line = L"Selected: the standalone widget " + d.id + L" (" + (w ? Widgets::DisplayName(w->id) : std::wstring()) + L" at " +
               coord(d.x) + L", " + coord(d.y) + L", " + std::to_wstring(d.widgetWindow.w) + L" × " + std::to_wstring(d.widgetWindow.h) + L")";
    } else if (const size_t fl = page_.SelectedCfg(); fl < c.docks.size() && c.docks[fl].Floating()) {
        const DockConfig& d = c.docks[fl];
        auto coord = [](int v) { return v == DockConfig::kCentred ? std::wstring(L"centred") : std::to_wstring(v); };
        line = L"Selected: " + d.id + L" (floating at " + coord(d.x) + L", " + coord(d.y) + L", faces " + EdgeWord(d.edge) + L")";
    } else if (page_.SelectedCfg() < c.docks.size()) {
        const DockConfig& d = c.docks[page_.SelectedCfg()];
        line = L"Selected: " + d.id + L" (" + EdgeWord(d.edge) + L", " + AlignWord(d.align) +
               (d.offset ? (d.offset > 0 ? L" +" : L" ") + std::to_wstring(d.offset) : L"") + L")";
    }
    page_.SetSelectedText(line);
}

void LayoutCanvas::Paint(HDC hdc) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const DarkMode::Palette& pal = page_.Win().Colors();
    auto mix = [](COLORREF a, COLORREF b, int pa) {
        auto ch = [&](BYTE x, BYTE y) { return static_cast<BYTE>((x * pa + y * (100 - pa)) / 100); };
        return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
    };
    HBRUSH back = CreateSolidBrush(page_.Win().SurfaceColor(hwnd_));  // a card's fill (round 8 phase 8A)
    FillRect(hdc, &client, back);
    DeleteObject(back);
    // The page's message font (WM_SETFONT may have come before the canvas was attached: then the
    // selection line's font, which is the same).
    HFONT font = font_ ? font_ : reinterpret_cast<HFONT>(SendMessageW(GetDlgItem(GetParent(hwnd_), kLayoutSelected), WM_GETFONT, 0, 0));
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))));
    SetBkMode(hdc, TRANSPARENT);
    const COLORREF monFill = mix(pal.text, pal.layer, 8), workFill = mix(pal.text, pal.layer, 4);
    HPEN monPen = CreatePen(PS_SOLID, 1, pal.textSecondary);
    HGDIOBJ oldPen = SelectObject(hdc, monPen);
    for (size_t i = 0; i < mons_.size(); ++i) {
        const RECT r = ToCanvas(mons_[i].rc);
        HBRUSH f = CreateSolidBrush(monFill);
        HGDIOBJ ob = SelectObject(hdc, f);
        Rectangle(hdc, r.left, r.top, r.right, r.bottom);
        SelectObject(hdc, ob);
        DeleteObject(f);
        RECT w = ToCanvas(mons_[i].work);
        InflateRect(&w, -1, -1);
        HBRUSH wf = CreateSolidBrush(workFill);
        FillRect(hdc, &w, wf);
        DeleteObject(wf);
        const std::wstring name = (i == 0 ? L"1 (primary)  " : std::to_wstring(i + 1) + L"  ") +
                                  std::to_wstring(mons_[i].rc.right - mons_[i].rc.left) + L" × " +
                                  std::to_wstring(mons_[i].rc.bottom - mons_[i].rc.top);
        SetTextColor(hdc, pal.textSecondary);
        RECT tr{r.left + 6, r.top + 4, r.right - 6, r.bottom - 4};
        DrawTextW(hdc, name.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(monPen);
    const Config& c = page_.Config_();
    for (size_t i = 0; i < docks_.size(); ++i) {
        const Dock& d = docks_[i];
        if (d.monitor < 0 || d.cfg >= c.docks.size()) continue;
        const bool selected = d.cfg == page_.SelectedCfg();
        const bool moving = dragging_ && static_cast<int>(i) == pressed_;
        // Every dock in its identity colour (round 5 phase B); the selected one outlined.
        const uint32_t argb = c.DockColor(d.cfg);
        const COLORREF color = pal.highContrast ? (selected ? pal.accent : pal.textSecondary)
                                                : RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
        HBRUSH f = CreateSolidBrush(color);
        RECT r = d.canvas;
        if (selected && !moving) {
            RECT o = r;
            InflateRect(&o, 3, 3);
            HBRUSH ring = CreateSolidBrush(pal.text);
            FrameRect(hdc, &o, ring);
            InflateRect(&o, -1, -1);
            FrameRect(hdc, &o, ring);
            DeleteObject(ring);
        }
        if (d.widget) {
            // A standalone widget (round 6 phase 6B): a small rounded box, labelled by its widget's name.
            HBRUSH fill = CreateSolidBrush(mix(color, pal.layer, moving ? 20 : 45));
            HPEN pen = CreatePen(PS_SOLID, 1, color);
            HGDIOBJ ob = SelectObject(hdc, fill);
            HGDIOBJ op = SelectObject(hdc, pen);
            const int round = std::max(4, std::min<int>(r.right - r.left, r.bottom - r.top) / 3);
            RoundRect(hdc, r.left, r.top, r.right, r.bottom, round, round);
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(pen);
            DeleteObject(fill);
            DeleteObject(f);
            const DockConfig& wc = c.docks[d.cfg];
            const DockItemEntry* w = wc.WidgetItem();
            const std::wstring name = w ? Widgets::DisplayName(w->id) : wc.id;
            SetTextColor(hdc, selected ? pal.text : pal.textSecondary);
            SIZE sz{};
            GetTextExtentPoint32W(hdc, name.c_str(), static_cast<int>(name.size()), &sz);
            RECT tr{};
            if (sz.cx <= r.right - r.left - 4 && sz.cy <= r.bottom - r.top - 2) {
                tr = r;  // inside the box
                DrawTextW(hdc, name.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            } else if (selected) {
                // Under it (kept inside the canvas); only the selected one's, so small boxes on a
                // wide display do not bury each other in labels.
                tr.left = std::clamp<LONG>((r.left + r.right - sz.cx) / 2, 2L, std::max(2L, client.right - 2 - sz.cx));
                tr.top = std::clamp<LONG>(r.bottom + 2, 2L, std::max(2L, client.bottom - 2 - sz.cy));
                tr.right = tr.left + sz.cx;
                tr.bottom = tr.top + sz.cy;
                DrawTextW(hdc, name.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
            }
            continue;
        }
        if (d.folder) {
            // A folder window: an outlined rectangle with a title strip and its name inside.
            HBRUSH fill = CreateSolidBrush(mix(color, pal.layer, moving ? 15 : 30));
            FillRect(hdc, &r, fill);
            DeleteObject(fill);
            FrameRect(hdc, &r, f);
            RECT strip{r.left, r.top, r.right, std::min(r.bottom, r.top + std::max<LONG>(3, (r.bottom - r.top) / 6))};
            FillRect(hdc, &strip, f);
            DeleteObject(f);
            const DockConfig& fc = c.docks[d.cfg];
            const std::wstring name = fc.title.empty() ? fc.id : fc.title;
            SetTextColor(hdc, selected ? pal.text : pal.textSecondary);
            RECT tr{r.left + 3, strip.bottom + 1, r.right - 3, r.bottom - 1};
            DrawTextW(hdc, name.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            continue;
        }
        if (moving) {
            HBRUSH dim = CreateSolidBrush(mix(color, pal.layer, 40));
            FillRect(hdc, &r, dim);
            DeleteObject(dim);
        } else {
            FillRect(hdc, &r, f);
        }
        DeleteObject(f);
        // The label beside the bar, toward the middle of its monitor.
        const DockConfig& dc = c.docks[d.cfg];
        std::wstring label = dc.id + (dc.title.empty() ? L"" : L" · " + dc.title) +
                             (dc.layer == DockLayer::Desktop  ? L" (desktop)"
                              : dc.layer == DockLayer::Behind ? L" (behind windows)"
                                                              : L"") +
                             (dc.Floating() ? L" · floating" : L"");
        SetTextColor(hdc, selected ? pal.text : pal.textSecondary);
        SIZE sz{};
        GetTextExtentPoint32W(hdc, label.c_str(), static_cast<int>(label.size()), &sz);
        const int gap = 4;
        RECT tr{};
        switch (dc.edge) {
            case DockEdge::Top: tr = RECT{(r.left + r.right - sz.cx) / 2, r.bottom + gap, 0, 0}; break;
            case DockEdge::Left: tr = RECT{r.right + gap, (r.top + r.bottom - sz.cy) / 2, 0, 0}; break;
            case DockEdge::Right: tr = RECT{r.left - gap - sz.cx, (r.top + r.bottom - sz.cy) / 2, 0, 0}; break;
            case DockEdge::Bottom:
            default: tr = RECT{(r.left + r.right - sz.cx) / 2, r.top - gap - sz.cy, 0, 0}; break;
        }
        // Kept inside the canvas (a floating dock near a display's side, round 6 phase 6A).
        tr.left = std::clamp(tr.left, 2L, std::max(2L, client.right - 2 - sz.cx));
        tr.top = std::clamp(tr.top, 2L, std::max(2L, client.bottom - 2 - sz.cy));
        tr.right = tr.left + sz.cx;
        tr.bottom = tr.top + sz.cy;
        DrawTextW(hdc, label.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    }
    if (dragging_ && pressed_ >= 0) {
        // Where the drop puts it: an outline in the accent colour.
        const RECT g = ToCanvas(target_.ghost);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, page_.Win().Px(2)), pal.accent);
        HGDIOBJ op = SelectObject(hdc, pen);
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, g.left - 1, g.top - 1, std::max(g.right, g.left + 4) + 1, std::max(g.bottom, g.top + 4) + 1);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(pen);
    }
    if (GetFocus() == hwnd_) {
        RECT fr = client;
        InflateRect(&fr, -1, -1);
        DrawFocusRect(hdc, &fr);
    }
    SelectObject(hdc, oldFont);
}

LRESULT LayoutCanvas::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SETFONT:
            font_ = reinterpret_cast<HFONT>(wp);
            return 0;
        case WM_SIZE:
            Layout();
            Publish();
            Repaint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd_, &ps);
            RECT client{};
            GetClientRect(hwnd_, &client);
            // Double-buffered: one blit per repaint.
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max<LONG>(1, client.right), std::max<LONG>(1, client.bottom));
            HGDIOBJ old = SelectObject(mem, bmp);
            Paint(mem);
            BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            Repaint();
            return 0;
        case WM_GETDLGCODE:
            return dragging_ ? DLGC_WANTALLKEYS : 0;
        case WM_LBUTTONDBLCLK: {
            // A double click on a dock: its own settings window.
            const int hit = HitDock(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hit >= 0) page_.OpenCfg(docks_[static_cast<size_t>(hit)].cfg);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            pressed_ = HitDock(pt);
            if (pressed_ < 0) return 0;
            pressPt_ = pt;
            const RECT& b = docks_[static_cast<size_t>(pressed_)].bar;
            const POINT s = ToScreen(pt);
            grab_ = POINT{s.x - (b.left + b.right) / 2, s.y - (b.top + b.bottom) / 2};
            SetCapture(hwnd_);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (pressed_ < 0 || GetCapture() != hwnd_) return 0;
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!dragging_ && std::abs(pt.x - pressPt_.x) <= GetSystemMetrics(SM_CXDRAG) &&
                std::abs(pt.y - pressPt_.y) <= GetSystemMetrics(SM_CYDRAG)) {
                return 0;
            }
            dragging_ = true;
            const Target t = TargetFor(pt);
            if (!EqualRect(&t.ghost, &target_.ghost) || t.edge != target_.edge || t.align != target_.align ||
                t.offsetDip != target_.offsetDip || t.monitor != target_.monitor) {
                target_ = t;  // repaint only when the drop place changed
                Publish();
                Repaint();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (pressed_ < 0) return 0;
            const bool drop = dragging_;
            const int index = pressed_;
            dragging_ = false;
            if (GetCapture() == hwnd_) ReleaseCapture();
            pressed_ = index;
            if (drop) {
                const Target t = TargetFor(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                Drop(t);
            } else {
                // A click: this dock is the one the other pages edit.
                page_.SelectCfg(docks_[static_cast<size_t>(index)].cfg);
            }
            pressed_ = -1;
            target_ = Target{};
            Reload();
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && dragging_) {
                dragging_ = false;
                pressed_ = -1;
                if (GetCapture() == hwnd_) ReleaseCapture();
                Publish();
                Repaint();
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lp) != hwnd_ && (dragging_ || pressed_ >= 0)) {
                // Someone took the mouse: cancel the drag.
                dragging_ = false;
                pressed_ = -1;
                Publish();
                Repaint();
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace

std::unique_ptr<SettingsPage> MakeLayoutPage(SettingsWindow& window) { return std::make_unique<LayoutPage>(window); }
