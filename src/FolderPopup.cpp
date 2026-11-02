#include "FolderPopup.h"

#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "Theme.h"

namespace {

// Layout in DIPs.
constexpr float kPad = 8.0f;           // popup edge -> header / grid
constexpr float kHeaderH = 32.0f;
constexpr float kHeaderGap = 6.0f;     // header -> grid (the hairline sits in it)
constexpr float kBackSize = 28.0f;     // back glyph hit box
constexpr float kButtonH = 24.0f;      // "Open in Explorer"
constexpr float kButtonPadX = 10.0f;
constexpr float kCellW = 88.0f;
constexpr float kCellH = 84.0f;
constexpr float kIconDip = 40.0f;
constexpr float kIconTop = 6.0f;       // cell top -> icon
constexpr float kTextGap = 4.0f;       // icon -> name
constexpr float kTextPadX = 4.0f;      // cell edge -> name
constexpr float kLineH = 16.0f;        // name line height (two lines)
constexpr float kFooterH = 28.0f;      // "+N more"
constexpr float kMessageH = 56.0f;     // "This folder is empty"
constexpr float kMinWidth = 260.0f;
constexpr float kMinTitleW = 96.0f;    // the header keeps at least this much room for the name
constexpr float kCellRadius = 6.0f;
constexpr float kNameFont = 12.0f;
constexpr float kTitleFont = 14.0f;
constexpr float kButtonFont = 12.0f;
constexpr float kMonitorMargin = 8.0f;
constexpr size_t kMaxColumns = 6;
constexpr wchar_t kExplorerText[] = L"Open in Explorer";

LONG Px(float dip, float scale) { return static_cast<LONG>(std::lround(dip * scale)); }

D2D1_RECT_F ToRectF(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                       static_cast<float>(r.bottom));
}

bool Empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) return dir + name;
    return dir + L'\\' + name;
}

// "C:\Users\me\" -> "C:\Users\me"; a drive root keeps its backslash ("C:\").
std::wstring Normalize(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    if (path.size() == 2 && path[1] == L':') path += L'\\';
    return path;
}

std::wstring Parent(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return Normalize(path.substr(0, slash == 2 && path[1] == L':' ? 3 : slash));
}

std::wstring LastComponent(const std::wstring& path) {
    const std::wstring p = Normalize(path);
    if (p.size() == 3 && p[1] == L':') return p.substr(0, 2);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

}  // namespace

void FolderPopup::Init(HINSTANCE instance, Renderer* renderer, IconCache* icons, Callbacks callbacks) {
    PopupWindow::Init(instance, renderer);
    icons_ = icons;
    callbacks_ = std::move(callbacks);
    SetClosedCallback(callbacks_.closed);
}

void FolderPopup::Release() {
    entries_.clear();
    entries_.shrink_to_fit();
    root_.clear();
    rootName_.clear();
    current_.clear();
    total_ = 0;
    readable_ = true;
}

void FolderPopup::OnHidden() { Release(); }

void FolderPopup::OnDestroying() {
    Release();
    nameFormat_ = nullptr;
    titleFormat_ = nullptr;
    buttonFormat_ = nullptr;
    nameEllipsis_ = nullptr;
    titleEllipsis_ = nullptr;
    formatScale_ = 0.0f;
}

bool FolderPopup::Show(const std::wstring& folder, const std::wstring& name, const Style& style,
                       const PopupAnchor& anchor) {
    if (folder.empty()) return false;
    style_ = style;
    frame_ = Frame{};
    frame_.scale = style.scale;
    frame_.acrylic = style.acrylic;
    frame_.cornerRadiusDip = style.cornerRadiusDip;
    frame_.monitor = style.monitor;
    anchor_ = anchor;
    root_ = Normalize(folder);
    rootName_ = name.empty() ? LastComponent(root_) : name;
    Load(root_);
    if (!ShowAt(frame_, anchor_)) {
        if (!OnScreen()) Release();
        return false;
    }
    return true;
}

