#include "IconCache.h"

#include <appmodel.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl_core.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

#include "IconUtil.h"

namespace {

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool ReadUtf8File(const std::wstring& path, std::wstring& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::string bytes;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (4 << 20);
    if (ok) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
    }
    CloseHandle(h);
    if (!ok) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    out.assign(static_cast<size_t>(std::max(n, 0)), L' ');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
    return n > 0;
}

// Value of attribute `name` inside the tag text [begin, end) of `xml`; empty if absent.
std::wstring AttributeIn(const std::wstring& xml, size_t begin, size_t end, const std::wstring& name) {
    const std::wstring needle = L" " + name + L"=\"";
    const size_t at = xml.find(needle, begin);
    if (at == std::wstring::npos || at >= end) return {};
    const size_t v = at + needle.size();
    const size_t close = xml.find(L'"', v);
    if (close == std::wstring::npos || close > end) return {};
    return xml.substr(v, close - v);
}

// Install folder of the first package of a package family, e.g. "Microsoft.Paint_8wekyb3d8bbwe".
std::wstring PackagePathForFamily(const std::wstring& family) {
    UINT32 count = 0;
    UINT32 bufferLength = 0;
    if (GetPackagesByPackageFamily(family.c_str(), &count, nullptr, &bufferLength, nullptr) !=
            ERROR_INSUFFICIENT_BUFFER ||
        count == 0) {
        return {};
    }
    std::vector<PWSTR> names(count);
    std::vector<wchar_t> buffer(bufferLength);
    if (GetPackagesByPackageFamily(family.c_str(), &count, names.data(), &bufferLength, buffer.data()) !=
            ERROR_SUCCESS ||
        count == 0) {
        return {};
    }
    UINT32 len = 0;
    if (GetPackagePathByFullName(names[0], &len, nullptr) != ERROR_INSUFFICIENT_BUFFER) return {};
    std::wstring path(len, L' ');
    if (GetPackagePathByFullName(names[0], &len, path.data()) != ERROR_SUCCESS) return {};
    path.resize(wcsnlen(path.c_str(), path.size()));
    return path;
}

// Picks the best MRT-qualified variant of a package asset (e.g. Assets\Logo.png) for `px`:
// targetsize + altform-unplated (transparent, meant for the dark taskbar) >= px, then any
// targetsize, then the largest scale-N, then the unqualified file. High-contrast and
// light-theme variants are skipped.
std::wstring ResolveAssetFile(const std::wstring& packagePath, const std::wstring& relative, UINT px) {
    const std::wstring full = packagePath + L"\\" + relative;
    const size_t slash = full.find_last_of(L"\\/");
    const std::wstring dir = full.substr(0, slash + 1);
    const std::wstring file = full.substr(slash + 1);
    const size_t dot = file.rfind(L'.');
    const std::wstring stem = dot == std::wstring::npos ? file : file.substr(0, dot);
    const std::wstring ext = dot == std::wstring::npos ? L"" : file.substr(dot);

    std::wstring bestName;
    int bestScore = INT_MIN;
    WIN32_FIND_DATAW fd;
    const std::wstring pattern = dir + stem + L".*" + ext;
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name.size() <= stem.size()) continue;
            const std::wstring q = Lower(name.substr(stem.size()));  // ".targetsize-48_altform-unplated.png"
            if (q.find(L"contrast-") != std::wstring::npos) continue;
            if (q.find(L"lightunplated") != std::wstring::npos) continue;
            int score = 0;
            if (size_t t = q.find(L"targetsize-"); t != std::wstring::npos) {
                const int size = _wtoi(q.c_str() + t + 11);
                // Smallest size >= px wins; otherwise the largest below it.
                score = size >= static_cast<int>(px) ? 3000 - size : 1000 + size;
                if (q.find(L"altform-unplated") != std::wstring::npos) score += 5000;
            } else if (size_t sc = q.find(L"scale-"); sc != std::wstring::npos) {
                score = _wtoi(q.c_str() + sc + 6);  // larger scale = more pixels
            }
            if (score > bestScore) {
                bestScore = score;
                bestName = name;
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    if (!bestName.empty()) return dir + bestName;
    return GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES ? full : std::wstring{};
}

