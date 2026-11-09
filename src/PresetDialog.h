#pragma once

// Round 9 phase 9C: the preset chooser (WinDock.Presets). A dialog-like window, 720 x 480 DIPs, modal
// to its owner (a settings window is disabled while it is open; the welcome variant is owned by the
// app window): the presets at the left (name + tagline), at the right the chosen one's name, its
// description, a preview (a silhouette of where it sits and the list of what it puts on the dock),
// a strip drawn by SettingsPreview from the preset's dock and theme (a private Config copy: the live
// docks are never touched), the pins check box, Apply / Cancel. Three variants: Apply (to one dock:
// "Keep my pinned apps and folders"), New dock ("Copy my pinned apps from the first dock") and
// Welcome (the first run and About's "Show the welcome dialog": "Use this dock" / "Skip (basic
// dock)"). Keyboard: Up / Down in the list, Enter = Apply, Esc = Cancel (Skip), Tab.
//
// It runs in the app's message loop (no nested loop: automation that sends to the app keeps
// working); App::PreTranslateMessage hands it its keys.
//
// Round 12 phase 12C: 720 x 504 DIPs; a kind row over the list (Layouts / Looks / Mine, see
// PresetKind): Looks lists every theme preset and theme file with a tagline from its Quick look
// values and applies only the look (the strip shows the target dock in it); Mine lists the saved
// layouts with Rename… / Delete / Export… under the list (an empty folder: a line saying how to
// save one); Layouts and Mine get "Keep my current look" under the pins check box.

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Config.h"
#include "DarkMode.h"
#include "Presets.h"
#include "SettingsPreview.h"

// The variants (DockCommand 152's LOWORD: 1 welcome, 2 new dock, anything else apply; round 12
// phase 12C: AppLook, the Looks kind alone for the app look, DockCommand 152 LOWORD 5).
enum class PresetMode { Apply = 0, Welcome = 1, NewDock = 2, AppLook = 3 };

// --- round 12 phase 12C: the chooser's three kinds (its kind row, 5672; QueryState 304) ---
// Layouts: the built-in presets (Presets::All()); Looks: theme presets and theme files
// (Presets::Looks()); Mine: the saved layouts (Presets::Mine()). The welcome variant shows only
// Layouts, a new dock Layouts and Mine, the app look only Looks.
enum class PresetKind { Layouts = 0, Looks = 1, Mine = 2 };

struct PresetDialogHost {
    const Config* config = nullptr;  // read for the preview (copied) and the pins
    // Apply / Use this dock / Skip: entry `index` of `kind`'s list with or without the pins and (a
    // layout) without its look (`keepLook`: the dock keeps its own). Round 12 phase 12C: `kind`
    // and `keepLook` added. False: refused (the dialog stays open, beeps).
    std::function<bool(PresetKind kind, size_t index, bool keepPins, bool keepLook)> apply;
    // The window is gone (the app frees the object later: posted).
    std::function<void()> destroyed;
};

class PresetDialog {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.Presets";
    static constexpr wchar_t kPreviewClass[] = L"WinDock.PresetPreview";
    static constexpr wchar_t kKindsClass[] = L"WinDock.PresetKinds";  // round 12 phase 12C: the kind row

    // `owner`: the settings window it is modal to (disabled while open), or the app window (not
    // disabled). `dockId`: the dock it applies to (Apply / Welcome); round 12 phase 12C: `dockName`
    // its display name (the title and the texts name it), `kind` the kind shown first.
    PresetDialog(PresetDialogHost host, PresetMode mode, HWND owner, std::wstring dockId, std::wstring dockName = {},
                 PresetKind kind = PresetKind::Layouts);
    ~PresetDialog();
    PresetDialog(const PresetDialog&) = delete;
    PresetDialog& operator=(const PresetDialog&) = delete;

    bool Create(HINSTANCE instance);
    HWND Hwnd() const { return hwnd_; }
    PresetMode Mode() const { return mode_; }
    // The selected entry (its index in the shown kind's list: Presets::All() / Looks() / Mine(); -1 none).
    int Selected() const;
    void Activate();
    // IsDialogMessage for the window (Tab, Enter, Esc). True if handled.
    bool PreTranslateMessage(MSG& msg);
    // Round 12 phase 12C: the kind shown (QueryState 304: 0 Layouts, 1 Looks, 2 Mine).
    PresetKind Kind() const { return kind_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PreviewProc(HWND, UINT, WPARAM, LPARAM);
    // --- round 12 phase 12C ---
    static LRESULT CALLBACK KindsProc(HWND, UINT, WPARAM, LPARAM);
    // The kinds this variant offers, in order.
    std::vector<PresetKind> Kinds() const;
    void SetKind(PresetKind kind, bool focusList);
    void FillList();
    void ReloadMine(const std::wstring& selectId);
    void PaintKinds(HWND hwnd, HDC hdc);
    RECT KindRect(size_t slot) const;  // in the kind row's client px
    const Presets::Preset* CurrentLayout() const;
    const Presets::Look* CurrentLook() const;
    bool KeepLook() const;
    void OnMineCommand(int id);
    // --- end round 12 phase 12C ---
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }

    void CreateFonts();
    void ApplyTheme();
    void Layout();
    // The selection changed (or the check box): the right side follows.
    void UpdateSelection();
    void PaintPreview(HWND hwnd, HDC hdc);
    void DrawListItem(const DRAWITEMSTRUCT& di);
    bool KeepPins() const;
    void OnApply();
    void OnCancel();
    void Close();

    PresetDialogHost host_;
    PresetMode mode_;
    HWND owner_ = nullptr;
    bool ownerDisabled_ = false;
    std::wstring dockId_;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr, name_ = nullptr, description_ = nullptr, preview_ = nullptr, lookCaption_ = nullptr;
    HWND check_ = nullptr, checkCaption_ = nullptr, note_ = nullptr, apply_ = nullptr, cancel_ = nullptr;
    HWND heading_ = nullptr, intro_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    DarkMode::Palette palette_;
    HFONT font_ = nullptr, fontBold_ = nullptr, fontName_ = nullptr, fontHeading_ = nullptr;
    HBRUSH layerBrush_ = nullptr, controlBrush_ = nullptr;
    // The preview strip draws from this copy (the preset's dock as docks[0], the user's global
    // theme): the live config and docks are never touched.
    Config previewConfig_;
    SettingsPreview strip_;
    bool closing_ = false;
    // --- round 12 phase 12C ---
    std::wstring dockName_;       // the target's display name (empty: the id)
    PresetKind kind_ = PresetKind::Layouts;
    Presets::MineList mine_;      // read when the dialog opens and after Rename / Delete
    std::vector<Presets::Look> looks_;
    HWND kinds_ = nullptr;        // the kind row (kPresetKind)
    HWND keepLook_ = nullptr, keepLookCaption_ = nullptr;
    HWND mineRename_ = nullptr, mineDelete_ = nullptr, mineExport_ = nullptr;
    HWND empty_ = nullptr, status_ = nullptr;
    int kindHot_ = -1;
    bool kindTracking_ = false;
    // --- end round 12 phase 12C ---
};
