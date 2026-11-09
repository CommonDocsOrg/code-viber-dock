#pragma once

#include <windows.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <d2d1.h>

#include "Renderer.h"

// Identity of an application. `id` is normalized (lowercase): an AppUserModelID when
// `isAumid`, otherwise a full exe path (a folder path for folder pins), "class:<window class>"
// when the process could not be queried, or a synthetic "kind:..." id for the special pins
// (separators, the Recycle Bin). Equality and hashing use only (id, isAumid).
struct AppKey {
    std::wstring id;
    bool isAumid = false;
    // The id as reported by the system (original case), used for shell parsing names.
    // Not part of the identity.
    std::wstring original;

    bool operator==(const AppKey& o) const { return isAumid == o.isAumid && id == o.id; }
    bool operator!=(const AppKey& o) const { return !(*this == o); }

    bool IsClassFallback() const { return !isAumid && id.rfind(L"class:", 0) == 0; }
    // "kind:separator#<n>" / "kind:recyclebin": no file behind the id.
    bool IsSynthetic() const { return !isAumid && id.rfind(L"kind:", 0) == 0; }
    // The string to use for parsing names / display: original case when known.
    const std::wstring& Source() const { return original.empty() ? id : original; }
};

struct AppKeyHash {
    size_t operator()(const AppKey& k) const noexcept {
        return std::hash<std::wstring>{}(k.id) ^ (k.isAumid ? 0x9e3779b97f4a7c15ull : 0ull);
    }
};

// One slot of the dock's icon row: a pinned app and/or a running app with its windows, a folder,
// a separator, the Recycle Bin, or a widget (the clock, the weather). Slots come from the dock's
// elements (DockElement.h) in config order (docks[].items[]); the `running` element contributes
// one slot per unpinned running app.
struct DockItem {
    // App: a pinned and / or running app. Folder (a directory, opened in Explorer), Separator (a
    // spacer between items) and RecycleBin are pins only and never get windows. Clock and Weather
    // are widget slots: their content is drawn by their element, they never magnify, are never
    // dragged and have no name label (QueryState 24 reports the kind as its number).
    // Widget: a script widget (docs/widgets.md), drawn by its element like the clock.
    // Group: a shortcut group (one slot; its members open in a popup, the icon shows up to four
    // of theirs unless the item has a custom icon).
    // Tray: the notify icons as a grid (a widget slot, drawn by its element; TrayHost). TrayApp: one
    // notify icon pinned as a full-size, magnifying icon slot (round 4 phase 8b).
    // Notifications: the notification centre's bell (round 4 phase 9b; a widget slot).
    enum class Kind { App, Folder, Separator, RecycleBin, Clock, Weather, Widget, Group, Tray, TrayApp, Notifications };

    AppKey key;
    Kind kind = Kind::App;
    size_t element = 0;  // index of the element that contributes this slot (DockWindow::elements_)

