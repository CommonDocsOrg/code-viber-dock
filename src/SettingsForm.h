#pragma once

// Shared by the settings page sources (SettingsPages*.cpp): the FormPage helper base class and
// the page factories. Not part of the settings window's interface.

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "SettingsWindow.h"

// A page with integer spinners bound to Config fields (applied on every up-down step, on Enter and
// on focus loss) and trackbars applied on release.
class FormPage : public SettingsPage {
public:
    using SettingsPage::SettingsPage;

protected:
    // The bound int of the config or of the window's dock (Dock(); round 5 phase B).
    using IntRef = int& (*)(Config&, DockConfig&);
    struct IntField {
        int edit;
        int spin;
        int lo;
        int hi;
        IntRef field;
        SettingsScope scope;
    };

    // `field` returns the bound int (a captureless lambda, e.g. [](Config&, DockConfig& d) -> int& { return d.x; }).
    void IntSpin(int edit, int spin, int x, int w, int lo, int hi, int step, IntRef field, SettingsScope scope) {
        Spin(edit, spin, x, w, lo, hi, step);
        ints_.push_back(IntField{edit, spin, lo, hi, field, scope});
    }

    void LoadInts() {
        for (const IntField& f : ints_) SetSpin(f.spin, f.field(Cfg(), Dock()));
        const RunningConfig r = RunningValues();
        for (const RunIntField& f : runInts_) {
            RunningConfig copy = r;
            SetSpin(f.spin, f.field(copy));
        }
    }

    // --- round 13 phase 13D: ints of a running item's settings (RunningConfig) ---
    using RunIntRef = int& (*)(RunningConfig&);
    struct RunIntField {
        int edit;
        int spin;
        int lo;
        int hi;
        RunIntRef field;
        SettingsScope scope;
    };
    // Bound like IntSpin, to an int of RunningItem()'s settings.
    void RunIntSpin(int edit, int spin, int x, int w, int lo, int hi, int step, RunIntRef field, SettingsScope scope) {
        Spin(edit, spin, x, w, lo, hi, step);
        runInts_.push_back(RunIntField{edit, spin, lo, hi, field, scope});
    }
    // The running item these settings are for: the window's dock's first one (an Item window: its
    // own item). Null: none (the values are the defaults, nothing is written).
    virtual DockItemEntry* RunningItem() {
        for (DockItemEntry& e : Dock().items) {
            if (e.Kind() == ItemKind::Running) return &e;
        }
        return nullptr;
    }
    RunningConfig RunningValues() {
        const DockItemEntry* e = RunningItem();
        return e ? RunningOf(*e) : RunningConfig{};
    }
    // Writes `r` into the running item and tells the host (`scope`) when something changed.
    bool StoreRunning(const RunningConfig& r, SettingsScope scope) {
        DockItemEntry* e = RunningItem();
        if (!e || RunningOf(*e) == r) return false;
        SetRunningSettings(*e, r);
        Changed(scope);
        return true;
    }
    void SetRunInt(const RunIntField& f, int value) {
        value = std::clamp(value, f.lo, f.hi);
        SetSpin(f.spin, value);
        RunningConfig r = RunningValues();
        int& field = f.field(r);
        if (field != value) {
            field = value;
            StoreRunning(r, f.scope);
        }
        IntChanged(f.spin, value);
    }

    // Applies `value` to the field (if it changed) and tells the host.
    void SetInt(const IntField& f, int value) {
        value = std::clamp(value, f.lo, f.hi);
        SetSpin(f.spin, value);
        int& field = f.field(Cfg(), Dock());
        if (field != value) {
            field = value;
            Changed(f.scope);
        }
        IntChanged(f.spin, value);
    }

    // A bound value changed (from its spinner or edit).
    virtual void IntChanged(int /*spin*/, int /*value*/) {}
    // WM_HSCROLL from a trackbar; `released` = apply now (see TrackReleased).
    virtual void Track(int /*id*/, int /*pos*/, bool /*released*/) {}
    // An up-down that is not bound with IntSpin moved (SB_THUMBPOSITION / SB_ENDSCROLL).
    virtual void Spun(int /*spin*/, int /*pos*/) {}
    virtual void Commit(int /*id*/) {}

