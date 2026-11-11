// The Dock window's Contents tab (round 5 phase C; it replaces the Dock contents list): the item
// strip (the dock's items as tiles in dock order: drag to reorder, keyboard moves), the drawer on
// the right ("Add to the dock", always shown since round 8 phase 8B: apps and folders (a dialog),
// the built-in kinds and the installed script widgets: drag a row onto the strip or press its Add)
// and the inspector of the selected item under the strip.
//
// The strip, the drawer and the inspector's identity line are three instances of one custom
// control (WinDock.SettingsStrip, Mode Strip / Drawer / Info): GDI, double-buffered, palette
// colours (dark / light, high contrast), DPI aware through the window's Px, no timers. A drag from
// the drawer to the strip uses plain mouse capture: the drawer keeps the capture and asks the strip
// under the pointer (WindowFromPoint) to show its insertion marker; no OLE drag and drop.
//
// Round 12 phase 12D: the control and the drawer's entries moved to SettingsStrip.h (shared with
// the drawer popup of customize mode); the drawer is a grid of tiles with filter chips over it (a
// click on a tile adds it), and a tile dragged out of the window lands on a dock (its marker shows
// where) or, a script widget, on the desktop (SettingsHost::drawerDrag).

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <memory>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Clock.h"
#include "IconUtil.h"
#include "JsonUtil.h"
#include "SettingsForm.h"
#include "SettingsIds.h"
#include "SettingsStrip.h"
#include "WidgetPackage.h"

namespace {

using namespace SettingsIds;
using namespace StripUi;  // round 12 phase 12D: the control and the drawer's catalog (SettingsStrip.h)
namespace json = winrt::Windows::Data::Json;


// --- what an item is called ------------------------------------------------------------------

std::wstring StemOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

const wchar_t* KindLabel(ItemKind k) {
    switch (k) {
        case ItemKind::App: return L"App";
        case ItemKind::Folder: return L"Folder";
        case ItemKind::Separator: return L"Separator";
        case ItemKind::RecycleBin: return L"Recycle Bin";
        case ItemKind::Running: return L"Running apps";
        case ItemKind::Clock: return L"Clock";
        case ItemKind::Weather: return L"Weather";
        case ItemKind::Widget: return L"Widget";
        case ItemKind::Group: return L"Group";
        case ItemKind::Tray: return L"Tray";
        case ItemKind::TrayApp: return L"Tray icon";
        case ItemKind::Notifications: return L"Notification centre";
        case ItemKind::Unknown:
        default: return L"Unknown";
    }
}

// Name and details of an item, as the Dock contents list's columns showed them (pin: its resolved
// twin from host.pins, or null).
void Describe(const Config& c, const DockItemEntry& e, const AppIdentity::ResolvedPin* pin, bool firstRunning,
              std::wstring& name, std::wstring& details) {
    switch (e.Kind()) {
        case ItemKind::App:
        case ItemKind::Folder:
            name = pin ? pin->displayName : std::wstring();
            if (name.empty()) name = !e.path.empty() ? StemOf(e.path) : e.aumid;
            if (pin && pin->missing) name += L" (missing)";
            details = pin && !pin->targetPath.empty() ? pin->targetPath : !e.path.empty() ? e.path : e.aumid;
            break;
        case ItemKind::Separator:
            name = L"Separator";
            details = L"A thin line between groups of icons";
            break;
        case ItemKind::RecycleBin:
            name = pin && !pin->displayName.empty() ? pin->displayName : L"Recycle Bin";
            details = L"Drop files on it to recycle them";
            break;
        case ItemKind::Group: {
            name = e.name.empty() ? std::wstring(L"Group") : e.name;
            details = std::to_wstring(e.members.size()) + (e.members.size() == 1 ? L" shortcut" : L" shortcuts");
            for (size_t i = 0; i < e.members.size() && i < 6; ++i) {
                std::wstring m = pin && i < pin->members.size() ? pin->members[i].displayName : std::wstring();
                if (m.empty()) m = !e.members[i].path.empty() ? StemOf(e.members[i].path) : e.members[i].aumid;
                details += (i == 0 ? L": " : L", ") + m;
            }
            if (e.members.size() > 6) details += L", …";
            break;
        }
        case ItemKind::Running:
            name = L"Running apps";
            details = firstRunning ? L"Open apps that are not pinned" : L"Not shown: only the first running apps item is used";
            break;
        case ItemKind::Clock: {
            const ClockConfig clock = ClockOf(c, e);
            name = clock.label.empty() ? std::wstring(L"Clock") : L"Clock · " + clock.label;
            details = ClockDetail(clock);
            break;
        }
        case ItemKind::Weather: {
            const WeatherConfig w = WeatherOf(c, e);
            const std::wstring place = !w.label.empty() ? w.label : w.location;
            name = place.empty() ? std::wstring(L"Weather") : L"Weather · " + place;
            details = WeatherDetail(w);
            break;
        }
        case ItemKind::Widget: {
            name = Widgets::DisplayName(e.id);
            std::wstring error;
            const auto pkg = Widgets::Load(e.id, error);
            details = !pkg ? L"Not installed: " + e.id
                           : e.id + (pkg->manifest.version.empty() ? std::wstring() : L" " + pkg->manifest.version) +
                                 (pkg->source == Widgets::Package::Source::BuiltIn ? L" (built-in)" : L" (" + pkg->folder + L")");
            break;
        }
        case ItemKind::Tray:
            name = L"Tray";
            details = std::to_wstring(e.rows) + (e.rows == 1 ? L" row, " : L" rows, ") + std::to_wstring(e.columns) + L" columns, " +
                      std::to_wstring(e.trayIconSize) + L" px" +
                      (c.tray.enabled ? std::wstring() : std::wstring(L" (the tray is off: App settings > Tray icons)"));
            if (!e.hidden.empty()) details += L", " + std::to_wstring(e.hidden.size()) + L" hidden";
            break;
        case ItemKind::TrayApp:
            name = L"Tray icon";
            details = e.id;
            break;
        case ItemKind::Notifications:
            name = L"Notification centre";
            details = L"A bell with the unread count; a click lists the notifications";
            break;
        case ItemKind::Unknown:
        default:
            name = e.kind.empty() ? L"(no kind)" : e.kind;
            details = L"Not supported by this version (kept in the file)";
            break;
    }
}



// --- the page --------------------------------------------------------------------------------

// Round 8 phase 8B: the drawer ("Add to the dock") is always shown (no Hide / Show toggle).
// Round 15 phase 15A: the drawer's width follows the window (DrawerW: two fifths of the page,
// between these), and its grid runs down to the view's bottom (DrawerGridBottom). Measured on the
// panel opened wide (about 1500 DIPs): the 340-DIP drawer scrolled two columns of tiles beside 400
// DIPs of empty page under the inspector, and 600 DIPs of empty page under the grid itself.
constexpr int kDrawerMinW = 340;  // DIPs (round 6 phase 6E: 300 -> 340 for the rows' previews)
constexpr int kDrawerMaxW = 640;  // five columns of tiles; wider only adds to the left column
constexpr int kDrawerLinkH = 36;  // the "Manage installed widgets" link under the grid (two lines)
// Round 12 phase 12F: everything 24 DIPs lower than before, under the page's sentence (kPageLead).
constexpr int kLeadDy = 24;
constexpr int kStripTop = 44 + kLeadDy, kStripBottom = 162 + kLeadDy;  // the item strip (118 DIPs tall, as before)
constexpr int kInspectorTop = 176 + kLeadDy;  // round 8 phase 8B: right under the strip (the add buttons are gone)


// Round 12 phase 12D: the strip control's host is the settings window (its DPI, palette, dock
// colour and preview cache), as before the control moved to SettingsStrip.h.
class WindowStripHost final : public StripHost {
public:
    explicit WindowStripHost(SettingsWindow& w) : w_(w) {}
    int Px(int dip) const override { return w_.Px(dip); }
    const DarkMode::Palette& Colors() const override { return w_.Colors(); }
    COLORREF DockColor() const override { return w_.DockColor(); }
    const Image* Preview(const std::wstring& id, SIZE fit, bool render, float zoom) override { return w_.Preview(id, fit, render, zoom); }
    const Image* PreviewEntry(const std::wstring& id, SIZE fit, float zoom) override { return w_.PreviewEntry(id, fit, zoom); }
    void PaintPreview(HDC dc, const RECT& box, const Image& img, COLORREF under) const override { w_.PaintPreview(dc, box, img, under); }

private:
    SettingsWindow& w_;
};

class ContentsPage : public FormPage {
public:
    explicit ContentsPage(SettingsWindow& w)
        : FormPage(w, L"Contents", L'\xE718'),
          host_(w),
          strip_(host_, SettingsStrip::Mode::Strip, L"Items on the dock"),
          drawer_(host_, SettingsStrip::Mode::Drawer, L"Add to the dock"),
          info_(host_, SettingsStrip::Mode::Info, L"Selected item") {}
    ~ContentsPage() override {
        for (auto& entry : icons_) {
            if (entry.second.icon) DestroyIcon(entry.second.icon);
        }
        for (HICON i : {infoIcon_}) {
            if (i) DestroyIcon(i);
        }
    }

protected:
    void Build() override {
        // Round 8 phase 8A: the tab lays itself out (LayoutColumns / FlowInspector): no reflow, no cards.
        NoReflow();
        HelpTopic(L"guide-settings.md#contents-tab");  // round 9 phase 9D: F1 (no cards: no "?" buttons)
        SettingsStrip::Register();
        const int W = Width(), L = kMargin, R = Right(), dx = DrawerDx(), dy = DrawerDy();
        // Round 12 phase 12F: the page's sentence over both columns (the tab lays itself out: no Lead).
        Add(kPageLead, CtlKind::Note, WC_STATICW, L"What is on the dock, in order. Add, move, change and remove items here.",
            SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS, RECT{L, 8, W - 16, 32});
        // Round 8 phase 8B: the heading and its hint on one line, the strip right under them, the
        // inspector right under the strip (no add buttons: the drawer adds everything).
        Add(kPageTitle, CtlKind::Section, WC_STATICW, L"Items on the dock", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE,
            RECT{L, 12 + kLeadDy, L + kTitleW, 36 + kLeadDy});
        Add(kContentsNote, CtlKind::Note, WC_STATICW, L"drag to reorder, drop from the drawer",
            SS_LEFT | SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE | SS_ENDELLIPSIS, RECT{L + kTitleW + 8, 12 + kLeadDy, R, 36 + kLeadDy});
        Tip(kContentsNote, L"Drag to reorder. Drop a widget from the drawer anywhere on the strip. Each copy keeps its own "
                           L"settings. Ctrl+Left / Ctrl+Right move the selected item, Delete removes it.");
        Add(kContentsStrip, CtlKind::Custom, SettingsStrip::kClass, L"Items on the dock", WS_TABSTOP,
            RECT{L, kStripTop, R, kStripBottom});
        Tip(kContentsStrip, L"The dock's items in their order. Click one to select it (its details and buttons are below), drag it "
                            L"to move it, Delete removes it.");
        // The inspector (its frame is painted by PaintBackground; FlowInspector places the buttons).
        Add(kContentsInfo, CtlKind::Custom, SettingsStrip::kClass, L"Selected item", 0,
            RECT{L + 14, kInspectorTop + 10, R - 14 - 212, kInspectorTop + 46});
        Tip(kContentsInfo, L"The selected item: its icon, its name and what it is.");
        y_ = kInspectorTop + 12;
        Button(kPinUp, L"◀ Move left", R - 14 - 206, 100);
        Button(kPinDown, L"Move right ▶", R - 14 - 100, 100);
        y_ = kInspectorTop + 50;
        // Round 10 phase 10A: names its target ("Folder settings…", "Weather settings…"; UpdateInspector).
        Button(kPinSettings, L"Item settings…", L + 14, kSettingsW);
        Tip(kPinSettings, L"Opens every setting of the selected item in a window of its own.");  // per selection: UpdateInspector
        // Round 10 phase 10B: "Customize icon ▾" (was "More ▾"; the item menu's submenu has that name since 10A).
        Button(kPinMore, L"Customize icon ▾", L + 14 + kSettingsW + 6, kMoreW);  // pinned items: Choose icon… / Choose image… / Reset icon
        Tip(kPinMore, L"Choose icon…, Choose image… or Reset icon for the selected pinned item.");
        Button(kPinRemove, L"Remove", R - 14 - kRemoveW, kRemoveW);
        // Round 10 phase 10B: the selected item's common settings, edited in place. A pool of rows
        // (each with every kind of control; UpdateInspector shows the one its setting needs and
        // FlowInspector places them above the placement rows). The tips are set per selection.
        for (int n = 0; n < kInspectorRowMax; ++n) {
            const int b = RowId(n);
            const int cx = L + 14 + kPlaceLabelW + 6;
            y_ = kInspectorTop + 90;
            Add(b + kInspectorLabel, CtlKind::Label, WC_STATICW, L"Setting", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                RECT{L + 14, y_, L + 14 + kPlaceLabelW, y_ + kRowH});
            Check(b + kInspectorCheck, b + kInspectorCaption, L"Setting", L + 14);
            Combo(b + kInspectorCombo, cx, kFieldW, false);
            Spin(b + kInspectorIntEdit, b + kInspectorIntSpin, cx, 96, 0, 100, 1);
            EditBox(b + kInspectorTextEdit, cx, kFieldW, 1024);
            // Round 16 phase 16D: a colour's swatch before its edit (the Windows colour dialog).
            Add(b + kInspectorSwatch, CtlKind::Swatch, WC_BUTTONW, L"Colour", WS_TABSTOP | BS_OWNERDRAW,
                RECT{cx, y_ + 5, cx + 36, y_ + kRowH - 5});
            Text(b + kInspectorUnit, CtlKind::Note, L"", cx + 104, 120);
            Text(b + kInspectorValue, CtlKind::Value, L"", cx, R - 14 - cx);
            for (int part : {kInspectorLabel, kInspectorCheck, kInspectorCaption, kInspectorCombo, kInspectorIntEdit, kInspectorTextEdit,
                             kInspectorValue}) {
                Tip(b + part, L"A setting of the selected item.");
            }
            Tip(b + kInspectorSwatch, L"Click to pick a colour. Its alpha stays; type #AARRGGBB in the box to change it.");
        }
        // Round 9 phase 9A: why they may be greyed.
        Tip(kPinUp, L"Moves the selected item one place left on the dock (Ctrl+Left). Greyed for the first item.");
        Tip(kPinDown, L"Moves the selected item one place right on the dock (Ctrl+Right). Greyed for the last item.");
        Tip(kPinRemove, L"Removes the selected item from the dock (Delete). Select an item in the strip first.");
        // Round 7 phase 7B: the item's place on the dock (FlowInspector places them under the buttons).
        // Round 10 phase 10D: under a heading expander, closed at first (its title counts the
        // selection's anchor / offset when set: "Anchor and offset · 1 changed").
        y_ = kInspectorTop + 90;
        HeadingExpander(kItemPlaceExpander, L"Anchor and offset");
        Tip(kItemPlaceExpander, L"Where the selected item sits on the dock (its own group at the start, centre or end) and "
                                L"a shift along the dock. Closed at first: most items follow the dock's alignment.");
        Add(kItemAnchorLabel, CtlKind::Label, WC_STATICW, L"Position on the dock",
            SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS, RECT{L + 14, y_, L + 14 + kPlaceLabelW, y_ + kRowH});
        Combo(kItemAnchorCombo, L + 14 + kPlaceLabelW + 6, kPlaceComboW, false);
        y_ += kRowH + 2;
        Add(kItemOffsetLabel, CtlKind::Label, WC_STATICW, L"Offset (DIPs)", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS,
            RECT{L + 14, y_, L + 14 + kPlaceLabelW, y_ + kRowH});
        Spin(kItemOffsetEdit, kItemOffsetSpin, L + 14 + kPlaceLabelW + 6, 96, -DockConfig::kMaxItemOffset, DockConfig::kMaxItemOffset, 10);
        Tip(kItemAnchorCombo, L"Where the selected item sits on the dock: its own group (start, centre, end) or the "
                              L"dock's alignment. Select an item in the strip first.");  // round 9 phase 9A
        Tip(kItemOffsetEdit, L"Shifts the selected item and the ones after it in its group. Select an item in the strip "
                             L"first.");
        y_ += kRowH;
        Text(kItemOffsetNote, CtlKind::Note, L"Shifts this item and the ones after it in its group", L + 14, R - L - 28, 20);
        EndExpander();
        // The drawer: everything that can be added (round 8 phase 8B: apps and folders too).
        Add(kDrawerTitle, CtlKind::Section, WC_STATICW, L"Add to the dock", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE,
            RECT{dx + 16, 12 + kLeadDy + dy, dx + 16 + kDrawerTitleW, 36 + kLeadDy + dy});
        Add(kDrawerHint, CtlKind::Note, WC_STATICW, L"drag onto the strip", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE,
            RECT{dx + 16 + kDrawerTitleW, 12 + kLeadDy + dy, W - 16, 36 + kLeadDy + dy});
        y_ = 38 + kLeadDy + dy;
        EditBox(kDrawerSearch, dx + 16, W - 32 - dx, 80);
        Tip(kDrawerSearch, L"Type to filter the tiles below by name or description.");
        // Round 12 phase 12D: the filter chips (push-like radio buttons) and the grid of tiles.
        LayoutChips(true);
        // Round 15 phase 15A: as tall as the view allows (LayoutDrawer places it again once the
        // page window exists: Build knows no height).
        const int gb = DrawerGridBottom();
        Add(kDrawerList, CtlKind::Custom, SettingsStrip::kClass, L"Add to the dock", WS_TABSTOP,
            RECT{dx + 16, kDrawerGridTop + dy, W - 16, gb});
        Tip(kDrawerList, L"Everything you can put on the dock: apps and folders, the built-in items and the widgets, with live "
                         L"previews. Drag a tile onto the strip or the dock, or click it to add it at the end. Drag a widget "
                         L"onto the desktop (or press its small monitor button) to place it there.");
        // Two lines: the text does not fit the drawer's width on one.
        Add(kDrawerLibraryLink, CtlKind::Link, WC_LINK, L"<a>Manage installed widgets in App settings > Widget library</a>", WS_TABSTOP,
            RECT{dx + 16, gb + 8, W - 16, gb + 8 + kDrawerLinkH});
    }

