// The settings pages: control tables (DIP layout) and handlers. This file: General, Behaviour,
// About and the page list; Appearance: SettingsPagesAppearance.cpp; Animation and Dock contents:
// SettingsPagesMore.cpp; Weather: SettingsPagesWeather.cpp. The frame, fonts, colours, theming, layout and scrolling live in
// SettingsWindow.cpp.

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

#include "Clock.h"
#include "ContextMenu.h"
#include "SettingsForm.h"
#include "SettingsIds.h"
#include "SettingsWindow.h"

namespace {

using namespace SettingsIds;

// --- General ---------------------------------------------------------------------------------

constexpr int kMaxTrackIconSize = 128;  // the spinner goes on to 256 (Config::Sanitize)
constexpr const wchar_t* kClockFormats[] = {L"%H:%M", L"%H:%M:%S", L"%I:%M %p", L"%#I:%M %p"};

std::wstring FormatMagnification(int pos) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.2f×", pos / 20.0);
    return buf;
}

std::wstring ClockPreview(const std::wstring& format, bool seconds) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    const std::wstring text = Clock::Format(Clock::EffectiveFormat(format, seconds), local);
    return text.empty() ? std::wstring() : L"Shows “" + text + L"”";
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

class GeneralPage : public FormPage {
public:
    explicit GeneralPage(SettingsWindow& w) : FormPage(w, L"General", L'\xE713') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"General");
        Section(kSectionDock, L"Dock");
        Label(kIconSizeLabel, L"Icon size");
        Trackbar(kIconSizeTrack, kColX - 6, 212, 16, kMaxTrackIconSize, 8);
        IntSpin(kIconSizeEdit, kIconSizeSpin, kColX + 216, 72, 16, 256, 4,
                [](Config& c) -> int& { return c.Edit().iconSize; }, SettingsScope::Appearance);
        NextRow();
        Label(kMagnifyLabel, L"Magnification");
        Trackbar(kMagnifyTrack, kColX - 6, 212, 20, 50, 2);
        Text(kMagnifyValue, CtlKind::Value, L"", kColX + 216, 72);
        NextRow();
        Label(kRangeLabel, L"Magnification range (icons)");
        IntSpin(kRangeEdit, kRangeSpin, kColX, 72, 1, 4, 1, [](Config& c) -> int& { return c.Edit().magnifyRange; },
                SettingsScope::Appearance);
        NextRow();
        Label(kMonitorLabel, L"Display");
        Combo(kMonitorCombo, kColX, right - kColX, false);
        NextRow();
        Check(kRunningMonitorOnly, kRunningMonitorOnlyCaption, L"Show only the windows on this dock's display");
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
        IntSpin(kOffsetEdit, kOffsetSpin, kColX, 88, -16000, 16000, 10,
                [](Config& c) -> int& { return c.Edit().offset; }, SettingsScope::Appearance);
        NextRow(kRowH);
        Text(kPositionNote, CtlKind::Note,
             L"Moves the dock along its edge from the aligned place (right / down). Ctrl+drag the dock to move it "
             L"along its edge or onto another edge (unless it is locked).",
             kColX, right - kColX, 54);
        NextRow(56);
        Check(kFullWidth, kFullWidthCaption, L"Full width (the bar spans the whole edge)");
        NextRow();
        Check(kReserveSpace, kReserveSpaceCaption, L"Reserve screen space (maximized windows stop at the dock)");
        NextRow();
        Label(kPeekLabel, L"Peek when hidden (pixels)");
        IntSpin(kPeekEdit, kPeekSpin, kColX, 72, 0, 64, 2, [](Config& c) -> int& { return c.Edit().peekPx; },
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
             kColX, right - kColX, 104);
        NextRow(106);

        Section(kSectionClock, L"Clock");
        Check(kClockShow, kClockShowCaption, L"Show the clock");
        NextRow();
        Label(kClockFormatLabel, L"Time format");
        Combo(kClockFormatCombo, kColX, 140, true);
        Text(kClockPreview, CtlKind::Note, L"", kColX + 152, right - kColX - 152);
        NextRow();
        Check(kClockSeconds, kClockSecondsCaption, L"Show seconds");
        NextRow();
        Check(kClockDate, kClockDateCaption, L"Show the date under the time");
        NextRow();
        Label(kClockClickLabel, L"Clicking the clock opens");
        Combo(kClockClickCombo, kColX, right - kColX, false);
        NextRow();
        Label(kClockTargetLabel, L"App or URL");
        EditBox(kClockTargetEdit, kColX, right - kColX - 112, 2048);
        Button(kClockTargetBrowse, L"Browse…", right - 104, 104);
        NextRow(kRowH);
        Text(kClockTargetNote, CtlKind::Note, L"The month calendar shows a button that opens it too.", kColX,
             right - kColX, 20);
        NextRow(22);

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
    }

    void Created() override {
        for (const wchar_t* f : kClockFormats) ComboAdd(kClockFormatCombo, f);
        SendMessageW(Item(kClockFormatCombo), CB_LIMITTEXT, 128, 0);
        ComboAdd(kClockClickCombo, L"The month calendar");  // ClockClick order
        ComboAdd(kClockClickCombo, L"The app or URL below");
        ComboAdd(kClockClickCombo, L"Nothing");
        for (const wchar_t* e : {L"Bottom", L"Top", L"Left", L"Right"}) ComboAdd(kEdgeCombo, e);  // DockEdge order
        ComboAdd(kLayerCombo, L"Above windows");  // DockLayer order
        ComboAdd(kLayerCombo, L"Desktop only (shown while the desktop is visible)");
        for (const wchar_t* a : {L"Centre", L"Start (left / top)", L"End (right / bottom)"}) ComboAdd(kAlignCombo, a);
        ComboAdd(kRendererCombo, L"Automatic (GPU, software if that fails)");
        ComboAdd(kRendererCombo, L"Software (WARP, less memory)");
    }

    void Load() override {
        const Config& c = Cfg();
        LoadInts();
        SetTrack(kIconSizeTrack, std::min(c.Edit().iconSize, kMaxTrackIconSize));
        const int mag = static_cast<int>(std::lround(c.Edit().magnification * 20.0));
        SetTrack(kMagnifyTrack, mag);
        SetText(kMagnifyValue, FormatMagnification(mag));
        FillMonitors();
        SetCheck(kRunningMonitorOnly, c.Edit().runningFilter == RunningFilter::Monitor);
        SetComboSel(kLayerCombo, static_cast<int>(c.Edit().layer));
        SetComboSel(kEdgeCombo, static_cast<int>(c.Edit().edge));
        SetComboSel(kAlignCombo, static_cast<int>(c.Edit().align));
        SetCheck(kFullWidth, c.Edit().fullWidth);
        SetCheck(kReserveSpace, c.Edit().reserveSpace);
        SetSpin(kPeekOpacitySpin, static_cast<int>(std::lround(c.Edit().peekOpacity * 100.0)));
        SetText(kTitleEdit, c.Edit().title);
        SetCheck(kClockShow, c.Edit().Has(ItemKind::Clock));
        SetText(kClockFormatCombo, c.clock.format);
        SetText(kClockPreview, ClockPreview(c.clock.format, c.clock.showSeconds));
        SetCheck(kClockSeconds, c.clock.showSeconds);
        SetCheck(kClockDate, c.clock.showDate);
        SetComboSel(kClockClickCombo, static_cast<int>(c.clock.click));
        SetText(kClockTargetEdit, c.clock.clickTarget);
        EnableClock(c.Edit().Has(ItemKind::Clock));
        SetCheck(kStartWithWindows, Host().autostartEnabled ? Host().autostartEnabled() : c.startWithWindows);
        SetCheck(kHideTaskbar, Host().taskbarHidden ? Host().taskbarHidden() : c.hideTaskbar);
        SetComboSel(kRendererCombo, c.renderer == L"software" ? 1 : 0);
        const bool warp = Host().usingWarp && Host().usingWarp();
        SetText(kRendererNote, std::wstring(L"Takes effect after WinDock restarts. Currently: ") +
                                   (warp ? L"software (WARP)" : L"GPU"));
    }

    void OnDisplayChange() override { FillMonitors(); }

    void IntChanged(int spin, int value) override {
        if (spin == kIconSizeSpin) SetTrack(kIconSizeTrack, std::min(value, kMaxTrackIconSize));
    }

    void Track(int id, int pos, bool released) override {
        Config& c = Cfg();
        if (id == kIconSizeTrack) {
            SetSpin(kIconSizeSpin, pos);  // follows the thumb; applied on release
            if (released && c.Edit().iconSize != pos) {
                c.Edit().iconSize = pos;
                Changed(SettingsScope::Appearance);
            }
        } else if (id == kMagnifyTrack) {
            SetText(kMagnifyValue, FormatMagnification(pos));
            const double mag = pos / 20.0;
            if (released && std::fabs(c.Edit().magnification - mag) > 1e-9) {
                c.Edit().magnification = mag;
                Changed(SettingsScope::Appearance);
            }
        }
    }

    void OnCommand(int id, int code, HWND) override {
        Config& c = Cfg();
        switch (id) {
            case kMonitorCombo:
                if (code == CBN_SELCHANGE) {
                    // The displays, then "Every display" (monitor "each").
                    const int sel = ComboSel(id);
                    const int count = static_cast<int>(SendMessageW(Item(id), CB_GETCOUNT, 0, 0));
                    const int value = sel == count - 1 ? DockConfig::kMonitorEach : sel;
                    if (sel >= 0 && value != c.Edit().monitor &&
                        !(sel == 0 && c.Edit().monitor == DockConfig::kMonitorPrimary)) {
                        c.Edit().monitor = value;
                        Changed(SettingsScope::Appearance);
                    }
                }
                break;
            case kLayerCombo:
                if (code == CBN_SELCHANGE) {
                    const int sel = ComboSel(id);
                    if (sel >= 0 && sel <= 1 && static_cast<DockLayer>(sel) != c.Edit().layer) {
                        c.Edit().layer = static_cast<DockLayer>(sel);
                        Changed(SettingsScope::Appearance);
                    }
                }
                break;
            case kRunningMonitorOnly:
                if (code == BN_CLICKED) {
                    c.Edit().runningFilter = GetCheck(id) ? RunningFilter::Monitor : RunningFilter::All;
                    Changed(SettingsScope::Pins);  // the dock rebuilds its items from the window list
                }
                break;
            case kEdgeCombo:
                if (code == CBN_SELCHANGE) {
                    const int sel = ComboSel(id);
                    if (sel >= 0 && sel <= 3 && static_cast<DockEdge>(sel) != c.Edit().edge) {
                        c.Edit().edge = static_cast<DockEdge>(sel);
                        Changed(SettingsScope::Appearance);
                    }
                }
                break;
            case kFullWidth:
                if (code == BN_CLICKED) {
                    c.Edit().fullWidth = GetCheck(id);
                                Changed(SettingsScope::Appearance);
                }
                break;
            case kReserveSpace:
                if (code == BN_CLICKED) {
                    c.Edit().reserveSpace = GetCheck(id);
                    Changed(SettingsScope::Appearance);
                }
                break;
            case kAlignCombo:
                if (code == CBN_SELCHANGE) {
                    const int sel = ComboSel(id);
                    if (sel >= 0 && sel <= 2 && static_cast<DockAlign>(sel) != c.Edit().align) {
                        c.Edit().align = static_cast<DockAlign>(sel);
                        Changed(SettingsScope::Appearance);
                    }
                }
                break;
            case kClockShow:
                if (code == BN_CLICKED) {
                    // The clock is shown by a "clock" item: added at the end / every one removed.
                    c.Edit().Show(ItemKind::Clock, GetCheck(id));
                    EnableClock(c.Edit().Has(ItemKind::Clock));
                    Changed(SettingsScope::Appearance);
                }
                break;
            case kClockFormatCombo:
                if (code == CBN_SELCHANGE) {
                    // The edit still holds the old text: read the chosen item.
                    const int sel = ComboSel(id);
                    if (sel >= 0 && sel < static_cast<int>(std::size(kClockFormats))) CommitFormat(kClockFormats[sel]);
                } else if (code == CBN_EDITCHANGE) {
                    SetText(kClockPreview, ClockPreview(GetText(id), c.clock.showSeconds));  // live preview, applied on commit
                }
                break;
            case kClockSeconds:
                if (code == BN_CLICKED) {
                    c.clock.showSeconds = GetCheck(id);
                    SetText(kClockPreview, ClockPreview(c.clock.format, c.clock.showSeconds));
                    Changed(SettingsScope::Appearance);  // the clock's text, its width and its tick
                }
                break;
            case kClockClickCombo:
                if (code == CBN_SELCHANGE) {
                    const int sel = ComboSel(id);
                    if (sel >= 0 && sel <= 2 && static_cast<ClockClick>(sel) != c.clock.click) {
                        c.clock.click = static_cast<ClockClick>(sel);  // read at click time
                        Changed(SettingsScope::SaveOnly);
                    }
                }
                break;
            case kClockTargetBrowse:
                if (code == BN_CLICKED) BrowseTarget();
                break;
            case kClockDate:
                if (code == BN_CLICKED) {
                    c.clock.showDate = GetCheck(id);
                    Changed(SettingsScope::Appearance);
                }
                break;
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
            default:
                break;
        }
    }

    void Commit(int id) override {
        if (id == kClockFormatCombo) CommitFormat(GetText(id));
        if (id == kClockTargetEdit) CommitTarget(GetText(id));
        if (id == kTitleEdit) {
            std::wstring title = GetText(id);
            if (title.size() > 128) title.resize(128);
            if (title != Cfg().Edit().title) {
                Cfg().Edit().title = title;
                Changed(SettingsScope::Appearance);
            }
        }
        if (id == kPeekOpacityEdit) {
            int v = 0;
            if (ReadInt(id, 5, 100, v)) {
                SetPeekOpacity(v);
            } else {
                SetSpin(kPeekOpacitySpin, static_cast<int>(std::lround(Cfg().Edit().peekOpacity * 100.0)));
            }
        }
    }

    void Spun(int spin, int pos) override {
        if (spin == kPeekOpacitySpin) SetPeekOpacity(pos);
    }

    void SetPeekOpacity(int percent) {
        percent = std::clamp(percent, 5, 100);
        SetSpin(kPeekOpacitySpin, percent);
        const double v = percent / 100.0;
        if (std::fabs(Cfg().Edit().peekOpacity - v) < 1e-9) return;
        Cfg().Edit().peekOpacity = v;
        Changed(SettingsScope::Appearance);
    }