// The shell returns 32 bpp DIBs whose alpha channel is premultiplied. Some icons (legacy, no
// alpha) come back with alpha = 0 everywhere; those must be treated as opaque.
bool HasAnyAlpha(HBITMAP bmp) {
    DIBSECTION ds{};
    if (GetObjectW(bmp, sizeof(ds), &ds) != sizeof(ds) || ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits) {
        return true;  // can't inspect: trust WIC
    }
    const auto* px = static_cast<const BYTE*>(ds.dsBm.bmBits);
    const size_t count = static_cast<size_t>(ds.dsBm.bmWidthBytes) * static_cast<size_t>(ds.dsBm.bmHeight);
    for (size_t i = 3; i < count; i += 4) {
        if (px[i] != 0) return true;
    }
    return false;
}

bool IsDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// SHIL_JUMBO renders an icon that has no 256 px image at its largest size in the top-left corner
// of a transparent 256 x 256 canvas. True when nothing outside that corner (`edge` px) is
// visible, i.e. the smaller list should be used instead.
bool OnlyInCorner(IWICBitmap* bitmap, UINT edge) {
    UINT w = 0, h = 0;
    if (FAILED(bitmap->GetSize(&w, &h)) || w <= edge || h <= edge) return false;
    winrt::com_ptr<IWICBitmapLock> lock;
    const WICRect all{0, 0, static_cast<INT>(w), static_cast<INT>(h)};
    if (FAILED(bitmap->Lock(&all, WICBitmapLockRead, lock.put()))) return false;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data)) || !data) return false;
    if (static_cast<size_t>(stride) * h > size || stride < w * 4) return false;
    for (UINT y = 0; y < h; ++y) {
        const BYTE* row = data + static_cast<size_t>(y) * stride;
        for (UINT x = y < edge ? edge : 0; x < w; ++x) {
            if (row[static_cast<size_t>(x) * 4 + 3] != 0) return false;
        }
    }
    return true;
}

}  // namespace

IWICImagingFactory* IconCache::Wic() {
    if (!wic_) {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.put())))) {
            wic_ = nullptr;
        }
    }
    return wic_.get();
}

winrt::com_ptr<ID2D1Bitmap> IconCache::FromWic(IWICBitmapSource* source, ID2D1DeviceContext* dc) {
    IWICImagingFactory* wic = Wic();
    if (!wic || !source || !dc) return nullptr;
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.put()))) return nullptr;
    if (FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }
    winrt::com_ptr<ID2D1Bitmap1> bitmap;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc->CreateBitmapFromWicBitmap(converter.get(), &props, bitmap.put()))) return nullptr;
    return bitmap.as<ID2D1Bitmap>();
}

winrt::com_ptr<ID2D1Bitmap> IconCache::FromWicScaled(IWICBitmapSource* source, UINT px, ID2D1DeviceContext* dc) {
    IWICImagingFactory* wic = Wic();
    if (!wic || !source || px == 0) return nullptr;
    UINT w = 0, h = 0;
    if (FAILED(source->GetSize(&w, &h)) || w == 0 || h == 0) return nullptr;
    if (w <= px && h <= px) return FromWic(source, dc);
    // Premultiply first, then scale: scaling straight alpha bleeds the colour of transparent
    // pixels into the edges.
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }
    const double fit = static_cast<double>(px) / static_cast<double>(std::max(w, h));
    const UINT sw = std::max(1u, static_cast<UINT>(std::lround(w * fit)));
    const UINT sh = std::max(1u, static_cast<UINT>(std::lround(h * fit)));
    winrt::com_ptr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(converter.get(), sw, sh, WICBitmapInterpolationModeHighQualityCubic))) {
        return nullptr;
    }
    return FromWic(scaler.get(), dc);
}

winrt::com_ptr<ID2D1Bitmap> IconCache::FromHicon(HICON icon, ID2D1DeviceContext* dc) {
    IWICImagingFactory* wic = Wic();
    if (!wic || !icon || !dc) return nullptr;
    winrt::com_ptr<IWICBitmap> wicBitmap;
    if (FAILED(wic->CreateBitmapFromHICON(icon, wicBitmap.put()))) return nullptr;
    return FromWic(wicBitmap.get(), dc);
}

