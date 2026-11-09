// Round 13 phase 13D: the running item's own Item window (it replaces the Dock window's Running
// apps tab, SettingsIds::kTabRunning, retired). Every value is the item's `settings` object
// (RunningConfig, Config.h): how running apps and their windows appear (stacked or one slot per
// window, the display filter, titles, thumbnails), the badges, and the new Spacing (gap, padding)
// and Order. The rows moved from the tab keep their control ids and wording. The window's dock
// reads them through DockConfig::Running (its first running item; a second one is refused).

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "SettingsForm.h"
#include "SettingsIds.h"

namespace {

using namespace SettingsIds;

constexpr int kNumberW = 90;

class RunningItemPage : public FormPage {
public:
    explicit RunningItemPage(SettingsWindow& w) : FormPage(w, L"Running apps", 0) {}

protected:
    // The window's own item (FormPage::RunningItem: what RunIntSpin, RunningValues and StoreRunning use).
    DockItemEntry* RunningItem() override {
        DockItemEntry* e = Window().TargetItem();
        return e && e->Kind() == ItemKind::Running ? e : nullptr;
    }

    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Running apps");  // a no-op in an Item window (the header names it)
        Lead(L"How running apps and their windows appear on this dock.");
        HelpTopic(L"guide-settings.md#running-apps-widget");
        // The rows of the Running apps tab (round 4 phase 6, round 8 phase 8A hints, round 12 phase
        // 12F reasons), moved as they were.
        Section(kSectionTaskList, L"Running apps", L"guide-settings.md#running-apps-widget");
        Label(kWindowsLabel, L"Windows", L"A slot per app or per window", kTaskListNote);
        Tip(kWindowsLabel, L"One slot per window: a pinned app's icon is its first window and its other windows follow it.");
        Combo(kWindowsCombo, kColX, right - kColX, false);
        NextRow();
        Check(kRunningMonitorOnly, kRunningMonitorOnlyCaption, L"Show only the windows on this dock's display");
        Tip(kRunningMonitorOnlyCaption, L"With several displays, this dock shows only the running apps whose windows are on its own display. Off by default: every window.");
        NextRow();
        Label(kTitlesLabel, L"Titles under icons");
        Tip(kTitlesLabel, L"Titles show the window title, or the app's name.");
        Combo(kTitlesCombo, kColX, right - kColX, false);
        NextRow();
        Ctl& titlesReason = Text(kTitlesReason, CtlKind::Note,
                                 L"Titles on vertical docks appear when Titles under icons is not Off.", kMargin, right - kMargin, 20);
        titlesReason.maxLines = 2;
        NextRow(22);
        Label(kTitlesVerticalLabel, L"Titles on vertical docks");
        Tip(kTitlesVerticalLabel, L"On a left or right dock, titles go beside the icons (the default) or are left out.");
        Combo(kTitlesVerticalCombo, kColX, right - kColX, false);
        NextRow();
        Label(kThumbnailsLabel, L"Window thumbnails", L"Live: GPU · snapshots: cheap", kThumbnailsNote);
        Tip(kThumbnailsLabel,
            L"Live uses the GPU to show windows as they change (only while the dock is visible, at most the frames per "
            L"second below). Snapshots takes a picture every N seconds and costs almost nothing in between. Each window "
            L"gets a tile (stacked: each app its active window).");
        Combo(kThumbnailsCombo, kColX, right - kColX, false);
        NextRow();
        Ctl& thumbReason = Text(kThumbReason, CtlKind::Note, L"", kMargin, right - kMargin, 20);
        thumbReason.maxLines = 2;
        NextRow(22);
        Label(kThumbIntervalLabel, L"Snapshot every");
        Tip(kThumbIntervalLabel, L"With snapshot thumbnails: how often each window's picture is taken again. Default: 5 seconds.");
        RunIntSpin(kThumbIntervalEdit, kThumbIntervalSpin, kColX, kNumberW, 1, 3600, 1,
                   [](RunningConfig& r) -> int& { return r.thumbnailIntervalS; }, SettingsScope::Appearance);
        Text(kThumbIntervalUnit, CtlKind::Note, L"s", kColX + 98, 40);
        NextRow();
        Label(kThumbFpsLabel, L"Live frames per second");
        Tip(kThumbFpsLabel, L"With live thumbnails: at most this many pictures a second per window (more is smoother but uses more GPU). Default: 10.");
        RunIntSpin(kThumbFpsEdit, kThumbFpsSpin, kColX, kNumberW, 1, 60, 1, [](RunningConfig& r) -> int& { return r.thumbnailFps; },
                   SettingsScope::Appearance);
        Text(kThumbFpsUnit, CtlKind::Note, L"at most", kColX + 98, 80);
        NextRow();
        Label(kThumbSizeLabel, L"Tile length");
        Tip(kThumbSizeLabel, L"A thumbnail tile's length along the dock in DIPs (0: twice the icon size, the default).");
        RunIntSpin(kThumbSizeEdit, kThumbSizeSpin, kColX, kNumberW, 0, 512, 8, [](RunningConfig& r) -> int& { return r.thumbnailSize; },
                   SettingsScope::Appearance);
        Text(kThumbSizeUnit, CtlKind::Note, L"DIP (0: twice the icon)", kColX + 98, 200);
        NextRow();

