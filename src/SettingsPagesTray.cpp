// The tray pages (round 4 phase 8; split in round 5 phase D). Global > Tray icons: the takeover of
// the notify icons (tray.enabled) and "Put the tray in its own dock" (a new dock: global). A tray
// item's own Item window: that item's grid (rows, columns, icon size, spacing, labels, colours), its
// arrow and drawer (round 24 phase 24C), the icons shown in its grid (hide, order) and "Reset to
// Explorer". No page acts on a dock selected elsewhere.

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
constexpr int kTrackW = 212;  // a slider's width (the Appearance tab's), its value 4 DIPs after it
constexpr int kComboW = 240;

// Round 24 phase 24C: the tray item's number rows (spinner + edit), each bound to one field. One
// table serves Spun / Commit / Load instead of a branch per row.
struct TrayNumber {
    int edit;
    int spin;
    int lo;
    int hi;
    int DockItemEntry::*field;
};
const TrayNumber kTrayNumbers[] = {
    {kTrayRowsEdit, kTrayRowsSpin, 1, 20, &DockItemEntry::rows},  // round 16 phase 16B: rows 1 - 20 too
    {kTrayColumnsEdit, kTrayColumnsSpin, 1, 20, &DockItemEntry::columns},
    {kTraySpacingEdit, kTraySpacingSpin, 0, 8, &DockItemEntry::traySpacing},
    {kTrayDrawerLabelSizeEdit, kTrayDrawerLabelSizeSpin, 10, 16, &DockItemEntry::trayDrawerLabelSize},
    {kTrayDrawerColumnsEdit, kTrayDrawerColumnsSpin, 0, 8, &DockItemEntry::trayDrawerColumns},
};

// True when every look setting of tray item `e` is the factory default (a fresh entry's): the
// footer's Reset to factory defaults then has nothing to do. Every field the page sets is here.
bool TrayLookIsDefault(const DockItemEntry& e) {
    const DockItemEntry f{e.kind};
    return e.rows == f.rows && e.columns == f.columns && e.trayIconSize == f.trayIconSize &&
           e.traySpacing == f.traySpacing && e.trayLabels == f.trayLabels && e.trayColour == f.trayColour &&
           e.trayExpander == f.trayExpander && e.trayExpanderStyle == f.trayExpanderStyle &&
           e.trayExpanderShow == f.trayExpanderShow && e.trayDrawerOpen == f.trayDrawerOpen &&
           e.trayDrawerShows == f.trayDrawerShows && e.trayDrawerIconSize == f.trayDrawerIconSize &&
           e.trayDrawerLabels == f.trayDrawerLabels && e.trayDrawerLabelSize == f.trayDrawerLabelSize &&
           e.trayDrawerColumns == f.trayDrawerColumns && e.trayDrawerTitle == f.trayDrawerTitle && e.hidden.empty() &&
           e.order.empty();
}

// ... and puts them back (the same fields as TrayLookIsDefault).
void ResetTrayLook(DockItemEntry& e) {
    const DockItemEntry f{e.kind};
    e.rows = f.rows;
    e.columns = f.columns;
    e.trayIconSize = f.trayIconSize;
    e.traySpacing = f.traySpacing;
    e.trayLabels = f.trayLabels;
    e.trayColour = f.trayColour;
    e.trayExpander = f.trayExpander;
    e.trayExpanderStyle = f.trayExpanderStyle;
    e.trayExpanderShow = f.trayExpanderShow;
    e.trayDrawerOpen = f.trayDrawerOpen;
    e.trayDrawerShows = f.trayDrawerShows;
    e.trayDrawerIconSize = f.trayDrawerIconSize;
    e.trayDrawerLabels = f.trayDrawerLabels;
    e.trayDrawerLabelSize = f.trayDrawerLabelSize;
    e.trayDrawerColumns = f.trayDrawerColumns;
    e.trayDrawerTitle = f.trayDrawerTitle;
    e.hidden.clear();
    e.order.clear();
}

