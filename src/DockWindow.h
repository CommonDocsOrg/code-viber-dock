#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "AppIdentity.h"
#include "Backdrop.h"
#include "Clock.h"
#include "Config.h"
#include "DockItems.h"
#include "IconCache.h"
#include "PreviewPopup.h"
#include "Renderer.h"
#include "WindowTracker.h"

struct IDataObject;

// The dock HWND: placement, DPI, layout, input, magnification, hover label, auto-hide, and the
// composition layers drawn into it.
class DockWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.DockWindow";
    static constexpr UINT_PTR kClockTimerId = 1;
    // 3 = WindowTracker::kCoalesceTimerId
    static constexpr UINT_PTR kIconEvictTimerId = 4;
    // 5 = App::kSaveTimerId
    static constexpr UINT_PTR kBounceTimerId = 6;
    // One-shot timers of the interaction state machine; none of them runs while idle.
    static constexpr UINT_PTR kHideTimerId = 7;      // hideDelayMs after the mouse left
    static constexpr UINT_PTR kRevealTimerId = 8;    // revealDelayMs on the bottom-edge strip
    static constexpr UINT_PTR kSlideTimerId = 9;     // end of the slide-in/out animation
    static constexpr UINT_PTR kMagnifyTimerId = 10;  // throttles magnification updates
    static constexpr UINT_PTR kLabelTimerId = 11;    // hover label delay
    static constexpr UINT_PTR kRestTimerId = 12;     // end of the return-to-rest animation
    static constexpr UINT_PTR kBackdropTimerId = 13; // moves the acrylic window while the bar animates
    static constexpr UINT_PTR kDragTimerId = 14;     // polls Escape while an icon is dragged
    static constexpr UINT_PTR kRemoveTimerId = 15;   // end of the fade of an icon dragged off the dock
    static constexpr UINT_PTR kPreviewTimerId = 16;  // previewDelayMs hover delay before the thumbnail popup
    static constexpr UINT_PTR kPreviewLeaveTimerId = 17;  // grace period after leaving item and popup

    // Diagnostics / automation: SendMessage(dock, RegisterWindowMessage(kQueryItemMessage), wp, lp)
    // with wp = a top-level HWND (or 0 and lp = item index) returns the screen point of the
    // matching item's icon centre (rest layout) as MAKELONG(x, y), or -1 if there is no such item.
    static constexpr wchar_t kQueryItemMessage[] = L"WinDock.QueryItemPoint";
    // Diagnostics: SendMessage(dock, RegisterWindowMessage(kQueryStateMessage), what, 0):
    // what = 0: auto-hide state (0 Shown, 1 Hiding, 2 Hidden, 3 Showing); 1: the current
    // SHQueryUserNotificationState value (or -1); 2: hovered item index (-1 none); 3: 1 if the
    // hover label is visible; 4: 1 while magnified; 5: number of pending interaction timers;
    // 6: 1 while the acrylic backdrop is active; 7: drag state (0 none, 1 dragging an icon,
    // 2 fading out an icon dragged off the dock, 3 a file drag is over the dock); 8: 1 while the
    // dock window has the mouse capture; 9: insertion index of the current drag (-1 none);
    // 10: 1 if the OLE drop target is registered; 11: 1 while the thumbnail popup is visible;
    // 12: number of cards in the thumbnail popup; 13: the window shown by popup card lp (0 none).
    static constexpr wchar_t kQueryStateMessage[] = L"WinDock.QueryState";

    enum class HideState { Shown, Hiding, Hidden, Showing };

    // Called for every message before DockWindow handles it. Return true to consume
    // the message, with the LRESULT in `result`.
    using MessageHook = std::function<bool(HWND, UINT, WPARAM, LPARAM, LRESULT& result)>;

    // Changes to config->pinned the dock asks for (the dock only reads the config; the owner
    // edits it, calls ReloadPins and saves).
    struct PinRequests {
        // New order of the pinned section as item keys. A key that is not pinned yet (a running
        // item dragged into the pinned section) is pinned at that position.
        std::function<void(const std::vector<AppKey>& order)> reorder;
        // Remove every pin of this app (an icon dragged off the dock).
        std::function<void(const AppKey& key)> unpin;
        // Pin .lnk / .exe files (dropped on the dock) before config->pinned[at] (at == size: append).
        std::function<void(const std::vector<std::wstring>& paths, size_t at)> pinFiles;
    };

    DockWindow() = default;
    DockWindow(const DockWindow&) = delete;
    DockWindow& operator=(const DockWindow&) = delete;
    ~DockWindow();

    // Creates the window, devices and visuals and shows the dock. Returns a failure HRESULT
    // if the composition stack can't be created.
    HRESULT Create(HINSTANCE instance, const Config* config, MessageHook hook);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    void SetPinRequests(PinRequests requests) { pinRequests_ = std::move(requests); }
    // An icon is being dragged (or fading out after being dragged off the dock).
    bool InDrag() const { return dragPhase_ != DragPhase::None; }

    // Recomputes the layout, moves/resizes the window and redraws everything.
    void Relayout();

    // Re-reads every setting from the config (sizes, theme, clock, monitor, auto-hide) and
    // redraws. Pins are separate: ReloadPins().
    void ApplyConfig();
    // Applies config->autoHide: off shows the dock and keeps it shown; always / intelligent hide
    // it once the mouse is away (intelligent does not test window overlap yet).
    void ApplyAutoHide();
    // While held (a menu is open, a drag is in progress) the dock never hides. Releasing the
    // hold starts the hide delay if the mouse is outside the dock.
    void SetHold(bool hold);

    // The item under a client-area point (relative to the current window rect), or nullptr.
    const DockItem* ItemAt(POINT clientPt) const;
    // Index of the item under a client-area point (relative to the current window rect), or -1.
    int IndexAt(POINT clientPt) const { return HitTest(ToStage(clientPt)); }
    // Index of the item owning a tracked top-level window, or -1.
    int IndexOfWindow(HWND hwnd) const;
    const std::vector<DockItem>& Items() const { return items_; }

    // Re-resolves config->pinned (cached per entry) and rebuilds the item list. Call after
    // changing the pinned list.
    void ReloadPins();
    // Resolved pins, parallel to config->pinned.
    const std::vector<AppIdentity::ResolvedPin>& Pins() const { return pins_; }

    // Starts the item's app (newInstance: even if it already has windows) and bounces its icon
    // until a new window appears. Returns false if the launch failed.
    bool LaunchItem(int index, bool newInstance);