winrt::com_ptr<ID2D1Bitmap> IconCache::LoadImageFile(const std::wstring& file, UINT px, ID2D1DeviceContext* dc) {
    IWICImagingFactory* wic = Wic();
    if (!wic) return nullptr;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              decoder.put()))) {
        return nullptr;
    }
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return nullptr;  // a GIF: its first frame
    // Scaled to the bitmap size: a large photo would otherwise sit in video memory at full size.
    return FromWicScaled(frame.get(), px, dc);
}

winrt::com_ptr<ID2D1Bitmap> IconCache::LoadFileIcon(const std::wstring& file, int index, UINT px,
                                                    ID2D1DeviceContext* dc) {
    if (IconUtil::IsImageFile(file)) return LoadImageFile(file, px, dc);
    // Reads the icon resource straight from the exe at the requested size. Much lighter than
    // the shell namespace (IShellItemImageFactory costs ~15 MB of private bytes in caches).
    HICON icon = nullptr;
    const UINT size = std::min(px, 256u);
    if (FAILED(SHDefExtractIconW(file.c_str(), index, 0, &icon, nullptr, MAKELONG(size, 0))) || !icon) {
        return nullptr;
    }
    winrt::com_ptr<ID2D1Bitmap> result = FromHicon(icon, dc);
    DestroyIcon(icon);
    return result;
}

winrt::com_ptr<ID2D1Bitmap> IconCache::LoadFolderIcon(const wchar_t* path, PCIDLIST_ABSOLUTE pidl, UINT px,
                                                      ID2D1DeviceContext* dc) {
    IWICImagingFactory* wic = Wic();
    if (!wic || (!path && !pidl)) return nullptr;
    // The system image list index (it reflects desktop.ini icons and the Recycle Bin's
    // empty / full state). The image list is the shell's own: never destroyed.
    SHFILEINFOW sfi{};
    const DWORD_PTR ok = pidl ? SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &sfi, sizeof(sfi),
                                               SHGFI_PIDL | SHGFI_SYSICONINDEX)
                              : SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX);
    if (!ok) return nullptr;
    // 48 px (SHIL_EXTRALARGE) is enough for small icons; otherwise the 256 px list, scaled down
    // (falling back to 48 px for an icon without a 256 px image).
    const int lists[2] = {px > 48 ? SHIL_JUMBO : SHIL_EXTRALARGE, SHIL_EXTRALARGE};
    for (const int list : lists) {
        winrt::com_ptr<IImageList> images;
        if (FAILED(SHGetImageList(list, IID_PPV_ARGS(images.put())))) continue;
        HICON icon = nullptr;
        if (FAILED(images->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon)) || !icon) continue;
        winrt::com_ptr<IWICBitmap> wicBitmap;
        const HRESULT hr = wic->CreateBitmapFromHICON(icon, wicBitmap.put());
        DestroyIcon(icon);
        if (FAILED(hr)) continue;
        if (list == SHIL_JUMBO && OnlyInCorner(wicBitmap.get(), 48)) continue;
        return FromWicScaled(wicBitmap.get(), px, dc);
    }
    return nullptr;
}

winrt::com_ptr<ID2D1Bitmap> IconCache::LoadRecycleBinIcon(UINT px, ID2D1DeviceContext* dc) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, &pidl)) || !pidl) {
        return nullptr;
    }
    winrt::com_ptr<ID2D1Bitmap> result = LoadFolderIcon(nullptr, pidl, px, dc);
    CoTaskMemFree(pidl);
    return result;
}

