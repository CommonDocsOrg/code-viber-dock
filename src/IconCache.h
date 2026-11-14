#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>
#include <shtypes.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "DockItems.h"

// App icons as D2D bitmaps. Round 25 phase 25C: an entry per (AppKey, pixel size, icon file, icon
// index), so different requests for one app coexist. Until 25C the map held one entry per AppKey
// and a request with another size / file / index replaced it: two group members resolving to the
// same app with different icon files freed the bitmap of the first while RenderItem still held its
// raw pointer, and the notification centre asking at its own small size evicted the dock's entry
// (the next redraw extracted the icon and computed its dominant colour again). Loaded once per entry, from
// the cheapest source that works: the explicit icon file (an image via WIC, else an icon resource
// via SHDefExtractIcon), the package logo (AUMIDs), the exe's icon resource, the system image
// list (folders and the Recycle Bin), IShellItemImageFactory, and last the window's own icon
// (WM_GETICON / GCLP_HICON). Failures are cached too (Get returns nullptr and the caller draws a
// placeholder).
// Bitmaps are device resources: call Clear() after the D2D device is recreated.
class IconCache {
public:
    static constexpr ULONGLONG kEvictAfterMs = 60'000;

    IconCache() = default;
    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;

    // `dc` is any device context of the current D2D device (used to create the bitmap).
    // `iconFile` / `iconIndex`: an explicit icon tried first (a custom icon, a shortcut's icon
    // location): .png .jpg .jpeg .bmp .gif are decoded with WIC, anything else (.ico .exe .dll)
    // is read as an icon resource. A cached entry is used only when pixelSize, iconFile and
    // iconIndex all match; otherwise the icon is loaded into an entry of its own.
    // `transient`: a request from outside the dock's slots (the notification centre's rows): such an
    // entry is never in use, so it is dropped kEvictAfterMs after its last Get; a dock request for
    // the same exact entry makes it an ordinary one. A dock (non-transient) request that loads a new
    // entry drops the app's other dock entries with the same icon file / index at another pixel size
    // (the icon size changed: they are stale; transient ones stay).
    // The pointer stays valid until the entry is dropped (Evict, EvictExpired, Clear, or such a
    // size change of the same icon), never because of a Get at the same size: within one render
    // pass a raw pointer is safe; keep a com_ptr (copy_from) to hold it longer.
    ID2D1Bitmap* Get(const AppKey& key, HWND sampleWindow, UINT pixelSize, ID2D1DeviceContext* dc,
                     const std::wstring& iconFile = {}, int iconIndex = 0, bool transient = false);

    // Saturation- and alpha-weighted average colour (0xFFRRGGBB) of that exact entry's icon,
    // computed once per load. 0 when unknown (not loaded, no icon, or nothing opaque).
    // Round 25 phase 25C: by the full entry key (it was by AppKey alone, the one entry there was).
    // Round 26 phase 26C: computed on the first call for the entry, not in Get: it is a GPU read-back
    // (a CPU-readable copy, a map that waits for the GPU) that only the glow and the Windows 7 hot
    // tile use, and it ran for every icon load, also at every new size, with neither on. `dc`: the
    // context the entry was loaded with (the dock's resource context).
    uint32_t DominantColor(const AppKey& key, UINT pixelSize, const std::wstring& iconFile, int iconIndex,
                           ID2D1DeviceContext* dc) const;

    // HICON -> D2D bitmap (the icon is not destroyed). Null on failure.
    winrt::com_ptr<ID2D1Bitmap> FromHicon(HICON icon, ID2D1DeviceContext* dc);

    // Forgets every entry of the app (the next Get loads it again, e.g. the Recycle Bin after it
    // filled or was emptied).
    void Evict(const AppKey& key);

