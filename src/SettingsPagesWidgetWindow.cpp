// The Standalone widget page (round 6 phase 6B): a "widget" docks[] entry's own window (one page,
// no tabs): which widget (its own settings are the generated Item page: item 0 of the entry),
// display, position, size, layer, background, lock (round 8 phase 8A: Flash is the header's).
// Every change applies at once.

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "ContextMenu.h"
#include "SettingsForm.h"
#include "SettingsIds.h"
#include "WidgetPackage.h"

namespace {

using namespace SettingsIds;

constexpr int kNumberW = 88;

class WidgetWindowPage : public FormPage {
public:
    explicit WidgetWindowPage(SettingsWindow& w) : FormPage(w, L"Standalone widget", L'\xE737') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Standalone widget");
        Text(kWwInfo, CtlKind::Note, L"", kMargin, right - kMargin, 20);
        NextRow(22);
        Section(kWwWidgetSection, L"Widget");
        Label(kWwWidgetLabel, L"Widget");
        Text(kWwWidgetName, CtlKind::Value, L"", kColX, right - kColX - 200);
        Button(kWwOpenWidget, L"Open widget settings", right - 190, 190);
        NextRow();
        Section(kWwPlaceSection, L"Place and size");
        Label(kWwDisplayLabel, L"Display");
        Combo(kWwDisplayCombo, kColX, right - kColX, false);
        NextRow();
        // Round 8 phase 8A: the notes are hints (kWwPositionNote, kWwLayerNote) and tooltips.
        Label(kWwXLabel, L"Position x", L"DIPs; empty: centred", kWwPositionNote);
        Tip(kWwXLabel, L"DIPs from the display's work area's top-left corner; empty: centred.");
        EditBox(kWwXEdit, kColX, kNumberW, 7);
        NextRow();
        Label(kWwYLabel, L"Position y");
        EditBox(kWwYEdit, kColX, kNumberW, 7);
        NextRow();
        Label(kWwWLabel, L"Width");
        IntSpin(kWwWEdit, kWwWSpin, kColX, kNumberW, WidgetWindowConfig::kMinSize, WidgetWindowConfig::kMaxSize, 8,
                [](Config&, DockConfig& d) -> int& { return d.widgetWindow.w; }, SettingsScope::Appearance);
        NextRow();
        Label(kWwHLabel, L"Height");
        IntSpin(kWwHEdit, kWwHSpin, kColX, kNumberW, WidgetWindowConfig::kMinSize, WidgetWindowConfig::kMaxSize, 8,
                [](Config&, DockConfig& d) -> int& { return d.widgetWindow.h; }, SettingsScope::Appearance);
        NextRow();
        Text(kWwSizeNote, CtlKind::Note, L"", kColX, right - kColX, 40);
        NextRow(42);
        Section(kWwLookSection, L"Layer and look");
        Label(kWwLayerLabel, L"Layer", L"", kWwLayerNote);
        Tip(kWwLayerLabel,
            L"Normal: above every window, always. On the desktop, only while the desktop is showing: shown only while the "
            L"desktop is in front (Win+D, a click on the wallpaper). On the desktop, under every window: always on the "
            L"desktop, like folder windows. The dashboard hotkey brings the desktop layers to the front.");
        Combo(kWwLayerCombo, kColX, right - kColX, false);
        NextRow();
        Label(kWwBackgroundLabel, L"Background");
        Combo(kWwBackgroundCombo, kColX, right - kColX, false);
        NextRow();
        Check(kWwLocked, kWwLockedCaption, L"Locked (it can be neither moved nor resized)");
        NextRow(kRowH);
        // Round 8 phase 8A: "Flash this widget" (4528) is gone: the header has it.
        Text(kWwNote, CtlKind::Note, L"Drag it to move it, its edges to resize it; right-click it for its menu.", kMargin,
             right - kMargin, 20);
        Tip(kWwNote, L"Drag the widget with the left button to move it, drag its edges or corners to resize it (while it is "
                     L"unlocked); a click without moving is the widget's own. It snaps to the edges of the work area, to "
                     L"floating docks and to other standalone widgets. Right-click it for its menu.");
        NextRow(22);
    }

    void Created() override {
        ComboAdd(kWwLayerCombo, L"Normal (above windows)");  // DockLayer order
        ComboAdd(kWwLayerCombo, L"On the desktop, only while the desktop is showing");
        ComboAdd(kWwLayerCombo, L"On the desktop, under every window");
        ComboAdd(kWwBackgroundCombo, L"None (the widget over the wallpaper)");
        ComboAdd(kWwBackgroundCombo, L"Panel (the theme's bar background)");
    }