    void Created() override {
        strip_.Attach(Item(kContentsStrip));
        drawer_.Attach(Item(kDrawerList));
        info_.Attach(Item(kContentsInfo));
        drawer_.SetPeer(&strip_);
        strip_.onSelect = [this](int item) {
            sel_ = item;
            UpdateInspector();
        };
        strip_.onActivate = [this](int item) { OpenItemSettings(item); };
        strip_.onMove = [this](int from, int slot) { MoveItem(from, slot); };
        strip_.onDelete = [this](int item) { RemoveItem(item); };
        drawer_.onActivate = [this](int entry) { AddEntry(entry); };
        drawer_.onDropEntry = [this](int entry, int slot) { InsertEntry(entry, slot); };
        drawer_.onDesktop = [this](int entry) { AddToDesktop(entry); };
        // Round 12 phase 12D: a tile dragged out of the window onto a dock (its marker shows where it
        // lands) or, a script widget, onto the desktop (App: SettingsHost::drawerDrag).
        drawer_.onDragOutside = [this](POINT screen, int entry) {
            const std::optional<DockItemEntry> e = DragEntry(entry);
            return Host().drawerDrag && e ? Host().drawerDrag(0, screen, &*e) : 0;
        };
        drawer_.onDragOutsideLeave = [this] {
            if (Host().drawerDrag) Host().drawerDrag(1, POINT{}, nullptr);
        };
        drawer_.onDropOutside = [this](POINT screen, int entry) {
            const std::optional<DockItemEntry> e = DragEntry(entry);
            const bool done = Host().drawerDrag && e && Host().drawerDrag(2, screen, &*e) != 0;
            if (done) {
                FillStrip(sel_);
                FillDrawer();
            }
            return done;
        };
        if (HWND search = Item(kDrawerSearch)) SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search widgets"));
        SetCheck(kDrawerChipAll, true);
        // ItemAnchor order.
        for (const wchar_t* a : {L"Follow the dock's alignment", L"Start (left / top)", L"Centre", L"End (right / bottom)"}) {
            ComboAdd(kItemAnchorCombo, a);
        }
        LayoutColumns();
    }

    void Load() override {
        // Round 10 phase 10B: the schemas again (Reload widgets, a new clock face file).
        schemas_.clear();
        clockSchema_.settings.clear();
        weatherSchema_.settings.clear();
        catalog_.Build();
        FillStrip(sel_);
        FillDrawer();
        // Round 12 phase 12F: a locked dock takes no drags on itself (the strip here still reorders):
        // the heading's hint says so and the way to unlock (the header's Lock, which refreshes the page).
        static constexpr wchar_t kDragTip[] = L"Drag to reorder. Drop a widget from the drawer anywhere on the strip. Each copy keeps "
                                              L"its own settings. Ctrl+Left / Ctrl+Right move the selected item, Delete removes it.";
        const bool locked = Dock().locked;
        SetText(kContentsNote, locked ? L"locked: unlock it (the header) to drag items on the dock" : L"drag to reorder, drop from the drawer");
        SetTip(kContentsNote, locked ? std::wstring(L"This dock is locked. Unlock it (the header's Unlock) to drag items on the dock "
                                                    L"itself; the strip here still reorders them. ") + kDragTip
                                     : std::wstring(kDragTip));
    }