private:
    class DropTarget;
    friend class DropTarget;

    // All values in physical pixels. Rects are relative to the "stage" (layout.window) unless
    // noted. The stage is the full area the dock can draw into while shown (bar, magnified icons,
    // label, bar growth); the HWND covers all of it only while the mouse is on the dock (see
    // DesiredRect). Visuals are positioned in stage coordinates; the root visual's transform
    // maps them into the HWND's client area.
    struct Layout {
        float scale = 1.0f;
        RECT monitor{};        // screen coordinates
        RECT window{};         // screen coordinates: the stage
        D2D1_RECT_F bar{};     // drawn background at rest
        D2D1_RECT_F clock{};   // clock text block at rest
        float cornerRadius = 0.0f;
        float iconPx = 0.0f;   // unmagnified icon size
        UINT bitmapPx = 0;     // icon bitmap size (iconPx * magnification, for crisp magnification)
        float magnification = 1.0f;
        float padding = 0.0f;
        float gap = 0.0f;      // between icons
        float itemsLeft = 0.0f;  // left edge of slot 0 (a slot is iconPx + gap wide)
        float itemTop = 0.0f;    // top of the icon box
        float itemHeight = 0.0f; // icon top -> bar bottom
        float magTop = 0.0f;     // top of a fully magnified icon
        float dotTop = 0.0f;     // top of the dot layers
        UINT dotHeight = 0;
        float dotCenterY = 0.0f; // dot-layer-local
        float dotRadius = 0.0f;
        float separatorX = -1.0f; // bar-local; < 0 when there is no separator
        float growMax = 0.0f;     // most the bar can grow while magnified
        float bgSurfaceW = 0.0f;  // background surface width (bar width + growMax)
        float seamX = 0.0f;       // where the two background halves meet (fixed)
        float slide = 0.0f;       // auto-hide slide distance
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    HMONITOR PickMonitor() const;
    void ComputeLayout();
    // The HWND rect (screen) for the current state: the 1 px strip while Hidden, the whole stage
    // while expanded_ (mouse on the dock), the bar column down to the screen edge while sliding,
    // and just the drawn bar while Shown at rest.
    RECT DesiredRect() const;
    // Places the HWND at DesiredRect() and re-asserts topmost (layout passes). The caller commits.
    void ApplyWindowPos();
    // Resizes the HWND to DesiredRect() if it differs, committing the matching root translation
    // first (no z-order change, no activation). Re-arms mouse-leave tracking.
    void UpdateWindowRect();
    // Sets the root visual's transform so stage coordinates land at the right screen place in
    // the HWND at winRect_.
    void ApplyStageTransform();
    // Client point of the HWND at winRect_ -> stage point.
    POINT ToStage(POINT clientPt) const {
        return POINT{clientPt.x + winRect_.left - layout_.window.left, clientPt.y + winRect_.top - layout_.window.top};
    }
    // Something still draws outside the bar (magnification, label, return-to-rest, bounce, menu).
    bool NeedsStage() const;
    // Shrinks the HWND back to the bar once nothing needs the stage.
    void TryCompact();
    // Layout + window placement (handles DPI changes caused by moving monitors).
    void LayoutPass();
    // Layout after the item set changed: redraws only what changed.
    void RelayoutItems();

    HRESULT CreateLayers();
    HRESULT CreateItemVisuals(DockItem& item);
    void ResetLayers();
    HRESULT RenderBackground();
    HRESULT RenderClock();
    HRESULT RenderItem(DockItem& item);
    HRESULT RenderDotSources();
    HRESULT RenderDot(DockItem& item);
    HRESULT RenderLabel();
    // Redraws every layer (all) or only dirty ones, and positions everything.
    HRESULT DrawFrame(bool all);
    void RenderAll();
    void OnClockTick();
    // Runs `step`, then commits. On device loss, rebuilds devices and redraws everything.
    void RenderWithRecovery(const std::function<HRESULT()>& step);
    bool RecoverDevice();

    // --- placement / magnification ---
    // Computes every target position (items, background halves, clock, label) from the rest
    // layout and, while magnifying, the cursor x.
    void ComputeTargets();
    // Drives all placement tweens to their targets (duration 0: static snap).
    void ApplyTargets(double duration);
    void ResetPlacementTweens();
    void ScheduleMagnify();
    void ApplyMagnify();
    void SetBigIcons(bool big);
    // The mouse left the dock (window or active zone): return to rest, hide the label, start
    // the hide delay.
    void LeaveZone();
    bool InZone(POINT stagePt) const;
    bool CursorInZone() const;

    // --- hover label ---
    void SetHover(int index);
    void OnLabelTimer();
    void HideLabel();

    // --- thumbnail popup (multi-window items) ---
    // The hovered item can show the popup now (previews on, 2+ windows, dock shown, not held).
    bool PreviewEligible(int index) const;
    // Hover changed (called by SetHover): arm the delay, switch the popup's item, or dismiss.
    void UpdatePreviewHover();
    void OnPreviewTimer();
    void OnPreviewLeaveTimer();
    void StartPreviewLeave();
    void CancelPreviewLeave();
    // Shows the popup for items_[index] (or switches it to that item).
    void ShowPreview(int index, bool keepAnchor = false);
    // Hides the popup and releases its hold on the dock. With `leaveCheck`, a cursor outside the
    // dock then counts as having left it (return to rest, hide delay).
    void DismissPreview(bool animate, bool leaveCheck = true);
    // Dismisses the popup because of a click / menu on the dock and suppresses it for the
    // hovered item (see previewSuppressed_).
    void SuppressPreview();
    // The item list changed: follow the popup's app (its windows) or dismiss it.
    void RefreshPreview();
    void OnPreviewHover(bool inside);
    bool Held() const { return hold_ || previewHold_; }

    // --- acrylic backdrop ---
    // Creates / destroys the backdrop window per config and system settings (call before a
    // relayout: the bar's tint and corner radius depend on it).
    void UpdateAcrylic();
    // Moves the backdrop window to where the bar is drawn right now (following the bar-edge and
    // slide tweens), hides it while the dock is hidden, and keeps a ~60 Hz one-shot timer chain
    // running only while one of those tweens is still animating.
    void SyncBackdrop();

    // --- auto-hide ---
    void ScheduleHide();
    void CancelHide();
    void OnHideTimer();
    void OnRevealTimer();
    void OnSlideTimer();
    void StartHide();
    void StartShow();
    // Drives the root visual's slide offset / opacity to the pose of hideState_.
    // Returns the duration actually used (shorter for a partial reversal).
    double ApplyRootPose(double duration);
    bool CursorOnStrip() const;
    void ArmMouseTracking();

    void ResolvePins();
    // Rebuilds items_: pins in config order, then running apps in first-seen order.
    void Rebuild(const WindowTracker::Snapshot& snap);

    // Launch bounce (DirectComposition OffsetY animation; no timers while idle).
    void StartBounce(DockItem& item, size_t windowsBeforeLaunch);
    void FinishBounce(DockItem& item);
    void OnBounceTimer();
    void ScheduleBounceTimer();
    void UpdateIconUsage();
    int HitTest(POINT stagePt) const;
    LRESULT QueryItemPoint(WPARAM wp, LPARAM lp) const;
    LRESULT QueryState(WPARAM wp, LPARAM lp) const;

    // --- drag to reorder (Goal: macOS-style icon dragging) ---
    // Press on an item and move past the system drag threshold: the icon follows the cursor,
    // the others open a gap at the insertion slot. Release commits the order (or, far above the
    // bar, unpins a pinned app with no windows); Escape or losing the capture cancels.
    enum class DragPhase { None, Dragging, Removing };
    void BeginDrag(POINT stagePt);
    void UpdateDrag(POINT stagePt);
    void EndDrag(bool commit);
    // Abandons a drag without animation (the item list changed under it).
    void AbortDrag();
    void OnRemoveTimer();
    // Insertion slot for an internal drag at stage x (see DockWindow.cpp).
    int DragInsertIndex(float stageX) const;
    // Gap slot (0..pinned count) for an external drop at stage x.
    int DropGapIndex(float stageX) const;
    bool DragInRemoveZone(POINT stagePt) const;
    bool DragRemovable() const;
    size_t PinnedCount() const;
    // The config->pinned index of the item at `itemIndex` (config->pinned.size() past the pins).
    size_t ConfigIndexOfItem(size_t itemIndex) const;
    void SetItemOpacity(DockItem& item, float to, double duration);
    // Keeps the HWND at stage size until an animation of `seconds` has ended.
    void KeepStage(double seconds);

    // --- external file drops (OLE IDropTarget on the dock HWND) ---
    HRESULT RenderMarker();
    DWORD OnDragEnter(IDataObject* data, POINT screenPt, DWORD allowed);
    DWORD OnDragOver(POINT screenPt, DWORD allowed);
    void OnDragLeave();
    DWORD OnDrop(IDataObject* data, POINT screenPt, DWORD allowed);
    void SetDropGap(int gap);
    static std::vector<std::wstring> PinnableFiles(IDataObject* data);

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Config* config_ = nullptr;
    MessageHook hook_;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    bool inLayout_ = false;
    bool clockDirty_ = true;
    bool snapPending_ = true;
    UINT msgQueryItem_ = 0;
    UINT msgQueryState_ = 0;
    RECT winRect_{};          // where the HWND currently is (screen)
    bool expanded_ = false;   // the HWND covers the whole stage (mouse on the dock)

    Renderer renderer_;
    Clock clock_;
    // Background: one surface (bar width + growMax wide) shown by two visuals, each inside a
    // container clipped at a fixed seam: the left half moves with the bar's left edge and the
    // right half with its right edge, so the bar grows/shrinks without redrawing.
    Layer bgLeftClip_;
    Layer bgLeft_;
    Layer bgRightClip_;
    Layer bgRight_;
    Layer clockLayer_;
    Layer itemsGroup_;
    // The three indicator-dot looks (inactive, active, attention), drawn once and shared by every
    // item's dot visual (each DComp surface has a sizeable fixed cost). The source visuals are hidden.
    Layer dotSources_[3];
    Layer label_;
    Layout layout_;
    D2D1_SIZE_U clockSize_{};
    Tween bgLeftX_, bgRightX_, clockX_, labelX_, labelY_;
    float bgLeftTarget_ = 0.0f, bgRightTarget_ = 0.0f, clockTarget_ = 0.0f;
    float labelXTarget_ = 0.0f, labelYTarget_ = 0.0f;

    WindowTracker tracker_;
    IconCache icons_;
    std::vector<DockItem> items_;
    bool evictPending_ = false;
    std::vector<AppIdentity::ResolvedPin> pins_;                       // parallel to config_->pinned
    std::unordered_map<std::wstring, AppIdentity::ResolvedPin> pinCache_;  // "p:"+path / "a:"+aumid

    int hovered_ = -1;
    int pressed_ = -1;
    int pressedMiddle_ = -1;
    bool trackingMouse_ = false;

    // Magnification
    bool magnifying_ = false;
    float cursorX_ = 0.0f;        // client x of the last mouse position in the zone
    double lastMagnify_ = 0.0;    // Renderer::Now() of the last applied update
    bool magnifyPending_ = false;
    bool restPending_ = false;
    bool bigIcons_ = false;       // magnified-size icon surfaces are the visible ones

    // Hover label
    bool labelShown_ = false;
    bool labelDirty_ = true;
    bool labelPending_ = false;
    int labelItem_ = -1;          // item the label surface was drawn for
    float labelScale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> labelFormat_;
    winrt::com_ptr<IDWriteInlineObject> labelEllipsis_;

    // Acrylic backdrop
    Backdrop backdrop_;
    bool acrylic_ = false;            // the backdrop window exists and the bar uses the acrylic tint
    float acrylicRadiusDip_ = 8.0f;   // corner radius DWM rounds the blur with
    bool backdropPending_ = false;

    // Auto-hide
    HideState hideState_ = HideState::Shown;
    Tween rootY_, rootOpacity_;
    bool hold_ = false;
    bool hidePending_ = false;
    bool revealPending_ = false;
    bool revealArmed_ = true;     // false until the cursor leaves the strip (no reveal loops)

    PinRequests pinRequests_;

    // Drag to reorder
    POINT pressPt_{};             // stage point of the last left press on an item
    DragPhase dragPhase_ = DragPhase::None;
    int dragSource_ = -1;         // item index of the dragged icon
    AppKey dragKey_;
    float dragGrabDx_ = 0.0f;     // cursor x - icon box left edge when the drag started
    int dragInsert_ = -1;         // final item index of the dragged icon if dropped now
    bool dragRemove_ = false;     // released now, the pin would be removed
    bool dragLanding_ = false;    // targets for the dropped icon landing in its slot
    bool dragPending_ = false;    // kDragTimerId running
    bool removePending_ = false;  // kRemoveTimerId running
    AppKey removeKey_;

    // Thumbnail popup
    PreviewPopup preview_;
    bool previewPending_ = false;       // kPreviewTimerId running
    bool previewLeavePending_ = false;  // kPreviewLeaveTimerId running
    bool previewHold_ = false;          // the popup keeps the dock shown
    int previewItem_ = -1;              // item the popup shows
    AppKey previewKey_;
    int previewAnchorX_ = 0;            // screen x the popup is centred on
    // Dismissed by a click or a menu while the mouse stays on the item: no popup for that item
    // again until the mouse hovers another item or leaves the dock.
    bool previewSuppressed_ = false;
    AppKey previewSuppressKey_;

    // External drops
    DropTarget* dropTarget_ = nullptr;     // the registered IDropTarget (one reference held)
    bool dropActive_ = false;     // a drag with pinnable files is over the dock
    bool dropAccept_ = false;     // the current drag carries pinnable files
    int dropGap_ = -1;            // slot the gap is open at (-1: none)
    Layer marker_;                // insertion marker
    Tween markerX_;
};
