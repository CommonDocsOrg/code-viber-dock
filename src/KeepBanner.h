#pragma once

// Round 12 phase 12A: the Keep / Revert and Keep / Cancel bar as a small topmost banner near the
// dock (WinDock.KeepBanner), for a change made while no settings window is visible (the dock
// menu's Position on screen, a Ctrl+drag to another edge, DockCommand 153's preset preview). One
// line of text and two buttons; it never takes the foreground (WS_EX_NOACTIVATE: the buttons
// still take clicks) and App closes it with the bar (a Keep / Revert bar keeps itself after 12 s).
// Plain GDI and real child buttons (ids SettingsIds::kKeepBarText / kKeepBarKeep /
// kKeepBarRevert), so automation can read and press it like the settings windows' bar.

#include <windows.h>

#include <functional>
#include <string>

#include "Config.h"
#include "DarkMode.h"

class KeepBanner {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.KeepBanner";

    // `command`: a button was pressed (its control id).
    explicit KeepBanner(std::function<void(int id)> command) : command_(std::move(command)) {}
    ~KeepBanner();
    KeepBanner(const KeepBanner&) = delete;
    KeepBanner& operator=(const KeepBanner&) = delete;

    // Shows the banner (created the first time) with `text` and the second button's caption,
    // placed beside `bar` (the dock's bar at rest, screen px) on the side away from `edge`, inside
    // `work` (the dock's monitor). Updating it with the same text redraws nothing.
    bool Show(HINSTANCE instance, const RECT& bar, const RECT& work, DockEdge edge, bool floating, UINT dpi,
              const std::wstring& text, const std::wstring& cancel);
    void Close();
    HWND Hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }
    void Layout();
    void ApplyStyle();

    std::function<void(int)> command_;
    HWND hwnd_ = nullptr;
    HWND text_ = nullptr, keep_ = nullptr, cancel_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    UINT fontDpi_ = 0;
    HFONT font_ = nullptr;
    HBRUSH brush_ = nullptr;
    DarkMode::Palette palette_;
    std::wstring shownText_, shownCancel_;
};