    void OnStyleChanged() override {
        if (Window().Px(44) != iconPx_) {
            // Every icon again at the new size.
            iconPx_ = Window().Px(44);
            for (auto& entry : icons_) {
                if (entry.second.icon) DestroyIcon(entry.second.icon);
            }
            icons_.clear();
            for (HICON* i : {&infoIcon_}) {
                if (*i) DestroyIcon(*i);
                *i = nullptr;
            }
            infoKey_.clear();
            FillStrip(sel_);
            FillDrawer();
        }
        strip_.StyleChanged();
        drawer_.StyleChanged();
        info_.StyleChanged();
        if (Hwnd()) RedrawWindow(Hwnd(), nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
    }

    void PaintBackground(HDC dc) override {
        SettingsPage::PaintBackground(dc);  // the cards (none here: the tab lays itself out)
        const DarkMode::Palette& p = Window().Colors();
        const int s = ScrollPx();
        const COLORREF hair = Mix(p.text, p.layer, 16);
        // The inspector's frame.
        const RECT f{Window().Px(kMargin), Window().Px(kInspectorTop) - s, Window().Px(Right()), Window().Px(inspectorBottom_) - s};
        Frame(dc, f, hair, Window().Px(8) * 2);
        // The drawer's edge (round 12 phase 12H: none while the drawer is under the inspector).
        if (Stacked()) return;
        RECT rc{};
        GetClientRect(Hwnd(), &rc);
        const int x = Window().Px(Width() - DrawerW());
        Fill(dc, RECT{x, 0, x + std::max(1, Window().Px(1)), rc.bottom}, Mix(p.text, p.layer, 12));
    }

    bool Query(int code, LPARAM arg, LRESULT& out) override {
        POINT pt{};
        switch (code) {
            case 229:  // a strip tile's centre (screen px) for item `arg`
                out = strip_.CentreOf(static_cast<int>(arg), pt) ? static_cast<LRESULT>(MAKELONG(static_cast<WORD>(pt.x), static_cast<WORD>(pt.y))) : -1;
                return true;
            case 230:  // a drawer row's centre for entry `arg` (round 8 phase 8B: 1000 App or shortcut…, 1001 Folder…)
                out = drawer_.CentreOf(static_cast<int>(arg), pt) ? static_cast<LRESULT>(MAKELONG(static_cast<WORD>(pt.x), static_cast<WORD>(pt.y)))
                                                                    : -1;
                return true;
            case 231:  // the strip's selected item (-1 none)
                out = strip_.Selected();
                return true;
            case 232:  // the drawer is shown (round 8 phase 8B: always)
                out = 1;
                return true;
            case 278:  // round 8 phase 8B: the drawer's rows in group `arg` (0 apps and folders, 1 built in, 2 script widgets; as filtered)
                out = arg >= 0 && arg < 3 ? groupRows_[static_cast<size_t>(arg)] : -1;
                return true;
            case 233:  // the strip's insertion marker (the slot a drop lands in; -1 none)
                out = strip_.Marker();
                return true;
            case 252:  // round 6 phase 6B: the "Desktop" button's centre of drawer entry `arg` (a script widget)
                out = drawer_.DesktopCentreOf(static_cast<int>(arg), pt)
                          ? static_cast<LRESULT>(MAKELONG(static_cast<WORD>(pt.x), static_cast<WORD>(pt.y)))
                          : -1;
                return true;
            case 264: {  // round 6 phase 6E: drawer entry `arg`'s preview bitmap size (MAKELONG(w, h) px; 0 none / failed)
                // Rendered now when the drawer has not painted it yet (a row scrolled out of view).
                if (!catalog_.EntryOf(static_cast<int>(arg)) || catalog_.EntryOf(static_cast<int>(arg))->kind != ItemKind::Widget) {
                    out = 0;
                    return true;
                }
                const SettingsWindow::PreviewImage* img =
                    Window().Preview(catalog_.EntryOf(static_cast<int>(arg))->id, SettingsStrip::DrawerPreviewFit(host_), true);
                out = img ? static_cast<LRESULT>(MAKELONG(static_cast<WORD>(img->px.cx), static_cast<WORD>(img->px.cy))) : 0;
                return true;
            }
            case 267: {  // round 6 phase 6E: drawer entry `arg`'s preview render time in microseconds (-1: not rendered)
                if (!catalog_.EntryOf(static_cast<int>(arg)) || catalog_.EntryOf(static_cast<int>(arg))->kind != ItemKind::Widget) {
                    out = -1;
                    return true;
                }
                const SettingsWindow::PreviewImage* img =
                    Window().PreviewEntry(catalog_.EntryOf(static_cast<int>(arg))->id, SettingsStrip::DrawerPreviewFit(host_));
                out = img ? static_cast<LRESULT>(img->ms * 1000.0) : -1;
                return true;
            }
            case 268:  // round 6 phase 6E: the inspector shows a widget preview (1 / 0)
                out = infoPreview_ ? 1 : 0;
                return true;
            case 289:  // round 10 phase 10B: the inspector's common-setting rows shown for the selection
                out = static_cast<LRESULT>(rows_.size());
                return true;
            case 290:  // ... the id of row `arg`'s control (its input; a read-only value's static), -1 none
                out = arg >= 0 && static_cast<size_t>(arg) < rows_.size() ? RowId(static_cast<int>(arg)) + PartOf(rows_[static_cast<size_t>(arg)].kind)
                                                                         : -1;
                return true;
            default:
                return false;
        }
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) == kDrawerLibraryLink && (nm.code == NM_CLICK || nm.code == NM_RETURN)) {
            if (Host().openGlobal) Host().openGlobal(kPageWidgets);
        }
        return 0;
    }

    void OnCommand(int id, int code, HWND) override {
        if (id == kDrawerSearch) {
            if (code == EN_CHANGE) {
                filter_ = GetText(kDrawerSearch);
                FillDrawer();
            }
            return;
        }
        if (id >= kDrawerChipAll && id <= kDrawerChipApps) {  // round 12 phase 12D: a filter chip
            if (code == BN_CLICKED) {
                chip_ = static_cast<DrawerFilter>(id - kDrawerChipAll);
                for (int c = kDrawerChipAll; c <= kDrawerChipApps; ++c) SetCheck(c, c == id);
                FillDrawer();
            }
            return;
        }
        if (id == kItemAnchorCombo) {
            if (code == CBN_SELCHANGE) SetAnchor(ComboSel(kItemAnchorCombo));
            return;
        }
        // Round 10 phase 10B: the common-setting rows (and the refresh posted after a change).
        if (id == kInspectorRefreshCmd) {
            refreshPosted_ = false;
            FillStrip(sel_);
            return;
        }
        if (id >= kInspectorRowBase && id < RowId(kInspectorRowMax)) {
            const int part = (id - kInspectorRowBase) % 10;
            if ((part == kInspectorCheck && code == BN_CLICKED) || (part == kInspectorCombo && code == CBN_SELCHANGE)) {
                ApplyRow((id - kInspectorRowBase) / 10, part);
            } else if (part == kInspectorSwatch && code == BN_CLICKED) {
                PickRowColor((id - kInspectorRowBase) / 10);  // round 16 phase 16D
            }
            return;
        }
        if (code != BN_CLICKED) return;
        DockConfig& dock = Dock();
        const int n = static_cast<int>(dock.items.size());
        const bool valid = sel_ >= 0 && sel_ < n;
        switch (id) {
            // Round 8 phase 8B: the add buttons are gone (the drawer's rows post these; so may
            // automation: WM_COMMAND with lParam 0 to the page).
            case kPinAdd: AddFiles(); break;
            case kPinAddFolder: AddFolder(); break;
            case kPinAddSeparator: {
                // After the selected item; nothing selected: at the end.
                size_t at = valid ? static_cast<size_t>(sel_) + 1 : dock.items.size();
                DockItemEntry e;
                e.kind = ToString(ItemKind::Separator);
                dock.items.insert(dock.items.begin() + static_cast<ptrdiff_t>(at), std::move(e));
                at = Regroup(at);
                Changed(SettingsScope::Pins);
                FillStrip(static_cast<int>(at));
                break;
            }
            case kPinAddRecycleBin: AddEntry(DrawerRefs::kRecycleBin); break;
            case kPinUp:
                if (valid && sel_ > 0) MoveItem(sel_, sel_ - 1);
                break;
            case kPinDown:
                if (valid && sel_ < n - 1) MoveItem(sel_, sel_ + 2);
                break;
            case kPinRemove:
                if (valid) RemoveItem(sel_);
                break;
            case kPinSettings:
                if (valid) OpenItemSettings(sel_);
                break;
            case kPinMore:
                if (valid) ShowIconMenu();
                break;
            // Round 8 phase 8B: the More ▾ menu's commands (the old buttons' ids).
            case kPinChooseIcon:
            case kPinChooseImage:
                if (valid && dock.items[static_cast<size_t>(sel_)].IsPin()) ChooseIcon(sel_, id == kPinChooseImage);
                break;
            case kPinResetIcon:
                if (!valid || !dock.items[static_cast<size_t>(sel_)].IsPin() || dock.items[static_cast<size_t>(sel_)].icon.empty()) return;
                dock.items[static_cast<size_t>(sel_)].icon.clear();
                Changed(SettingsScope::Pins);
                FillStrip(sel_);
                break;
            default:
                break;
        }
    }

    // Round 7 phase 7B: the offset edit (Enter / focus loss) and its up-down.
    void Commit(int id) override {
        if (id >= kInspectorRowBase && id < RowId(kInspectorRowMax)) {  // round 10 phase 10B
            const int part = (id - kInspectorRowBase) % 10;
            if (part == kInspectorIntEdit || part == kInspectorTextEdit) ApplyRow((id - kInspectorRowBase) / 10, part);
            return;
        }
        if (id != kItemOffsetEdit) return;
        int value = 0;
        if (ReadInt(kItemOffsetEdit, -DockConfig::kMaxItemOffset, DockConfig::kMaxItemOffset, value)) {
            SetOffset(value);
        } else {
            UpdateInspector();  // not a number: back to the item's value
        }
    }
    void Spun(int spin, int pos) override {
        if (spin == kItemOffsetSpin) SetOffset(pos);
        if (spin >= kInspectorRowBase && spin < RowId(kInspectorRowMax) && (spin - kInspectorRowBase) % 10 == kInspectorIntSpin) {
            ApplyRow((spin - kInspectorRowBase) / 10, kInspectorIntSpin, pos);  // round 10 phase 10B
        }
    }

private:
    static constexpr int kPlaceLabelW = 150, kPlaceComboW = 220;  // the inspector's placement rows (DIPs)
    static constexpr int kTitleW = 128, kDrawerTitleW = 124;       // the headings (DIPs)
    // The inspector's buttons (DIPs; round 10 phase 10A: Settings… 96 -> 200 for "<name> settings…").
    // Round 10 phase 10B: More ▾ 84 -> 140 for "Customize icon ▾".
    static constexpr int kSettingsW = 200, kMoreW = 140, kRemoveW = 96;
    static constexpr int kFieldW = 300;  // round 10 phase 10B: a row's drop-down / edit (DIPs; at most to the inspector's edge)
    // Round 10 phase 10B: posted to the page after an inspector row changed the config (the strip's
    // names and the rows follow once the change is through; not a control).
    static constexpr int kInspectorRefreshCmd = kInspectorRowBase - 1;

    // --- the inspector's common settings (round 10 phase 10B) ---