// Global > Tray icons.
class TrayPage : public FormPage {
public:
    explicit TrayPage(SettingsWindow& w) : FormPage(w, L"Tray icons", L'\xE7F4') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Tray icons");
        Lead(L"Whether WinDock takes over the notification area.");  // round 12 phase 12F
        HelpTopic(L"guide-settings.md#tray-icons");
        // Round 8 phase 8A: one-line notes, the explanations as their tooltips; round 8 phase 8D:
        // one card, the note the check box's hint.
        Section(kTraySection, L"Notification area", L"guide-settings.md#notification-area");
        Check(kTrayEnabled, kTrayEnabledCaption, L"Show the notification area icons in the dock",
              L"Needs a System tray item on a dock (from its Contents tab)", kTrayEnabledNote);
        static constexpr wchar_t kEnabledTip[] =
            L"The dock takes over the notification area (Shell_TrayWnd): apps register their tray icons with it, and "
            L"every change is passed on to Explorer too, so nothing is lost when the dock quits. Explorer's own items "
            L"(network, battery, input language, Explorer's clock and volume) are not tray icons: they stay with "
            L"Explorer, whose taskbar is hidden anyway (the volume, media and date widgets cover them). Needs a System "
            L"tray item on a dock (drag it from the widget drawer of a dock's Contents tab).";
        Tip(kTrayEnabled, kEnabledTip);
        Tip(kTrayEnabledCaption, kEnabledTip);
        Tip(kTrayEnabledNote, kEnabledTip);
        NextRow();
        Ctl& grid = Text(kTrayGlobalNote, CtlKind::Note, L"Each tray item's grid and icons: its own settings window.",
                         kMargin, right - kMargin, 20);
        grid.maxLines = 2;
        Tip(kTrayGlobalNote,
            L"Each tray item's grid (rows, columns, icon size, which icons it shows and their order) is in that item's own "
            L"settings window: right-click the tray on the dock > Tray settings…, select the System tray tile on its "
            L"dock's Contents tab and choose Tray settings…, or Ctrl + right click a tray icon > Tray settings….");
        NextRow(24);
        Button(kTrayOwnDock, L"Put the tray in its own dock", kMargin, kButtonW);
        Text(kTrayOwnDockNote, CtlKind::Note, L"A new dock with only the tray", kMargin + kButtonW + 12,
             right - kMargin - kButtonW - 12);  // one line beside the button (cut with "…" when narrow)
        static constexpr wchar_t kOwnDockTip[] =
            L"A new dock at the bottom right holding only the tray item (the first dock's that has one, with its "
            L"settings; else a new one), always shown. Docks and displays moves or removes it.";
        Tip(kTrayOwnDock, kOwnDockTip);
        Tip(kTrayOwnDockNote, kOwnDockTip);
        NextRow();
        Text(kTrayOwnDockStatus, CtlKind::Value, L"", kMargin, right - kMargin, 22);
        NextRow(24);
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
            if (!d.IsDock()) continue;
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
        Title(L"System tray");  // a no-op in an Item window (the header names it)
        Lead(L"The tray icons this item shows on the dock, and their grid.");  // round 12 phase 12F
        HelpTopic(L"guide-items.md#the-tray");
        // Round 8 phase 8C: three cards, hints under the labels, the explanations as tooltips.
        Section(kTrayGridSection, L"The grid", L"guide-items.md#the-grid");
        // Round 16 phase 16B: rows / columns as seen on screen on every edge; both 1 - 20.
        Label(kTrayRowsLabel, L"Rows", L"Top to bottom", kTrayGridNote);
        Tip(kTrayRowsLabel,
            L"Rows are stacked top to bottom and columns run left to right, on every edge (round 16 phase 16B; before, "
            L"columns ran along the dock whichever way it faced). Across the bar (rows on a bottom / top dock, columns "
            L"on a left / right one) two cells always fit, shrunk to half an icon; more fit only when the bar is thick "
            L"enough at full size (Dock settings > Position > Fixed thickness, or titles beside the icons). With more "
            L"icons than cells, or icons hidden below, the arrow opens a drawer (The arrow, The drawer). A right click on "
            L"an icon opens its app's menu; Ctrl + right click opens the tray menu.");
        Spin(kTrayRowsEdit, kTrayRowsSpin, kColX, kNumberW, 1, 20, 1);
        NextRow();
        Label(kTrayColumnsLabel, L"Columns", L"Left to right");
        Tip(kTrayColumnsLabel,
            L"Columns run left to right on every edge; on a left or right dock they run across the bar, where more than "
            L"two fit only when the bar is thick enough. With more icons than cells the arrow opens a drawer.");
        Spin(kTrayColumnsEdit, kTrayColumnsSpin, kColX, kNumberW, 1, 20, 1);
        NextRow();
        // Round 24 phase 24C: the size became 16 - 32 (a slider; the 16 / 20 / 24 combo 4110 is
        // retired), and the grid gained spacing, labels and colours. Every default is the old look.
        Label(kTrayIconSizeLabel, L"Icon size");
        static constexpr wchar_t kSizeTip[] =
            L"The size of the notify icons in the grid, 16 - 32 DIPs. Default: 20. Larger sizes are drawn from the "
            L"app's own larger icon where it has one, so they stay sharp.";
        Tip(kTrayIconSizeLabel, kSizeTip);
        Trackbar(kTrayIconSizeTrack, kColX - 6, kTrackW, 16, 32, 4);
        Tip(kTrayIconSizeTrack, kSizeTip);
        Text(kTrayIconSizeValue, CtlKind::Value, L"", kColX + kTrackW + 4, 48);
        NextRow();
        Label(kTraySpacingLabel, L"Space around icons");
        Tip(kTraySpacingLabel,
            L"Empty DIPs on every side of each grid icon, 0 - 8: a cell is the icon plus twice this. Default: 3. Less "
            L"space fits more icons in the same bar.");
        Spin(kTraySpacingEdit, kTraySpacingSpin, kColX, kNumberW, 0, 8, 1);
        NextRow();
        Label(kTrayLabelsLabel, L"Labels");
        Tip(kTrayLabelsLabel,
            L"None: icons only (the default). Beside the icon: each cell grows along the bar by the app's name, cut with "
            L"\x2026 when long. Only where the bar has room for it across: a side dock too narrow stays icons only.");
        Combo(kTrayLabelsCombo, kColX, kComboW, false);
        NextRow();
        Label(kTrayColourLabel, L"Colours");
        Tip(kTrayColourLabel,
            L"Original: the icons as their apps draw them (the default). One colour: the grid's icons in the theme's text "
            L"colour, for a calmer bar. The drawer always shows the original colours.");
        Combo(kTrayColourCombo, kColX, kComboW, false);
        NextRow();

