// The settings pages: control tables (DIP layout) and handlers. This file: General, Behaviour,
// About and the page list; Appearance: SettingsPagesAppearance.cpp; Animation and Pinned apps:
// SettingsPagesMore.cpp; Weather: SettingsPagesWeather.cpp. The frame, fonts, colours, theming, layout and scrolling live in
// SettingsWindow.cpp.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

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

std::wstring ClockPreview(const std::wstring& format) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    const std::wstring text = Clock::Format(format, local);
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
                [](Config& c) -> int& { return c.Main().iconSize; }, SettingsScope::Appearance);
        NextRow();
        Label(kMagnifyLabel, L"Magnification");
        Trackbar(kMagnifyTrack, kColX - 6, 212, 20, 50, 2);
        Text(kMagnifyValue, CtlKind::Value, L"", kColX + 216, 72);
        NextRow();
        Label(kRangeLabel, L"Magnification range (icons)");
        IntSpin(kRangeEdit, kRangeSpin, kColX, 72, 1, 4, 1, [](Config& c) -> int& { return c.Main().magnifyRange; },
                SettingsScope::Appearance);
        NextRow();
        Label(kMonitorLabel, L"Display");
        Combo(kMonitorCombo, kColX, right - kColX, false);
        NextRow();

        Section(kSectionClock, L"Clock");
        Check(kClockShow, kClockShowCaption, L"Show the clock");
        NextRow();
        Label(kClockFormatLabel, L"Time format");
        Combo(kClockFormatCombo, kColX, 140, true);
        Text(kClockPreview, CtlKind::Note, L"", kColX + 152, right - kColX - 152);
        NextRow();
        Check(kClockDate, kClockDateCaption, L"Show the date under the time");
        NextRow();
        Check(kClockCalendar, kClockCalendarCaption, L"Click the clock to show a month calendar");
        NextRow();

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
        ComboAdd(kRendererCombo, L"Automatic (GPU, software if that fails)");
        ComboAdd(kRendererCombo, L"Software (WARP, less memory)");
    }

    void Load() override {
        const Config& c = Cfg();
        LoadInts();
        SetTrack(kIconSizeTrack, std::min(c.Main().iconSize, kMaxTrackIconSize));
        const int mag = static_cast<int>(std::lround(c.Main().magnification * 20.0));
        SetTrack(kMagnifyTrack, mag);
        SetText(kMagnifyValue, FormatMagnification(mag));
        FillMonitors();
        SetCheck(kClockShow, c.Main().Has(ItemKind::Clock));
        SetText(kClockFormatCombo, c.clock.format);
        SetText(kClockPreview, ClockPreview(c.clock.format));
        SetCheck(kClockDate, c.clock.showDate);
        SetCheck(kClockCalendar, c.clock.calendarPopup);
        EnableClock(c.Main().Has(ItemKind::Clock));
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
            if (released && c.Main().iconSize != pos) {
                c.Main().iconSize = pos;
                Changed(SettingsScope::Appearance);
            }
        } else if (id == kMagnifyTrack) {
            SetText(kMagnifyValue, FormatMagnification(pos));
            const double mag = pos / 20.0;
            if (released && std::fabs(c.Main().magnification - mag) > 1e-9) {
                c.Main().magnification = mag;
                Changed(SettingsScope::Appearance);
            }
        }
    }

    void OnCommand(int id, int code, HWND) override {
        Config& c = Cfg();
        switch (id) {
            case kMonitorCombo:
                if (code == CBN_SELCHANGE) {
                    const int sel = ComboSel(id);
                    if (sel >= 0 && sel != c.Main().monitor) {
                        c.Main().monitor = sel;
                        Changed(SettingsScope::Appearance);
                    }
                }
                break;
            case kClockShow:
                if (code == BN_CLICKED) {
                    // The clock is shown by a "clock" item: added at the end / every one removed.
                    c.Main().Show(ItemKind::Clock, GetCheck(id));
                    EnableClock(c.Main().Has(ItemKind::Clock));
                    Changed(SettingsScope::Appearance);
                }
                break;
            case kClockFormatCombo:
                if (code == CBN_SELCHANGE) {
                    // The edit still holds the old text: read the chosen item.
                    const int sel = ComboSel(id);
                    if (sel >= 0 && sel < static_cast<int>(std::size(kClockFormats))) CommitFormat(kClockFormats[sel]);
                } else if (code == CBN_EDITCHANGE) {
                    SetText(kClockPreview, ClockPreview(GetText(id)));  // live preview, applied on commit
                }
                break;
            case kClockDate:
                if (code == BN_CLICKED) {
                    c.clock.showDate = GetCheck(id);
                    Changed(SettingsScope::Appearance);
                }
                break;
            case kClockCalendar:
                if (code == BN_CLICKED) {
                    c.clock.calendarPopup = GetCheck(id);  // read at click time
                    Changed(SettingsScope::SaveOnly);
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
    }

private:
    void CommitFormat(std::wstring format) {
        if (format.size() > 128) format.resize(128);
        Config& c = Cfg();
        if (format.empty()) format = c.clock.format;  // an empty format would hide the clock
        SetText(kClockFormatCombo, format);
        SetText(kClockPreview, ClockPreview(format));
        if (format == c.clock.format) return;
        c.clock.format = format;
        Changed(SettingsScope::Appearance);
    }

    void EnableClock(bool on) {
        Enable(kClockFormatCombo, on);
        Enable(kClockDate, on);
        Enable(kClockCalendar, on);
    }

    void FillMonitors() {
        HWND combo = Item(kMonitorCombo);
        if (!combo) return;
        std::vector<std::wstring> names = MonitorNames();
        const int want = std::max(Cfg().Main().monitor, 0);
        // A configured display that is not connected: the dock uses the primary meanwhile.
        for (int i = static_cast<int>(names.size()); i <= want; ++i) {
            names.push_back(L"Display " + std::to_wstring(i + 1) + L" (not connected)");
        }
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (const std::wstring& n : names) ComboAdd(kMonitorCombo, n.c_str());
        SetComboSel(kMonitorCombo, want);
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
        IntSpin(kHideDelayEdit, kHideDelaySpin, kColX, 90, 0, 10000, 50, [](Config& c) -> int& { return c.Main().hideDelayMs; },
                SettingsScope::SaveOnly);
        Text(kHideDelayUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow();
        Label(kRevealDelayLabel, L"Reveal delay");
        IntSpin(kRevealDelayEdit, kRevealDelaySpin, kColX, 90, 0, 10000, 50,
                [](Config& c) -> int& { return c.Main().revealDelayMs; }, SettingsScope::SaveOnly);
        Text(kRevealDelayUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow(kRowH);
        Text(kAutoHideNote, CtlKind::Note, L"The dock stays shown while this window is active.", kMargin, right - kMargin, 20);
        NextRow(20);

        Section(kSectionPreviews, L"Window previews");
        Check(kPreviews, kPreviewsCaption, L"Show window previews for apps with several windows");
        NextRow();
        Label(kPreviewDelayLabel, L"Preview delay");
        IntSpin(kPreviewDelayEdit, kPreviewDelaySpin, kColX, 90, 0, 10000, 50,
                [](Config& c) -> int& { return c.Main().previewDelayMs; }, SettingsScope::SaveOnly);
        Text(kPreviewDelayUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow();
    }

    void Load() override {
        const Config& c = Cfg();
        LoadInts();
        SetCheck(kAutoHideOff, c.Main().autoHide == AutoHideMode::Off);
        SetCheck(kAutoHideAlways, c.Main().autoHide == AutoHideMode::Always);
        SetCheck(kAutoHideIntelligent, c.Main().autoHide == AutoHideMode::Intelligent);
        SetCheck(kPreviews, c.Main().previews);
        EnableDependents();
    }

    void OnCommand(int id, int code, HWND) override {
        if (code != BN_CLICKED) return;
        Config& c = Cfg();
        switch (id) {
            case kAutoHideOff:
            case kAutoHideAlways:
            case kAutoHideIntelligent: {
                const AutoHideMode mode = id == kAutoHideOff      ? AutoHideMode::Off
                                          : id == kAutoHideAlways ? AutoHideMode::Always
                                                                  : AutoHideMode::Intelligent;
                if (!GetCheck(id) || mode == c.Main().autoHide) break;
                c.Main().autoHide = mode;
                EnableDependents();
                Changed(SettingsScope::AutoHide);
                break;
            }
            case kPreviews:
                c.Main().previews = GetCheck(id);
                EnableDependents();
                Changed(SettingsScope::SaveOnly);
                break;
            default:
                break;
        }
    }

private:
    void EnableDependents() {
        const Config& c = Cfg();
        Enable(kHideDelayEdit, c.Main().AutoHideOn());
        Enable(kRevealDelayEdit, c.Main().AutoHideOn());
        Enable(kPreviewDelayEdit, c.Main().previews);
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
    return pages;
}
