#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>

#include "Backdrop.h"
#include "Config.h"
#include "Renderer.h"

// Where a dock popup opens: centred on screen coordinate `along` along the dock's edge (x for
// bottom / top docks, y for left / right ones), with its dock-facing side at screen coordinate
// `side` on the cross axis, extending away from the edge (above a bottom dock, below a top dock,
// right of a left dock, left of a right dock). The popup is kept inside the frame's monitor rect.
struct PopupAnchor {
    DockEdge edge = DockEdge::Bottom;
    int along = 0;
    int side = 0;
};

// Base of the dock's popups (window previews; later folder contents, weather, calendar): a
// separate top-level window (WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP) whose
// content is one D2D surface drawn with the dock's Renderer through a second composition target
// on the same device (Renderer::CreateWindowTarget), so there is no second D3D/D2D device. With
// acrylic, a Backdrop window (same recipe as the bar's) sits directly beneath it. Fade in/out:
// the target root's opacity (a DirectComposition animation), stepped by a 16 ms one-shot timer
// only while the fade runs (OnFade lets a subclass follow it, e.g. DWM thumbnails). Nothing runs
// while the popup is shown and the mouse is still.
//
// Two activation models:
//  - Hover: WS_EX_NOACTIVATE, never takes the focus; the dock's hover state machine shows and
//    dismisses it (hover callback: the mouse entered / left the popup).
//  - Click: activatable. Shown in the foreground; losing activation (WM_ACTIVATE WA_INACTIVE)
//    or Escape hides it and fires the closed callback. The owner dismisses it on its own events.
//    While it is shown, an out-of-context WinEvent hook (EVENT_SYSTEM_FOREGROUND) also closes it
//    when a window of another process comes to the foreground: the popup may not have been
//    allowed to take the foreground itself (opened by automation, not by a click), and then it
//    never sees WM_ACTIVATE. The hook exists only while a click popup is on screen.
//
// Subclasses lay out in Measure(), draw in Paint(), and hit-test in HitAt(); the base tracks the
// mouse, the hot item / part and clicks (OnClick). The window class is the subclass's
// (`className`); every popup window belongs to this process, which is how the dock's foreground
// tests (ForegroundOverlaps, ForegroundWatch) recognise them as the dock's own windows.
class PopupWindow {
public:
    enum class Mode { Hover, Click };
    enum class Button { Left, Middle, Right };
    static constexpr UINT_PTR kFadeTimerId = 1;  // subclasses use timer ids >= 100

    // Window chrome for one Show: placement, the backdrop and the panel's corner radius.
    struct Frame {
        float scale = 1.0f;            // DPI scale
        bool acrylic = false;          // wanted; ShowAt falls back to none if the backdrop fails
        float cornerRadiusDip = 8.0f;  // without acrylic (with it, the system radius)
        RECT monitor{};                // screen px (the dock monitor's work area); the popup is kept inside it
    };

    PopupWindow(const wchar_t* className, const wchar_t* title, Mode mode)
        : className_(className), title_(title), mode_(mode) {}
    PopupWindow(const PopupWindow&) = delete;
    PopupWindow& operator=(const PopupWindow&) = delete;
    // Subclasses call Destroy() in their own destructor (their OnDestroying must still run).
    virtual ~PopupWindow() { Destroy(); }

    void Init(HINSTANCE instance, Renderer* renderer) {
        instance_ = instance;
        renderer_ = renderer;
    }
    // The mouse entered / left the popup.
    void SetHoverCallback(std::function<void(bool inside)> cb) { hoverCallback_ = std::move(cb); }
    // Click popups: hidden by itself (deactivated, Escape). Not called for Hide().
    void SetClosedCallback(std::function<void()> cb) { closedCallback_ = std::move(cb); }

    // Fades out (animate) or hides at once.
    void Hide(bool animate);
    // Destroys the window, the composition target and the backdrop (device loss, shutdown).
    // Does not commit.
    void Destroy();

    // Shown or fading in (not fading out, not hidden).
    bool Visible() const { return state_ == State::Shown; }
    bool OnScreen() const { return state_ != State::Hidden; }
    bool FadePending() const { return fadePending_; }
    // Fade-in / fade-out duration (default 0.12 s; 0: none). Takes effect with the next fade.
    void SetFadeSeconds(double seconds) { fadeS_ = seconds > 0.0 ? seconds : 0.0; }
    bool ContainsCursor() const;
    HWND Hwnd() const { return hwnd_; }
    Mode ActivationMode() const { return mode_; }

protected:
    // Shows the popup (fading in) or updates it in place: Measure() gives the client size,
    // Paint() draws it, and it is placed at `anchor` (see PopupAnchor), clamped to frame.monitor.
    // Returns false if it could not be shown.
    bool ShowAt(const Frame& frame, const PopupAnchor& anchor);
    // Paint() into the content surface again (no commit; call Commit()).
    HRESULT Redraw();
    void Commit();