    void OnScroll(int id, int code) override {
        for (const IntField& f : ints_) {
            if (f.spin == id) {
                if (code == SB_THUMBPOSITION || code == SB_ENDSCROLL) SetInt(f, GetSpin(id));
                return;
            }
        }
        for (const RunIntField& f : runInts_) {
            if (f.spin == id) {
                if (code == SB_THUMBPOSITION || code == SB_ENDSCROLL) SetRunInt(f, GetSpin(id));
                return;
            }
        }
        if (const Ctl* c = Find(id); c && c->kind == CtlKind::Spin) {
            if (code == SB_THUMBPOSITION || code == SB_ENDSCROLL) Spun(id, GetSpin(id));
            return;
        }
        Track(id, GetTrack(id), TrackReleased(code));
    }

    void OnCommit(int id) override {
        for (const IntField& f : ints_) {
            if (f.edit == id) {
                int value = 0;
                if (ReadInt(f.edit, f.lo, f.hi, value)) {
                    SetInt(f, value);
                } else {
                    SetSpin(f.spin, f.field(Cfg(), Dock()));  // not a number: back to the current value
                }
                return;
            }
        }
        for (const RunIntField& f : runInts_) {
            if (f.edit == id) {
                int value = 0;
                if (ReadInt(f.edit, f.lo, f.hi, value)) {
                    SetRunInt(f, value);
                } else {
                    RunningConfig r = RunningValues();
                    SetSpin(f.spin, f.field(r));
                }
                return;
            }
        }
        Commit(id);
    }

    // Trackbars apply on release: the end of a drag or of a key / channel-click sequence
    // (TB_ENDTRACK), or a step with no button or key held (the mouse wheel).
    static bool TrackReleased(int code) {
        if (code == TB_ENDTRACK || code == TB_THUMBPOSITION) return true;
        if (code == TB_THUMBTRACK) return false;
        if (GetKeyState(VK_LBUTTON) < 0) return false;
        for (int vk : {VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_PRIOR, VK_NEXT, VK_HOME, VK_END}) {
            if (GetKeyState(vk) < 0) return false;
        }
        return true;
    }

    // A one-line edit box in the current row (committed on Enter / focus loss: OnCommit).
    void EditBox(int id, int x, int w, int maxLength) {
        Ctl& e = Add(id, CtlKind::Edit, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, RECT{x, y_ + 4, x + w, y_ + kRowH - 4});
        e.exStyle = WS_EX_CLIENTEDGE;
        e.hi = maxLength;
    }

    int ComboSel(int id) const { return static_cast<int>(SendMessageW(Item(id), CB_GETCURSEL, 0, 0)); }
    void SetComboSel(int id, int index) { SendMessageW(Item(id), CB_SETCURSEL, static_cast<WPARAM>(index), 0); }
    void ComboAdd(int id, const wchar_t* text) { SendMessageW(Item(id), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text)); }

private:
    std::vector<IntField> ints_;
    std::vector<RunIntField> runInts_;
};