void FolderPopup::Load(const std::wstring& folder) {
    current_ = folder;
    entries_.clear();
    total_ = 0;
    readable_ = true;
    ClearHot();

    std::vector<Entry> all;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(Join(folder, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        readable_ = false;
        return;
    }
    do {
        const wchar_t* n = fd.cFileName;
        if ((n[0] == L'.' && n[1] == 0) || (n[0] == L'.' && n[1] == L'.' && n[2] == 0)) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        Entry e;
        e.name = n;
        e.folder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        all.push_back(std::move(e));
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    // Folders first, then Explorer's logical order ("file2" before "file10").
    std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) {
        if (a.folder != b.folder) return a.folder;
        return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    });
    total_ = all.size();

    // As many cells as fit: columns min(6, ceil(sqrt(n))) within the monitor's width (beside a
    // vertical dock: the room on its side), rows within the room above the dock (below a top dock,
    // the monitor's height beside a vertical one); the rest goes into "+N more".
    const float s = style_.scale;
    const RECT& m = style_.monitor;
    const LONG pad = Px(kPad, s), cellW = Px(kCellW, s), cellH = Px(kCellH, s), margin = Px(kMonitorMargin, s);
    LONG roomW = (m.right - m.left) - 2 * margin;
    LONG roomH = (anchor_.side - m.top) - margin;
    switch (anchor_.edge) {
        case DockEdge::Top: roomH = (m.bottom - anchor_.side) - margin; break;
        case DockEdge::Left:
            roomW = (m.right - anchor_.side) - margin;
            roomH = (m.bottom - m.top) - 2 * margin;
            break;
        case DockEdge::Right:
            roomW = (anchor_.side - m.left) - margin;
            roomH = (m.bottom - m.top) - 2 * margin;
            break;
        case DockEdge::Bottom:
        default: break;
    }
    const LONG availW = roomW - 2 * pad;
    const LONG availH = roomH - 2 * pad - Px(kHeaderH + kHeaderGap + kFooterH, s);
    const size_t maxColumns = static_cast<size_t>(std::max<LONG>(1, availW / std::max<LONG>(1, cellW)));
    const size_t maxRows = static_cast<size_t>(std::max<LONG>(1, availH / std::max<LONG>(1, cellH)));
    size_t n = std::min(total_, kMaxEntries);
    columns_ = std::max<size_t>(
        1, std::min({kMaxColumns, static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(n)))), maxColumns}));
    n = std::min(n, columns_ * maxRows);
    all.resize(n);

    // Display names and icons (the system image list; one bitmap per distinct icon).
    winrt::com_ptr<IImageList> images;
    if (FAILED(SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(images.put())))) images = nullptr;
    ID2D1DeviceContext* dc = Render() ? Render()->ResourceContext() : nullptr;
    std::unordered_map<int, winrt::com_ptr<ID2D1Bitmap>> byIndex;
    for (Entry& e : all) {
        e.path = Join(folder, e.name);
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(e.path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_DISPLAYNAME)) {
            e.label = sfi.szDisplayName;
            if (images && dc && icons_) {
                auto it = byIndex.find(sfi.iIcon);
                if (it == byIndex.end()) {
                    winrt::com_ptr<ID2D1Bitmap> bitmap;
                    HICON icon = nullptr;
                    if (SUCCEEDED(images->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon)) && icon) {
                        bitmap = icons_->FromHicon(icon, dc);
                        DestroyIcon(icon);
                    }
                    it = byIndex.emplace(sfi.iIcon, std::move(bitmap)).first;
                }
                e.icon = it->second;
            }
        }
        if (e.label.empty()) e.label = e.name;
    }
    entries_ = std::move(all);
}

void FolderPopup::Navigate(const std::wstring& folder) {
    if (folder.empty()) return;
    Load(folder);
    if (!ShowAt(frame_, anchor_) && callbacks_.dismiss) callbacks_.dismiss();
}

void FolderPopup::GoBack() {
    if (current_.empty() || SamePath(current_, root_)) return;
    const std::wstring parent = Parent(current_);
    Navigate(parent.empty() ? root_ : parent);
}

std::wstring FolderPopup::Title() const {
    return SamePath(current_, root_) ? rootName_ : LastComponent(current_);
}

