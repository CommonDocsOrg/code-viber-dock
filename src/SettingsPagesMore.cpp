// The Animation page, and the icon of a dock item as the settings windows show it (round 5 phase
// C: the Dock contents list that lived here became the Contents tab, SettingsPagesContents.cpp).

#include <windows.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>

#include "IconUtil.h"
#include "SettingsForm.h"
#include "SettingsIds.h"

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
        HelpTopic(L"guide-settings.md#animation");
        Section(kSectionSpeed, L"Speed", L"guide-settings.md#speed");
        // Round 8 phase 8A: the note is the label's hint (kSpeedNote) and its tooltip.
        Label(kSpeedLabel, L"Animation speed", L"Off: no animation", kSpeedNote);
        Tip(kSpeedLabel, L"Off: the dock moves instantly. The speed divides every animation's duration.");
        Trackbar(kSpeedTrack, kColX - 6, 212, 0, kSpeedSteps, 4);
        Text(kSpeedValue, CtlKind::Value, L"", kColX + 216, 72);
        NextRow();

        Section(kSectionHide, L"Hiding and showing", L"guide-settings.md#hiding-and-showing");
        Radios({
            {kHideSlide, kHideSlideCaption, L"Slide and fade"},
            {kHideSlideOnly, kHideSlideOnlyCaption, L"Slide"},
            {kHideFade, kHideFadeCaption, L"Fade"},
            {kHideNone, kHideNoneCaption, L"None (instant)"},
        });
        Tip(kHideSlideCaption, L"How an auto-hiding dock goes away and comes back: it slides off its edge while it fades (the default).");
        Tip(kHideSlideOnlyCaption, L"The dock slides off its edge without fading.");
        Tip(kHideFadeCaption, L"The dock fades out where it is, without moving.");
        Tip(kHideNoneCaption, L"The dock disappears and appears at once.");

        Section(kSectionLaunch, L"Launching an app", L"guide-settings.md#launching-an-app");
        Radios({
            {kLaunchBounce, kLaunchBounceCaption, L"Bounce until the app opens"},
            {kLaunchHop, kLaunchHopCaption, L"Hop once"},
            {kLaunchPulse, kLaunchPulseCaption, L"Pulse"},
            {kLaunchNone, kLaunchNoneCaption, L"None"},
        });
        Tip(kLaunchBounceCaption, L"A clicked app's icon bounces until its window opens (at most a few seconds). The default.");
        Tip(kLaunchHopCaption, L"The icon hops once when you click it.");
        Tip(kLaunchPulseCaption, L"The icon pulses (grows and shrinks) once when you click it.");
        Tip(kLaunchNoneCaption, L"No launch animation.");

        Section(kSectionMotion, L"Motion", L"guide-settings.md#motion");
        Label(kFalloffLabel, L"Magnification falloff");
        Tip(kFalloffLabel, L"How the magnification fades from the icon under the pointer to its neighbours: smooth (cosine, the default) or linear.");
        Combo(kFalloffCombo, kColX, 220, false);
        NextRow();
        Label(kEasingLabel, L"Easing");
        Tip(kEasingLabel, L"The feel of the dock's movements: ease out (fast, then slowing down; the default), linear, or spring (a small overshoot).");
        Combo(kEasingCombo, kColX, 220, false);
        NextRow();
        Label(kLabelFadeLabel, L"Name label fade");
        Tip(kLabelFadeLabel, L"How long the app's name label above the icon takes to fade in and out (0: at once). Default: 120 ms.");
        IntSpin(kLabelFadeEdit, kLabelFadeSpin, kColX, 90, 0, 1000, 20,
                [](Config& c, DockConfig&) -> int& { return c.animation.labelFadeMs; }, SettingsScope::Appearance);
        Text(kLabelFadeUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow();

        Section(kSectionAccessibility, L"Accessibility", L"guide-settings.md#accessibility");
        Label(kReduceMotionLabel, L"Reduce motion", L"", kReduceMotionNote);
        Combo(kReduceMotionCombo, kColX, right - kColX, false);
        NextRow();
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
        SetText(kReduceMotionNote, std::wstring(L"Windows animations: ") + (SystemAnimationsOn() ? L"on" : L"off"));
    }
};

}  // namespace

