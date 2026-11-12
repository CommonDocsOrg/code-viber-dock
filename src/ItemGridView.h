#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <winrt/base.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Renderer.h"

// One entry of an ItemGridView (the host's model; the view only draws and navigates it).
struct GridItem {
    std::wstring label;
    std::wstring secondary;            // list mode: dimmer text at the right (size, date); optional
    std::wstring detail;               // detail rows (Style::detailRows): up to two lines under the label
    winrt::com_ptr<ID2D1Bitmap> icon;  // null: none (yet); the host may fill it in `resolve`
    uint64_t id = 0;                   // the host's
    uint32_t flags = 0;                // the host's (e.g. "a folder")
    bool resolved = false;             // the resolve callback has run for it (or the host filled it)
};

// A scrolling grid / list of icons with labels, drawn into composition layers under a parent Layer
// of a host window (a PopupWindow's content: the folder / group popup; later the tray drawer and
// the desktop folder docks). It has no window of its own and no dependency on any popup: the host
// gives it a rect in its client area, forwards mouse / keyboard / timer messages and the OLE drop
// feedback, and gets clicks, context menus, drags and drops back through callbacks.
//
// Rendering: a viewport visual (clipped to the rect) holds a content visual whose OffsetY is the
// scroll position (driven by DirectComposition through Renderer::Drive, so wheel and keyboard
// scrolling animate without a frame loop) and one visual + surface per *visible* row (rows outside
// the viewport have no visual: 500 entries cost a screenful of surfaces). During an animated
// scroll the rows of the start and end positions exist; a one-shot timer at the end of the
// animation drops the ones that scrolled away. A thin scrollbar thumb (its own visual, moved by the
// same curve) fades in on scrolling and out after a second (one one-shot timer). Hover, selection
// and drop feedback redraw only the rows they touch. Nothing runs while the view is idle.
//
// Layout: grid (fixed cells, the host picks the column count, the cells centred in the width) or
// list (one entry per row: icon, name, optional secondary text). Keyboard: arrows, Home / End,
// Page Up / Down, Enter (activate), the Apps key / Shift+F10 (context menu), typing selects the
// next entry whose label starts with the typed text. Escape / Backspace are the host's.
class ItemGridView {
public:
    enum class Mode { Grid, List };
    enum class Button { Left, Middle, Right };
    // Timer ids used on the host window: timerBase + 0 (end of a scroll animation), + 1 (the
    // scrollbar's fade-out delay).
    static constexpr UINT_PTR kTimerCount = 2;

    struct Style {
        float scale = 1.0f;                   // DPI scale
        uint32_t text = 0xFFFFFFFF;           // ARGB
        uint32_t secondary = 0xB6FFFFFF;      // list mode's secondary text
        uint32_t hotBackground = 0x1AFFFFFF;  // hovered / selected entry
        uint32_t hotBorder = 0x33FFFFFF;
        uint32_t accent = 0xFF4CC2FF;         // keyboard selection outline, drop target
        uint32_t scrollbar = 0x80FFFFFF;      // the thumb
        // Grid cell (DIPs): the icon on top, two lines of name under it.
        float cellW = 88.0f;
        float cellH = 84.0f;
        float iconDip = 40.0f;
        // List row (DIPs).
        float rowH = 36.0f;
        float listIconDip = 24.0f;
        float fontDip = 12.0f;
        double scrollSeconds = 0.16;          // wheel / keyboard scroll animation (0: none)
        bool reduceMotion = false;            // scroll without animation
        // List mode, detail rows (the notification centre): the icon at the top left, the label in
        // semibold on the first line with `secondary` (a time) at its right, `detail` in up to two
        // dimmer lines under it. Make rowH tall enough (about 68 DIPs).
        bool detailRows = false;
        // A close glyph at the right of the hovered row's first line (in place of `secondary`); a
        // click on it calls Callbacks::close instead of activate.
        bool closeButton = false;
        // false (the desktop folder windows): a left click selects, a double click (DoubleClick)
        // or Enter activates.
        bool clickActivates = true;
        // Grid: lines of name under the icon (1 or 2; make cellH fit them).
        int labelLines = 2;
        // Names drawn with a dark shadow (entries over the wallpaper, no panel behind them).
        bool textShadow = false;
    };

    struct Callbacks {
        // Before an entry is first drawn (resolved false): fill its icon / label / secondary text.
        std::function<void(size_t index, GridItem& item)> resolve;
        // A click (left / middle) on an entry, or Enter on the selected one (Left).
        std::function<void(size_t index, Button button)> activate;
        // Right click / Apps key on an entry (-1: on the background), at a screen point.
        std::function<void(int index, POINT screen)> contextMenu;
        // A press on an entry that moved past the drag threshold while SetDragOut(true) (the
        // button is still down: the host may start an OLE drag). The view has released the mouse.
        std::function<void(size_t index, POINT screen)> beginDrag;
        // The close glyph of an entry was clicked (Style::closeButton).
        std::function<void(size_t index)> close;
    };

