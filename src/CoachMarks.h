#pragma once

// Round 12 phase 12G: the three tips on the dock (WinDock.CoachMark): a small topmost bubble beside
// the first dock's resting bar, on the screen's side of it (as KeepBanner places its bar), with one
// sentence, "Tip n of 3", Skip tips and Next / Done. Like KeepBanner it never takes the foreground
// (WS_EX_NOACTIVATE; its buttons still take clicks), is plain GDI with real child controls
// (SettingsIds::kTipText / kTipCount / kTipSkip / kTipNext) so automation reads and presses it, and
// runs no timer: it is drawn when shown and moved by App when the dock moves (nothing ticks while a
// tip is up). At most 320 DIPs wide; the sentence wraps.

#include <windows.h>

#include <functional>
#include <string>

#include "Config.h"
#include "DarkMode.h"

class CoachMark {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.CoachMark";
    static constexpr int kWidthDip = 320;

    // `command`: Skip tips or Next / Done was pressed (its control id).
    explicit CoachMark(std::function<void(int id)> command) : command_(std::move(command)) {}
    ~CoachMark();
    CoachMark(const CoachMark&) = delete;
    CoachMark& operator=(const CoachMark&) = delete;

    // The sentence of tip `index` (0 - 2).
    static const wchar_t* Text(int index);
    // Shows tip `index` of `count` (created the first time) beside `bar` (the dock's bar at rest,
    // screen px) on the side away from `edge`, inside `work` (the dock's monitor). `last`: the
    // second button reads "Done". Showing the same tip again only moves it.
    bool Show(HINSTANCE instance, int index, int count, bool last, const RECT& bar, const RECT& work, DockEdge edge, bool floating,
              UINT dpi);
    void Close();
    HWND Hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }
    void ApplyStyle();
    // The bubble's height for its text at the current width (px).
    int Measure(int width);
    void Layout();

    std::function<void(int)> command_;
    HWND hwnd_ = nullptr;
    HWND text_ = nullptr, count_ = nullptr, skip_ = nullptr, next_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    UINT fontDpi_ = 0;
    HFONT font_ = nullptr, fontSmall_ = nullptr;
    HBRUSH brush_ = nullptr;
    DarkMode::Palette palette_;
    int shownIndex_ = -1;
    bool shownLast_ = false;
};
