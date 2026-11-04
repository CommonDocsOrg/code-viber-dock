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
#include "Hotkey.h"
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

// --- the hotkey capture box (round 6 phase 6A: the dashboard hotkey) ---------------------------
// An edit control subclassed to record key combinations instead of text: focus it and press the
// combination (Win, Ctrl, Alt, Shift and a key); Backspace / Delete clear it (no hotkey). The
// canonical text is its window text; each new combination is reported to the page as WM_COMMAND
// with kHotkeyChanged. Not the common-controls hotkey control: that one cannot record Win. Plain
// Tab / Shift+Tab, Escape and Enter stay the window's (focus, close, commit). The registered
// combination itself never arrives as keys (Windows hands it to RegisterHotKey's window): App
// forwards it to the focused box (Hotkey::kCaptureMessage).
constexpr WORD kHotkeyChanged = 0x7A01;

void HotkeyBoxSet(HWND box, const std::wstring& text, bool notify) {
    SetWindowTextW(box, text.c_str());
    SendMessageW(box, EM_SETSEL, text.size(), text.size());
    if (notify) {
        SendMessageW(GetParent(box), WM_COMMAND, MAKEWPARAM(static_cast<WORD>(GetDlgCtrlID(box)), kHotkeyChanged),
                     reinterpret_cast<LPARAM>(box));
    }
}