    // An icon that magnifies (apps, folders, the Recycle Bin).
    bool Magnifies() const {
        return kind == Kind::App || kind == Kind::Folder || kind == Kind::RecycleBin || kind == Kind::Group ||
               kind == Kind::TrayApp;
    }
    // A widget slot (content drawn by its element; not dragged, no label).
    bool IsWidget() const {
        return kind == Kind::Clock || kind == Kind::Weather || kind == Kind::Widget || kind == Kind::Tray ||
               kind == Kind::Notifications;
    }
    std::wstring displayName;
    std::optional<std::wstring> launchPath;  // .lnk/.exe for pinned entries (keeps args / working dir)
    std::wstring workingDir;                 // launch directory (the link's, else the target's folder)
    std::wstring targetPath;                 // resolved file for "Open file location" (empty: none)
    std::wstring iconFile;                   // explicit icon resource (a link's icon location), optional
    int iconIndex = 0;
    bool customIcon = false;                 // iconFile comes from the pin's "icon" (config.json)
    bool missing = false;                    // pinned file no longer exists: placeholder icon
    bool pinned = false;
    std::vector<HWND> windows;               // running windows, first-seen order
    // A group: its members (what the composed icon shows) and whether any of them runs (its dot;
    // `active` / `wantsAttention` then follow the members' windows). A group never has `windows`.
    struct Member {
        AppKey key;
        std::wstring displayName;
        std::wstring iconFile;
        int iconIndex = 0;
        bool missing = false;
        bool operator==(const Member&) const = default;
    };
    std::vector<Member> members;
    bool groupRunning = false;
    bool wantsAttention = false;             // a window flashed (HSHELL_FLASH) and is not active
    // A window slot (docks[].windows "unstacked"): the one window this slot shows (`windows` is
    // {window} while it exists); null for every other slot. Slots are matched by key + window.
    HWND window = nullptr;
    bool minimized = false;                  // a window slot's window is minimized (no dot)
    std::wstring windowTitle;                // a window slot's caption (hover label, title under the icon)
    // A thumbnail tile (docks[].thumbnails): the window shown in it (a window slot's window, or a
    // stacked app's active window); null: an ordinary icon. A tile is boxMain (thumbnailSize) long.
    HWND thumbWindow = nullptr;
    bool thumbDim = false;  // that window is minimized: its last frame, dimmed
    float boxMain = 0.0f;   // the box along the bar (icons: iconPx; tiles: the tile's length); set by the layout
    D2D1_POINT_2F dotShift{};  // the dot's static offset (physical; centred under a longer tile)
    // The text under the icon (docks[].titles; empty: none) as last drawn into `caption`.
    std::wstring captionText;
    bool captionDirty = true;

