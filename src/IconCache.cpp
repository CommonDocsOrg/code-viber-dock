#include "IconCache.h"

#include <appmodel.h>
#include <shlobj.h>
#include <shobjidl_core.h>

#include <algorithm>
#include <climits>
#include <cwctype>
#include <string>
#include <vector>

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

winrt::com_ptr<ID2D1Bitmap> IconCache::LoadFileIcon(const std::wstring& file, int index, UINT px,
                                                    ID2D1DeviceContext* dc) {
    // Reads the icon resource straight from the exe at the requested size. Much lighter than
    // the shell namespace (IShellItemImageFactory costs ~15 MB of private bytes in caches).
    HICON icon = nullptr;
    const UINT size = std::min(px, 256u);
    if (FAILED(SHDefExtractIconW(file.c_str(), index, 0, &icon, nullptr, MAKELONG(size, 0))) || !icon) {
        return nullptr;
    }
    winrt::com_ptr<ID2D1Bitmap> result;
    if (IWICImagingFactory* wic = Wic()) {
        winrt::com_ptr<IWICBitmap> wicBitmap;
        if (SUCCEEDED(wic->CreateBitmapFromHICON(icon, wicBitmap.put()))) result = FromWic(wicBitmap.get(), dc);
    }
    DestroyIcon(icon);
    return result;
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
    IWICImagingFactory* wic = Wic();
    if (!wic) return nullptr;
    // The icon belongs to the other process / its class: don't destroy it.
    winrt::com_ptr<IWICBitmap> wicBitmap;
    if (FAILED(wic->CreateBitmapFromHICON(reinterpret_cast<HICON>(icon), wicBitmap.put()))) return nullptr;
    return FromWic(wicBitmap.get(), dc);
}

ID2D1Bitmap* IconCache::Get(const AppKey& key, HWND sampleWindow, UINT pixelSize, ID2D1DeviceContext* dc,
                            const std::wstring& iconFile, int iconIndex) {
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second.pixelSize == pixelSize) {
        it->second.inUse = true;
        return it->second.bitmap.get();
    }
    if (!dc) return nullptr;

    Entry e;
    e.pixelSize = pixelSize;
    // Cheapest source first; the shell (exact but memory hungry) only when those fail.
    if (!iconFile.empty()) e.bitmap = LoadFileIcon(iconFile, iconIndex, pixelSize, dc);
    if (!e.bitmap && key.isAumid) {
        e.bitmap = LoadPackageLogo(key, pixelSize, dc);
    } else if (!e.bitmap && !key.IsClassFallback()) {
        e.bitmap = LoadFileIcon(key.Source(), 0, pixelSize, dc);
    }
    if (!e.bitmap) e.bitmap = LoadShellImage(key, pixelSize, dc);
    if (!e.bitmap) e.bitmap = LoadWindowIcon(sampleWindow, dc);
    ID2D1Bitmap* raw = e.bitmap.get();
    entries_[key] = std::move(e);
    return raw;
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
