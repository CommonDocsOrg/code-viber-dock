// The Docks page: every config dock (id, edge, display, windows), Add / Duplicate / Remove, Lock
// all / Unlock all, and the selection: the dock the other pages edit (config.editDock).

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ContextMenu.h"
#include "SettingsForm.h"
#include "SettingsIds.h"

namespace {

using namespace SettingsIds;

constexpr int kListH = 196;
constexpr int kIdColDip = 150;
constexpr int kEdgeColDip = 90;
constexpr int kDisplayColDip = 150;
constexpr int kButtonW = 122;
constexpr int kButtonStep = 130;

const wchar_t* EdgeName(DockEdge e) {
    switch (e) {
        case DockEdge::Top: return L"Top";
        case DockEdge::Left: return L"Left";
        case DockEdge::Right: return L"Right";
        case DockEdge::Bottom:
        default: return L"Bottom";
    }
}

std::wstring DisplayName(int monitor) {
    if (monitor == DockConfig::kMonitorEach) return L"Every display";
    if (monitor == DockConfig::kMonitorPrimary || monitor == 0) return L"Primary";
    return L"Display " + std::to_wstring(monitor + 1);
}

class DocksPage : public FormPage {
public:
    explicit DocksPage(SettingsWindow& w) : FormPage(w, L"Docks", L'\xE8A9') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Docks");
        Add(kDocksList, CtlKind::List, WC_LISTVIEWW, L"Docks",
            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
            RECT{kMargin, y_, right, y_ + kListH});
        NextRow(kListH + 8);
        Text(kDockEditing, CtlKind::Value, L"", kMargin, right - kMargin, 22);
        NextRow(26);
        Button(kDockAdd, L"Add dock", kMargin, kButtonW);
        Button(kDockDuplicate, L"Duplicate", kMargin + kButtonStep, kButtonW);
        Button(kDockRemove, L"Remove", kMargin + 2 * kButtonStep, kButtonW);
        NextRow(kRowH + 4);
        Button(kLockAllDocks, L"Lock all docks", kMargin, kButtonW);
        Button(kUnlockAllDocks, L"Unlock all docks", kMargin + kButtonStep, kButtonW);
        NextRow(kRowH + 4);
        Text(kDocksNote, CtlKind::Note,
             L"Select a dock to edit it on the other pages (General, Behaviour, Dock contents and the theme on "
             L"Appearance). A new dock starts at the bottom of the primary display with the running apps and the "
             L"clock: move it there (Position) or Ctrl+drag it. A dock whose display is not connected is parked "
             L"until it is. \"Every display\" shows one copy of the dock on each display.",
             kMargin, right - kMargin, 90);
        NextRow(90);
    }

    void Created() override {
        HWND list = Item(kDocksList);
        if (!list) return;
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT);
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

    void OnStyleChanged() override {
        HWND list = Item(kDocksList);
        if (!list) return;
        const Ctl* c = Find(kDocksList);
        const int scrollBar = GetSystemMetricsForDpi(SM_CXVSCROLL, Window().Dpi());
        const int total = Window().Px(c->dip.right - c->dip.left) - scrollBar - 4;
        const int id = Window().Px(kIdColDip), edge = Window().Px(kEdgeColDip), display = Window().Px(kDisplayColDip);
        ListView_SetColumnWidth(list, 0, id);
        ListView_SetColumnWidth(list, 1, edge);
        ListView_SetColumnWidth(list, 2, display);
        ListView_SetColumnWidth(list, 3, std::max(Window().Px(60), total - id - edge - display));
    }

