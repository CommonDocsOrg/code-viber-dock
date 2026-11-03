// The settings pages: control tables (DIP layout) and handlers. This file: Global > General and
// About, a Dock window's Position / Behaviour / Running apps tabs, and the page lists of the three
// window kinds; Themes and Dock > Theme: SettingsPagesAppearance.cpp; Animation and Dock > Contents:
// SettingsPagesMore.cpp; a widget / clock / weather item: the generated page
// (SettingsPagesWidgets.cpp). The frame, fonts, colours, theming, layout and scrolling live in
// SettingsWindow.cpp, the header / footer bands in SettingsBands.cpp.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

#include "ContextMenu.h"
#include "SettingsForm.h"
#include "SettingsIds.h"
#include "SettingsWindow.h"

namespace {

using namespace SettingsIds;

// --- General ---------------------------------------------------------------------------------

constexpr int kMaxTrackIconSize = 128;  // the spinner goes on to 256 (Config::Sanitize)
std::wstring FormatMagnification(int pos) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.2f×", pos / 20.0);
    return buf;
}

// Display names in the dock's monitor order (DockWindow::PickMonitor): the primary, then the
// others in enumeration order.
std::vector<std::wstring> MonitorNames() {
    struct Mon {
        HMONITOR handle;
        std::wstring device;
    };
    std::vector<Mon> all;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(m, &mi);
            reinterpret_cast<std::vector<Mon>*>(lp)->push_back(Mon{m, mi.szDevice});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&all));
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    std::vector<std::wstring> names{L"Primary"};
    int n = 2;
    for (const Mon& m : all) {
        if (m.handle == primary) continue;
        names.push_back(L"Display " + std::to_wstring(n++) + L" (" + m.device + L")");
    }
    return names;
}

// Global > General: what applies to the whole app (startup, the taskbar, the renderer, the file).
class GeneralPage : public FormPage {
public:
    explicit GeneralPage(SettingsWindow& w) : FormPage(w, L"General", L'\xE713') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"General");
        Section(kSectionStartup, L"System");
        Check(kStartWithWindows, kStartWithWindowsCaption, L"Start with Windows");
        NextRow();
        Check(kHideTaskbar, kHideTaskbarCaption, L"Hide the Windows taskbar");
        NextRow();
        Label(kRendererLabel, L"Renderer");
        Combo(kRendererCombo, kColX, right - kColX, false);
        NextRow(kRowH);
        Text(kRendererNote, CtlKind::Note, L"", kColX, right - kColX, 20);
        NextRow(20);
        Section(kSectionConfigFile, L"Configuration file");
        Button(kGeneralOpenConfig, L"Open config file", kMargin, 150);
        Button(kGeneralReloadConfig, L"Reload config", kMargin + 162, 150);
        NextRow();
        Text(kPositionNote, CtlKind::Note,
             L"Each dock's own settings (its items, position, behaviour, running apps and theme) are in its own window: "
             L"right-click the dock and choose Dock settings…, or open it from Docks and displays.",
             kMargin, right - kMargin, 40);
        NextRow(40);
    }

    void Created() override {
        ComboAdd(kRendererCombo, L"Automatic (GPU, software if that fails)");
        ComboAdd(kRendererCombo, L"Software (WARP, less memory)");
    }

    void Load() override {
        const Config& c = Cfg();
        SetCheck(kStartWithWindows, Host().autostartEnabled ? Host().autostartEnabled() : c.startWithWindows);
        SetCheck(kHideTaskbar, Host().taskbarHidden ? Host().taskbarHidden() : c.hideTaskbar);
        SetComboSel(kRendererCombo, c.renderer == L"software" ? 1 : 0);
        const bool warp = Host().usingWarp && Host().usingWarp();
        SetText(kRendererNote, std::wstring(L"Takes effect after WinDock restarts. Currently: ") +
                                   (warp ? L"software (WARP)" : L"GPU"));
    }

    void OnCommand(int id, int code, HWND) override {
        Config& c = Cfg();
        switch (id) {
            case kStartWithWindows:
                if (code == BN_CLICKED) {
                    c.startWithWindows = GetCheck(id);
                    Changed(SettingsScope::Autostart);
                }
                break;
            case kHideTaskbar:
                if (code == BN_CLICKED) {
                    c.hideTaskbar = GetCheck(id);
                    Changed(SettingsScope::Taskbar);
                }
                break;
            case kRendererCombo:
                if (code == CBN_SELCHANGE) {
                    const std::wstring r = ComboSel(id) == 1 ? L"software" : L"auto";
                    if (r != c.renderer) {
                        c.renderer = r;
                        Changed(SettingsScope::SaveOnly);
                    }
                }
                break;
            case kGeneralOpenConfig:
                if (code == BN_CLICKED && Host().command) Host().command(ContextMenu::kOpenConfig);
                break;
            case kGeneralReloadConfig:
                if (code == BN_CLICKED && Host().command) Host().command(ContextMenu::kReloadConfig);  // refreshes every window
                break;
            default:
                break;
        }
    }
};

