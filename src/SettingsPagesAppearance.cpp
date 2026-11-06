// The Appearance page: theme preset, export, and one row per Theme::Fields() entry. A field
// edited here is an override (Config::themeOverrides): saved inline in config.json on top of the
// preset, until its Reset button (or "Reset all to preset", or another preset) clears it.

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

#include "SettingsForm.h"
#include "SettingsIds.h"
#include "Theme.h"

namespace {

using namespace SettingsIds;
using Theme::Field;

struct FieldGroup {
    int sectionId;
    const wchar_t* title;
    std::vector<Field> fields;
    int noteId = 0;                  // optional note under the rows
    const wchar_t* note = nullptr;   // its tooltip (round 8 phase 8A)
    const wchar_t* hint = nullptr;   // the one line shown
};

// Rows in this order; a field missing here lands in "Other" (the table stays the source of truth).
std::vector<FieldGroup> FieldGroups() {
    std::vector<FieldGroup> groups = {
        {kSectionFieldsBar, L"Bar",
         {Field::Background, Field::Acrylic, Field::AcrylicBackground, Field::CornerRadius, Field::BarBorder,
          Field::Separator, Field::Accent, Field::Shelf, Field::ShelfSlant, Field::ShelfEdge}},
        {kSectionFieldsLabel, L"Name label",
         {Field::LabelBackground, Field::LabelText, Field::LabelBorder, Field::LabelCornerRadius}},
        {kSectionFieldsDots, L"Indicator dots", {Field::DotRunning, Field::DotActive, Field::DotAttention}},
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

constexpr int kMaxTrackIconSize = 128;  // the spinner goes on to 256 (Config::Sanitize)
std::wstring FormatMagnification(int pos) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.2f×", pos / 20.0);
    return buf;
}

// Global > Themes (the global theme and its per-field overrides), or with `dockOnly` a Dock
// window's Appearance tab (round 8 phase 8B; the Theme tab until then): that dock's preset
// (docks[].themePreset), a pointer to Themes and the dock's Size (icon size, magnification and its
// range: the Behaviour tab's Size card until 8B).
class AppearancePage : public FormPage {
public:
    AppearancePage(SettingsWindow& w, bool dockOnly)
        : FormPage(w, dockOnly ? L"Appearance" : L"Themes", L'\xE790'), dockOnly_(dockOnly) {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        if (dockOnly_) {
            Title(L"Appearance");
            HelpTopic(L"guide-settings.md#appearance-tab");
            Section(kSectionTheme, L"Theme", L"guide-settings.md#dock-theme");
            // Round 8 phase 8A: the note is the label's hint (kDockThemeNote) and its tooltip.
            // Round 10 phase 10A: "App settings" (was "Global settings").
            Label(kDockThemeLabel, L"This dock's theme", L"Its fields: App settings > Themes", kDockThemeNote);
            Tip(kDockThemeLabel, L"\"Same as all docks\" uses the app's theme. The theme's fields (colours, sizes, "
                                 L"effects) are in App settings > Themes: they apply to every dock, on top of this "
                                 L"dock's preset.");
            Combo(kDockThemeCombo, kColX, right - kColX, false);
            NextRow();
            Link(kDockThemeLink, L"<a>Open App settings > Themes</a>", kMargin, right - kMargin);
            NextRow();
            Section(kSectionDock, L"Size", L"guide-settings.md#size");
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
            return;
        }
        Title(L"Themes");
        HelpTopic(L"guide-settings.md#themes");
        // Round 8 phase 8A: a narrower label column, so a field row (swatch, #AARRGGBB, alpha,
        // Reset) fits the window's minimum width beside the scroll bar.
        labelW_ = kThemeLabelW;
        constexpr int cx = kThemeColX;
        Section(kSectionTheme, L"Theme", L"guide-settings.md#choosing-a-theme");
        // Round 8 phase 8D: the hint counts the changes; Export / Open folder / Reset all are the
        // "⋯" menu's.
        Label(kThemeLabel, L"Theme", L"The preset's values", kThemeNote);
        Tip(kThemeLabel, L"A change below overrides the theme (saved in config.json); \x21BA brings back the theme's value, "
                         L"\x22EF > Reset all to preset every one. Colours are #AARRGGBB, the number next to them is the "
                         L"alpha (0 clear, 255 opaque). A dock with a theme of its own (its Dock settings > Appearance) "
                         L"uses it under these changes, which apply to every dock.");
        Combo(kThemeCombo, cx, right - kMoreW - 8 - cx, false);
        Button(kThemeMore, L"\x22EF", right - kMoreW, kMoreW);
        Tip(kThemeMore, L"Export current theme\x2026, Open themes folder, Reset all to preset");
        NextRow();

        // One card per group, its heading an expander ("Bar · 2 changed"); Bar open at first.
        for (const FieldGroup& g : FieldGroups()) {
            UntitledCard();
            HeadingExpander(g.sectionId, g.title, g.sectionId == kSectionFieldsBar, L"guide-settings.md#theme-values");
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

    void Created() override {
        if (!dockOnly_) return;
        ComboAdd(kMagnifyBarCombo, L"Grows once and stays");  // MagnifyBar order
        ComboAdd(kMagnifyBarCombo, L"Follows the pointer");
        ComboAdd(kMagnifyBarCombo, L"Always wide (never grows or shrinks)");
    }

    void Load() override {
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
            return;
        }
        for (size_t i = 0; i < Theme::FieldCount(); ++i) UpdateRow(static_cast<int>(i));
        UpdateCounts();
    }


    void OnCommand(int id, int code, HWND) override {
        if (int field = 0, part = 0; code == CBN_SELCHANGE && SplitId(id, field, part) && part == kFieldValue &&
                                     Theme::Fields()[field].kind == Theme::Kind::Enum) {
            SetChoice(field, ComboSel(id));
            return;
        }
        if (id == kMagnifyBarCombo) {  // round 9 phase 9A
            const int sel = ComboSel(id);
            if (code == CBN_SELCHANGE && sel >= 0 && sel <= static_cast<int>(MagnifyBar::Always) && static_cast<MagnifyBar>(sel) != Dock().magnifyBar) {
                Dock().magnifyBar = static_cast<MagnifyBar>(sel);
                Changed(SettingsScope::Appearance);  // relayout: the steady growth is part of the layout
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
        if (id == kThemeMore) {
            ShowThemeMenu();
            return;
        }
        // Round 8 phase 8D: the ⋯ menu's commands (the old buttons' / link's ids).
        if (id == kExportTheme) {
            Export();
            return;
        }
        if (id == kThemesFolderLink) {
            OpenThemesFolder();
            return;
        }
        if (id == kResetAllTheme) {
            Config& c = Cfg();
            if (c.themeOverrides == 0) return;
            c.themeOverrides = 0;
            c.ResolveTheme();
            Changed(SettingsScope::Appearance);
            Load();  // hides every ↺
            return;
        }
        int field = 0, part = 0;
        if (!SplitId(id, field, part)) return;
        const Theme::FieldInfo& info = Theme::Fields()[field];
        if (part == kFieldReset) {
            ResetField(field);
        } else if (part == kFieldSwatch && info.kind == Theme::Kind::Color) {
            PickColor(field);
        } else if (part == kFieldSwatch && info.kind == Theme::Kind::Bool) {
            Theme::SetBool(Cfg().theme, info, GetCheck(id));
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
    }

    void Track(int id, int pos, bool released) override {
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
        int field = 0, part = 0;
        if (!SplitId(static_cast<int>(di.CtlID), field, part) || part != kFieldSwatch) return false;
        const uint32_t argb = Theme::GetColor(Cfg().theme, Theme::Fields()[field]);
        const DarkMode::Palette& pal = Window().Colors();
        RECT r = di.rcItem;
        HBRUSH back = CreateSolidBrush(Window().SurfaceColor(di.hwndItem));  // a card's fill (round 8 phase 8A)
        FillRect(di.hDC, &r, back);
        DeleteObject(back);
        // Frame, then a checkerboard under the colour so the alpha is visible.
        const bool focus = (di.itemState & ODS_FOCUS) != 0;
        HBRUSH frame = CreateSolidBrush(focus ? pal.accent : pal.textSecondary);
        FrameRect(di.hDC, &r, frame);
        if (focus) {
            RECT r2 = r;
            InflateRect(&r2, -1, -1);
            FrameRect(di.hDC, &r2, frame);
        }
        DeleteObject(frame);
        InflateRect(&r, focus ? -3 : -2, focus ? -3 : -2);
        const unsigned alpha = argb >> 24;
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
    // A Float field's value as the settings UI shows it (value x scale, rounded).
    static int FloatToUi(const Theme::FieldInfo& info, float v) { return static_cast<int>(std::lround(v * info.scale)); }

    static bool SplitId(int id, int& field, int& part) {
        const int rel = id - kFieldRowBase;
        if (rel < 0 || rel >= static_cast<int>(Theme::FieldCount()) * kFieldRowStride) return false;
        field = rel / kFieldRowStride;
        part = rel % kFieldRowStride;
        return true;
    }

    static constexpr int kThemeLabelW = 156, kThemeColX = kMargin + kThemeLabelW + 8;  // round 8 phase 8A
    static constexpr int kMoreW = 32;  // round 8 phase 8D: the "⋯" button beside the theme combo

    void FieldRowCtl(int field) {
        const Theme::FieldInfo& info = Theme::Fields()[field];
        const int right = Width() - kMargin;
        constexpr int kResetW = 24, colX = kThemeColX;  // the page's own (narrower) columns
        switch (info.kind) {
            case Theme::Kind::Color: {
                Label(FieldRow(field, kFieldLabel), info.label);
                // The swatch's window text is its accessible name (never drawn).
                Add(FieldRow(field, kFieldSwatch), CtlKind::Swatch, WC_BUTTONW, info.label, WS_TABSTOP | BS_OWNERDRAW,
                    RECT{colX, y_ + 5, colX + 36, y_ + kRowH - 5});
                Ctl& hex = Add(FieldRow(field, kFieldHex), CtlKind::Edit, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL,
                               RECT{colX + 44, y_ + 4, colX + 44 + 92, y_ + kRowH - 4});
                hex.exStyle = WS_EX_CLIENTEDGE;
                hex.hi = 9;
                Spin(FieldRow(field, kFieldValue), FieldRow(field, kFieldSpin), colX + 144, 60, 0, 255, 5);
                break;
            }
            case Theme::Kind::Float:
                Label(FieldRow(field, kFieldLabel), info.label);
                Spin(FieldRow(field, kFieldValue), FieldRow(field, kFieldSpin), colX, 72, FloatToUi(info, info.min),
                     FloatToUi(info, info.max), info.scale > 1.0f ? 5 : 1);
                Text(FieldRow(field, kFieldUnit), CtlKind::Note, info.unit, colX + 80, 96);
                break;
            case Theme::Kind::Bool:
                Check(FieldRow(field, kFieldSwatch), FieldRow(field, kFieldHex), info.label);
                break;
            case Theme::Kind::Enum:
                Label(FieldRow(field, kFieldLabel), info.label);
                Combo(FieldRow(field, kFieldValue), colX, 168, false);
                break;
        }
        // Round 8 phase 8D: a 24 DIP "↺" (was a "Reset" button), shown only while overridden.
        Add(FieldRow(field, kFieldReset), CtlKind::Button, WC_BUTTONW, L"\x21BA", WS_TABSTOP | BS_PUSHBUTTON,
            RECT{right - kResetW, y_ + 4, right, y_ + 4 + kResetW});
        Tip(FieldRow(field, kFieldReset), L"Reset to the preset's value");
        NextRow();
    }

    // Round 8 phase 8D: each group's heading counts its overridden fields ("Bar · 2 changed") and
    // the Theme label's hint all of them.
    void UpdateCounts() {
        const uint64_t over = Cfg().themeOverrides;
        int total = 0;
        for (const FieldGroup& g : FieldGroups()) {
            int n = 0;
            for (Field f : g.fields) n += (over & Theme::Bit(f)) ? 1 : 0;
            total += n;
            SetExpanderText(g.sectionId, n > 0 ? std::wstring(g.title) + L" \x00B7 " + std::to_wstring(n) + L" changed" : std::wstring(g.title));
        }
        SetText(kThemeNote, total == 0   ? std::wstring(L"The preset's values")
                            : total == 1 ? std::wstring(L"1 value changed")
                                         : std::to_wstring(total) + L" values changed");
    }

    // The ⋯ menu under its button: Export current theme…, Open themes folder, Reset all to preset.
    void ShowThemeMenu() {
        HWND button = Item(kThemeMore);
        HMENU menu = CreatePopupMenu();
        if (!button || !menu) {
            if (menu) DestroyMenu(menu);
            return;
        }
        AppendMenuW(menu, MF_STRING, kExportTheme, L"Export current theme\x2026");
        AppendMenuW(menu, MF_STRING, kThemesFolderLink, L"Open themes folder");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (Cfg().themeOverrides == 0 ? MF_GRAYED : 0), kResetAllTheme, L"Reset all to preset");
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

    // Config -> one row (and its Reset button).
    void UpdateRow(int field) {
        const Config& c = Cfg();
        const Theme::FieldInfo& info = Theme::Fields()[field];
        switch (info.kind) {
            case Theme::Kind::Color: {
                const uint32_t argb = Theme::GetColor(c.theme, info);
                SetText(FieldRow(field, kFieldHex), FormatArgbColor(argb));
                SetSpin(FieldRow(field, kFieldSpin), static_cast<int>(argb >> 24));
                if (HWND swatch = Item(FieldRow(field, kFieldSwatch))) InvalidateRect(swatch, nullptr, FALSE);
                break;
            }
            case Theme::Kind::Float:
                SetSpin(FieldRow(field, kFieldSpin), FloatToUi(info, Theme::GetFloat(c.theme, info)));
                break;
            case Theme::Kind::Bool:
                SetCheck(FieldRow(field, kFieldSwatch), Theme::GetBool(c.theme, info));
                break;
            case Theme::Kind::Enum: {
                const int id = FieldRow(field, kFieldValue);
                if (HWND combo = Item(id); combo && SendMessageW(combo, CB_GETCOUNT, 0, 0) == 0) {
                    for (size_t i = 0; i < info.count; ++i) ComboAdd(id, info.choices[i]);
                }
                SetComboSel(id, Theme::GetInt(c.theme, info));
                break;
            }
        }
        Show(FieldRow(field, kFieldReset), (c.themeOverrides & Theme::Bit(info.field)) != 0);
    }

    // The field was edited in Cfg().theme: mark it overridden and apply.
    void Override(int field) {
        Config& c = Cfg();
        c.themeOverrides |= Theme::Bit(Theme::Fields()[field].field);
        Changed(SettingsScope::Appearance);
        UpdateRow(field);
        UpdateCounts();
    }

    void SetColor(int field, uint32_t argb) {
        const Theme::FieldInfo& info = Theme::Fields()[field];
        if (Theme::GetColor(Cfg().theme, info) == argb) {
            UpdateRow(field);  // normalises the text
            return;
        }
        Theme::SetColor(Cfg().theme, info, argb);
        Override(field);
    }

    // The number of a row: a colour's alpha or a float field's value.
    void SetValue(int field, int value) {
        const Theme::FieldInfo& info = Theme::Fields()[field];
        if (info.kind == Theme::Kind::Color) {
            const uint32_t argb = Theme::GetColor(Cfg().theme, info);
            SetColor(field, (static_cast<uint32_t>(std::clamp(value, 0, 255)) << 24) | (argb & 0x00FFFFFFu));
        } else if (info.kind == Theme::Kind::Float) {
            const float v = std::clamp(static_cast<float>(value) / info.scale, info.min, info.max);
            if (Theme::GetFloat(Cfg().theme, info) == v) {
                UpdateRow(field);
                return;
            }
            Theme::SetFloat(Cfg().theme, info, v);
            Override(field);
        }
    }

    // An Enum row's combo selection.
    void SetChoice(int field, int index) {
        const Theme::FieldInfo& info = Theme::Fields()[field];
        if (index < 0 || static_cast<size_t>(index) >= info.count || Theme::GetInt(Cfg().theme, info) == index) {
            UpdateRow(field);
            return;
        }
        Theme::SetInt(Cfg().theme, info, index);
        Override(field);
    }

    void ResetField(int field) {
        Config& c = Cfg();
        const uint64_t bit = Theme::Bit(Theme::Fields()[field].field);
        if (!(c.themeOverrides & bit)) return;
        c.themeOverrides &= ~bit;
        c.ResolveTheme();  // copies the preset's value back (and only into fields without overrides)
        Changed(SettingsScope::Appearance);
        const bool hadFocus = GetFocus() == Item(FieldRow(field, kFieldReset));
        UpdateRow(field);  // hides the ↺ button
        UpdateCounts();
        if (hadFocus) {
            // The keyboard focus stays on the row: its swatch / check box, or a float's number box.
            const Theme::Kind kind = Theme::Fields()[field].kind;
            const int first = kind == Theme::Kind::Float || kind == Theme::Kind::Enum ? kFieldValue : kFieldSwatch;
            if (HWND h = Item(FieldRow(field, first))) SetFocus(h);
        }
    }

    void PickColor(int field) {
        static COLORREF custom[16] = {  // the dialog's custom colours, kept for the session
            RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
            RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
            RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
            RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255)};
        const Theme::FieldInfo& info = Theme::Fields()[field];
        const uint32_t argb = Theme::GetColor(Cfg().theme, info);
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = Owner();
        cc.rgbResult = RgbOf(argb);
        cc.lpCustColors = custom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
        if (!ChooseColorW(&cc)) return;
        // RGB replaced, alpha kept.
        const uint32_t rgb = (static_cast<uint32_t>(GetRValue(cc.rgbResult)) << 16) |
                             (static_cast<uint32_t>(GetGValue(cc.rgbResult)) << 8) | GetBValue(cc.rgbResult);
        SetColor(field, (argb & 0xFF000000u) | rgb);
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

    // Built-ins by name, then the theme files by name; a preset that is not there is listed as
    // "<id> (missing)" so the user sees what config.json names.
    void FillThemes() {
        HWND combo = Item(kThemeCombo);
        HWND dock = Item(kDockThemeCombo);  // Dock > Appearance (only it)
        if (!combo && !dock) return;
        const std::wstring& current = Cfg().themePreset;
        std::vector<std::wstring> names;
        themeIds_.clear();
        size_t count = 0;
        const Theme::Preset* presets = Theme::BuiltIns(count);
        for (size_t i = 0; i < count; ++i) {
            themeIds_.push_back(presets[i].id);
            names.push_back(presets[i].name);
        }
        for (const Theme::UserTheme& t : Theme::ListUserThemes()) {
            themeIds_.push_back(t.id);
            names.push_back(t.valid ? t.name : t.name + L" (unreadable)");
        }
        int sel = -1;
        for (size_t i = 0; i < themeIds_.size(); ++i) {
            if (EqualsNoCase(themeIds_[i], current)) {
                sel = static_cast<int>(i);
                break;
            }
        }
        if (sel < 0) {
            themeIds_.push_back(current);
            names.push_back(current.empty() ? std::wstring(L"None (dark defaults)") : current + L" (missing)");
            sel = static_cast<int>(themeIds_.size()) - 1;
        }
        if (combo) {
            SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
            SendMessageW(combo, CB_RESETCONTENT, 0, 0);
            for (const std::wstring& n : names) ComboAdd(kThemeCombo, n.c_str());
            SetComboSel(kThemeCombo, sel);
            SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(combo, nullptr, TRUE);
        }

        // This dock's theme: "Same as all docks", then the same list (an unknown id: "(missing)").
        if (!dock) return;
        const std::wstring& own = Dock().themePreset;
        int dockSel = 0;
        for (size_t i = 0; i < themeIds_.size() && !own.empty(); ++i) {
            if (EqualsNoCase(themeIds_[i], own)) {
                dockSel = static_cast<int>(i) + 1;
                break;
            }
        }
        if (!own.empty() && dockSel == 0) {
            themeIds_.push_back(own);
            names.push_back(own + L" (missing)");
            dockSel = static_cast<int>(themeIds_.size());
        }
        SendMessageW(dock, CB_RESETCONTENT, 0, 0);
        ComboAdd(kDockThemeCombo, L"Same as all docks");
        for (const std::wstring& n : names) ComboAdd(kDockThemeCombo, n.c_str());
        SetComboSel(kDockThemeCombo, dockSel);
    }

    // The window's dock's own preset (docks[].themePreset; "" = the global theme).
    void SelectDockPreset(const std::wstring& id) {
        Config& c = Cfg();
        if (id == Dock().themePreset) return;
        Dock().themePreset = id;
        c.ResolveDockThemes();
        Changed(SettingsScope::Appearance);
    }

    void Export() {
        Config& c = Cfg();
        EnsureThemesFolder();
        const std::wstring dir = Theme::Directory();
        // A file named after a built-in id would be ignored: suggest another name for those.
        std::wstring suggested = c.themePreset.empty() ? L"my-theme"
                                 : Theme::FindBuiltIn(c.themePreset) ? L"my-" + c.themePreset
                                                                      : c.themePreset;
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
        if (FAILED(dialog->Show(Owner()))) return;  // cancelled
        winrt::com_ptr<IShellItem> result;
        PWSTR raw = nullptr;
        if (FAILED(dialog->GetResult(result.put())) || FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        const std::wstring path = raw;
        CoTaskMemFree(raw);
        const std::wstring stem = StemOf(path);
        if (!Theme::Export(path, c.theme, stem)) {
            MessageBeep(MB_ICONERROR);
            MessageBoxW(Owner(), (L"Could not write " + path).c_str(), L"WinDock", MB_OK | MB_ICONERROR);
            return;
        }
        // Saved as a theme file: it becomes the preset, with nothing overridden.
        // (Theme ids are at most 64 characters and never a built-in id.)
        if (EqualsNoCase(FolderOf(path), dir) && !stem.empty() && stem.size() <= 64 && !Theme::FindBuiltIn(stem)) {
            c.themePreset = stem;
            c.themeOverrides = 0;
            c.ResolveTheme();
            Changed(SettingsScope::Appearance);
        }
        Load();
    }

    std::vector<std::wstring> themeIds_;  // parallel to the combo items
    bool dockOnly_ = false;
};

}  // namespace

std::unique_ptr<SettingsPage> MakeAppearancePage(SettingsWindow& window) {
    return std::make_unique<AppearancePage>(window, false);
}

std::unique_ptr<SettingsPage> MakeDockThemePage(SettingsWindow& window) {
    return std::make_unique<AppearancePage>(window, true);
}