        // Round 24 phase 24C: the arrow (the overflow button) and the drawer it opens, between the
        // grid and its icon list: they shape what is on the dock, the list only which icons.
        Section(kTrayArrowSection, L"The arrow", L"guide-items.md#the-arrow");
        Label(kTrayArrowPlaceLabel, L"Place");
        Tip(kTrayArrowPlaceLabel,
            L"Where the arrow that opens the drawer sits. Last cell: the grid's last cell (the default). First cell: the "
            L"grid's first cell, the icons after it. Strip before / after the icons: a slim strip outside the grid that "
            L"takes no cell, so every cell holds an icon.");
        Combo(kTrayArrowPlaceCombo, kColX, kComboW, false);
        NextRow();
        Label(kTrayArrowLookLabel, L"Looks like");
        Tip(kTrayArrowLookLabel,
            L"Chevron: an arrow that turns over while the drawer is open (the default). Dots: three dots. Count: how many "
            L"icons the grid does not show (+3).");
        Combo(kTrayArrowLookCombo, kColX, kComboW, false);
        NextRow();
        Label(kTrayArrowShowLabel, L"Shown");
        Tip(kTrayArrowShowLabel,
            L"When there are more icons: only while some icons do not fit the grid or are hidden (the default). Always: "
            L"the drawer is one click away even when every icon fits.");
        Combo(kTrayArrowShowCombo, kColX, kComboW, false);
        NextRow();
        Label(kTrayArrowOpenLabel, L"Opens the drawer on");
        Tip(kTrayArrowOpenLabel,
            L"Click: the drawer opens when you click the arrow and closes when you click elsewhere (the default). Hover: "
            L"it opens when the pointer rests on the arrow, and closes shortly after the pointer leaves both the arrow "
            L"and the drawer.");
        Combo(kTrayArrowOpenCombo, kColX, kComboW, false);
        NextRow();