// Page factories (one source file each, besides SettingsPages.cpp).
std::unique_ptr<SettingsPage> MakeAppearancePage(SettingsWindow& window);  // SettingsPagesAppearance.cpp (Global > Themes)
std::unique_ptr<SettingsPage> MakeDockThemePage(SettingsWindow& window);   // SettingsPagesAppearance.cpp (Dock > Theme)
std::unique_ptr<SettingsPage> MakeAnimationPage(SettingsWindow& window);   // SettingsPagesMore.cpp
std::unique_ptr<SettingsPage> MakeContentsPage(SettingsWindow& window);    // SettingsPagesContents.cpp (Dock > Contents)
std::unique_ptr<SettingsPage> MakeLayoutPage(SettingsWindow& window);      // SettingsPagesLayout.cpp (Global > Docks and displays)
std::unique_ptr<SettingsPage> MakeWidgetsPage(SettingsWindow& window);     // SettingsPagesWidgets.cpp
// SettingsPagesWidgets.cpp: the generated settings of a widget / clock / weather item. Round 10
// phase 10C: two pages, "Content" and "Appearance", when its schema has fields of both sections
// (SettingField::section), else one page with every field (named after the section it uses).
// `standaloneItem` >= 0 (round 8 phase 8C): the widget tabs of a standalone widget's Dock window,
// item `standaloneItem` of its entry (Reset to defaults and Console… as each page's last card, the
// ids of the Item window's footer).
std::vector<std::unique_ptr<SettingsPage>> MakeWidgetSettingsPages(SettingsWindow& window, int standaloneItem = -1);
// The clock's / weather item's sentence on its Item window's header (round 8 phase 8C).
std::wstring BuiltInItemDescription(ItemKind kind);
std::unique_ptr<SettingsPage> MakeTrayPage(SettingsWindow& window);            // SettingsPagesTray.cpp (Global > Tray icons)
std::unique_ptr<SettingsPage> MakeTrayItemPage(SettingsWindow& window);        // SettingsPagesTray.cpp (a tray item's Item window)
std::unique_ptr<SettingsPage> MakeRunningItemPage(SettingsWindow& window);     // SettingsPagesRunning.cpp (the running item's Item window; round 13 phase 13D)
std::unique_ptr<SettingsPage> MakeNotificationsPage(SettingsWindow& window);   // SettingsPagesNotifications.cpp
std::unique_ptr<SettingsPage> MakeItemSettingsPage(SettingsWindow& window);    // SettingsPagesItems.cpp ("Folders and groups")
std::unique_ptr<SettingsPage> MakeCalendarPage(SettingsWindow& window);        // SettingsPagesCalendar.cpp
std::unique_ptr<SettingsPage> MakeFolderWindowPage(SettingsWindow& window);    // SettingsPagesFolderWindow.cpp
std::unique_ptr<SettingsPage> MakeWidgetWindowPage(SettingsWindow& window);    // SettingsPagesWidgetWindow.cpp (round 6 phase 6B)

// One line about a clock / weather item (the Contents tab's details, the generated page's item list;
// SettingsPagesWidgets.cpp): "%H:%M, date, Tokyo Standard Time, “Tokyo”" / "Seattle, °C".
std::wstring ClockDetail(const ClockConfig& c);
std::wstring WeatherDetail(const WeatherConfig& w);

// Round 10 phase 10B: the generated page's values and apply path (SettingsPagesWidgets.cpp), shared
// with the Contents tab's inspector so both editors read and write an item's settings the same way.
namespace Widgets {
struct Manifest;
struct SettingField;
}
// The values a clock / weather / script widget item shows (JSON object text): a clock's / weather
// item's effective settings (its own over the defaults), a widget's merged over its schema's defaults.
std::wstring ItemSettingsValues(const Config& config, const DockItemEntry& item, const Widgets::Manifest* schema);
// Writes keys (each value as JSON text) into the item's `settings` object (a clock / weather item
// without one starts from its effective values). False when nothing changed; the caller then tells
// the host (SettingsScope::Pins).
bool StoreItemSettings(const Config& config, DockItemEntry& item, const std::vector<std::pair<std::wstring, std::wstring>>& keys);
// A field's `visibleIf` holds in `valuesJson` (ItemSettingsValues).
bool SettingFieldVisible(const Widgets::SettingField& field, const std::wstring& valuesJson);

// The icon of a pin entry as the Contents tab's item strip shows it (its custom icon, a shortcut's
// icon, the file's / app's icon, the Recycle Bin's), `px` square (above 32 px from the system's
// 48 px image list); null for kinds without one (separators, a group without an icon of its own).
// The caller destroys it (SettingsPagesMore.cpp).
HICON SettingsItemIcon(const AppIdentity::ResolvedPin* pin, const DockItemEntry& e, int px);