    ItemGridView() = default;
    ItemGridView(const ItemGridView&) = delete;
    ItemGridView& operator=(const ItemGridView&) = delete;
    ~ItemGridView() { Detach(); }

    void Init(Renderer* renderer, HWND host, UINT_PTR timerBase, Callbacks callbacks);
    // Creates the view's visuals under `parent` (in front of its content); Detach removes them
    // (and stops the timers). Attach while attached to another parent re-parents.
    HRESULT Attach(Layer& parent);
    void Detach();
    bool Attached() const { return viewport_.visual != nullptr; }

    // --- model and layout ---
    void SetStyle(const Style& style);
    const Style& GetStyle() const { return style_; }
    // Replaces the entries: scroll back to the top, no hot / selected entry.
    void SetItems(std::vector<GridItem> items);
    size_t Count() const { return items_.size(); }
    GridItem& At(size_t index) { return items_[index]; }
    const GridItem& At(size_t index) const { return items_[index]; }
    // The entry's look changed (icon, label, dot): its row is redrawn.
    void InvalidateItem(size_t index);
    void SetMode(Mode mode);
    Mode GetMode() const { return mode_; }
    // Grid: entries per row (at least 1). List: always 1.
    void SetColumns(size_t columns);
    // Entries may be dragged out (the beginDrag callback): a press on an entry that moves starts
    // that drag; only the background (and the scrollbar) drag-scrolls. Off: every drag scrolls.
    void SetDragOut(bool on) { dragOut_ = on; }
    size_t Columns() const { return mode_ == Mode::List ? 1 : columns_; }
    // The viewport in the host's client px (the parent layer's coordinates).
    void SetBounds(const RECT& bounds);
    // Applies items / mode / columns / bounds / style changes: places the viewport, creates and
    // draws the visible rows. Call after a batch of the setters above (they only record).
    void Refresh();
    RECT Bounds() const { return bounds_; }
    // Sizes for the host's layout (px at the style's scale).
    LONG RowHeight() const;
    LONG CellWidth() const;
    size_t RowCount() const;
    LONG ContentHeight() const { return static_cast<LONG>(RowCount()) * RowHeight(); }

    // --- state ---
    // Keyboard selection (-1 none); `reveal` scrolls it into view (animated).
    void Select(int index, bool reveal);
    int Selected() const { return selected_; }
    // Multiple selection (the folder windows): on, a click selects one entry, Ctrl + click toggles
    // one, Shift + click / Shift + arrows select the range from the anchor, Ctrl+A every entry and
    // a click on the background none. Selected() is then the focused entry (the caret).
    void SetMultiSelect(bool on) { multi_ = on; }
    bool Marked(size_t index) const { return multi_ ? index < marked_.size() && marked_[index] : static_cast<int>(index) == selected_; }
    size_t MarkedCount() const;
    std::vector<size_t> MarkedIndexes() const;
    // A click's selection on entry `index` with Ctrl / Shift held (automation; -1 with neither: none).
    void SelectWith(int index, bool ctrl, bool shift, bool reveal);
    void SelectAll();
    int Hot() const { return hot_; }
    // Scroll position in px (the target of a running animation).
    LONG ScrollOffset() const { return static_cast<LONG>(std::lround(target_)); }
    LONG MaxScroll() const;
    void ScrollTo(float px, bool animate);
    // Rows with a visual right now (virtualisation check: a screenful, two during a scroll).
    size_t LiveRows() const { return rows_.size(); }
    // One of the view's one-shot timers is pending (end of a scroll, scrollbar fade).
    bool TimerPending() const { return animTimer_ || fadeTimer_; }
    // The entry under a client point of the host (-1: none / outside).
    int HitTest(POINT clientPt) const;
    bool Contains(POINT clientPt) const { return PtInRect(&bounds_, clientPt) != FALSE; }
    // The entry's rect in the host's client px at the current scroll position.
    RECT ItemRect(size_t index) const;
    // The close glyph's rect of an entry (Style::closeButton; host client px; empty without one).
    RECT CloseRect(size_t index) const;
    // Drop feedback: the entry a drag would drop into (-1: none). Redraws the affected rows.
    void SetDropHighlight(int index);
    int DropHighlight() const { return drop_; }
    // Round 19 phase 19A: insert feedback (the tray drawer's reorder): an accent line before entry
    // `index` (Count(): after the last; -1: none), in the gap between cells (grid) or rows (list).
    // Redraws the affected rows. InsertIndexAt gives the index for a client point: the nearest
    // cell edge of the row under it (clamped to the rows there are; -1 outside the view).
    void SetInsertMarker(int index);
    int InsertMarker() const { return insert_; }
    int InsertIndexAt(POINT clientPt) const;

