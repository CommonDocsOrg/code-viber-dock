// The "Folders and groups" page: one folder or shortcut group item's settings (its popup's layout,
// its icon; a group's name and members).

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

#include "IconUtil.h"
#include "SettingsForm.h"
#include "SettingsIds.h"

namespace {

using namespace SettingsIds;

constexpr int kButtonW = 122;
constexpr int kButtonStep = 130;
constexpr int kMembersH = 150;

std::wstring StemOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        // A shortcut / exe shows without its extension; a folder name keeps its dots.
        const std::wstring ext = name.substr(dot);
        if (_wcsicmp(ext.c_str(), L".lnk") == 0 || _wcsicmp(ext.c_str(), L".exe") == 0) name.resize(dot);
    }
    return name;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::iswspace(s[start])) ++start;
    return s.substr(start);
}

// Files picked in an open dialog (shortcuts / apps, or one folder).
std::vector<std::wstring> PickFiles(HWND owner, bool folder) {
    std::vector<std::wstring> paths;
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
        MessageBeep(MB_ICONERROR);
        return paths;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    if (folder) {
        dialog->SetTitle(L"Add a folder to the group");
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    } else {
        const COMDLG_FILTERSPEC filters[] = {{L"Apps and shortcuts (*.lnk;*.exe)", L"*.lnk;*.exe"}, {L"All files", L"*.*"}};
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetTitle(L"Add apps to the group");
        dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM |
                           FOS_NODEREFERENCELINKS | FOS_NOCHANGEDIR);
        winrt::com_ptr<IShellItem> programs;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(programs.put())))) {
            dialog->SetDefaultFolder(programs.get());
        }
    }
    if (FAILED(dialog->Show(owner))) return paths;  // cancelled
    winrt::com_ptr<IShellItemArray> items;
    DWORD count = 0;
    if (FAILED(dialog->GetResults(items.put())) || FAILED(items->GetCount(&count))) return paths;
    for (DWORD i = 0; i < count; ++i) {
        winrt::com_ptr<IShellItem> item;
        PWSTR raw = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
            paths.emplace_back(raw);
            CoTaskMemFree(raw);
        }
    }
    return paths;
}

class ItemSettingsPage : public FormPage {
public:
    explicit ItemSettingsPage(SettingsWindow& w) : FormPage(w, L"Folders and groups", L'\xE8B7') {}

