// The Animation and Dock contents pages.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

#include "Clock.h"
#include "IconUtil.h"
#include "SettingsForm.h"
#include "SettingsIds.h"
#include "WidgetPackage.h"

namespace {

using namespace SettingsIds;

// --- Animation -------------------------------------------------------------------------------

// Speed trackbar: 0 = off, 1 - 33 = 0.25x - 4x on a log2 scale (8 steps per doubling, 17 = 1x).
constexpr int kSpeedSteps = 33;
constexpr int kSpeedOne = 17;

double PosToSpeed(int pos) { return pos <= 0 ? 0.0 : std::exp2(static_cast<double>(pos - kSpeedOne) / 8.0); }

int SpeedToPos(double speed) {
    if (!(speed > 0.0)) return 0;
    return std::clamp(static_cast<int>(std::lround(std::log2(speed) * 8.0)) + kSpeedOne, 1, kSpeedSteps);
}

std::wstring FormatSpeed(double speed) {
    if (!(speed > 0.0)) return L"Off";
    wchar_t buf[32];
    swprintf_s(buf, L"%.2f×", speed);
    return buf;
}

bool SystemAnimationsOn() {
    BOOL on = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) return true;
    return on != FALSE;
}

class AnimationPage : public FormPage {
public:
    explicit AnimationPage(SettingsWindow& w) : FormPage(w, L"Animation", L'\xE768') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Animation");
        Section(kSectionSpeed, L"Speed");
        Label(kSpeedLabel, L"Animation speed");
        Trackbar(kSpeedTrack, kColX - 6, 212, 0, kSpeedSteps, 4);
        Text(kSpeedValue, CtlKind::Value, L"", kColX + 216, 72);
        NextRow(kRowH);
        Text(kSpeedNote, CtlKind::Note, L"Off: the dock moves instantly. The speed divides every animation's duration.",
             kColX, right - kColX, 36);
        NextRow(36);

        Section(kSectionHide, L"Hiding and showing");
        Radios({
            {kHideSlide, kHideSlideCaption, L"Slide and fade"},
            {kHideSlideOnly, kHideSlideOnlyCaption, L"Slide"},
            {kHideFade, kHideFadeCaption, L"Fade"},
            {kHideNone, kHideNoneCaption, L"None (instant)"},
        });

        Section(kSectionLaunch, L"Launching an app");
        Radios({
            {kLaunchBounce, kLaunchBounceCaption, L"Bounce until the app opens"},
            {kLaunchHop, kLaunchHopCaption, L"Hop once"},
            {kLaunchPulse, kLaunchPulseCaption, L"Pulse"},
            {kLaunchNone, kLaunchNoneCaption, L"None"},
        });

        Section(kSectionMotion, L"Motion");
        Label(kFalloffLabel, L"Magnification falloff");
        Combo(kFalloffCombo, kColX, 220, false);
        NextRow();
        Label(kEasingLabel, L"Easing");
        Combo(kEasingCombo, kColX, 220, false);
        NextRow();
        Label(kLabelFadeLabel, L"Name label fade");
        IntSpin(kLabelFadeEdit, kLabelFadeSpin, kColX, 90, 0, 1000, 20,
                [](Config& c) -> int& { return c.animation.labelFadeMs; }, SettingsScope::Appearance);
        Text(kLabelFadeUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow();

        Section(kSectionAccessibility, L"Accessibility");
        Label(kReduceMotionLabel, L"Reduce motion");
        Combo(kReduceMotionCombo, kColX, right - kColX, false);
        NextRow(kRowH);
        Text(kReduceMotionNote, CtlKind::Note, L"", kColX, right - kColX, 20);
        NextRow(20);
    }

