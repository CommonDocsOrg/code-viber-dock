#include "IconUtil.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <vector>

namespace IconUtil {

namespace {

// ".png" for "C:\x\a.PNG"; empty when the last component has no extension.
std::wstring ExtensionOf(const std::wstring& file) {
    const size_t dot = file.rfind(L'.');
    if (dot == std::wstring::npos || file.find_first_of(L"\\/", dot) != std::wstring::npos) return {};
    std::wstring ext = file.substr(dot);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

bool IsFile(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Expand(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    const DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (n == 0) return s;
    std::wstring out(n, L'\0');
    if (ExpandEnvironmentStringsW(s.c_str(), out.data(), n) == 0) return s;
    out.resize(wcsnlen(out.c_str(), out.size()));
    return out;
}

std::wstring Shell32() {
    wchar_t dir[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    return (n > 0 && n < MAX_PATH ? std::wstring(dir) : std::wstring(L"C:\\Windows\\System32")) + L"\\shell32.dll";
}

}  // namespace

bool IsImageFile(const std::wstring& file) {
    const std::wstring ext = ExtensionOf(file);
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" || ext == L".gif";
}

HICON HiconFromImageFile(const std::wstring& file, int px) {
    if (px <= 0 || px > 1024 || file.empty()) return nullptr;
    winrt::com_ptr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put())))) {
        return nullptr;
    }
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put()))) {
        return nullptr;
    }
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return nullptr;
    // Premultiplied while scaling (straight alpha bleeds the colour of transparent pixels into
    // the edges), straight again for the icon.
    winrt::com_ptr<IWICFormatConverter> premultiplied;
    if (FAILED(wic->CreateFormatConverter(premultiplied.put())) ||
        FAILED(premultiplied->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                         0.0, WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }
    const double fit = static_cast<double>(px) / static_cast<double>(std::max(w, h));
    const UINT sw = std::clamp(static_cast<UINT>(std::lround(w * fit)), 1u, static_cast<UINT>(px));
    const UINT sh = std::clamp(static_cast<UINT>(std::lround(h * fit)), 1u, static_cast<UINT>(px));
    winrt::com_ptr<IWICBitmapScaler> scaler;
    winrt::com_ptr<IWICFormatConverter> straight;
    if (FAILED(wic->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(premultiplied.get(), sw, sh, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(wic->CreateFormatConverter(straight.put())) ||
        FAILED(straight->Initialize(scaler.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }
    const UINT stride = sw * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * sh);
    if (FAILED(straight->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()))) return nullptr;

    // A top-down 32 bpp DIB with the image centred, plus an all-zero mask (the alpha decides).
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = -px;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    auto* dst = static_cast<BYTE*>(bits);
    const size_t dstStride = static_cast<size_t>(px) * 4;
    std::fill(dst, dst + dstStride * static_cast<size_t>(px), BYTE{0});
    const UINT x0 = (static_cast<UINT>(px) - sw) / 2, y0 = (static_cast<UINT>(px) - sh) / 2;
    for (UINT y = 0; y < sh; ++y) {
        std::copy_n(pixels.data() + static_cast<size_t>(y) * stride, stride,
                    dst + static_cast<size_t>(y0 + y) * dstStride + static_cast<size_t>(x0) * 4);
    }
    const std::vector<BYTE> zeros(static_cast<size_t>((px + 15) / 16 * 2) * static_cast<size_t>(px), 0);
    HBITMAP mask = CreateBitmap(px, px, 1, 1, zeros.data());
    HICON icon = nullptr;
    if (mask) {
        ICONINFO info{};
        info.fIcon = TRUE;
        info.hbmMask = mask;
        info.hbmColor = color;
        icon = CreateIconIndirect(&info);  // copies both bitmaps
        DeleteObject(mask);
    }
    DeleteObject(color);
    return icon;
}

void StartIconFor(const AppIdentity::ResolvedPin& pin, std::wstring& file, int& index) {
    file.clear();
    index = 0;
    if (!pin.iconFile.empty() && !IsImageFile(pin.iconFile) && IsFile(pin.iconFile)) {
        file = pin.iconFile;
        index = std::max(pin.iconIndex, 0);  // a negative value is a resource id, not a position
        return;
    }
    switch (pin.kind) {
        case AppIdentity::PinKind::Folder:
            file = Shell32();
            index = 3;  // the folder icon
            return;
        case AppIdentity::PinKind::RecycleBin:
            file = Shell32();
            index = 31;  // the (empty) Recycle Bin icon
            return;
        default:
            break;
    }
    for (const std::wstring* candidate : {&pin.targetPath, &pin.launchPath}) {
        const std::wstring ext = ExtensionOf(*candidate);
        // An app execution alias (WindowsApps\mspaint.exe) is a reparse point without icons.
        if ((ext == L".exe" || ext == L".dll" || ext == L".ico") && IsFile(*candidate) &&
            ExtractIconExW(candidate->c_str(), -1, nullptr, nullptr, 0) > 0) {
            file = *candidate;
            return;
        }
    }
    file = Shell32();
}

std::wstring StartImageFor(const AppIdentity::ResolvedPin& pin) {
    if (!pin.customIcon || !(IsImageFile(pin.iconFile) || ExtensionOf(pin.iconFile) == L".ico")) return {};
    return pin.iconFile;
}

bool ChooseIconResource(HWND owner, const std::wstring& file, int index, std::wstring& spec) {
    wchar_t path[MAX_PATH]{};
    wcsncpy_s(path, (file.empty() ? Shell32() : file).c_str(), _TRUNCATE);
    int chosen = std::max(index, 0);
    if (!PickIconDlg(owner, path, static_cast<UINT>(std::size(path)), &chosen)) return false;  // cancelled
    // The dialog may hand back an unexpanded path ("%SystemRoot%\..."): store the absolute one.
    const std::wstring picked = Expand(path);
    if (picked.empty()) return false;
    spec = chosen == 0 && ExtensionOf(picked) == L".ico" ? picked : picked + L"," + std::to_wstring(chosen);
    return true;
}

bool ChooseIconFile(HWND owner, const std::wstring& startFile, std::wstring& spec) {
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
        MessageBeep(MB_ICONERROR);
        return false;
    }
    const COMDLG_FILTERSPEC filters[] = {
        {L"Icons and images (*.png;*.jpg;*.bmp;*.gif;*.ico;*.exe;*.dll)",
         L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.ico;*.exe;*.dll"},
        {L"All files", L"*.*"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetTitle(L"Choose an icon or image");
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    if (IsFile(startFile)) {
        const size_t slash = startFile.find_last_of(L"\\/");
        winrt::com_ptr<IShellItem> folder;
        if (slash != std::wstring::npos &&
            SUCCEEDED(SHCreateItemFromParsingName(startFile.substr(0, slash).c_str(), nullptr,
                                                  IID_PPV_ARGS(folder.put())))) {
            dialog->SetFolder(folder.get());
        }
    }
    if (FAILED(dialog->Show(owner))) return false;  // cancelled
    winrt::com_ptr<IShellItem> item;
    PWSTR raw = nullptr;
    if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) {
        return false;
    }
    const std::wstring picked(raw);
    CoTaskMemFree(raw);
    if (IsImageFile(picked) || ExtensionOf(picked) == L".ico") {
        spec = picked;
        return true;
    }
    // An icon resource file (.exe, .dll, ...): pick which icon.
    return ChooseIconResource(owner, picked, 0, spec);
}

}  // namespace IconUtil
