// The Tray page (round 4 phase 8): the takeover of the notify icons (tray.enabled), the edited
// dock's tray item (rows, columns, icon size, the icons shown in its grid), "Reset to Explorer".

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

class TrayPage : public FormPage {
public:
    explicit TrayPage(SettingsWindow& w) : FormPage(w, L"Tray", L'\xE7F4') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Tray");
        Check(kTrayEnabled, kTrayEnabledCaption, L"Show the notification area icons in the dock");
        NextRow();
        Text(kTrayEnabledNote, CtlKind::Note,
             L"The dock takes over the notification area (Shell_TrayWnd): apps register their tray icons with it, and "
             L"every change is passed on to Explorer too, so nothing is lost when the dock quits. Explorer's own "
             L"items (network, battery, input language, Explorer's clock and volume) are not tray icons: they stay "
             L"with Explorer, whose taskbar is hidden anyway (the volume, media and date widgets cover them). Needs a "
             L"Tray item on a dock (Add widget > Tray).",
             kMargin, right - kMargin, 76);
        NextRow(80);
        Text(kTrayNoItem, CtlKind::Value, L"", kMargin, right - kMargin - kButtonW - 8, 22);
        Button(kTrayAddItem, L"Add a tray to this dock", right - kButtonW, kButtonW);
        NextRow(kRowH + 4);
        Section(kTrayGridSection, L"This dock's tray");
        Label(kTrayRowsLabel, L"Rows");
        Spin(kTrayRowsEdit, kTrayRowsSpin, kColX, kNumberW, 1, 2, 1);
        NextRow();
        Label(kTrayColumnsLabel, L"Columns");
        Spin(kTrayColumnsEdit, kTrayColumnsSpin, kColX, kNumberW, 1, 20, 1);
        NextRow();
        Label(kTrayIconSizeLabel, L"Icon size");
        Combo(kTrayIconSizeCombo, kColX, 140, false);
        NextRow();
        Text(kTrayGridNote, CtlKind::Note,
             L"Columns run along the dock and rows across it (on a left or right dock the grid is \"rows\" wide and "
             L"\"columns\" tall). With more icons than cells, or icons hidden below, the last cell opens a drawer "
             L"with every icon. A right click on an icon opens its app's menu; Ctrl + right click opens the dock's.",
             kMargin, right - kMargin, 58);
        NextRow(62);
        Label(kTrayIconsLabel, L"Icons in the grid");
        NextRow(26);
        Add(kTrayIconList, CtlKind::List, WC_LISTVIEWW, L"Tray icons",
            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
            RECT{kMargin, y_, right, y_ + kListH});
        NextRow(kListH + 8);
        Text(kTrayIconsNote, CtlKind::Note,
             L"Unchecked icons are only in the drawer. Icons of apps that are not running are listed while they are "
             L"hidden. Drag an icon within the grid to move it, out of the grid or the drawer onto the dock to pin it "
             L"there (Ctrl + right click on it: Hide / Pin / Unpin).",
             kMargin, right - kMargin, 40);
        NextRow(44);
        Button(kTrayOwnDock, L"Put the tray in its own dock", kMargin, kButtonW);
        Text(kTrayOwnDockNote, CtlKind::Note,
             L"A new dock at the bottom right holding only this dock's tray (with its settings), always shown. The "
             L"Docks page moves or removes it.",
             kMargin + kButtonW + 12, right - kMargin - kButtonW - 12, 40);
        NextRow(kRowH + 16);
        Button(kTrayReset, L"Reset to Explorer", kMargin, kButtonW);
        Text(kTrayResetNote, CtlKind::Note, L"Hands the icons back to Explorer and turns the takeover off.",
             kMargin + kButtonW + 12, right - kMargin - kButtonW - 12, 40);
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
        SetCheck(kTrayEnabled, Cfg().tray.enabled);
        DockItemEntry* e = TrayItem();
        SetText(kTrayNoItem, e ? L"This dock has a tray." : L"This dock has no tray.");
        Enable(kTrayAddItem, !e);
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
        FillList();
    }

    void OnCommand(int id, int code, HWND) override {
        if (id == kTrayEnabled && code == BN_CLICKED) {
            if (Host().command) Host().command(GetCheck(kTrayEnabled) ? ContextMenu::kTrayEnable : ContextMenu::kTrayDisable);
            return;
        }
        if (id == kTrayReset && code == BN_CLICKED) {
            if (Host().command) Host().command(ContextMenu::kTrayDisable);
            return;
        }
        if (id == kTrayOwnDock && code == BN_CLICKED) {
            if (!Host().editDocks || !Host().editDocks(SettingsDockEdit::TrayDock, Cfg().editDock)) {
                MessageBeep(MB_ICONWARNING);
                return;
            }
            Window().Refresh();  // every page now edits the new tray dock
            Window().ConfigApplied();
            return;
        }
        if (id == kTrayAddItem && code == BN_CLICKED) {
            if (TrayItem()) return;
            Cfg().tray.enabled = true;
            Cfg().Edit().AddWidget(ItemKind::Tray);
            Changed(SettingsScope::Pins);  // reloads the dock; the host starts with the item
            Window().Refresh();
            return;
        }
        if (id == kTrayIconSizeCombo && code == CBN_SELCHANGE) {
            const int sel = ComboSel(kTrayIconSizeCombo);
            DockItemEntry* e = TrayItem();
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
        } else if (DockItemEntry* e = TrayItem()) {
            SetSpin(spin, spin == kTrayRowsSpin ? e->rows : e->columns);
        }
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) != kTrayIconList || nm.code != LVN_ITEMCHANGED || Loading() || filling_) return 0;
        const auto& change = reinterpret_cast<const NMLISTVIEW&>(nm);
        // The check box is the state image (1 unchecked, 2 checked).
        if (!(change.uChanged & LVIF_STATE) || ((change.uNewState ^ change.uOldState) & LVIS_STATEIMAGEMASK) == 0) return 0;
        DockItemEntry* e = TrayItem();
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
    // The edited dock's first tray item (null: none).
    DockItemEntry* TrayItem() {
        for (DockItemEntry& e : Cfg().Edit().items) {
            if (e.Kind() == ItemKind::Tray) return &e;
        }
        return nullptr;
    }

    void SetGrid(int spin, int value) {
        DockItemEntry* e = TrayItem();
        if (!e) return;
        int& field = spin == kTrayRowsSpin ? e->rows : e->columns;
        value = std::clamp(value, 1, spin == kTrayRowsSpin ? 2 : 20);
        SetSpin(spin, value);
        if (field == value) return;
        field = value;
        Changed(SettingsScope::Pins);
    }

    // Every icon TrayHost knows (registration order) and the hidden keys of icons not running.
    void FillList() {
        HWND list = Item(kTrayIconList);
        if (!list) return;
        filling_ = true;
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        keys_.clear();
        const DockItemEntry* e = TrayItem();
        std::vector<SettingsTrayIcon> icons = Host().trayIcons ? Host().trayIcons() : std::vector<SettingsTrayIcon>{};
        if (e) {
            for (const std::wstring& key : e->hidden) {
                if (std::none_of(icons.begin(), icons.end(), [&](const SettingsTrayIcon& i) { return i.key == key; })) {
                    icons.push_back(SettingsTrayIcon{key, L"(not running)", L""});
                }
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
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        filling_ = false;
    }

    std::vector<std::wstring> keys_;  // the list's rows
    bool filling_ = false;
};

}  // namespace

std::unique_ptr<SettingsPage> MakeTrayPage(SettingsWindow& window) { return std::make_unique<TrayPage>(window); }
