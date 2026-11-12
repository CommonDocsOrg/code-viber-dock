#include "PopupWindow.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

#include "Theme.h"

namespace {

constexpr DWORD kStyle = WS_POPUP;
constexpr DWORD kHoverExStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
constexpr DWORD kClickExStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
// Mouse-transparent popups (SetMouseTransparent): hit-testing skips a top-level window only when it
// is layered AND transparent (WS_EX_TRANSPARENT alone still takes the mouse). The layer is fully
// opaque (SetLayeredWindowAttributes 255); the content is the composition target's as before.
constexpr DWORD kTransparentExStyle = WS_EX_TRANSPARENT | WS_EX_LAYERED;
constexpr float kMonitorMargin = 8.0f;  // DIPs between the popup and the monitor edge
constexpr float kKeepCursorDip = 20.0f; // a shrinking popup keeps this much of itself under the cursor
constexpr float kMaxPlainRadiusDip = 12.0f;

LONG Px(float dip, float scale) { return static_cast<LONG>(std::lround(dip * scale)); }

// Click popups with a foreground hook (WinEvent callbacks carry no context; there is at most a
// handful of popups, all on this thread).
std::vector<PopupWindow*>& HookedPopups() {
    static std::vector<PopupWindow*> popups;
    return popups;
}
// ... and those held for a foreign menu (HoldForMenu: the menu hook), the same pattern.
std::vector<PopupWindow*>& MenuHookedPopups() {
    static std::vector<PopupWindow*> popups;
    return popups;
}

// HoldForMenu: how long the app gets to open its menu before the hold ends without one (some apps
// show a window of their own instead of a Win32 menu, which fires no MENUPOPUPSTART; the
// foreground then decides as at the end of a menu).
constexpr UINT kMenuHoldStartMs = 1500;

}  // namespace

bool PopupWindow::EnsureWindow() {
    if (hwnd_ && target_ && content_.visual) return true;
    if (!renderer_ || !instance_) return false;
    if (!hwnd_) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &PopupWindow::WndProcThunk;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = className_;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        const DWORD ex = (mode_ == Mode::Hover ? kHoverExStyle : kClickExStyle) | (mouseTransparent_ ? kTransparentExStyle : 0);
        hwnd_ = CreateWindowExW(ex, className_, title_, kStyle, 0, 0, 1, 1, nullptr, nullptr, instance_, this);
        if (!hwnd_) return false;
        if (mouseTransparent_) SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        // Shown and hidden instantly; the fade is ours.
        const BOOL off = TRUE;
        DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    }
    HRESULT hr = renderer_->CreateWindowTarget(hwnd_, target_, root_);
    if (SUCCEEDED(hr)) hr = renderer_->CreateLayer(content_, &root_);
    if (FAILED(hr)) {
        wchar_t buf[128];
        swprintf_s(buf, L"WinDock: popup target failed (%s, 0x%08X)\n", className_, static_cast<unsigned>(hr));
        OutputDebugStringW(buf);
        Destroy();
        return false;
    }
    fade_.valid = false;
    return true;
}

void PopupWindow::Destroy() {
    if (fadePending_ && hwnd_) KillTimer(hwnd_, kFadeTimerId);
    fadePending_ = false;
    EndMenuHold(false);
    keptForeground_ = nullptr;
    HookForeground(false);
    OnDestroying();
    content_.Reset();
    root_.Reset();
    if (target_) target_->SetRoot(nullptr);
    target_ = nullptr;
    backdrop_.Destroy();
    state_ = State::Hidden;
    hot_ = -1;
    hotPart_ = 0;
    std::fill(std::begin(pressed_), std::end(pressed_), -1);
    tracking_ = inside_ = false;
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
}

void PopupWindow::Commit() {
    if (!renderer_) return;
    const HRESULT hr = renderer_->Commit();
    if (FAILED(hr)) {
        wchar_t buf[96];
        swprintf_s(buf, L"WinDock: popup commit failed (0x%08X)\n", static_cast<unsigned>(hr));
        OutputDebugStringW(buf);
    }
}

HRESULT PopupWindow::Redraw() {
    if (!content_.visual || !renderer_ || size_.cx <= 0 || size_.cy <= 0) return E_UNEXPECTED;
    return renderer_->DrawLayer(content_, static_cast<UINT>(size_.cx), static_cast<UINT>(size_.cy),
                                [&](ID2D1DeviceContext* dc) { Paint(dc); });
}

float PopupWindow::CornerRadiusPx() const {
    const float dip = frame_.acrylic ? frame_.cornerRadiusDip : std::min(frame_.cornerRadiusDip, kMaxPlainRadiusDip);
    return std::round(dip * frame_.scale);
}

