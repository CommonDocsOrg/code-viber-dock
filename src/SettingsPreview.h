#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <winrt/base.h>

#include <functional>
#include <string>
#include <vector>

#include "Clock.h"
#include "Config.h"

// The dock preview strip of the settings window (WinDock.SettingsPreview): a child window under
// every page that draws a miniature dock from the current Config with Direct2D. It has its own
// factory and DC render target (never the dock's Renderer or its DirectComposition device), it
// paints only on WM_PAINT (after Invalidate() or a mouse move: the magnification follows the
// cursor) and runs no timers. Everything is released when the window is destroyed.
class SettingsPreview {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.SettingsPreview";

    SettingsPreview() = default;
    ~SettingsPreview();
    SettingsPreview(const SettingsPreview&) = delete;
    SettingsPreview& operator=(const SettingsPreview&) = delete;

    bool Create(HINSTANCE instance, HWND parent, int id, const Config* config, UINT dpi);
    HWND Hwnd() const { return hwnd_; }
    // The dock it draws (its items and sizes; looked up at every paint; null: docks[0]) and whether
    // in the global theme (the Global window's Themes / Animation pages) or the dock's own.
    void SetDock(std::function<const DockConfig*()> dock, bool globalTheme) {
        dock_ = std::move(dock);
        globalTheme_ = globalTheme;
    }
    // Round 9 phase 9C: without the sample preview popup at the right (the bar gets the whole width;
    // the preset chooser's narrow strip).
    void SetShowPopup(bool show) {
        showPopup_ = show;
        Invalidate();
    }
    // Background colours of the strip (the settings window's content layer and a divider).
    void SetColors(COLORREF layer, COLORREF divider);
    // Round 12 phase 12F: the divider on the strip's bottom edge (it sits above the page: App
    // settings > Themes) instead of its top.
    void SetDividerBottom(bool bottom) {
        if (bottom == dividerBottom_) return;
        dividerBottom_ = bottom;
        Invalidate();
    }
    // The window moved to a monitor with another DPI: re-create the target at that DPI.
    void SetDpi(UINT dpi);
    // The config changed: repaint.
    void Invalidate();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    void Paint();
    HRESULT EnsureTarget();
    void ReleaseTarget();
    void ReleaseAll();
    void Draw(ID2D1DeviceContext* dc);
    IDWriteTextFormat* Format(winrt::com_ptr<IDWriteTextFormat>& slot, float& slotSize, float size,
                              DWRITE_FONT_WEIGHT weight);

    HWND hwnd_ = nullptr;
    const Config* config_ = nullptr;
    std::function<const DockConfig*()> dock_;
    bool globalTheme_ = false;
    bool showPopup_ = true;
    bool dividerBottom_ = false;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    COLORREF layer_ = RGB(0x2B, 0x2B, 0x2B);
    COLORREF divider_ = RGB(0x3A, 0x3A, 0x3A);
    // Cursor x in client pixels while the mouse is over the strip; < 0: the default focus.
    int mouseX_ = -1;
    bool tracking_ = false;

    winrt::com_ptr<ID2D1Factory1> factory_;
    winrt::com_ptr<IDWriteFactory> dwrite_;
    winrt::com_ptr<ID2D1DCRenderTarget> target_;  // bound to the WM_PAINT DC
    winrt::com_ptr<IDWriteTextFormat> labelFormat_;
    winrt::com_ptr<IDWriteTextFormat> letterFormat_;
    winrt::com_ptr<IDWriteTextFormat> titleFormat_;
    float labelSize_ = 0.0f, letterSize_ = 0.0f, titleSize_ = 0.0f;
    Clock clock_;
    std::wstring clockFormat_;  // what clock_ was configured with
    bool clockDate_ = false;
    std::wstring clockZone_;
    std::wstring clockLabel_;
    bool clockConfigured_ = false;
};

// Round 12 phase 12B: a small dock in theme `t` (four sample icons with their dots or slot buttons,
// the fill or the shelf, the sheen, the border, glow and reflection) on a wallpaper filling `box`
// (pixels); `s` = pixels per DIP. The gallery's tiles.
void DrawMiniDock(ID2D1DeviceContext* dc, const D2D1_RECT_F& box, const ThemeConfig& t, float s);

// Round 12 phase 12B: the theme gallery (WinDock.SettingsGallery), a control of the Themes page and
// of a dock's Look tab: one tile per look (a small dock drawn by DrawMiniDock, the name under it),
// wrapping to the control's width (SettingsPage::MeasureCustomDip asks HeightDip). The tile of the
// current look is marked (accent frame, "Current · n changed"). Arrow keys / Home / End move the
// selection, Enter, Space or a click chooses: WM_COMMAND(MAKEWPARAM(id, kChosen), hwnd) to the
// parent page, which reads Selected(). Software Direct2D on the WM_PAINT DC (as the preview strip:
// PrintWindow sees it); paints only when asked, no timers; everything goes with the window.
class SettingsGallery {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.SettingsGallery";
    static constexpr WORD kChosen = 1;                // the WM_COMMAND notification code
    static constexpr UINT kMsgSelect = WM_USER + 1;   // automation: wParam = tile index, selected (not chosen)
    static constexpr UINT kMsgChoose = WM_USER + 2;   // automation: wParam = tile index, chosen as a click would
    static constexpr int kTileW = 116, kTileH = 92, kGapX = 8, kGapY = 8;  // DIPs
    struct Tile {
        std::wstring name;    // "Glass", "App look (Dark)"
        std::wstring detail;  // the current tile's second line ("Current · 2 changed"; "" others)
        ThemeConfig theme;
        bool current = false;
    };

    SettingsGallery() = default;
    ~SettingsGallery();
    SettingsGallery(const SettingsGallery&) = delete;
    SettingsGallery& operator=(const SettingsGallery&) = delete;

    static bool Register(HINSTANCE instance);
    // The control the page's table created (CtlKind::Custom with kClassName).
    void Attach(HWND hwnd);
    HWND Hwnd() const { return hwnd_; }
    // New tiles; the selection goes to `selected` (clamped).
    void SetTiles(std::vector<Tile> tiles, int selected);
    int Selected() const { return selected_; }
    int Count() const { return static_cast<int>(tiles_.size()); }
    void SetColors(COLORREF back, COLORREF text, COLORREF muted, COLORREF accent);
    // The height of `count` tiles wrapped at `widthDip`.
    static int HeightDip(int count, int widthDip);

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);
    void Paint();
    float Scale() const;
    int Columns() const;
    D2D1_RECT_F TileRect(int index) const;  // client px
    int HitTest(POINT pt) const;
    void Select(int index);
    void Choose(int index);
    IDWriteTextFormat* Format(winrt::com_ptr<IDWriteTextFormat>& slot, float& size, float want, DWRITE_FONT_WEIGHT weight);

    HWND hwnd_ = nullptr;
    std::vector<Tile> tiles_;
    int selected_ = 0;
    int hot_ = -1;
    bool tracking_ = false;
    COLORREF back_ = RGB(0x2B, 0x2B, 0x2B), text_ = RGB(0xFF, 0xFF, 0xFF), muted_ = RGB(0xC0, 0xC0, 0xC0),
             accent_ = RGB(0x4C, 0xC2, 0xFF);
    winrt::com_ptr<ID2D1Factory1> factory_;
    winrt::com_ptr<IDWriteFactory> dwrite_;
    winrt::com_ptr<ID2D1DCRenderTarget> target_;
    winrt::com_ptr<IDWriteTextFormat> nameFormat_, detailFormat_;
    float nameSize_ = 0.0f, detailSize_ = 0.0f;
};
