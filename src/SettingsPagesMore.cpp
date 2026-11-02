// The Animation and Pinned apps pages.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

#include "IconUtil.h"
#include "SettingsForm.h"
#include "SettingsIds.h"

namespace {

using namespace SettingsIds;

// --- Animation -------------------------------------------------------------------------------

// Speed trackbar: 0 = off, 1 - 33 = 0.25x - 4x on a log2 scale (8 steps per doubling, 17 = 1x).
constexpr int kSpeedSteps = 33;
constexpr int kSpeedOne = 17;

double PosToSpeed(int pos) { return pos <= 0 ? 0.0 : std::exp2(static_cast<double>(pos - kSpeedOne) / 8.0); }

int SpeedToPos(double speed) {
    if (!(speed > 0.0)) return 0;
    return std::clamp(static_cast<int>(std::lround(std::log2(speed) * 8.0)) + kSpeedOne, 1, kSpeedSteps);
}

std::wstring FormatSpeed(double speed) {
    if (!(speed > 0.0)) return L"Off";
    wchar_t buf[32];
    swprintf_s(buf, L"%.2f×", speed);
    return buf;
}

bool SystemAnimationsOn() {
    BOOL on = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) return true;
    return on != FALSE;
}

class AnimationPage : public FormPage {
public:
    explicit AnimationPage(SettingsWindow& w) : FormPage(w, L"Animation", L'\xE768') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Animation");
        Section(kSectionSpeed, L"Speed");
        Label(kSpeedLabel, L"Animation speed");
        Trackbar(kSpeedTrack, kColX - 6, 212, 0, kSpeedSteps, 4);
        Text(kSpeedValue, CtlKind::Value, L"", kColX + 216, 72);
        NextRow(kRowH);
        Text(kSpeedNote, CtlKind::Note, L"Off: the dock moves instantly. The speed divides every animation's duration.",
             kColX, right - kColX, 36);
        NextRow(36);

        Section(kSectionHide, L"Hiding and showing");
        Radios({
            {kHideSlide, kHideSlideCaption, L"Slide and fade"},
            {kHideSlideOnly, kHideSlideOnlyCaption, L"Slide"},
            {kHideFade, kHideFadeCaption, L"Fade"},
            {kHideNone, kHideNoneCaption, L"None (instant)"},
        });

        Section(kSectionLaunch, L"Launching an app");
        Radios({
            {kLaunchBounce, kLaunchBounceCaption, L"Bounce until the app opens"},
            {kLaunchHop, kLaunchHopCaption, L"Hop once"},
            {kLaunchPulse, kLaunchPulseCaption, L"Pulse"},
            {kLaunchNone, kLaunchNoneCaption, L"None"},
        });

        Section(kSectionMotion, L"Motion");
        Label(kFalloffLabel, L"Magnification falloff");
        Combo(kFalloffCombo, kColX, 220, false);
        NextRow();
        Label(kEasingLabel, L"Easing");
        Combo(kEasingCombo, kColX, 220, false);
        NextRow();
        Label(kLabelFadeLabel, L"Name label fade");
        IntSpin(kLabelFadeEdit, kLabelFadeSpin, kColX, 90, 0, 1000, 20,
                [](Config& c) -> int& { return c.animation.labelFadeMs; }, SettingsScope::Appearance);
        Text(kLabelFadeUnit, CtlKind::Note, L"ms", kColX + 98, 40);
        NextRow();