uint32_t IconCache::ComputeDominantColor(ID2D1Bitmap* bitmap, ID2D1DeviceContext* dc) {
    // Read back through a CPU-readable copy (a bitmap that can be drawn cannot be mapped). Done
    // once per load; the copy is released at once.
    if (!bitmap || !dc) return 0;
    const D2D1_SIZE_U size = bitmap->GetPixelSize();
    if (size.width == 0 || size.height == 0) return 0;
    winrt::com_ptr<ID2D1Bitmap1> cpu;
    const D2D1_BITMAP_PROPERTIES1 props =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, &props, cpu.put()))) return 0;
    if (FAILED(cpu->CopyFromBitmap(nullptr, bitmap, nullptr))) return 0;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped)) || !mapped.bits) return 0;
    // Weight = alpha x (a little + saturation): vivid, opaque pixels decide the colour; grey
    // outlines, shadows and anti-aliased edges count for little.
    double r = 0.0, g = 0.0, b = 0.0, total = 0.0;
    for (UINT y = 0; y < size.height; ++y) {
        const BYTE* row = mapped.bits + static_cast<size_t>(y) * mapped.pitch;
        for (UINT x = 0; x < size.width; ++x) {
            const BYTE* p = row + static_cast<size_t>(x) * 4;  // B G R A, premultiplied
            const int a = p[3];
            if (a < 16) continue;
            const double pb = p[0] * 255.0 / a, pg = p[1] * 255.0 / a, pr = p[2] * 255.0 / a;
            const double hi = std::max({pr, pg, pb}), lo = std::min({pr, pg, pb});
            const double saturation = hi > 0.0 ? (hi - lo) / hi : 0.0;
            const double w = (a / 255.0) * (0.05 + saturation);
            r += pr * w;
            g += pg * w;
            b += pb * w;
            total += w;
        }
    }
    cpu->Unmap();
    if (total <= 0.0) return 0;
    auto channel = [&](double v) { return static_cast<uint32_t>(std::clamp(std::lround(v / total), 0L, 255L)); };
    return 0xFF000000u | (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

winrt::com_ptr<ID2D1Bitmap> IconCache::LoadPackageLogo(const AppKey& key, UINT px, ID2D1DeviceContext* dc) {
    // "Family_publisherhash!AppId": read the app's Square44x44Logo from the package manifest and
    // decode the best-fitting PNG with WIC. Avoids the shell namespace (~10 MB of caches).
    const std::wstring& aumid = key.Source();
    const size_t bang = aumid.find(L'!');
    if (bang == std::wstring::npos || bang == 0) return nullptr;
    const std::wstring family = aumid.substr(0, bang);
    const std::wstring appId = Lower(aumid.substr(bang + 1));

    const std::wstring packagePath = PackagePathForFamily(family);
    if (packagePath.empty()) return nullptr;
    std::wstring xml;
    if (!ReadUtf8File(packagePath + L"\\AppxManifest.xml", xml)) return nullptr;

    std::wstring logo;
    for (size_t pos = xml.find(L"<Application "); pos != std::wstring::npos && logo.empty();
         pos = xml.find(L"<Application ", pos + 1)) {
        const size_t tagEnd = xml.find(L'>', pos);
        if (tagEnd == std::wstring::npos) break;
        if (Lower(AttributeIn(xml, pos, tagEnd, L"Id")) != appId) continue;
        size_t appEnd = xml.find(L"</Application>", pos);
        if (appEnd == std::wstring::npos) appEnd = xml.size();
        const size_t ve = xml.find(L"VisualElements ", pos);
        if (ve == std::wstring::npos || ve > appEnd) break;
        const size_t veEnd = xml.find(L'>', ve);
        logo = AttributeIn(xml, ve, veEnd, L"Square44x44Logo");
        if (logo.empty()) logo = AttributeIn(xml, ve, veEnd, L"Logo");
    }
    if (logo.empty()) return nullptr;

    const std::wstring file = ResolveAssetFile(packagePath, logo, px);
    IWICImagingFactory* wic = Wic();
    if (file.empty() || !wic) return nullptr;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              decoder.put()))) {
        return nullptr;
    }
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return nullptr;
    return FromWic(frame.get(), dc);
}

