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

// App icons as D2D bitmaps, keyed by AppKey. Loaded once per key, pixel size and icon file, from
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
    // iconIndex all match; otherwise the icon is loaded again.
    ID2D1Bitmap* Get(const AppKey& key, HWND sampleWindow, UINT pixelSize, ID2D1DeviceContext* dc,
                     const std::wstring& iconFile = {}, int iconIndex = 0);

    // Saturation- and alpha-weighted average colour (0xFFRRGGBB) of the key's cached icon,
    // computed once per load. 0 when unknown (not loaded, no icon, or nothing opaque).
    uint32_t DominantColor(const AppKey& key) const;

    // HICON -> D2D bitmap (the icon is not destroyed). Null on failure.
    winrt::com_ptr<ID2D1Bitmap> FromHicon(HICON icon, ID2D1DeviceContext* dc);

    // Forgets the key's icon (the next Get loads it again, e.g. the Recycle Bin after it filled
    // or was emptied).
    void Evict(const AppKey& key) { entries_.erase(key); }

    // Marks which keys are on the dock right now; the others start their eviction countdown.
    void SetInUse(const std::vector<AppKey>& keys);
    // Drops entries unused for at least kEvictAfterMs. Returns true if unused entries remain.
    bool EvictExpired();
    bool HasUnused() const;

    void Clear();
    // Clear() plus the WIC factory (call before COM goes away).
    void Shutdown() {
        Clear();
        wic_ = nullptr;
    }

private:
    struct Entry {
        winrt::com_ptr<ID2D1Bitmap> bitmap;  // null: no icon could be loaded (placeholder)
        UINT pixelSize = 0;
        std::wstring iconFile;               // the explicit icon this entry was loaded with
        int iconIndex = 0;
        uint32_t dominant = 0;               // DominantColor (0: unknown)
        bool inUse = true;
        ULONGLONG unusedSince = 0;
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

    std::unordered_map<AppKey, Entry, AppKeyHash> entries_;
    winrt::com_ptr<IWICImagingFactory> wic_;
};