        Section(kSectionAccessibility, L"Accessibility");
        Label(kReduceMotionLabel, L"Reduce motion");
        Combo(kReduceMotionCombo, kColX, right - kColX, false);
        NextRow(kRowH);
        Text(kReduceMotionNote, CtlKind::Note, L"", kColX, right - kColX, 20);
        NextRow(20);
    }

    void Created() override {
        ComboAdd(kFalloffCombo, L"Smooth (cosine)");  // Falloff order
        ComboAdd(kFalloffCombo, L"Linear");
        ComboAdd(kEasingCombo, L"Ease out");  // Easing order
        ComboAdd(kEasingCombo, L"Linear");
        ComboAdd(kEasingCombo, L"Spring");
        ComboAdd(kReduceMotionCombo, L"Follow Windows (animation effects)");  // ReduceMotion order
        ComboAdd(kReduceMotionCombo, L"On: no slides, bounces or fades");
        ComboAdd(kReduceMotionCombo, L"Off: always animate");
    }

    void Load() override {
        const AnimationConfig& a = Cfg().animation;
        LoadInts();
        SetTrack(kSpeedTrack, SpeedToPos(a.speed));
        SetText(kSpeedValue, FormatSpeed(a.speed));
        SetCheck(kHideSlide, a.hideStyle == HideStyle::Slide);
        SetCheck(kHideSlideOnly, a.hideStyle == HideStyle::SlideOnly);
        SetCheck(kHideFade, a.hideStyle == HideStyle::Fade);
        SetCheck(kHideNone, a.hideStyle == HideStyle::None);
        SetCheck(kLaunchBounce, a.launchStyle == LaunchStyle::Bounce);
        SetCheck(kLaunchHop, a.launchStyle == LaunchStyle::Hop);
        SetCheck(kLaunchPulse, a.launchStyle == LaunchStyle::Pulse);
        SetCheck(kLaunchNone, a.launchStyle == LaunchStyle::None);
        SetComboSel(kFalloffCombo, static_cast<int>(a.magnifyFalloff));
        SetComboSel(kEasingCombo, static_cast<int>(a.easing));
        SetComboSel(kReduceMotionCombo, static_cast<int>(a.reduceMotion));
        UpdateSystemNote();
    }

    void OnSettingChange(WPARAM wp, LPARAM) override {
        if (wp == SPI_SETCLIENTAREAANIMATION || wp == 0) UpdateSystemNote();
    }

    void Track(int id, int pos, bool released) override {
        if (id != kSpeedTrack) return;
        const double speed = PosToSpeed(pos);
        SetText(kSpeedValue, FormatSpeed(speed));
        AnimationConfig& a = Cfg().animation;
        if (released && std::fabs(a.speed - speed) > 1e-9) {
            a.speed = speed;
            Changed(SettingsScope::Appearance);
        }
    }

    void OnCommand(int id, int code, HWND) override {
        AnimationConfig& a = Cfg().animation;
        if (code == CBN_SELCHANGE) {
            const int sel = ComboSel(id);
            if (sel < 0) return;
            if (id == kFalloffCombo) Set(a.magnifyFalloff, static_cast<Falloff>(std::min(sel, 1)));
            if (id == kEasingCombo) Set(a.easing, static_cast<Easing>(std::min(sel, 2)));
            if (id == kReduceMotionCombo) Set(a.reduceMotion, static_cast<ReduceMotion>(std::min(sel, 2)));
            return;
        }
        if (code != BN_CLICKED || !GetCheck(id)) return;
        switch (id) {
            case kHideSlide: Set(a.hideStyle, HideStyle::Slide); break;
            case kHideSlideOnly: Set(a.hideStyle, HideStyle::SlideOnly); break;
            case kHideFade: Set(a.hideStyle, HideStyle::Fade); break;
            case kHideNone: Set(a.hideStyle, HideStyle::None); break;
            case kLaunchBounce: Set(a.launchStyle, LaunchStyle::Bounce); break;
            case kLaunchHop: Set(a.launchStyle, LaunchStyle::Hop); break;
            case kLaunchPulse: Set(a.launchStyle, LaunchStyle::Pulse); break;
            case kLaunchNone: Set(a.launchStyle, LaunchStyle::None); break;
            default: break;
        }
    }

private:
    // DockWindow::ApplyConfig -> RefreshMotion picks every animation setting up.
    template <typename T>
    void Set(T& field, T value) {
        if (field == value) return;
        field = value;
        Changed(SettingsScope::Appearance);
    }

    void UpdateSystemNote() {
        SetText(kReduceMotionNote, std::wstring(L"Windows animation effects: ") + (SystemAnimationsOn() ? L"on" : L"off"));
    }
};

// --- Pinned apps -----------------------------------------------------------------------------

constexpr int kListH = 236;
constexpr int kNameColDip = 180;

std::wstring StemOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