void PopupWindow::PaintPanel(ID2D1DeviceContext* dc, uint32_t tint, uint32_t border) const {
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(tint), brush.put()))) return;
    const float radius = CornerRadiusPx();
    const float stroke = std::max(1.0f, std::round(frame_.scale));
    const float fw = static_cast<float>(size_.cx), fh = static_cast<float>(size_.cy);
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, fw, fh), radius, radius), brush.get());
    const float inset = stroke / 2.0f;
    brush->SetColor(Theme::ToColor(border));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(inset, inset, fw - inset, fh - inset),
                                               std::max(0.0f, radius - inset), std::max(0.0f, radius - inset)),
                             brush.get(), stroke);
}

bool PopupWindow::ShowAt(const Frame& frame, const PopupAnchor& anchor) {
    if (!EnsureWindow()) return false;
    const bool wasHidden = state_ == State::Hidden;
    frame_ = frame;
    if (frame_.acrylic && !backdrop_.Exists() && !backdrop_.Create(instance_, mouseTransparent_)) frame_.acrylic = false;
    if (frame_.acrylic) {
        frame_.cornerRadiusDip = backdrop_.SetCornerRadius(8.0f);
    } else {
        backdrop_.Destroy();
    }
    const SIZE size = Measure();
    if (size.cx <= 0 || size.cy <= 0) return false;
    size_ = size;
    if (FAILED(Redraw())) return false;

    const RECT& m = frame_.monitor;
    const LONG margin = Px(kMonitorMargin, frame_.scale);
    // Main axis (along the dock's edge) and cross axis, as x / y of a bottom dock.
    const bool vertical = IsVertical(anchor.edge);
    const LONG mainSize = vertical ? size_.cy : size_.cx;
    const LONG crossSize = vertical ? size_.cx : size_.cy;
    const LONG mainLo = vertical ? m.top : m.left, mainHi = vertical ? m.bottom : m.right;
    LONG along = static_cast<LONG>(anchor.along) - mainSize / 2 + static_cast<LONG>(std::lround(anchor.alongBias * static_cast<float>(mainSize) / 2.0f));
    const LONG side = static_cast<LONG>(anchor.side);
    // Shrunk while the mouse is in it (e.g. a window closed): keep the cursor inside instead of
    // re-centring away from it, so a hover popup does not dismiss itself under a still mouse. A
    // click popup keeps its anchor (it does not close when the mouse leaves it).
    POINT cursor{};
    if (mode_ == Mode::Hover && inside_ && !wasHidden && GetCursorPos(&cursor)) {
        const LONG keep = Px(kKeepCursorDip, frame_.scale);
        const LONG c = vertical ? cursor.y : cursor.x;
        if (c >= along + mainSize) along = c - mainSize + keep;
        if (c < along) along = c - keep;
    }
    along = std::clamp(along, mainLo + margin, std::max(mainLo + margin, mainHi - margin - mainSize));
    // Away from the edge, kept inside the monitor on that side.
    LONG across = 0;
    switch (anchor.edge) {
        case DockEdge::Top: across = std::min(side, m.bottom - margin - crossSize); break;
        case DockEdge::Left: across = std::min(side, m.right - margin - crossSize); break;
        case DockEdge::Right: across = std::max(side - crossSize, m.left + margin); break;
        case DockEdge::Bottom:
        default: across = std::max(side - crossSize, m.top + margin); break;
    }
    const LONG x = vertical ? across : along;
    const LONG y = vertical ? along : across;
    rect_ = RECT{x, y, x + size_.cx, y + size_.cy};

    const double now = Renderer::Now();
    if (wasHidden) {
        fade_ = Tween{0.0f, 0.0f, now, 0.0, true};
        winrt::com_ptr<IDCompositionVisual3> root3;
        if (root_.visual && SUCCEEDED(root_.visual->QueryInterface(IID_PPV_ARGS(root3.put())))) {
            root3->SetOpacity(0.0f);
        } else {
            fade_ = Tween{1.0f, 1.0f, now, 0.0, true};  // no opacity support: no fade
        }
    }
    state_ = State::Shown;
    StartFade(1.0f);  // first: with no fade (SetFadeSeconds(0)) OnFade starts opaque
    OnFade(static_cast<BYTE>(std::lround(std::clamp(fade_.Value(now), 0.0f, 1.0f) * 255.0f)), true);
    Commit();
    // A click popup takes the foreground when it appears (the dock just got the click, so this
    // process may set it); a hover popup never activates.
    // Shown again while fading out (or not allowed to take the foreground before): try again.
    // Round 19 phase 19A: not while held for a foreign menu (refilled because the app changed its
    // icon or tip as its menu opened): taking the foreground would close that menu.
    const bool activate = mode_ == Mode::Click && !menuHold_ && (wasHidden || GetForegroundWindow() != hwnd_);
    // Held: the z-order stays too (raised to the top of the topmost band it would cover the menu).
    SetWindowPos(hwnd_, HWND_TOPMOST, rect_.left, rect_.top, size_.cx, size_.cy,
                 (activate ? 0 : SWP_NOACTIVATE) | SWP_NOOWNERZORDER | (wasHidden ? SWP_SHOWWINDOW : 0) |
                     (menuHold_ && !wasHidden ? SWP_NOZORDER : 0));
    if (activate) {
        SetForegroundWindow(hwnd_);
        HookForeground(true);
    }
    if (frame_.acrylic) backdrop_.Place(hwnd_, rect_);

    // Resized away from under the cursor: that is a leave (no WM_MOUSELEAVE may come).
    if (inside_ && !ContainsCursor()) {
        CancelTracking();
        inside_ = false;
        hot_ = -1;
        hotPart_ = 0;
        if (hoverCallback_) hoverCallback_(false);
    }
    return true;
}

