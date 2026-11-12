#include "MenuIcons.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include "DarkMode.h"
#include "resource.h"

namespace MenuIcons {

namespace {

std::unordered_map<uint64_t, HBITMAP> g_glyphs;     // (codepoint, dpi, colour) -> bitmap
std::unordered_map<HICON, HBITMAP> g_transient;     // FromIcon, until ReleaseTransient
std::unordered_map<UINT, HBITMAP> g_app;            // AppIcon by dpi
std::wstring g_face;                                // the icon font face that exists here

// A top-down 32 bpp DIB section, cleared (alpha 0).
HBITMAP CreateDib(UINT px, uint32_t*& bits) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = static_cast<LONG>(px);
    bi.bmiHeader.biHeight = -static_cast<LONG>(px);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* raw = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &raw, nullptr, 0);
    bits = static_cast<uint32_t*>(raw);
    if (bmp && bits) std::memset(bits, 0, static_cast<size_t>(px) * px * 4);
    return bmp;
}

HFONT CreateIconFont(const wchar_t* face, int px) {
    // ANTIALIASED_QUALITY: grey-scale coverage (ClearType's colour fringes would not make an alpha).
    return CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

// "Segoe Fluent Icons" (Windows 11) when GDI really gives that face, else "Segoe MDL2 Assets"
// (Windows 10; CreateFont silently substitutes a face that is not installed, so the selected
// face's name is compared).
const wchar_t* Face() {
    if (!g_face.empty()) return g_face.c_str();
    if (HDC dc = CreateCompatibleDC(nullptr)) {
        for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
            HFONT font = CreateIconFont(face, 16);
            if (!font) continue;
            HGDIOBJ old = SelectObject(dc, font);
            wchar_t got[LF_FACESIZE]{};
            GetTextFaceW(dc, LF_FACESIZE, got);
            SelectObject(dc, old);
            DeleteObject(font);
            if (_wcsicmp(got, face) == 0) {
                g_face = face;
                break;
            }
        }
        DeleteDC(dc);
    }
    if (g_face.empty()) g_face = L"Segoe MDL2 Assets";
    return g_face.c_str();
}