void FolderPopup::Open(std::wstring path, bool explorer) {
    // First: hiding may release the entries (and `path` may have come from one).
    if (callbacks_.dismiss) callbacks_.dismiss();
    std::wstring dir = explorer ? std::wstring{} : Parent(path);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    // No error UI (a message box would block the dock's thread).
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = explorer ? L"open" : nullptr;
    sei.lpFile = path.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    AllowSetForegroundWindow(ASFW_ANY);  // the click was ours: let what starts take the foreground
    if (ShellExecuteExW(&sei)) return;
    if (!explorer && GetLastError() == ERROR_NO_ASSOCIATION) {
        sei.lpVerb = L"openas";  // "How do you want to open this file?" (a separate process)
        if (ShellExecuteExW(&sei)) return;
    }
    MessageBeep(MB_ICONWARNING);
}

void FolderPopup::EnsureFormats() {
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    const float s = style_.scale;
    if (!dw || (nameFormat_ && formatScale_ == s)) return;
    nameFormat_ = titleFormat_ = buttonFormat_ = nullptr;
    nameEllipsis_ = titleEllipsis_ = nullptr;
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, kNameFont * s, L"", nameFormat_.put()))) {
        nameFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        nameFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        nameFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        // Fixed line height: exactly two lines fit the name box; the second one is ellipsized.
        nameFormat_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, kLineH * s, 13.0f * s);
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(nameFormat_.get(), nameEllipsis_.put()))) {
            nameFormat_->SetTrimming(&trimming, nameEllipsis_.get());
        }
    } else {
        nameFormat_ = nullptr;
    }
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, kTitleFont * s, L"", titleFormat_.put()))) {
        titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(titleFormat_.get(), titleEllipsis_.put()))) {
            titleFormat_->SetTrimming(&trimming, titleEllipsis_.get());
        }
    } else {
        titleFormat_ = nullptr;
    }
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, kButtonFont * s, L"", buttonFormat_.put()))) {
        buttonFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        buttonFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        buttonFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
        buttonFormat_ = nullptr;
    }
    // The button's width follows its text.
    explorerTextW_ = 0.0f;
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (buttonFormat_ && SUCCEEDED(dw->CreateTextLayout(kExplorerText, static_cast<UINT32>(wcslen(kExplorerText)),
                                                        buttonFormat_.get(), 1000.0f, 100.0f, layout.put()))) {
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(layout->GetMetrics(&tm))) explorerTextW_ = tm.widthIncludingTrailingWhitespace;
    }
    formatScale_ = s;
}

SIZE FolderPopup::Measure() {
    EnsureFormats();
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    const float s = style_.scale;
    const LONG pad = Px(kPad, s), headerH = Px(kHeaderH, s), cellW = Px(kCellW, s), cellH = Px(kCellH, s);
    const LONG buttonW = static_cast<LONG>(std::ceil(explorerTextW_)) + 2 * Px(kButtonPadX, s);
    const LONG buttonH = Px(kButtonH, s), back = Px(kBackSize, s);
    const bool navigated = !SamePath(current_, root_);
    const size_t n = entries_.size();
    const size_t cols = n ? std::min(columns_, n) : 1;
    const size_t rows = n ? (n + cols - 1) / cols : 0;
    const LONG gridW = n ? static_cast<LONG>(cols) * cellW : 0;

    LONG w = std::max({2 * pad + gridW, Px(kMinWidth, s),
                       2 * pad + (navigated ? back : 0) + Px(kMinTitleW, s) + buttonW + Px(8.0f, s)});
    const RECT& m = style_.monitor;
    w = std::min<LONG>(w, std::max<LONG>(Px(kMinWidth, s), (m.right - m.left) - 2 * Px(kMonitorMargin, s)));

    LONG y = pad;
    headerRect_ = RECT{pad, y, w - pad, y + headerH};
    backRect_ = navigated ? RECT{pad, y + (headerH - back) / 2, pad + back, y + (headerH - back) / 2 + back} : RECT{};
    explorerRect_ = RECT{w - pad - buttonW, y + (headerH - buttonH) / 2, w - pad, y + (headerH - buttonH) / 2 + buttonH};
    y += headerH + Px(kHeaderGap, s);

    if (n == 0) {
        messageRect_ = RECT{pad, y, w - pad, y + Px(kMessageH, s)};
        y = messageRect_.bottom;
    } else {
        messageRect_ = RECT{};
        const LONG left = (w - gridW) / 2;
        const float textW = static_cast<float>(cellW - 2 * Px(kTextPadX, s));
        const float textH = std::round(2.0f * kLineH * s);
        for (size_t i = 0; i < n; ++i) {
            Entry& e = entries_[i];
            const LONG x = left + static_cast<LONG>(i % cols) * cellW;
            const LONG top = y + static_cast<LONG>(i / cols) * cellH;
            e.rect = RECT{x, top, x + cellW, top + cellH};
            if (!e.text && dw && nameFormat_) {
                if (FAILED(dw->CreateTextLayout(e.label.c_str(), static_cast<UINT32>(e.label.size()), nameFormat_.get(),
                                                textW, textH, e.text.put()))) {
                    e.text = nullptr;
                }
            }
        }
        y += static_cast<LONG>(rows) * cellH;
    }
    if (total_ > n && readable_) {
        moreRect_ = RECT{pad, y, w - pad, y + Px(kFooterH, s)};
        y = moreRect_.bottom;
    } else {
        moreRect_ = RECT{};
    }
    y += pad;
    if (Hot() >= 0 && Hot() < kHitBack && static_cast<size_t>(Hot()) >= n) ClearHot();
    return SIZE{w, y};
}

