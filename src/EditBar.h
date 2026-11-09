#pragma once

// Round 12 phase 12D: the edit bar of customize mode (WinDock.EditBar), a small topmost strip
// beside the dock being customized: "Customizing <dock>" in the dock's colour, the save state (the
// 12A status: Saved / Saving… / Not saved yet / Not saved), Undo, Add items (the drawer popup),
// Settings (the dock's settings) and Done. Above a bottom dock, under a top dock, beside a vertical
// one, at the dock's end and far enough out to leave the anchor targets of a drag free; it never
// covers the dock. Like the 12A KeepBanner (whose recipe it follows) it is plain GDI with real child
// controls (ids SettingsIds::kEditBarTitle ... kEditBarDone, so automation presses them with
// WM_COMMAND) and never takes the foreground (WS_EX_NOACTIVATE: the buttons still take clicks), so
// the dock keeps the keyboard for Esc. Not a PopupWindow: GDI child controls do not render in a
// WS_EX_NOREDIRECTIONBITMAP composition window. Nothing of it ticks; App updates it on a change.

#include <windows.h>

#include <functional>
#include <string>

#include "Config.h"
#include "DarkMode.h"

class EditBar {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.EditBar";

    // `command`: a button was pressed (its control id).
    explicit EditBar(std::function<void(int id)> command) : command_(std::move(command)) {}
    ~EditBar();
    EditBar(const EditBar&) = delete;
    EditBar& operator=(const EditBar&) = delete;

    struct Content {
        std::wstring dock;       // the dock's name ("main")
        uint32_t dockArgb = 0;   // its identity colour (Config::DockColor)
        std::wstring status;     // the save state's text
        bool canUndo = false;
        bool drawerShown = false;  // "Add items" reads "Hide items" while the drawer popup shows
    };
    // Shows the bar (created the first time) for the dock whose bar (with its outline) is at `bar`
    // (screen px) on `edge` inside `work`; updating it with the same content redraws nothing.
    bool Show(HINSTANCE instance, const RECT& bar, const RECT& work, DockEdge edge, bool floating, UINT dpi, const Content& content);
    void Close();
    HWND Hwnd() const { return hwnd_; }
    bool Shown() const { return hwnd_ && IsWindowVisible(hwnd_); }
    RECT Rect() const;

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }
    void Layout();
    void ApplyStyle();

    std::function<void(int)> command_;
    HWND hwnd_ = nullptr;
    HWND title_ = nullptr, status_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    UINT fontDpi_ = 0;
    HFONT font_ = nullptr, bold_ = nullptr;
    HBRUSH brush_ = nullptr;
    DarkMode::Palette palette_;
    COLORREF dockColor_ = RGB(0x4C, 0xC2, 0xFF);
    Content shown_;
    bool hasShown_ = false;
    int titleW_ = 0;  // px: the title's text width
};
