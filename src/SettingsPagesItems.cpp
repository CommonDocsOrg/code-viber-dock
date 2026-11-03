// The "Folders and groups" page: one folder item's settings (its popup's layout, its icon).

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <string>
#include <vector>

#include "IconUtil.h"
#include "SettingsForm.h"
#include "SettingsIds.h"

namespace {

using namespace SettingsIds;

constexpr int kButtonW = 122;
constexpr int kButtonStep = 130;

std::wstring StemOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0 && slash != std::wstring::npos && path.size() > 4) {
        // A shortcut / exe shows without its extension; a folder name keeps its dots.
        const std::wstring ext = name.substr(dot);
        if (_wcsicmp(ext.c_str(), L".lnk") == 0 || _wcsicmp(ext.c_str(), L".exe") == 0) name.resize(dot);
    }
    return name;
}

class ItemSettingsPage : public FormPage {
public:
    explicit ItemSettingsPage(SettingsWindow& w) : FormPage(w, L"Folders and groups", L'\xE8B7') {}

    void SelectItem(int item) override { item_ = item; }

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Folders and groups");
        Label(kItemPickLabel, L"Item");
        Combo(kItemPick, kColX, right - kColX, false);
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
        NextRow(36);
    }

    void Created() override {
        ComboAdd(kItemViewCombo, L"Automatic");
        ComboAdd(kItemViewCombo, L"Grid");
        ComboAdd(kItemViewCombo, L"List");
    }

    void Load() override {
        HWND pick = Item(kItemPick);
        SendMessageW(pick, CB_RESETCONTENT, 0, 0);
        picks_.clear();
        const auto& items = Cfg().Edit().items;
        const std::vector<size_t> pinIdx = Cfg().Edit().PinIndexes();
        static const std::vector<AppIdentity::ResolvedPin> kNone;
        const auto& pins = Host().pins ? Host().pins() : kNone;
        // The selected item: the one asked for, else the first folder of the dock.
        if (item_ < 0 || static_cast<size_t>(item_) >= items.size() || !Editable(items[static_cast<size_t>(item_)], item_)) {
            item_ = -1;
        }
        for (size_t i = 0; i < items.size(); ++i) {
            if (!Editable(items[i], static_cast<int>(i))) continue;
            std::wstring name;
            for (size_t k = 0; k < pinIdx.size(); ++k) {
                if (pinIdx[k] == i && k < pins.size()) name = pins[k].displayName;
            }
            if (name.empty()) name = StemOf(items[i].path);
            const std::wstring text = L"Folder: " + name + L" (item " + std::to_wstring(i + 1) + L")";
            SendMessageW(pick, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            if (item_ < 0) item_ = static_cast<int>(i);
            if (static_cast<int>(i) == item_) SetComboSel(kItemPick, static_cast<int>(picks_.size()));
            picks_.push_back(static_cast<int>(i));
        }
        const DockItemEntry* e = Entry();
        Enable(kItemPick, !picks_.empty());
        Enable(kItemViewCombo, e != nullptr);
        Enable(kItemChooseIcon, e != nullptr);
        Enable(kItemChooseImage, e != nullptr);
        Enable(kItemResetIcon, e && !e->icon.empty());
        SetComboSel(kItemViewCombo, e ? static_cast<int>(e->view) : -1);
        SetText(kItemNote, e ? std::wstring(L"Path: ") + e->path
                             : L"The dock " + Cfg().Edit().id + L" has no folder. Add one on Dock contents (Add folder…).");
    }

    void OnCommand(int id, int code, HWND) override {
        if (id == kItemPick && code == CBN_SELCHANGE) {
            const int sel = ComboSel(kItemPick);
            if (sel >= 0 && static_cast<size_t>(sel) < picks_.size()) item_ = picks_[static_cast<size_t>(sel)];
            Window().Refresh();
            return;
        }
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
            default:
                break;
        }
    }

private:
    // A folder item, or an app item that resolved to a folder (a shortcut to one).
    bool Editable(const DockItemEntry& e, int row) {
        if (e.Kind() == ItemKind::Folder) return true;
        if (e.Kind() != ItemKind::App || !Host().pins) return false;
        const std::vector<size_t> pinIdx = Cfg().Edit().PinIndexes();
        const auto& pins = Host().pins();
        for (size_t k = 0; k < pinIdx.size() && k < pins.size(); ++k) {
            if (static_cast<int>(pinIdx[k]) == row) return pins[k].kind == AppIdentity::PinKind::Folder;
        }
        return false;
    }

    DockItemEntry* Entry() {
        auto& items = Cfg().Edit().items;
        if (item_ < 0 || static_cast<size_t>(item_) >= items.size() || !Editable(items[static_cast<size_t>(item_)], item_)) {
            return nullptr;
        }
        return &items[static_cast<size_t>(item_)];
    }

    void ChooseIcon(bool image) {
        const DockItemEntry* e = Entry();
        if (!e) return;
        const DockItemEntry before = *e;
        std::wstring file, startImage;
        int index = 0;
        const std::vector<size_t> pinIdx = Cfg().Edit().PinIndexes();
        if (Host().pins) {
            const auto& pins = Host().pins();
            for (size_t k = 0; k < pinIdx.size() && k < pins.size(); ++k) {
                if (static_cast<int>(pinIdx[k]) != item_) continue;
                IconUtil::StartIconFor(pins[k], file, index);
                startImage = IconUtil::StartImageFor(pins[k]);
            }
        }
        std::wstring spec;
        const bool ok = image ? IconUtil::ChooseIconFile(Owner(), startImage, spec)
                              : IconUtil::ChooseIconResource(Owner(), file, index, spec);
        if (!ok) return;
        // The list may have changed while the dialog was open: only the same entry gets the icon.
        DockItemEntry* now = Entry();
        if (!now || now->path != before.path || now->kind != before.kind) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        now->icon = spec;
        Changed(SettingsScope::Pins);
        Window().Refresh();
    }

    int item_ = -1;          // index into the edited dock's items (-1: none)
    std::vector<int> picks_; // the pick list's rows -> item indexes
};

}  // namespace

std::unique_ptr<SettingsPage> MakeItemSettingsPage(SettingsWindow& window) {
    return std::make_unique<ItemSettingsPage>(window);
}
