#pragma once

// Round 12 phase 12D: the drawer beside the dock in customize mode (WinDock.DrawerPopup): the
// search box, the filter chips (All, Built in, Widgets, Apps and folders) and the drawer grid,
// the same control and catalog as the Contents tab's drawer (SettingsStrip.h). A click on a tile
// adds it at the end of the dock, a drag puts it where the dock's marker shows (the in-process
// mouse-capture drag the Contents strip uses, no OLE), a script widget dragged onto the desktop
// becomes a standalone widget there (or its tile's small monitor button: the old "Desktop").
//
// The click model of the dock's popups (PopupWindow): it is activated when shown, and losing the
// activation (a click on the desktop or another window) or Escape hides it and tells the owner;
// while one of its own dialogs is open (App or shortcut…, Folder…) it stays. It is not a
// PopupWindow because it hosts real child controls (an edit box, buttons), which GDI cannot draw
// into a WS_EX_NOREDIRECTIONBITMAP composition window; it paints with GDI like the settings
// windows (the palette's layer colour, Windows 11's rounded corners and a border in the dock's
// colour). Nothing runs while it is shown and idle; previews render as tiles are painted.
//
// Round 18 phase 18B: the same popup as the standalone widget picker of App settings > Docks and
// displays > Add a dock > More ▾ > Standalone widget… (ShowStandalone: a second instance in App, its
// own callbacks). The script widgets only, no chips and no monitor buttons; a click (or a drag onto
// the desktop) makes a standalone widget; a widget whose manifest needs a dock is greyed "Needs a
// dock". It hangs under the button that opened it, within that display's work area.

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "Config.h"
#include "DarkMode.h"
#include "SettingsStrip.h"
#include "SettingsWindow.h"

class DrawerPopup final : private StripUi::StripHost {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.DrawerPopup";

    struct Callbacks {
        std::function<void(int ref)> add;      // a click on a tile (dialog entries included)
        std::function<void(int ref)> desktop;  // a script widget tile's monitor button
        // A drag outside the popup (screen px): phase 0 over (returns 0 nowhere, 1 a dock, 2 the
        // desktop), 1 left / cancelled, 2 dropped (returns 1 when something was added).
        std::function<int(int phase, POINT screen, int ref)> drag;
        std::function<void()> closed;          // hidden by itself (deactivated; Escape without scape)
        std::function<void()> escape;          // Escape in it (customize mode: leaves the mode, which hides it)
        // Widget previews (as SettingsHost::renderPreview / previewStamp / previewPanel).
        std::function<SettingsWidgetPreview(const std::wstring& id, float scale, SIZE fitPx, float maxZoom)> renderPreview;
        std::function<uint64_t()> previewStamp;
        std::function<uint32_t()> previewPanel;
        // Round 18 phase 18B (the standalone picker): widget `id` may live on the desktop (App::
        // CanBeStandalone); unset: every widget may. A tile that may not is greyed "Needs a dock".
        std::function<bool(const std::wstring& id)> standaloneOk;
    };

    DrawerPopup() = default;
    ~DrawerPopup();
    DrawerPopup(const DrawerPopup&) = delete;
    DrawerPopup& operator=(const DrawerPopup&) = delete;