void PopupWindow::StartFade(float to) {
    if (!root_.visual || !renderer_) return;
    winrt::com_ptr<IDCompositionVisual3> root3;
    if (FAILED(root_.visual->QueryInterface(IID_PPV_ARGS(root3.put())))) {
        fade_ = Tween{to, to, Renderer::Now(), 0.0, true};
        return;
    }
    const double now = Renderer::Now();
    const float remaining = std::fabs(to - fade_.Value(now));
    // A partial reversal takes the remaining fraction of the fade (at least 30 ms, or the whole
    // fade when that is shorter); 0 s: no fade.
    const double duration =
        fadeS_ > 0.0 ? std::max(std::min(0.03, fadeS_), fadeS_ * static_cast<double>(remaining)) : 0.0;
    renderer_->Drive(fade_, to, duration, now, [&](auto v) { return root3->SetOpacity(v); }, 0.001f,
                     renderer_->FadeEasing());
    if (fade_.Running(now) && !fadePending_) {
        SetTimer(hwnd_, kFadeTimerId, 16, nullptr);
        fadePending_ = true;
    }
}

void PopupWindow::OnFadeTimer() {
    KillTimer(hwnd_, kFadeTimerId);
    fadePending_ = false;
    const double now = Renderer::Now();
    const float v = std::clamp(fade_.Value(now), 0.0f, 1.0f);
    if (fade_.Running(now)) {
        OnFade(static_cast<BYTE>(std::lround(v * 255.0f)), false);
        SetTimer(hwnd_, kFadeTimerId, 16, nullptr);
        fadePending_ = true;
        return;
    }
    if (state_ == State::FadingOut) {
        FinishHide();
        return;
    }
    OnFade(static_cast<BYTE>(std::lround(v * 255.0f)), false);
}

void PopupWindow::Hide(bool animate) {
    if (state_ == State::Hidden || !hwnd_) return;
    if (!animate) {
        FinishHide();
        return;
    }
    if (state_ == State::FadingOut) return;
    EndMenuHold(false);
    state_ = State::FadingOut;
    std::fill(std::begin(pressed_), std::end(pressed_), -1);
    StartFade(0.0f);
    Commit();
    if (!fadePending_) FinishHide();  // nothing to animate
}

void PopupWindow::CancelTracking() {
    if (tracking_ && hwnd_) {
        TRACKMOUSEEVENT cancel{sizeof(cancel), TME_LEAVE | TME_CANCEL, hwnd_, 0};
        TrackMouseEvent(&cancel);
    }
    tracking_ = false;
}

void PopupWindow::FinishHide() {
    if (fadePending_) KillTimer(hwnd_, kFadeTimerId);
    fadePending_ = false;
    EndMenuHold(false);
    keptForeground_ = nullptr;
    HookForeground(false);
    state_ = State::Hidden;  // first: hiding an active click popup sends WM_ACTIVATE (ignored now)
    OnHidden();
    hot_ = -1;
    hotPart_ = 0;
    std::fill(std::begin(pressed_), std::end(pressed_), -1);
    CancelTracking();
    inside_ = false;
    if (hwnd_) {
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOSIZE);
    }
    backdrop_.Hide();
    if (renderer_ && content_.visual) {
        renderer_->DrawLayer(content_, 0, 0, {});  // frees the surface
        Commit();
    }
    fade_.valid = false;
}

