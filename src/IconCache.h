#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "DockItems.h"

// App icons as D2D bitmaps, keyed by AppKey. Loaded once per key and pixel size:
//   IShellItemImageFactory (exe path or shell:AppsFolder\<AUMID>) -> HBITMAP -> WIC -> ID2D1Bitmap,
//   falling back to the window's icon (WM_GETICON / GCLP_HICON). Failures are cached too
//   (Get returns nullptr and the caller draws a placeholder).
// Bitmaps are device resources: call Clear() after the D2D device is recreated.
class IconCache {
public:
    static constexpr ULONGLONG kEvictAfterMs = 60'000;

    IconCache() = default;
    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;

    // `dc` is any device context of the current D2D device (used to create the bitmap).
    // `iconFile` / `iconIndex`: an explicit icon resource tried first (a shortcut's icon location).
    ID2D1Bitmap* Get(const AppKey& key, HWND sampleWindow, UINT pixelSize, ID2D1DeviceContext* dc,
                     const std::wstring& iconFile = {}, int iconIndex = 0);

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
        bool inUse = true;
        ULONGLONG unusedSince = 0;
    };

    winrt::com_ptr<ID2D1Bitmap> LoadPackageLogo(const AppKey& key, UINT px, ID2D1DeviceContext* dc);
    winrt::com_ptr<ID2D1Bitmap> LoadFileIcon(const std::wstring& file, int index, UINT px, ID2D1DeviceContext* dc);
    winrt::com_ptr<ID2D1Bitmap> LoadShellImage(const AppKey& key, UINT px, ID2D1DeviceContext* dc);
    winrt::com_ptr<ID2D1Bitmap> LoadWindowIcon(HWND hwnd, ID2D1DeviceContext* dc);
    winrt::com_ptr<ID2D1Bitmap> FromWic(IWICBitmapSource* source, ID2D1DeviceContext* dc);
    IWICImagingFactory* Wic();

    std::unordered_map<AppKey, Entry, AppKeyHash> entries_;
    winrt::com_ptr<IWICImagingFactory> wic_;
};