// --- a dock's Position, Behaviour and Running apps tabs ------------------------------------------
// One class, three tables: the handlers below serve every control id of the three (a control the
// part does not build is simply never there). Everything edits the window's dock (Dock()).

class DockFieldsPage : public FormPage {
public:
    enum class Part { Position, Behaviour, Running };
    DockFieldsPage(SettingsWindow& w, Part part)
        : FormPage(w, part == Part::Position ? L"Position" : part == Part::Behaviour ? L"Behaviour" : L"Running apps", 0),
          part_(part) {}

protected:
    void Build() override {
        switch (part_) {
            case Part::Position: BuildPosition(); break;
            case Part::Behaviour: BuildBehaviour(); break;
            case Part::Running: BuildRunning(); break;
        }
    }

    void BuildPosition() {
        const int right = Width() - kMargin;
        Title(L"Position");
        Section(kSectionDisplay, L"Display");
        Label(kMonitorLabel, L"Display");
        Combo(kMonitorCombo, kColX, right - kColX, false);
        NextRow();
        Label(kLayerLabel, L"Layer");
        Combo(kLayerCombo, kColX, right - kColX, false);
        NextRow();

        Section(kSectionPosition, L"Position");
        Label(kEdgeLabel, L"Screen edge");
        Combo(kEdgeCombo, kColX, 160, false);
        NextRow();
        Label(kAlignLabel, L"Alignment");
        Combo(kAlignCombo, kColX, 160, false);
        NextRow();
        Label(kOffsetLabel, L"Offset (pixels)");
        IntSpin(kOffsetEdit, kOffsetSpin, kColX, 88, -16000, 16000, 10, [](Config&, DockConfig& d) -> int& { return d.offset; },
                SettingsScope::Appearance);
        NextRow(kRowH);
        Text(kPositionNote, CtlKind::Note,
             L"Moves the dock along its edge from the aligned place (right / down). Ctrl+drag the dock to move it "
             L"along its edge or onto another edge (unless it is locked).",
             kColX, right - kColX, 40);
        NextRow(42);
        Check(kFullWidth, kFullWidthCaption, L"Full width (the bar spans the whole edge)");
        NextRow();
        Check(kReserveSpace, kReserveSpaceCaption, L"Reserve screen space (maximized windows stop at the dock)");
        NextRow();
        Label(kPeekLabel, L"Peek when hidden (pixels)");
        IntSpin(kPeekEdit, kPeekSpin, kColX, 72, 0, 64, 2, [](Config&, DockConfig& d) -> int& { return d.peekPx; },
                SettingsScope::Appearance);
        NextRow();
        Label(kPeekOpacityLabel, L"Peek opacity (%)");
        Spin(kPeekOpacityEdit, kPeekOpacitySpin, kColX, 72, 5, 100, 5);
        NextRow();
        Label(kTitleLabel, L"Title");
        EditBox(kTitleEdit, kColX, right - kColX, 128);
        NextRow(kRowH);
        Text(kPositionNote2, CtlKind::Note,
             L"Reserve screen space keeps maximized windows off the dock; reserves a strip along the whole edge "
             L"(auto-hide off only). Peek keeps that much of the hidden dock on screen (0: off). The title is drawn "
             L"beside the bar, toward the middle of the screen.",
             kColX, right - kColX, 58);
        NextRow(60);

        Section(kSectionShape, L"Shape");
        Label(kShapeLabel, L"Shape");
        Combo(kShapeCombo, kColX, 200, false);
        NextRow();
        Label(kRadiusLabel, L"Radius (pixels, 0: automatic)");
        IntSpin(kRadiusEdit, kRadiusSpin, kColX, 88, 0, 2000, 10, [](Config&, DockConfig& d) -> int& { return d.radius; },
                SettingsScope::Appearance);
        NextRow();
        Check(kRotateIcons, kRotateIconsCaption, L"Turn the icons with the ring");
        NextRow();
        Label(kCircleXLabel, L"Circle centre x (DIPs)");
        EditBox(kCircleXEdit, kColX, 88, 7);
        NextRow();
        Label(kCircleYLabel, L"Circle centre y (DIPs)");
        EditBox(kCircleYEdit, kColX, 88, 7);
        NextRow(kRowH);
        Text(kCircleNote, CtlKind::Note,
             L"A circle's centre from its display's work area's top-left; empty: the middle. Ctrl+drag the circle "
             L"to move it.",
             kColX, right - kColX, 40);
        NextRow(42);
        Label(kOverflowLabel, L"When the icons do not fit");
        Combo(kOverflowCombo, kColX, right - kColX, false);
        NextRow();
    }