std::unique_ptr<SettingsPage> MakeAnimationPage(SettingsWindow& window) {
    return std::make_unique<AnimationPage>(window);
}

namespace {

// SHGetFileInfoW's icon of `path` (a PIDL with SHGFI_PIDL in `flags`) at about `px`: the small /
// large shell icon up to 32 px, above that the system image list's 48 px icon (SHIL_EXTRALARGE),
// which DrawIconEx scales down without the blur of a 32 px icon scaled up.
HICON ShellIcon(LPCWSTR path, DWORD attributes, UINT flags, int px) {
    SHFILEINFOW sfi{};
    if (px <= 32) {
        const UINT size = px > GetSystemMetrics(SM_CXSMICON) ? SHGFI_LARGEICON : SHGFI_SMALLICON;
        return SHGetFileInfoW(path, attributes, &sfi, sizeof(sfi), flags | SHGFI_ICON | size) ? sfi.hIcon : nullptr;
    }
    if (!SHGetFileInfoW(path, attributes, &sfi, sizeof(sfi), flags | SHGFI_SYSICONINDEX)) return nullptr;
    winrt::com_ptr<IImageList> list;
    HICON icon = nullptr;
    if (SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(list.put())))) list->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon);
    if (icon) return icon;
    return SHGetFileInfoW(path, attributes, &sfi, sizeof(sfi), flags | SHGFI_ICON | SHGFI_LARGEICON) ? sfi.hIcon : nullptr;
}

}  // namespace

// The icon of a pin entry as the Contents tab's strip (and an Item window's header) shows it.
HICON SettingsItemIcon(const AppIdentity::ResolvedPin* pin, const DockItemEntry& e, int px) {
    const ItemKind special = e.Kind();
    if (special == ItemKind::Separator) return nullptr;  // no icon: the name says "Separator"
    if (special == ItemKind::Group && (!pin || pin->iconFile.empty())) return nullptr;  // the dock composes its icon
    HICON icon = nullptr;
    // The custom icon (or a shortcut's icon location) first: an image, else an icon resource.
    if (pin && !pin->iconFile.empty()) {
        if (IconUtil::IsImageFile(pin->iconFile)) {
            icon = IconUtil::HiconFromImageFile(pin->iconFile, px);
        } else {
            SHDefExtractIconW(pin->iconFile.c_str(), pin->iconIndex, 0, &icon, nullptr,
                              MAKELONG(px, px));
        }
    }
    if (!icon && special == ItemKind::RecycleBin) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, &pidl)) && pidl) {
            icon = ShellIcon(reinterpret_cast<LPCWSTR>(pidl), 0, SHGFI_PIDL, px);
            CoTaskMemFree(pidl);
        }
        if (!icon) return nullptr;
    }
    if (!icon && !e.path.empty()) {
        // A missing file: the generic icon of its type.
        icon = ShellIcon(e.path.c_str(), FILE_ATTRIBUTE_NORMAL, pin && pin->missing ? SHGFI_USEFILEATTRIBUTES : 0, px);
    }
    if (!icon && !e.aumid.empty()) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName((L"shell:AppsFolder\\" + e.aumid).c_str(), nullptr, &pidl, 0, nullptr))) {
            icon = ShellIcon(reinterpret_cast<LPCWSTR>(pidl), 0, SHGFI_PIDL, px);
            CoTaskMemFree(pidl);
        }
    }
    if (!icon) icon = ShellIcon(L".exe", FILE_ATTRIBUTE_NORMAL, SHGFI_USEFILEATTRIBUTES, px);
    return icon;
}