    void Created() override {
        ComboAdd(kFalloffCombo, L"Smooth (cosine)");  // Falloff order
        ComboAdd(kFalloffCombo, L"Linear");
        ComboAdd(kEasingCombo, L"Ease out");  // Easing order
        ComboAdd(kEasingCombo, L"Linear");
        ComboAdd(kEasingCombo, L"Spring");
        ComboAdd(kReduceMotionCombo, L"Follow Windows (animation effects)");  // ReduceMotion order
        ComboAdd(kReduceMotionCombo, L"On: no slides, bounces or fades");
        ComboAdd(kReduceMotionCombo, L"Off: always animate");
    }

    void Load() override {
        const AnimationConfig& a = Cfg().animation;
        LoadInts();
        SetTrack(kSpeedTrack, SpeedToPos(a.speed));
        SetText(kSpeedValue, FormatSpeed(a.speed));
        SetCheck(kHideSlide, a.hideStyle == HideStyle::Slide);
        SetCheck(kHideSlideOnly, a.hideStyle == HideStyle::SlideOnly);
        SetCheck(kHideFade, a.hideStyle == HideStyle::Fade);
        SetCheck(kHideNone, a.hideStyle == HideStyle::None);
        SetCheck(kLaunchBounce, a.launchStyle == LaunchStyle::Bounce);
        SetCheck(kLaunchHop, a.launchStyle == LaunchStyle::Hop);
        SetCheck(kLaunchPulse, a.launchStyle == LaunchStyle::Pulse);
        SetCheck(kLaunchNone, a.launchStyle == LaunchStyle::None);
        SetComboSel(kFalloffCombo, static_cast<int>(a.magnifyFalloff));
        SetComboSel(kEasingCombo, static_cast<int>(a.easing));
        SetComboSel(kReduceMotionCombo, static_cast<int>(a.reduceMotion));
        UpdateSystemNote();
    }

    void OnSettingChange(WPARAM wp, LPARAM) override {
        if (wp == SPI_SETCLIENTAREAANIMATION || wp == 0) UpdateSystemNote();
    }

    void Track(int id, int pos, bool released) override {
        if (id != kSpeedTrack) return;
        const double speed = PosToSpeed(pos);
        SetText(kSpeedValue, FormatSpeed(speed));
        AnimationConfig& a = Cfg().animation;
        if (released && std::fabs(a.speed - speed) > 1e-9) {
            a.speed = speed;
            Changed(SettingsScope::Appearance);
        }
    }

    void OnCommand(int id, int code, HWND) override {
        AnimationConfig& a = Cfg().animation;
        if (code == CBN_SELCHANGE) {
            const int sel = ComboSel(id);
            if (sel < 0) return;
            if (id == kFalloffCombo) Set(a.magnifyFalloff, static_cast<Falloff>(std::min(sel, 1)));
            if (id == kEasingCombo) Set(a.easing, static_cast<Easing>(std::min(sel, 2)));
            if (id == kReduceMotionCombo) Set(a.reduceMotion, static_cast<ReduceMotion>(std::min(sel, 2)));
            return;
        }
        if (code != BN_CLICKED || !GetCheck(id)) return;
        switch (id) {
            case kHideSlide: Set(a.hideStyle, HideStyle::Slide); break;
            case kHideSlideOnly: Set(a.hideStyle, HideStyle::SlideOnly); break;
            case kHideFade: Set(a.hideStyle, HideStyle::Fade); break;
            case kHideNone: Set(a.hideStyle, HideStyle::None); break;
            case kLaunchBounce: Set(a.launchStyle, LaunchStyle::Bounce); break;
            case kLaunchHop: Set(a.launchStyle, LaunchStyle::Hop); break;
            case kLaunchPulse: Set(a.launchStyle, LaunchStyle::Pulse); break;
            case kLaunchNone: Set(a.launchStyle, LaunchStyle::None); break;
            default: break;
        }
    }

private:
    // DockWindow::ApplyConfig -> RefreshMotion picks every animation setting up.
    template <typename T>
    void Set(T& field, T value) {
        if (field == value) return;
        field = value;
        Changed(SettingsScope::Appearance);
    }