    void BuildBehaviour() {
        const int right = Width() - kMargin;
        Title(L"Behaviour");
        Section(kSectionDock, L"Size");
        Label(kIconSizeLabel, L"Icon size");
        Trackbar(kIconSizeTrack, kColX - 6, 212, 16, kMaxTrackIconSize, 8);
        IntSpin(kIconSizeEdit, kIconSizeSpin, kColX + 216, 72, 16, 256, 4,
                [](Config&, DockConfig& d) -> int& { return d.iconSize; }, SettingsScope::Appearance);
        NextRow();
        Label(kMagnifyLabel, L"Magnification");
        Trackbar(kMagnifyTrack, kColX - 6, 212, 20, 50, 2);
        Text(kMagnifyValue, CtlKind::Value, L"", kColX + 216, 72);
        NextRow();
        Label(kRangeLabel, L"Magnification range (icons)");
        IntSpin(kRangeEdit, kRangeSpin, kColX, 72, 1, 4, 1, [](Config&, DockConfig& d) -> int& { return d.magnifyRange; },
                SettingsScope::Appearance);
        NextRow();

        Section(kSectionAutoHide, L"Auto-hide");
        Radios({
            {kAutoHideOff, kAutoHideOffCaption, L"Off: the dock is always shown"},
            {kAutoHideAlways, kAutoHideAlwaysCaption, L"Always: hide when the mouse leaves the dock"},
            {kAutoHideIntelligent, kAutoHideIntelligentCaption, L"When a window overlaps the dock"},
        });
        Label(kHideDelayLabel, L"Hide delay");
        IntSpin(kHideDelayEdit, kHideDelaySpin, kColX, 90, 0, 10000, 50,
                [](Config&, DockConfig& d) -> int& { return d.hideDelayMs; }, SettingsScope::SaveOnly);
        Text(kHideDelayUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow();
        Label(kRevealDelayLabel, L"Reveal delay");
        IntSpin(kRevealDelayEdit, kRevealDelaySpin, kColX, 90, 0, 10000, 50,
                [](Config&, DockConfig& d) -> int& { return d.revealDelayMs; }, SettingsScope::SaveOnly);
        Text(kRevealDelayUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow(kRowH);
        Text(kAutoHideNote, CtlKind::Note, L"The dock stays shown while this window (or one of its item windows) is active.",
             kMargin, right - kMargin, 20);
        NextRow(20);

        Section(kSectionPreviews, L"Window previews");
        Check(kPreviews, kPreviewsCaption, L"Show window previews for apps with several windows");
        NextRow();
        Check(kPreviewSingle, kPreviewSingleCaption, L"Show window previews for apps with a single window",
              kMargin + kCaptionDx);
        NextRow();
        Label(kPreviewDelayLabel, L"Preview delay");
        IntSpin(kPreviewDelayEdit, kPreviewDelaySpin, kColX, 90, 0, 10000, 50,
                [](Config&, DockConfig& d) -> int& { return d.previewDelayMs; }, SettingsScope::SaveOnly);
        Text(kPreviewDelayUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow();

        Section(kSectionMouse, L"Mouse");
        Label(kMiddleClickLabel, L"Middle click on an app");
        Combo(kMiddleClickCombo, kColX, right - kColX, false);
        NextRow();
        Label(kWheelLabel, L"Mouse wheel over an app");
        Combo(kWheelCombo, kColX, right - kColX, false);
        NextRow();
    }

    void BuildRunning() {
        const int right = Width() - kMargin;
        Title(L"Running apps");
        Section(kSectionTaskList, L"Running apps");
        Label(kWindowsLabel, L"Windows");
        Combo(kWindowsCombo, kColX, right - kColX, false);
        NextRow();
        Check(kRunningMonitorOnly, kRunningMonitorOnlyCaption, L"Show only the windows on this dock's display");
        NextRow();
        Label(kTitlesLabel, L"Titles under icons");
        Combo(kTitlesCombo, kColX, right - kColX, false);
        NextRow();
        Label(kTitlesVerticalLabel, L"Titles on vertical docks");
        Combo(kTitlesVerticalCombo, kColX, right - kColX, false);
        NextRow(kRowH);
        Text(kTaskListNote, CtlKind::Note,
             L"One slot per window: a pinned app keeps its icon and its windows follow it. Titles show the "
             L"window title, or the app's name.",
             kMargin, right - kMargin, 36);
        NextRow(36);
        Label(kThumbnailsLabel, L"Window thumbnails");
        Combo(kThumbnailsCombo, kColX, right - kColX, false);
        NextRow(kRowH);
        Text(kThumbnailsNote, CtlKind::Note,
             L"Live uses the GPU to show windows as they change (only while the dock is visible, at most the "
             L"frames per second below). Snapshots takes a picture every N seconds and costs almost nothing in "
             L"between. Each window gets a tile (stacked: each app its active window).",
             kMargin, right - kMargin, 52);
        NextRow(52);
        Label(kThumbIntervalLabel, L"Snapshot every");
        IntSpin(kThumbIntervalEdit, kThumbIntervalSpin, kColX, 90, 1, 3600, 1,
                [](Config&, DockConfig& d) -> int& { return d.thumbnailIntervalS; }, SettingsScope::Appearance);
        Text(kThumbIntervalUnit, CtlKind::Note, L"s", kColX + 98, 40);
        NextRow();
        Label(kThumbFpsLabel, L"Live frames per second");
        IntSpin(kThumbFpsEdit, kThumbFpsSpin, kColX, 90, 1, 60, 1, [](Config&, DockConfig& d) -> int& { return d.thumbnailFps; },
                SettingsScope::Appearance);
        Text(kThumbFpsUnit, CtlKind::Note, L"at most", kColX + 98, 80);
        NextRow();
        Label(kThumbSizeLabel, L"Tile length");
        IntSpin(kThumbSizeEdit, kThumbSizeSpin, kColX, 90, 0, 512, 8, [](Config&, DockConfig& d) -> int& { return d.thumbnailSize; },
                SettingsScope::Appearance);
        Text(kThumbSizeUnit, CtlKind::Note, L"DIP (0: twice the icon)", kColX + 98, 200);
        NextRow();
    }

    void Created() override {
        for (const wchar_t* e : {L"Bottom", L"Top", L"Left", L"Right"}) ComboAdd(kEdgeCombo, e);  // DockEdge order
        ComboAdd(kLayerCombo, L"Above windows");  // DockLayer order
        ComboAdd(kLayerCombo, L"Desktop only (shown while the desktop is visible)");
        for (const wchar_t* a : {L"Centre", L"Start (left / top)", L"End (right / bottom)"}) ComboAdd(kAlignCombo, a);
        // OverflowMode order.
        for (const wchar_t* o : {L"Scroll (wheel, drag the bar)", L"Shrink the icons", L"Clip at the screen's end"}) {
            ComboAdd(kOverflowCombo, o);
        }
        // DockShape order.
        for (const wchar_t* o : {L"Bar", L"Half circle (on the edge)", L"Circle (anywhere)"}) ComboAdd(kShapeCombo, o);
        ComboAdd(kMiddleClickCombo, L"Start a new instance");  // MiddleClickAction order
        ComboAdd(kMiddleClickCombo, L"Minimize its windows");
        ComboAdd(kMiddleClickCombo, L"Close its windows");
        ComboAdd(kMiddleClickCombo, L"Nothing");
        ComboAdd(kWheelCombo, L"Nothing");  // WheelAction order
        ComboAdd(kWheelCombo, L"Switch between its windows");
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
    }

    void Load() override {
        const DockConfig& d = Bound();
        LoadInts();
        // Behaviour
        SetTrack(kIconSizeTrack, std::min(d.iconSize, kMaxTrackIconSize));
        const int mag = static_cast<int>(std::lround(d.magnification * 20.0));
        SetTrack(kMagnifyTrack, mag);
        SetText(kMagnifyValue, FormatMagnification(mag));
        SetCheck(kAutoHideOff, d.autoHide == AutoHideMode::Off);
        SetCheck(kAutoHideAlways, d.autoHide == AutoHideMode::Always);
        SetCheck(kAutoHideIntelligent, d.autoHide == AutoHideMode::Intelligent);
        SetCheck(kPreviews, d.previews);
        SetCheck(kPreviewSingle, d.previewSingle);
        SetComboSel(kMiddleClickCombo, static_cast<int>(d.middleClick));
        SetComboSel(kWheelCombo, static_cast<int>(d.wheel));
        // Running apps
        SetCheck(kRunningMonitorOnly, d.runningFilter == RunningFilter::Monitor);
        SetComboSel(kWindowsCombo, static_cast<int>(d.windows));
        SetComboSel(kTitlesCombo, static_cast<int>(d.titles));
        SetComboSel(kTitlesVerticalCombo, static_cast<int>(d.titlesVertical));
        SetComboSel(kThumbnailsCombo, static_cast<int>(d.thumbnails));
        // Position
        if (part_ == Part::Position) FillMonitors();
        SetComboSel(kLayerCombo, static_cast<int>(d.layer));
        SetComboSel(kEdgeCombo, static_cast<int>(d.edge));
        SetComboSel(kAlignCombo, static_cast<int>(d.align));
        SetComboSel(kOverflowCombo, static_cast<int>(d.overflow));
        SetComboSel(kShapeCombo, static_cast<int>(d.shape));
        SetCheck(kRotateIcons, d.rotateIcons);
        SetText(kCircleXEdit, d.x == DockConfig::kCentred ? std::wstring() : std::to_wstring(d.x));
        SetText(kCircleYEdit, d.y == DockConfig::kCentred ? std::wstring() : std::to_wstring(d.y));
        SetCheck(kFullWidth, d.fullWidth);
        SetCheck(kReserveSpace, d.reserveSpace);
        SetSpin(kPeekOpacitySpin, static_cast<int>(std::lround(d.peekOpacity * 100.0)));
        SetText(kTitleEdit, d.title);
        EnableDependents();
    }

    void OnDisplayChange() override {
        if (part_ == Part::Position) FillMonitors();
    }

    void IntChanged(int spin, int value) override {
        if (spin == kIconSizeSpin) SetTrack(kIconSizeTrack, std::min(value, kMaxTrackIconSize));
    }

    void Track(int id, int pos, bool released) override {
        DockConfig& d = Bound();
        if (id == kIconSizeTrack) {
            SetSpin(kIconSizeSpin, pos);  // follows the thumb; applied on release
            if (released && d.iconSize != pos) {
                d.iconSize = pos;
                Changed(SettingsScope::Appearance);
            }
        } else if (id == kMagnifyTrack) {
            SetText(kMagnifyValue, FormatMagnification(pos));
            const double mag = pos / 20.0;
            if (released && std::fabs(d.magnification - mag) > 1e-9) {
                d.magnification = mag;
                Changed(SettingsScope::Appearance);
            }
        }
    }

    void OnCommand(int id, int code, HWND) override {
        DockConfig& d = Bound();
        if (code == CBN_SELCHANGE) {
            const int sel = ComboSel(id);
            if (sel < 0) return;
            switch (id) {
                case kMonitorCombo: {
                    // The displays, then "Every display" (monitor "each").
                    const int count = static_cast<int>(SendMessageW(Item(id), CB_GETCOUNT, 0, 0));
                    const int value = sel == count - 1 ? DockConfig::kMonitorEach : sel;
                    if (value != d.monitor && !(sel == 0 && d.monitor == DockConfig::kMonitorPrimary)) {
                        d.monitor = value;
                        Changed(SettingsScope::Appearance);
                    }
                    break;
                }
                case kLayerCombo:
                    if (sel <= 1 && static_cast<DockLayer>(sel) != d.layer) {
                        d.layer = static_cast<DockLayer>(sel);
                        Changed(SettingsScope::Appearance);
                    }
                    break;
                case kEdgeCombo:
                    if (sel <= 3 && static_cast<DockEdge>(sel) != d.edge) {
                        d.edge = static_cast<DockEdge>(sel);
                        Changed(SettingsScope::Appearance);
                    }
                    break;
                case kAlignCombo:
                    if (sel <= 2 && static_cast<DockAlign>(sel) != d.align) {
                        d.align = static_cast<DockAlign>(sel);
                        Changed(SettingsScope::Appearance);
                    }
                    break;
                case kShapeCombo:
                    if (sel <= 2 && static_cast<DockShape>(sel) != d.shape) {
                        d.shape = static_cast<DockShape>(sel);
                        EnableDependents();
                        Changed(SettingsScope::Appearance);
                    }
                    break;
                case kOverflowCombo:
                    if (sel <= 2 && static_cast<OverflowMode>(sel) != d.overflow) {
                        d.overflow = static_cast<OverflowMode>(sel);
                        Changed(SettingsScope::Appearance);
                    }
                    break;
                // Read by the dock at click / wheel time.
                case kMiddleClickCombo:
                    if (sel <= 3 && static_cast<MiddleClickAction>(sel) != d.middleClick) {
                        d.middleClick = static_cast<MiddleClickAction>(sel);
                        Changed(SettingsScope::SaveOnly);
                    }
                    break;
                case kWheelCombo:
                    if (sel <= 1 && static_cast<WheelAction>(sel) != d.wheel) {
                        d.wheel = static_cast<WheelAction>(sel);
                        Changed(SettingsScope::SaveOnly);
                    }
                    break;
                case kWindowsCombo:
                    if (sel <= 1 && static_cast<WindowsMode>(sel) != d.windows) {
                        d.windows = static_cast<WindowsMode>(sel);
                        Changed(SettingsScope::Appearance);  // the dock rebuilds its slots
                    }
                    break;
                case kTitlesCombo:
                    if (sel <= 2 && static_cast<TitlesMode>(sel) != d.titles) {
                        d.titles = static_cast<TitlesMode>(sel);
                        EnableDependents();
                        Changed(SettingsScope::Appearance);  // relayout: the bar grows by the titles
                    }
                    break;
                case kTitlesVerticalCombo:
                    if (sel <= 1 && static_cast<TitlesVertical>(sel) != d.titlesVertical) {
                        d.titlesVertical = static_cast<TitlesVertical>(sel);
                        Changed(SettingsScope::Appearance);
                    }
                    break;
                case kThumbnailsCombo:
                    if (sel <= 2 && static_cast<ThumbnailMode>(sel) != d.thumbnails) {
                        d.thumbnails = static_cast<ThumbnailMode>(sel);
                        EnableDependents();
                        Changed(SettingsScope::Appearance);  // slots become tiles; capture starts / stops
                    }
                    break;
                default:
                    break;
            }
            return;
        }
        if (code != BN_CLICKED) return;
        switch (id) {
            case kRunningMonitorOnly:
                d.runningFilter = GetCheck(id) ? RunningFilter::Monitor : RunningFilter::All;
                Changed(SettingsScope::Pins);  // the dock rebuilds its items from the window list
                break;
            case kFullWidth:
                d.fullWidth = GetCheck(id);
                Changed(SettingsScope::Appearance);
                break;
            case kReserveSpace:
                d.reserveSpace = GetCheck(id);
                Changed(SettingsScope::Appearance);
                break;
            case kRotateIcons:
                d.rotateIcons = GetCheck(id);
                Changed(SettingsScope::Appearance);
                break;
            case kAutoHideOff:
            case kAutoHideAlways:
            case kAutoHideIntelligent: {
                const AutoHideMode mode = id == kAutoHideOff      ? AutoHideMode::Off
                                          : id == kAutoHideAlways ? AutoHideMode::Always
                                                                  : AutoHideMode::Intelligent;
                if (!GetCheck(id) || mode == d.autoHide) break;
                d.autoHide = mode;
                EnableDependents();
                Changed(SettingsScope::AutoHide);
                break;
            }
            case kPreviews:
                d.previews = GetCheck(id);
                EnableDependents();
                Changed(SettingsScope::SaveOnly);
                break;
            case kPreviewSingle:
                d.previewSingle = GetCheck(id);  // read when the mouse rests on an app
                Changed(SettingsScope::SaveOnly);
                break;
            default:
                break;
        }
    }

    void Commit(int id) override {
        DockConfig& d = Bound();
        if (id == kTitleEdit) {
            std::wstring title = GetText(id);
            if (title.size() > 128) title.resize(128);
            if (title != d.title) {
                d.title = title;
                Changed(SettingsScope::Appearance);
            }
        }
        if (id == kPeekOpacityEdit) {
            int v = 0;
            if (ReadInt(id, 5, 100, v)) {
                SetPeekOpacity(v);
            } else {
                SetSpin(kPeekOpacitySpin, static_cast<int>(std::lround(d.peekOpacity * 100.0)));
            }
        }
        if (id == kCircleXEdit || id == kCircleYEdit) {
            int& field = id == kCircleXEdit ? d.x : d.y;
            const std::wstring text = GetText(id);
            int v = DockConfig::kCentred;
            if (!text.empty() && !ReadInt(id, -16000, 16000, v)) {
                SetText(id, field == DockConfig::kCentred ? std::wstring() : std::to_wstring(field));  // not a number
                return;
            }
            if (v != field) {
                field = v;
                Changed(SettingsScope::Appearance);
            }
        }
    }

    void Spun(int spin, int pos) override {
        if (spin == kPeekOpacitySpin) SetPeekOpacity(pos);
    }

private:
    // The dock this page edits (the window's, looked up by id each time).
    DockConfig& Bound() { return Dock(); }

    void SetPeekOpacity(int percent) {
        DockConfig& d = Bound();
        percent = std::clamp(percent, 5, 100);
        SetSpin(kPeekOpacitySpin, percent);
        const double v = percent / 100.0;
        if (std::fabs(d.peekOpacity - v) < 1e-9) return;
        d.peekOpacity = v;
        Changed(SettingsScope::Appearance);
    }

    void EnableDependents() {
        const DockConfig& d = Bound();
        // Radius and Rotate icons apply to the ring shapes only; the centre to a circle.
        for (int id : {kRadiusLabel, kRadiusEdit, kRadiusSpin, kRotateIcons, kRotateIconsCaption}) {
            if (HWND h = Item(id)) EnableWindow(h, d.Polar());
        }
        for (int id : {kCircleXLabel, kCircleXEdit, kCircleYLabel, kCircleYEdit}) {
            if (HWND h = Item(id)) EnableWindow(h, d.shape == DockShape::Circle);
        }
        Enable(kHideDelayEdit, d.AutoHideOn());
        Enable(kRevealDelayEdit, d.AutoHideOn());
        Enable(kPreviewDelayEdit, d.previews);
        Enable(kPreviewSingle, d.previews);
        Enable(kTitlesVerticalCombo, d.titles != TitlesMode::Off);
        Enable(kThumbIntervalEdit, d.thumbnails == ThumbnailMode::Snapshots);
        Enable(kThumbFpsEdit, d.thumbnails == ThumbnailMode::Live);
        Enable(kThumbSizeEdit, d.thumbnails != ThumbnailMode::Off);
    }

    void FillMonitors() {
        HWND combo = Item(kMonitorCombo);
        if (!combo) return;
        std::vector<std::wstring> names = MonitorNames();
        const int monitor = Bound().monitor;
        const int want = std::max(monitor, 0);  // "primary": the first entry
        // A configured display that is not connected: the dock is parked until it is.
        for (int i = static_cast<int>(names.size()); i <= want; ++i) {
            names.push_back(L"Display " + std::to_wstring(i + 1) + L" (not connected)");
        }
        names.push_back(L"Every display (one dock on each)");
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (const std::wstring& n : names) ComboAdd(kMonitorCombo, n.c_str());
        SetComboSel(kMonitorCombo, monitor == DockConfig::kMonitorEach ? static_cast<int>(names.size()) - 1 : want);
    }

    Part part_;
};

// --- About -----------------------------------------------------------------------------------

std::wstring ModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring FileVersion() {
    const std::wstring path = ModulePath();
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return L"unknown";
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) {
        return L"unknown";
    }
    wchar_t buf[64];
    swprintf_s(buf, L"%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
               HIWORD(info->dwFileVersionLS));
    return buf;
}

class AboutPage : public FormPage {
public:
    explicit AboutPage(SettingsWindow& w) : FormPage(w, L"About", L'\xE946') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"About");
        Text(kAboutName, CtlKind::Section, L"WinDock", kMargin, right - kMargin, 24);
        NextRow(26);
        Text(kAboutVersion, CtlKind::Value, L"Version " + FileVersion(), kMargin, right - kMargin, 20);
        NextRow(22);
        Text(kAboutNote, CtlKind::Note, L"A macOS-style dock for Windows 11.", kMargin, right - kMargin, 20);
        NextRow(20);

        Section(kSectionConfig, L"Configuration");
        Label(kConfigPathLabel, L"Settings are saved to");
        NextRow(kRowH - 6);
        Link(kConfigPathLink, L"<a>" + Config::FilePath() + L"</a>", kMargin, right - kMargin);
        NextRow();
        Button(kOpenConfig, L"Open config file", kMargin, 150);
        Button(kReloadConfig, L"Reload config", kMargin + 162, 150);
        NextRow();
        Label(kAboutRendererLabel, L"Renderer in use");
        Text(kAboutRenderer, CtlKind::Value, L"", kColX, right - kColX);
        NextRow();
    }