winrt::com_ptr<ID2D1Bitmap> IconCache::LoadShellImage(const AppKey& key, UINT px, ID2D1DeviceContext* dc) {
    if (key.IsClassFallback()) return nullptr;
    const std::wstring parsing = key.isAumid ? L"shell:AppsFolder\\" + key.Source() : key.Source();

    winrt::com_ptr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(factory.put())))) return nullptr;

    HBITMAP hbmp = nullptr;
    const SIZE size{static_cast<LONG>(px), static_cast<LONG>(px)};
    if (FAILED(factory->GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp)) || !hbmp) return nullptr;

    winrt::com_ptr<ID2D1Bitmap> result;
    if (IWICImagingFactory* wic = Wic()) {
        const WICBitmapAlphaChannelOption alpha = HasAnyAlpha(hbmp) ? WICBitmapUsePremultipliedAlpha : WICBitmapIgnoreAlpha;
        winrt::com_ptr<IWICBitmap> wicBitmap;
        if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(hbmp, nullptr, alpha, wicBitmap.put()))) {
            result = FromWic(wicBitmap.get(), dc);
        }
    }
    DeleteObject(hbmp);
    return result;
}

winrt::com_ptr<ID2D1Bitmap> IconCache::LoadWindowIcon(HWND hwnd, ID2D1DeviceContext* dc) {
    if (!hwnd || !IsWindow(hwnd)) return nullptr;
    DWORD_PTR icon = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &icon) || !icon) {
        icon = static_cast<DWORD_PTR>(GetClassLongPtrW(hwnd, GCLP_HICON));
    }
    if (!icon) return nullptr;
    // The icon belongs to the other process / its class: don't destroy it.
    return FromHicon(reinterpret_cast<HICON>(icon), dc);
}

ID2D1Bitmap* IconCache::Get(const AppKey& key, HWND sampleWindow, UINT pixelSize, ID2D1DeviceContext* dc,
                            const std::wstring& iconFile, int iconIndex) {
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second.pixelSize == pixelSize && it->second.iconFile == iconFile &&
        it->second.iconIndex == iconIndex) {
        it->second.inUse = true;
        return it->second.bitmap.get();
    }
    if (!dc) return nullptr;

    Entry e;
    e.pixelSize = pixelSize;
    e.iconFile = iconFile;
    e.iconIndex = iconIndex;
    // Cheapest source first; the shell (exact but memory hungry) only when those fail.
    if (!iconFile.empty()) e.bitmap = LoadFileIcon(iconFile, iconIndex, pixelSize, dc);
    if (!e.bitmap) {
        if (key.IsSynthetic()) {
            if (key.id == L"kind:recyclebin") e.bitmap = LoadRecycleBinIcon(pixelSize, dc);
        } else if (key.isAumid) {
            e.bitmap = LoadPackageLogo(key, pixelSize, dc);
        } else if (!key.IsClassFallback()) {
            e.bitmap = IsDirectory(key.Source()) ? LoadFolderIcon(key.Source().c_str(), nullptr, pixelSize, dc)
                                                 : LoadFileIcon(key.Source(), 0, pixelSize, dc);
        }
    }
    if (!e.bitmap && !key.IsSynthetic()) e.bitmap = LoadShellImage(key, pixelSize, dc);
    if (!e.bitmap) e.bitmap = LoadWindowIcon(sampleWindow, dc);
    e.dominant = ComputeDominantColor(e.bitmap.get(), dc);
    ID2D1Bitmap* raw = e.bitmap.get();
    entries_[key] = std::move(e);
    return raw;
}

uint32_t IconCache::DominantColor(const AppKey& key) const {
    const auto it = entries_.find(key);
    return it != entries_.end() ? it->second.dominant : 0;
}

void IconCache::SetInUse(const std::vector<AppKey>& keys) {
    const ULONGLONG now = GetTickCount64();
    for (auto& [key, e] : entries_) {
        const bool used = std::find(keys.begin(), keys.end(), key) != keys.end();
        if (e.inUse && !used) e.unusedSince = now;
        e.inUse = used;
    }
}

bool IconCache::EvictExpired() {
    const ULONGLONG now = GetTickCount64();
    std::erase_if(entries_, [&](const auto& kv) {
        return !kv.second.inUse && now - kv.second.unusedSince + 500 >= kEvictAfterMs;
    });
    return HasUnused();
}

bool IconCache::HasUnused() const {
    return std::any_of(entries_.begin(), entries_.end(), [](const auto& kv) { return !kv.second.inUse; });
}

void IconCache::Clear() { entries_.clear(); }