    // One row: a setting of the selected item and the control it is edited with.
    enum class RowKind { Check, Combo, Int, Text, Value };
    enum class RowSource { Field, GroupName, View, Place };
    struct InspectorRow {
        RowKind kind = RowKind::Text;
        RowSource source = RowSource::Field;
        size_t field = 0;  // RowSource::Field: the index in the schema's settings
        std::wstring label, tip, unit;
        std::vector<std::pair<std::wstring, std::wstring>> choices;  // RowKind::Combo: value, label
        int lo = 0, hi = 100, step = 1;                               // RowKind::Int
        bool color = false;                                           // RowKind::Text of a `colour` field: a swatch before the edit (round 16 phase 16D)
    };
    static int RowId(int n) { return kInspectorRowBase + 10 * n; }
    static int PartOf(RowKind k) {
        switch (k) {
            case RowKind::Check: return kInspectorCheck;
            case RowKind::Combo: return kInspectorCombo;
            case RowKind::Int: return kInspectorIntEdit;
            case RowKind::Text: return kInspectorTextEdit;
            case RowKind::Value:
            default: return kInspectorValue;
        }
    }

    // The settings schema of item `e` (a widget's manifest, a clock's / weather item's built-in
    // one; null: none), read once per id / kind until the next Load.
    const Widgets::Manifest* SchemaOf(const DockItemEntry& e) {
        const ItemKind k = e.Kind();
        if (k == ItemKind::Clock || k == ItemKind::Weather) {
            Widgets::Manifest& m = k == ItemKind::Clock ? clockSchema_ : weatherSchema_;
            if (m.settings.empty()) {
                m.name = k == ItemKind::Clock ? L"Clock" : L"Weather";
                m.settings = Widgets::BuiltInSchema(k);
            }
            return &m;
        }
        if (k != ItemKind::Widget) return nullptr;
        auto it = schemas_.find(e.id);
        if (it == schemas_.end()) {
            std::wstring error;
            it = schemas_.emplace(e.id, Widgets::Load(e.id, error)).first;
        }
        return it->second ? &it->second->manifest : nullptr;
    }

    bool IsFolderLike(size_t item) {
        const DockConfig& d = Dock();
        if (item >= d.items.size()) return false;
        const ItemKind k = d.items[item].Kind();
        if (k == ItemKind::Folder || k == ItemKind::Group) return true;
        const AppIdentity::ResolvedPin* pin = k == ItemKind::App ? PinFor(item) : nullptr;
        return pin && pin->kind == AppIdentity::PinKind::Folder;  // a shortcut to a folder
    }

    // The rows of item `item`: a folder's / group's (name, Contents layout), a clock's / weather
    // item's / widget's common fields (Widgets::CommonFields, those whose visibleIf holds, of the
    // types the inspector edits; a place as a read-only line), none for the other kinds.
    std::vector<InspectorRow> RowsFor(size_t item) {
        std::vector<InspectorRow> rows;
        const DockConfig& d = Dock();
        if (item >= d.items.size()) return rows;
        const DockItemEntry& e = d.items[item];
        if (IsFolderLike(item)) {
            if (e.Kind() == ItemKind::Group) {
                InspectorRow r;
                r.kind = RowKind::Text;
                r.source = RowSource::GroupName;
                r.label = L"Name";
                r.tip = L"The group's name: its label on the dock and the title of its popup. Default: Group.";
                rows.push_back(std::move(r));
            }
            InspectorRow v;
            v.kind = RowKind::Combo;
            v.source = RowSource::View;
            v.label = L"Contents layout";
            v.tip = L"How the popup shows the contents. Automatic: a grid up to 48 entries, a scrolling list above.";
            v.choices = {{L"auto", L"Automatic"}, {L"grid", L"Grid"}, {L"list", L"List"}};
            rows.push_back(std::move(v));
            return rows;
        }
        const Widgets::Manifest* m = SchemaOf(e);
        if (!m) return rows;
        const std::wstring values = ItemSettingsValues(Cfg(), e, m);
        using T = Widgets::SettingField::Type;
        for (size_t k : Widgets::CommonFields(*m)) {
            if (rows.size() >= static_cast<size_t>(kInspectorRowMax)) break;
            const Widgets::SettingField& f = m->settings[k];
            if (!SettingFieldVisible(f, values)) continue;
            InspectorRow r;
            r.field = k;
            r.label = f.label;
            r.unit = f.unit;
            r.tip = !f.description.empty() ? f.description : f.label + L".";
            switch (f.type) {
                case T::Bool: r.kind = RowKind::Check; break;
                case T::Int:
                    r.kind = RowKind::Int;
                    r.lo = static_cast<int>(std::max(-1e9, f.min));
                    r.hi = static_cast<int>(std::min(1e9, f.max));
                    r.step = std::max(1, static_cast<int>(f.step));
                    break;
                case T::Choice:
                    if (f.ChoiceFromPerf()) continue;  // the perf metrics: More settings… lists them
                    r.kind = RowKind::Combo;
                    r.choices = f.choices;
                    break;
                case T::TimeZone:
                    r.kind = RowKind::Combo;
                    if (zones_.empty()) {
                        zones_.push_back({L"", L"Local time"});
                        for (const Clock::TimeZoneInfo& z : Clock::TimeZones()) zones_.push_back({z.key, z.display});
                    }
                    r.choices = zones_;
                    break;
                case T::Place:
                    r.kind = RowKind::Value;
                    r.source = RowSource::Place;
                    r.tip = L"The place this item shows. More settings… finds another one (the place search) or edits its "
                            L"coordinates.";
                    break;
                case T::Color:
                    r.kind = RowKind::Text;
                    r.color = true;
                    break;
                case T::Number:
                case T::String:
                case T::Font:
                default: r.kind = RowKind::Text; break;
            }
            rows.push_back(std::move(r));
        }
        return rows;
    }

    // A place as the inspector's read-only line ("Colorado Springs · 38.83, −104.82").
    static std::wstring PlaceLine(const WeatherConfig& p) {
        if (!p.Configured()) return L"None yet: choose one in More settings…";
        auto coord = [](double v) {
            wchar_t buf[32];
            swprintf_s(buf, L"%.2f", std::fabs(v));
            std::wstring s = buf;
            return (v < 0 && s != L"0.00" ? std::wstring(L"\x2212") : std::wstring()) + s;
        };
        return (!p.location.empty() ? p.location : std::wstring(L"Unnamed place")) + L" · " + coord(p.latitude) + L", " +
               coord(p.longitude);
    }

    // rows_ for the selection -> the pool's controls: each row's control shown (the others hidden),
    // its label, tips, choices and value. Under updatingRows_ (no row applies anything meanwhile).
    void LoadRows() {
        updatingRows_ = true;
        const DockConfig& d = Dock();
        const bool valid = sel_ >= 0 && sel_ < static_cast<int>(d.items.size());
        const DockItemEntry* e = valid ? &d.items[static_cast<size_t>(sel_)] : nullptr;
        const Widgets::Manifest* m = e ? SchemaOf(*e) : nullptr;
        json::JsonObject values{nullptr};
        if (e && m) {
            try {
                if (!json::JsonObject::TryParse(ItemSettingsValues(Cfg(), *e, m), values)) values = nullptr;
            } catch (...) {
                values = nullptr;
            }
        }
        // The rows' shape (kinds, labels, choices) decides whether the controls are set up again;
        // their values are set every time.
        std::wstring shape;
        for (const InspectorRow& r : rows_) {
            shape += std::to_wstring(static_cast<int>(r.kind)) + (r.color ? L"c" : L"") + L":" + r.label + L":" +
                     std::to_wstring(r.choices.size()) + L":" + std::to_wstring(r.lo) + L"," + std::to_wstring(r.hi) + L"|";
        }
        const bool reshape = shape != rowsShape_;
        rowsShape_ = shape;
        for (int n = 0; n < kInspectorRowMax; ++n) {
            const int b = RowId(n);
            const InspectorRow* r = static_cast<size_t>(n) < rows_.size() ? &rows_[static_cast<size_t>(n)] : nullptr;
            const RowKind k = r ? r->kind : RowKind::Value;
            Show(b + kInspectorLabel, r && k != RowKind::Check);
            Show(b + kInspectorCheck, r && k == RowKind::Check);
            Show(b + kInspectorCaption, r && k == RowKind::Check);
            Show(b + kInspectorCombo, r && k == RowKind::Combo);
            Show(b + kInspectorIntEdit, r && k == RowKind::Int);
            Show(b + kInspectorIntSpin, r && k == RowKind::Int);
            Show(b + kInspectorTextEdit, r && k == RowKind::Text);
            Show(b + kInspectorSwatch, r && k == RowKind::Text && r->color);
            Show(b + kInspectorUnit, r && !r->unit.empty() && (k == RowKind::Int || k == RowKind::Text));
            Show(b + kInspectorValue, r && k == RowKind::Value);
            if (!r) continue;
            if (reshape) {
                SetText(b + kInspectorLabel, r->label);
                SetText(b + kInspectorCaption, r->label);
                if (HWND check = Item(b + kInspectorCheck)) SetWindowTextW(check, r->label.c_str());  // its accessible name
                if (HWND cap = Item(b + kInspectorCaption)) InvalidateRect(cap, nullptr, TRUE);
                SetText(b + kInspectorUnit, r->unit);
                for (int part : {kInspectorLabel, kInspectorCheck, kInspectorCaption, kInspectorCombo, kInspectorIntEdit, kInspectorTextEdit,
                                 kInspectorValue}) {
                    SetTip(b + part, r->tip);
                }
                if (k == RowKind::Combo) {
                    HWND combo = Item(b + kInspectorCombo);
                    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
                    for (const auto& c : r->choices) ComboAdd(b + kInspectorCombo, c.second.c_str());
                    // A time zone's display names are wider than the box.
                    SendMessageW(combo, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(Window().Px(r->choices.size() > 20 ? 420 : kFieldW)), 0);
                }
                if (k == RowKind::Int) {
                    if (HWND spin = Item(b + kInspectorIntSpin)) {
                        SendMessageW(spin, UDM_SETRANGE32, static_cast<WPARAM>(r->lo), r->hi);
                        UDACCEL accel[] = {{0, static_cast<UINT>(r->step)}, {2, static_cast<UINT>(r->step * 5)}};
                        SendMessageW(spin, UDM_SETACCEL, std::size(accel), reinterpret_cast<LPARAM>(accel));
                    }
                }
            }
            // The value.
            if (r->source == RowSource::GroupName) {
                SetText(b + kInspectorTextEdit, e && !e->name.empty() ? e->name : std::wstring(L"Group"));
                continue;
            }
            if (r->source == RowSource::View) {
                SetComboSel(b + kInspectorCombo, e ? static_cast<int>(e->view) : -1);
                continue;
            }
            if (r->source == RowSource::Place) {
                if (e) SetText(b + kInspectorValue, PlaceLine(e->Kind() == ItemKind::Weather ? WeatherOf(Cfg(), *e) : ScriptWeatherOf(Cfg(), d, *e)));
                continue;
            }
            if (!m || r->field >= m->settings.size()) continue;
            const Widgets::SettingField& f = m->settings[r->field];
            const json::IJsonValue v = values ? JsonUtil::Find(values, f.key.c_str()) : nullptr;
            using JT = json::JsonValueType;
            switch (k) {
                case RowKind::Check: SetCheck(b + kInspectorCheck, v && v.ValueType() == JT::Boolean && v.GetBoolean()); break;
                case RowKind::Int:
                    SetSpin(b + kInspectorIntSpin, v && v.ValueType() == JT::Number ? static_cast<int>(std::lround(v.GetNumber())) : r->lo);
                    break;
                case RowKind::Combo: {
                    const std::wstring s = v && v.ValueType() == JT::String ? std::wstring(v.GetString()) : std::wstring();
                    int pick = f.type == Widgets::SettingField::Type::TimeZone ? 0 : -1;  // a zone this Windows lacks: local
                    for (size_t c = 0; c < r->choices.size(); ++c) {
                        if (f.type == Widgets::SettingField::Type::TimeZone ? _wcsicmp(r->choices[c].first.c_str(), s.c_str()) == 0
                                                                            : r->choices[c].first == s) {
                            pick = static_cast<int>(c);
                        }
                    }
                    SetComboSel(b + kInspectorCombo, pick);
                    break;
                }
                case RowKind::Text:
                    if (v && v.ValueType() == JT::Number) {
                        SetText(b + kInspectorTextEdit, JsonUtil::Num(v.GetNumber()));
                    } else {
                        SetText(b + kInspectorTextEdit, v && v.ValueType() == JT::String ? std::wstring(v.GetString()) : std::wstring());
                    }
                    if (r->color) {
                        if (HWND sw = Item(b + kInspectorSwatch)) InvalidateRect(sw, nullptr, FALSE);
                    }
                    break;
                default: break;
            }
        }
        updatingRows_ = false;
    }

