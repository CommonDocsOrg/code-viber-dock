// The Standalone widget page (round 6 phase 6B): a "widget" docks[] entry's own window. Round 8
// phase 8C: its Window tab (tab 1; tab 0, Widget, is the widget's generated settings page, item 0
// of the entry): display, position, size, layer, background, lock, in two cards (round 8 phase 8A:
// Flash is the header's). Every change applies at once.

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
    // Round 8 phase 8C: the Window tab (the Widget tab, kTabWidgetSettings, has the widget's own
    // settings); the info line, the Widget card and Open widget settings are gone.
    explicit WidgetWindowPage(SettingsWindow& w) : FormPage(w, L"Window", L'\xE737') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Standalone widget");  // a no-op in a Dock window
        HelpTopic(L"guide-settings.md#window-tab");
        Section(kWwPlaceSection, L"Place and size", L"guide-settings.md#place-and-size");
        Label(kWwDisplayLabel, L"Display");
        Tip(kWwDisplayLabel, L"The display the widget's window is on (its position counts from that display's work area). Default: the primary display.");
        Combo(kWwDisplayCombo, kColX, right - kColX, false);
        NextRow();
        // Round 8 phase 8A: the notes are hints (kWwPositionNote, kWwLayerNote) and tooltips.
        Label(kWwXLabel, L"Position x", L"DIPs; empty: centred", kWwPositionNote);
        Tip(kWwXLabel, L"DIPs from the display's work area's top-left corner; empty: centred.");
        EditBox(kWwXEdit, kColX, kNumberW, 7);
        NextRow();
        Label(kWwYLabel, L"Position y");
        Tip(kWwYLabel, L"DIPs from the top of the work area to the window (empty: centred). Drag the widget to move it.");
        EditBox(kWwYEdit, kColX, kNumberW, 7);
        NextRow();
        // Round 8 phase 8C: the size note is the Width label's hint (the widget's minimum and shape).
        Label(kWwWLabel, L"Width", L"", kWwSizeNote);
        Tip(kWwWLabel, L"DIPs (48 - 4000). The widget's manifest sets the smallest window and whether resizing keeps its "
                       L"shape; dragging the window's edges resizes it too.");
        IntSpin(kWwWEdit, kWwWSpin, kColX, kNumberW, WidgetWindowConfig::kMinSize, WidgetWindowConfig::kMaxSize, 8,
                [](Config&, DockConfig& d) -> int& { return d.widgetWindow.w; }, SettingsScope::Appearance);
        Text(kWwWUnit, CtlKind::Note, L"DIPs", kColX + kNumberW + 8, 80);
        NextRow();
        Label(kWwHLabel, L"Height");
        Tip(kWwHLabel, L"The window's height in DIPs (at least 48). Default: the widget's own size. Drag its edge to resize it.");
        IntSpin(kWwHEdit, kWwHSpin, kColX, kNumberW, WidgetWindowConfig::kMinSize, WidgetWindowConfig::kMaxSize, 8,
                [](Config&, DockConfig& d) -> int& { return d.widgetWindow.h; }, SettingsScope::Appearance);
        Text(kWwHUnit, CtlKind::Note, L"DIPs", kColX + kNumberW + 8, 80);
        NextRow();
        Section(kWwLookSection, L"Layer and look", L"guide-settings.md#layer-and-look");
        Label(kWwLayerLabel, L"Layer", L"", kWwLayerNote);
        Tip(kWwLayerLabel,
            L"Normal: above every window, always. On the desktop, only while the desktop is showing: shown only while the "
            L"desktop is in front (Win+D, a click on the wallpaper). On the desktop, under every window: always on the "
            L"desktop, like folder windows. The dashboard hotkey brings the desktop layers to the front.");
        Combo(kWwLayerCombo, kColX, right - kColX, false);
        NextRow();
        Label(kWwBackgroundLabel, L"Background");
        Tip(kWwBackgroundLabel, L"None (the default): the widget draws on the desktop as it is; Panel: a rounded panel in the theme's colour behind it.");
        Combo(kWwBackgroundCombo, kColX, right - kColX, false);
        NextRow();
        // Round 8 phase 8A: "Flash this widget" (4528) is gone: the header has it. 8C: the move /
        // resize note is the Locked check box's hint.
        Check(kWwLocked, kWwLockedCaption, L"Locked (it can be neither moved nor resized)",
              L"Unlocked: drag it to move it, its edges to resize it", kWwNote);
        const std::wstring how = L"Drag the widget with the left button to move it, drag its edges or corners to resize it (while it is "
                                 L"unlocked); a click without moving is the widget's own. It snaps to the edges of the work area, to "
                                 L"floating docks and to other standalone widgets. Right-click it for its menu.";
        Tip(kWwLockedCaption, how);
        Tip(kWwNote, how);
        NextRow();
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
        std::wstring sizeNote;
        if (item) {
            std::wstring error;
            if (const auto pkg = Widgets::Load(item->id, error)) {
                const Widgets::StandaloneHint& s = pkg->manifest.standalone;
                sizeNote = L"Min. " + std::to_wstring(static_cast<int>(std::max(48.0f, s.minWidth))) + L" × " +
                           std::to_wstring(static_cast<int>(std::max(48.0f, s.minHeight))) +
                           (s.aspect == Widgets::StandaloneHint::Aspect::Keep    ? L", keeps its shape"
                            : s.aspect == Widgets::StandaloneHint::Aspect::Ratio ? L", keeps its ratio"
                                                                                 : L", any shape");
            }
        }
        SetText(kWwSizeNote, sizeNote.empty() ? std::wstring(L"48 - 4000") : sizeNote);
        FillDisplays();
        SetText(kWwXEdit, d.x == DockConfig::kCentred ? std::wstring() : std::to_wstring(d.x));
        SetText(kWwYEdit, d.y == DockConfig::kCentred ? std::wstring() : std::to_wstring(d.y));
        LoadInts();
        SetComboSel(kWwLayerCombo, static_cast<int>(d.layer));
        SetText(kWwLayerNote, LayerNote(d.layer));
        SetComboSel(kWwBackgroundCombo, static_cast<int>(d.widgetWindow.background));
        SetCheck(kWwLocked, d.locked);
        for (int id : {kWwDisplayCombo, kWwXEdit, kWwYEdit, kWwWEdit, kWwWSpin, kWwHEdit, kWwHSpin, kWwLayerCombo,
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