void PopupWindow::CloseSelf() {
    if (state_ != State::Shown) return;
    Hide(true);
    if (closedCallback_) closedCallback_();
}

void PopupWindow::HookForeground(bool on) {
    auto& popups = HookedPopups();
    if (!on) {
        if (foregroundHook_) UnhookWinEvent(foregroundHook_);
        foregroundHook_ = nullptr;
        std::erase(popups, this);
        return;
    }
    if (foregroundHook_ || mode_ != Mode::Click) return;
    // Out of context: the callback runs on this thread from its message loop, no DLL injection.
    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                      &PopupWindow::OnForegroundEvent, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (foregroundHook_) popups.push_back(this);
}

void CALLBACK PopupWindow::OnForegroundEvent(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG,
                                             DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW || !hwnd) return;
    // WINEVENT_SKIPOWNPROCESS: this is another process's window (the settings window or a menu
    // of the dock never close the popup this way; deactivation does that when it was active).
    for (PopupWindow* p : HookedPopups()) {
        if (p->foregroundHook_ == hook) {
            // Round 19 phase 19A: held for a foreign menu (its owner window comes to the
            // foreground before or after MENUPOPUPSTART), or the window it stayed open behind.
            if (p->menuHold_ || hwnd == p->keptForeground_) return;
            p->CloseSelf();
            return;
        }
    }
}

void PopupWindow::HoldForMenu(DWORD pid) {
    if (mode_ != Mode::Click || state_ != State::Shown || !hwnd_) return;
    // Held already (a second right click before the first menu showed): start over.
    HookMenus(true);
    menuHold_ = true;
    menuHoldPid_ = pid;
    menuPid_ = 0;
    menuDepth_ = 0;
    menuStarted_ = false;
    keptForeground_ = nullptr;
    SetTimer(hwnd_, kMenuHoldTimerId, kMenuHoldStartMs, nullptr);  // one-shot (killed when it fires)
    menuTimer_ = true;
}

void PopupWindow::EndMenuHold(bool check) {
    if (!menuHold_) return;
    menuHold_ = false;
    HookMenus(false);
    if (menuTimer_ && hwnd_) KillTimer(hwnd_, kMenuHoldTimerId);
    menuTimer_ = false;
    if (!check || state_ != State::Shown) return;
    HWND fg = GetForegroundWindow();
    if (!fg) return;  // between two windows (a menu just closed): nothing says the user left
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return;  // the popup itself (clicked back on) or the dock
    if (pid == menuHoldPid_ || (menuPid_ != 0 && pid == menuPid_)) {
        // The menu was dismissed (Escape, a click on the popup that the menu ate) and the app's
        // (hidden) owner window kept the foreground: stay open behind it. The popup is not active
        // now and gets no WA_INACTIVE again, so the foreground hook closes it on the next change
        // (armed here also when the popup never had the foreground: opened by automation).
        keptForeground_ = fg;
        HookForeground(true);
        return;
    }
    CloseSelf();
}

void PopupWindow::HookMenus(bool on) {
    auto& popups = MenuHookedPopups();
    if (!on) {
        if (menuHook_) UnhookWinEvent(menuHook_);
        menuHook_ = nullptr;
        std::erase(popups, this);
        return;
    }
    if (menuHook_) return;
    // Out of context (no DLL injection), other processes only: the dock's own menus never count.
    menuHook_ = SetWinEventHook(EVENT_SYSTEM_MENUPOPUPSTART, EVENT_SYSTEM_MENUPOPUPEND, nullptr, &PopupWindow::OnMenuEvent,
                                0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (menuHook_) popups.push_back(this);
}

void CALLBACK PopupWindow::OnMenuEvent(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    for (PopupWindow* p : MenuHookedPopups()) {
        if (p->menuHook_ != hook) continue;
        if (!p->menuHold_) return;
        if (event == EVENT_SYSTEM_MENUPOPUPSTART) {
            ++p->menuDepth_;
            p->menuStarted_ = true;
            if (hwnd) {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid != 0) p->menuPid_ = pid;
            }
            // The menu is up: it may stay open as long as the user likes; its end ends the hold.
            if (p->menuTimer_ && p->hwnd_) KillTimer(p->hwnd_, kMenuHoldTimerId);
            p->menuTimer_ = false;
        } else if (event == EVENT_SYSTEM_MENUPOPUPEND) {
            if (p->menuDepth_ > 0) --p->menuDepth_;
            if (p->menuStarted_ && p->menuDepth_ == 0) p->EndMenuHold(true);  // unhooks: `popups` changes
        }
        return;
    }
}