    void Load() override { FillList(); }
    void OnDisplayChange() override { FillList(); }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) != kDocksList || nm.code != LVN_ITEMCHANGED || Loading()) return 0;
        const auto& change = reinterpret_cast<const NMLISTVIEW&>(nm);
        if (!(change.uChanged & LVIF_STATE) || !(change.uNewState & LVIS_SELECTED)) return 0;
        const int row = change.iItem;
        Config& c = Cfg();
        if (row < 0 || static_cast<size_t>(row) >= c.docks.size() || static_cast<size_t>(row) == c.editDock) return 0;
        c.editDock = static_cast<size_t>(row);
        Window().Refresh();  // every page re-reads the selected dock (this list is updated in place)
        Window().ConfigApplied();
        return 0;
    }

    void OnCommand(int id, int code, HWND) override {
        if (code != BN_CLICKED) return;
        Config& c = Cfg();
        switch (id) {
            case kDockAdd:
            case kDockDuplicate:
            case kDockRemove: {
                const SettingsDockEdit edit = id == kDockAdd         ? SettingsDockEdit::Add
                                              : id == kDockDuplicate ? SettingsDockEdit::Duplicate
                                                                     : SettingsDockEdit::Remove;
                if (!Host().editDocks || !Host().editDocks(edit, c.editDock)) {
                    MessageBeep(MB_ICONWARNING);
                    return;
                }
                Window().Refresh();  // the new selection on every page
                Window().ConfigApplied();
                break;
            }
            case kLockAllDocks:
            case kUnlockAllDocks:
                if (Host().command) {
                    Host().command(id == kLockAllDocks ? ContextMenu::kLockAllDocks : ContextMenu::kUnlockAllDocks);
                }
                break;
            default:
                break;
        }
    }

private:
    void FillList() {
        HWND list = Item(kDocksList);
        if (!list) return;
        const Config& c = Cfg();
        const int n = static_cast<int>(c.docks.size());
        // Updated in place while the row count stays (a selection change refreshes every page,
        // this one included, from inside the list's own notification).
        const bool rebuild = ListView_GetItemCount(list) != n;
        if (rebuild) {
            SendMessageW(list, WM_SETREDRAW, FALSE, 0);
            ListView_DeleteAllItems(list);
        }
        for (int i = 0; i < n; ++i) {
            const DockConfig& d = c.docks[static_cast<size_t>(i)];
            std::wstring id = d.id + (d.title.empty() ? L"" : L" (" + d.title + L")");
            const std::wstring edge = std::wstring(EdgeName(d.edge)) + (d.fullWidth ? L", full" : L"");
            const std::wstring display = DisplayName(d.monitor);
            const int windows = Host().dockWindows ? Host().dockWindows(static_cast<size_t>(i)) : 1;
            std::wstring state = windows == 0 ? L"Parked (display not connected)"
                                 : d.Each()   ? std::to_wstring(windows) + L" (one per display)"
                                              : L"Shown";
            if (d.locked) state += L", locked";
            if (rebuild) {
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = i;
                item.pszText = id.data();
                ListView_InsertItem(list, &item);
            } else {
                ListView_SetItemText(list, i, 0, id.data());
            }
            ListView_SetItemText(list, i, 1, const_cast<wchar_t*>(edge.c_str()));
            ListView_SetItemText(list, i, 2, const_cast<wchar_t*>(display.c_str()));
            ListView_SetItemText(list, i, 3, state.data());
        }
        const int sel = std::clamp(static_cast<int>(c.editDock), 0, std::max(0, n - 1));
        if (n > 0 && ListView_GetNextItem(list, -1, LVNI_SELECTED) != sel) {
            ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        if (n > 0) ListView_EnsureVisible(list, sel, FALSE);
        if (rebuild) {
            SendMessageW(list, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(list, nullptr, TRUE);
        }
        SetText(kDockEditing, n > 0 ? L"The other pages edit: " + c.docks[static_cast<size_t>(sel)].id : L"");
        Enable(kDockRemove, n > 1);
        bool anyLocked = false, anyUnlocked = false;
        for (const DockConfig& d : c.docks) (d.locked ? anyLocked : anyUnlocked) = true;
        Enable(kLockAllDocks, anyUnlocked);
        Enable(kUnlockAllDocks, anyLocked);
        if (HWND f = GetFocus(); f && IsChild(Hwnd(), f) && !IsWindowEnabled(f)) SetFocus(list);
    }
};

}  // namespace

std::unique_ptr<SettingsPage> MakeDocksPage(SettingsWindow& window) { return std::make_unique<DocksPage>(window); }
