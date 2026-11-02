#pragma once

#include <windows.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

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

// One dock entry: a pinned app (Phase 4) and/or a running app with its windows.
// Dock order = pinned items in config order, then running apps in first-seen order.
struct DockItem {
    // App: a pinned and / or running app. The other kinds are pins only and never get windows:
    // Folder (a directory, opened in Explorer), Separator (a spacer between items), RecycleBin.
    enum class Kind { App, Folder, Separator, RecycleBin };

    AppKey key;
    Kind kind = Kind::App;
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
    bool wantsAttention = false;             // a window flashed (HSHELL_FLASH) and is not active

    // --- render state ---
    // `layer` is a content-less container carrying the placement transform (magnification
    // scale about the icon's bottom centre + x/y translation) and the bounce OffsetY. Its
    // children are the icon drawn at rest size (crisp at rest) and, when magnification is on,
    // the icon drawn at the magnified size (crisp while magnified); one of them is visible.
    // The running-indicator dot is a separate, unscaled layer that follows the icon's x.
    Layer layer;
    Layer icon;
    Layer iconBig;
    Layer dot;
    bool active = false;    // owns the foreground window
    bool dirty = true;      // icon surfaces must be redrawn
    bool dotDirty = true;   // dot content must be re-assigned
    float x = 0.0f;         // rest position of the icon box (client pixels)
    float y = 0.0f;
    // Rest slot (set by the layout): left edge (stage px) and width. An app / folder / bin slot is
    // iconPx + gap with the icon box centred in it; a separator's slot is narrow and its box (the
    // hairline surface) fills it.
    float slotX = 0.0f;
    float slotW = 0.0f;
    float targetX = 0.0f;   // current target of the icon box's left edge (magnified layout)
    float targetScale = 1.0f;
    Tween tx;               // translate.OffsetX (and the dot's OffsetX)
    Tween ts;               // scale.ScaleX/Y
    Tween fade;             // layer opacity (dragging: dimmed; dragged off the dock: fading out)

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