private:
    void CommitTarget(std::wstring target) {
        if (target.size() > 2048) target.resize(2048);
        Config& c = Cfg();
        if (target == c.clock.clickTarget) return;
        c.clock.clickTarget = target;  // read when the clock is clicked / the calendar opens
        Changed(SettingsScope::SaveOnly);
    }

    void BrowseTarget() {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        const COMDLG_FILTERSPEC filters[] = {{L"Apps and shortcuts (*.exe;*.lnk)", L"*.exe;*.lnk"},
                                             {L"All files", L"*.*"}};
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetTitle(L"Open when the clock is clicked");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NODEREFERENCELINKS | FOS_NOCHANGEDIR);
        if (FAILED(dialog->Show(Owner()))) return;  // cancelled
        winrt::com_ptr<IShellItem> item;
        PWSTR raw = nullptr;
        if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return;
        std::wstring path(raw);
        CoTaskMemFree(raw);
        // A path with spaces is quoted, so arguments may be typed after it.
        if (path.find(L' ') != std::wstring::npos) path = L"\"" + path + L"\"";
        SetText(kClockTargetEdit, path);
        CommitTarget(path);
    }

    void CommitFormat(std::wstring format) {
        if (format.size() > 128) format.resize(128);
        Config& c = Cfg();
        if (format.empty()) format = c.clock.format;  // an empty format would hide the clock
        SetText(kClockFormatCombo, format);
        SetText(kClockPreview, ClockPreview(format, c.clock.showSeconds));
        if (format == c.clock.format) return;
        c.clock.format = format;
        Changed(SettingsScope::Appearance);
    }

    void EnableClock(bool on) {
        Enable(kClockFormatCombo, on);
        Enable(kClockSeconds, on);
        Enable(kClockDate, on);
        Enable(kClockClickCombo, on);
        Enable(kClockTargetEdit, on);
        Enable(kClockTargetBrowse, on);
    }

    void FillMonitors() {
        HWND combo = Item(kMonitorCombo);
        if (!combo) return;
        std::vector<std::wstring> names = MonitorNames();
        const int monitor = Cfg().Edit().monitor;
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
};