    void SetCallbacks(Callbacks cb) { cb_ = std::move(cb); }
    // Shows the popup for `dock` (its items decide "On the dock"; `name` and `argb` its title and
    // border) inside `avail` (screen px: the free room beside the dock and its edit bar), hugging
    // the side of it toward `edge`'s dock, its end at screen coordinate `endAlong` along the edge.
    // Activates it (the search box gets the focus) unless it is shown already.
    bool Show(HINSTANCE instance, const DockConfig& dock, const std::wstring& name, uint32_t argb, const RECT& avail, DockEdge edge,
              int endAlong, UINT dpi);
    // Round 18 phase 18B: the standalone widget picker, hanging under `anchor` (screen px: the button
    // that opened it; right-aligned to it, above it when the work area has no room below), with a
    // border in `argb`. A click on a tile is the `add` callback (the owner makes a standalone widget),
    // a drag the `drag` callbacks (the desktop or a dock). Activated: the search box gets the focus.
    bool ShowStandalone(HINSTANCE instance, const RECT& anchor, uint32_t argb, UINT dpi);
    void Hide();
    bool Shown() const { return hwnd_ && IsWindowVisible(hwnd_); }
    HWND Hwnd() const { return hwnd_; }
    // The dock's items changed: "On the dock" and the tiles again (the popup stays where it is).
    void Refresh(const DockConfig& dock);
    // The window that gets the foreground back when the popup, being the foreground, hides (the dock:
    // it keeps the keyboard for Escape, and no other app's window is activated in between).
    void SetReturnFocus(HWND hwnd) { returnFocus_ = hwnd; }
    // A dialog of its own is open: deactivation does not close it meanwhile.
    void SetModal(bool modal) { modal_ = modal; }
    // QueryState 312: the tiles after the search and the chip.
    int TileCount() const { return grid_ ? grid_->TileCount() : 0; }
    // The item drawer entry `ref` adds to `dock` (nullopt: a dialog entry, none, or a kind the dock
    // has once already: a warning beep); `tray`: it is a tray item.
    std::optional<DockItemEntry> EntryFor(int ref, const DockConfig& dock, bool* tray = nullptr) {
        if (catalog_.EntryCount() == 0) catalog_.Build();  // (read on Show: a drag asks on every move)
        return catalog_.NewEntry(ref, dock, tray);
    }
    const StripUi::DrawerEntry* Entry(int ref) {
        if (catalog_.EntryCount() == 0) catalog_.Build();
        return catalog_.EntryOf(ref);
    }
    // Every cached preview goes (DockCommand 148, Reload widgets).
    void InvalidatePreviews();

private:
    // --- StripHost ---
    int Px(int dip) const override { return MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }
    const DarkMode::Palette& Colors() const override { return palette_; }
    COLORREF DockColor() const override { return dockColor_; }
    const Image* Preview(const std::wstring& id, SIZE fit, bool render, float zoom) override;
    const Image* PreviewEntry(const std::wstring& id, SIZE fit, float zoom) override;
    void PaintPreview(HDC dc, const RECT& box, const Image& img, COLORREF under) const override;

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    bool EnsureWindow(HINSTANCE instance);
    void ApplyStyle();
    void Fill();
    void Layout();
    // The client height the content wants at client width `w` (px).
    int ContentHeight(int w) const;
    void CloseSelf();
    void ClearPreviews();

    Callbacks cb_;
    HWND hwnd_ = nullptr;
    HWND search_ = nullptr;
    std::unique_ptr<StripUi::SettingsStrip> grid_;
    StripUi::DrawerCatalog catalog_;
    StripUi::DrawerFilter chip_ = StripUi::DrawerFilter::All;
    std::wstring filter_;
    DockConfig dock_;  // a copy of the dock it adds to ("On the dock"); refreshed by Refresh
    std::wstring name_;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    UINT fontDpi_ = 0;
    HFONT font_ = nullptr, bold_ = nullptr;
    HBRUSH brush_ = nullptr, editBrush_ = nullptr;
    DarkMode::Palette palette_;
    COLORREF dockColor_ = RGB(0x4C, 0xC2, 0xFF);
    bool standalone_ = false;  // round 18 phase 18B: the standalone widget picker (ShowStandalone)
    bool modal_ = false;
    HWND returnFocus_ = nullptr;
    bool hiding_ = false;
    // The preview cache (SettingsWindow's recipe): by id and box, dropped when the stamp or the DPI change.
    std::map<std::wstring, Image> previews_;
    uint64_t previewStamp_ = 0;
    UINT previewDpi_ = 0;
};