    void Load() override {
        const DockConfig& d = Dock();
        const bool ww = d.IsWidgetWindow();
        const DockItemEntry* item = d.WidgetItem();
        std::wstring name = L"(none)", sizeNote;
        if (item) {
            std::wstring error;
            const auto pkg = Widgets::Load(item->id, error);
            name = (pkg ? pkg->manifest.name : item->id) + L" (" + item->id + (pkg ? L"" : L", not installed") + L")";
            if (pkg) {
                const Widgets::StandaloneHint& s = pkg->manifest.standalone;
                sizeNote = L"DIPs. This widget needs at least " + std::to_wstring(static_cast<int>(std::max(48.0f, s.minWidth))) +
                           L" × " + std::to_wstring(static_cast<int>(std::max(48.0f, s.minHeight))) +
                           (s.aspect == Widgets::StandaloneHint::Aspect::Keep    ? L"; resizing keeps its shape."
                            : s.aspect == Widgets::StandaloneHint::Aspect::Ratio ? L"; resizing keeps its width / height ratio."
                                                                                 : L"; any shape.");
            }
        }
        SetText(kWwInfo, ww ? L"The standalone widget " + d.id + L": a widget in a window of its own on the desktop."
                            : L"This is not a standalone widget.");
        SetText(kWwWidgetName, name);
        SetText(kWwSizeNote, sizeNote.empty() ? std::wstring(L"DIPs (48 - 4000).") : sizeNote);
        FillDisplays();
        SetText(kWwXEdit, d.x == DockConfig::kCentred ? std::wstring() : std::to_wstring(d.x));
        SetText(kWwYEdit, d.y == DockConfig::kCentred ? std::wstring() : std::to_wstring(d.y));
        LoadInts();
        SetComboSel(kWwLayerCombo, static_cast<int>(d.layer));
        SetText(kWwLayerNote, LayerNote(d.layer));
        SetComboSel(kWwBackgroundCombo, static_cast<int>(d.widgetWindow.background));
        SetCheck(kWwLocked, d.locked);
        for (int id : {kWwOpenWidget, kWwDisplayCombo, kWwXEdit, kWwYEdit, kWwWEdit, kWwWSpin, kWwHEdit, kWwHSpin, kWwLayerCombo,
                       kWwBackgroundCombo, kWwLocked}) {
            Enable(id, ww);
        }
    }

    void OnCommand(int id, int code, HWND) override {
        DockConfig& d = Dock();
        if (!d.IsWidgetWindow()) return;
        if (code == CBN_SELCHANGE) {
            switch (id) {
                case kWwDisplayCombo: {
                    const int m = std::max(0, ComboSel(id));
                    if (d.monitor == m || (m == 0 && d.monitor == DockConfig::kMonitorPrimary)) return;
                    d.monitor = m;
                    break;
                }
                case kWwLayerCombo: {
                    const DockLayer layer = static_cast<DockLayer>(std::clamp(ComboSel(id), 0, 2));
                    if (layer == d.layer) return;
                    d.layer = layer;
                    SetText(kWwLayerNote, LayerNote(layer));
                    break;
                }
                case kWwBackgroundCombo: {
                    const WidgetBackground b = static_cast<WidgetBackground>(std::clamp(ComboSel(id), 0, 1));
                    if (b == d.widgetWindow.background) return;
                    d.widgetWindow.background = b;
                    break;
                }
                default:
                    return;
            }
            Changed(SettingsScope::Appearance);
            return;
        }
        if (code != BN_CLICKED) return;
        switch (id) {
            case kWwOpenWidget:
                if (Host().openItem && DockIndex() != SIZE_MAX) Host().openItem(DockIndex(), 0);
                return;
            case kWwLocked:
                d.locked = GetCheck(id);
                Changed(SettingsScope::Appearance);  // the window and the script's dock.locked follow
                return;
            default:
                return;
        }
    }

    void Commit(int id) override {
        DockConfig& d = Dock();
        if (!d.IsWidgetWindow() || (id != kWwXEdit && id != kWwYEdit)) return;
        int& field = id == kWwXEdit ? d.x : d.y;
        const std::wstring text = GetText(id);
        int v = DockConfig::kCentred;
        if (!text.empty() && !ReadInt(id, -16000, 16000, v)) {
            SetText(id, field == DockConfig::kCentred ? std::wstring() : std::to_wstring(field));  // not a number
            return;
        }
        if (v == field) return;
        field = v;
        Changed(SettingsScope::Appearance);
    }

private:
    // The Layer label's hint (round 8 phase 8A: one line, the tooltip explains).
    static const wchar_t* LayerNote(DockLayer layer) {
        switch (layer) {
            case DockLayer::Desktop: return L"Only while the desktop shows";
            case DockLayer::Normal: return L"Above every window";
            case DockLayer::Behind:
            default: return L"Under every window";
        }
    }

    // "Primary", "Display 2", … (DockManager's order), a configured one that is not connected too.
    void FillDisplays() {
        HWND combo = Item(kWwDisplayCombo);
        if (!combo) return;
        const int count = std::max(1, GetSystemMetrics(SM_CMONITORS));
        const int want = std::max(Dock().monitor, 0);  // "primary": the first entry
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < std::max(count, want + 1); ++i) {
            std::wstring name = SettingsDisplayName(i);
            if (i >= count) name += L" (not connected)";
            ComboAdd(kWwDisplayCombo, name.c_str());
        }
        SetComboSel(kWwDisplayCombo, want);
    }
};

}  // namespace

std::unique_ptr<SettingsPage> MakeWidgetWindowPage(SettingsWindow& window) { return std::make_unique<WidgetWindowPage>(window); }
