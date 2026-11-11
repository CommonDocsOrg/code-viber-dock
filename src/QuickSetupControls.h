#pragma once

// Round 13 phase 13H: the painting kit and the one custom control of Quick setup's redesign (the
// approved prototype of 2026-11-10: a step rail, cards, switches, segmented controls and tiles).
//
// - Canvas: Direct2D (software, on a GDI DC) for the shapes, so the circles, switches and rounded
//   cards are antialiased and the docks of the step 4 preview blend with the wallpaper by their ARGB.
//   Text stays GDI (DrawTextW after End()): the same fonts and ClearType as the system controls
//   around it. One factory and one DC render target for the thread (Quick setup lives on the UI
//   thread), bound to each paint's DC; nothing is drawn outside a paint.
// - Buffer: a memory DC for one paint (copied in for a control drawn over its own background), so a
//   repaint never flickers and the D2D target binds to a plain bitmap, never to a themed control's
//   buffered-paint DC.
// - DrawGlyph: the prototype's SVG icons as strokes and fills in their own view box (no icon font:
//   the monitor-edge, auto-hide and start-up pictures have no Segoe Fluent Icons counterpart, and one
//   way of drawing keeps the line weights alike).
// - DrawButton / DrawSwitch: what the window's NM_CUSTOMDRAW of a push button / check box paints
//   (the controls stay standard buttons: BM_CLICK, BM_GETCHECK / BM_SETCHECK and WM_COMMAND work as
//   before, only CDDS_PREPAINT answers CDRF_SKIPDEFAULT).
// - Choice (WinDock.QuickSetupChoice): a segmented control or a row of tiles, one selected option;
//   arrow keys / Home / End choose (as a radio group), a click chooses, WM_COMMAND(MAKEWPARAM(id,
//   kChosen), hwnd) tells the parent. Quick setup keeps a hidden combo beside each one (the automation
//   drives the combos) and the choice only mirrors it.

#include <Unknwn.h>
#include <windows.h>
#include <commctrl.h>
#include <d2d1_1.h>

#include <winrt/base.h>

#include <string>
#include <vector>

#include "DarkMode.h"

namespace QsUi {

COLORREF Mix(COLORREF a, COLORREF b, int percentA);

// The colours of the redesign, every one a mix of the DarkMode palette (the dark results are the
// prototype's: base #202020, rail #1A1A1A, card #2B2B2B, wells #1F1F1F, hairlines white 7 - 10 %).
struct Colors {
    bool dark = false, highContrast = false;
    COLORREF base = 0;        // the content column (palette.base)
    COLORREF rail = 0;        // the step rail, a little darker
    COLORREF card = 0;        // cards and tiles (palette.layer)
    COLORREF well = 0;        // a segmented control's groove, darker than a card
    COLORREF raised = 0;      // a segmented control's chosen segment (palette.control)
    COLORREF text = 0, muted = 0, faint = 0, disabled = 0;
    COLORREF accent = 0, accentText = 0;  // accentText: legible on the accent (black or white)
    COLORREF border = 0;      // a card's 1 px frame
    COLORREF hair = 0;        // a divider line
    COLORREF knob = 0;        // a slider thumb's middle
};
Colors ColorsFor(const DarkMode::Palette& p);
// `c` mixed `percent` % into `back` (the prototype's rgba(255, 255, 255, .06) over a card is
// Tint(text, card, 6)); high contrast: `c` itself only where the caller says so.
inline COLORREF Tint(COLORREF c, COLORREF back, int percent) { return Mix(c, back, percent); }

inline D2D1_RECT_F F(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom));
}

class Buffer {
public:
    // A memory bitmap the size of `rc` (in `target`'s coordinates). `copyIn`: it starts with what
    // `target` shows there. Drawing uses 0-based coordinates; the destructor copies it back.
    Buffer(HDC target, const RECT& rc, bool copyIn = false);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    HDC Dc() const { return dc_; }
    int W() const { return w_; }
    int H() const { return h_; }

private:
    HDC target_ = nullptr, dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
    RECT rc_{};
    int w_ = 0, h_ = 0;
};

class Canvas {
public:
    Canvas(HDC dc, int w, int h);  // BeginDraw over (0, 0, w, h) of `dc`
    ~Canvas() { End(); }
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    void End();  // the shapes land in the DC (then GDI text may go on top)
    bool Ok() const { return dc_ != nullptr; }