    void UpdateSystemNote() {
        SetText(kReduceMotionNote, std::wstring(L"Windows animation effects: ") + (SystemAnimationsOn() ? L"on" : L"off"));
    }
};

// --- Dock contents ---------------------------------------------------------------------------
// One row per docks[0].items[] entry, in dock order: apps, folders, separators, the Recycle Bin,
// the running apps, the clock, the weather. Every change applies to the dock at once
// (SettingsScope::Pins: DockWindow::ReloadPins) and is saved.

constexpr int kListH = 236;
constexpr int kNameColDip = 180;
constexpr int kKindColDip = 100;
constexpr int kButtonW = 122;
constexpr int kButtonStep = 130;

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
        default: return L"";
    }
}

class DockContentsPage : public FormPage {
public:
    explicit DockContentsPage(SettingsWindow& w) : FormPage(w, L"Dock contents", L'\xE718') {}
    ~DockContentsPage() override {
        if (images_) ImageList_Destroy(images_);  // LVS_SHAREIMAGELISTS: the list never frees it
    }

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Dock contents");
        Add(kPinnedList, CtlKind::List, WC_LISTVIEWW, L"Dock contents",
            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | LVS_SHAREIMAGELISTS,
            RECT{kMargin, y_, right, y_ + kListH});
        NextRow(kListH + 8);
        Button(kPinAdd, L"Add app…", kMargin, kButtonW);
        Button(kPinAddFolder, L"Add folder…", kMargin + kButtonStep, kButtonW);
        Button(kPinAddSeparator, L"Add separator", kMargin + 2 * kButtonStep, kButtonW);
        Button(kPinAddWidget, L"Add widget ▾", kMargin + 3 * kButtonStep, kButtonW);
        NextRow(kRowH + 4);
        Button(kPinUp, L"Move up", kMargin, kButtonW);
        Button(kPinDown, L"Move down", kMargin + kButtonStep, kButtonW);
        Button(kPinRemove, L"Remove", kMargin + 2 * kButtonStep, kButtonW);
        Button(kPinSettings, L"Settings…", kMargin + 3 * kButtonStep, kButtonW);
        NextRow(kRowH + 4);
        Button(kPinChooseIcon, L"Choose icon…", kMargin, kButtonW);
        Button(kPinChooseImage, L"Choose image…", kMargin + kButtonStep, kButtonW);
        Button(kPinResetIcon, L"Reset icon", kMargin + 2 * kButtonStep, kButtonW);
        Button(kPinAddRecycleBin, L"Add Recycle Bin", kMargin + 3 * kButtonStep, kButtonW);
        NextRow(kRowH + 4);
        Check(kLockDock, kLockDockCaption, L"Lock the dock (no dragging, no pinning by dropping files)");
        NextRow();
        Text(kPinnedNote, CtlKind::Note,
             L"Changes apply to the dock at once. On the dock itself you can also drag icons and widgets to "
             L"reorder them, drop shortcuts or folders onto it to pin them, or drag a pinned icon away to unpin it.",
             kMargin, right - kMargin, 36);
        NextRow(36);
    }

    void Created() override {
        HWND list = Item(kPinnedList);
        if (!list) return;
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<wchar_t*>(L"Name");
        col.cx = 100;
        col.iSubItem = 0;
        ListView_InsertColumn(list, 0, &col);
        col.pszText = const_cast<wchar_t*>(L"Kind");
        col.iSubItem = 1;
        ListView_InsertColumn(list, 1, &col);
        col.pszText = const_cast<wchar_t*>(L"Details");
        col.iSubItem = 2;
        ListView_InsertColumn(list, 2, &col);
    }

    void OnStyleChanged() override {
        HWND list = Item(kPinnedList);
        if (!list) return;
        // Column widths and the small icons at the current DPI.
        const Ctl* c = Find(kPinnedList);
        const int scrollBar = GetSystemMetricsForDpi(SM_CXVSCROLL, Window().Dpi());
        const int total = Window().Px(c->dip.right - c->dip.left) - scrollBar - 4;
        const int name = Window().Px(kNameColDip);
        const int kind = Window().Px(kKindColDip);
        ListView_SetColumnWidth(list, 0, name);
        ListView_SetColumnWidth(list, 1, kind);
        ListView_SetColumnWidth(list, 2, std::max(Window().Px(80), total - name - kind));
        const int px = GetSystemMetricsForDpi(SM_CXSMICON, Window().Dpi());
        if (images_ && px == iconPx_) return;
        HIMAGELIST old = images_;
        iconPx_ = px;
        images_ = ImageList_Create(px, px, ILC_COLOR32 | ILC_MASK, 8, 8);
        ListView_SetImageList(list, images_, LVSIL_SMALL);
        if (old) {
            ImageList_Destroy(old);
            FillList(Selected());  // the icons again, at the new size
        }
    }

    void Load() override {
        SetCheck(kLockDock, Cfg().Edit().locked);
        FillList(Selected());
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) == kPinnedList && nm.code == LVN_ITEMCHANGED) UpdateButtons();
        return 0;
    }

    void OnCommand(int id, int code, HWND) override {
        if (code != BN_CLICKED) return;
        DockConfig& dock = Cfg().Edit();
        const int sel = Selected();
        const int n = static_cast<int>(dock.items.size());
        const bool valid = sel >= 0 && sel < n;
        switch (id) {
            case kPinAdd:
                AddFiles();
                break;
            case kPinAddFolder:
                AddFolder();
                break;
            case kPinAddSeparator:
            case kPinAddRecycleBin: {
                if (id == kPinAddRecycleBin && dock.Has(ItemKind::RecycleBin)) return;
                DockItemEntry entry;
                entry.kind = ToString(id == kPinAddSeparator ? ItemKind::Separator : ItemKind::RecycleBin);
                dock.InsertPin(SIZE_MAX, std::move(entry));  // after the last pin
                Changed(SettingsScope::Pins);
                FillList(RowOfPin(dock.PinCount() - 1));
                break;
            }
            case kPinAddWidget:
                AddWidget();
                break;
            case kPinChooseIcon:
            case kPinChooseImage:
                if (valid && Cfg().Edit().items[static_cast<size_t>(sel)].IsPin()) ChooseIcon(sel, id == kPinChooseImage);
                break;
            case kPinResetIcon:
                if (!valid || !dock.items[static_cast<size_t>(sel)].IsPin() || dock.items[static_cast<size_t>(sel)].icon.empty()) {
                    return;
                }
                dock.items[static_cast<size_t>(sel)].icon.clear();
                Changed(SettingsScope::Pins);
                FillList(sel);
                break;
            case kPinRemove: {
                if (!valid) return;
                const bool widget = IsWidgetKind(dock.items[static_cast<size_t>(sel)].Kind());
                dock.items.erase(dock.items.begin() + sel);
                Changed(SettingsScope::Pins);
                AfterChange(widget, std::min(sel, n - 2));
                break;
            }
            case kPinUp:
            case kPinDown: {
                const int to = id == kPinUp ? sel - 1 : sel + 1;
                if (!valid || to < 0 || to >= n) return;
                std::swap(dock.items[static_cast<size_t>(sel)], dock.items[static_cast<size_t>(to)]);
                Changed(SettingsScope::Pins);
                FillList(to);
                break;
            }
            case kPinSettings:
                if (valid) OpenItemSettings(sel);
                break;
            case kLockDock:
                dock.locked = GetCheck(id);  // read by the dock when a drag / drop / menu starts
                Changed(SettingsScope::SaveOnly);
                break;
            default:
                break;
        }
    }