void FolderPopup::Paint(ID2D1DeviceContext* dc) {
    EnsureFormats();
    IDWriteFactory* dw = Render() ? Render()->DWrite() : nullptr;
    const float s = style_.scale;
    const float stroke = std::max(1.0f, std::round(s));
    const float inset = stroke / 2.0f;
    const float radius = std::round(kCellRadius * s);
    const int hot = Hot();

    PaintPanel(dc, style_.tint, style_.border);
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(style_.text), brush.put()))) return;

    auto hotBox = [&](const RECT& r, float fillFactor, bool outline) {
        const D2D1_RECT_F f = ToRectF(r);
        brush->SetColor(Theme::ToColor(style_.hotBackground, fillFactor));
        dc->FillRoundedRectangle(D2D1::RoundedRect(f, radius, radius), brush.get());
        if (!outline) return;
        brush->SetColor(Theme::ToColor(style_.hotBorder));
        const D2D1_RECT_F edge = D2D1::RectF(f.left + inset, f.top + inset, f.right - inset, f.bottom - inset);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(edge, radius - inset, radius - inset), brush.get(), stroke);
    };

    // Header: back glyph, title, "Open in Explorer".
    if (!Empty(backRect_)) {
        if (hot == kHitBack) hotBox(backRect_, 1.0f, true);
        const D2D1_RECT_F b = ToRectF(backRect_);
        const float cx = (b.left + b.right) / 2.0f - std::round(1.0f * s);
        const float cy = (b.top + b.bottom) / 2.0f;
        const float g = std::round(5.0f * s);
        brush->SetColor(Theme::ToColor(style_.text, hot == kHitBack ? 1.0f : 0.8f));
        const float glyphStroke = std::max(1.0f, 1.5f * s);
        dc->DrawLine(D2D1::Point2F(cx + g / 2.0f, cy - g), D2D1::Point2F(cx - g / 2.0f, cy), brush.get(), glyphStroke);
        dc->DrawLine(D2D1::Point2F(cx - g / 2.0f, cy), D2D1::Point2F(cx + g / 2.0f, cy + g), brush.get(), glyphStroke);
    }
    const std::wstring title = Title();
    if (dw && titleFormat_ && !title.empty()) {
        const float left = static_cast<float>(Empty(backRect_) ? headerRect_.left + Px(4.0f, s)
                                                               : backRect_.right + Px(6.0f, s));
        const float right = static_cast<float>(explorerRect_.left - Px(8.0f, s));
        winrt::com_ptr<IDWriteTextLayout> layout;
        if (right > left &&
            SUCCEEDED(dw->CreateTextLayout(title.c_str(), static_cast<UINT32>(title.size()), titleFormat_.get(),
                                           right - left, static_cast<float>(headerRect_.bottom - headerRect_.top),
                                           layout.put()))) {
            brush->SetColor(Theme::ToColor(style_.text));
            dc->DrawTextLayout(D2D1::Point2F(left, static_cast<float>(headerRect_.top)), layout.get(), brush.get(),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    hotBox(explorerRect_, hot == kHitExplorer ? 1.0f : 0.6f, hot == kHitExplorer);
    if (buttonFormat_) {
        brush->SetColor(Theme::ToColor(style_.text, hot == kHitExplorer ? 1.0f : 0.85f));
        dc->DrawText(kExplorerText, static_cast<UINT32>(wcslen(kExplorerText)), buttonFormat_.get(),
                     ToRectF(explorerRect_), brush.get());
    }
    // Hairline under the header.
    {
        const float y = static_cast<float>(headerRect_.bottom) + std::round(kHeaderGap * s / 2.0f) - inset;
        brush->SetColor(Theme::ToColor(style_.border));
        dc->DrawLine(D2D1::Point2F(static_cast<float>(headerRect_.left), y),
                     D2D1::Point2F(static_cast<float>(headerRect_.right), y), brush.get(), stroke);
    }

    // Cells.
    const float iconPx = std::round(kIconDip * s);
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        const bool isHot = static_cast<int>(i) == hot;
        if (isHot) hotBox(e.rect, 1.0f, true);
        const D2D1_RECT_F cell = ToRectF(e.rect);
        const float ix = std::round((cell.left + cell.right - iconPx) / 2.0f);
        const float iy = cell.top + std::round(kIconTop * s);
        if (e.icon) {
            const D2D1_RECT_F dst = D2D1::RectF(ix, iy, ix + iconPx, iy + iconPx);
            dc->DrawBitmap(e.icon.get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        }
        if (e.text) {
            brush->SetColor(Theme::ToColor(style_.text, isHot ? 1.0f : 0.9f));
            dc->DrawTextLayout(D2D1::Point2F(cell.left + std::round(kTextPadX * s),
                                             iy + iconPx + std::round(kTextGap * s)),
                               e.text.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    if (!Empty(messageRect_) && buttonFormat_) {
        const wchar_t* text = readable_ ? L"This folder is empty" : L"Cannot open this folder";
        brush->SetColor(Theme::ToColor(style_.text, 0.6f));
        dc->DrawText(text, static_cast<UINT32>(wcslen(text)), buttonFormat_.get(), ToRectF(messageRect_), brush.get());
    }
    if (!Empty(moreRect_) && buttonFormat_) {
        if (hot == kHitMore) hotBox(moreRect_, 1.0f, true);
        const std::wstring more = L"+" + std::to_wstring(total_ - entries_.size()) + L" more";
        brush->SetColor(Theme::ToColor(style_.text, hot == kHitMore ? 1.0f : 0.75f));
        dc->DrawText(more.c_str(), static_cast<UINT32>(more.size()), buttonFormat_.get(), ToRectF(moreRect_),
                     brush.get());
    }
}

int FolderPopup::HitAt(POINT pt, int* part) const {
    if (part) *part = 0;
    if (!Empty(backRect_) && PtInRect(&backRect_, pt)) return kHitBack;
    if (PtInRect(&explorerRect_, pt)) return kHitExplorer;
    if (!Empty(moreRect_) && PtInRect(&moreRect_, pt)) return kHitMore;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (PtInRect(&entries_[i].rect, pt)) return static_cast<int>(i);
    }
    return -1;
}

void FolderPopup::OnClick(int index, int, Button button) {
    if (button == Button::Right) return;
    switch (index) {
        case kHitBack:
            if (button == Button::Left) GoBack();
            return;
        case kHitExplorer:
        case kHitMore:
            Open(current_, true);
            return;
        default:
            break;
    }
    if (index < 0 || static_cast<size_t>(index) >= entries_.size()) return;
    const Entry& e = entries_[static_cast<size_t>(index)];
    if (e.folder && button == Button::Left) {
        Navigate(std::wstring(e.path));  // a copy: Load replaces the entries
        return;
    }
    // A file (left or middle click), or a sub-folder with the middle button (Explorer).
    Open(e.path, e.folder);
}

bool FolderPopup::OnMessage(UINT msg, WPARAM wp, LPARAM, LRESULT& result) {
    if (msg == WM_KEYDOWN && wp == VK_BACK) {
        GoBack();
        result = 0;
        return true;
    }
    return false;
}
