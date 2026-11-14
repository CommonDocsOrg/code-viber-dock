#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Round 27 phase 27C: the `input` provider of the widget API (permission "input"): the taskbar's
// input-language indicator ("ENG"), which hiding the taskbar loses. The keyboard layout (input
// locale, HKL) of the foreground window's thread (GetKeyboardLayout(GetWindowThreadProcessId)), the
// installed layouts (GetKeyboardLayoutList) with each one's language tag (LCIDToLocaleName), the
// short code the taskbar shows (LOCALE_SISO639LANGNAME2 upper-cased: "ENG", "DEU", "FRA"; with the
// region, LOCALE_SISO3166CTRYNAME: "US"), the language's display name (LOCALE_SLOCALIZEDDISPLAYNAME)
// and the layout's name (the registry's "Layout Display Name" under Keyboard Layouts\<KLID>, an
// indirect string, else "Layout Text"). Out of scope: an IME's conversion mode (あ / A) and TSF
// text-service profiles beyond the HKL list.
//
// Signals (no hook of its own, nothing polls): WindowTracker's shell hook window (RegisterShellHook
// Window, already registered for the running apps) forwards HSHELL_LANGUAGE ("keyboard language was
// changed or a new keyboard layout was loaded", lParam the HKL: documented for the WH_SHELL hook;
// RegisterShellHookWindow says it gets "many of the same" messages without listing this one, so its
// arrival there is ASSUMED, not verified) and the activations (HSHELL_WINDOWACTIVATED /
// RUDEAPPACTIVATED: another window may have another layout when Windows' "Let me use a different
// input method for each app window" is on). A switch inside one window is reported to that window
// only (WM_INPUTLANGCHANGE goes to the focus window); if HSHELL_LANGUAGE does not come, the indicator
// follows at the next activation and at every get() (a click on the widget always shows the truth).
// Each signal costs a GetKeyboardLayout and a GetKeyboardLayoutList; names are cached per HKL.
// Quiet (SetQuiet: no user can be seen, or the session is paused): signals are only noted; leaving
// quiet re-reads once. Started by WidgetRuntime for the first widget that uses `input`, stopped with
// the last.
class InputService {
public:
    struct Layout {
        std::wstring id;        // the HKL as 8 hex digits ("04090409"): what input.set takes
        std::wstring tag;       // "en-US"
        std::wstring code;      // "ENG" (the taskbar's short form)
        std::wstring region;    // "US"
        std::wstring language;  // "English (United States)"
        std::wstring layout;    // "US", "German", "Microsoft IME"
        HKL hkl = nullptr;
        bool operator==(const Layout& o) const { return hkl == o.hkl; }
    };
    struct State {
        bool available = false;
        int current = -1;              // index into layouts (-1: the active layout is not listed)
        std::vector<Layout> layouts;   // GetKeyboardLayoutList order (the switcher's)
        Layout active;                 // the active one (also when not listed)
        bool operator==(const State& o) const { return current == o.current && layouts == o.layouts && active == o.active; }
    };

    InputService() = default;
    InputService(const InputService&) = delete;
    InputService& operator=(const InputService&) = delete;

    bool Start();
    void Stop();
    bool Active() const { return active_; }
    // Re-reads (cheap) and returns the state; `changed` runs when it differs from the last one.
    const State& Get();
    const State& Last() const { return state_; }

    // WindowTracker's shell hook events (WidgetRuntime forwards them while the provider runs).
    void OnShellEvent(WPARAM code, LPARAM lp);
    void SetQuiet(bool quiet);
    bool Quiet() const { return quiet_; }

    // input.set: asks the window the user works in to switch to layout `idOrTag` (an `id`, or a
    // tag: its first layout) with WM_INPUTLANGCHANGEREQUEST, posted, the way the language bar did.
    // The window accepts it by passing it to DefWindowProc; one that handles the message itself,
    // a window of higher integrity (UIPI refuses the post) or a console / UWP window may ignore it.
    bool Select(const std::wstring& idOrTag, std::wstring& error);

    // input.get()'s object (docs/widgets.md "input"), UTF-8 JSON; WidgetSamples uses it too.
    static std::string Json(const State& s);

    void SetChanged(std::function<void()> changed) { changed_ = std::move(changed); }
    uint64_t Refreshes() const { return refreshes_; }  // reads since the process started (automation)

private:
    // The window whose layout is shown and switched: the foreground one, or (when the foreground is
    // one of ours: a popup of the widget) the last other window that was activated.
    HWND Target() const;
    State Read();
    const Layout& Describe(HKL hkl);
    void Refresh(bool notify);

    bool active_ = false;
    bool quiet_ = false;
    bool missed_ = false;
    HWND lastForeign_ = nullptr;
    State state_;
    std::map<HKL, Layout> names_;
    std::function<void()> changed_;
    uint64_t refreshes_ = 0;
};
