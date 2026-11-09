// The look editor (round 12 phase 12B: one editor, two targets). App settings > Themes edits the
// app look: its preset and one row per Theme::Fields() entry; a field edited there is an override
// (Config::themeOverrides), saved inline in config.json on top of the preset until its Reset button
// (or "Reset all to preset", or another preset) clears it. A dock's Look tab (the Appearance tab
// until 12B) edits the dock's own look the same way (docks[].themePreset + docks[].theme with
// DockConfig::themeOverrides), or shows the app's values read-only while the dock uses the app look.
// Both start with the Quick look card (Theme::Derived) and choose presets in the gallery
// (SettingsGallery, a Keep / Cancel preview); their ⋯ menu saves, imports, exports and applies looks.

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

#include "InputDialog.h"
#include "SettingsForm.h"
#include "SettingsIds.h"
#include "SettingsPreview.h"
#include "Theme.h"

namespace {

using namespace SettingsIds;
using Theme::Field;
using Theme::Quick;

struct FieldGroup {
    int sectionId;
    const wchar_t* title;
    std::vector<Field> fields;
    int noteId = 0;                  // optional note under the rows
    const wchar_t* note = nullptr;   // its tooltip (round 8 phase 8A)
    const wchar_t* hint = nullptr;   // the one line shown
    const wchar_t* topic = nullptr;  // round 11 phase 11A: the card's "?" (null: the Theme values section)
};

// Rows in this order; a field missing here lands in "Other" (the table stays the source of truth).
std::vector<FieldGroup> FieldGroups() {
    std::vector<FieldGroup> groups = {
        // Round 11 phase 11A: Sheen, Gap between icons and Bar padding joined the Bar group.
        {kSectionFieldsBar, L"Bar",
         {Field::Background, Field::Acrylic, Field::AcrylicBackground, Field::CornerRadius, Field::BarBorder,
          Field::Sheen, Field::Separator, Field::Accent, Field::IconGap, Field::BarPadding, Field::Shelf,
          Field::ShelfSlant, Field::ShelfEdge}},
        {kSectionFieldsLabel, L"Name label",
         {Field::LabelBackground, Field::LabelText, Field::LabelBorder, Field::LabelCornerRadius}},
        {kSectionFieldsDots, L"Indicator dots", {Field::DotRunning, Field::DotActive, Field::DotAttention}},
        // Round 11 phase 11A: the taskbar-button look (its rows under Slot style collapse while it is None).
        {kSectionFieldsSlots, L"Slot buttons",
         {Field::SlotStyle, Field::SlotRunning, Field::SlotActive, Field::SlotAttention, Field::SlotHover,
          Field::SlotBorder, Field::SlotCornerRadius, Field::SlotHotTrack},
         kSlotsNote,
         L"Buttons draw a frame behind every app, folder, group and Recycle Bin slot instead of the indicator dots: "
         L"a running app's frame, a brighter one for the app you are using, an orange pulsing one for an app that "
         L"wants attention, outlines behind it for two or more windows. Hovering adds the Hover fill and, with the "
         L"light under the pointer, a glow in the icon's colour that follows the pointer. The frames do not grow with "
         L"magnification: they are meant for a dock with magnification 1.00\x00D7. Rings keep their dots.",
         L"Instead of the dots. Meant for magnification 1.00\x00D7; rings keep their dots.",
         L"guide-settings.md#slot-buttons"},
        {kSectionFieldsBadges, L"Notification badges", {Field::BadgeFill, Field::BadgeText, Field::BadgeSize}},
        {kSectionFieldsClock, L"Clock", {Field::ClockText, Field::ClockDateText}},
        {kSectionFieldsIcons, L"Placeholder icon", {Field::PlaceholderFill, Field::PlaceholderText}},
        {kSectionFieldsPreviews, L"Window previews",
         {Field::PreviewHotBackground, Field::PreviewHotBorder, Field::CloseHover}},
        {kSectionFieldsEffects, L"Icon effects",
         {Field::Glow, Field::GlowColor, Field::GlowRadius, Field::GlowStrength, Field::Reflection,
          Field::ReflectionClip, Field::ReflectionHeight, Field::ReflectionOpacity, Field::Perspective,
          Field::PerspectiveAngle},
         kEffectsNote,
         L"A clear glow colour (alpha 0) uses each icon's own colour. Reflections show on bottom and top docks "
         L"only (left / right docks have none); clipped, they keep the bar at its height and show the part "
         L"that fits. The perspective tilt is cosmetic (clicks use the flat layout) "
         L"and turns acrylic off, like the glass shelf.",
         L"Alpha 0 glow: the icon's colour. Reflections: top and bottom docks. The tilt turns acrylic off."},
    };
    FieldGroup other{kSectionFieldsOther, L"Other", {}};
    for (size_t i = 0; i < Theme::FieldCount(); ++i) {
        const Field f = Theme::Fields()[i].field;
        const bool listed = std::any_of(groups.begin(), groups.end(), [f](const FieldGroup& g) {
            return std::find(g.fields.begin(), g.fields.end(), f) != g.fields.end();
        });
        if (!listed) other.fields.push_back(f);
    }
    if (!other.fields.empty()) groups.push_back(std::move(other));
    return groups;
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

COLORREF RgbOf(uint32_t argb) { return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF); }

COLORREF Blend(COLORREF over, COLORREF under, unsigned alpha) {
    auto ch = [alpha](unsigned a, unsigned b) { return static_cast<BYTE>((a * alpha + b * (255 - alpha) + 127) / 255); };
    return RGB(ch(GetRValue(over), GetRValue(under)), ch(GetGValue(over), GetGValue(under)),
               ch(GetBValue(over), GetBValue(under)));
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::iswspace(s[i])) ++i;
    return s.substr(i);
}

// The folder part of a path (no trailing backslash).
std::wstring FolderOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring StemOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

void EnsureThemesFolder() {
    CreateDirectoryW(Config::Directory().c_str(), nullptr);
    CreateDirectoryW(Theme::Directory().c_str(), nullptr);
}

std::wstring ChangedCount(int n) {
    return n == 1 ? std::wstring(L"1 value changed") : std::to_wstring(n) + L" values changed";
}

std::wstring HexRgb(uint32_t rgb) {
    wchar_t buf[16];
    swprintf_s(buf, L"#%06X", static_cast<unsigned>(rgb & 0xFFFFFFu));
    return buf;
}

// "#RRGGBB" / "RRGGBB" (an #AARRGGBB value gives its RGB).
bool ParseRgb(std::wstring text, uint32_t& rgb) {
    text = Trim(std::move(text));
    if (!text.empty() && text[0] == L'#') text.erase(0, 1);
    if (text.size() != 6 && text.size() != 8) return false;
    uint32_t v = 0;
    for (wchar_t c : text) {
        v <<= 4;
        if (c >= L'0' && c <= L'9') v |= static_cast<uint32_t>(c - L'0');
        else if (c >= L'a' && c <= L'f') v |= static_cast<uint32_t>(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') v |= static_cast<uint32_t>(c - L'A' + 10);
        else return false;
    }
    rgb = v & 0xFFFFFFu;
    return true;
}

constexpr int kMaxTrackIconSize = 128;  // the spinner goes on to 256 (Config::Sanitize)
std::wstring FormatMagnification(int pos) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.2f×", pos / 20.0);
    return buf;
}

const wchar_t* const kCornerNames[] = {L"Square", L"Soft", L"Round", L"Mixed"};
const wchar_t* const kEffectNames[] = {L"Off", L"Glow", L"Reflection", L"Glow and reflection"};

