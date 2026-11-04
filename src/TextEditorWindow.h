#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

#include "DarkMode.h"

// The text editor of popup.editText (round 6 phase 6C; docs/widgets.md "popup"): a small, resizable,
// non-modal top-level window owned by the widget's host window (a DockWindow or a WidgetWindow), in
// the Windows app mode's light / dark colours like the settings windows.
//
// - A multi-line EDIT (Segoe UI 10 pt, a themed scroll bar, the control's own undo with Ctrl+Z,
//   Ctrl+A selects all, Ctrl+Backspace deletes the word before the caret), OK (also Ctrl+Enter) and
//   Cancel (also Escape), a status line "n characters" that warns at the maximum length. Tab moves
//   the focus (edit, OK, Cancel). Single-line (multiline false): Enter is OK too.
// - Placed next to the widget's box on the side the host asks for (a bottom dock: above it), else
//   the opposite side, else beside it, always kept on the box's monitor's work area. Its size is
//   remembered per key (the widget id) for the session. Per-monitor DPI aware (WM_DPICHANGED).
// - Topmost only when its owner is (a dock above the windows; not a desktop-layer dock or widget).
// - The object deletes itself when its window is destroyed. `done` runs exactly once, unless
//   Abandon() was called: OK with the text ("\n" line breaks), or cancelled (Cancel, Escape, the
//   close box, the window destroyed with its owner). Nothing runs while it is open (no timers).
class TextEditorWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.TextEditor";

    // Where the window goes relative to the widget's box (the host's popup direction).
    enum class Side { Above, Below, Right, Left };

    struct Options {
        std::wstring title = L"Edit";
        std::wstring text;  // "\n" line breaks
        std::wstring placeholder;
        bool multiline = true;
        uint32_t maxLength = 100000;
        std::wstring sizeKey;  // the window's size is remembered per key for the session
        Side side = Side::Above;
    };
    // ok: the text ("\n" line breaks); !ok: cancelled.
    using Done = std::function<void(bool ok, const std::wstring& text)>;

    // Creates and shows the editor owned by `owner` (it takes the focus), next to `anchor` (the
    // widget's box, screen px). Null when the window could not be made.
    static TextEditorWindow* Open(HWND owner, const RECT& anchor, bool topmost, Options options, Done done);

    HWND Hwnd() const { return hwnd_; }
    HWND Edit() const { return edit_; }
    // Closes it as OK (true) / Cancel (false) would: `done` runs, then the window goes (and the
    // object with it).
    void Finish(bool ok);
    // Closes it without calling `done` (the widget reloads or goes).
    void Abandon();

private:
    static constexpr int kOk = IDOK;
    static constexpr int kCancel = IDCANCEL;
    static constexpr int kEdit = 100;
    static constexpr int kStatus = 101;

    TextEditorWindow() = default;
    ~TextEditorWindow();
    bool Create(HWND owner, const RECT& anchor, bool topmost);
    void ApplyTheme();
    void Layout();
    void UpdateStatus();
    std::wstring Text() const;
    int Px(float dip) const { return MulDiv(static_cast<int>(dip * 100.0f), static_cast<int>(dpi_), 9600); }
    // Keyboard shared by the edit and the buttons (Escape, Ctrl+Enter, Tab ...). True if handled.
    bool OnKey(HWND from, UINT msg, WPARAM wp);
    void FocusNext(HWND from, bool back);
    void DeleteWordBack();

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK ChildProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HWND ok_ = nullptr;
    HWND cancel_ = nullptr;
    HWND status_ = nullptr;
    UINT dpi_ = 96;
    Options options_;
    Done done_;
    bool finished_ = false;
    bool selfDelete_ = false;  // set once created: WM_NCDESTROY deletes the object
    bool atLimit_ = false;
    DarkMode::Palette palette_;
    HFONT editFont_ = nullptr;
    HFONT uiFont_ = nullptr;
    HBRUSH layerBrush_ = nullptr;
    HBRUSH controlBrush_ = nullptr;
};