bool PopupWindow::ContainsCursor() const {
    if (state_ == State::Hidden) return false;
    POINT pt{};
    return GetCursorPos(&pt) && PtInRect(&rect_, pt);
}

void PopupWindow::SetHot(int index, int part) {
    if (index < 0) part = 0;
    if (index == hot_ && part == hotPart_) return;
    hot_ = index;
    hotPart_ = part;
    if (state_ == State::Hidden) return;
    OnHotChanged();
}

void PopupWindow::OnHotChanged() {
    if (SUCCEEDED(Redraw())) Commit();
}

void PopupWindow::ButtonDown(Button button, POINT pt) {
    const size_t b = static_cast<size_t>(button);
    int part = 0;
    pressed_[b] = state_ == State::Shown ? HitAt(pt, &part) : -1;
    pressedPart_[b] = part;
}

void PopupWindow::ButtonUp(Button button, POINT pt) {
    const size_t b = static_cast<size_t>(button);
    int part = 0;
    const int index = HitAt(pt, &part);
    // Middle click acts on the item as a whole; left / right need the same part as the press.
    const bool clicked = state_ == State::Shown && index >= 0 && index == pressed_[b] &&
                         (button == Button::Middle || part == pressedPart_[b]);
    if (button == Button::Middle) part = pressedPart_[b];
    pressed_[b] = -1;
    if (clicked) OnClick(index, part, button);  // may hide the popup
}

LRESULT CALLBACK PopupWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PopupWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<PopupWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PopupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self || !self->hwnd_ || self->hwnd_ != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(msg, wp, lp);
}

LRESULT PopupWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    {
        LRESULT result = 0;
        if (OnMessage(msg, wp, lp, result)) return result;
    }
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return mode_ == Mode::Hover ? MA_NOACTIVATE : MA_ACTIVATE;

        case WM_ACTIVATE:
            // A click popup closes when another window takes the foreground; not while held for a
            // foreign menu (round 19 phase 19A: the app's menu owner takes it). Activated again
            // (clicked back on): the hold is over and the popup stays.
            if (mode_ == Mode::Click) {
                if (LOWORD(wp) == WA_INACTIVE) {
                    if (!menuHold_) CloseSelf();
                } else {
                    keptForeground_ = nullptr;
                    EndMenuHold(false);
                }
            }
            break;  // DefWindowProc gives the window the keyboard focus on activation

        case WM_KEYDOWN:
            if (mode_ == Mode::Click && wp == VK_ESCAPE) {
                CloseSelf();
                return 0;
            }
            break;

        case WM_MOUSEMOVE: {
            if (state_ != State::Shown) return 0;
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                tracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            if (!inside_) {
                inside_ = true;
                if (hoverCallback_) hoverCallback_(true);
            }
            int part = 0;
            const int index = HitAt(pt, &part);
            SetHot(index, part);
            return 0;
        }

        case WM_MOUSELEAVE:
            tracking_ = false;
            std::fill(std::begin(pressed_), std::end(pressed_), -1);
            SetHot(-1, 0);
            if (inside_) {
                inside_ = false;
                if (hoverCallback_) hoverCallback_(false);
            }
            return 0;

        case WM_LBUTTONDOWN:
            ButtonDown(Button::Left, pt);
            return 0;
        case WM_LBUTTONUP:
            ButtonUp(Button::Left, pt);
            return 0;
        case WM_MBUTTONDOWN:
            ButtonDown(Button::Middle, pt);
            return 0;
        case WM_MBUTTONUP:
            ButtonUp(Button::Middle, pt);
            return 0;
        case WM_RBUTTONDOWN:
            ButtonDown(Button::Right, pt);
            return 0;
        case WM_RBUTTONUP:
            ButtonUp(Button::Right, pt);
            return 0;

        case WM_TIMER:
            if (wp == kFadeTimerId) {
                OnFadeTimer();
                return 0;
            }
            if (wp == kMenuHoldTimerId) {
                // No menu came within kMenuHoldStartMs: the foreground decides.
                KillTimer(hwnd_, kMenuHoldTimerId);
                menuTimer_ = false;
                EndMenuHold(true);
                return 0;
            }
            break;

        case WM_WINDOWPOSCHANGED:
            if (!(reinterpret_cast<const WINDOWPOS*>(lp)->flags & SWP_NOZORDER)) backdrop_.Restack(hwnd_);
            break;

        case WM_DPICHANGED:
            return 0;  // the dock places and scales the popup

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            ValidateRect(hwnd_, nullptr);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