        Section(kTrayDrawerSection, L"The drawer", L"guide-items.md#the-drawer");
        Label(kTrayDrawerShowsLabel, L"Shows");
        Tip(kTrayDrawerShowsLabel,
            L"Every icon: the drawer lists all the tray's icons, the grid's too (the default). Only icons not in the "
            L"grid: the ones that did not fit or are hidden. Drags between the grid and the drawer work either way.");
        Combo(kTrayDrawerShowsCombo, kColX, kComboW, false);
        NextRow();
        Label(kTrayDrawerIconSizeLabel, L"Icon size");
        static constexpr wchar_t kDrawerSizeTip[] = L"The size of the icons in the drawer, 16 - 48 DIPs. Default: 40.";
        Tip(kTrayDrawerIconSizeLabel, kDrawerSizeTip);
        Trackbar(kTrayDrawerIconSizeTrack, kColX - 6, kTrackW, 16, 48, 4);
        Tip(kTrayDrawerIconSizeTrack, kDrawerSizeTip);
        Text(kTrayDrawerIconSizeValue, CtlKind::Value, L"", kColX + kTrackW + 4, 48);
        NextRow();
        Label(kTrayDrawerLabelsLabel, L"Labels");
        Tip(kTrayDrawerLabelsLabel,
            L"Below: each icon with its app's name under it, in a grid (the default). Beside: one icon per row with the "
            L"name after it, a list. None: icons only, closer together.");
        Combo(kTrayDrawerLabelsCombo, kColX, kComboW, false);
        NextRow();
        Label(kTrayDrawerLabelSizeLabel, L"Label size");
        Tip(kTrayDrawerLabelSizeLabel, L"The size of the names in the drawer, 10 - 16. Default: 12.");
        Spin(kTrayDrawerLabelSizeEdit, kTrayDrawerLabelSizeSpin, kColX, kNumberW, 10, 16, 1);
        NextRow();
        Label(kTrayDrawerColumnsLabel, L"Columns");
        static constexpr wchar_t kColumnsTip[] =
            L"How many icons side by side in the drawer, 1 - 8; 0: automatic, as many as suit the number of icons (the "
            L"default). Greyed while the labels are beside the icons: that list has one column.";
        Tip(kTrayDrawerColumnsLabel, kColumnsTip);
        Spin(kTrayDrawerColumnsEdit, kTrayDrawerColumnsSpin, kColX, kNumberW, 0, 8, 1);
        Text(kTrayDrawerColumnsValue, CtlKind::Note, L"", kColX + kNumberW + 8, 120);
        Tip(kTrayDrawerColumnsValue, kColumnsTip);
        NextRow();
        Check(kTrayDrawerTitle, kTrayDrawerTitleCaption, L"Show the title");
        static constexpr wchar_t kTitleTip[] =
            L"The drawer's title row (Notification area) with the settings gear at its right, which opens this window. "
            L"Off: no title and no gear, only the icons (Ctrl + right click on a tray icon > Tray settings\x2026 still "
            L"opens this window).";
        Tip(kTrayDrawerTitle, kTitleTip);
        Tip(kTrayDrawerTitleCaption, kTitleTip);
        NextRow();