// Global > Themes (the app look and its per-field overrides), or with `dockOnly` a Dock window's Look
// tab (round 8 phase 8B: Appearance, the Theme tab until then; round 12 phase 12B: Look): that
// dock's look (its own, or the app's read-only), the gallery and the dock's Size (icon size,
// magnification and its range: the Behaviour tab's Size card until 8B).
class AppearancePage : public FormPage {
public:
    AppearancePage(SettingsWindow& w, bool dockOnly)
        : FormPage(w, dockOnly ? L"Look" : L"Themes", L'\xE790'), dockOnly_(dockOnly),
          rowBase_(dockOnly ? kDockFieldRowBase : kFieldRowBase), colX_(dockOnly ? kColX : kThemeColX) {}

protected:
    void Build() override {
        SettingsGallery::Register(GetModuleHandleW(nullptr));
        const int right = Width() - kMargin;
        if (dockOnly_) {
            Title(L"Look");
            HelpTopic(L"guide-settings.md#look-tab");
            // Round 10 phase 10D: one first card with the common controls (the look, icon size,
            // magnification; the Size card of 8B joined it), the magnification's tuning in a closed
            // heading expander under it. Round 12 phase 12B: the look is chosen in the gallery (the
            // combo stays, hidden, for automation), its ⋯ menu beside the ↺.
            Section(kSectionTheme, L"Look and size", L"guide-settings.md#this-docks-look");
            Label(kDockThemeLabel, L"This dock's look", L"The app look", kDockThemeNote);
            Tip(kDockThemeLabel, L"A dock either uses the app look (App settings > Themes: every dock that has no look of "
                                 L"its own) or has its own: a theme from the gallery below plus your changes on this tab, "
                                 L"which then apply to this dock only. \x21BA (Use the app look again) drops its own look.");
            Combo(kDockThemeCombo, colX_, right - kRevertW - 8 - kMoreW - 8 - colX_, false);
            Button(kThemeMore, L"\x22EF", right - kRevertW - 8 - kMoreW, kMoreW);
            Tip(kThemeMore, L"Save as theme\x2026, Import theme\x2026, Export theme\x2026, Apply to other docks\x2026, "
                            L"Open themes folder, Reset all to preset");
            Button(kDockThemeReset, L"\x21BA", right - kRevertW, kRevertW);
            Tip(kDockThemeReset, L"Use the app look again: this dock drops its own look and follows App settings > Themes.");
            NextRow();
            GalleryRow(kGalleryDock, L"Choose a look for this dock. The first tile is the app look; the others give this "
                                     L"dock a look of its own. A choice is a preview: Keep or Cancel it in the bar above.");
            Label(kIconSizeLabel, L"Icon size");
            Tip(kIconSizeLabel, L"The size of the icons at rest, in DIPs (16 - 256; the slider goes to 128). Default: 48.");
            Trackbar(kIconSizeTrack, kColX - 6, 212, 16, kMaxTrackIconSize, 8);
            IntSpin(kIconSizeEdit, kIconSizeSpin, kColX + 216, 72, 16, 256, 4,
                    [](Config&, DockConfig& d) -> int& { return d.iconSize; }, SettingsScope::Appearance);
            NextRow();
            Label(kMagnifyLabel, L"Magnification");
            Tip(kMagnifyLabel, L"How much the icon under the pointer grows (1.00\x00D7: no magnification, up to 2.50\x00D7). Default: 1.50\x00D7.");
            Trackbar(kMagnifyTrack, kColX - 6, 212, 20, 50, 2);
            Text(kMagnifyValue, CtlKind::Value, L"", kColX + 216, 72);
            NextRow();
            // Round 10 phase 10D: the magnification's tuning, closed at first.
            UntitledCard();
            HeadingExpander(kMagnifyMore, L"Magnification details", false, L"guide-settings.md#size");
            Label(kRangeLabel, L"Magnification range (icons)");
            Tip(kRangeLabel, L"How many icons on each side of the pointer grow too (1 - 4). Default: 2.");
            IntSpin(kRangeEdit, kRangeSpin, kColX, 72, 1, 4, 1, [](Config&, DockConfig& d) -> int& { return d.magnifyRange; },
                    SettingsScope::Appearance);
            NextRow();
            // Round 9 phase 9A: docks[].magnifyBar.
            Label(kMagnifyBarLabel, L"Bar while magnified");
            Tip(kMagnifyBarLabel,
                L"Grows once and stays (the default): when the pointer first reaches something that zooms (an app, a "
                L"folder, a group, the Recycle Bin) the bar grows to its magnified length at once, evenly at both ends, "
                L"and keeps that length until the pointer leaves the dock (menus and popups keep it too): no wobble left "
                L"and right, the widgets and separators stay where they are and only the magnified icons move. Follows "
                L"the pointer: the bar grows away from the pointer and its ends move as you go along the icons (the older "
                L"behaviour). Always wide: the bar always has its magnified length (room for the magnified icons at both "
                L"ends of the icons), so nothing grows or shrinks. Rings, a scrolling dock and a full-width bar always "
                L"grow with the pointer.");
            Combo(kMagnifyBarCombo, kColX, 252, false);  // the longest entry: "Always wide (...)"
            NextRow();
            EndExpander();
            // Round 12 phase 12B: why the values below are greyed while the dock uses the app look,
            // and the two ways on (collapsed while the dock has its own look).
            UntitledCard(L"guide-settings.md#this-docks-look");
            Ctl& reason = Text(kLookReason, CtlKind::Note, L"This dock uses the app look. Changes to it are in App settings > Themes.",
                               kMargin, right - kMargin, 20);
            reason.maxLines = 2;
            Tip(kLookReason, L"The values below are the app's (read-only here). Give this dock its own look to change them for "
                             L"this dock only, or change them for every dock that uses the app look in App settings > Themes.");
            NextRow(24);
            Button(kOwnLook, L"Give this dock its own look", kMargin, 220);
            Tip(kOwnLook, L"Copies the app look (its theme and your changes) to this dock as its own starting point. From then "
                          L"on the changes on this tab apply to this dock only. \x21BA beside This dock's look goes back.");
            Link(kDockThemeLink, L"<a>Change for every dock</a>", kMargin + 236, right - kMargin - 236);
            Tip(kDockThemeLink, L"Opens App settings > Themes: the look of every dock that uses the app look.");
            NextRow();
            QuickCard();
            FieldCards();
            return;
        }
        Title(L"Themes");
        HelpTopic(L"guide-settings.md#themes");
        // Round 8 phase 8A: a narrower label column, so a field row (swatch, #AARRGGBB, alpha,
        // Reset) fits the window's minimum width beside the scroll bar.
        labelW_ = kThemeLabelW;
        // Round 12 phase 12B: Quick look first (open), then the Theme card with the gallery.
        QuickCard();
        Section(kSectionTheme, L"Theme", L"guide-settings.md#choosing-a-theme");
        // Round 8 phase 8D: the hint counts the changes; the "⋯" menu has the theme files.
        Label(kThemeLabel, L"Theme", L"The preset's values", kThemeNote);
        Tip(kThemeLabel, L"A change on this page overrides the theme (saved in config.json); \x21BA brings back the theme's value, "
                         L"\x22EF > Reset all to preset every one. Colours are #AARRGGBB, the number next to them is the "
                         L"alpha (0 clear, 255 opaque). This is the app look: every dock that has no look of its own (its "
                         L"Dock settings > Look) shows it.");
        Combo(kThemeCombo, colX_, right - kMoreW - 8 - colX_, false);  // hidden: the gallery replaces it (automation keeps it)
        Button(kThemeMore, L"\x22EF", right - kMoreW, kMoreW);
        Tip(kThemeMore, L"Save as theme\x2026, Import theme\x2026, Export theme\x2026, Apply to docks\x2026, Open themes folder, "
                        L"Reset all to preset");
        NextRow();
        GalleryRow(kGalleryApp, L"Choose the app look: the built-in themes, then your theme files. A choice is a preview: Keep "
                                L"or Cancel it in the bar above. The tile of the look in use is framed and says how many of its "
                                L"values you changed.");
        FieldCards();
    }

    void Created() override {
        if (HWND g = Item(GalleryId())) gallery_.Attach(g);
        Show(dockOnly_ ? kDockThemeCombo : kThemeCombo, false);  // round 12 phase 12B: the gallery shows the choice
        for (const wchar_t* n : kCornerNames) ComboAdd(Q(kQuickCornersCombo), n);
        for (const wchar_t* n : kEffectNames) ComboAdd(Q(kQuickEffectsCombo), n);
        SendMessageW(Item(Q(kQuickCornersCombo)), CB_DELETESTRING, 3, 0);  // "Mixed" comes back only while it applies
        if (!dockOnly_) return;
        ComboAdd(kMagnifyBarCombo, L"Grows once and stays");  // MagnifyBar order
        ComboAdd(kMagnifyBarCombo, L"Follows the pointer");
        ComboAdd(kMagnifyBarCombo, L"Always wide (never grows or shrinks)");
    }

    void Load() override {
        if (HWND g = Item(GalleryId())) gallery_.Attach(g);
        GalleryColors();
        FillThemes();
        if (dockOnly_) {
            // The Size card (round 8 phase 8B: from the Behaviour tab).
            const DockConfig& d = Dock();
            LoadInts();
            SetTrack(kIconSizeTrack, std::min(d.iconSize, kMaxTrackIconSize));
            const int mag = static_cast<int>(std::lround(d.magnification * 20.0));
            SetTrack(kMagnifyTrack, mag);
            SetText(kMagnifyValue, FormatMagnification(mag));
            SetComboSel(kMagnifyBarCombo, static_cast<int>(d.magnifyBar));
            UpdateDockCounts();
            const bool own = d.OwnLook();
            Collapse({kLookReason, kOwnLook, kDockThemeLink}, own);
            Show(kDockThemeReset, own);
        }
        LoadValues();
    }