// `icon` drawn into a new premultiplied DIB of px x px. DrawIconEx alpha-blends an icon with an
// alpha channel onto the cleared surface (premultiplied colour and alpha); an icon without one
// leaves every alpha byte 0, and its alpha then comes from the AND mask (black = opaque).
HBITMAP RenderIcon(HICON icon, UINT px) {
    if (!icon || px == 0) return nullptr;
    uint32_t* bits = nullptr;
    HBITMAP bmp = CreateDib(px, bits);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return nullptr;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        DeleteObject(bmp);
        return nullptr;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    const int s = static_cast<int>(px);
    DrawIconEx(dc, 0, 0, icon, s, s, 0, nullptr, DI_NORMAL);
    GdiFlush();
    const size_t n = static_cast<size_t>(px) * px;
    bool anyAlpha = false;
    for (size_t i = 0; i < n && !anyAlpha; ++i) anyAlpha = (bits[i] >> 24) != 0;
    if (!anyAlpha) {
        uint32_t* mask = nullptr;
        HBITMAP maskBmp = CreateDib(px, mask);
        if (maskBmp && mask) {
            HGDIOBJ prev = SelectObject(dc, maskBmp);
            DrawIconEx(dc, 0, 0, icon, s, s, 0, nullptr, DI_MASK);
            GdiFlush();
            SelectObject(dc, prev);
            for (size_t i = 0; i < n; ++i) {
                const bool opaque = (mask[i] & 0x00FFFFFF) == 0;
                bits[i] = opaque ? (bits[i] | 0xFF000000u) : 0u;
            }
        }
        if (maskBmp) DeleteObject(maskBmp);
    } else {
        // Keep it valid premultiplied data (a colour channel never above alpha).
        for (size_t i = 0; i < n; ++i) {
            const uint32_t a = bits[i] >> 24;
            const uint32_t r = std::min((bits[i] >> 16) & 0xFF, a);
            const uint32_t g = std::min((bits[i] >> 8) & 0xFF, a);
            const uint32_t b = std::min(bits[i] & 0xFF, a);
            bits[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    return bmp;
}

bool IsImageFile(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring ext = path.substr(dot);
    for (const wchar_t* e : {L".png", L".jpg", L".jpeg", L".bmp", L".gif"}) {
        if (_wcsicmp(ext.c_str(), e) == 0) return true;
    }
    return false;
}

HICON ShellIcon(LPCWSTR pathOrPidl, UINT extraFlags, bool smallIcon) {
    SHFILEINFOW sfi{};
    const UINT flags = SHGFI_ICON | (smallIcon ? SHGFI_SMALLICON : SHGFI_LARGEICON) | extraFlags;
    if (!SHGetFileInfoW(pathOrPidl, 0, &sfi, sizeof(sfi), flags)) return nullptr;
    return sfi.hIcon;
}

}  // namespace

UINT SizeFor(UINT dpi) { return static_cast<UINT>(MulDiv(16, static_cast<int>(dpi ? dpi : 96), 96)); }

namespace {

// The glyph as a new px-square DIB in `colour`: premultiplied (a menu bitmap) or straight alpha
// (an image list entry: comctl32 premultiplies on ImageList_Add). Null on failure; the caller
// owns the bitmap. Round 18 phase 18C: shared by Glyph and GlyphList.
HBITMAP RenderGlyph(wchar_t codepoint, UINT px, COLORREF colour, bool premultiplied) {
    uint32_t* bits = nullptr;
    HBITMAP bmp = CreateDib(px, bits);
    HDC dc = bmp ? CreateCompatibleDC(nullptr) : nullptr;
    HFONT font = dc ? CreateIconFont(Face(), static_cast<int>(px)) : nullptr;
    if (!bmp || !bits || !dc || !font) {
        if (font) DeleteObject(font);
        if (dc) DeleteDC(dc);
        if (bmp) DeleteObject(bmp);
        return nullptr;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, font);
    // White on black: the red channel is the glyph's coverage.
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    RECT r{0, 0, static_cast<LONG>(px), static_cast<LONG>(px)};
    DrawTextW(dc, &codepoint, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    GdiFlush();
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(font);
    DeleteDC(dc);
    const uint32_t cr = GetRValue(colour), cg = GetGValue(colour), cb = GetBValue(colour);
    const size_t n = static_cast<size_t>(px) * px;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t p = bits[i];
        const uint32_t a = std::max({(p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF});
        bits[i] = premultiplied ? (a << 24) | ((cr * a / 255) << 16) | ((cg * a / 255) << 8) | (cb * a / 255)
                                : (a << 24) | (cr << 16) | (cg << 8) | cb;
    }
    return bmp;
}

}  // namespace

HBITMAP Glyph(wchar_t codepoint, UINT dpi, COLORREF colour) {
    if (!codepoint) return nullptr;
    const uint64_t key = static_cast<uint64_t>(codepoint) | (static_cast<uint64_t>(dpi & 0xFFFF) << 16) |
                         (static_cast<uint64_t>(colour & 0xFFFFFF) << 32);
    if (auto it = g_glyphs.find(key); it != g_glyphs.end()) return it->second;
    HBITMAP bmp = RenderGlyph(codepoint, SizeFor(dpi), colour, true);
    g_glyphs[key] = bmp;  // null: not tried again
    return bmp;
}

HIMAGELIST GlyphList(wchar_t codepoint, UINT dpi, COLORREF colour, COLORREF disabled) {
    if (!codepoint) return nullptr;
    const UINT px = SizeFor(dpi);
    HIMAGELIST list = ImageList_Create(static_cast<int>(px), static_cast<int>(px), ILC_COLOR32, 4, 0);
    if (!list) return nullptr;
    // BCM_SETIMAGELIST's state order: normal, hot, pressed, disabled (the button takes image
    // min(state, count - 1), so one image would stay bright on a greyed button).
    for (COLORREF c : {colour, colour, colour, disabled}) {
        HBITMAP bmp = RenderGlyph(codepoint, px, c, false);
        const int added = bmp ? ImageList_Add(list, bmp, nullptr) : -1;
        if (bmp) DeleteObject(bmp);
        if (added < 0) {
            ImageList_Destroy(list);
            return nullptr;
        }
    }
    return list;
}

HBITMAP FromIcon(HICON icon, UINT dpi) {
    if (!icon) return nullptr;
    if (auto it = g_transient.find(icon); it != g_transient.end()) return it->second;
    HBITMAP bmp = RenderIcon(icon, SizeFor(dpi));
    g_transient[icon] = bmp;
    return bmp;
}

HBITMAP AppIcon(UINT dpi) {
    if (auto it = g_app.find(dpi); it != g_app.end()) return it->second;
    const UINT px = SizeFor(dpi);
    HICON icon = nullptr;
    HBITMAP bmp = nullptr;
    if (SUCCEEDED(LoadIconWithScaleDown(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), static_cast<int>(px),
                                        static_cast<int>(px), &icon)) &&
        icon) {
        bmp = RenderIcon(icon, px);
        DestroyIcon(icon);
    }
    g_app[dpi] = bmp;
    return bmp;
}

COLORREF TextColour(HWND owner) {
    // The dark popup menus' text is white (uxtheme's dark "ImmersiveMenu" class).
    return DarkMode::MenusDarkFor(owner) ? RGB(0xFF, 0xFF, 0xFF) : GetSysColor(COLOR_MENUTEXT);
}

HICON ItemIcon(const DockItem& item, UINT px, bool binEmpty, bool& owned) {
    owned = true;
    const bool smallIcon = static_cast<int>(px) <= GetSystemMetrics(SM_CXSMICON);
    // An explicit icon (a custom icon, a shortcut's icon location). Image files would need WIC:
    // the path's shell icon below stands in for them.
    if (!item.iconFile.empty() && !IsImageFile(item.iconFile)) {
        HICON icon = nullptr;
        if (SHDefExtractIconW(item.iconFile.c_str(), item.iconIndex, 0, &icon, nullptr, MAKELONG(px, 0)) == S_OK &&
            icon) {
            return icon;
        }
    }
    if (item.kind == DockItem::Kind::RecycleBin) {
        SHSTOCKICONINFO sii{sizeof(sii)};
        if (SUCCEEDED(SHGetStockIconInfo(binEmpty ? SIID_RECYCLER : SIID_RECYCLERFULL,
                                         SHGSI_ICON | (smallIcon ? SHGSI_SMALLICON : SHGSI_LARGEICON), &sii)) &&
            sii.hIcon) {
            return sii.hIcon;
        }
        return nullptr;
    }
    if (item.kind == DockItem::Kind::Group) return nullptr;  // its icon is composed: the kind's glyph
    const AppKey& key = item.key;
    // The pin's file (the .lnk / .exe / folder) or the running app's exe: the system image list's
    // icon (the same the shell shows; cached by the shell, no extraction of ours).
    std::wstring path;
    if (item.launchPath && !item.launchPath->empty()) {
        path = *item.launchPath;
    } else if (!item.targetPath.empty()) {
        path = item.targetPath;
    } else if (!key.isAumid && !key.IsClassFallback() && !key.IsSynthetic()) {
        path = key.Source();
    }
    if (!path.empty() && !item.missing) {
        if (HICON icon = ShellIcon(path.c_str(), 0, smallIcon)) return icon;
    }
    if (key.isAumid) {
        const std::wstring name = L"shell:AppsFolder\\" + key.Source();
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(name.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
            HICON icon = ShellIcon(reinterpret_cast<LPCWSTR>(pidl), SHGFI_PIDL, smallIcon);
            CoTaskMemFree(pidl);
            if (icon) return icon;
        }
    }
    // A running window's own icon (not owned: the window / class keeps it).
    owned = false;
    for (HWND w : item.windows) {
        if (!IsWindow(w)) continue;
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(w, WM_GETICON, smallIcon ? ICON_SMALL2 : ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 50,
                                &result) &&
            result) {
            return reinterpret_cast<HICON>(result);
        }
        if (auto icon = reinterpret_cast<HICON>(GetClassLongPtrW(w, smallIcon ? GCLP_HICONSM : GCLP_HICON))) return icon;
        if (auto icon = reinterpret_cast<HICON>(GetClassLongPtrW(w, smallIcon ? GCLP_HICON : GCLP_HICONSM))) return icon;
        break;  // the first window only
    }
    return nullptr;
}

void ReleaseTransient() {
    for (auto& [icon, bmp] : g_transient) {
        if (bmp) DeleteObject(bmp);
    }
    g_transient.clear();
}

void Shutdown() {
    ReleaseTransient();
    for (auto& [key, bmp] : g_glyphs) {
        if (bmp) DeleteObject(bmp);
    }
    g_glyphs.clear();
    for (auto& [dpi, bmp] : g_app) {
        if (bmp) DeleteObject(bmp);
    }
    g_app.clear();
}

}  // namespace MenuIcons
