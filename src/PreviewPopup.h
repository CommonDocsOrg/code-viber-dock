#pragma once

#include <windows.h>
#include <dwmapi.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Backdrop.h"
#include "Renderer.h"

// The hover popup with live window thumbnails for apps with several windows: a separate
// top-level window (class WinDockPreview; WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST)
// with one card per window: title, close glyph and a DWM thumbnail (DwmRegisterThumbnail; DWM
// renders it, this process does nothing while the popup is shown and the mouse is still).
//
// The cards, titles and background are drawn with the dock's Renderer through a second
// composition target on the same device (Renderer::CreateWindowTarget), so there is no second
// D3D/D2D device. With acrylic, a Backdrop window (same recipe as the bar's) sits directly
// beneath the popup. Fade in/out: the target root's opacity (DirectComposition animation) plus
// the thumbnails' DWM opacity, stepped by a 16 ms one-shot timer only while the fade runs.
class PreviewPopup {
public:
    static constexpr wchar_t kClassName[] = L"WinDockPreview";
    static constexpr size_t kMaxCards = 6;  // more windows: "+N more"

    struct Callbacks {
        std::function<void(HWND)> activate;     // card clicked
        std::function<void(HWND)> close;        // close glyph or middle click
        std::function<void(bool inside)> hover;  // the mouse entered / left the popup
    };
    struct Content {
        std::vector<HWND> windows;                // first-seen order
        winrt::com_ptr<ID2D1Bitmap> icon;         // app icon for cards without a thumbnail
        bool operator==(const Content& o) const { return windows == o.windows && icon == o.icon; }
    };
    struct Style {
        float scale = 1.0f;
        bool acrylic = false;
        uint32_t tint = 0xCC202020;   // ARGB background
        uint32_t text = 0xFFFFFFFF;
        uint32_t border = 0x24FFFFFF;         // hairline around the popup
        uint32_t hotBackground = 0x1AFFFFFF;  // hovered card (half alpha: the icon box of a card without thumbnail)
        uint32_t hotBorder = 0x33FFFFFF;      // hovered card outline
        uint32_t closeHover = 0xFFC42B1C;     // close button behind the hovered "x"
        float cornerRadiusDip = 8.0f; // without acrylic
        RECT monitor{};
    };

    PreviewPopup() = default;
    PreviewPopup(const PreviewPopup&) = delete;
    PreviewPopup& operator=(const PreviewPopup&) = delete;
    ~PreviewPopup() { Destroy(); }

    void Init(HINSTANCE instance, Renderer* renderer, Callbacks callbacks);
    // Shows the popup (fading in) or updates it in place: centred on screen x `anchorX`, bottom
    // edge at screen y `bottom`, clamped to the monitor. Returns false if it could not be shown.
    bool Show(const Content& content, const Style& style, int anchorX, int bottom);
    // Fades out (animate) or hides at once. Thumbnails are unregistered when it is hidden.
    void Hide(bool animate);
    // Destroys the window, the composition target and every thumbnail (device loss, shutdown).
    // Does not commit.
    void Destroy();

    // Shown or fading in (not fading out, not hidden).
    bool Visible() const { return state_ == State::Shown; }
    bool OnScreen() const { return state_ != State::Hidden; }
    size_t CardCount() const { return state_ == State::Hidden ? 0 : cards_.size(); }
    bool FadePending() const { return fadePending_; }
    HWND CardWindow(size_t card) const {
        return state_ != State::Hidden && card < cards_.size() ? cards_[card].window : nullptr;
    }
    bool ContainsCursor() const;
    HWND Hwnd() const { return hwnd_; }

private:
    enum class State { Hidden, Shown, FadingOut };
    struct Card {
        HWND window = nullptr;
        HTHUMBNAIL thumb = nullptr;
        bool hasThumb = false;   // registered with a non-empty source size
        std::wstring title;
        RECT rect{};             // card, client px
        RECT thumbRect{};        // thumbnail destination (fitted), client px
        RECT boxRect{};          // thumbnail box
        RECT closeRect{};        // close glyph
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    bool EnsureWindow();
    void SyncCards(const std::vector<HWND>& windows);
    void ReleaseThumbs();
    void LayoutCards();
    HRESULT Draw();
    void UpdateThumbs(BYTE opacity, bool full);
    void StartFade(float to);
    void OnFadeTimer();
    void FinishHide();
    void SetHot(int card, bool close);
    int CardAt(POINT pt, bool* onClose) const;
    void Commit();

    HINSTANCE instance_ = nullptr;
    Renderer* renderer_ = nullptr;
    Callbacks callbacks_;
    HWND hwnd_ = nullptr;
    winrt::com_ptr<IDCompositionTarget> target_;
    Layer root_;
    Layer content_;
    Backdrop backdrop_;

    State state_ = State::Hidden;
    Style style_;
    Content contentData_;
    std::vector<Card> cards_;
    size_t hiddenCount_ = 0;  // windows beyond the cards ("+N more")
    RECT moreRect_{};
    SIZE size_{};             // client size, px
    RECT rect_{};             // screen
    int hot_ = -1;            // hovered card
    bool hotClose_ = false;   // hovered close glyph of hot_
    int pressed_ = -1;
    bool pressedClose_ = false;
    int pressedMiddle_ = -1;
    bool tracking_ = false;
    bool inside_ = false;

    Tween fade_;
    bool fadePending_ = false;
    float formatScale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> titleFormat_;
    winrt::com_ptr<IDWriteTextFormat> moreFormat_;
    winrt::com_ptr<IDWriteInlineObject> ellipsis_;
};