    void SelectItem(int item) override { item_ = item; }

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Folders and groups");
        Label(kGroupNameLabel, L"Name");
        EditBox(kGroupNameEdit, kColX, 260, 128);
        NextRow();
        Label(kItemViewLabel, L"Show the contents as");
        Combo(kItemViewCombo, kColX, 260, false);
        NextRow();
        Text(kItemViewNote, CtlKind::Note,
             L"Automatic: a grid up to 48 entries, a scrolling list above. Both scroll with the wheel or by "
             L"dragging; type to select, Enter opens.",
             kColX, right - kColX, 36);
        NextRow(40);
        Button(kItemChooseIcon, L"Choose icon…", kMargin, kButtonW);
        Button(kItemChooseImage, L"Choose image…", kMargin + kButtonStep, kButtonW);
        Button(kItemResetIcon, L"Reset icon", kMargin + 2 * kButtonStep, kButtonW);
        NextRow(kRowH + 8);
        Text(kItemNote, CtlKind::Note, L"", kMargin, right - kMargin, 36);
        NextRow(40);
        Section(kGroupMembersLabel, L"Shortcuts in the group");
        NextRow();
        Add(kGroupMembers, CtlKind::List, WC_LISTVIEWW, L"Shortcuts in the group",
            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
            RECT{kMargin, y_, right, y_ + kMembersH});
        NextRow(kMembersH + 8);
        Button(kGroupAdd, L"Add app…", kMargin, kButtonW);
        Button(kGroupAddFolder, L"Add folder…", kMargin + kButtonStep, kButtonW);
        Button(kGroupRemove, L"Remove", kMargin + 2 * kButtonStep, kButtonW);
        NextRow(kRowH + 4);
        Button(kGroupUp, L"Move up", kMargin, kButtonW);
        Button(kGroupDown, L"Move down", kMargin + kButtonStep, kButtonW);
        NextRow(kRowH + 4);
    }

    void Created() override {
        ComboAdd(kItemViewCombo, L"Automatic");
        ComboAdd(kItemViewCombo, L"Grid");
        ComboAdd(kItemViewCombo, L"List");
        if (HWND list = Item(kGroupMembers)) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.cx = 100;
            col.pszText = const_cast<wchar_t*>(L"Name");
            col.iSubItem = 0;
            ListView_InsertColumn(list, 0, &col);
            col.pszText = const_cast<wchar_t*>(L"Target");
            col.iSubItem = 1;
            ListView_InsertColumn(list, 1, &col);
        }
    }

    void OnStyleChanged() override {
        HWND list = Item(kGroupMembers);
        const Ctl* c = Find(kGroupMembers);
        if (!list || !c) return;
        const int scrollBar = GetSystemMetricsForDpi(SM_CXVSCROLL, Window().Dpi());
        const int total = Window().Px(c->dip.right - c->dip.left) - scrollBar - 4;
        const int name = Window().Px(180);
        ListView_SetColumnWidth(list, 0, name);
        ListView_SetColumnWidth(list, 1, std::max(Window().Px(80), total - name));
    }

    void Load() override {
        // The Item window's item (SelectItem); Entry() is null when it is not a folder / group.
        const DockItemEntry* e = Entry();
        const bool group = e && e->Kind() == ItemKind::Group;
        Enable(kItemViewCombo, e != nullptr);
        Enable(kItemChooseIcon, e != nullptr);
        Enable(kItemChooseImage, e != nullptr);
        Enable(kItemResetIcon, e && !e->icon.empty());
        SetComboSel(kItemViewCombo, e ? static_cast<int>(e->view) : -1);
        for (int id : {kGroupNameLabel, kGroupNameEdit, kGroupMembersLabel, kGroupMembers, kGroupAdd, kGroupAddFolder,
                       kGroupRemove, kGroupUp, kGroupDown}) {
            Show(id, group);
        }
        SetText(kGroupNameEdit, group ? (e->name.empty() ? std::wstring(L"Group") : e->name) : std::wstring());
        if (!e) {
            SetText(kItemNote, L"This item is not a folder or a group any more.");
        } else if (group) {
            SetText(kItemNote, std::to_wstring(e->members.size()) +
                                   (e->members.size() == 1 ? L" shortcut." : L" shortcuts.") +
                                   L" On the dock: drag an icon onto the group to add it, drag one out of the group's popup "
                                   L"onto the dock to make it a pin again.");
        } else {
            SetText(kItemNote, std::wstring(L"Path: ") + e->path);
        }
        FillMembers(memberSel_);
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) == kGroupMembers && nm.code == LVN_ITEMCHANGED) UpdateMemberButtons();
        return 0;
    }

    void OnCommit(int id) override {
        if (id != kGroupNameEdit) return;
        DockItemEntry* e = Entry();
        if (!e || e->Kind() != ItemKind::Group) return;
        std::wstring name = Trim(GetText(kGroupNameEdit));
        if (name == L"Group") name.clear();  // the default is not written
        if (name == e->name) return;
        e->name = name;
        Changed(SettingsScope::Pins);
        Window().Refresh();
    }

    void OnCommand(int id, int code, HWND) override {
        DockItemEntry* e = Entry();
        if (!e) return;
        if (id == kItemViewCombo && code == CBN_SELCHANGE) {
            const int sel = ComboSel(kItemViewCombo);
            if (sel < 0 || sel > 2 || static_cast<int>(e->view) == sel) return;
            e->view = static_cast<ItemView>(sel);
            Changed(SettingsScope::Pins);
            return;
        }
        if (code != BN_CLICKED) return;
        const bool group = e->Kind() == ItemKind::Group;
        const int member = SelectedMember();
        switch (id) {
            case kItemChooseIcon:
            case kItemChooseImage:
                ChooseIcon(id == kItemChooseImage);
                break;
            case kItemResetIcon:
                if (e->icon.empty()) return;
                e->icon.clear();
                Changed(SettingsScope::Pins);
                Window().Refresh();
                break;
            case kGroupAdd:
            case kGroupAddFolder:
                if (group) AddMembers(id == kGroupAddFolder);
                break;
            case kGroupRemove:
                if (!group || member < 0 || static_cast<size_t>(member) >= e->members.size()) return;
                e->members.erase(e->members.begin() + member);
                memberSel_ = std::min(member, static_cast<int>(e->members.size()) - 1);
                Changed(SettingsScope::Pins);
                Window().Refresh();
                break;
            case kGroupUp:
            case kGroupDown: {
                const int to = id == kGroupUp ? member - 1 : member + 1;
                if (!group || member < 0 || to < 0 || to >= static_cast<int>(e->members.size())) return;
                std::swap(e->members[static_cast<size_t>(member)], e->members[static_cast<size_t>(to)]);
                memberSel_ = to;
                Changed(SettingsScope::Pins);
                Window().Refresh();
                break;
            }
            default:
                break;
        }
    }