class PinnedPage : public FormPage {
public:
    explicit PinnedPage(SettingsWindow& w) : FormPage(w, L"Pinned apps", L'\xE718') {}
    ~PinnedPage() override {
        if (images_) ImageList_Destroy(images_);  // LVS_SHAREIMAGELISTS: the list never frees it
    }

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Pinned apps");
        Add(kPinnedList, CtlKind::List, WC_LISTVIEWW, L"Pinned apps",
            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | LVS_SHAREIMAGELISTS,
            RECT{kMargin, y_, right, y_ + kListH});
        NextRow(kListH + 8);
        Button(kPinAdd, L"Add…", kMargin, 100);
        Button(kPinRemove, L"Remove", kMargin + 108, 100);
        Button(kPinUp, L"Move up", kMargin + 216, 100);
        Button(kPinDown, L"Move down", kMargin + 324, 100);
        NextRow(kRowH + 4);
        Button(kPinAddFolder, L"Add folder…", kMargin, 124);
        Button(kPinAddSeparator, L"Add separator", kMargin + 132, 124);
        Button(kPinAddRecycleBin, L"Add Recycle Bin", kMargin + 264, 124);
        NextRow(kRowH + 4);
        Button(kPinChooseIcon, L"Choose icon…", kMargin, 124);
        Button(kPinChooseImage, L"Choose image…", kMargin + 132, 124);
        Button(kPinResetIcon, L"Reset icon", kMargin + 264, 124);
        NextRow(kRowH + 4);
        Text(kPinnedNote, CtlKind::Note,
             L"Changes apply to the dock at once. On the dock itself you can also drag icons to reorder them, "
             L"drop shortcuts or folders onto it to pin them, or drag a pinned icon away to unpin it.",
             kMargin, right - kMargin, 36);
        NextRow(36);
    }

    void Created() override {
        HWND list = Item(kPinnedList);
        if (!list) return;
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<wchar_t*>(L"Name");
        col.cx = 100;
        col.iSubItem = 0;
        ListView_InsertColumn(list, 0, &col);
        col.pszText = const_cast<wchar_t*>(L"Target");
        col.iSubItem = 1;
        ListView_InsertColumn(list, 1, &col);
    }

    void OnStyleChanged() override {
        HWND list = Item(kPinnedList);
        if (!list) return;
        // Column widths and the small icons at the current DPI.
        const Ctl* c = Find(kPinnedList);
        const int scrollBar = GetSystemMetricsForDpi(SM_CXVSCROLL, Window().Dpi());
        const int total = Window().Px(c->dip.right - c->dip.left) - scrollBar - 4;
        const int name = Window().Px(kNameColDip);
        ListView_SetColumnWidth(list, 0, name);
        ListView_SetColumnWidth(list, 1, std::max(Window().Px(80), total - name));
        const int px = GetSystemMetricsForDpi(SM_CXSMICON, Window().Dpi());
        if (images_ && px == iconPx_) return;
        HIMAGELIST old = images_;
        iconPx_ = px;
        images_ = ImageList_Create(px, px, ILC_COLOR32 | ILC_MASK, 8, 8);
        ListView_SetImageList(list, images_, LVSIL_SMALL);
        if (old) {
            ImageList_Destroy(old);
            FillList(Selected());  // the icons again, at the new size
        }
    }

    void Load() override { FillList(Selected()); }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) == kPinnedList && nm.code == LVN_ITEMCHANGED) UpdateButtons();
        return 0;
    }

    void OnCommand(int id, int code, HWND) override {
        if (code != BN_CLICKED) return;
        // Rows are the pin items of the dock (app / folder / separator / recycleBin), in order.
        DockConfig& dock = Cfg().Main();
        const std::vector<size_t> idx = dock.PinIndexes();
        const int sel = Selected();
        const int n = static_cast<int>(idx.size());
        switch (id) {
            case kPinAdd:
                AddFiles();
                break;
            case kPinAddFolder:
                AddFolder();
                break;
            case kPinAddSeparator:
            case kPinAddRecycleBin: {
                if (id == kPinAddRecycleBin && HasRecycleBin()) return;
                DockItemEntry entry;
                entry.kind = ToString(id == kPinAddSeparator ? ItemKind::Separator : ItemKind::RecycleBin);
                dock.InsertPin(SIZE_MAX, std::move(entry));  // after the last pin
                Changed(SettingsScope::Pins);
                FillList(static_cast<int>(dock.PinCount()) - 1);
                break;
            }
            case kPinChooseIcon:
            case kPinChooseImage:
                ChooseIcon(sel, id == kPinChooseImage);
                break;
            case kPinResetIcon:
                if (sel < 0 || sel >= n || dock.items[idx[static_cast<size_t>(sel)]].icon.empty()) return;
                dock.items[idx[static_cast<size_t>(sel)]].icon.clear();
                Changed(SettingsScope::Pins);
                FillList(sel);
                break;
            case kPinRemove:
                if (sel < 0 || sel >= n) return;
                dock.ErasePin(static_cast<size_t>(sel));
                Changed(SettingsScope::Pins);
                FillList(std::min(sel, n - 2));
                break;
            case kPinUp:
            case kPinDown: {
                const int to = id == kPinUp ? sel - 1 : sel + 1;
                if (sel < 0 || sel >= n || to < 0 || to >= n) return;
                std::swap(dock.items[idx[static_cast<size_t>(sel)]], dock.items[idx[static_cast<size_t>(to)]]);
                Changed(SettingsScope::Pins);
                FillList(to);
                break;
            }
            default:
                break;
        }
    }

