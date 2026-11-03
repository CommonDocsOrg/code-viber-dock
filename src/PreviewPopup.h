#pragma once

#include <windows.h>
#include <dwmapi.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "PopupWindow.h"
#include "Renderer.h"

// The hover popup with live window thumbnails for apps with several windows (a PopupWindow in
// hover mode, class WinDockPreview) with one card per window: title, close glyph and a DWM
// thumbnail (DwmRegisterThumbnail; DWM renders it, this process does nothing while the popup is
// shown and the mouse is still). The thumbnails follow the popup's fade through their DWM
// opacity (OnFade).
class PreviewPopup : public PopupWindow {
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

    PreviewPopup() : PopupWindow(kClassName, L"WinDock preview", Mode::Hover) {}
    ~PreviewPopup() override { Destroy(); }

    void Init(HINSTANCE instance, Renderer* renderer, Callbacks callbacks);
    // Shows the popup (fading in) or updates it in place at `anchor` (PopupAnchor: beside the dock,
    // away from its edge), clamped to the monitor. Returns false if it could not be shown.
    bool Show(const Content& content, const Style& style, const PopupAnchor& anchor);

    size_t CardCount() const { return OnScreen() ? cards_.size() : 0; }
    HWND CardWindow(size_t card) const { return OnScreen() && card < cards_.size() ? cards_[card].window : nullptr; }
    // Characters in a card's title (-1: no such card).
    long CardTitleLength(size_t card) const {
        return OnScreen() && card < cards_.size() ? static_cast<long>(cards_[card].title.size()) : -1;
    }
    // These windows' captions changed: re-reads the titles of their cards and repaints (the caller
    // commits). True if a card changed.
    bool UpdateTitles(const std::vector<HWND>& windows);

protected:
    SIZE Measure() override;
    void Paint(ID2D1DeviceContext* dc) override;
    int HitAt(POINT pt, int* part) const override;
    void OnClick(int index, int part, Button button) override;
    void OnFade(BYTE opacity, bool placed) override;
    void OnHidden() override;
    void OnDestroying() override;

private:
    enum Part { kCard = 0, kClose = 1 };
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

    void SyncCards(const std::vector<HWND>& windows);
    void ReleaseThumbs();
    void EnsureFormats();

    Callbacks callbacks_;
    Style style_;
    Content contentData_;
    std::vector<Card> cards_;
    size_t hiddenCount_ = 0;  // windows beyond the cards ("+N more")
    RECT moreRect_{};

    float formatScale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> titleFormat_;
    winrt::com_ptr<IDWriteTextFormat> moreFormat_;
    winrt::com_ptr<IDWriteInlineObject> ellipsis_;
};