// --- Behaviour -------------------------------------------------------------------------------

class BehaviourPage : public FormPage {
public:
    explicit BehaviourPage(SettingsWindow& w) : FormPage(w, L"Behaviour", L'\xE7C4') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Behaviour");
        Section(kSectionAutoHide, L"Auto-hide");
        Radios({
            {kAutoHideOff, kAutoHideOffCaption, L"Off: the dock is always shown"},
            {kAutoHideAlways, kAutoHideAlwaysCaption, L"Always: hide when the mouse leaves the dock"},
            {kAutoHideIntelligent, kAutoHideIntelligentCaption, L"When a window overlaps the dock"},
        });
        Label(kHideDelayLabel, L"Hide delay");
        IntSpin(kHideDelayEdit, kHideDelaySpin, kColX, 90, 0, 10000, 50, [](Config& c) -> int& { return c.Edit().hideDelayMs; },
                SettingsScope::SaveOnly);
        Text(kHideDelayUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow();
        Label(kRevealDelayLabel, L"Reveal delay");
        IntSpin(kRevealDelayEdit, kRevealDelaySpin, kColX, 90, 0, 10000, 50,
                [](Config& c) -> int& { return c.Edit().revealDelayMs; }, SettingsScope::SaveOnly);
        Text(kRevealDelayUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow(kRowH);
        Text(kAutoHideNote, CtlKind::Note, L"The dock stays shown while this window is active.", kMargin, right - kMargin, 20);
        NextRow(20);

        Section(kSectionPreviews, L"Window previews");
        Check(kPreviews, kPreviewsCaption, L"Show window previews for apps with several windows");
        NextRow();
        Check(kPreviewSingle, kPreviewSingleCaption, L"Show window previews for apps with a single window",
              kMargin + kCaptionDx);
        NextRow();
        Label(kPreviewDelayLabel, L"Preview delay");
        IntSpin(kPreviewDelayEdit, kPreviewDelaySpin, kColX, 90, 0, 10000, 50,
                [](Config& c) -> int& { return c.Edit().previewDelayMs; }, SettingsScope::SaveOnly);
        Text(kPreviewDelayUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow();

        Section(kSectionMouse, L"Mouse");
        Label(kMiddleClickLabel, L"Middle click on an app");
        Combo(kMiddleClickCombo, kColX, right - kColX, false);
        NextRow();
        Label(kWheelLabel, L"Mouse wheel over an app");
        Combo(kWheelCombo, kColX, right - kColX, false);
        NextRow();

        Section(kSectionTaskList, L"Running apps");
        Label(kWindowsLabel, L"Windows");
        Combo(kWindowsCombo, kColX, right - kColX, false);
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
                [](Config& c) -> int& { return c.Edit().thumbnailIntervalS; }, SettingsScope::Appearance);
        Text(kThumbIntervalUnit, CtlKind::Note, L"s", kColX + 98, 40);
        NextRow();
        Label(kThumbFpsLabel, L"Live frames per second");
        IntSpin(kThumbFpsEdit, kThumbFpsSpin, kColX, 90, 1, 60, 1, [](Config& c) -> int& { return c.Edit().thumbnailFps; },
                SettingsScope::Appearance);
        Text(kThumbFpsUnit, CtlKind::Note, L"at most", kColX + 98, 80);
        NextRow();
        Label(kThumbSizeLabel, L"Tile length");
        IntSpin(kThumbSizeEdit, kThumbSizeSpin, kColX, 90, 0, 512, 8, [](Config& c) -> int& { return c.Edit().thumbnailSize; },
                SettingsScope::Appearance);
        Text(kThumbSizeUnit, CtlKind::Note, L"DIP (0: twice the icon)", kColX + 98, 200);
        NextRow();
    }

    void Created() override {
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
        const Config& c = Cfg();
        LoadInts();
        SetCheck(kAutoHideOff, c.Edit().autoHide == AutoHideMode::Off);
        SetCheck(kAutoHideAlways, c.Edit().autoHide == AutoHideMode::Always);
        SetCheck(kAutoHideIntelligent, c.Edit().autoHide == AutoHideMode::Intelligent);
        SetCheck(kPreviews, c.Edit().previews);
        SetCheck(kPreviewSingle, c.Edit().previewSingle);
        SetComboSel(kMiddleClickCombo, static_cast<int>(c.Edit().middleClick));
        SetComboSel(kWheelCombo, static_cast<int>(c.Edit().wheel));
        SetComboSel(kWindowsCombo, static_cast<int>(c.Edit().windows));
        SetComboSel(kTitlesCombo, static_cast<int>(c.Edit().titles));
        SetComboSel(kTitlesVerticalCombo, static_cast<int>(c.Edit().titlesVertical));
        SetComboSel(kThumbnailsCombo, static_cast<int>(c.Edit().thumbnails));
        EnableDependents();
    }

    void OnCommand(int id, int code, HWND) override {
        Config& c = Cfg();
        if (code == CBN_SELCHANGE) {
            // Read by the dock at click / wheel time.
            const int sel = ComboSel(id);
            if (id == kMiddleClickCombo && sel >= 0 && sel <= 3 && static_cast<MiddleClickAction>(sel) != c.Edit().middleClick) {
                c.Edit().middleClick = static_cast<MiddleClickAction>(sel);
                Changed(SettingsScope::SaveOnly);
            } else if (id == kWheelCombo && sel >= 0 && sel <= 1 && static_cast<WheelAction>(sel) != c.Edit().wheel) {
                c.Edit().wheel = static_cast<WheelAction>(sel);
                Changed(SettingsScope::SaveOnly);
            } else if (id == kWindowsCombo && sel >= 0 && sel <= 1 && static_cast<WindowsMode>(sel) != c.Edit().windows) {
                c.Edit().windows = static_cast<WindowsMode>(sel);
                Changed(SettingsScope::Appearance);  // the dock rebuilds its slots
            } else if (id == kTitlesCombo && sel >= 0 && sel <= 2 && static_cast<TitlesMode>(sel) != c.Edit().titles) {
                c.Edit().titles = static_cast<TitlesMode>(sel);
                EnableDependents();
                Changed(SettingsScope::Appearance);  // relayout: the bar grows by the titles
            } else if (id == kTitlesVerticalCombo && sel >= 0 && sel <= 1 &&
                       static_cast<TitlesVertical>(sel) != c.Edit().titlesVertical) {
                c.Edit().titlesVertical = static_cast<TitlesVertical>(sel);
                Changed(SettingsScope::Appearance);
            } else if (id == kThumbnailsCombo && sel >= 0 && sel <= 2 &&
                       static_cast<ThumbnailMode>(sel) != c.Edit().thumbnails) {
                c.Edit().thumbnails = static_cast<ThumbnailMode>(sel);
                EnableDependents();
                Changed(SettingsScope::Appearance);  // slots become tiles; capture starts / stops
            }
            return;
        }
        if (code != BN_CLICKED) return;
        switch (id) {
            case kAutoHideOff:
            case kAutoHideAlways:
            case kAutoHideIntelligent: {
                const AutoHideMode mode = id == kAutoHideOff      ? AutoHideMode::Off
                                          : id == kAutoHideAlways ? AutoHideMode::Always
                                                                  : AutoHideMode::Intelligent;
                if (!GetCheck(id) || mode == c.Edit().autoHide) break;
                c.Edit().autoHide = mode;
                EnableDependents();
                Changed(SettingsScope::AutoHide);
                break;
            }
            case kPreviews:
                c.Edit().previews = GetCheck(id);
                EnableDependents();
                Changed(SettingsScope::SaveOnly);
                break;
            case kPreviewSingle:
                c.Edit().previewSingle = GetCheck(id);  // read when the mouse rests on an app
                Changed(SettingsScope::SaveOnly);
                break;
            default:
                break;
        }
    }

private:
    void EnableDependents() {
        const Config& c = Cfg();
        Enable(kHideDelayEdit, c.Edit().AutoHideOn());
        Enable(kRevealDelayEdit, c.Edit().AutoHideOn());
        Enable(kPreviewDelayEdit, c.Edit().previews);
        Enable(kPreviewSingle, c.Edit().previews);
        Enable(kTitlesVerticalCombo, c.Edit().titles != TitlesMode::Off);
        Enable(kThumbIntervalEdit, c.Edit().thumbnails == ThumbnailMode::Snapshots);
        Enable(kThumbFpsEdit, c.Edit().thumbnails == ThumbnailMode::Live);
        Enable(kThumbSizeEdit, c.Edit().thumbnails != ThumbnailMode::Off);
    }
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
    pages.push_back(std::make_unique<GeneralPage>(window));  // kPageGeneral
    pages.push_back(MakeAppearancePage(window));  // kPageAppearance
    pages.push_back(std::make_unique<BehaviourPage>(window));  // kPageBehaviour
    pages.push_back(MakeAnimationPage(window));  // kPageAnimation
    pages.push_back(MakePinnedPage(window));  // kPagePinned
    pages.push_back(std::make_unique<AboutPage>(window));  // kPageAbout
    pages.push_back(MakeWeatherPage(window));  // kPageWeather (after About: the nav shows pages in index order)
    pages.push_back(MakeDocksPage(window));    // kPageDocks
    pages.push_back(MakeLayoutPage(window));   // kPageLayout
    pages.push_back(MakeWidgetsPage(window));  // kPageWidgets
    pages.push_back(MakeWidgetSettingsPage(window));  // kPageWidgetSettings
    pages.push_back(MakeItemSettingsPage(window));    // kPageItemSettings
    pages.push_back(MakeTrayPage(window));            // kPageTray
    pages.push_back(MakeNotificationsPage(window));   // kPageNotifications
    return pages;
}