        Section(kTrayIconsLabel, L"Icons in the grid", L"guide-items.md#icons-in-the-grid");
        WideCard();  // round 12 phase 12F: the icon list gains from the width
        Text(kTrayIconsNote, CtlKind::Note, L"In order; unchecked icons are only in the drawer", kMargin, right - kMargin, 20);
        Tip(kTrayIconsNote,
            L"Unchecked icons are only in the drawer. Icons of apps that are not running are listed while they are "
            L"hidden or ordered. Move up / down (or a drag within the grid on the dock) sets the order; drag an icon out "
            L"of the grid or the drawer onto the dock to pin it there (Ctrl + right click on it: Hide / Pin / Unpin).");
        NextRow(24);
        Add(kTrayIconList, CtlKind::List, WC_LISTVIEWW, L"Tray icons",
            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
            RECT{kMargin, y_, right, y_ + kListH});
        Tip(kTrayIconList, L"The notify icons the tray has now. A cleared check box hides that icon from the grid (it stays in Windows' "
                           L"own list); Move up / Move down change their order. Every icon is shown by default.");
        NextRow(kListH + 8);
        Button(kTrayIconUp, L"Move up", kMargin, 110);
        Button(kTrayIconDown, L"Move down", kMargin + 122, 110);
        // Round 9 phase 9A: why they may be greyed.
        Tip(kTrayIconUp, L"Moves the selected icon one place earlier in the grid (select an icon below the first one).");
        Tip(kTrayIconDown, L"Moves the selected icon one place later in the grid (select an icon above the last one).");
        NextRow();
        Section(kTrayTakeoverSection, L"Takeover", L"guide-items.md#takeover");
        Label(kTrayResetLabel, L"Explorer's icons", L"Back to the taskbar, every dock", kTrayResetNote);
        Tip(kTrayResetLabel, L"Reset to Explorer hands every icon back to Explorer and turns the takeover off (on every dock); "
                             L"App settings > Tray icons turns it on again.");
        Button(kTrayReset, L"Reset to Explorer", kColX, 160);
        Tip(kTrayReset, L"Hands every icon back to Explorer and turns the takeover off (on every dock). Greyed while the "
                        L"takeover is off: Explorer has the icons already (App settings > Tray icons turns it on).");
        NextRow();
    }

    void OnResized() override { OnStyleChanged(); }  // round 8 phase 8A: the list's columns

    void Created() override {
        // Round 24 phase 24C: each combo's entries in its config enum's order (Config.h).
        ComboAdd(kTrayLabelsCombo, L"None");  // TrayLabels
        ComboAdd(kTrayLabelsCombo, L"Beside the icon");
        ComboAdd(kTrayColourCombo, L"Original");  // TrayColour
        ComboAdd(kTrayColourCombo, L"One colour");
        ComboAdd(kTrayArrowPlaceCombo, L"Last cell");  // TrayExpander
        ComboAdd(kTrayArrowPlaceCombo, L"First cell");
        ComboAdd(kTrayArrowPlaceCombo, L"Strip before the icons");
        ComboAdd(kTrayArrowPlaceCombo, L"Strip after the icons");
        ComboAdd(kTrayArrowLookCombo, L"Chevron");  // TrayExpanderStyle
        ComboAdd(kTrayArrowLookCombo, L"Dots");
        ComboAdd(kTrayArrowLookCombo, L"Count");
        ComboAdd(kTrayArrowShowCombo, L"When there are more icons");  // TrayExpanderShow
        ComboAdd(kTrayArrowShowCombo, L"Always");
        ComboAdd(kTrayArrowOpenCombo, L"Click");  // TrayDrawerOpen
        ComboAdd(kTrayArrowOpenCombo, L"Hover");
        ComboAdd(kTrayDrawerShowsCombo, L"Every icon");  // TrayDrawerShows
        ComboAdd(kTrayDrawerShowsCombo, L"Only icons not in the grid");
        ComboAdd(kTrayDrawerLabelsCombo, L"Below");  // TrayDrawerLabels
        ComboAdd(kTrayDrawerLabelsCombo, L"Beside");
        ComboAdd(kTrayDrawerLabelsCombo, L"None");
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
        for (int id : {kTrayIconSizeTrack, kTrayLabelsCombo, kTrayColourCombo, kTrayArrowPlaceCombo, kTrayArrowLookCombo,
                       kTrayArrowShowCombo, kTrayArrowOpenCombo, kTrayDrawerShowsCombo, kTrayDrawerIconSizeTrack,
                       kTrayDrawerLabelsCombo, kTrayDrawerTitle, kTrayIconList}) {
            Enable(id, e != nullptr);
        }
        for (const TrayNumber& n : kTrayNumbers) {
            Enable(n.edit, e != nullptr);
            Enable(n.spin, e != nullptr);
        }
        if (e) {
            for (const TrayNumber& n : kTrayNumbers) SetSpin(n.spin, e->*n.field);
            SetTrack(kTrayIconSizeTrack, e->trayIconSize);
            SetText(kTrayIconSizeValue, std::to_wstring(e->trayIconSize));
            SetComboSel(kTrayLabelsCombo, static_cast<int>(e->trayLabels));
            SetComboSel(kTrayColourCombo, static_cast<int>(e->trayColour));
            SetComboSel(kTrayArrowPlaceCombo, static_cast<int>(e->trayExpander));
            SetComboSel(kTrayArrowLookCombo, static_cast<int>(e->trayExpanderStyle));
            SetComboSel(kTrayArrowShowCombo, static_cast<int>(e->trayExpanderShow));
            SetComboSel(kTrayArrowOpenCombo, static_cast<int>(e->trayDrawerOpen));
            SetComboSel(kTrayDrawerShowsCombo, static_cast<int>(e->trayDrawerShows));
            SetTrack(kTrayDrawerIconSizeTrack, e->trayDrawerIconSize);
            SetText(kTrayDrawerIconSizeValue, std::to_wstring(e->trayDrawerIconSize));
            SetComboSel(kTrayDrawerLabelsCombo, static_cast<int>(e->trayDrawerLabels));
            SetCheck(kTrayDrawerTitle, e->trayDrawerTitle);
        }
        UpdateDrawerColumns();
        Enable(kTrayReset, Cfg().tray.enabled);
        // Round 12 phase 12F: while the takeover is off the list is empty and Reset to Explorer greyed:
        // the existing hint slots say why and the way on.
        const bool on = Cfg().tray.enabled;
        SetText(kTrayIconsNote, on ? L"In order; unchecked icons are only in the drawer"
                                   : L"The takeover is off, so Explorer has the icons. Turn it on in App settings > Tray icons.");
        SetText(kTrayResetNote, on ? L"Back to the taskbar, every dock" : L"Off: Explorer has them");
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
        if (id == kWidgetReset && code == BN_CLICKED) {  // the footer's Reset to factory defaults… (confirmed: round 10 phase 10D)
            // Round 24 phase 24C: every look field of the grid, the arrow and the drawer too.
            DockItemEntry* e = Tray();
            if (!e || TrayLookIsDefault(*e)) return;
            ResetTrayLook(*e);
            Changed(SettingsScope::Pins);
            Window().Refresh();
            return;
        }
        if (id == kTrayDrawerTitle && code == BN_CLICKED) {
            DockItemEntry* e = Tray();
            const bool on = GetCheck(kTrayDrawerTitle);
            if (!e || e->trayDrawerTitle == on) return;
            e->trayDrawerTitle = on;
            Changed(SettingsScope::Pins);
            return;
        }
        if (code == CBN_SELCHANGE) {
            const int sel = ComboSel(id);
            DockItemEntry* e = Tray();
            if (!e || sel < 0) return;
            bool changed = false;
            switch (id) {
                case kTrayLabelsCombo: changed = SetEnum(e->trayLabels, sel, 2); break;
                case kTrayColourCombo: changed = SetEnum(e->trayColour, sel, 2); break;
                case kTrayArrowPlaceCombo: changed = SetEnum(e->trayExpander, sel, 4); break;
                case kTrayArrowLookCombo: changed = SetEnum(e->trayExpanderStyle, sel, 3); break;
                case kTrayArrowShowCombo: changed = SetEnum(e->trayExpanderShow, sel, 2); break;
                case kTrayArrowOpenCombo: changed = SetEnum(e->trayDrawerOpen, sel, 2); break;
                case kTrayDrawerShowsCombo: changed = SetEnum(e->trayDrawerShows, sel, 2); break;
                case kTrayDrawerLabelsCombo:
                    changed = SetEnum(e->trayDrawerLabels, sel, 3);
                    UpdateDrawerColumns();
                    break;
                default: return;
            }
            if (changed) Changed(SettingsScope::Pins);
        }
    }

    // Round 24 phase 24C: the two icon size sliders, applied on release (the value follows the thumb).
    void Track(int id, int pos, bool released) override {
        if (id != kTrayIconSizeTrack && id != kTrayDrawerIconSizeTrack) return;
        const bool drawer = id == kTrayDrawerIconSizeTrack;
        pos = std::clamp(pos, 16, drawer ? 48 : 32);
        SetText(drawer ? kTrayDrawerIconSizeValue : kTrayIconSizeValue, std::to_wstring(pos));
        DockItemEntry* e = Tray();
        if (!released || !e) return;
        int& field = drawer ? e->trayDrawerIconSize : e->trayIconSize;
        if (field == pos) return;
        field = pos;
        Changed(SettingsScope::Pins);
    }

    void Spun(int spin, int pos) override {
        for (const TrayNumber& n : kTrayNumbers) {
            if (n.spin == spin) SetNumber(n, pos);
        }
    }
    void Commit(int id) override {
        for (const TrayNumber& n : kTrayNumbers) {
            if (n.edit != id) continue;
            int value = 0;
            if (ReadInt(id, n.lo, n.hi, value)) {
                SetNumber(n, value);
            } else if (DockItemEntry* e = Tray()) {
                SetSpin(n.spin, e->*n.field);  // not a number: back to the current value
            }
            return;
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

    void SetNumber(const TrayNumber& n, int value) {
        DockItemEntry* e = Tray();
        if (!e) return;
        int& field = e->*n.field;
        value = std::clamp(value, n.lo, n.hi);
        SetSpin(n.spin, value);
        if (n.spin == kTrayDrawerColumnsSpin) UpdateDrawerColumns(value);
        if (field == value) return;
        field = value;
        Changed(SettingsScope::Pins);
    }

    // A combo's selection into config enum `field` (`count` values). True when it changed.
    template <class E>
    static bool SetEnum(E& field, int sel, int count) {
        const E value = static_cast<E>(std::min(sel, count - 1));
        if (field == value) return false;
        field = value;
        return true;
    }

    // The drawer's Columns row: its note reads "Automatic" at 0, and the row is greyed while the
    // drawer's labels are beside the icons (that list layout has one column, phase 24B).
    void UpdateDrawerColumns(int value = -1) {
        const DockItemEntry* e = Tray();
        if (value < 0) value = e ? e->trayDrawerColumns : 0;
        SetText(kTrayDrawerColumnsValue, value == 0 ? L"Automatic" : value == 1 ? L"column" : L"columns");
        const bool on = e && e->trayDrawerLabels != TrayDrawerLabels::Beside;
        Enable(kTrayDrawerColumnsEdit, on);
        Enable(kTrayDrawerColumnsSpin, on);
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
