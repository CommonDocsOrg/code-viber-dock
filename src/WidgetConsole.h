#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "DarkMode.h"

class WidgetRuntime;
class WidgetInstance;

// The widget Console (Settings > Widgets > Console, DockCommand 127): a small top-level window
// (WinDock.WidgetConsole) listing every live widget instance with its state, the selected one's
// log (the last 500 lines: console.* output, errors with their stacks, the host's notes), and
// Reload / Open folder / Clear buttons. It follows the runtime's log and instance notifications
// (posted to itself, coalesced): nothing polls. Light / dark like the settings window.
class WidgetConsole {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.WidgetConsole";
    // Control ids (automation).
    static constexpr int kList = 4001;    // LISTBOX: one row per instance
    static constexpr int kLog = 4002;     // read-only EDIT: the selected instance's log
    static constexpr int kReload = 4003;
    static constexpr int kFolder = 4004;  // Open folder (the widget's own, or the widgets folder)
    static constexpr int kClear = 4005;   // clears the view (the log itself is kept)
    static constexpr int kStatus = 4006;

    explicit WidgetConsole(WidgetRuntime& runtime, std::function<void()> destroyed)
        : runtime_(runtime), destroyed_(std::move(destroyed)) {}
    ~WidgetConsole();
    WidgetConsole(const WidgetConsole&) = delete;
    WidgetConsole& operator=(const WidgetConsole&) = delete;

    bool Create(HINSTANCE instance);
    void Activate();
    HWND Hwnd() const { return hwnd_; }
    // Lines shown in the log view now (QueryState 64).
    int ShownLines() const { return shownLines_; }

private:
    static constexpr UINT kRefreshMessage = WM_APP + 1;  // posted: the log / the instances changed

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    void Layout();
    void ApplyTheme();
    void FillList();
    void FillLog();
    void PostRefresh(bool list);
    WidgetInstance* Selected() const;
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }

    WidgetRuntime& runtime_;
    std::function<void()> destroyed_;
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr, log_ = nullptr, reload_ = nullptr, folder_ = nullptr, clear_ = nullptr, status_ = nullptr;
    HFONT font_ = nullptr, mono_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    DarkMode::Palette palette_;
    HBRUSH layerBrush_ = nullptr, controlBrush_ = nullptr;
    std::vector<WidgetInstance*> rows_;  // list rows -> instances (checked against the runtime)
    WidgetInstance* shown_ = nullptr;    // the instance whose log is shown
    uint64_t clearedAt_ = 0;             // Clear: lines up to this total are not shown
    int shownLines_ = 0;
    bool refreshPosted_ = false;
    bool listDirty_ = true;
};