    // --- input forwarded by the host (client px of the host window) ---
    void MouseMove(POINT pt);
    void MouseLeave();
    // True when the press is the view's (inside it); it then gets the matching ButtonUp too.
    bool ButtonDown(Button button, POINT pt);
    bool ButtonUp(Button button, POINT pt);
    // WM_LBUTTONDBLCLK: activates the entry under the point (Style::clickActivates false). True if
    // it was on an entry.
    bool DoubleClick(POINT pt);
    bool Pressed(Button button) const { return press_.active && press_.button == button; }
    // WM_MOUSEWHEEL (delta in WHEEL_DELTA units) at a client point; true if it scrolled the view.
    bool Wheel(int delta, POINT pt);
    // WM_KEYDOWN; true if handled (arrows, Home / End, Page Up / Down, Enter, Apps, Shift+F10).
    bool KeyDown(UINT vk);
    // WM_CHAR: type-to-select (true if consumed).
    bool Char(wchar_t ch);
    // WM_TIMER with one of the view's ids; true if it was one.
    bool Timer(UINT_PTR id);
    // WM_CAPTURECHANGED (the view may have had the capture for a press).
    void CaptureLost();

private:
    struct Row {
        size_t index = 0;  // row number
        Layer layer;
        bool dirty = true;
    };
    struct Press {
        bool active = false;
        Button button = Button::Left;
        POINT pt{};
        int item = -1;
        float scroll = 0.0f;
        bool scrolling = false;  // dragging the content
        bool thumb = false;      // dragging the scrollbar thumb
    };

    float Px(float dip) const { return std::round(dip * style_.scale); }
    float ViewHeight() const { return static_cast<float>(bounds_.bottom - bounds_.top); }
    float ViewWidth() const { return static_cast<float>(bounds_.right - bounds_.left); }
    float CurrentScroll() const;
    // Row range [first, last] intersecting content px [top, bottom).
    bool RowRange(float top, float bottom, size_t& first, size_t& last) const;
    // Makes rows for content px [top, bottom) exist (drawn), keeps the others.
    void EnsureRows(float top, float bottom);
    // Drops rows outside content px [top, bottom).
    void TrimRows(float top, float bottom);
    Row* FindRow(size_t row);
    HRESULT DrawRow(Row& row);
    void RedrawDirty();
    void InvalidateRowOf(int index);
    void InvalidateAll();
    void EnsureFormats();
    // Entry rect in content px (y relative to the content's top).
    D2D1_RECT_F ContentRect(size_t index) const;
    void SetHot(int index);
    // Animated (style) or static scroll to `px` (clamped).
    void ScrollToInternal(float px, bool animate);
    void Reveal(size_t index);
    // Scrollbar: geometry, appearance, fade.
    bool Scrollable() const { return MaxScroll() > 0; }
    void ThumbGeometry(float scroll, float& y, float& h) const;
    HRESULT DrawThumb();
    void ShowScrollbar();
    void FadeScrollbar(float to, double seconds);
    bool InScrollbar(POINT pt) const;
    void Commit();
    void StartTimer(UINT_PTR which, UINT ms);
    void StopTimer(UINT_PTR which);

    Renderer* renderer_ = nullptr;
    HWND host_ = nullptr;
    UINT_PTR timerBase_ = 0;
    Callbacks callbacks_;
    Style style_;
    Mode mode_ = Mode::Grid;
    size_t columns_ = 1;
    RECT bounds_{};
    std::vector<GridItem> items_;

    Layer* parent_ = nullptr;
    Layer viewport_;  // offset to bounds_, clipped to its size
    Layer content_;   // OffsetY = -scroll
    Layer thumb_;     // the scrollbar thumb
    std::vector<Row> rows_;
    Tween scrollY_;       // content_.OffsetY (= -scroll)
    Tween thumbY_;        // thumb_.OffsetY
    Tween thumbOpacity_;
    float target_ = 0.0f; // scroll px the view is at / animating to
    float thumbH_ = 0.0f; // the thumb surface's height (px) as drawn
    bool animTimer_ = false;
    bool fadeTimer_ = false;
    bool thumbShown_ = false;
    bool dragOut_ = false;

    int hot_ = -1;
    int selected_ = -1;
    bool multi_ = false;
    std::vector<uint8_t> marked_;  // multiple selection, parallel to items_
    int anchor_ = -1;              // Shift ranges start here
    int drop_ = -1;
    int insert_ = -1;  // SetInsertMarker
    Press press_;
    std::wstring typed_;       // type-to-select prefix
    ULONGLONG typedAt_ = 0;

    float formatScale_ = 0.0f;
    float formatFont_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> gridFormat_;  // two centred lines, ellipsized
    winrt::com_ptr<IDWriteTextFormat> listFormat_;  // one line, ellipsized
    winrt::com_ptr<IDWriteTextFormat> secondaryFormat_;  // one line, right aligned
    winrt::com_ptr<IDWriteInlineObject> gridEllipsis_;
    winrt::com_ptr<IDWriteInlineObject> listEllipsis_;
    // Detail rows: the label (semibold), the detail (two wrapped lines), the time (trailing).
    winrt::com_ptr<IDWriteTextFormat> detailTitleFormat_;
    winrt::com_ptr<IDWriteTextFormat> detailFormat_;
    winrt::com_ptr<IDWriteTextFormat> detailTimeFormat_;
    winrt::com_ptr<IDWriteInlineObject> detailEllipsis_;
};
