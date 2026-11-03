// The tray pages (round 4 phase 8; split in round 5 phase D). Global > Tray icons: the takeover of
// the notify icons (tray.enabled) and "Put the tray in its own dock" (a new dock: global). A tray
// item's own Item window: that item's rows, columns, icon size, the icons shown in its grid (hide,
// order) and "Reset to Explorer". No page acts on a dock selected elsewhere.

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

constexpr int kListH = 200;
constexpr int kButtonW = 200;
constexpr int kNumberW = 72;
const int kIconSizes[] = {16, 20, 24};

// Global > Tray icons.
class TrayPage : public FormPage {
public:
    explicit TrayPage(SettingsWindow& w) : FormPage(w, L"Tray icons", L'\xE7F4') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Tray icons");
        Check(kTrayEnabled, kTrayEnabledCaption, L"Show the notification area icons in the dock");
        NextRow();
        Text(kTrayEnabledNote, CtlKind::Note,
             L"The dock takes over the notification area (Shell_TrayWnd): apps register their tray icons with it, and "
             L"every change is passed on to Explorer too, so nothing is lost when the dock quits. Explorer's own "
             L"items (network, battery, input language, Explorer's clock and volume) are not tray icons: they stay "
             L"with Explorer, whose taskbar is hidden anyway (the volume, media and date widgets cover them). Needs a "
             L"System tray item on a dock (drag it from the widget drawer of a dock's Contents tab).",
             kMargin, right - kMargin, 76);
        NextRow(80);
        Text(kTrayGlobalNote, CtlKind::Note,
             L"Each tray item's grid (rows, columns, icon size, which icons it shows and their order) is in that item's "
             L"own settings window: select the System tray tile on its dock's Contents tab and choose Open settings in "
             L"its own window, or Ctrl + right click a tray icon > Tray settings….",
             kMargin, right - kMargin, 58);
        NextRow(66);
        Button(kTrayOwnDock, L"Put the tray in its own dock", kMargin, kButtonW);
        Text(kTrayOwnDockNote, CtlKind::Note,
             L"A new dock at the bottom right holding only the tray item (the first dock's that has one, with its "
             L"settings; else a new one), always shown. Docks and displays moves or removes it.",
             kMargin + kButtonW + 12, right - kMargin - kButtonW - 12, 58);
        NextRow(62);
        Text(kTrayOwnDockStatus, CtlKind::Value, L"", kMargin, right - kMargin, 22);
        NextRow(26);
    }

    void Load() override { SetCheck(kTrayEnabled, Cfg().tray.enabled); }

    void OnCommand(int id, int code, HWND) override {
        if (code != BN_CLICKED) return;
        if (id == kTrayEnabled) {
            if (Host().command) Host().command(GetCheck(kTrayEnabled) ? ContextMenu::kTrayEnable : ContextMenu::kTrayDisable);
            return;
        }
        if (id == kTrayOwnDock) PutInOwnDock();
    }

private:
    // The first dock's tray item that is not alone on its dock moves into a new dock; without one
    // a new dock gets a new tray item. A tray that already has a dock of its own: nothing.
    void PutInOwnDock() {
        const auto& docks = Cfg().docks;
        size_t from = SIZE_MAX, alone = SIZE_MAX, firstPlain = SIZE_MAX;
        for (size_t i = 0; i < docks.size(); ++i) {
            const DockConfig& d = docks[i];
            if (d.IsFolderWindow()) continue;
            if (firstPlain == SIZE_MAX) firstPlain = i;
            if (!d.Has(ItemKind::Tray)) continue;
            if (d.items.size() == 1) {
                if (alone == SIZE_MAX) alone = i;
            } else if (from == SIZE_MAX) {
                from = i;
            }
        }
        if (from == SIZE_MAX && alone != SIZE_MAX) {
            SetText(kTrayOwnDockStatus, L"The tray already has a dock of its own: " + docks[alone].id + L".");
            MessageBeep(MB_ICONWARNING);
            return;
        }
        const size_t index = from != SIZE_MAX ? from : firstPlain;
        const std::wstring fromId = from != SIZE_MAX ? docks[from].id : std::wstring();
        if (index == SIZE_MAX || !Host().editDocks || !Host().editDocks(SettingsDockEdit::TrayDock, index)) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        const std::wstring id = Cfg().docks.back().id;  // the new tray dock (appended)
        SetText(kTrayOwnDockStatus, fromId.empty() ? L"New dock " + id + L" with a tray."
                                                   : L"The tray of " + fromId + L" is now in its own dock: " + id + L".");
        Window().Refresh();
        Window().ConfigApplied();
    }
};