    // The window exists (created by ShowAt before Measure runs).
    HWND Window() const { return hwnd_; }
    // The layer Paint() draws into (the panel). Subclasses may add child layers in front of it
    // (an ItemGridView); they must remove them in OnHidden / OnDestroying.
    Layer& ContentLayer() { return content_; }
    Renderer* Render() const { return renderer_; }
    // The current frame with acrylic / corner radius as actually used.
    const Frame& CurrentFrame() const { return frame_; }
    float Scale() const { return frame_.scale; }
    SIZE ClientSize() const { return size_; }
    RECT ScreenRect() const { return rect_; }
    bool Inside() const { return inside_; }
    int Hot() const { return hot_; }
    int HotPart() const { return hotPart_; }
    // Forgets the hot item without OnHotChanged (Measure: the item it named is gone).
    void ClearHot() {
        hot_ = -1;
        hotPart_ = 0;
    }
    // The panel's corner radius in px (acrylic: the system radius; otherwise at most 12 DIPs).
    float CornerRadiusPx() const;
    // Fills the client area with the rounded panel (`tint`) and its hairline (`border`).
    void PaintPanel(ID2D1DeviceContext* dc, uint32_t tint, uint32_t border) const;

    // --- subclass hooks ---
    // Lays out the content for the current frame; returns the client size in px (> 0).
    virtual SIZE Measure() = 0;
    // Draws the whole client area (the surface is cleared; px units, origin top-left).
    virtual void Paint(ID2D1DeviceContext* dc) = 0;
    // Item under a client point (-1: none) and the part of it (0 = the item itself).
    virtual int HitAt(POINT /*clientPt*/, int* part) const {
        if (part) *part = 0;
        return -1;
    }
    // A press and release on the same item (left / right: the same part too).
    virtual void OnClick(int index, int part, Button button) {
        (void)index;
        (void)part;
        (void)button;
    }
    // The hot item / part changed (Hot(), HotPart()). Default: redraw and commit.
    virtual void OnHotChanged();
    // Each fade step, with the popup's current opacity (0 - 255). `placed`: ShowAt just laid out
    // and placed the popup (rects may have changed).
    virtual void OnFade(BYTE opacity, bool placed) {
        (void)opacity;
        (void)placed;
    }
    // Hidden (the fade ended, or at once): release per-show content.
    virtual void OnHidden() {}
    // Destroy(): release everything that belongs to the window or the device.
    virtual void OnDestroying() {}
    // Any other message; return true to consume it (with `result`).
    virtual bool OnMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
        (void)msg;
        (void)wp;
        (void)lp;
        (void)result;
        return false;
    }

private:
    enum class State { Hidden, Shown, FadingOut };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    bool EnsureWindow();
    void StartFade(float to);
    void OnFadeTimer();
    void FinishHide();
    void SetHot(int index, int part);
    void ButtonDown(Button button, POINT pt);
    void ButtonUp(Button button, POINT pt);
    // Click popups: hides (fading) and tells the owner.
    void CloseSelf();
    void CancelTracking();
    void HookForeground(bool on);
    static void CALLBACK OnForegroundEvent(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                                           DWORD thread, DWORD time);

    const wchar_t* className_;
    const wchar_t* title_;
    Mode mode_;
    HINSTANCE instance_ = nullptr;
    Renderer* renderer_ = nullptr;
    std::function<void(bool)> hoverCallback_;
    std::function<void()> closedCallback_;
    HWND hwnd_ = nullptr;
    winrt::com_ptr<IDCompositionTarget> target_;
    Layer root_;
    Layer content_;
    Backdrop backdrop_;

    State state_ = State::Hidden;
    Frame frame_;
    SIZE size_{};   // client size, px
    RECT rect_{};   // screen
    int hot_ = -1;
    int hotPart_ = 0;
    int pressed_[3] = {-1, -1, -1};  // per Button
    int pressedPart_[3] = {0, 0, 0};
    bool tracking_ = false;
    bool inside_ = false;
    HWINEVENTHOOK foregroundHook_ = nullptr;  // click popups, while on screen

    Tween fade_;
    bool fadePending_ = false;
    double fadeS_ = 0.12;
};