    void OnStyleChanged() override { GalleryColors(); }

    int MeasureCustomDip(int id, int widthDip) const override {
        if (id != GalleryId()) return -1;
        return SettingsGallery::HeightDip(std::max(1, gallery_.Count()), widthDip);
    }

    bool Query(int code, LPARAM, LRESULT& out) override {
        if (code == 301) {  // round 12 phase 12B: the gallery's selected tile
            out = gallery_.Selected();
            return true;
        }
        if (code == 302) {  // ... its tile count
            out = gallery_.Count();
            return true;
        }
        return false;
    }

    void OnCommand(int id, int code, HWND) override {
        if (id == GalleryId()) {
            if (code == SettingsGallery::kChosen) PickTile(gallery_.Selected());
            return;
        }
        if (int field = 0, part = 0; code == CBN_SELCHANGE && SplitId(id, field, part) && part == kFieldValue &&
                                     Theme::Fields()[field].kind == Theme::Kind::Enum) {
            SetChoice(field, ComboSel(id));
            return;
        }
        if (id == Q(kQuickCornersCombo) || id == Q(kQuickEffectsCombo)) {
            if (code != CBN_SELCHANGE) return;
            const int sel = ComboSel(id);
            if (id == Q(kQuickCornersCombo)) {
                if (sel < 0 || sel > 2) {
                    UpdateQuick();  // "Mixed" is not a choice
                    return;
                }
                SetQuick(Quick::Corners, sel, std::wstring(L"Corners: ") + kCornerNames[sel]);
            } else if (sel >= 0 && sel <= 3) {
                SetQuick(Quick::Effects, sel, std::wstring(L"Icon effects: ") + kEffectNames[sel]);
            }
            return;
        }
        if (id == kMagnifyBarCombo) {  // round 9 phase 9A
            const int sel = ComboSel(id);
            if (code == CBN_SELCHANGE && sel >= 0 && sel <= static_cast<int>(MagnifyBar::Always) && static_cast<MagnifyBar>(sel) != Dock().magnifyBar) {
                Dock().magnifyBar = static_cast<MagnifyBar>(sel);
                Changed(SettingsScope::Appearance);  // relayout: the steady growth is part of the layout
                UpdateDockCounts();
            }
            return;
        }
        if (id == kDockThemeCombo) {
            if (code == CBN_DROPDOWN) {
                FillThemes();
            } else if (code == CBN_SELCHANGE) {
                const int sel = ComboSel(id);
                if (sel >= 0 && sel <= static_cast<int>(themeIds_.size())) {
                    SelectDockPreset(sel == 0 ? std::wstring() : themeIds_[static_cast<size_t>(sel - 1)]);
                }
            }
            return;
        }
        if (id == kThemeCombo) {
            if (code == CBN_DROPDOWN) {
                FillThemes();  // theme files may have been added / renamed meanwhile
            } else if (code == CBN_SELCHANGE) {
                const int sel = ComboSel(id);
                if (sel >= 0 && sel < static_cast<int>(themeIds_.size())) SelectPreset(themeIds_[static_cast<size_t>(sel)]);
            }
            return;
        }
        if (code != BN_CLICKED) return;
        if (id == kDockThemeReset) {  // round 10 phase 10D: back to the app's look (round 12 phase 12B: "Use the app look again")
            UseAppLook();
            if (HWND g = Item(kGalleryDock); g && GetFocus() == nullptr) SetFocus(g);
            return;
        }
        if (id == kOwnLook) {
            GiveOwnLook();
            return;
        }
        if (id == kThemeMore) {
            ShowThemeMenu();
            return;
        }
        // Round 8 phase 8D: the ⋯ menu's commands (the old buttons' / link's ids); round 12 phase 12B: theme files.
        if (id == kExportTheme) {
            Export();
            return;
        }
        if (id == kThemesFolderLink) {
            OpenThemesFolder();
            return;
        }
        if (id == kSaveAsTheme) {
            SaveAsTheme();
            return;
        }
        if (id == kImportTheme) {
            ImportTheme();
            return;
        }
        if (id == kApplyToDocks) {
            ApplyToDocks();
            return;
        }
        if (id == kResetAllTheme) {
            if (ReadOnly() || Mask() == 0) return;
            ChangeLabel(L"Reset all to preset");  // round 12 phase 12A: one undo entry, named
            Mask() = 0;
            Resolve();
            Changed(SettingsScope::Appearance);
            LoadValues();  // hides every ↺
            return;
        }
        if (id == Q(kQuickTintSwatch)) {
            uint32_t rgb = static_cast<uint32_t>(Theme::Derived(Quick::Tint).get(Look()));
            if (PickRgb(rgb)) SetQuick(Quick::Tint, static_cast<int>(rgb), L"Tint changed");
            return;
        }
        if (id == Q(kQuickDotsCheck)) {
            const bool on = GetCheck(id);
            SetQuick(Quick::Dots, on ? 1 : 0, on ? L"Indicator dots on" : L"Indicator dots off");
            return;
        }
        for (int q = 0; q < static_cast<int>(Quick::Count); ++q) {
            if (id == Q(QuickReset(static_cast<Quick>(q)))) {
                ResetQuick(static_cast<Quick>(q));
                return;
            }
        }
        int field = 0, part = 0;
        if (!SplitId(id, field, part)) return;
        const Theme::FieldInfo& info = Theme::Fields()[field];
        if (part == kFieldReset) {
            ResetField(field);
        } else if (part == kFieldSwatch && info.kind == Theme::Kind::Color) {
            PickColor(field);
        } else if (part == kFieldSwatch && info.kind == Theme::Kind::Bool) {
            if (ReadOnly()) return;
            Theme::SetBool(Look(), info, GetCheck(id));
            Override(field);
        }
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) == kDockThemeLink && (nm.code == NM_CLICK || nm.code == NM_RETURN)) {
            if (Host().openGlobal) Host().openGlobal(kPageThemes);
        }
        return 0;
    }

    // The Size card's icon size box moves its slider; the sliders apply on release.
    void IntChanged(int spin, int value) override {
        if (spin == kIconSizeSpin) SetTrack(kIconSizeTrack, std::min(value, kMaxTrackIconSize));
        if (spin == kRangeSpin) UpdateDockCounts();
    }

    void Track(int id, int pos, bool released) override {
        if (id == Q(kQuickAlphaTrack)) {  // round 12 phase 12B: Transparency, applied on release
            SetText(Q(kQuickAlphaValue), std::to_wstring(pos) + L" %");
            if (!released) return;
            const int before = Theme::Derived(Quick::Transparency).get(Look());
            if (pos != before) {
                SetQuick(Quick::Transparency, pos,
                         L"Transparency " + std::to_wstring(before) + L" \x2192 " + std::to_wstring(pos) + L" %");
            }
            return;
        }
        if (!dockOnly_) return;
        DockConfig& d = Dock();
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

    void Spun(int spin, int pos) override {
        int field = 0, part = 0;
        if (!SplitId(spin, field, part) || part != kFieldSpin) return;
        SetValue(field, pos);
    }

    void Commit(int id) override {
        if (id == Q(kQuickTintHex)) {
            uint32_t rgb = 0;
            if (ReadOnly()) return;
            if (!ParseRgb(GetText(id), rgb)) {
                MessageBeep(MB_OK);
                UpdateQuick();
                return;
            }
            SetQuick(Quick::Tint, static_cast<int>(rgb), L"Tint changed");
            return;
        }
        int field = 0, part = 0;
        if (!SplitId(id, field, part)) return;
        const Theme::FieldInfo& info = Theme::Fields()[field];
        if (info.kind == Theme::Kind::Enum) return;  // the combo applies on selection
        if (part == kFieldHex && info.kind == Theme::Kind::Color) {
            std::wstring text = Trim(GetText(id));
            if (!text.empty() && text[0] != L'#') text.insert(0, 1, L'#');
            uint32_t argb = 0;
            if (!ParseArgbColor(text, argb)) {
                MessageBeep(MB_OK);
                UpdateRow(field);  // back to the current value
                return;
            }
            SetColor(field, argb);
        } else if (part == kFieldValue) {
            const int lo = info.kind == Theme::Kind::Color ? 0 : FloatToUi(info, info.min);
            const int hi = info.kind == Theme::Kind::Color ? 255 : FloatToUi(info, info.max);
            int value = 0;
            if (!ReadInt(id, lo, hi, value)) {
                MessageBeep(MB_OK);
                UpdateRow(field);
                return;
            }
            SetValue(field, value);
        }
    }

    bool OnDrawItem(const DRAWITEMSTRUCT& di) override {
        uint32_t argb = 0;
        if (static_cast<int>(di.CtlID) == Q(kQuickTintSwatch)) {
            argb = 0xFF000000u | static_cast<uint32_t>(Theme::Derived(Quick::Tint).get(Look()));  // the tint, opaque
        } else {
            int field = 0, part = 0;
            if (!SplitId(static_cast<int>(di.CtlID), field, part) || part != kFieldSwatch) return false;
            argb = Theme::GetColor(Look(), Theme::Fields()[field]);
        }
        const DarkMode::Palette& pal = Window().Colors();
        RECT r = di.rcItem;
        HBRUSH back = CreateSolidBrush(Window().SurfaceColor(di.hwndItem));  // a card's fill (round 8 phase 8A)
        FillRect(di.hDC, &r, back);
        DeleteObject(back);
        // Frame, then a checkerboard under the colour so the alpha is visible.
        const bool focus = (di.itemState & ODS_FOCUS) != 0;
        const bool disabled = (di.itemState & ODS_DISABLED) != 0;
        HBRUSH frame = CreateSolidBrush(focus ? pal.accent : disabled ? pal.textDisabled : pal.textSecondary);
        FrameRect(di.hDC, &r, frame);
        if (focus) {
            RECT r2 = r;
            InflateRect(&r2, -1, -1);
            FrameRect(di.hDC, &r2, frame);
        }
        DeleteObject(frame);
        InflateRect(&r, focus ? -3 : -2, focus ? -3 : -2);
        const unsigned alpha = disabled ? (argb >> 24) / 2 : argb >> 24;  // read-only: faded (round 12 phase 12B)
        const COLORREF c = RgbOf(argb);
        HBRUSH light = CreateSolidBrush(Blend(c, RGB(0xFF, 0xFF, 0xFF), alpha));
        HBRUSH dark = CreateSolidBrush(Blend(c, RGB(0xC4, 0xC4, 0xC4), alpha));
        const int cell = std::max(3, Window().Px(5));
        for (int y = r.top, row = 0; y < r.bottom; y += cell, ++row) {
            for (int x = r.left, col = 0; x < r.right; x += cell, ++col) {
                RECT cr{x, y, std::min<int>(x + cell, r.right), std::min<int>(y + cell, r.bottom)};
                FillRect(di.hDC, &cr, ((row + col) & 1) ? dark : light);
            }
        }
        DeleteObject(light);
        DeleteObject(dark);
        return true;
    }

private:
    // --- the target (round 12 phase 12B) ---
    // The dock uses the app look: its values are shown read-only.
    bool ReadOnly() { return dockOnly_ && !Dock().OwnLook(); }
    // The look shown and edited: the dock's own, else the app's.
    ThemeConfig& Look() { return dockOnly_ && Dock().OwnLook() ? Dock().theme : Cfg().theme; }
    uint64_t& Mask() { return dockOnly_ && Dock().OwnLook() ? Dock().themeOverrides : Cfg().themeOverrides; }
    // The preset values under it (what ↺ goes back to).
    ThemeConfig PresetValues() { return Cfg().PresetOf(dockOnly_ ? &Dock() : nullptr); }
    // The preset the target names (the dock's own, else the app's).
    std::wstring& PresetId() { return dockOnly_ && Dock().OwnLook() ? Dock().themePreset : Cfg().themePreset; }
    void Resolve() {
        if (dockOnly_) {
            Cfg().ResolveDockThemes();
        } else {
            Cfg().ResolveTheme();  // also the docks: one with its own look whose preset failed is based on the app's
        }
    }
    int GalleryId() const { return dockOnly_ ? kGalleryDock : kGalleryApp; }
    int Q(int part) const { return QuickId(part, dockOnly_); }
    int Row(int field, int part) const { return rowBase_ + field * kFieldRowStride + part; }
    std::wstring DockName() { return Dock().DisplayName(); }  // round 12 phase 12C: the name people see

    // A Float field's value as the settings UI shows it (value x scale, rounded).
    static int FloatToUi(const Theme::FieldInfo& info, float v) { return static_cast<int>(std::lround(v * info.scale)); }

    bool SplitId(int id, int& field, int& part) const {
        const int rel = id - rowBase_;
        if (rel < 0 || rel >= static_cast<int>(Theme::FieldCount()) * kFieldRowStride) return false;
        field = rel / kFieldRowStride;
        part = rel % kFieldRowStride;
        return true;
    }

    static constexpr int kThemeLabelW = 156, kThemeColX = kMargin + kThemeLabelW + 8;  // round 8 phase 8A
    static constexpr int kMoreW = 32;  // round 8 phase 8D: the "⋯" button beside the theme combo
    static constexpr int kRevertW = 32;  // round 10 phase 10D: the "↺" beside This dock's look

    // Round 12 phase 12B: the gallery in the current row (its height follows the page's width).
    void GalleryRow(int id, const wchar_t* tip) {
        const int right = Width() - kMargin;
        Ctl& g = Add(id, CtlKind::Custom, SettingsGallery::kClassName, L"Themes", WS_TABSTOP,
                     RECT{kMargin, y_ + 2, right, y_ + 2 + SettingsGallery::kTileH});
        g.measured = true;
        Tip(id, tip);
        NextRow(SettingsGallery::kTileH + 10);
    }

    void GalleryColors() {
        HWND g = Item(GalleryId());
        if (!g) return;
        const DarkMode::Palette& pal = Window().Colors();
        gallery_.SetColors(Window().SurfaceColor(g), pal.text, pal.textSecondary, pal.accent);
    }

    // Round 12 phase 12B: the Quick look card (open), ids QuickId(part, dockOnly_).
    void QuickCard() {
        const int right = Width() - kMargin;
        constexpr int kResetW = 24;
        auto reset = [&](int part) {
            Add(Q(part), CtlKind::Button, WC_BUTTONW, L"\x21BA", WS_TABSTOP | BS_PUSHBUTTON,
                RECT{right - kResetW, y_ + 4, right, y_ + 4 + kResetW});
            Tip(Q(part), L"Reset to the preset's value");
        };
        Section(Q(kQuickSection), L"Quick look", L"guide-settings.md#quick-look");
        Label(Q(kQuickTintLabel), L"Tint");
        Tip(Q(kQuickTintLabel), L"The colour of the bar (its opacity stays). The name label and the slot buttons take its hue "
                                L"at their own brightness. Click the swatch to pick a colour or type #RRGGBB.");
        Add(Q(kQuickTintSwatch), CtlKind::Swatch, WC_BUTTONW, L"Tint", WS_TABSTOP | BS_OWNERDRAW,
            RECT{colX_, y_ + 5, colX_ + 36, y_ + kRowH - 5});
        Ctl& hex = Add(Q(kQuickTintHex), CtlKind::Edit, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL,
                       RECT{colX_ + 44, y_ + 4, colX_ + 44 + 92, y_ + kRowH - 4});
        hex.exStyle = WS_EX_CLIENTEDGE;
        hex.hi = 9;
        reset(kQuickTintReset);
        NextRow();
        Label(Q(kQuickAlphaLabel), L"Transparency");
        Tip(Q(kQuickAlphaLabel), L"How much the wallpaper shows through the bar, 0 % (solid) to 100 % (clear): the bar's "
                                 L"background and the tint over the blur together.");
        Trackbar(Q(kQuickAlphaTrack), colX_ - 6, 212, 0, 100, 10);
        Text(Q(kQuickAlphaValue), CtlKind::Value, L"", colX_ + 216, 56);
        reset(kQuickAlphaReset);
        NextRow();
        Label(Q(kQuickCornersLabel), L"Corners");
        Tip(Q(kQuickCornersLabel), L"Square, Soft or Round: the bar's corner radius 0, 8 or 16 DIPs and the slot buttons' "
                                   L"0, 3 or 6. Mixed: the radius was set to another value in the Bar card.");
        Combo(Q(kQuickCornersCombo), colX_, 168, false);
        reset(kQuickCornersReset);
        NextRow();
        Label(Q(kQuickEffectsLabel), L"Icon effects");
        Tip(Q(kQuickEffectsLabel), L"A glow behind the icons (the theme's glow mode, else while hovered) and a reflection under "
                                   L"them. The Icon effects card has their details.");
        Combo(Q(kQuickEffectsCombo), colX_, 168, false);
        reset(kQuickEffectsReset);
        NextRow();
        Check(Q(kQuickDotsCheck), Q(kQuickDotsCaption), L"Indicator dots", L"Under running apps (slot buttons draw none)",
              Q(kQuickNote));
        Tip(Q(kQuickDotsCheck), L"Off makes the running, active and attention dots clear; on brings back the theme's.");
        reset(kQuickDotsReset);
        NextRow();
    }

    static int QuickReset(Quick q) {
        switch (q) {
            case Quick::Tint: return kQuickTintReset;
            case Quick::Transparency: return kQuickAlphaReset;
            case Quick::Corners: return kQuickCornersReset;
            case Quick::Effects: return kQuickEffectsReset;
            case Quick::Dots:
            default: return kQuickDotsReset;
        }
    }

    // One card per group, its heading an expander ("Bar · 2 changed"). Round 10 phase 10D: every
    // group closed at first (Bar was open): the individual values are detail.
    void FieldCards() {
        const int right = Width() - kMargin;
        for (const FieldGroup& g : FieldGroups()) {
            UntitledCard();
            HeadingExpander(g.sectionId, g.title, false, g.topic ? g.topic : L"guide-settings.md#theme-values");
            for (Field f : g.fields) FieldRowCtl(static_cast<int>(f));
            if (g.note) {
                // One line (two at most); the whole note as its tooltip.
                Ctl& n = Text(g.noteId, CtlKind::Note, g.hint ? g.hint : g.note, kMargin, right - kMargin, 20);
                n.maxLines = 2;
                Tip(g.noteId, g.note);
                NextRow(22);
            }
            EndExpander();
        }
    }

    void FieldRowCtl(int field) {
        const Theme::FieldInfo& info = Theme::Fields()[field];
        const int right = Width() - kMargin;
        constexpr int kResetW = 24;
        const int colX = colX_;  // the page's own columns (Themes: narrower)
        switch (info.kind) {
            case Theme::Kind::Color: {
                Label(Row(field, kFieldLabel), info.label);
                // The swatch's window text is its accessible name (never drawn).
                Add(Row(field, kFieldSwatch), CtlKind::Swatch, WC_BUTTONW, info.label, WS_TABSTOP | BS_OWNERDRAW,
                    RECT{colX, y_ + 5, colX + 36, y_ + kRowH - 5});
                Ctl& hex = Add(Row(field, kFieldHex), CtlKind::Edit, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL,
                               RECT{colX + 44, y_ + 4, colX + 44 + 92, y_ + kRowH - 4});
                hex.exStyle = WS_EX_CLIENTEDGE;
                hex.hi = 9;
                Spin(Row(field, kFieldValue), Row(field, kFieldSpin), colX + 144, 60, 0, 255, 5);
                break;
            }
            case Theme::Kind::Float:
                Label(Row(field, kFieldLabel), info.label);
                Spin(Row(field, kFieldValue), Row(field, kFieldSpin), colX, 72, FloatToUi(info, info.min),
                     FloatToUi(info, info.max), info.scale > 1.0f ? 5 : 1);
                Text(Row(field, kFieldUnit), CtlKind::Note, info.unit, colX + 80, 96);
                break;
            case Theme::Kind::Bool:
                Check(Row(field, kFieldSwatch), Row(field, kFieldHex), info.label);
                break;
            case Theme::Kind::Enum:
                Label(Row(field, kFieldLabel), info.label);
                Combo(Row(field, kFieldValue), colX, 168, false);
                break;
        }
        // Round 11 phase 11A: the new fields explain themselves (on the label, or a check box).
        if (const wchar_t* tip = FieldTip(info.field)) {
            Tip(Row(field, info.kind == Theme::Kind::Bool ? kFieldSwatch : kFieldLabel), tip);
        }
        // Round 8 phase 8D: a 24 DIP "↺" (was a "Reset" button), shown only while overridden.
        Add(Row(field, kFieldReset), CtlKind::Button, WC_BUTTONW, L"\x21BA", WS_TABSTOP | BS_PUSHBUTTON,
            RECT{right - kResetW, y_ + 4, right, y_ + 4 + kResetW});
        Tip(Row(field, kFieldReset), L"Reset to the preset's value");
        NextRow();
    }

    // Round 11 phase 11A: the tooltips of the fields this round added (the older rows have none:
    // their labels say enough).
    static const wchar_t* FieldTip(Field f) {
        switch (f) {
            case Field::SlotStyle:
                return L"None: the indicator dots under running apps (the default). Buttons: a frame behind each app, "
                       L"folder, group and Recycle Bin slot shows its state instead, like the Windows 7 taskbar.";
            case Field::SlotRunning: return L"The frame of a running app (a top-to-bottom gradient to 60 % of this alpha).";
            case Field::SlotActive: return L"The frame of the app that owns the foreground window.";
            case Field::SlotAttention: return L"The frame of an app flashing for attention; it pulses.";
            case Field::SlotHover:
                return L"Added while the pointer is over a slot. A pinned app that does not run shows only this.";
            case Field::SlotBorder: return L"The frames' hairline (with a fainter inner highlight) and the stacked outlines.";
            case Field::SlotCornerRadius: return L"The frames' corners, 0 - 16 DIPs. Default: 3.";
            case Field::SlotHotTrack:
                return L"While the pointer moves over a frame, a soft light in the icon's own colour follows it inside the "
                       L"frame. Drawn only while the pointer moves.";
            case Field::Sheen:
                return L"A glossy highlight over the upper half of the bar and a bright line along its far edge, 0 - 100 %. "
                       L"Not on the glass shelf or a ring. Default: 0 (none).";
            case Field::IconGap:
                return L"The space between two icon slots, 0 - 48 DIPs (a slot is the icon size plus this). Default: 8. "
                       L"Wide gaps suit slot buttons.";
            case Field::BarPadding:
                return L"The space between the icons and the bar's long edges, 0 - 24 DIPs (the bar is the icon size plus "
                       L"twice this). Default: 8.";
            default: return nullptr;
        }
    }

    // Round 11 phase 11A: the Slot buttons rows under Slot style collapse while it is None.
    void UpdateSlotRows() {
        const bool none = SlotStyleOf(Look()) == SlotStyle::None;
        for (Field f : {Field::SlotRunning, Field::SlotActive, Field::SlotAttention, Field::SlotHover, Field::SlotBorder,
                        Field::SlotCornerRadius, Field::SlotHotTrack}) {
            const int field = static_cast<int>(f);
            Collapse({Row(field, kFieldLabel), Row(field, kFieldSwatch), Row(field, kFieldHex), Row(field, kFieldValue),
                      Row(field, kFieldSpin), Row(field, kFieldReset), Row(field, kFieldUnit)},
                     none);
        }
    }

    // Every value row, the Quick look and the counts from the target.
    void LoadValues() {
        for (size_t i = 0; i < Theme::FieldCount(); ++i) UpdateRow(static_cast<int>(i));
        UpdateQuick();
        UpdateCounts();
    }

    // Round 8 phase 8D: each group's heading counts its overridden fields ("Bar · 2 changed") and
    // the Theme label's hint all of them. Round 12 phase 12B: of the look shown (the dock's own or
    // the app's); the Look tab's hint says which look the dock has.
    void UpdateCounts() {
        UpdateSlotRows();
        const uint64_t over = Mask();
        int total = 0;
        for (const FieldGroup& g : FieldGroups()) {
            int n = 0;
            for (Field f : g.fields) n += (over & Theme::Bit(f)) ? 1 : 0;
            total += n;
            SetExpanderText(g.sectionId, n > 0 ? std::wstring(g.title) + L" \x00B7 " + std::to_wstring(n) + L" changed" : std::wstring(g.title));
        }
        if (!dockOnly_) {
            SetText(kThemeNote, total == 0 ? std::wstring(L"The preset's values") : ChangedCount(total));
            return;
        }
        if (!Dock().OwnLook()) {
            SetText(kDockThemeNote, L"The app look (" + appName_ + L")");
        } else {
            SetText(kDockThemeNote, L"Its own: " + ownName_ + (total > 0 ? L" \x00B7 " + ChangedCount(total) : std::wstring()));
        }
    }

    // The ⋯ menu under its button (round 12 phase 12B: the theme files and Apply to other docks).
    void ShowThemeMenu() {
        HWND button = Item(kThemeMore);
        HMENU menu = CreatePopupMenu();
        if (!button || !menu) {
            if (menu) DestroyMenu(menu);
            return;
        }
        AppendMenuW(menu, MF_STRING, kSaveAsTheme, L"Save as theme\x2026");
        AppendMenuW(menu, MF_STRING, kImportTheme, L"Import theme\x2026");
        AppendMenuW(menu, MF_STRING, kExportTheme, L"Export theme\x2026");
        AppendMenuW(menu, MF_STRING | (OtherDocks().empty() ? MF_GRAYED : 0), kApplyToDocks,
                    dockOnly_ ? L"Apply to other docks\x2026" : L"Apply to docks\x2026");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kThemesFolderLink, L"Open themes folder");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (ReadOnly() || Mask() == 0 ? MF_GRAYED : 0), kResetAllTheme, L"Reset all to preset");
        RECT r{};
        GetWindowRect(button, &r);
        const int cmd = static_cast<int>(
            TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom, Owner(), nullptr));
        DestroyMenu(menu);
        if (cmd > 0) OnCommand(cmd, BN_CLICKED, nullptr);
    }

    void OpenThemesFolder() {
        EnsureThemesFolder();
        AllowSetForegroundWindow(ASFW_ANY);
        ShellExecuteW(nullptr, L"explore", Theme::Directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // Config -> one row (and its Reset button). Round 12 phase 12B: greyed while read-only.
    void UpdateRow(int field) {
        const ThemeConfig& t = Look();
        const bool editable = !ReadOnly();
        const Theme::FieldInfo& info = Theme::Fields()[field];
        switch (info.kind) {
            case Theme::Kind::Color: {
                const uint32_t argb = Theme::GetColor(t, info);
                SetText(Row(field, kFieldHex), FormatArgbColor(argb));
                SetSpin(Row(field, kFieldSpin), static_cast<int>(argb >> 24));
                if (HWND swatch = Item(Row(field, kFieldSwatch))) InvalidateRect(swatch, nullptr, FALSE);
                break;
            }
            case Theme::Kind::Float:
                SetSpin(Row(field, kFieldSpin), FloatToUi(info, Theme::GetFloat(t, info)));
                break;
            case Theme::Kind::Bool:
                SetCheck(Row(field, kFieldSwatch), Theme::GetBool(t, info));
                break;
            case Theme::Kind::Enum: {
                const int id = Row(field, kFieldValue);
                if (HWND combo = Item(id); combo && SendMessageW(combo, CB_GETCOUNT, 0, 0) == 0) {
                    for (size_t i = 0; i < info.count; ++i) ComboAdd(id, info.choices[i]);
                }
                SetComboSel(id, Theme::GetInt(t, info));
                break;
            }
        }
        for (int part : {kFieldSwatch, kFieldHex, kFieldValue, kFieldSpin}) {
            if (Item(Row(field, part))) Enable(Row(field, part), editable);
        }
        Show(Row(field, kFieldReset), editable && (Mask() & Theme::Bit(info.field)) != 0);
    }

    // Round 12 phase 12B: the Quick look's controls from the look's effective values (after every
    // change, so a raw edit shows here too), their ↺ while each differs from the preset's.
    void UpdateQuick() {
        const ThemeConfig& t = Look();
        const ThemeConfig preset = PresetValues();
        const bool editable = !ReadOnly();
        const int tint = Theme::Derived(Quick::Tint).get(t);
        SetText(Q(kQuickTintHex), HexRgb(static_cast<uint32_t>(tint)));
        if (HWND sw = Item(Q(kQuickTintSwatch))) InvalidateRect(sw, nullptr, FALSE);
        const int alpha = Theme::Derived(Quick::Transparency).get(t);
        SetTrack(Q(kQuickAlphaTrack), alpha);
        SetText(Q(kQuickAlphaValue), std::to_wstring(alpha) + L" %");
        // Corners: "Mixed" is a fourth row only while no choice matches.
        const int corners = Theme::Derived(Quick::Corners).get(t);
        if (HWND combo = Item(Q(kQuickCornersCombo))) {
            const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
            if (corners < 0 && count == 3) ComboAdd(Q(kQuickCornersCombo), kCornerNames[3]);
            if (corners >= 0 && count == 4) SendMessageW(combo, CB_DELETESTRING, 3, 0);
            SetComboSel(Q(kQuickCornersCombo), corners < 0 ? 3 : corners);
        }
        SetComboSel(Q(kQuickEffectsCombo), Theme::Derived(Quick::Effects).get(t));
        SetCheck(Q(kQuickDotsCheck), Theme::Derived(Quick::Dots).get(t) != 0);
        for (int part : {kQuickTintSwatch, kQuickTintHex, kQuickAlphaTrack, kQuickCornersCombo, kQuickEffectsCombo,
                         kQuickDotsCheck}) {
            Enable(Q(part), editable);
        }
        for (int q = 0; q < static_cast<int>(Quick::Count); ++q) {
            const Quick qq = static_cast<Quick>(q);
            Show(Q(QuickReset(qq)), editable && Theme::DerivedDiffers(qq, t, preset));
        }
    }

    // A Quick look control's new value: written through the derived table (one undo entry, `label`).
    void SetQuick(Quick q, int value, const std::wstring& label) {
        if (ReadOnly()) {
            UpdateQuick();
            return;
        }
        ThemeConfig t = Look();
        uint64_t mask = Mask();
        if (!Theme::SetDerived(q, t, mask, PresetValues(), value)) {
            UpdateQuick();
            return;
        }
        ChangeLabel(label);  // only for a change (a label without one would name the next change)
        Look() = t;
        Mask() = mask;
        Changed(SettingsScope::Appearance);
        LoadValues();
    }

    void ResetQuick(Quick q) {
        if (ReadOnly()) return;
        ThemeConfig t = Look();
        uint64_t mask = Mask();
        if (!Theme::ResetDerived(q, t, mask, PresetValues())) {
            UpdateQuick();
            return;
        }
        ChangeLabel(std::wstring(Theme::Derived(q).label) + L": the preset's value");
        Look() = t;
        Mask() = mask;
        Changed(SettingsScope::Appearance);
        const bool hadFocus = GetFocus() == Item(Q(QuickReset(q)));
        LoadValues();
        if (hadFocus) {
            const int first = q == Quick::Tint ? kQuickTintSwatch : q == Quick::Transparency ? kQuickAlphaTrack
                              : q == Quick::Corners ? kQuickCornersCombo : q == Quick::Effects ? kQuickEffectsCombo
                                                                                               : kQuickDotsCheck;
            if (HWND h = Item(Q(first))) SetFocus(h);
        }
    }

    // The field was edited in Look(): mark it overridden and apply.
    void Override(int field) {
        Mask() |= Theme::Bit(Theme::Fields()[field].field);
        Changed(SettingsScope::Appearance);
        UpdateRow(field);
        UpdateQuick();
        UpdateCounts();
    }

    void SetColor(int field, uint32_t argb) {
        const Theme::FieldInfo& info = Theme::Fields()[field];
        if (ReadOnly() || Theme::GetColor(Look(), info) == argb) {
            UpdateRow(field);  // normalises the text
            return;
        }
        Theme::SetColor(Look(), info, argb);
        Override(field);
    }

    // The number of a row: a colour's alpha or a float field's value.
    void SetValue(int field, int value) {
        const Theme::FieldInfo& info = Theme::Fields()[field];
        if (ReadOnly()) {
            UpdateRow(field);
            return;
        }
        if (info.kind == Theme::Kind::Color) {
            const uint32_t argb = Theme::GetColor(Look(), info);
            SetColor(field, (static_cast<uint32_t>(std::clamp(value, 0, 255)) << 24) | (argb & 0x00FFFFFFu));
        } else if (info.kind == Theme::Kind::Float) {
            const float v = std::clamp(static_cast<float>(value) / info.scale, info.min, info.max);
            if (Theme::GetFloat(Look(), info) == v) {
                UpdateRow(field);
                return;
            }
            Theme::SetFloat(Look(), info, v);
            Override(field);
        }
    }

    // An Enum row's combo selection.
    void SetChoice(int field, int index) {
        const Theme::FieldInfo& info = Theme::Fields()[field];
        if (ReadOnly() || index < 0 || static_cast<size_t>(index) >= info.count || Theme::GetInt(Look(), info) == index) {
            UpdateRow(field);
            return;
        }
        Theme::SetInt(Look(), info, index);
        Override(field);
    }

    void ResetField(int field) {
        const uint64_t bit = Theme::Bit(Theme::Fields()[field].field);
        if (ReadOnly() || !(Mask() & bit)) return;
        Mask() &= ~bit;
        Resolve();  // copies the preset's value back (and only into fields without overrides)
        Changed(SettingsScope::Appearance);
        const bool hadFocus = GetFocus() == Item(Row(field, kFieldReset));
        UpdateRow(field);  // hides the ↺ button
        UpdateQuick();
        UpdateCounts();
        if (hadFocus) {
            // The keyboard focus stays on the row: its swatch / check box, or a float's number box.
            const Theme::Kind kind = Theme::Fields()[field].kind;
            const int first = kind == Theme::Kind::Float || kind == Theme::Kind::Enum ? kFieldValue : kFieldSwatch;
            if (HWND h = Item(Row(field, first))) SetFocus(h);
        }
    }

    // The Windows colour dialog for an RGB (the session's custom colours kept).
    bool PickRgb(uint32_t& rgb) {
        static COLORREF custom[16] = {
            RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
            RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
            RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
            RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255)};
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = Owner();
        cc.rgbResult = RgbOf(rgb);
        cc.lpCustColors = custom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
        if (!ChooseColorW(&cc)) return false;
        rgb = (static_cast<uint32_t>(GetRValue(cc.rgbResult)) << 16) | (static_cast<uint32_t>(GetGValue(cc.rgbResult)) << 8) |
              GetBValue(cc.rgbResult);
        return true;
    }

    void PickColor(int field) {
        if (ReadOnly()) return;
        const Theme::FieldInfo& info = Theme::Fields()[field];
        const uint32_t argb = Theme::GetColor(Look(), info);
        uint32_t rgb = argb & 0x00FFFFFFu;
        if (!PickRgb(rgb)) return;
        SetColor(field, (argb & 0xFF000000u) | rgb);  // RGB replaced, alpha kept
    }

    void SelectPreset(const std::wstring& id) {
        Config& c = Cfg();
        if (id == c.themePreset && c.themeOverrides == 0) return;
        c.themePreset = id;
        c.themeOverrides = 0;  // a new preset starts clean
        if (!c.ResolveTheme()) MessageBeep(MB_ICONWARNING);  // broken theme file: dark meanwhile
        Changed(SettingsScope::Appearance);
        Load();
    }

    // The window's dock's own preset (docks[].themePreset; "" = the app look). Round 12 phase 12B:
    // a new preset starts clean, as the app's does.
    void SelectDockPreset(const std::wstring& id) {
        if (id.empty()) {
            UseAppLook();
            return;
        }
        DockConfig& d = Dock();
        if (id == d.themePreset && d.themeOverrides == 0) return;
        d.themePreset = id;
        d.themeOverrides = 0;
        Cfg().ResolveDockThemes();
        Changed(SettingsScope::Appearance);
        Load();
    }

    // Round 12 phase 12B: "Give this dock its own look" (the app's current look as the start).
    void GiveOwnLook() {
        if (!dockOnly_ || Dock().OwnLook()) return;
        ChangeLabel(L"Gave " + DockName() + L" its own look");
        Cfg().GiveOwnLook(Dock());
        Cfg().ResolveDockThemes();
        Changed(SettingsScope::Appearance);
        Load();
    }

    // ... "Use the app look again" (↺ beside This dock's look).
    void UseAppLook() {
        if (!dockOnly_ || !Dock().OwnLook()) return;
        ChangeLabel(DockName() + L" uses the app look again");
        Config::UseAppLook(Dock());
        Cfg().ResolveDockThemes();
        Changed(SettingsScope::Appearance);
        Load();
    }

    // Round 12 phase 12B: a gallery tile chosen: a Keep / Cancel preview of that look.
    void PickTile(int index) {
        if (index < 0 || static_cast<size_t>(index) >= tileIds_.size()) return;
        const std::wstring id = tileIds_[static_cast<size_t>(index)];
        const std::wstring name = tileNames_[static_cast<size_t>(index)];
        if (dockOnly_) {
            DockConfig& d = Dock();
            if (id.empty() ? !d.OwnLook() : (EqualsNoCase(id, d.themePreset) && d.themeOverrides == 0)) return;
            const std::wstring what = id.empty() ? std::wstring(L"the app look") : L"the " + name + L" look";
            if (Host().preview) Host().preview(L"Applied " + what + L" to " + DockName(), L"Previewing " + what + L" on " + DockName() + L".");
            if (id.empty()) {
                Config::UseAppLook(d);
                Cfg().ResolveDockThemes();
                Changed(SettingsScope::Appearance);
                Load();
            } else {
                SelectDockPreset(id);
            }
            return;
        }
        Config& c = Cfg();
        if (EqualsNoCase(id, c.themePreset) && c.themeOverrides == 0) return;
        if (Host().preview) Host().preview(L"Applied the " + name + L" look", L"Previewing the " + name + L" look.");
        SelectPreset(id);
    }

    // Built-ins by name, then the theme files by name; a preset that is not there is listed as
    // "<id> (missing)" so the user sees what config.json names. Round 12 phase 12B: the gallery
    // gets the same rows (the dock's: "App look (<name>)" first).
    void FillThemes() {
        const Config& c = Cfg();
        std::vector<std::wstring> ids, names;
        std::vector<ThemeConfig> values;
        size_t count = 0;
        const Theme::Preset* presets = Theme::BuiltIns(count);
        for (size_t i = 0; i < count; ++i) {
            ids.push_back(presets[i].id);
            names.push_back(presets[i].name);
            values.push_back(presets[i].values);
        }
        for (const Theme::UserTheme& t : Theme::ListUserThemes()) {
            ThemeConfig v;
            const bool ok = t.valid && Theme::LoadUserTheme(t.path, v, nullptr);
            ids.push_back(t.id);
            names.push_back(ok ? t.name : t.name + L" (unreadable)");
            values.push_back(ok ? v : ThemeConfig{});
        }
        auto indexOf = [&](const std::wstring& id) -> int {
            for (size_t i = 0; i < ids.size(); ++i) {
                if (EqualsNoCase(ids[i], id)) return static_cast<int>(i);
            }
            return -1;
        };
        auto addMissing = [&](const std::wstring& id) {
            ids.push_back(id);
            names.push_back(id.empty() ? std::wstring(L"None (dark defaults)") : id + L" (missing)");
            values.push_back(ThemeConfig{});
            return static_cast<int>(ids.size()) - 1;
        };
        int appSel = indexOf(c.themePreset);
        if (appSel < 0) appSel = addMissing(c.themePreset);
        appName_ = names[static_cast<size_t>(appSel)];
        int ownSel = -1;
        if (dockOnly_ && Dock().OwnLook()) {
            ownSel = indexOf(Dock().themePreset);
            if (ownSel < 0) ownSel = addMissing(Dock().themePreset);
            ownName_ = names[static_cast<size_t>(ownSel)];
        }
        themeIds_ = ids;
        const int changed = std::popcount(Mask());
        const std::wstring current = changed > 0 ? L"Current \x00B7 " + std::to_wstring(changed) + L" changed" : L"Current";
        std::vector<SettingsGallery::Tile> tiles;
        tileIds_.clear();
        tileNames_.clear();
        if (dockOnly_) {
            SettingsGallery::Tile app;
            app.name = L"App look (" + appName_ + L")";
            app.theme = c.theme;
            app.current = !Dock().OwnLook();
            if (app.current) app.detail = L"Current";
            tiles.push_back(std::move(app));
            tileIds_.push_back(std::wstring());
            tileNames_.push_back(L"App look");
        }
        const int sel = dockOnly_ ? ownSel : appSel;
        for (size_t i = 0; i < ids.size(); ++i) {
            SettingsGallery::Tile tile;
            tile.name = names[i];
            tile.theme = values[i];
            tile.current = static_cast<int>(i) == sel;
            if (tile.current) tile.detail = current;
            tiles.push_back(std::move(tile));
            tileIds_.push_back(ids[i]);
            tileNames_.push_back(names[i]);
        }
        const int galleryCurrent = dockOnly_ ? (ownSel < 0 ? 0 : ownSel + 1) : appSel;
        const int before = gallery_.Count();
        gallery_.SetTiles(std::move(tiles), galleryCurrent);
        if (gallery_.Count() != before) Relayout();  // the gallery's height (its rows) may change

        if (HWND combo = Item(kThemeCombo)) {
            SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
            SendMessageW(combo, CB_RESETCONTENT, 0, 0);
            for (const std::wstring& n : names) ComboAdd(kThemeCombo, n.c_str());
            SetComboSel(kThemeCombo, appSel);
            SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
        }
        // This dock's look: "Use app default (<the app's theme>)" (round 10 phase 10D: was "Same as
        // all docks"), then the same list (an unknown id: "(missing)").
        if (HWND dock = Item(kDockThemeCombo)) {
            SendMessageW(dock, CB_RESETCONTENT, 0, 0);
            ComboAdd(kDockThemeCombo, (L"Use app default (" + appName_ + L")").c_str());
            for (const std::wstring& n : names) ComboAdd(kDockThemeCombo, n.c_str());
            SetComboSel(kDockThemeCombo, ownSel < 0 ? 0 : ownSel + 1);
        }
    }

    // Round 10 phase 10D: "Magnification details · n changed" (range and bar mode against the
    // defaults of a new dock).
    void UpdateDockCounts() {
        const DockConfig& d = Dock();
        const DockConfig fresh;
        const int n = (d.magnifyRange != fresh.magnifyRange ? 1 : 0) + (d.magnifyBar != fresh.magnifyBar ? 1 : 0);
        SetExpanderText(kMagnifyMore, ChangedTitle(L"Magnification details", n));
    }

    // The docks Apply to (other) docks lists: config indices of the docks (not folder windows or
    // standalone widgets), this window's own left out on the Look tab.
    std::vector<size_t> OtherDocks() {
        std::vector<size_t> out;
        const Config& c = Cfg();
        for (size_t i = 0; i < c.docks.size(); ++i) {
            if (!c.docks[i].IsDock()) continue;
            if (dockOnly_ && i == DockIndex()) continue;
            out.push_back(i);
        }
        return out;
    }

    // Round 12 phase 12B: "Save as theme…": themes\<name>.json with this look; then the look's
    // preset (nothing changed: the same values), unless the dock uses the app look (the file only).
    void SaveAsTheme() {
        std::wstring name = PresetId().empty() || Theme::FindBuiltIn(PresetId()) ? std::wstring(L"My look") : ownOrAppName();
        if (Host().modal) Host().modal(true);
        const bool ok = InputDialog::Ask(Owner(), L"Save as theme", L"Name of the theme:", name, 64);
        if (Host().modal) Host().modal(false);
        if (!ok || Trim(name).empty()) return;
        name = Trim(name);
        std::wstring stem;
        DWORD error = Theme::SaveAs(name, Look(), false, stem);
        if (error == ERROR_FILE_EXISTS) {
            if (Host().modal) Host().modal(true);
            const int answer = MessageBoxW(Owner(), (L"A theme named \x201C" + name + L"\x201D exists. Replace it?").c_str(),
                                           L"Save as theme", MB_YESNO | MB_ICONQUESTION);
            if (Host().modal) Host().modal(false);
            if (answer != IDYES) return;
            error = Theme::SaveAs(name, Look(), true, stem);
        }
        if (error != 0) {
            MessageBeep(MB_ICONERROR);
            MessageBoxW(Owner(), (L"Could not save the theme \x201C" + name + L"\x201D.").c_str(), L"WinDock", MB_OK | MB_ICONERROR);
            return;
        }
        if (!ReadOnly()) {
            ChangeLabel(L"Saved the look as theme \x201C" + name + L"\x201D");
            PresetId() = stem;
            Mask() = 0;
            Resolve();
            Changed(SettingsScope::Appearance);
        }
        Load();  // the gallery lists the new file
    }

    std::wstring ownOrAppName() { return dockOnly_ && Dock().OwnLook() ? ownName_ : appName_; }

    // Round 12 phase 12B: "Import theme…": a theme file copied into the themes folder (the gallery
    // shows it; nothing is applied).
    void ImportTheme() {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        const COMDLG_FILTERSPEC filters[] = {{L"Theme files (*.json)", L"*.json"}};
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetTitle(L"Import theme");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR);
        if (Host().modal) Host().modal(true);
        const HRESULT shown = dialog->Show(Owner());
        if (Host().modal) Host().modal(false);
        if (FAILED(shown)) return;  // cancelled
        winrt::com_ptr<IShellItem> result;
        PWSTR raw = nullptr;
        if (FAILED(dialog->GetResult(result.put())) || FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        const std::wstring path = raw;
        CoTaskMemFree(raw);
        std::wstring stem;
        const DWORD error = Theme::Import(path, stem);
        if (error != 0) {
            MessageBeep(MB_ICONERROR);
            MessageBoxW(Owner(),
                        error == ERROR_INVALID_DATA ? (StemOf(path) + L" is not a theme file (a JSON object of theme values).").c_str()
                                                    : (L"Could not copy " + path + L" into the themes folder.").c_str(),
                        L"Import theme", MB_OK | MB_ICONERROR);
            return;
        }
        FillThemes();
        for (size_t i = 0; i < tileIds_.size(); ++i) {  // the new tile gets the selection (choose it to try it)
            if (EqualsNoCase(tileIds_[i], stem) && gallery_.Hwnd()) SendMessageW(gallery_.Hwnd(), SettingsGallery::kMsgSelect, i, 0);
        }
    }

    // Round 12 phase 12B: "Apply to other docks…": each checked dock gets this look as its own (one
    // undo entry).
    void ApplyToDocks() {
        const std::vector<size_t> others = OtherDocks();
        if (others.empty()) return;
        Config& c = Cfg();
        std::vector<std::wstring> rows;
        for (size_t k : others) {
            const DockConfig& d = c.docks[k];
            rows.push_back(d.DisplayName() + (d.OwnLook() ? L"" : L" (uses the app look)"));
        }
        std::vector<bool> checked(others.size(), false);
        if (Host().modal) Host().modal(true);
        const bool ok = InputDialog::Choose(Owner(), dockOnly_ ? L"Apply to other docks" : L"Apply to docks",
                                            L"Give these docks this look as their own:", rows, checked);
        if (Host().modal) Host().modal(false);
        if (!ok) return;
        std::vector<size_t> targets;
        for (size_t i = 0; i < others.size(); ++i) {
            if (checked[i]) targets.push_back(others[i]);
        }
        if (targets.empty()) return;
        ChangeLabel(targets.size() == 1 ? L"Applied the look to " + c.docks[targets[0]].DisplayName()
                                        : L"Applied the look to " + std::to_wstring(targets.size()) + L" docks");
        const DockConfig* source = dockOnly_ ? &Dock() : nullptr;
        for (size_t k : targets) c.CopyLook(source, c.docks[k]);
        c.ResolveDockThemes();
        Changed(SettingsScope::Appearance);
        Load();
    }

    void Export() {
        EnsureThemesFolder();
        const std::wstring dir = Theme::Directory();
        const std::wstring& preset = PresetId();
        // A file named after a built-in id would be ignored: suggest another name for those.
        std::wstring suggested = preset.empty() ? L"my-theme" : Theme::FindBuiltIn(preset) ? L"my-" + preset : preset;
        winrt::com_ptr<IFileSaveDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        const COMDLG_FILTERSPEC filters[] = {{L"Theme files (*.json)", L"*.json"}};
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetDefaultExtension(L"json");
        dialog->SetFileName(suggested.c_str());
        dialog->SetTitle(L"Export theme");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        winrt::com_ptr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(folder.put())))) {
            dialog->SetFolder(folder.get());
        }
        if (Host().modal) Host().modal(true);
        const HRESULT shown = dialog->Show(Owner());
        if (Host().modal) Host().modal(false);
        if (FAILED(shown)) return;  // cancelled
        winrt::com_ptr<IShellItem> result;
        PWSTR raw = nullptr;
        if (FAILED(dialog->GetResult(result.put())) || FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        const std::wstring path = raw;
        CoTaskMemFree(raw);
        const std::wstring stem = StemOf(path);
        if (!Theme::Export(path, Look(), stem)) {
            MessageBeep(MB_ICONERROR);
            MessageBoxW(Owner(), (L"Could not write " + path).c_str(), L"WinDock", MB_OK | MB_ICONERROR);
            return;
        }
        // Saved as a theme file: it becomes the preset, with nothing overridden (a dock that uses
        // the app look keeps it: the file only).
        // (Theme ids are at most 64 characters and never a built-in id.)
        if (!ReadOnly() && EqualsNoCase(FolderOf(path), dir) && !stem.empty() && stem.size() <= 64 && !Theme::FindBuiltIn(stem)) {
            PresetId() = stem;
            Mask() = 0;
            Resolve();
            Changed(SettingsScope::Appearance);
        }
        Load();
    }

    std::vector<std::wstring> themeIds_;   // parallel to kThemeCombo's rows (kDockThemeCombo's from row 1)
    std::vector<std::wstring> tileIds_;    // parallel to the gallery's tiles ("" the dock's App look)
    std::vector<std::wstring> tileNames_;
    std::wstring appName_, ownName_;       // the app's preset's name, the dock's own preset's
    SettingsGallery gallery_;
    bool dockOnly_ = false;
    int rowBase_ = kFieldRowBase;          // FieldRow ids: 3000 (Themes) or 6000 (the Look tab)
    int colX_ = 0;
};

}  // namespace

std::unique_ptr<SettingsPage> MakeAppearancePage(SettingsWindow& window) {
    return std::make_unique<AppearancePage>(window, false);
}

std::unique_ptr<SettingsPage> MakeDockThemePage(SettingsWindow& window) {
    return std::make_unique<AppearancePage>(window, true);
}