    // Round 16 phase 16D: the swatch of a colour row: the Windows colour dialog on the colour in
    // its edit (the alpha kept; a transparent "theme colour" default becomes opaque), then the
    // row applies as if the edit had been committed.
    void PickRowColor(int n) {
        if (n < 0 || static_cast<size_t>(n) >= rows_.size() || !rows_[static_cast<size_t>(n)].color) return;
        const int b = RowId(n);
        uint32_t argb = 0;
        if (!ParseArgbColor(GetText(b + kInspectorTextEdit), argb)) argb = 0xFF000000u;
        if (!PickArgbColor(Owner(), argb)) return;
        SetText(b + kInspectorTextEdit, FormatArgbColor(argb));
        if (HWND sw = Item(b + kInspectorSwatch)) InvalidateRect(sw, nullptr, FALSE);
        ApplyRow(n, kInspectorTextEdit);
    }

    bool OnDrawItem(const DRAWITEMSTRUCT& di) override {
        const int id = static_cast<int>(di.CtlID);
        if (id < kInspectorRowBase || id >= RowId(kInspectorRowMax) || (id - kInspectorRowBase) % 10 != kInspectorSwatch) return false;
        uint32_t argb = 0;
        if (!ParseArgbColor(GetText(id - kInspectorSwatch + kInspectorTextEdit), argb)) argb = 0;
        DrawColorSwatch(Window(), di, argb);
        return true;
    }

    // Row `n`'s control `part` changed: its value -> the item (the generated page's write,
    // StoreItemSettings), applied through SettingsScope::Pins like every other edit of the tab. A
    // value that is not valid puts the row back.
    void ApplyRow(int n, int part, int spinPos = 0) {
        if (updatingRows_ || Loading() || n < 0 || static_cast<size_t>(n) >= rows_.size()) return;
        DockConfig& d = Dock();
        if (sel_ < 0 || sel_ >= static_cast<int>(d.items.size())) return;
        const InspectorRow r = rows_[static_cast<size_t>(n)];
        if (PartOf(r.kind) != (part == kInspectorIntSpin ? kInspectorIntEdit : part)) return;  // not this row's control now
        DockItemEntry& e = d.items[static_cast<size_t>(sel_)];
        const int b = RowId(n);
        if (r.source == RowSource::GroupName) {
            if (e.Kind() != ItemKind::Group) return;
            std::wstring name = GetText(b + kInspectorTextEdit);
            while (!name.empty() && std::iswspace(name.back())) name.pop_back();
            while (!name.empty() && std::iswspace(name.front())) name.erase(name.begin());
            if (name.size() > 128) name.resize(128);
            if (name == L"Group") name.clear();  // the default is not written (as the Item window)
            if (name == e.name) return;
            e.name = name;
            ChangedRow();
            return;
        }
        if (r.source == RowSource::View) {
            const int s = ComboSel(b + kInspectorCombo);
            if (s < 0 || s > 2 || static_cast<int>(e.view) == s) return;
            e.view = static_cast<ItemView>(s);
            ChangedRow();
            return;
        }
        if (r.source != RowSource::Field) return;
        const Widgets::Manifest* m = SchemaOf(e);
        if (!m || r.field >= m->settings.size()) return;
        const Widgets::SettingField& f = m->settings[r.field];
        using T = Widgets::SettingField::Type;
        std::wstring value;  // JSON text
        switch (r.kind) {
            case RowKind::Check: value = GetCheck(b + kInspectorCheck) ? L"true" : L"false"; break;
            case RowKind::Int: {
                int v = spinPos;
                if (part != kInspectorIntSpin && !ReadInt(b + kInspectorIntEdit, r.lo, r.hi, v)) {
                    LoadRows();  // not a number: back to the item's value
                    return;
                }
                value = JsonUtil::Num(std::clamp(v, r.lo, r.hi));
                break;
            }
            case RowKind::Combo: {
                const int s = ComboSel(b + kInspectorCombo);
                if (s < 0 || static_cast<size_t>(s) >= r.choices.size()) return;
                value = JsonUtil::Quote(r.choices[static_cast<size_t>(s)].first);
                break;
            }
            case RowKind::Text: {
                const std::wstring text = GetText(b + kInspectorTextEdit);
                if (f.type == T::Number) {
                    wchar_t* end = nullptr;
                    const double v = std::wcstod(text.c_str(), &end);
                    if (end == text.c_str() || !std::isfinite(v)) {
                        LoadRows();
                        return;
                    }
                    value = JsonUtil::Num(std::clamp(v, f.min, f.max));
                } else if (f.type == T::Color) {
                    uint32_t argb = 0;
                    if (!ParseArgbColor(text, argb)) {
                        MessageBeep(MB_ICONWARNING);
                        LoadRows();
                        return;
                    }
                    value = JsonUtil::Quote(FormatArgbColor(argb));
                } else {
                    // A clock's format may not be empty (it would hide the clock), as on its page.
                    bool blank = true;
                    for (wchar_t ch : text) blank = blank && std::iswspace(ch);
                    if (e.Kind() == ItemKind::Clock && f.key == L"format" && blank) {
                        LoadRows();
                        return;
                    }
                    value = JsonUtil::Quote(text);
                }
                break;
            }
            default: return;
        }
        if (!StoreItemSettings(Cfg(), e, {{f.key, value}})) return;
        ChangedRow();
    }

    // An inspector row changed the item: the dock re-reads it, the Item window (if open) follows
    // (App refreshes the other windows); the strip's names and the rows (visibleIf) follow posted.
    void ChangedRow() {
        Changed(SettingsScope::Pins);
        if (!refreshPosted_ && Hwnd()) {
            refreshPosted_ = true;
            PostMessageW(Hwnd(), WM_COMMAND, MAKEWPARAM(kInspectorRefreshCmd, 0), 0);
        }
    }
    // Round 12 phase 12D: the drawer's entries are DrawerCatalog's (SettingsStrip.h, shared with the
    // drawer popup of customize mode); refs as before (DrawerRefs).
    static constexpr int kEntryWeather = DrawerRefs::kWeather, kEntryApp = DrawerRefs::kApp, kEntryFolder = DrawerRefs::kFolder;
    // The grid's top (DIPs): under the search box and the filter chips.
    static constexpr int kDrawerGridTop = 108 + kLeadDy;

    // The filter chips' row under the search box (`create`: made, else moved), push-like radio
    // buttons All / Built in / Widgets / Apps and folders (ids kDrawerChipAll ...).
    void LayoutChips(bool create) {
        const int dx = DrawerDx(), dy = DrawerDy();
        static constexpr struct {
            int id;
            const wchar_t* text;
            int w;
        } kChips[] = {{kDrawerChipAll, L"All", 40}, {kDrawerChipBuiltIn, L"Built in", 66}, {kDrawerChipWidgets, L"Widgets", 66},
                      {kDrawerChipApps, L"Apps and folders", 112}};
        int x = dx + 16;
        for (const auto& c : kChips) {
            const RECT r{x, 76 + kLeadDy + dy, x + c.w, 102 + kLeadDy + dy};
            if (create) {
                Add(c.id, CtlKind::Button, WC_BUTTONW, c.text,
                    WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE | (c.id == kDrawerChipAll ? WS_GROUP : 0), r);
                Tip(c.id, L"Shows only this group of the drawer's tiles (All: every group).");
            } else {
                MoveCtl(c.id, r);
            }
            x += c.w + 4;
        }
    }

    // The item a drag of drawer entry `ref` would add to this window's dock (nothing for a dialog
    // entry or a kind the dock has once already).
    std::optional<DockItemEntry> DragEntry(int ref) {
        const DrawerEntry* e = catalog_.EntryOf(ref);
        if (!e || ref == kEntryApp || ref == kEntryFolder || (e->once && Dock().Has(e->kind))) return std::nullopt;
        return catalog_.NewEntry(ref, Dock());
    }

    // Round 12 phase 12H: the Dock settings panel (12E) is 470 DIPs wide by default, so the left
    // column beside the 340-DIP drawer was about 60 DIPs wide (measured on the panel: the two
    // headings overlapped and the inspector's Move right / Customize icon / Remove buttons went
    // past the page's left edge). Narrower than kStackBelow, the drawer goes under the inspector
    // (full width; the page scrolls to it); wider, it stays on the right as before.
    static constexpr int kStackBelow = 720;
    bool Stacked() const { return Width() < kStackBelow; }
    // Round 15 phase 15A: the drawer's width (DIPs) beside the left column: two fifths of the page
    // between kDrawerMinW and kDrawerMaxW (the grid makes as many columns of tiles as fit).
    int DrawerW() const { return std::clamp(Width() * 2 / 5, kDrawerMinW, kDrawerMaxW); }
    // The drawer's left (its controls start 16 DIPs further in) and how far down it moves.
    int DrawerDx() const { return Stacked() ? kMargin - 16 : Width() - DrawerW(); }
    int DrawerDy() const { return Stacked() ? inspectorBottom_ + 16 - (12 + kLeadDy) : 0; }
    // Round 15 phase 15A: the grid's bottom (DIPs): beside the inspector, down to the view's bottom
    // with the link and the page's bottom margin under it (LayoutPage's content height adds kMargin
    // under the lowest control: this leaves no scroll bar), at least 160 DIPs of grid (a short
    // view scrolls the page). Stacked (under the inspector), 270 DIPs as before: the page scrolls.
    int DrawerGridBottom() const {
        if (Stacked()) return 378 + kLeadDy + DrawerDy();
        return std::max(kDrawerGridTop + 160, Height() - kMargin - 2 - kDrawerLinkH - 8);
    }

