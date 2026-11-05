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

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

#include "Config.h"
#include "DarkMode.h"
#include "SettingsPreview.h"

// The variants (DockCommand 152's LOWORD: 1 welcome, 2 new dock, anything else apply).
enum class PresetMode { Apply = 0, Welcome = 1, NewDock = 2 };

struct PresetDialogHost {
    const Config* config = nullptr;  // read for the preview (copied) and the pins
    // Apply / Use this dock / Skip: preset `index` (Presets::All() order) with or without the pins.
    // False: refused (the dialog stays open, beeps).
    std::function<bool(size_t index, bool keepPins)> apply;
    // The window is gone (the app frees the object later: posted).
    std::function<void()> destroyed;
};

class PresetDialog {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.Presets";
    static constexpr wchar_t kPreviewClass[] = L"WinDock.PresetPreview";

    // `owner`: the settings window it is modal to (disabled while open), or the app window (not
    // disabled). `dockId`: the dock it applies to (Apply / Welcome; the title names it).
    PresetDialog(PresetDialogHost host, PresetMode mode, HWND owner, std::wstring dockId);
    ~PresetDialog();
    PresetDialog(const PresetDialog&) = delete;
    PresetDialog& operator=(const PresetDialog&) = delete;

    bool Create(HINSTANCE instance);
    HWND Hwnd() const { return hwnd_; }
    PresetMode Mode() const { return mode_; }
    // The selected preset (its index in Presets::All(); -1 none).
    int Selected() const;
    void Activate();
    // IsDialogMessage for the window (Tab, Enter, Esc). True if handled.
    bool PreTranslateMessage(MSG& msg);

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PreviewProc(HWND, UINT, WPARAM, LPARAM);
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
};
