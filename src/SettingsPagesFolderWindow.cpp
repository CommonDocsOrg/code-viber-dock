// The Folder window page (round 4 phase 12b): the folder window selected on the Docks page (its
// folder, title, contents, appearance and Desktop rules). Every change applies at once.

#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "ContextMenu.h"
#include "SettingsForm.h"
#include "SettingsIds.h"

namespace {

using namespace SettingsIds;

constexpr int kNumberW = 72;
constexpr int kButtonW = 150;

std::wstring PickFolder(HWND owner) {
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) return {};
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetTitle(L"The folder this window shows");
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    winrt::com_ptr<IShellItem> item;
    PWSTR name = nullptr;
    std::wstring path;
    if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(item.put())) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
        path = name;
    }
    if (name) CoTaskMemFree(name);
    return path;
}

class FolderWindowPage : public FormPage {
public:
    explicit FolderWindowPage(SettingsWindow& w) : FormPage(w, L"Folder window", L'\xE8B7') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Folder window");
        Text(kFwInfo, CtlKind::Note, L"", kMargin, right - kMargin, 40);
        NextRow(44);
        Section(kFwFolderSection, L"Folder");
        Label(kFwPathLabel, L"Folder");
        EditBox(kFwPathEdit, kColX, right - kColX - 110, 4096);
        Button(kFwBrowse, L"Browse…", right - 100, 100);
        NextRow();
        Label(kFwTitleLabel, L"Title (empty: the folder's name)");
        EditBox(kFwTitleEdit, kColX, right - kColX, 128);
        NextRow();
        Section(kFwContentsSection, L"Contents");
        Label(kFwViewLabel, L"View");
        Combo(kFwViewCombo, kColX, 200, false);
        NextRow();
        Label(kFwIconLabel, L"Icon size");
        Spin(kFwIconEdit, kFwIconSpin, kColX, kNumberW, 32, 96, 8);
        Text(kFwIconUnit, CtlKind::Note, L"DIPs (32 - 96)", kColX + kNumberW + 8, 140);
        NextRow();
        Label(kFwSortLabel, L"Sort by");
        Combo(kFwSortCombo, kColX, 200, false);
        NextRow();
        Check(kFwHidden, kFwHiddenCaption, L"Show hidden items");
        NextRow();
        Check(kFwSingleClick, kFwSingleClickCaption, L"Open items with a single click");
        NextRow();
        Check(kFwCollapsed, kFwCollapsedCaption, L"Collapsed to the title bar (double-click the title)");
        NextRow();
        Section(kFwLookSection, L"Appearance");
        Label(kFwOpacityLabel, L"Opacity");
        Spin(kFwOpacityEdit, kFwOpacitySpin, kColX, kNumberW, 20, 100, 5);
        Text(kFwOpacityUnit, CtlKind::Note, L"%", kColX + kNumberW + 8, 60);
        NextRow();
        Label(kFwTitleColorLabel, L"Title colour (#AARRGGBB)");
        EditBox(kFwTitleColorEdit, kColX, 140, 9);
        Text(kFwTitleColorNote, CtlKind::Note, L"empty: the theme's", kColX + 150, 160);
        NextRow();
        Label(kFwLinesLabel, L"Lines of name (grid)");
        Combo(kFwLinesCombo, kColX, 120, false);
        NextRow();
        Check(kFwHideTitle, kFwHideTitleCaption, L"Hide the title (a thin strip stays to move it by)");
        NextRow();
        Check(kFwTransparent, kFwTransparentCaption, L"Transparent background (no panel: the icons over the wallpaper)");
        NextRow();
        Check(kFwRollUp, kFwRollUpCaption, L"When collapsed, expand while the pointer is over it");
        NextRow();
        Section(kFwRulesSection, L"Desktop rules");
        Label(kFwRulesLabel, L"Desktop files matching");
        EditBox(kFwRulesEdit, kColX, right - kColX, 1024);
        NextRow();
        Check(kFwAutoSort, kFwAutoSortCaption, L"Move them here automatically when they land on the Desktop");
        NextRow();
        Button(kFwSortNow, L"Sort desktop now", kColX, kButtonW);
        NextRow(kRowH + 4);
        Text(kFwRulesNote, CtlKind::Note,
             L"Patterns separated by semicolons, for example *.png;*.jpg;*.gif. Matching files on your Desktop are moved "
             L"into this window's folder (the first folder window whose rules match wins), never over a file of the same "
             L"name (it becomes \"name (2)\"), and can be undone with Ctrl+Z in Explorer. Off: only Sort desktop now "
             L"moves them.",
             kMargin, right - kMargin, 76);
        NextRow(80);
    }

    void Created() override {
        ComboAdd(kFwViewCombo, L"Automatic");
        ComboAdd(kFwViewCombo, L"Grid");
        ComboAdd(kFwViewCombo, L"List");
        ComboAdd(kFwSortCombo, L"Name");
        ComboAdd(kFwSortCombo, L"Date modified");
        ComboAdd(kFwSortCombo, L"Type");
        ComboAdd(kFwSortCombo, L"Size");
        ComboAdd(kFwLinesCombo, L"One");
        ComboAdd(kFwLinesCombo, L"Two");
    }

    void Load() override {
        const DockConfig& d = Dock();
        const bool fw = d.IsFolderWindow();
        SetText(kFwInfo, fw ? L"The folder window " + d.id + L": its folder, contents, look and desktop rules."
                            : L"This is not a folder window.");
        for (int id : {kFwPathEdit, kFwBrowse, kFwTitleEdit, kFwViewCombo, kFwIconEdit, kFwIconSpin, kFwSortCombo, kFwHidden,
                       kFwSingleClick, kFwCollapsed, kFwOpacityEdit, kFwOpacitySpin, kFwTitleColorEdit, kFwLinesCombo,
                       kFwHideTitle, kFwTransparent, kFwRollUp, kFwRulesEdit, kFwAutoSort, kFwSortNow}) {
            Enable(id, fw);
        }
        const FolderWindowConfig& f = d.folder;
        SetText(kFwPathEdit, fw ? f.path : L"");
        SetText(kFwTitleEdit, fw ? d.title : L"");
        SetComboSel(kFwViewCombo, static_cast<int>(f.view));
        SetSpin(kFwIconSpin, f.iconSize);
        SetComboSel(kFwSortCombo, static_cast<int>(f.sort));
        SetCheck(kFwHidden, f.showHidden);
        SetCheck(kFwSingleClick, f.singleClick);
        SetCheck(kFwCollapsed, f.collapsed);
        SetSpin(kFwOpacitySpin, static_cast<int>(std::lround(f.opacity * 100.0)));
        SetText(kFwTitleColorEdit, f.titleColor ? FormatArgbColor(f.titleColor) : L"");
        SetComboSel(kFwLinesCombo, std::clamp(f.labelLines, 1, 2) - 1);
        SetCheck(kFwHideTitle, f.hideTitle);
        SetCheck(kFwTransparent, f.transparentBackground);
        SetCheck(kFwRollUp, f.rollUpOnHover);
        SetText(kFwRulesEdit, f.rules);
        SetCheck(kFwAutoSort, f.autoSort);
    }

    void OnCommand(int id, int code, HWND) override {
        DockConfig& d = Dock();
        if (!d.IsFolderWindow()) return;
        FolderWindowConfig& f = d.folder;
        if (code == CBN_SELCHANGE) {
            if (id == kFwViewCombo) f.view = static_cast<ItemView>(std::clamp(ComboSel(id), 0, 2));
            else if (id == kFwSortCombo) f.sort = static_cast<FolderSort>(std::clamp(ComboSel(id), 0, 3));
            else if (id == kFwLinesCombo) f.labelLines = std::clamp(ComboSel(id) + 1, 1, 2);
            else return;
            Changed(SettingsScope::Appearance);
            return;
        }
        if (code != BN_CLICKED) return;
        switch (id) {
            case kFwBrowse: {
                const std::wstring path = PickFolder(Owner());
                if (path.empty()) return;
                f.path = path;
                SetText(kFwPathEdit, path);
                break;
            }
            case kFwHidden: f.showHidden = GetCheck(id); break;
            case kFwSingleClick: f.singleClick = GetCheck(id); break;
            case kFwCollapsed: f.collapsed = GetCheck(id); break;
            case kFwHideTitle: f.hideTitle = GetCheck(id); break;
            case kFwTransparent: f.transparentBackground = GetCheck(id); break;
            case kFwRollUp: f.rollUpOnHover = GetCheck(id); break;
            case kFwAutoSort: f.autoSort = GetCheck(id); break;
            case kFwSortNow:
                if (Host().command) Host().command(ContextMenu::kSortDesktopNow);
                return;
            default:
                return;
        }
        Changed(SettingsScope::Appearance);
    }

    void Spun(int spin, int pos) override { SetNumber(spin, pos); }

    void Commit(int id) override {
        DockConfig& d = Dock();
        if (!d.IsFolderWindow()) return;
        FolderWindowConfig& f = d.folder;
        switch (id) {
            case kFwIconEdit:
            case kFwOpacityEdit: {
                int value = 0;
                const bool icon = id == kFwIconEdit;
                if (ReadInt(id, icon ? 32 : 20, icon ? 96 : 100, value)) {
                    SetNumber(icon ? kFwIconSpin : kFwOpacitySpin, value);
                } else {
                    Load();
                }
                return;
            }
            case kFwPathEdit: {
                const std::wstring path = GetText(id);
                if (path.empty() || path == f.path) return;
                f.path = path;
                break;
            }
            case kFwTitleEdit: {
                const std::wstring title = GetText(id);
                if (title == d.title) return;
                d.title = title;
                break;
            }
            case kFwTitleColorEdit: {
                const std::wstring text = GetText(id);
                uint32_t argb = 0;
                if (!text.empty() && !ParseArgbColor(text, argb)) {
                    MessageBeep(MB_ICONWARNING);
                    SetText(id, f.titleColor ? FormatArgbColor(f.titleColor) : L"");
                    return;
                }
                if (argb == f.titleColor) return;
                f.titleColor = argb;
                break;
            }
            case kFwRulesEdit: {
                const std::wstring rules = GetText(id);
                if (rules == f.rules) return;
                f.rules = rules;
                break;
            }
            default:
                return;
        }
        Changed(SettingsScope::Appearance);
    }

private:
    void SetNumber(int spin, int value) {
        DockConfig& d = Dock();
        if (!d.IsFolderWindow()) return;
        FolderWindowConfig& f = d.folder;
        if (spin == kFwIconSpin) {
            value = std::clamp(value, 32, 96);
            SetSpin(spin, value);
            if (f.iconSize == value) return;
            f.iconSize = value;
        } else if (spin == kFwOpacitySpin) {
            value = std::clamp(value, 20, 100);
            SetSpin(spin, value);
            const double o = value / 100.0;
            if (std::fabs(f.opacity - o) < 1e-6) return;
            f.opacity = o;
        } else {
            return;
        }
        Changed(SettingsScope::Appearance);
    }
};

}  // namespace

std::unique_ptr<SettingsPage> MakeFolderWindowPage(SettingsWindow& window) { return std::make_unique<FolderWindowPage>(window); }