    // The left column's right edge (DIPs): beside the drawer (always shown since round 8 phase 8B).
    int Right() const { return Stacked() ? Width() - 16 : Width() - DrawerW() - 20; }

    // The drawer's controls where DrawerDx / DrawerDy put them.
    void LayoutDrawer() {
        const int W = Width(), dx = DrawerDx(), dy = DrawerDy(), gb = DrawerGridBottom();
        MoveCtl(kDrawerTitle, RECT{dx + 16, 12 + kLeadDy + dy, dx + 16 + kDrawerTitleW, 36 + kLeadDy + dy});
        MoveCtl(kDrawerHint, RECT{dx + 16 + kDrawerTitleW, 12 + kLeadDy + dy, W - 16, 36 + kLeadDy + dy});
        MoveCtl(kDrawerSearch, RECT{dx + 16, 38 + kLeadDy + 4 + dy, W - 16, 38 + kLeadDy + kRowH - 4 + dy});
        LayoutChips(false);
        MoveCtl(kDrawerList, RECT{dx + 16, kDrawerGridTop + dy, W - 16, gb});
        MoveCtl(kDrawerLibraryLink, RECT{dx + 16, gb + 8, W - 16, gb + 8 + kDrawerLinkH});
    }

    // --- the drawer ---

    // The More ▾ menu of a pinned item (round 8 phase 8B: the icon buttons of earlier versions).
    void ShowIconMenu() {
        const DockConfig& d = Dock();
        if (sel_ < 0 || sel_ >= static_cast<int>(d.items.size())) return;
        const DockItemEntry& e = d.items[static_cast<size_t>(sel_)];
        if (!e.IsPin() || e.Kind() == ItemKind::Separator) return;
        HWND button = Item(kPinMore);
        HMENU menu = CreatePopupMenu();
        if (!button || !menu) return;
        AppendMenuW(menu, MF_STRING, kPinChooseIcon, L"Choose icon…");
        AppendMenuW(menu, MF_STRING, kPinChooseImage, L"Choose image…");
        AppendMenuW(menu, MF_STRING | (e.icon.empty() ? MF_GRAYED : 0), kPinResetIcon, L"Reset icon");
        RECT r{};
        GetWindowRect(button, &r);
        const int cmd = static_cast<int>(
            TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, Owner(), nullptr));
        DestroyMenu(menu);
        if (cmd > 0) OnCommand(cmd, BN_CLICKED, nullptr);
    }

    // The drawer's grid for the search and the chip (round 12 phase 12D: DrawerCatalog::Tiles).
    void FillDrawer() {
        int select = -1;
        std::vector<StripTile> tiles = catalog_.Tiles(Dock(), filter_, chip_, drawer_.SelectedRef(), Window().Px(44), &select, groupRows_);
        drawer_.SetTiles(std::move(tiles), select);
    }

    // Round 8 phase 8A: the window was resized: the drawer on the right, the left column again.
    void OnResized() override {
        const int W = Width();
        MoveCtl(kPageLead, RECT{kMargin, 8, W - 16, 32});  // round 12 phase 12F
        LayoutDrawer();
        LayoutColumns();  // relayouts
    }

    // The left column (the heading, the strip; the inspector: FlowInspector) beside the drawer.
    void LayoutColumns() {
        const int L = kMargin, R = Right();
        MoveCtl(kPageTitle, RECT{L, 12 + kLeadDy, L + kTitleW, 36 + kLeadDy});
        MoveCtl(kContentsNote, RECT{L + kTitleW + 8, 12 + kLeadDy, R, 36 + kLeadDy});
        MoveCtl(kContentsStrip, RECT{L, kStripTop, R, kStripBottom});
        flowKey_.clear();
        FlowInspector(true);
    }

    // The inspector's buttons: Move left / right at the top right; Settings… and More ▾ (the
    // shown ones) left to right in its second row, Remove at its right end (a third row when they
    // do not fit beside it); with a widget's preview they stack in the right column.
    void FlowInspector(bool force = false) {
        const int L = kMargin, R = Right();
        const int left = L + 14, right = R - 14;
        auto shown = [&](int id) {
            HWND h = Item(id);
            return h && (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0;
        };
        std::wstring key = std::to_wstring(R) + (infoPreview_ ? L"p" : L"");
        for (int id : {kPinSettings, kPinMore}) key += shown(id) ? L"1" : L"0";
        key += L"|" + rowsShape_;  // round 10 phase 10B: the common-setting rows
        key += ExpanderOpen(kItemPlaceExpander) ? L"|o" : L"|c";  // round 10 phase 10D
        for (const InspectorRow& r : rows_) key += r.unit.empty() ? L"" : L"u";
        if (!force && key == flowKey_) return;
        flowKey_ = key;
        // Round 6 phase 6E: a widget's preview makes the identity line taller (the preview above it).
        const int extra = infoPreview_ ? SettingsStrip::kInfoPreviewH : 0;
        MoveCtl(kContentsInfo, RECT{left, kInspectorTop + 10, right - 212, kInspectorTop + 46 + extra});
        MoveCtl(kPinUp, RECT{right - 206, kInspectorTop + 14, right - 106, kInspectorTop + 42});
        MoveCtl(kPinDown, RECT{right - 100, kInspectorTop + 14, right, kInspectorTop + 42});
        const struct {
            int id;
            int w;
        } flow[] = {{kPinSettings, kSettingsW}, {kPinMore, kMoreW}};
        // Round 7 phase 7B: the placement rows (Position on the dock, Offset and its note) under
        // everything else; returns the inspector's bottom.
        auto placeRows = [&](int top) {
            const int cx = left + kPlaceLabelW + 6;
            // Round 10 phase 10B: the common-setting rows first (the same label / control columns).
            const int fieldRight = std::min(right, cx + kFieldW);
            for (int n = 0; n < kInspectorRowMax; ++n) {
                const int b = RowId(n);
                if (static_cast<size_t>(n) >= rows_.size()) continue;  // hidden (LoadRows)
                MoveCtl(b + kInspectorLabel, RECT{left, top, left + kPlaceLabelW, top + kRowH});
                MoveCtl(b + kInspectorCheck, RECT{left, top, left + kCaptionDx, top + kRowH});
                MoveCtl(b + kInspectorCaption, RECT{left + kCaptionDx, top, right, top + kRowH});
                MoveCtl(b + kInspectorCombo, RECT{cx, top + 4, fieldRight, top + 4 + 220});
                MoveCtl(b + kInspectorIntEdit, RECT{cx, top + 4, cx + 96 - 18, top + kRowH - 4});
                MoveCtl(b + kInspectorIntSpin, RECT{cx + 96 - 18, top + 4, cx + 96, top + kRowH - 4});
                const bool intRow = rows_[static_cast<size_t>(n)].kind == RowKind::Int;
                const int editRight = rows_[static_cast<size_t>(n)].unit.empty() ? fieldRight : std::min(fieldRight, right - 128);
                // Round 16 phase 16D: a colour row's swatch, then its edit after it.
                const int editX = rows_[static_cast<size_t>(n)].color ? cx + 44 : cx;
                MoveCtl(b + kInspectorSwatch, RECT{cx, top + 5, cx + 36, top + kRowH - 5});
                MoveCtl(b + kInspectorTextEdit, RECT{editX, top + 4, std::max(editX + 60, editRight), top + kRowH - 4});
                const int unitX = intRow ? cx + 104 : editRight + 8;
                MoveCtl(b + kInspectorUnit, RECT{unitX, top, std::min(right, unitX + 120), top + kRowH});
                MoveCtl(b + kInspectorValue, RECT{cx, top, right, top + kRowH});
                top += kRowH + 2;
            }
            // Round 10 phase 10D: the heading expander, then its rows only while it is open.
            MoveCtl(kItemPlaceExpander, RECT{left, top + 2, right, top + kRowH - 2});
            top += kRowH + 2;
            if (!ExpanderOpen(kItemPlaceExpander)) return top + 8;
            MoveCtl(kItemAnchorLabel, RECT{left, top, left + kPlaceLabelW, top + kRowH});
            MoveCtl(kItemAnchorCombo, RECT{cx, top + 4, std::min(right, cx + kPlaceComboW), top + 4 + 220});
            top += kRowH + 2;
            MoveCtl(kItemOffsetLabel, RECT{left, top, left + kPlaceLabelW, top + kRowH});
            MoveCtl(kItemOffsetEdit, RECT{cx, top + 4, cx + 96 - 18, top + kRowH - 4});
            MoveCtl(kItemOffsetSpin, RECT{cx + 96 - 18, top + 4, cx + 96, top + kRowH - 4});
            top += kRowH;
            // Round 8 phase 8A: as tall as its text at this width (it never clips).
            const int noteH = std::max(20, NoteHeightDip(kItemOffsetNote, right - left));
            MoveCtl(kItemOffsetNote, RECT{left, top, right, top + noteH});
            return top + noteH + 10;
        };
        if (infoPreview_) {
            // A widget with its preview: the buttons stack in the right column under Move left /
            // right, beside the preview (the inspector stays as tall as the preview and the name).
            int yc = kInspectorTop + 50;
            for (const auto& b : flow) {
                if (!shown(b.id)) continue;
                MoveCtl(b.id, RECT{right - 206, yc, right, yc + 28});
                yc += 34;
            }
            MoveCtl(kPinRemove, RECT{right - 206, yc, right, yc + 28});
            inspectorBottom_ = placeRows(std::max(kInspectorTop + 46 + extra, yc + 28) + 8);
            if (Stacked()) LayoutDrawer();  // round 12 phase 12H: under the inspector's new bottom
            Relayout();
            return;
        }
        int x = left, y = kInspectorTop + 52 + extra;
        for (const auto& b : flow) {
            if (!shown(b.id)) continue;
            MoveCtl(b.id, RECT{x, y, x + b.w, y + 28});
            x += b.w + 6;
        }
        if (x > left && x + kRemoveW > right) y += 34;  // no room beside them
        MoveCtl(kPinRemove, RECT{right - kRemoveW, y, right, y + 28});
        inspectorBottom_ = placeRows(y + 28 + 8);
        if (Stacked()) LayoutDrawer();  // round 12 phase 12H: under the inspector's new bottom
        Relayout();
    }

    // --- the strip ---

    const AppIdentity::ResolvedPin* PinFor(size_t item) {
        if (!Host().pins) return nullptr;
        const auto& pins = Host().pins();
        const std::vector<size_t> idx = Dock().PinIndexes();
        for (size_t k = 0; k < idx.size() && k < pins.size(); ++k) {
            if (idx[k] == item) return &pins[k];
        }
        return nullptr;
    }

    static std::wstring IconKey(const DockItemEntry& e, const AppIdentity::ResolvedPin* pin, int px) {
        std::wstring key = e.kind + L"|" + e.path + L"|" + e.aumid + L"|" + e.icon + L"|" + std::to_wstring(px);
        if (pin) key += L"|" + pin->iconFile + L"|" + std::to_wstring(pin->iconIndex) + (pin->missing ? L"|m" : L"");
        return key;
    }

    // The strip's icon of a pin (cached across refreshes; the unused ones are freed by FillStrip).
    HICON StripIcon(const DockItemEntry& e, const AppIdentity::ResolvedPin* pin) {
        const int px = Window().Px(44);
        const std::wstring key = IconKey(e, pin, px);
        auto it = icons_.find(key);
        if (it == icons_.end()) it = icons_.emplace(key, CachedIcon{SettingsItemIcon(pin, e, px), false}).first;
        it->second.used = true;
        return it->second.icon;
    }

    StripTile ItemTile(size_t i, const AppIdentity::ResolvedPin* pin, bool firstRunning, bool strip, std::wstring* details) {
        const DockItemEntry& e = Dock().items[i];
        const ItemKind k = e.Kind();
        std::wstring name, detail;
        Describe(Cfg(), e, pin, firstRunning, name, detail);
        std::wstring kind = KindLabel(k);
        if (k == ItemKind::App && pin && pin->kind == AppIdentity::PinKind::Folder) kind = L"Folder";  // a shortcut to a folder
        StripTile t;
        t.ref = static_cast<int>(i);
        t.name = name;
        t.note = kind;
        t.tint = TintOf(k, e.id);
        t.letter = LetterOf(k, name);
        if (k == ItemKind::Separator) {
            t.shape = StripTile::Shape::Separator;
        } else if (k == ItemKind::Running) {
            t.shape = StripTile::Shape::Running;
        } else if (e.IsPin() && strip) {
            t.icon = StripIcon(e, pin);
            t.shape = t.icon ? StripTile::Shape::Icon : StripTile::Shape::Letter;
        }
        if (details) *details = detail;
        return t;
    }

    // docks[].items (with host.pins, the pins' resolved twins) -> the strip; selects `select`.
    void FillStrip(int select) {
        for (auto& entry : icons_) entry.second.used = false;
        const std::vector<DockItemEntry>& items = Dock().items;
        std::vector<StripTile> tiles;
        bool runningSeen = false;
        for (size_t i = 0; i < items.size(); ++i) {
            const ItemKind k = items[i].Kind();
            const bool firstRunning = k == ItemKind::Running && !runningSeen;
            if (k == ItemKind::Running) runningSeen = true;
            tiles.push_back(ItemTile(i, items[i].IsPin() ? PinFor(i) : nullptr, firstRunning, true, nullptr));
        }
        // Keep the icons still in use (the tiles point at them).
        for (auto it = icons_.begin(); it != icons_.end();) {
            if (!it->second.used) {
                if (it->second.icon) DestroyIcon(it->second.icon);
                it = icons_.erase(it);
            } else {
                ++it;
            }
        }
        const int n = static_cast<int>(tiles.size());
        strip_.SetTiles(std::move(tiles), n == 0 ? -1 : std::clamp(select, 0, n - 1));
        sel_ = strip_.Selected();
        UpdateInspector();
    }

    bool HasSettings(size_t item) {
        const DockConfig& d = Dock();
        if (item >= d.items.size()) return false;
        switch (d.items[item].Kind()) {
            case ItemKind::Widget:
            case ItemKind::Clock:
            case ItemKind::Weather:
            case ItemKind::Folder:
            case ItemKind::Group:
            case ItemKind::Tray:
            case ItemKind::Running: return true;  // round 13 phase 13D: its own Item window
            case ItemKind::App: {
                const AppIdentity::ResolvedPin* pin = PinFor(item);
                return pin && pin->kind == AppIdentity::PinKind::Folder;  // a shortcut to a folder
            }
            default: return false;
        }
    }

    void UpdateInspector() {
        const DockConfig& dock = Dock();
        const int n = static_cast<int>(dock.items.size());
        const bool valid = sel_ >= 0 && sel_ < n;
        std::vector<StripTile> info;
        bool preview = false;
        if (valid) {
            const size_t i = static_cast<size_t>(sel_);
            const DockItemEntry& e = dock.items[i];
            const AppIdentity::ResolvedPin* pin = e.IsPin() ? PinFor(i) : nullptr;
            bool firstRunning = true;
            for (size_t k = 0; k < i; ++k) {
                if (dock.items[k].Kind() == ItemKind::Running) firstRunning = false;
            }
            std::wstring details;
            StripTile t = ItemTile(i, pin, firstRunning, false, &details);
            t.note = t.note + (details.empty() ? std::wstring() : L" · " + details);
            if (e.Kind() == ItemKind::Widget &&
                Window().Preview(e.id, SettingsStrip::InfoPreviewFit(host_), true, SettingsStrip::kInfoZoom)) {
                t.previewId = e.id;  // round 6 phase 6E: its live preview above the name
                preview = true;
            }
            if (e.IsPin() && e.Kind() != ItemKind::Separator) {
                const std::wstring key = IconKey(e, pin, Window().Px(32));
                if (key != infoKey_) {
                    if (infoIcon_) DestroyIcon(infoIcon_);
                    infoIcon_ = SettingsItemIcon(pin, e, Window().Px(32));
                    infoKey_ = key;
                }
                t.icon = infoIcon_;
                t.shape = infoIcon_ ? StripTile::Shape::Icon : StripTile::Shape::Letter;
            }
            info.push_back(std::move(t));
        }
        info_.SetTiles(std::move(info), 0);
        infoPreview_ = preview;
        const DockItemEntry* e = valid ? &dock.items[static_cast<size_t>(sel_)] : nullptr;
        const ItemKind k = e ? e->Kind() : ItemKind::Unknown;
        Enable(kPinUp, valid && sel_ > 0);
        Enable(kPinDown, valid && sel_ < n - 1);
        Enable(kPinRemove, valid);
        Show(kPinSettings, valid && HasSettings(static_cast<size_t>(sel_)));
        // Round 10 phase 10B: the common settings in place (rows_), before the button's text (it
        // depends on them).
        rows_ = valid ? RowsFor(static_cast<size_t>(sel_)) : std::vector<InspectorRow>{};
        LoadRows();
        // Round 10 phase 10A: the button names its target (as the item's dock menu entry does).
        // Round 10 phase 10B: "More settings…" when the inspector shows the item's common settings
        // above it (the tooltip names the window it opens).
        if (e) {
            const std::wstring target = k == ItemKind::Clock     ? std::wstring(L"Clock settings")
                                        : k == ItemKind::Weather ? std::wstring(L"Weather settings")
                                        : k == ItemKind::Tray    ? std::wstring(L"Tray settings")
                                        : k == ItemKind::Group   ? std::wstring(L"Group settings")
                                        : k == ItemKind::Running ? std::wstring(L"Running apps settings")  // round 13 phase 13D
                                        : k == ItemKind::Widget  ? Widgets::DisplayName(e->id) + L" settings"
                                                                 : std::wstring(L"Folder settings");  // a folder (or a shortcut to one)
            SetText(kPinSettings, rows_.empty() ? target + L"…" : std::wstring(L"More settings…"));
            SetTip(kPinSettings, L"Opens " + target + L": every setting of this item in a window of its own (Enter or a double "
                                 L"click on the tile does the same).");
        }
        // Round 8 phase 8B: the icon commands are in the Customize icon ▾ menu (pinned items only).
        Show(kPinMore, e && e->IsPin() && k != ItemKind::Separator);
        // Round 7 phase 7B: the item's anchor and offset (every kind).
        SetComboSel(kItemAnchorCombo, e ? static_cast<int>(e->anchor) : -1);
        SetSpin(kItemOffsetSpin, e ? e->offset : 0);
        for (int id : {kItemAnchorLabel, kItemAnchorCombo, kItemOffsetLabel, kItemOffsetEdit, kItemOffsetSpin}) Enable(id, valid);
        UpdatePlaceTitle();
        FlowInspector();
        // A button that just got disabled must not keep the keyboard focus.
        if (HWND f = GetFocus(); f && IsChild(Hwnd(), f) && !IsWindowEnabled(f)) SetFocus(strip_.Hwnd());
    }

    // --- edits ---

    void MoveItem(int from, int slot) {
        DockConfig& d = Dock();
        const std::wstring name = from >= 0 ? EntryLabel(static_cast<size_t>(from)) : std::wstring();
        if (from < 0 || slot < 0 || !d.MoveItem(static_cast<size_t>(from), static_cast<size_t>(slot))) {
            FillStrip(from);  // nothing moved: the selection stays
            return;
        }
        ChangeLabel(L"Moved " + name);  // round 12 phase 12A: the undo label
        const size_t at = Regroup(static_cast<size_t>(slot > from ? slot - 1 : slot));
        Changed(SettingsScope::Pins);
        FillStrip(static_cast<int>(at));
    }

    // Round 7 phase 7B. Item `at` was just put there (moved, dropped from the drawer, a new
    // separator): on a dock with anchors it joins the group of its new neighbours
    // (DockConfig::JoinNeighbourGroup); then the items are sorted into group order. Returns its
    // index after that.
    size_t Regroup(size_t at) {
        DockConfig& d = Dock();
        d.JoinNeighbourGroup(at);
        d.SortByGroup(&at);
        return at;
    }

    // Round 10 phase 10D: "Anchor and offset · n changed" for the selection.
    void UpdatePlaceTitle() {
        const DockConfig& d = Dock();
        const DockItemEntry* e = sel_ >= 0 && sel_ < static_cast<int>(d.items.size()) ? &d.items[static_cast<size_t>(sel_)] : nullptr;
        const int n = e ? (e->anchor != ItemAnchor::None ? 1 : 0) + (e->offset != 0 ? 1 : 0) : 0;
        SetExpanderText(kItemPlaceExpander, ChangedTitle(L"Anchor and offset", n));
    }

    void OnExpanderToggled(int id) override {
        if (id == kItemPlaceExpander) FlowInspector(true);
    }

    // Round 7 phase 7B: "Position on the dock" (an ItemAnchor) and "Offset" of the selected item.
    void SetAnchor(int anchor) {
        DockConfig& d = Dock();
        if (sel_ < 0 || sel_ >= static_cast<int>(d.items.size()) || anchor < 0 || anchor > static_cast<int>(ItemAnchor::End)) return;
        DockItemEntry& e = d.items[static_cast<size_t>(sel_)];
        if (e.anchor == static_cast<ItemAnchor>(anchor)) return;
        e.anchor = static_cast<ItemAnchor>(anchor);
        size_t at = static_cast<size_t>(sel_);
        d.SortByGroup(&at);  // the strip shows the dock's order
        Changed(SettingsScope::Pins);
        FillStrip(static_cast<int>(at));
    }

    void SetOffset(int offset) {
        DockConfig& d = Dock();
        if (sel_ < 0 || sel_ >= static_cast<int>(d.items.size())) return;
        offset = std::clamp(offset, -DockConfig::kMaxItemOffset, DockConfig::kMaxItemOffset);
        DockItemEntry& e = d.items[static_cast<size_t>(sel_)];
        SetSpin(kItemOffsetSpin, offset);
        if (e.offset == offset) return;
        e.offset = offset;
        Changed(SettingsScope::Pins);
        UpdatePlaceTitle();
    }

    void RemoveItem(int item) {
        DockConfig& d = Dock();
        const int n = static_cast<int>(d.items.size());
        if (item < 0 || item >= n) return;
        ChangeLabel(L"Removed " + EntryLabel(static_cast<size_t>(item)) + L" from " + d.id);  // round 12 phase 12A
        d.items.erase(d.items.begin() + item);
        Changed(SettingsScope::Pins);
        FillStrip(std::min(item, n - 2));
        FillDrawer();  // a single-instance kind may be addable again
    }

    // A new item of drawer entry `entry` (nothing for a single-instance kind the dock has).
    std::optional<DockItemEntry> NewEntry(int entry) {
        bool tray = false;
        std::optional<DockItemEntry> e = catalog_.NewEntry(entry, Dock(), &tray);  // round 12 phase 12D: the shared catalog
        if (e && tray) Cfg().tray.enabled = true;  // as the dock menu's Add widget > Tray
        return e;
    }

    // Add (or Enter on a row): where the dock menu's Add widget / Add separator put it.
    void AddEntry(int entry) {
        if (entry == kEntryApp || entry == kEntryFolder) {
            // Round 8 phase 8B: the Add app… / Add folder… dialogs, posted (they are modal; the
            // drawer may hold the mouse capture here).
            PostMessageW(Hwnd(), WM_COMMAND, MAKEWPARAM(entry == kEntryApp ? kPinAdd : kPinAddFolder, BN_CLICKED), 0);
            return;
        }
        std::optional<DockItemEntry> e = NewEntry(entry);
        if (!e) return;
        DockConfig& d = Dock();
        size_t at = SIZE_MAX;
        const ItemKind k = e->Kind();
        if (k == ItemKind::Widget) {
            at = d.AddScriptWidget(e->id);
        } else if (k == ItemKind::Separator || k == ItemKind::RecycleBin) {
            d.InsertPin(SIZE_MAX, std::move(*e));  // after the last pin
            const std::vector<size_t> idx = d.PinIndexes();
            at = idx.empty() ? SIZE_MAX : idx.back();
        } else {
            at = d.AddWidget(k);  // a clock / weather item gets the defaults (EnsureItemSettings)
        }
        if (at == SIZE_MAX) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        d.SortByGroup(&at);  // round 7 phase 7B: it joins its group (the dock's align)
        ChangeLabel(L"Added " + EntryLabel(at) + L" to " + d.id);  // round 12 phase 12A
        Changed(SettingsScope::Pins);  // also starts the weather service for a weather item
        FillStrip(static_cast<int>(at));
        FillDrawer();
    }

    // A script widget's "Desktop" (round 6 phase 6B): a standalone widget of it on this dock's
    // display (its default size, under the windows), whose own window opens.
    void AddToDesktop(int entry) {
        const DrawerEntry* de = catalog_.EntryOf(entry);
        if (!de || de->kind != ItemKind::Widget) return;
        const std::wstring id = de->id;
        if (!Host().addWidgetWindow || Host().addWidgetWindow(id, DockIndex(), true) == SIZE_MAX) MessageBeep(MB_ICONWARNING);
    }

    // A drawer row dropped on the strip at `slot`.
    void InsertEntry(int entry, int slot) {
        std::optional<DockItemEntry> e = NewEntry(entry);
        if (!e) return;
        DockConfig& d = Dock();
        size_t at = std::min(static_cast<size_t>(std::max(0, slot)), d.items.size());
        d.items.insert(d.items.begin() + static_cast<ptrdiff_t>(at), std::move(*e));
        at = Regroup(at);
        ChangeLabel(L"Added " + EntryLabel(at) + L" to " + d.id);  // round 12 phase 12A
        Changed(SettingsScope::Pins);
        FillStrip(static_cast<int>(at));
        FillDrawer();
        SetFocus(strip_.Hwnd());
    }

    // Enter / a double click on a tile, "Settings…": an item with settings
    // -> its own Item window (round 5 phase B; the tray too since phase D); a pinned tray icon ->
    // the dock's tray item's window (none: Global > Tray icons); the running apps -> their own Item
    // window (round 13 phase 13D: was the Running apps tab); the notification centre -> Global >
    // Notifications (balloons, polling, access, the apps that may badge: the centre has nothing of
    // its own per dock).
    void OpenItemSettings(int item) {
        const DockConfig& d = Dock();
        if (item < 0 || item >= static_cast<int>(d.items.size())) return;
        switch (d.items[static_cast<size_t>(item)].Kind()) {
            case ItemKind::TrayApp: {
                const auto it = std::find_if(d.items.begin(), d.items.end(), [](const DockItemEntry& e) { return e.Kind() == ItemKind::Tray; });
                if (it != d.items.end() && Host().openItem && DockIndex() != SIZE_MAX) {
                    Host().openItem(DockIndex(), static_cast<size_t>(it - d.items.begin()));
                } else if (Host().openGlobal) {
                    Host().openGlobal(kPageTray);
                }
                return;
            }
            case ItemKind::Notifications:
                if (Host().openGlobal) Host().openGlobal(kPageNotifications);
                return;
            default: break;
        }
        if (HasSettings(static_cast<size_t>(item)) && Host().openItem && DockIndex() != SIZE_MAX) {
            Host().openItem(DockIndex(), static_cast<size_t>(item));
        }
    }

    void AddFiles() {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        const COMDLG_FILTERSPEC filters[] = {{L"Apps and shortcuts (*.lnk;*.exe)", L"*.lnk;*.exe"}, {L"All files", L"*.*"}};
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetTitle(L"Pin apps to the dock");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        // NODEREFERENCELINKS: a chosen shortcut is pinned as the shortcut (keeps its arguments).
        dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NODEREFERENCELINKS |
                           FOS_NOCHANGEDIR);
        winrt::com_ptr<IShellItem> programs;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(programs.put())))) {
            dialog->SetDefaultFolder(programs.get());
        }
        if (FAILED(dialog->Show(Owner()))) return;  // cancelled
        winrt::com_ptr<IShellItemArray> items;
        DWORD count = 0;
        if (FAILED(dialog->GetResults(items.put())) || FAILED(items->GetCount(&count))) return;
        std::vector<std::wstring> paths;
        for (DWORD i = 0; i < count; ++i) {
            winrt::com_ptr<IShellItem> item;
            PWSTR raw = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                paths.emplace_back(raw);
                CoTaskMemFree(raw);
            }
        }
        PinPaths(paths);
    }

    void AddFolder() {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        dialog->SetTitle(L"Pin a folder to the dock");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        if (FAILED(dialog->Show(Owner()))) return;  // cancelled
        winrt::com_ptr<IShellItem> item;
        PWSTR raw = nullptr;
        if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return;
        std::vector<std::wstring> paths{std::wstring(raw)};
        CoTaskMemFree(raw);
        PinPaths(paths);
    }

    // Pins the paths after the last pin (skipping apps already pinned) and selects the last new one.
    void PinPaths(const std::vector<std::wstring>& paths) {
        if (paths.empty() || !Host().pinFiles) return;
        const size_t before = Dock().PinCount();
        Host().pinFiles(paths);  // reloads the dock and saves
        Window().ConfigApplied();
        const size_t after = Dock().PinCount();
        const std::vector<size_t> idx = Dock().PinIndexes();
        FillStrip(after > before && !idx.empty() ? static_cast<int>(idx.back()) : sel_);
        Window().UpdateIdentity();
    }

    // Choose icon… / Choose image… for the pin at item `item` (not separators).
    void ChooseIcon(int item, bool image) {
        const std::vector<size_t> idx = Dock().PinIndexes();
        size_t k = SIZE_MAX;
        for (size_t i = 0; i < idx.size(); ++i) {
            if (static_cast<int>(idx[i]) == item) k = i;
        }
        const std::vector<DockItemEntry> pinned = Dock().Pins();
        if (k >= pinned.size()) return;
        const DockItemEntry before = pinned[k];
        if (before.Kind() == ItemKind::Separator) return;
        std::wstring file, startImage;
        int index = 0;
        if (Host().pins) {
            const auto& pins = Host().pins();
            if (k < pins.size()) {
                IconUtil::StartIconFor(pins[k], file, index);
                startImage = IconUtil::StartImageFor(pins[k]);
            }
        }
        std::wstring spec;
        const bool ok = image ? IconUtil::ChooseIconFile(Owner(), startImage, spec) : IconUtil::ChooseIconResource(Owner(), file, index, spec);
        if (!ok) return;
        // The items may have changed while the dialog was open (a drag on the dock): only the
        // same entry at the same place gets the icon.
        DockConfig& now = Dock();
        if (k >= now.PinCount()) return;
        DockItemEntry& e = now.PinAt(k);
        if (e.path != before.path || e.aumid != before.aumid || e.kind != before.kind) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        e.icon = spec;  // the absolute path the dialog returned
        Changed(SettingsScope::Pins);
        FillStrip(static_cast<int>(now.PinIndexes()[k]));
    }

    struct CachedIcon {
        HICON icon = nullptr;
        bool used = false;
    };

    WindowStripHost host_;  // round 12 phase 12D: first (the strips keep a reference to it)
    SettingsStrip strip_;
    SettingsStrip drawer_;
    SettingsStrip info_;
    DrawerCatalog catalog_;  // round 12 phase 12D: the drawer's entries (shared with the drawer popup)
    DrawerFilter chip_ = DrawerFilter::All;  // the drawer's filter chip
    int groupRows_[3] = {0, 0, 0};         // the drawer's rows per group as shown (QueryState 278)
    std::map<std::wstring, CachedIcon> icons_;  // the strip's icons by IconKey
    HICON infoIcon_ = nullptr;                  // the inspector's
    std::wstring infoKey_;
    int iconPx_ = 0;
    int sel_ = 0;  // the selected item
    std::wstring filter_;
    std::wstring flowKey_;
    int inspectorBottom_ = kInspectorTop + 90;
    bool infoPreview_ = false;  // the inspector shows the selected widget's preview (round 6 phase 6E)
    // Round 10 phase 10B: the inspector's common-setting rows of the selection, the shape the pool's
    // controls were set up for, and the schemas read for them (until the next Load).
    std::vector<InspectorRow> rows_;
    std::wstring rowsShape_;
    bool updatingRows_ = false;
    bool refreshPosted_ = false;
    Widgets::Manifest clockSchema_, weatherSchema_;
    std::map<std::wstring, std::shared_ptr<const Widgets::Package>> schemas_;
    std::vector<std::pair<std::wstring, std::wstring>> zones_;  // time zone rows: key, display name
};

}  // namespace

std::unique_ptr<SettingsPage> MakeContentsPage(SettingsWindow& window) { return std::make_unique<ContentsPage>(window); }