// A tray item's own Item window.
class TrayItemPage : public FormPage {
public:
    explicit TrayItemPage(SettingsWindow& w) : FormPage(w, L"System tray", 0) {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"System tray");
        Section(kTrayGridSection, L"The grid");
        Label(kTrayRowsLabel, L"Rows");
        Spin(kTrayRowsEdit, kTrayRowsSpin, kColX, kNumberW, 1, 2, 1);
        NextRow();
        Label(kTrayColumnsLabel, L"Columns");
        Spin(kTrayColumnsEdit, kTrayColumnsSpin, kColX, kNumberW, 1, 20, 1);
        NextRow();
        Label(kTrayIconSizeLabel, L"Icon size");
        Combo(kTrayIconSizeCombo, kColX, 120, false);
        NextRow();
        Text(kTrayGridNote, CtlKind::Note,
             L"Columns run along the dock and rows across it (on a left or right dock the grid is \"rows\" wide and "
             L"\"columns\" tall). With more icons than cells, or icons hidden below, the last cell opens a drawer "
             L"with every icon. A right click on an icon opens its app's menu; Ctrl + right click opens the tray menu.",
             kMargin, right - kMargin, 76);
        NextRow(80);
        Label(kTrayIconsLabel, L"Icons in the grid (in order)");
        NextRow(26);
        Add(kTrayIconList, CtlKind::List, WC_LISTVIEWW, L"Tray icons",
            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
            RECT{kMargin, y_, right, y_ + kListH});
        NextRow(kListH + 8);
        Button(kTrayIconUp, L"Move up", kMargin, 110);
        Button(kTrayIconDown, L"Move down", kMargin + 122, 110);
        NextRow(kRowH + 4);
        Text(kTrayIconsNote, CtlKind::Note,
             L"Unchecked icons are only in the drawer. Icons of apps that are not running are listed while they are "
             L"hidden or ordered. Move up / down (or a drag within the grid on the dock) sets the order; drag an icon out "
             L"of the grid or the drawer onto the dock to pin it there (Ctrl + right click on it: Hide / Pin / Unpin).",
             kMargin, right - kMargin, 76);
        NextRow(80);
        Button(kTrayReset, L"Reset to Explorer", kMargin, 160);
        Text(kTrayResetNote, CtlKind::Note, L"Hands every icon back to Explorer and turns the takeover off (on every dock).",
             kMargin + 172, right - kMargin - 172, 40);
        NextRow(kRowH + 12);
    }

    void Created() override {
        for (int size : kIconSizes) ComboAdd(kTrayIconSizeCombo, (std::to_wstring(size) + L" px").c_str());
        HWND list = Item(kTrayIconList);
        if (!list) return;
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        const wchar_t* names[] = {L"Icon", L"App", L"Key"};
        for (int i = 0; i < 3; ++i) {
            col.pszText = const_cast<wchar_t*>(names[i]);
            col.cx = 120;
            col.iSubItem = i;
            ListView_InsertColumn(list, i, &col);
        }
    }

    void OnStyleChanged() override {
        HWND list = Item(kTrayIconList);
        const Ctl* c = Find(kTrayIconList);
        if (!list || !c) return;
        const int total = Window().Px(c->dip.right - c->dip.left) - GetSystemMetricsForDpi(SM_CXVSCROLL, Window().Dpi()) - 4;
        ListView_SetColumnWidth(list, 0, total * 45 / 100);
        ListView_SetColumnWidth(list, 1, total * 25 / 100);
        ListView_SetColumnWidth(list, 2, total - total * 45 / 100 - total * 25 / 100);
    }

    void Load() override {
        const DockItemEntry* e = Tray();
        for (int id : {kTrayRowsEdit, kTrayRowsSpin, kTrayColumnsEdit, kTrayColumnsSpin, kTrayIconSizeCombo, kTrayIconList}) {
            Enable(id, e != nullptr);
        }
        if (e) {
            SetSpin(kTrayRowsSpin, e->rows);
            SetSpin(kTrayColumnsSpin, e->columns);
            int sel = 1;
            for (int i = 0; i < 3; ++i) {
                if (kIconSizes[i] == e->trayIconSize) sel = i;
            }
            SetComboSel(kTrayIconSizeCombo, sel);
        }
        Enable(kTrayReset, Cfg().tray.enabled);
        FillList(-1);
    }

    void OnCommand(int id, int code, HWND) override {
        if (id == kTrayReset && code == BN_CLICKED) {
            if (Host().command) Host().command(ContextMenu::kTrayDisable);
            return;
        }
        if ((id == kTrayIconUp || id == kTrayIconDown) && code == BN_CLICKED) {
            MoveSelected(id == kTrayIconUp ? -1 : 1);
            return;
        }
        if (id == kWidgetReset && code == BN_CLICKED) {  // the footer's Reset to defaults
            DockItemEntry* e = Tray();
            if (!e) return;
            DockItemEntry fresh{e->kind};
            if (e->rows == fresh.rows && e->columns == fresh.columns && e->trayIconSize == fresh.trayIconSize &&
                e->hidden.empty() && e->order.empty()) {
                return;
            }
            e->rows = fresh.rows;
            e->columns = fresh.columns;
            e->trayIconSize = fresh.trayIconSize;
            e->hidden.clear();
            e->order.clear();
            Changed(SettingsScope::Pins);
            Window().Refresh();
            return;
        }
        if (id == kTrayIconSizeCombo && code == CBN_SELCHANGE) {
            const int sel = ComboSel(kTrayIconSizeCombo);
            DockItemEntry* e = Tray();
            if (!e || sel < 0 || sel >= 3 || e->trayIconSize == kIconSizes[sel]) return;
            e->trayIconSize = kIconSizes[sel];
            Changed(SettingsScope::Pins);
        }
    }

    void Spun(int spin, int pos) override { SetGrid(spin, pos); }
    void Commit(int id) override {
        const int spin = id == kTrayRowsEdit ? kTrayRowsSpin : id == kTrayColumnsEdit ? kTrayColumnsSpin : 0;
        if (!spin) return;
        int value = 0;
        if (ReadInt(id, 1, spin == kTrayRowsSpin ? 2 : 20, value)) {
            SetGrid(spin, value);
        } else if (DockItemEntry* e = Tray()) {
            SetSpin(spin, spin == kTrayRowsSpin ? e->rows : e->columns);
        }
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) != kTrayIconList || Loading() || filling_) return 0;
        if (nm.code != LVN_ITEMCHANGED) return 0;
        const auto& change = reinterpret_cast<const NMLISTVIEW&>(nm);
        if ((change.uChanged & LVIF_STATE) && ((change.uNewState ^ change.uOldState) & LVIS_SELECTED)) UpdateButtons();
        // The check box is the state image (1 unchecked, 2 checked).
        if (!(change.uChanged & LVIF_STATE) || ((change.uNewState ^ change.uOldState) & LVIS_STATEIMAGEMASK) == 0) return 0;
        DockItemEntry* e = Tray();
        if (!e || change.iItem < 0 || static_cast<size_t>(change.iItem) >= keys_.size()) return 0;
        const bool shown = ((change.uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
        const std::wstring& key = keys_[static_cast<size_t>(change.iItem)];
        auto it = std::find(e->hidden.begin(), e->hidden.end(), key);
        if (shown && it != e->hidden.end()) {
            e->hidden.erase(it);
        } else if (!shown && it == e->hidden.end()) {
            e->hidden.push_back(key);
        } else {
            return 0;
        }
        Changed(SettingsScope::Pins);
        return 0;
    }

private:
    // The window's item (null: gone, or not a tray item).
    DockItemEntry* Tray() {
        DockItemEntry* e = Window().TargetItem();
        return e && e->Kind() == ItemKind::Tray ? e : nullptr;
    }

    void SetGrid(int spin, int value) {
        DockItemEntry* e = Tray();
        if (!e) return;
        int& field = spin == kTrayRowsSpin ? e->rows : e->columns;
        value = std::clamp(value, 1, spin == kTrayRowsSpin ? 2 : 20);
        SetSpin(spin, value);
        if (field == value) return;
        field = value;
        Changed(SettingsScope::Pins);
    }

    int SelectedRow() const {
        HWND list = Item(kTrayIconList);
        return list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    }

    void UpdateButtons() {
        const int sel = SelectedRow();
        const bool ok = Tray() != nullptr && sel >= 0;
        Enable(kTrayIconUp, ok && sel > 0);
        Enable(kTrayIconDown, ok && sel + 1 < static_cast<int>(keys_.size()));
    }

    // Moves the selected icon one row up / down: the list's order becomes the item's `order`.
    void MoveSelected(int step) {
        DockItemEntry* e = Tray();
        const int sel = SelectedRow();
        const int to = sel + step;
        if (!e || sel < 0 || to < 0 || to >= static_cast<int>(keys_.size())) {
            MessageBeep(MB_OK);
            return;
        }
        std::vector<std::wstring> order = keys_;
        std::swap(order[static_cast<size_t>(sel)], order[static_cast<size_t>(to)]);
        e->order = std::move(order);
        Changed(SettingsScope::Pins);
        FillList(to);
    }

    // The icons in the grid's order (the item's `order` first, then TrayHost's registration
    // order), then hidden / ordered keys of icons that are not running.
    void FillList(int select) {
        HWND list = Item(kTrayIconList);
        if (!list) return;
        if (select < 0) select = SelectedRow();
        filling_ = true;
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        keys_.clear();
        const DockItemEntry* e = Tray();
        const std::vector<SettingsTrayIcon> running = Host().trayIcons ? Host().trayIcons() : std::vector<SettingsTrayIcon>{};
        std::vector<SettingsTrayIcon> icons;
        auto has = [&](const std::wstring& key) {
            return std::any_of(icons.begin(), icons.end(), [&](const SettingsTrayIcon& i) { return i.key == key; });
        };
        auto runningIcon = [&](const std::wstring& key) -> const SettingsTrayIcon* {
            for (const SettingsTrayIcon& i : running) {
                if (i.key == key) return &i;
            }
            return nullptr;
        };
        if (e) {
            for (const std::wstring& key : e->order) {
                if (has(key)) continue;
                const SettingsTrayIcon* r = runningIcon(key);
                icons.push_back(r ? *r : SettingsTrayIcon{key, L"(not running)", L""});
            }
        }
        for (const SettingsTrayIcon& i : running) {
            if (!has(i.key)) icons.push_back(i);
        }
        if (e) {
            for (const std::wstring& key : e->hidden) {
                if (!has(key)) icons.push_back(SettingsTrayIcon{key, L"(not running)", L""});
            }
        }
        for (size_t i = 0; i < icons.size(); ++i) {
            const SettingsTrayIcon& icon = icons[i];
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            std::wstring title = icon.title;
            item.pszText = title.data();
            const int at = ListView_InsertItem(list, &item);
            std::wstring app = icon.app;
            ListView_SetItemText(list, at, 1, app.data());
            std::wstring key = icon.key;
            ListView_SetItemText(list, at, 2, key.data());
            const bool hidden = e && std::find(e->hidden.begin(), e->hidden.end(), icon.key) != e->hidden.end();
            ListView_SetCheckState(list, at, !hidden);
            keys_.push_back(icon.key);
        }
        if (select >= 0 && select < static_cast<int>(keys_.size())) {
            ListView_SetItemState(list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list, select, FALSE);
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        filling_ = false;
        UpdateButtons();
    }

    std::vector<std::wstring> keys_;  // the list's rows
    bool filling_ = false;
};

}  // namespace

std::unique_ptr<SettingsPage> MakeTrayPage(SettingsWindow& window) { return std::make_unique<TrayPage>(window); }
std::unique_ptr<SettingsPage> MakeTrayItemPage(SettingsWindow& window) { return std::make_unique<TrayItemPage>(window); }