    void Load() override {
        const bool warp = Host().usingWarp && Host().usingWarp();
        SetText(kAboutRenderer, warp ? L"Software (WARP)" : L"GPU (Direct3D 11)");
    }

    void OnCommand(int id, int code, HWND) override {
        if (code != BN_CLICKED || !Host().command) return;
        if (id == kOpenConfig) Host().command(ContextMenu::kOpenConfig);
        if (id == kReloadConfig) Host().command(ContextMenu::kReloadConfig);  // refreshes every page
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) == kConfigPathLink && (nm.code == NM_CLICK || nm.code == NM_RETURN)) {
            // The folder, with the file selected (or just the folder before the first save).
            const std::wstring file = Config::FilePath();
            AllowSetForegroundWindow(ASFW_ANY);
            if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) {
                const std::wstring args = L"/select,\"" + file + L"\"";
                ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            } else {
                ShellExecuteW(nullptr, L"open", Config::Directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }
        return 0;
    }
};


}  // namespace

std::vector<std::unique_ptr<SettingsPage>> CreateSettingsPages(SettingsWindow& window) {
    std::vector<std::unique_ptr<SettingsPage>> pages;
    switch (window.Kind()) {
        case SettingsKind::Global:
            pages.push_back(std::make_unique<GeneralPage>(window));  // kPageGeneral
            pages.push_back(MakeAppearancePage(window));             // kPageThemes
            pages.push_back(MakeAnimationPage(window));              // kPageAnimation
            pages.push_back(MakeCalendarPage(window));               // kPageCalendar
            pages.push_back(MakeTrayPage(window));                   // kPageTray
            pages.push_back(MakeNotificationsPage(window));          // kPageNotifications
            pages.push_back(MakeWidgetsPage(window));                // kPageWidgets
            pages.push_back(MakeLayoutPage(window));                 // kPageDocks
            pages.push_back(std::make_unique<AboutPage>(window));    // kPageAbout
            break;
        case SettingsKind::Dock:
            if (window.IsFolderWindow()) {
                pages.push_back(MakeFolderWindowPage(window));  // kTabFolderWindow
                break;
            }
            pages.push_back(MakeContentsPage(window));                                                    // kTabContents
            pages.push_back(std::make_unique<DockFieldsPage>(window, DockFieldsPage::Part::Position));   // kTabPosition
            pages.push_back(std::make_unique<DockFieldsPage>(window, DockFieldsPage::Part::Behaviour));  // kTabBehaviour
            pages.push_back(std::make_unique<DockFieldsPage>(window, DockFieldsPage::Part::Running));    // kTabRunning
            pages.push_back(MakeDockThemePage(window));                                                  // kTabTheme
            break;
        case SettingsKind::Item: {
            // A widget, clock or weather item: its generated schema page; a folder / group: Folders and groups.
            const DockItemEntry* e = window.TargetItem();
            const ItemKind k = e ? e->Kind() : ItemKind::Unknown;
            if (k == ItemKind::Widget || k == ItemKind::Clock || k == ItemKind::Weather) {
                pages.push_back(MakeWidgetSettingsPage(window));
            } else {
                pages.push_back(MakeItemSettingsPage(window));
            }
            break;
        }
    }
    return pages;
}