private:
    static bool IsWidgetKind(ItemKind k) {
        return k == ItemKind::Clock || k == ItemKind::Weather || k == ItemKind::Tray || k == ItemKind::Notifications;
    }

    int Selected() const {
        HWND list = Item(kPinnedList);
        return list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    }

    // The row of pin k (DockConfig::PinIndexes), or -1.
    int RowOfPin(size_t k) {
        const std::vector<size_t> idx = Cfg().Edit().PinIndexes();
        return k < idx.size() ? static_cast<int>(idx[k]) : -1;
    }
    // The pin index of row `row` (a pin kind), or SIZE_MAX.
    size_t PinOfRow(int row) {
        const std::vector<size_t> idx = Cfg().Edit().PinIndexes();
        for (size_t k = 0; k < idx.size(); ++k) {
            if (static_cast<int>(idx[k]) == row) return k;
        }
        return SIZE_MAX;
    }

    // A clock / weather item came or went: the General and Weather pages show that too.
    void AfterChange(bool widget, int select) {
        if (widget) {
            Window().Refresh();  // every page, this list included (keeps the selection)
            SelectRow(select);
        } else {
            FillList(select);
        }
    }

    void SelectRow(int row) {
        HWND list = Item(kPinnedList);
        const int n = list ? ListView_GetItemCount(list) : 0;
        if (n <= 0) return;
        const int sel = std::clamp(row, 0, n - 1);
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, sel, FALSE);
        UpdateButtons();
    }

    void UpdateButtons() {
        const int sel = Selected();
        const DockConfig& dock = Cfg().Edit();
        const int n = static_cast<int>(dock.items.size());
        const DockItemEntry* e = sel >= 0 && sel < n ? &dock.items[static_cast<size_t>(sel)] : nullptr;
        const ItemKind k = e ? e->Kind() : ItemKind::Unknown;
        Enable(kPinRemove, e != nullptr);
        Enable(kPinUp, e && sel > 0);
        Enable(kPinDown, e && sel < n - 1);
        Enable(kPinAddRecycleBin, !dock.Has(ItemKind::RecycleBin));
        const bool iconable = e && e->IsPin() && k != ItemKind::Separator;
        Enable(kPinChooseIcon, iconable);
        Enable(kPinChooseImage, iconable);
        Enable(kPinResetIcon, iconable && !e->icon.empty());
        Enable(kPinSettings, e && k != ItemKind::Separator && k != ItemKind::Unknown);
        // A button that just got disabled must not keep the keyboard focus.
        if (HWND f = GetFocus(); f && IsChild(Hwnd(), f) && !IsWindowEnabled(f)) SetFocus(Item(kPinnedList));
    }

    // Settings… of a row: the clock -> General, the weather -> Weather, the running apps ->
    // Behaviour (previews, middle click, wheel), a folder -> Folders and groups (layout, icon),
    // an app / the Recycle Bin -> Choose icon….
    void OpenItemSettings(int row) {
        switch (Cfg().Edit().items[static_cast<size_t>(row)].Kind()) {
            case ItemKind::Clock: Window().ShowPage(kPageGeneral); break;
            case ItemKind::Weather: Window().ShowPage(kPageWeather); break;
            case ItemKind::Running: Window().ShowPage(kPageBehaviour); break;
            case ItemKind::Widget: Window().ShowWidgetSettings(row); break;
            case ItemKind::Tray:
            case ItemKind::TrayApp: Window().ShowPage(kPageTray); break;
            case ItemKind::Notifications: Window().ShowPage(kPageNotifications); break;
            case ItemKind::Folder:
            case ItemKind::Group: Window().ShowItemSettings(row); break;
            case ItemKind::App: {
                // A shortcut to a folder is a folder item too (its popup's layout); an app: Choose icon….
                static const std::vector<AppIdentity::ResolvedPin> kNone;
                const auto& pins = Host().pins ? Host().pins() : kNone;
                const size_t k = PinOfRow(row);
                if (k < pins.size() && pins[k].kind == AppIdentity::PinKind::Folder) {
                    Window().ShowItemSettings(row);
                } else {
                    ChooseIcon(row, false);
                }
                break;
            }
            case ItemKind::RecycleBin: ChooseIcon(row, false); break;
            default: break;
        }
    }

    // "Add widget ▾": a menu under the button (Clock, Weather, Running apps; DockConfig::AddWidget),
    // then the installed script widgets (built-in and folder, Widgets::Catalog), ids 100 + n.
    void AddWidget() {
        HWND button = Item(kPinAddWidget);
        HMENU menu = CreatePopupMenu();
        if (!button || !menu) return;
        constexpr UINT kClock = 1, kWeather = 2, kRunning = 3, kTray = 4, kBell = 5, kScript = 100;
        AppendMenuW(menu, MF_STRING, kClock, L"Clock");
        AppendMenuW(menu, MF_STRING, kWeather, L"Weather");
        AppendMenuW(menu, MF_STRING | (Cfg().Edit().Has(ItemKind::Running) ? MF_GRAYED : 0), kRunning, L"Running apps");
        AppendMenuW(menu, MF_STRING | (Cfg().Edit().Has(ItemKind::Tray) ? MF_GRAYED : 0), kTray, L"Tray (notify icons)");
        AppendMenuW(menu, MF_STRING | (Cfg().Edit().Has(ItemKind::Notifications) ? MF_GRAYED : 0), kBell, L"Notification centre");
        const std::vector<Widgets::CatalogEntry> installed = Widgets::Catalog();
        if (!installed.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < installed.size() && i < 200; ++i) {
            const Widgets::CatalogEntry& w = installed[i];
            const std::wstring text =
                w.name + L"\t" + (w.source == Widgets::Package::Source::Folder ? L"folder" : L"built-in");
            AppendMenuW(menu, MF_STRING, kScript + static_cast<UINT>(i), text.c_str());
        }
        RECT r{};
        GetWindowRect(button, &r);
        const UINT cmd = static_cast<UINT>(
            TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, Owner(), nullptr));
        DestroyMenu(menu);
        if (cmd == 0) return;
        if (cmd >= kScript && cmd - kScript < installed.size()) {
            const size_t at = Cfg().Edit().AddScriptWidget(installed[cmd - kScript].id);
            Changed(SettingsScope::Pins);
            FillList(static_cast<int>(at));
            return;
        }
        const ItemKind kind = cmd == kClock     ? ItemKind::Clock
                              : cmd == kWeather ? ItemKind::Weather
                              : cmd == kTray    ? ItemKind::Tray
                              : cmd == kBell    ? ItemKind::Notifications
                                                : ItemKind::Running;
        if (kind == ItemKind::Tray) Cfg().tray.enabled = true;  // as the dock menu's Add widget > Tray
        const size_t at = Cfg().Edit().AddWidget(kind);
        if (at == SIZE_MAX) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        Changed(SettingsScope::Pins);  // also starts the weather service for a weather item
        AfterChange(IsWidgetKind(kind), static_cast<int>(at));
    }

    int AddIcon(const AppIdentity::ResolvedPin* pin, const DockItemEntry& e) {
        if (!images_) return -1;
        const ItemKind special = e.Kind();
        if (special == ItemKind::Separator) return -1;  // no icon: the name says "Separator"
        if (special == ItemKind::Group && (!pin || pin->iconFile.empty())) return -1;  // the dock composes its icon
        HICON icon = nullptr;
        // The custom icon (or a shortcut's icon location) first: an image, else an icon resource.
        if (pin && !pin->iconFile.empty()) {
            if (IconUtil::IsImageFile(pin->iconFile)) {
                icon = IconUtil::HiconFromImageFile(pin->iconFile, iconPx_);
            } else {
                SHDefExtractIconW(pin->iconFile.c_str(), pin->iconIndex, 0, &icon, nullptr,
                                  MAKELONG(iconPx_, iconPx_));
            }
        }
        if (!icon && special == ItemKind::RecycleBin) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            SHFILEINFOW sfi{};
            if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, &pidl)) && pidl) {
                if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &sfi, sizeof(sfi),
                                   SHGFI_PIDL | SHGFI_ICON | SHGFI_SMALLICON)) {
                    icon = sfi.hIcon;
                }
                CoTaskMemFree(pidl);
            }
            if (!icon) return -1;
        }
        SHFILEINFOW sfi{};
        if (!icon && !e.path.empty()) {
            UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
            if (pin && pin->missing) flags |= SHGFI_USEFILEATTRIBUTES;  // the generic icon of its type
            if (SHGetFileInfoW(e.path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), flags)) icon = sfi.hIcon;
        }
        if (!icon && !e.aumid.empty()) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName((L"shell:AppsFolder\\" + e.aumid).c_str(), nullptr, &pidl, 0, nullptr))) {
                if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &sfi, sizeof(sfi),
                                   SHGFI_PIDL | SHGFI_ICON | SHGFI_SMALLICON)) {
                    icon = sfi.hIcon;
                }
                CoTaskMemFree(pidl);
            }
        }
        if (!icon && SHGetFileInfoW(L".exe", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                                    SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_SMALLICON)) {
            icon = sfi.hIcon;
        }
        if (!icon) return -1;
        const int index = ImageList_AddIcon(images_, icon);  // scaled to the list's size if needed
        DestroyIcon(icon);
        return index;
    }

    // Name and details of a row (pin: its resolved twin from host.pins, or null).
    void Describe(const DockItemEntry& e, const AppIdentity::ResolvedPin* pin, bool firstRunning, std::wstring& name,
                  std::wstring& details) {
        const Config& c = Cfg();
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
                break;
            case ItemKind::RecycleBin:
                name = pin && !pin->displayName.empty() ? pin->displayName : L"Recycle Bin";
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
                details = firstRunning ? L"Open apps that are not pinned"
                                       : L"Not shown: only the first running apps item is used";
                break;
            case ItemKind::Clock:
                name = L"Clock";
                details = Clock::EffectiveFormat(c.clock.format, c.clock.showSeconds) + (c.clock.showDate ? L", date" : L"");
                break;
            case ItemKind::Weather:
                name = L"Weather";
                details = c.weather.Configured() ? (c.weather.location.empty() ? L"Location set" : c.weather.location)
                                                 : L"No location yet";
                break;
            case ItemKind::Widget: {
                name = Widgets::DisplayName(e.id);
                std::wstring error;
                const auto pkg = Widgets::Load(e.id, error);
                details = !pkg ? L"Not installed: " + e.id
                               : e.id + (pkg->source == Widgets::Package::Source::BuiltIn ? L" (built-in)"
                                                                                         : L" (" + pkg->folder + L")");
                break;
            }
            case ItemKind::Tray: {
                name = L"Tray";
                details = std::to_wstring(e.rows) + (e.rows == 1 ? L" row, " : L" rows, ") + std::to_wstring(e.columns) +
                          L" columns, " + std::to_wstring(e.trayIconSize) + L" px" +
                          (c.tray.enabled ? std::wstring() : std::wstring(L" (the tray is off: Settings > Tray)"));
                if (!e.hidden.empty()) details += L", " + std::to_wstring(e.hidden.size()) + L" hidden";
                break;
            }
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

    // docks[0].items[] (with host.pins(), the pins' resolved twins) -> the list; selects `select`
    // (clamped).
    void FillList(int select) {
        HWND list = Item(kPinnedList);
        if (!list) return;
        static const std::vector<AppIdentity::ResolvedPin> kNone;
        const auto& pins = Host().pins ? Host().pins() : kNone;
        const std::vector<DockItemEntry>& items = Cfg().Edit().items;
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        if (images_) ImageList_RemoveAll(images_);
        size_t pinIndex = 0;
        bool runningSeen = false;
        for (size_t i = 0; i < items.size(); ++i) {
            const DockItemEntry& e = items[i];
            const ItemKind kind = e.Kind();
            const AppIdentity::ResolvedPin* pin = nullptr;
            if (e.IsPin()) {
                pin = pinIndex < pins.size() ? &pins[pinIndex] : nullptr;
                ++pinIndex;
            }
            const bool firstRunning = kind == ItemKind::Running && !runningSeen;
            if (kind == ItemKind::Running) runningSeen = true;
            std::wstring name, details;
            Describe(e, pin, firstRunning, name, details);
            std::wstring kindText = KindLabel(kind);
            if (kind == ItemKind::Folder || (kind == ItemKind::App && pin && pin->kind == AppIdentity::PinKind::Folder)) {
                kindText = L"Folder";  // a shortcut to a folder
            }
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_IMAGE;
            item.iItem = static_cast<int>(i);
            item.pszText = name.data();
            item.iImage = e.IsPin() ? AddIcon(pin, e) : -1;
            const int at = ListView_InsertItem(list, &item);
            if (at >= 0) {
                ListView_SetItemText(list, at, 1, kindText.data());
                ListView_SetItemText(list, at, 2, details.data());
            }
        }
        const int n = static_cast<int>(items.size());
        if (n > 0) {
            const int sel = std::clamp(select, 0, n - 1);
            ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list, sel, FALSE);
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        UpdateButtons();
    }

    void AddFiles() {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        const COMDLG_FILTERSPEC filters[] = {{L"Apps and shortcuts (*.lnk;*.exe)", L"*.lnk;*.exe"},
                                             {L"All files", L"*.*"}};
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetTitle(L"Pin apps to the dock");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        // NODEREFERENCELINKS: a chosen shortcut is pinned as the shortcut (keeps its arguments).
        dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM |
                           FOS_NODEREFERENCELINKS | FOS_NOCHANGEDIR);
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
            if (SUCCEEDED(items->GetItemAt(i, item.put())) &&
                SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
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
        if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) {
            return;
        }
        std::vector<std::wstring> paths{std::wstring(raw)};
        CoTaskMemFree(raw);
        PinPaths(paths);
    }

    // Pins the paths after the last pin (skipping apps already pinned) and selects the last new one.
    void PinPaths(const std::vector<std::wstring>& paths) {
        if (paths.empty() || !Host().pinFiles) return;
        const size_t before = Cfg().Edit().PinCount();
        Host().pinFiles(paths);  // reloads the dock and saves
        Window().ConfigApplied();
        const size_t after = Cfg().Edit().PinCount();
        FillList(after > before ? RowOfPin(after - 1) : Selected());
    }

    // Choose icon… / Choose image… for the pin in row `row` (not separators).
    void ChooseIcon(int row, bool image) {
        const size_t k = PinOfRow(row);
        const std::vector<DockItemEntry> pinned = Cfg().Edit().Pins();
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
        const bool ok = image ? IconUtil::ChooseIconFile(Owner(), startImage, spec)
                              : IconUtil::ChooseIconResource(Owner(), file, index, spec);
        if (!ok) return;
        // The list may have changed while the dialog was open (a drag on the dock): only the
        // same entry at the same place gets the icon.
        DockConfig& now = Cfg().Edit();
        if (k >= now.PinCount()) return;
        DockItemEntry& e = now.PinAt(k);
        if (e.path != before.path || e.aumid != before.aumid || e.kind != before.kind) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        e.icon = spec;  // the absolute path the dialog returned
        Changed(SettingsScope::Pins);
        FillList(RowOfPin(k));
    }

    HIMAGELIST images_ = nullptr;
    int iconPx_ = 0;
};

}  // namespace

std::unique_ptr<SettingsPage> MakeAnimationPage(SettingsWindow& window) {
    return std::make_unique<AnimationPage>(window);
}

std::unique_ptr<SettingsPage> MakePinnedPage(SettingsWindow& window) {
    return std::make_unique<DockContentsPage>(window);
}