        // New in 13D: the running apps' own spacing (a relayout).
        Section(kRunSpacingSection, L"Spacing", L"guide-settings.md#spacing");
        Ctl& spacingLead = Text(kRunSpacingLead, CtlKind::Note,
                                L"The space between the running apps, and beside them. Pinned apps keep the theme's gap.", kMargin,
                                right - kMargin, 20);
        spacingLead.maxLines = 2;
        NextRow(22);
        Check(kRunGapTheme, kRunGapThemeCaption, L"Use the theme's gap");
        Tip(kRunGapThemeCaption, L"On (the default): the running apps are as far apart as the theme's icon gap. Off: set their own gap below.");
        NextRow();
        Label(kRunGapLabel, L"Gap");
        Tip(kRunGapLabel,
            L"DIPs between two running apps, 0 - 48. Beside a pinned app or a widget the space is the mean of the two gaps.");
        RunIntSpin(kRunGapEdit, kRunGapSpin, kColX, kNumberW, 0, 48, 1, [](RunningConfig& r) -> int& { return r.gap; },
                   SettingsScope::Appearance);
        Text(kRunGapUnit, CtlKind::Note, L"DIP between the running apps", kColX + 98, right - kColX - 98);
        NextRow();
        Label(kRunPaddingLabel, L"Padding");
        Tip(kRunPaddingLabel, L"Extra space before the first and after the last running app, 0 - 64 DIPs. Default: 0.");
        RunIntSpin(kRunPaddingEdit, kRunPaddingSpin, kColX, kNumberW, 0, 64, 1, [](RunningConfig& r) -> int& { return r.padding; },
                   SettingsScope::Appearance);
        Text(kRunPaddingUnit, CtlKind::Note, L"DIP before and after the running apps", kColX + 98, right - kColX - 98);
        NextRow();

        Section(kRunOrderSection, L"Order", L"guide-settings.md#order");
        Label(kRunOrderLabel, L"Order");
        Tip(kRunOrderLabel,
            L"As they appear (the default): in the order the apps started, and as you drag them. By name: alphabetical, "
            L"by each app's name.");
        Combo(kRunOrderCombo, kColX, right - kColX, false);
        NextRow();

        // The badges (from the Behaviour tab; round 4 phase 9).
        Section(kRunBadgesSection, L"Notifications", L"guide-settings.md#notification-badges");
        Check(kBadges, kBadgesCaption, L"Notification badges on app icons");
        Tip(kBadgesCaption,
            L"Badges: the unread count of each app's notifications on its icon (pinned apps too). Which apps may badge, "
            L"tray balloons and the poll interval apply to every dock: App settings > Notifications.");
        NextRow();
    }

    void Created() override {
        ComboAdd(kWindowsCombo, L"Stacked (one slot per app)");  // WindowsMode order
        ComboAdd(kWindowsCombo, L"One slot per window");
        ComboAdd(kTitlesCombo, L"Off");  // TitlesMode order
        ComboAdd(kTitlesCombo, L"One line");
        ComboAdd(kTitlesCombo, L"Two lines");
        ComboAdd(kTitlesVerticalCombo, L"Off");  // TitlesVertical order
        ComboAdd(kTitlesVerticalCombo, L"Beside the icon");
        ComboAdd(kThumbnailsCombo, L"Off");  // ThumbnailMode order
        ComboAdd(kThumbnailsCombo, L"Live (GPU capture)");
        ComboAdd(kThumbnailsCombo, L"Snapshots (every N seconds)");
        ComboAdd(kRunOrderCombo, L"As they appear");  // RunningOrder order
        ComboAdd(kRunOrderCombo, L"By name");
    }

    void Load() override {
        LoadInts();
        const RunningConfig r = RunningValues();
        SetCheck(kRunningMonitorOnly, r.filter == RunningFilter::Monitor);
        SetComboSel(kWindowsCombo, static_cast<int>(r.windows));
        SetComboSel(kTitlesCombo, static_cast<int>(r.titles));
        SetComboSel(kTitlesVerticalCombo, static_cast<int>(r.titlesVertical));
        SetComboSel(kThumbnailsCombo, static_cast<int>(r.thumbnails));
        SetCheck(kRunGapTheme, r.gap < 0);
        if (r.gap < 0) SetSpin(kRunGapSpin, ThemeGap());  // what "off" would start from
        SetComboSel(kRunOrderCombo, static_cast<int>(r.order));
        SetCheck(kBadges, r.badges);
        EnableDependents();
    }