    // What the dock shows right now: an app with the icon file / index a slot (or a group member)
    // asks for. The dock's entries of anything else start their eviction countdown.
    struct Use {
        AppKey key;
        std::wstring iconFile;
        int iconIndex = 0;
    };
    // Marks the (non-transient) entries matching `uses` (any pixel size: a stale size is dropped on
    // the next load, see Get) in use; the others start their eviction countdown.
    void SetInUse(const std::vector<Use>& uses);
    // Drops entries unused (and not asked for) for at least kEvictAfterMs. Returns true if unused
    // entries remain (transient ones included).
    bool EvictExpired();
    bool HasUnused() const;

    void Clear();
    // Clear() plus the WIC factory (call before COM goes away).
    void Shutdown() {
        Clear();
        wic_ = nullptr;
    }

private:
    struct EntryKey {
        AppKey app;
        UINT pixelSize = 0;
        std::wstring iconFile;  // the explicit icon the entry was loaded with
        int iconIndex = 0;
        bool operator==(const EntryKey& o) const {
            return pixelSize == o.pixelSize && iconIndex == o.iconIndex && app == o.app && iconFile == o.iconFile;
        }
    };
    struct EntryKeyHash {
        size_t operator()(const EntryKey& k) const noexcept {
            size_t h = AppKeyHash{}(k.app);
            h ^= std::hash<std::wstring>{}(k.iconFile) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            h ^= (static_cast<size_t>(k.pixelSize) << 32 | static_cast<uint32_t>(k.iconIndex)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            return h;
        }
    };
    struct Entry {
        winrt::com_ptr<ID2D1Bitmap> bitmap;  // null: no icon could be loaded (placeholder)
        mutable uint32_t dominant = 0;       // DominantColor (0: unknown)
        mutable bool dominantDone = false;   // round 26 phase 26C: computed (lazily, DominantColor)
        bool inUse = true;
        bool transient = false;              // Get(..., transient): never in use
        ULONGLONG unusedSince = 0;
        ULONGLONG lastUsed = 0;              // the last Get (the countdown runs from the later of the two)
    };

    winrt::com_ptr<ID2D1Bitmap> LoadPackageLogo(const AppKey& key, UINT px, ID2D1DeviceContext* dc);
    // Image files via WIC (scaled down to px), other files via SHDefExtractIconW.
    winrt::com_ptr<ID2D1Bitmap> LoadFileIcon(const std::wstring& file, int index, UINT px, ID2D1DeviceContext* dc);
    winrt::com_ptr<ID2D1Bitmap> LoadImageFile(const std::wstring& file, UINT px, ID2D1DeviceContext* dc);
    // The system image list's icon for a folder path or a shell item (`pidl`, e.g. the Recycle
    // Bin, whose icon shows empty / full). Honours desktop.ini icons.
    winrt::com_ptr<ID2D1Bitmap> LoadFolderIcon(const wchar_t* path, PCIDLIST_ABSOLUTE pidl, UINT px,
                                               ID2D1DeviceContext* dc);
    winrt::com_ptr<ID2D1Bitmap> LoadRecycleBinIcon(UINT px, ID2D1DeviceContext* dc);
    winrt::com_ptr<ID2D1Bitmap> LoadShellImage(const AppKey& key, UINT px, ID2D1DeviceContext* dc);
    winrt::com_ptr<ID2D1Bitmap> LoadWindowIcon(HWND hwnd, ID2D1DeviceContext* dc);
    winrt::com_ptr<ID2D1Bitmap> FromWic(IWICBitmapSource* source, ID2D1DeviceContext* dc);
    // FromWic, scaled down (aspect kept) when the source is larger than px in either dimension.
    winrt::com_ptr<ID2D1Bitmap> FromWicScaled(IWICBitmapSource* source, UINT px, ID2D1DeviceContext* dc);
    static uint32_t ComputeDominantColor(ID2D1Bitmap* bitmap, ID2D1DeviceContext* dc);
    IWICImagingFactory* Wic();

    // Node-based: an entry's bitmap does not move when others are added (rehash).
    std::unordered_map<EntryKey, Entry, EntryKeyHash> entries_;
    winrt::com_ptr<IWICImagingFactory> wic_;
};