private:
    // The resolved pin of row `row` (an index into the edited dock's items; null: none).
    const AppIdentity::ResolvedPin* PinOf(int row) {
        if (!Host().pins) return nullptr;
        const std::vector<size_t> pinIdx = Dock().PinIndexes();
        const auto& pins = Host().pins();
        for (size_t k = 0; k < pinIdx.size() && k < pins.size(); ++k) {
            if (static_cast<int>(pinIdx[k]) == row) return &pins[k];
        }
        return nullptr;
    }
    std::wstring PinName(int row) {
        const AppIdentity::ResolvedPin* p = PinOf(row);
        return p ? p->displayName : std::wstring();
    }

    // A folder item, an app item that resolved to a folder (a shortcut to one), or a group.
    bool Editable(const DockItemEntry& e, int row) {
        const ItemKind k = e.Kind();
        if (k == ItemKind::Folder || k == ItemKind::Group) return true;
        if (k != ItemKind::App) return false;
        const AppIdentity::ResolvedPin* p = PinOf(row);
        return p && p->kind == AppIdentity::PinKind::Folder;
    }

    DockItemEntry* Entry() {
        auto& items = Dock().items;
        if (item_ < 0 || static_cast<size_t>(item_) >= items.size() || !Editable(items[static_cast<size_t>(item_)], item_)) {
            return nullptr;
        }
        return &items[static_cast<size_t>(item_)];
    }

    int SelectedMember() const {
        HWND list = Item(kGroupMembers);
        return list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    }

    void FillMembers(int select) {
        HWND list = Item(kGroupMembers);
        if (!list) return;
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        const DockItemEntry* e = Entry();
        if (e && e->Kind() == ItemKind::Group) {
            const AppIdentity::ResolvedPin* group = PinOf(item_);
            for (size_t i = 0; i < e->members.size(); ++i) {
                const DockItemEntry& m = e->members[i];
                std::wstring name = group && i < group->members.size() ? group->members[i].displayName : std::wstring();
                if (name.empty()) name = !m.path.empty() ? StemOf(m.path) : m.aumid;
                std::wstring target = !m.path.empty() ? m.path : m.aumid;
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = static_cast<int>(i);
                item.pszText = name.data();
                const int at = ListView_InsertItem(list, &item);
                if (at >= 0) ListView_SetItemText(list, at, 1, target.data());
            }
            const int n = static_cast<int>(e->members.size());
            if (n > 0) {
                const int sel = std::clamp(select, 0, n - 1);
                ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(list, sel, FALSE);
            }
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        UpdateMemberButtons();
    }

    void UpdateMemberButtons() {
        const DockItemEntry* e = Entry();
        const bool group = e && e->Kind() == ItemKind::Group;
        const int sel = SelectedMember();
        const int n = group ? static_cast<int>(e->members.size()) : 0;
        if (sel >= 0) memberSel_ = sel;
        Enable(kGroupAdd, group);
        Enable(kGroupAddFolder, group);
        Enable(kGroupRemove, group && sel >= 0 && sel < n);
        Enable(kGroupUp, group && sel > 0 && sel < n);
        Enable(kGroupDown, group && sel >= 0 && sel < n - 1);
        if (HWND f = GetFocus(); f && IsChild(Hwnd(), f) && !IsWindowEnabled(f)) SetFocus(Item(kGroupMembers));
    }

    // Add app… / Add folder…: new members after the last one (apps already in the group skipped).
    void AddMembers(bool folder) {
        const std::vector<std::wstring> paths = PickFiles(Owner(), folder);
        if (paths.empty()) return;
        DockItemEntry* e = Entry();  // again: the dialog was modal
        if (!e || e->Kind() != ItemKind::Group) return;
        std::vector<AppKey> keys;
        for (const DockItemEntry& m : e->members) keys.push_back(AppIdentity::ResolvePin(m).key);
        bool changed = false;
        for (const std::wstring& path : paths) {
            DockItemEntry entry;
            entry.path = path;
            const AppIdentity::ResolvedPin pin = AppIdentity::ResolvePin(entry);
            if (pin.kind != AppIdentity::PinKind::App && pin.kind != AppIdentity::PinKind::Folder) continue;
            if (std::find(keys.begin(), keys.end(), pin.key) != keys.end()) continue;
            keys.push_back(pin.key);
            DockItemEntry m;
            m.kind = ToString(pin.kind == AppIdentity::PinKind::Folder ? ItemKind::Folder : ItemKind::App);
            m.path = pin.launchPath.empty() ? path : pin.launchPath;
            e->members.push_back(std::move(m));
            changed = true;
        }
        if (!changed) return;
        memberSel_ = static_cast<int>(e->members.size()) - 1;
        Changed(SettingsScope::Pins);
        Window().Refresh();
    }

    void ChooseIcon(bool image) {
        const DockItemEntry* e = Entry();
        if (!e) return;
        const DockItemEntry before = *e;
        std::wstring file, startImage;
        int index = 0;
        if (const AppIdentity::ResolvedPin* pin = PinOf(item_)) {
            IconUtil::StartIconFor(*pin, file, index);
            startImage = IconUtil::StartImageFor(*pin);
        }
        std::wstring spec;
        const bool ok = image ? IconUtil::ChooseIconFile(Owner(), startImage, spec)
                              : IconUtil::ChooseIconResource(Owner(), file, index, spec);
        if (!ok) return;
        // The list may have changed while the dialog was open: only the same entry gets the icon.
        DockItemEntry* now = Entry();
        if (!now || now->path != before.path || now->kind != before.kind || now->name != before.name) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        now->icon = spec;
        Changed(SettingsScope::Pins);
        Window().Refresh();
    }

    int item_ = -1;           // the Item window's item: an index into its dock's items (-1: none)
    int memberSel_ = 0;       // the selected member row (kept across reloads)
};

}  // namespace

std::unique_ptr<SettingsPage> MakeItemSettingsPage(SettingsWindow& window) {
    return std::make_unique<ItemSettingsPage>(window);
}