    // --- render state ---
    // `layer` is a content-less container carrying the placement transform (magnification
    // scale about the icon's bottom centre + x/y translation) and the bounce OffsetY. Its
    // children are the icon drawn at rest size (crisp at rest) and, when magnification is on,
    // the icon drawn at the magnified size (crisp while magnified); one of them is visible.
    // The running-indicator dot is a separate, unscaled layer that follows the icon's x.
    // Icon effects (theme glow / reflection) are two more children, drawn at the magnified bitmap
    // size with a static scale down to the rest size (like iconBig), so magnification, bounces and
    // drags never redraw them: `glow` behind the icon (its opacity driven by glowOpacity) and
    // `reflection` under the icon's foot. Both have no surface while their effect is off, and
    // separators never get them. Z-order in `layer`: glow, reflection, icon, iconBig (and `button`,
    // round 11 phase 11A, behind `layer` in the items group).
    // A widget slot has only `layer`: one plain visual (no placement transform, no children) that
    // shows its element's content and is moved with its own offsets.
    Layer layer;
    Layer glow;
    Layer reflection;
    Layer icon;
    Layer iconBig;
    Layer dot;
    // The title under the icon (docks[].titles): like the dot a separate, unscaled layer in front
    // of the icon container that follows the icon box along the bar (the same tween drives both);
    // it never scales with magnification, so the text stays crisp and is never redrawn while the
    // dock magnifies. No visual while titles are off.
    Layer caption;
    // Round 11 phase 11A, slot buttons (theme.slotStyle "buttons"): the button frame behind the
    // icon container (z-order: button, then `layer`), unscaled, following the icon box along the bar
    // like the dot (the same tween; buttonShift is its static offset from the box to the frame's
    // start). Its surface is drawn only when the state below changes (or the pointer moves inside it
    // with hot tracking); none while there is nothing to show. buttonDrawn: the QueryState 293 code
    // last drawn (-1: redraw), buttonStacked the outlines drawn, buttonHot the hot-track point drawn
    // (surface px; x < 0: none), buttonPulse: the attention pulse (a DWM opacity animation) runs.
    Layer button;
    int buttonDrawn = -1;
    int buttonStacked = 0;
    D2D1_POINT_2F buttonHot{-1.0f, -1.0f};
    float buttonShift = 0.0f;
    bool buttonPulse = false;
    // The notification badge (docks[].badges, round 4 phase 9): the unread count of the slot's app
    // (a group: the sum of its members'), drawn as a bubble by two more children of `layer` in front
    // of the icon (the rest size and the magnified size, toggled with icon / iconBig), so the
    // bubble magnifies and bounces with the icon. Created the first time a count shows.
    int badgeCount = 0;
    bool badgeDirty = false;
    Layer badge;
    Layer badgeBig;
    Tween glowOpacity;
    bool active = false;    // owns the foreground window
    bool dirty = true;      // icon surfaces must be redrawn
    bool dotDirty = true;   // dot content must be re-assigned
    float x = 0.0f;         // rest position of the icon box (client pixels)
    float y = 0.0f;
    // Rest slot (set by the layout): left edge (stage px) and width. An app / folder / bin slot is
    // iconPx + gap with the icon box centred in it; a separator's slot is narrow and its box (the
    // hairline surface) fills it; a widget's slot is its content box (Measure's main length).
    float slotX = 0.0f;
    float slotW = 0.0f;
    // Round 13 phase 13D: an icon slot's own gap (px; its element's SlotGap: the theme's iconGap, or
    // the running item's `gap`): the slot is boxMain + gapPx with floor(gapPx / 2) before the box.
    // Two neighbouring slots are therefore the mean of their gaps apart. Set by the layout.
    float gapPx = 0.0f;
    // Round 13 phase 13D: its element's RunPadding (px; the running item's `padding`), and the part
    // of `lead` that is padding (that element's before its first slot, the previous element's after
    // its last): the hairline sits in the rest of the lead.
    float runPad = 0.0f;
    float padLead = 0.0f;
    // Spacer before the slot at rest (between an icon and a widget, or two widgets; 0 between
    // icons). It never scales: magnification and drags keep it.
    float lead = 0.0f;
    float boxCross = 0.0f;  // a widget's content length on the cross axis (icons: the icon size)
    float hitSlack = 0.0f;  // a widget also takes clicks this far beyond its box on the main axis
    // Round 11 phase 11C: a flush widget on a full-width bar (ElementSize::flush): as the first /
    // last slot it takes no end padding (it touches the screen's end).
    bool flush = false;
    float appliedY = -1.0f; // a widget: its visual's OffsetY as last set (< 0: not yet)
    float targetX = 0.0f;   // current target of the icon box's left edge (magnified layout)
    float targetScale = 1.0f;
    Tween tx;               // translate.OffsetX (and the dot's OffsetX)
    Tween ts;               // scale.ScaleX/Y
    Tween fade;             // layer opacity (dragging: dimmed; dragged off the dock: fading out)
    // A scrolling dock (DockGeometry::Viewport): the edge-fade opacity set on the slot's visuals (the
    // icon container, the dot and the caption; a widget's visual): the static value, or -1 while an
    // animation drives it (built for the `tx` curve recorded in edgeFadeFor) or it is unknown.
    float edgeOpacity = 1.0f;
    Tween edgeFadeFor;
    unsigned edgeFadeSerial = 0;  // DockWindow::fadeSerial_ when it was built
    // The slot was outside the viewport at its last placement: only its tweens' bookkeeping
    // followed (no visual was touched); its visuals are re-placed before they are driven again.
    bool placementStale = false;
    // A ring with rotateIcons: the icon's turn (degrees clockwise; 0 upright) its surfaces show.
    float turn = 0.0f;

    // Launch feedback (animation.launchStyle): the icon bounces / hops / pulses until a new
    // window appears (or a timeout), then finishes the current bounce (or fades back to opaque).
    enum class Bounce { None, Bouncing, Finishing };
    Bounce bounce = Bounce::None;
    LaunchStyle bounceStyle = LaunchStyle::Bounce;  // the style it was started with
    float bounceHalf = 0.3f;       // seconds: rise (= fall) of one bounce, half a pulse
    ULONGLONG bounceStart = 0;     // GetTickCount64 when the bounce animation was committed
    ULONGLONG bounceDeadline = 0;  // Bouncing: timeout; Finishing: when the last bounce lands
    size_t bounceWindowCount = 0;  // windows when the launch started; more = launched
};