    void OnCommand(int id, int code, HWND) override {
        if (code == CBN_SELCHANGE) {
            const int sel = ComboSel(id);
            if (sel < 0) return;
            RunningConfig r = RunningValues();
            switch (id) {
                case kWindowsCombo:
                    if (sel > 1) return;
                    r.windows = static_cast<WindowsMode>(sel);
                    StoreRunning(r, SettingsScope::Appearance);  // the dock rebuilds its slots
                    return;
                case kTitlesCombo:
                    if (sel > 2) return;
                    r.titles = static_cast<TitlesMode>(sel);
                    if (StoreRunning(r, SettingsScope::Appearance)) EnableDependents();  // relayout: the bar grows by the titles
                    return;
                case kTitlesVerticalCombo:
                    if (sel > 1) return;
                    r.titlesVertical = static_cast<TitlesVertical>(sel);
                    StoreRunning(r, SettingsScope::Appearance);
                    return;
                case kThumbnailsCombo:
                    if (sel > 2) return;
                    r.thumbnails = static_cast<ThumbnailMode>(sel);
                    if (StoreRunning(r, SettingsScope::Appearance)) EnableDependents();  // slots become tiles; capture starts / stops
                    return;
                case kRunOrderCombo:
                    if (sel > 1) return;
                    r.order = static_cast<RunningOrder>(sel);
                    StoreRunning(r, SettingsScope::Pins);  // the slots are rebuilt in the new order
                    return;
                default: return;
            }
        }
        if (code != BN_CLICKED) return;
        RunningConfig r = RunningValues();
        switch (id) {
            case kRunningMonitorOnly:
                r.filter = GetCheck(id) ? RunningFilter::Monitor : RunningFilter::All;
                StoreRunning(r, SettingsScope::Pins);  // the dock rebuilds its items from the window list
                break;
            case kRunGapTheme:
                // Checked: the theme's gap (-1); cleared: the number shown (the theme's gap at first).
                r.gap = GetCheck(id) ? RunningConfig::kThemeGap : std::clamp(GetSpin(kRunGapSpin), 0, 48);
                StoreRunning(r, SettingsScope::Appearance);
                EnableDependents();
                break;
            case kBadges:
                r.badges = GetCheck(id);
                StoreRunning(r, SettingsScope::Notifications);  // the service starts / stops with the demand
                break;
            case kWidgetReset: {  // the footer's Reset to factory defaults… (confirmed by the window)
                if (!RunningItem()) return;
                if (StoreRunning(RunningConfig{}, SettingsScope::Pins)) Window().Refresh();
                break;
            }
            default: break;
        }
    }

    void EnableDependents() {
        const RunningConfig r = RunningValues();
        Collapse(kTitlesReason, r.titles != TitlesMode::Off);
        Collapse({kTitlesVerticalLabel, kTitlesVerticalCombo}, r.titles == TitlesMode::Off);
        SetText(kThumbReason, r.thumbnails == ThumbnailMode::Off         ? L"Off: no tiles. Choose Live or Snapshots to set their size and rate."
                              : r.thumbnails == ThumbnailMode::Snapshots ? L"Frames per second applies to live thumbnails. Choose Live to set it."
                                                                         : L"Snapshot every applies to snapshots. Choose Snapshots to set it.");
        Collapse({kThumbIntervalLabel, kThumbIntervalEdit, kThumbIntervalUnit}, r.thumbnails != ThumbnailMode::Snapshots);
        Collapse({kThumbFpsLabel, kThumbFpsEdit, kThumbFpsUnit}, r.thumbnails != ThumbnailMode::Live);
        Collapse({kThumbSizeLabel, kThumbSizeEdit, kThumbSizeUnit}, r.thumbnails == ThumbnailMode::Off);
        // The gap row while the theme's gap is not used (collapsed, not greyed).
        Collapse({kRunGapLabel, kRunGapEdit, kRunGapUnit}, r.gap < 0);
    }

private:
    // The theme's icon gap of the window's dock in whole DIPs (the gap row's start value).
    int ThemeGap() {
        const DockConfig& d = Dock();
        const ThemeConfig& t = d.ownTheme ? d.theme : Cfg().theme;
        return std::clamp(static_cast<int>(std::lround(t.iconGap)), 0, 48);
    }
};

}  // namespace

std::unique_ptr<SettingsPage> MakeRunningItemPage(SettingsWindow& window) { return std::make_unique<RunningItemPage>(window); }