LRESULT CALLBACK HotkeyBoxProc(HWND box, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    switch (msg) {
        case WM_GETDLGCODE: {
            const MSG* m = reinterpret_cast<const MSG*>(lp);
            if (m && (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN)) {
                const UINT mods = Hotkey::HeldModifiers();
                if ((m->wParam == VK_TAB && (mods & ~static_cast<UINT>(MOD_SHIFT)) == 0) ||
                    ((m->wParam == VK_ESCAPE || m->wParam == VK_RETURN) && mods == 0)) {
                    return 0;  // the window's keys
                }
            }
            return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const UINT vk = static_cast<UINT>(wp);
            if (vk == VK_BACK || vk == VK_DELETE) {
                HotkeyBoxSet(box, L"", true);
                return 0;
            }
            const UINT mods = Hotkey::HeldModifiers();
            if (Hotkey::IsModifierKey(vk)) {
                // Only modifiers so far: show them ("Win+Shift+…"); the key completes it.
                std::wstring partial;
                if (mods & MOD_WIN) partial += L"Win+";
                if (mods & MOD_CONTROL) partial += L"Ctrl+";
                if (mods & MOD_ALT) partial += L"Alt+";
                if (mods & MOD_SHIFT) partial += L"Shift+";
                SetPropW(box, L"WinDock.HotkeyPartial", reinterpret_cast<HANDLE>(1));
                SetWindowTextW(box, (partial + L"…").c_str());
                return 0;
            }
            const std::wstring text = Hotkey::Format(Hotkey::Combo{mods, vk});
            RemovePropW(box, L"WinDock.HotkeyPartial");
            if (text.empty()) {
                MessageBeep(MB_OK);  // a key without a name
                return 0;
            }
            HotkeyBoxSet(box, text, true);
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
            // Modifiers released without a key: back to the recorded combination.
            if (GetPropW(box, L"WinDock.HotkeyPartial") && Hotkey::HeldModifiers() == 0) {
                RemovePropW(box, L"WinDock.HotkeyPartial");
                SendMessageW(GetParent(box), WM_COMMAND, MAKEWPARAM(static_cast<WORD>(GetDlgCtrlID(box)), kHotkeyChanged + 1),
                             reinterpret_cast<LPARAM>(box));
            }
            return 0;
        case Hotkey::kCaptureMessage:
            // The registered hotkey itself, pressed while the box has the focus (App's WM_HOTKEY).
            RemovePropW(box, L"WinDock.HotkeyPartial");
            HotkeyBoxSet(box, Hotkey::Format(Hotkey::Combo{static_cast<UINT>(wp), static_cast<UINT>(lp)}), true);
            return 0;
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
        case WM_PASTE:
        case WM_CUT:
        case WM_CLEAR:
        case WM_UNDO:
        case WM_CONTEXTMENU:
            return 0;  // no typing, no editing menu
        case WM_NCDESTROY:
            RemovePropW(box, Hotkey::kCaptureProp);
            RemovePropW(box, L"WinDock.HotkeyPartial");
            RemoveWindowSubclass(box, &HotkeyBoxProc, id);
            break;
        default:
            break;
    }
    return DefSubclassProc(box, msg, wp, lp);
}

// Global > General: what applies to the whole app (startup, the taskbar, the renderer, the
// dashboard hotkey; round 8 phase 8A: the config file buttons are on About only).
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
        // Round 8 phase 8A: notes are one-line hints, the explanations tooltips.
        Label(kRendererLabel, L"Renderer", L"", kRendererNote);
        Tip(kRendererLabel, L"Takes effect after WinDock restarts. Automatic draws with the GPU (Direct3D 11) and falls "
                            L"back to software when that fails; Software (WARP) needs less memory.");
        Combo(kRendererCombo, kColX, right - kColX, false);
        NextRow();
        Text(kPositionNote, CtlKind::Note,
             L"Each dock's own settings are in its own window: right-click the dock > Dock settings…", kMargin,
             right - kMargin, 20);
        Tip(kPositionNote, L"Each dock's own settings (its items, position, behaviour, running apps and theme) are in its "
                           L"own window: right-click the dock and choose Dock settings…, or open it from Docks and displays.");
        NextRow(22);
        // Round 6 phase 6A.
        Section(kSectionDashboard, L"Dashboard");
        Label(kDashboardHotkeyLabel, L"Hotkey", L"Click, then press the keys", kDashboardNote);
        Tip(kDashboardHotkeyLabel,
            L"The hotkey brings every dock on the desktop layers (\"On the desktop\" in a dock's Position tab) to the "
            L"front, above every window. Press it again, press Escape, or switch to another window to put them back; "
            L"clicking the wallpaper keeps them up. Click the box and press the combination (Win, Ctrl, Alt, Shift and "
            L"a key); Backspace or Delete clears it (no hotkey).");
        EditBox(kDashboardHotkeyEdit, kColX, 220, 64);
        NextRow();
        Text(kDashboardStatus, CtlKind::Value, L"", kColX, right - kColX, 22);
        NextRow(24);
        Button(kDashboardShow, L"Show the dashboard now", kColX, 200);
        NextRow();
        // Round 8 phase 8A: the Configuration file section is gone (About has Open config file /
        // Reload config).
    }

    void Created() override {
        ComboAdd(kRendererCombo, L"Automatic (GPU, software if that fails)");
        ComboAdd(kRendererCombo, L"Software (WARP, less memory)");
        if (HWND box = Item(kDashboardHotkeyEdit)) {
            SetWindowSubclass(box, &HotkeyBoxProc, 1, 0);
            SetPropW(box, Hotkey::kCaptureProp, reinterpret_cast<HANDLE>(1));
            SendMessageW(box, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"None: click here and press a combination"));
        }
    }

    void Load() override {
        const Config& c = Cfg();
        SetCheck(kStartWithWindows, Host().autostartEnabled ? Host().autostartEnabled() : c.startWithWindows);
        SetCheck(kHideTaskbar, Host().taskbarHidden ? Host().taskbarHidden() : c.hideTaskbar);
        SetComboSel(kRendererCombo, c.renderer == L"software" ? 1 : 0);
        const bool warp = Host().usingWarp && Host().usingWarp();
        SetText(kRendererNote, std::wstring(L"After a restart · now ") + (warp ? L"software" : L"GPU"));
        SetText(kDashboardHotkeyEdit, c.dashboard.hotkey);
        LoadDashboardStatus();
    }

    void LoadDashboardStatus() {
        const Config& c = Cfg();
        const bool registered = Host().hotkeyRegistered && Host().hotkeyRegistered();
        SetText(kDashboardStatus, c.dashboard.hotkey.empty() ? std::wstring(L"No hotkey: use the tray menu's Show dashboard.")
                                  : registered              ? L"Registered: " + c.dashboard.hotkey + L" shows the dashboard."
                                                            : L"Not registered: another app uses " + c.dashboard.hotkey +
                                                                 L". Choose another combination.");
    }

    void OnCommand(int id, int code, HWND) override {
        Config& c = Cfg();
        switch (id) {
            case kDashboardHotkeyEdit:
                if (code == kHotkeyChanged) {
                    const std::wstring text = GetText(id);
                    if (text != c.dashboard.hotkey) {
                        c.dashboard.hotkey = text;
                        Changed(SettingsScope::Dashboard);  // App registers it (or none)
                    }
                    LoadDashboardStatus();
                } else if (code == kHotkeyChanged + 1) {
                    SetText(id, c.dashboard.hotkey);  // modifiers released without a key
                }
                break;
            case kDashboardShow:
                if (code == BN_CLICKED && Host().command) Host().command(ContextMenu::kEnterDashboard);
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
        // Round 8 phase 8A: the notes are one-line hints (kLayerNote, kCircleNote, kAlignNote,
        // kPositionNote, kMaxLengthNote, kPositionNote2 are the hints' ids) and tooltips.
        Label(kLayerLabel, L"Layer", L"", kLayerNote);
        Tip(kLayerLabel,
            L"Normal: above every window; it auto-hides as set on the Behaviour tab. On the desktop, only while the "
            L"desktop is showing: shown only while the desktop is in front (Win+D, a click on the wallpaper), at the "
            L"bottom of the windows. On the desktop, under every window: always on the desktop, like folder windows. "
            L"The dashboard hotkey brings the desktop layers to the front.");
        Combo(kLayerCombo, kColX, right - kColX, false);
        NextRow();

        Section(kSectionPosition, L"Position");
        // Round 6 phase 6A: a floating bar at x / y (the circle's keys) instead of on an edge.
        Check(kFloating, kFloatingCaption, L"Floating (place the bar anywhere on the display)");
        NextRow(kRowH);
        Text(kFloatingNote, CtlKind::Note, L"Drag its bar's background to move it. Bar shape only.", kMargin + kCaptionDx,
             right - kMargin - kCaptionDx, 20);
        static constexpr wchar_t kFloatingTip[] =
            L"A floating dock faces its screen edge (that is where its icons grow and popups open) and never "
            L"auto-hides: its layer and the dashboard hotkey show it. Drag its bar's background to move it (it snaps "
            L"to the work area's edges and other floating docks) unless it is locked. Bar shape only.";
        Tip(kFloatingCaption, kFloatingTip);
        Tip(kFloatingNote, kFloatingTip);
        NextRow(22);
        Label(kEdgeLabel, L"Screen edge (floating: faces)");
        Combo(kEdgeCombo, kColX, 160, false);
        NextRow();
        Label(kCircleXLabel, L"Position x", L"DIPs; empty: centred", kCircleNote);
        Tip(kCircleXLabel, L"Position (DIPs from the work area's top-left; empty = centred): a floating bar's top-left "
                           L"corner, a circle's centre. Ctrl+drag a circle to move it.");
        EditBox(kCircleXEdit, kColX, 88, 7);
        NextRow();
        Label(kCircleYLabel, L"Position y");
        EditBox(kCircleYEdit, kColX, 88, 7);
        NextRow();
        // Round 7 phase 7B: items[].anchor (the Contents tab's "Position on the dock").
        Label(kAlignLabel, L"Alignment", L"Where the dock sits", kAlignNote);
        Tip(kAlignLabel, L"Where the dock sits on its edge; with full width or anchored items, where the items without a "
                         L"position of their own sit.");
        Combo(kAlignCombo, kColX, 160, false);
        NextRow();
        Label(kOffsetLabel, L"Offset (pixels)", L"Along the edge", kPositionNote);
        Tip(kOffsetLabel, L"Moves the dock along its edge from the aligned place (right / down). Ctrl+drag the dock to "
                          L"move it along its edge or onto another edge (unless it is locked).");
        IntSpin(kOffsetEdit, kOffsetSpin, kColX, 88, -16000, 16000, 10, [](Config&, DockConfig& d) -> int& { return d.offset; },
                SettingsScope::Appearance);
        NextRow();
        Check(kFullWidth, kFullWidthCaption, L"Full width (the bar spans the whole edge)");
        NextRow();
        // Round 7 phase 7A: docks[].maxLength (a bar that is not full width).
        Label(kMaxLengthLabel, L"Maximum length (pixels)", L"0: no limit", kMaxLengthNote);
        Tip(kMaxLengthLabel, L"0: no limit. A longer run of icons scrolls (see When the icons do not fit). Not for a "
                             L"ring or a full-width bar.");
        Spin(kMaxLengthEdit, kMaxLengthSpin, kColX, 88, 0, 8000, 50);
        NextRow();
        Check(kReserveSpace, kReserveSpaceCaption, L"Reserve screen space (maximized windows stop at the dock)");
        Tip(kReserveSpaceCaption, L"Keeps maximized windows off the dock: reserves a strip along the whole edge (auto-hide "
                                  L"off only).");
        NextRow();
        Label(kPeekLabel, L"Peek when hidden (pixels)", L"0: off");
        Tip(kPeekLabel, L"Keeps that much of the hidden dock on screen (0: off).");
        IntSpin(kPeekEdit, kPeekSpin, kColX, 72, 0, 64, 2, [](Config&, DockConfig& d) -> int& { return d.peekPx; },
                SettingsScope::Appearance);
        NextRow();
        Label(kPeekOpacityLabel, L"Peek opacity (%)");
        Spin(kPeekOpacityEdit, kPeekOpacitySpin, kColX, 72, 5, 100, 5);
        NextRow();
        Label(kTitleLabel, L"Title", L"Drawn beside the bar", kPositionNote2);
        Tip(kTitleLabel, L"The title is drawn beside the bar, toward the middle of the screen (empty: none).");
        EditBox(kTitleEdit, kColX, right - kColX, 128);
        NextRow();

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
        Text(kAutoHideNote, CtlKind::Note, L"The dock stays shown while this window (or one of its item windows) is active.",
             kMargin, right - kMargin, 20);
        NextRow(22);
        // Round 8 phase 8A: the delays behind an expander.
        Expander(kAutoHideMore, L"More: hide and reveal delays");
        Label(kHideDelayLabel, L"Hide delay", L"After the mouse leaves");
        IntSpin(kHideDelayEdit, kHideDelaySpin, kColX, 90, 0, 10000, 50,
                [](Config&, DockConfig& d) -> int& { return d.hideDelayMs; }, SettingsScope::SaveOnly);
        Text(kHideDelayUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow();
        Label(kRevealDelayLabel, L"Reveal delay", L"Before it shows again");
        IntSpin(kRevealDelayEdit, kRevealDelaySpin, kColX, 90, 0, 10000, 50,
                [](Config&, DockConfig& d) -> int& { return d.revealDelayMs; }, SettingsScope::SaveOnly);
        Text(kRevealDelayUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow();
        EndExpander();

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

        // Round 7 phase 7A: docks[].popupGap (read when a popup opens).
        Section(kSectionPopups, L"Popups");
        // (The label column is 196 DIPs: "Gap between the dock and its popups" did not fit there.)
        Label(kPopupGapLabel, L"Popup gap");
        IntSpin(kPopupGapEdit, kPopupGapSpin, kColX, 90, 0, 64, 1,
                [](Config&, DockConfig& d) -> int& { return d.popupGap; }, SettingsScope::SaveOnly);
        Text(kPopupGapUnit, CtlKind::Note, L"DIP between the dock and its popups", kColX + 98, right - kColX - 98);
        NextRow();

        Section(kSectionMouse, L"Mouse");
        Label(kMiddleClickLabel, L"Middle click on an app");
        Combo(kMiddleClickCombo, kColX, right - kColX, false);
        NextRow();
        Label(kWheelLabel, L"Mouse wheel over an app");
        Combo(kWheelCombo, kColX, right - kColX, false);
        NextRow();

        // Round 5 phase D: this dock's part of the notifications (moved from Global > Notifications).
        // The notification centre itself is a widget: drag it from the Contents tab's drawer (no
        // check box here since round 5 phase E).
        Section(kSectionDockNotifications, L"Notifications");
        Check(kBadges, kBadgesCaption, L"Notification badges on app icons");
        NextRow(kRowH);
        Text(kDockNotificationsNote, CtlKind::Note,
             L"The notification centre is a widget: drag it onto the dock from the Contents tab.", kMargin, right - kMargin, 20);
        static constexpr wchar_t kBadgesTip[] =
            L"Badges: the unread count of each app's notifications on its icon. The notification centre (a bell with the "
            L"list) is a widget: drag it onto the dock from the Contents tab. Which apps may badge, tray balloons and the "
            L"poll interval apply to every dock: Global settings > Notifications.";
        Tip(kBadgesCaption, kBadgesTip);
        Tip(kDockNotificationsNote, kBadgesTip);
        NextRow(22);
    }

    void BuildRunning() {
        const int right = Width() - kMargin;
        Title(L"Running apps");
        Section(kSectionTaskList, L"Running apps");
        // Round 8 phase 8A: the notes are the hints kTaskListNote / kThumbnailsNote and tooltips.
        Label(kWindowsLabel, L"Windows", L"A slot per app or per window", kTaskListNote);
        Tip(kWindowsLabel, L"One slot per window: a pinned app's icon is its first window and its other windows follow it.");
        Combo(kWindowsCombo, kColX, right - kColX, false);
        NextRow();
        Check(kRunningMonitorOnly, kRunningMonitorOnlyCaption, L"Show only the windows on this dock's display");
        NextRow();
        Label(kTitlesLabel, L"Titles under icons");
        Tip(kTitlesLabel, L"Titles show the window title, or the app's name.");
        Combo(kTitlesCombo, kColX, right - kColX, false);
        NextRow();
        Label(kTitlesVerticalLabel, L"Titles on vertical docks");
        Combo(kTitlesVerticalCombo, kColX, right - kColX, false);
        NextRow();
        Label(kThumbnailsLabel, L"Window thumbnails", L"Live: GPU · snapshots: cheap", kThumbnailsNote);
        Tip(kThumbnailsLabel,
            L"Live uses the GPU to show windows as they change (only while the dock is visible, at most the frames per "
            L"second below). Snapshots takes a picture every N seconds and costs almost nothing in between. Each window "
            L"gets a tile (stacked: each app its active window).");
        Combo(kThumbnailsCombo, kColX, right - kColX, false);
        NextRow();
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
        ComboAdd(kLayerCombo, L"Normal (above windows)");  // DockLayer order
        ComboAdd(kLayerCombo, L"On the desktop, only while the desktop is showing");
        ComboAdd(kLayerCombo, L"On the desktop, under every window");
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
        SetText(kAutoHideNote, d.Floating() ? L"A floating dock is shown by its layer and the dashboard hotkey (no auto-hide)."
                                            : L"The dock stays shown while this window (or one of its item windows) is active.");
        SetCheck(kPreviews, d.previews);
        SetCheck(kPreviewSingle, d.previewSingle);
        SetComboSel(kMiddleClickCombo, static_cast<int>(d.middleClick));
        SetComboSel(kWheelCombo, static_cast<int>(d.wheel));
        SetCheck(kBadges, d.badges);
        // Running apps
        SetCheck(kRunningMonitorOnly, d.runningFilter == RunningFilter::Monitor);
        SetComboSel(kWindowsCombo, static_cast<int>(d.windows));
        SetComboSel(kTitlesCombo, static_cast<int>(d.titles));
        SetComboSel(kTitlesVerticalCombo, static_cast<int>(d.titlesVertical));
        SetComboSel(kThumbnailsCombo, static_cast<int>(d.thumbnails));
        // Position
        if (part_ == Part::Position) FillMonitors();
        SetComboSel(kLayerCombo, static_cast<int>(d.layer));
        SetText(kLayerNote, LayerNote(d.layer));
        SetCheck(kFloating, d.Floating());
        SetComboSel(kEdgeCombo, static_cast<int>(d.edge));
        SetComboSel(kAlignCombo, static_cast<int>(d.align));
        SetComboSel(kOverflowCombo, static_cast<int>(d.overflow));
        SetComboSel(kShapeCombo, static_cast<int>(d.shape));
        SetCheck(kRotateIcons, d.rotateIcons);
        SetText(kCircleXEdit, d.x == DockConfig::kCentred ? std::wstring() : std::to_wstring(d.x));
        SetText(kCircleYEdit, d.y == DockConfig::kCentred ? std::wstring() : std::to_wstring(d.y));
        SetCheck(kFullWidth, d.fullWidth);
        SetSpin(kMaxLengthSpin, d.maxLength);
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
                    if (sel <= 2 && static_cast<DockLayer>(sel) != d.layer) {
                        d.layer = static_cast<DockLayer>(sel);
                        SetText(kLayerNote, LayerNote(d.layer));
                        EnableDependents();
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
                        if (d.shape != DockShape::Bar && d.floating) {
                            d.floating = false;  // a ring is never floating (Config::Sanitize)
                            SetCheck(kFloating, false);
                        }
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
                EnableDependents();  // Maximum length is greyed for a full-width bar
                Changed(SettingsScope::Appearance);
                break;
            case kFloating: {
                const bool on = GetCheck(id) && d.shape == DockShape::Bar;
                if (on == d.Floating()) break;
                d.floating = on;
                if (on) d.autoHide = AutoHideMode::Off;  // shown by its layer and the dashboard (6A)
                EnableDependents();
                Changed(SettingsScope::Appearance);  // placed at x / y (or back on its edge)
                break;
            }
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
            case kBadges:
                d.badges = GetCheck(id);
                Changed(SettingsScope::Notifications);  // the service starts / stops with the demand
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
        if (id == kMaxLengthEdit) {
            int v = 0;
            if (ReadInt(id, 0, 8000, v)) {
                SetMaxLength(v, false);
            } else {
                SetSpin(kMaxLengthSpin, d.maxLength);  // not a number: back to the current value
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
        if (spin == kMaxLengthSpin) SetMaxLength(pos, true);
    }

private:
    // The dock this page edits (the window's, looked up by id each time).
    DockConfig& Bound() { return Dock(); }

    // The Layer label's hint (round 6 phase 6A; round 8 phase 8A: one line, the tooltip explains).
    static const wchar_t* LayerNote(DockLayer layer) {
        switch (layer) {
            case DockLayer::Desktop: return L"Only while the desktop shows";
            case DockLayer::Behind: return L"Under every window";
            case DockLayer::Normal:
            default: return L"Above every window";
        }
    }

    void SetPeekOpacity(int percent) {
        DockConfig& d = Bound();
        percent = std::clamp(percent, 5, 100);
        SetSpin(kPeekOpacitySpin, percent);
        const double v = percent / 100.0;
        if (std::fabs(d.peekOpacity - v) < 1e-9) return;
        d.peekOpacity = v;
        Changed(SettingsScope::Appearance);
    }

    // docks[].maxLength (round 7 phase 7A): 0 = no limit, else 96 - 8000 (Config::Sanitize). A value
    // between: typed, the minimum; stepped by the spinner, the step leaves 0 for 96 and 96 for 0.
    void SetMaxLength(int value, bool spun) {
        DockConfig& d = Bound();
        constexpr int kMin = 96;
        if (value > 0 && value < kMin) value = spun && d.maxLength > 0 && d.maxLength <= kMin ? 0 : kMin;
        value = std::clamp(value, 0, 8000);
        SetSpin(kMaxLengthSpin, value);
        if (value == d.maxLength) return;
        d.maxLength = value;
        Changed(SettingsScope::Appearance);  // relayout: the bar's room along the edge
    }

    void EnableDependents() {
        const DockConfig& d = Bound();
        // Radius and Rotate icons apply to the ring shapes only; the centre to a circle.
        for (int id : {kRadiusLabel, kRadiusEdit, kRadiusSpin, kRotateIcons, kRotateIconsCaption}) {
            if (HWND h = Item(id)) EnableWindow(h, d.Polar());
        }
        // x / y: a circle's centre or a floating bar's top-left (round 6 phase 6A).
        const bool floating = d.Floating();
        for (int id : {kCircleXLabel, kCircleXEdit, kCircleYLabel, kCircleYEdit}) {
            if (HWND h = Item(id)) EnableWindow(h, d.shape == DockShape::Circle || floating);
        }
        // Floating: a bar only; alignment, offset, full width, reserve space and peek do not apply.
        for (int id : {kFloating, kFloatingCaption}) {
            if (HWND h = Item(id)) EnableWindow(h, d.shape == DockShape::Bar);
        }
        for (int id : {kAlignLabel, kAlignCombo, kOffsetLabel, kOffsetEdit, kOffsetSpin, kFullWidth, kFullWidthCaption,
                       kReserveSpace, kReserveSpaceCaption, kPeekLabel, kPeekEdit, kPeekSpin, kPeekOpacityLabel,
                       kPeekOpacityEdit, kPeekOpacitySpin}) {
            if (HWND h = Item(id)) EnableWindow(h, !floating);
        }
        // Maximum length (round 7 phase 7A): a bar that is not full width (a floating bar too).
        const bool capped = !d.Polar() && !(d.fullWidth && !floating);
        for (int id : {kMaxLengthLabel, kMaxLengthEdit, kMaxLengthSpin, kMaxLengthNote}) {
            if (HWND h = Item(id)) EnableWindow(h, capped);
        }
        // Auto-hide: off for a floating dock (Config::Sanitize).
        for (int id : {kAutoHideOff, kAutoHideOffCaption, kAutoHideAlways, kAutoHideAlwaysCaption, kAutoHideIntelligent,
                       kAutoHideIntelligentCaption}) {
            if (HWND h = Item(id)) EnableWindow(h, !floating);
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
        NextRow(22);

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
            if (window.IsWidgetWindow()) {
                pages.push_back(MakeWidgetWindowPage(window));  // kTabWidgetWindow (round 6 phase 6B)
                break;
            }
            pages.push_back(MakeContentsPage(window));                                                    // kTabContents
            pages.push_back(std::make_unique<DockFieldsPage>(window, DockFieldsPage::Part::Position));   // kTabPosition
            pages.push_back(std::make_unique<DockFieldsPage>(window, DockFieldsPage::Part::Behaviour));  // kTabBehaviour
            pages.push_back(std::make_unique<DockFieldsPage>(window, DockFieldsPage::Part::Running));    // kTabRunning
            pages.push_back(MakeDockThemePage(window));                                                  // kTabTheme
            break;
        case SettingsKind::Item: {
            // A widget, clock or weather item: its generated schema page; a tray: its grid (round 5
            // phase D); a folder / group: Folders and groups.
            const DockItemEntry* e = window.TargetItem();
            const ItemKind k = e ? e->Kind() : ItemKind::Unknown;
            if (k == ItemKind::Widget || k == ItemKind::Clock || k == ItemKind::Weather) {
                pages.push_back(MakeWidgetSettingsPage(window));
            } else if (k == ItemKind::Tray) {
                pages.push_back(MakeTrayItemPage(window));
            } else {
                pages.push_back(MakeItemSettingsPage(window));
            }
            break;
        }
    }
    return pages;
}