    void FillRound(const D2D1_RECT_F& r, float radius, COLORREF c, float alpha = 1.0f);
    void StrokeRound(const D2D1_RECT_F& r, float radius, COLORREF c, float width, float alpha = 1.0f, bool dashed = false);
    void FillArgb(const D2D1_RECT_F& r, float radius, uint32_t argb);  // a ThemeConfig colour
    void StrokeArgb(const D2D1_RECT_F& r, float radius, uint32_t argb, float width);
    void FillCircle(float cx, float cy, float radius, COLORREF c, float alpha = 1.0f);
    void StrokeCircle(float cx, float cy, float radius, COLORREF c, float width, float alpha = 1.0f);
    void Line(float x0, float y0, float x1, float y1, COLORREF c, float width, float alpha = 1.0f);
    void Polyline(const D2D1_POINT_2F* pts, int n, COLORREF c, float width, float alpha = 1.0f);
    // The pretend desktop picture of the dock-card monitors and the step 4 preview (the prototype's
    // radial blue, the same in every theme as the gallery's mini-dock wallpaper is).
    void FillWallpaper(const D2D1_RECT_F& r, float radius);
    void FillGradient(const D2D1_RECT_F& r, float radius, COLORREF from, COLORREF to);  // top left to bottom right

private:
    HDC hdc_ = nullptr;  // the GDI fallback's (no Direct2D: plain round rectangles)
    winrt::com_ptr<ID2D1DeviceContext> dc_;
    winrt::com_ptr<ID2D1SolidColorBrush> brush_;
    bool begun_ = false;
};

enum class Glyph {
    None, Check, Close, Add, Info, DockMark,
    EdgeTop, EdgeBottom, EdgeLeft, EdgeRight,  // a monitor with a bar on that edge
    StartUp, Taskbar,                          // step 3's two switch cards
    HideNever, HideAlways, HideOverlap,        // step 3's auto-hide tiles
};
// `box`: the icon's square in px. `cutout`: the colour behind it (HideOverlap's window is filled with it).
void DrawGlyph(Canvas& c, Glyph g, const D2D1_RECT_F& box, COLORREF color, COLORREF cutout = 0, float alpha = 1.0f);

// A push button drawn in the redesign's kinds (NM_CUSTOMDRAW, CDDS_PREPAINT). `state`: the
// NMCUSTOMDRAW uItemState bits (CDIS_HOT / SELECTED / FOCUS / DISABLED / SHOWKEYBOARDCUES).
enum class ButtonKind { Standard, Primary, Ghost, Dashed, Icon, Link };
void DrawButton(HDC dc, const RECT& rc, ButtonKind kind, const std::wstring& text, Glyph glyph, UINT state, HFONT font,
                const Colors& k, COLORREF back, float s);
// A 44 x 24 DIP toggle switch centred in `rc` (a check box's whole client).
void DrawSwitch(HDC dc, const RECT& rc, bool on, UINT state, const Colors& k, COLORREF back, float s);
// A trackbar (NM_CUSTOMDRAW, CDDS_PREPAINT): the groove filled in the accent up to the thumb, the
// thumb an accent ring. Returns the custom-draw answer.
LRESULT DrawSlider(NMCUSTOMDRAW& cd, const Colors& k, COLORREF back, float s);

class Choice {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.QuickSetupChoice";
    static constexpr WORD kChosen = 1;               // WM_COMMAND code: the selection changed by the user
    static constexpr WORD kFocused = 2;              // WM_COMMAND code: it got the focus (the rows scroll it into view)
    static constexpr UINT kMsgChoose = WM_USER + 2;  // automation: wParam = option, chosen as a click would (1 done, 0 no such option)
    static constexpr UINT kMsgSelected = WM_USER + 3;  // returns the selected option (-1 none)
    enum class Style { Segments, Tiles };
    struct Option {
        std::wstring text;    // a segment's label / a tile's title ("" a glyph-only segment)
        std::wstring detail;  // a tile's one-line meaning
        Glyph glyph = Glyph::None;
        std::wstring name;    // a glyph-only segment's accessible name ("Top")
    };
    struct Look {
        Colors colors;
        COLORREF back = 0;  // what the control sits on
        HFONT font = nullptr, bold = nullptr, smallFont = nullptr;  // ("small" is a macro of rpcndr.h)
        bool compactTiles = false;  // the tile's glyph beside its title (a short window), not over it
    };

    static bool Register(HINSTANCE instance);
    static HWND Create(HWND parent, int id, Style style, const wchar_t* name, HINSTANCE instance);
    static void SetOptions(HWND hwnd, std::vector<Option> options);
    static void SetSelected(HWND hwnd, int index);  // no notification; -1 none (a mixed value)
    static int Selected(HWND hwnd);
    static void SetLook(HWND hwnd, const Look& look);
    // Tiles: the height (px) the tiles need at the control's width (their details wrapped).
    static int TilesHeight(HWND hwnd);
};

}  // namespace QsUi