private:
    int Selected() const {
        HWND list = Item(kPinnedList);
        return list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    }

    void UpdateButtons() {
        const int sel = Selected();
        const std::vector<size_t> idx = Cfg().Main().PinIndexes();
        const int n = static_cast<int>(idx.size());
        Enable(kPinRemove, sel >= 0);
        Enable(kPinUp, sel > 0);
        Enable(kPinDown, sel >= 0 && sel < n - 1);
        Enable(kPinAddRecycleBin, !HasRecycleBin());
        const DockItemEntry* e = sel >= 0 && sel < n ? &Cfg().Main().items[idx[static_cast<size_t>(sel)]] : nullptr;
        const bool iconable = e && e->Kind() != ItemKind::Separator;
        Enable(kPinChooseIcon, iconable);
        Enable(kPinChooseImage, iconable);
        Enable(kPinResetIcon, iconable && !e->icon.empty());
        // A button that just got disabled must not keep the keyboard focus.
        if (HWND f = GetFocus(); f && IsChild(Hwnd(), f) && !IsWindowEnabled(f)) SetFocus(Item(kPinnedList));
    }

    bool HasRecycleBin() { return Cfg().Main().Has(ItemKind::RecycleBin); }

    int AddIcon(const AppIdentity::ResolvedPin* pin, const DockItemEntry& e) {
        if (!images_) return -1;
        const ItemKind special = e.Kind();
        if (special == ItemKind::Separator) return -1;  // no icon: the name says "Separator"
        HICON icon = nullptr;
        // The custom icon (or a shortcut's icon location) first: an image, else an icon resource.
        if (pin && !pin->iconFile.empty()) {
            if (IconUtil::IsImageFile(pin->iconFile)) {
                icon = IconUtil::HiconFromImageFile(pin->iconFile, iconPx_);
            } else {
                SHDefExtractIconW(pin->iconFile.c_str(), pin->iconIndex, 0, &icon, nullptr,
                                  MAKELONG(iconPx_, iconPx_));
            }
        }
        if (!icon && special == ItemKind::RecycleBin) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            SHFILEINFOW sfi{};
            if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, &pidl)) && pidl) {
                if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &sfi, sizeof(sfi),
                                   SHGFI_PIDL | SHGFI_ICON | SHGFI_SMALLICON)) {
                    icon = sfi.hIcon;
                }
                CoTaskMemFree(pidl);
            }
            if (!icon) return -1;
        }
        SHFILEINFOW sfi{};
        if (!icon && !e.path.empty()) {
            UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
            if (pin && pin->missing) flags |= SHGFI_USEFILEATTRIBUTES;  // the generic icon of its type
            if (SHGetFileInfoW(e.path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), flags)) icon = sfi.hIcon;
        }
        if (!icon && !e.aumid.empty()) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName((L"shell:AppsFolder\\" + e.aumid).c_str(), nullptr, &pidl, 0, nullptr))) {
                if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &sfi, sizeof(sfi),
                                   SHGFI_PIDL | SHGFI_ICON | SHGFI_SMALLICON)) {
                    icon = sfi.hIcon;
                }
                CoTaskMemFree(pidl);
            }
        }
        if (!icon && SHGetFileInfoW(L".exe", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                                    SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_SMALLICON)) {
            icon = sfi.hIcon;
        }
        if (!icon) return -1;
        const int index = ImageList_AddIcon(images_, icon);  // scaled to the list's size if needed
        DestroyIcon(icon);
        return index;
    }

    // The pins of config.Main() (with host.pins(), their resolved twin) -> the list; selects
    // `select` (clamped).
    void FillList(int select) {
        HWND list = Item(kPinnedList);
        if (!list) return;
        static const std::vector<AppIdentity::ResolvedPin> kNone;
        const auto& pins = Host().pins ? Host().pins() : kNone;
        const std::vector<DockItemEntry> pinned = Cfg().Main().Pins();
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        if (images_) ImageList_RemoveAll(images_);
        for (size_t i = 0; i < pinned.size(); ++i) {
            const DockItemEntry& e = pinned[i];
            const AppIdentity::ResolvedPin* pin = i < pins.size() ? &pins[i] : nullptr;
            std::wstring name = pin ? pin->displayName : std::wstring();
            if (name.empty()) name = !e.path.empty() ? StemOf(e.path) : e.aumid;
            if (pin && pin->missing) name += L" (missing)";
            std::wstring target = pin && !pin->targetPath.empty() ? pin->targetPath : !e.path.empty() ? e.path : e.aumid;
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_IMAGE;
            item.iItem = static_cast<int>(i);
            item.pszText = name.data();
            item.iImage = AddIcon(pin, e);
            const int at = ListView_InsertItem(list, &item);
            if (at >= 0) ListView_SetItemText(list, at, 1, target.data());
        }
        const int n = static_cast<int>(pinned.size());
        if (n > 0) {
            const int sel = std::clamp(select, 0, n - 1);
            ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list, sel, FALSE);
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        UpdateButtons();
    }

    void AddFiles() {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        const COMDLG_FILTERSPEC filters[] = {{L"Apps and shortcuts (*.lnk;*.exe)", L"*.lnk;*.exe"},
                                             {L"All files", L"*.*"}};
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetTitle(L"Pin apps to the dock");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        // NODEREFERENCELINKS: a chosen shortcut is pinned as the shortcut (keeps its arguments).
        dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM |
                           FOS_NODEREFERENCELINKS | FOS_NOCHANGEDIR);
        winrt::com_ptr<IShellItem> programs;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(programs.put())))) {
            dialog->SetDefaultFolder(programs.get());
        }
        if (FAILED(dialog->Show(Owner()))) return;  // cancelled
        winrt::com_ptr<IShellItemArray> items;
        DWORD count = 0;
        if (FAILED(dialog->GetResults(items.put())) || FAILED(items->GetCount(&count))) return;
        std::vector<std::wstring> paths;
        for (DWORD i = 0; i < count; ++i) {
            winrt::com_ptr<IShellItem> item;
            PWSTR raw = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, item.put())) &&
                SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                paths.emplace_back(raw);
                CoTaskMemFree(raw);
            }
        }
        if (paths.empty() || !Host().pinFiles) return;
        const size_t before = Cfg().Main().PinCount();
        Host().pinFiles(paths);  // skips apps that are already pinned; reloads the dock and saves
        Window().ConfigApplied();
        const size_t after = Cfg().Main().PinCount();
        FillList(after > before ? static_cast<int>(after) - 1 : Selected());
    }

    void AddFolder() {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        dialog->SetTitle(L"Pin a folder to the dock");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        if (FAILED(dialog->Show(Owner()))) return;  // cancelled
        winrt::com_ptr<IShellItem> item;
        PWSTR raw = nullptr;
        if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) {
            return;
        }
        std::vector<std::wstring> paths{std::wstring(raw)};
        CoTaskMemFree(raw);
        if (!Host().pinFiles) return;
        const size_t before = Cfg().Main().PinCount();
        Host().pinFiles(paths);  // skips a folder that is already pinned; reloads the dock and saves
        Window().ConfigApplied();
        const size_t after = Cfg().Main().PinCount();
        FillList(after > before ? static_cast<int>(after) - 1 : Selected());
    }

    // Choose icon… / Choose image… for the entry at `sel` (not separators).
    void ChooseIcon(int sel, bool image) {
        const std::vector<DockItemEntry> pinned = Cfg().Main().Pins();
        if (sel < 0 || sel >= static_cast<int>(pinned.size())) return;
        const DockItemEntry before = pinned[static_cast<size_t>(sel)];
        if (before.Kind() == ItemKind::Separator) return;
        std::wstring file, startImage;
        int index = 0;
        if (Host().pins) {
            const auto& pins = Host().pins();
            if (static_cast<size_t>(sel) < pins.size()) {
                IconUtil::StartIconFor(pins[static_cast<size_t>(sel)], file, index);
                startImage = IconUtil::StartImageFor(pins[static_cast<size_t>(sel)]);
            }
        }
        std::wstring spec;
        const bool ok = image ? IconUtil::ChooseIconFile(Owner(), startImage, spec)
                              : IconUtil::ChooseIconResource(Owner(), file, index, spec);
        if (!ok) return;
        // The list may have changed while the dialog was open (a drag on the dock): only the
        // same entry at the same place gets the icon.
        DockConfig& now = Cfg().Main();
        if (sel >= static_cast<int>(now.PinCount())) return;
        DockItemEntry& e = now.PinAt(static_cast<size_t>(sel));
        if (e.path != before.path || e.aumid != before.aumid || e.kind != before.kind) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        e.icon = spec;  // the absolute path the dialog returned
        Changed(SettingsScope::Pins);
        FillList(sel);
    }

    HIMAGELIST images_ = nullptr;
    int iconPx_ = 0;
};

}  // namespace

std::unique_ptr<SettingsPage> MakeAnimationPage(SettingsWindow& window) {
    return std::make_unique<AnimationPage>(window);
}

std::unique_ptr<SettingsPage> MakePinnedPage(SettingsWindow& window) {
    return std::make_unique<PinnedPage>(window);
}
