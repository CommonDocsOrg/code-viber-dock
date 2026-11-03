// The Widgets page (installed widgets, Console, export) and the Widget settings page (one widget
// item's settings, generated from its manifest's schema).

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

#include "ContextMenu.h"
#include "JsonUtil.h"
#include "SettingsForm.h"
#include "SettingsIds.h"
#include "WidgetPackage.h"

namespace {

using namespace SettingsIds;
namespace json = winrt::Windows::Data::Json;

constexpr int kListH = 220;
constexpr int kButtonW = 150;
constexpr int kButtonStep = 158;

// --- Widgets -------------------------------------------------------------------------------------

class WidgetsPage : public FormPage {
public:
    explicit WidgetsPage(SettingsWindow& w) : FormPage(w, L"Widgets", L'\xEA86') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Widgets");
        Add(kWidgetsList, CtlKind::List, WC_LISTVIEWW, L"Installed widgets",
            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
            RECT{kMargin, y_, right, y_ + kListH});
        NextRow(kListH + 8);
        Button(kWidgetsAddToDock, L"Add to dock", kMargin, kButtonW);
        Button(kWidgetsAddFolder, L"Add from folder…", kMargin + kButtonStep, kButtonW);
        Button(kWidgetsExport, L"Export built-in to folder", kMargin + 2 * kButtonStep, kButtonW);
        NextRow(kRowH + 4);
        Button(kWidgetsReload, L"Reload widgets", kMargin, kButtonW);
        Button(kWidgetsOpenFolder, L"Open widgets folder", kMargin + kButtonStep, kButtonW);
        Button(kWidgetsConsole, L"Console…", kMargin + 2 * kButtonStep, kButtonW);
        NextRow(kRowH + 4);
        Text(kWidgetsStatus, CtlKind::Value, L"", kMargin, right - kMargin);
        NextRow(kRowH);
        Text(kWidgetsNote, CtlKind::Note,
             L"A widget is a folder in " + Widgets::Directory() +
                 L" with a widget.json and a main.js (see docs/widgets.md). A folder with a built-in's id replaces "
                 L"the built-in: export one to edit it. Each widget item's settings are on the Widget settings page.",
             kMargin, right - kMargin, 52);
        NextRow(52);
    }

    void Created() override {
        HWND list = Item(kWidgetsList);
        if (!list) return;
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        const wchar_t* names[] = {L"Name", L"Id", L"Source", L"Version"};
        for (int i = 0; i < 4; ++i) {
            col.pszText = const_cast<wchar_t*>(names[i]);
            col.cx = 100;
            col.iSubItem = i;
            ListView_InsertColumn(list, i, &col);
        }
    }

    void OnStyleChanged() override {
        HWND list = Item(kWidgetsList);
        if (!list) return;
        const Ctl* c = Find(kWidgetsList);
        const int total = Window().Px(c->dip.right - c->dip.left) - GetSystemMetricsForDpi(SM_CXVSCROLL, Window().Dpi()) - 4;
        const int widths[] = {Window().Px(170), Window().Px(130), Window().Px(110)};
        for (int i = 0; i < 3; ++i) ListView_SetColumnWidth(list, i, widths[i]);
        ListView_SetColumnWidth(list, 3, std::max(Window().Px(60), total - widths[0] - widths[1] - widths[2]));
    }

    void Load() override { Fill(); }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) == kWidgetsList && nm.code == LVN_ITEMCHANGED) UpdateButtons();
        return 0;
    }

    void OnCommand(int id, int code, HWND) override {
        if (code != BN_CLICKED) return;
        const Widgets::CatalogEntry* sel = Selected();
        switch (id) {
            case kWidgetsAddToDock: {
                if (!sel) return;
                const std::wstring wid = sel->id, name = sel->name;  // Refresh refills the catalog
                Cfg().Edit().AddScriptWidget(wid);
                Changed(SettingsScope::Pins);
                Window().Refresh();
                SetText(kWidgetsStatus, L"Added " + name + L" to the dock " + Cfg().Edit().id + L".");
                break;
            }
            case kWidgetsAddFolder:
                AddFromFolder();
                break;
            case kWidgetsExport: {
                if (!sel || !sel->builtIn) return;
                std::wstring folder, error;
                const std::wstring name = sel->name, wid = sel->id;
                if (Widgets::ExportBuiltIn(wid, folder, error)) {
                    Host().command(ContextMenu::kReloadWidgets);  // the folder copy replaces the built-in
                    Fill(wid);
                    SetText(kWidgetsStatus, L"Exported " + name + L" to " + folder + L".");
                } else {
                    SetText(kWidgetsStatus, L"Not exported: " + error);
                    MessageBeep(MB_ICONWARNING);
                }
                break;
            }
            case kWidgetsReload:
                Host().command(ContextMenu::kReloadWidgets);
                SetText(kWidgetsStatus, L"Every widget was reloaded.");
                break;
            case kWidgetsOpenFolder: {
                const std::wstring dir = Widgets::Directory();
                CreateDirectoryW(dir.c_str(), nullptr);
                ShellExecuteW(Owner(), L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            }
            case kWidgetsConsole:
                Host().command(ContextMenu::kWidgetConsole);
                break;
            default:
                break;
        }
    }

private:
    const Widgets::CatalogEntry* Selected() const {
        HWND list = Item(kWidgetsList);
        const int sel = list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
        return sel >= 0 && sel < static_cast<int>(catalog_.size()) ? &catalog_[static_cast<size_t>(sel)] : nullptr;
    }

    void UpdateButtons() {
        const Widgets::CatalogEntry* sel = Selected();
        Enable(kWidgetsAddToDock, sel != nullptr);
        Enable(kWidgetsExport, sel && sel->builtIn && sel->source == Widgets::Package::Source::BuiltIn);
        if (HWND f = GetFocus(); f && IsChild(Hwnd(), f) && !IsWindowEnabled(f)) SetFocus(Item(kWidgetsList));
    }

    // Lists the catalog; selects `select` (an id) or keeps the selection.
    void Fill(const std::wstring& select = {}) {
        HWND list = Item(kWidgetsList);
        if (!list) return;
        const Widgets::CatalogEntry* old = Selected();
        const std::wstring keep = !select.empty() ? select : old ? old->id : std::wstring();
        catalog_ = Widgets::Catalog();
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        int sel = catalog_.empty() ? -1 : 0;
        for (size_t i = 0; i < catalog_.size(); ++i) {
            const Widgets::CatalogEntry& e = catalog_[i];
            std::wstring error, version;
            if (const auto pkg = Widgets::Load(e.id, error)) version = pkg->manifest.version;
            std::wstring source = e.source == Widgets::Package::Source::Folder ? (e.builtIn ? L"folder (replaces built-in)" : L"folder")
                                                                             : L"built-in";
            std::wstring name = e.name, wid = e.id;
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = name.data();
            const int at = ListView_InsertItem(list, &item);
            if (at >= 0) {
                ListView_SetItemText(list, at, 1, wid.data());
                ListView_SetItemText(list, at, 2, source.data());
                ListView_SetItemText(list, at, 3, version.data());
            }
            if (e.id == keep) sel = static_cast<int>(i);
        }
        if (sel >= 0) ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        UpdateButtons();
    }

    void AddFromFolder() {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) {
            MessageBeep(MB_ICONERROR);
            return;
        }
        dialog->SetTitle(L"Add a widget folder (it must contain a widget.json)");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        if (FAILED(dialog->Show(Owner()))) return;
        winrt::com_ptr<IShellItem> item;
        PWSTR raw = nullptr;
        if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return;
        const std::wstring folder = raw;
        CoTaskMemFree(raw);
        std::wstring wid, error;
        if (Widgets::InstallFolder(folder, wid, error)) {
            Fill(wid);
            SetText(kWidgetsStatus, L"Installed " + wid + L" (" + Widgets::Directory() + L"\\" + wid + L").");
        } else {
            SetText(kWidgetsStatus, L"Not added: " + error);
            MessageBeep(MB_ICONWARNING);
        }
    }

    std::vector<Widgets::CatalogEntry> catalog_;
};

// --- Widget settings -----------------------------------------------------------------------------
// The controls are generated from the selected item's widget.json "settings" schema; the page
// builds its table again (SettingsWindow::RequestRebuild) when another item is picked or the
// item's widget changes. Values are written to the item's `settings` object (only the keys the
// user changed) and applied at once (SettingsScope::Pins: the dock re-reads the item, the widget
// gets onSettingsChanged).

class WidgetSettingsPage : public FormPage {
public:
    explicit WidgetSettingsPage(SettingsWindow& w) : FormPage(w, L"Widget settings", L'\xE713') {}

    void SelectItem(int item) override { item_ = item; }

protected:
    void Build() override {
        const int right = Width() - kMargin;
        ResolveItem();
        builtFor_ = Signature();
        std::wstring error;
        pkg_ = item_ >= 0 ? Widgets::Load(Cfg().Edit().items[static_cast<size_t>(item_)].id, error) : nullptr;
        Title(pkg_ ? (L"Widget: " + pkg_->manifest.name).c_str() : L"Widget settings");
        Label(kWidgetPickLabel, L"Widget");
        Combo(kWidgetPick, kColX, right - kColX, false);
        NextRow();
        std::wstring note;
        if (item_ < 0) {
            note = L"The dock " + Cfg().Edit().id + L" has no widget. Add one on Dock contents (Add widget) or Widgets.";
        } else if (!pkg_) {
            note = L"Not installed: " + error;
        } else if (pkg_->manifest.settings.empty()) {
            note = pkg_->manifest.name + L" has no settings.";
        } else {
            note = pkg_->manifest.description;
        }
        if (!note.empty()) {
            Text(kWidgetSettingsNote, CtlKind::Note, note, kMargin, right - kMargin, 36);
            NextRow(36);
        }
        if (!pkg_) return;
        const auto& fields = pkg_->manifest.settings;
        for (size_t k = 0; k < fields.size() && k < static_cast<size_t>(kWidgetFieldMax); ++k) {
            const Widgets::SettingField& f = fields[k];
            const int base = kWidgetFieldBase + 10 * static_cast<int>(k);
            using T = Widgets::SettingField::Type;
            switch (f.type) {
                case T::Bool:
                    Check(base + 1, base + 2, f.label.c_str());
                    NextRow();
                    break;
                case T::Int: {
                    Label(base, f.label.c_str());
                    const int lo = static_cast<int>(std::max(-1e9, f.min)), hi = static_cast<int>(std::min(1e9, f.max));
                    Spin(base + 1, base + 2, kColX, 120, lo, hi, std::max(1, static_cast<int>(f.step)));
                    NextRow();
                    break;
                }
                case T::Choice:
                    Label(base, f.label.c_str());
                    Combo(base + 1, kColX, right - kColX, false);
                    NextRow();
                    break;
                case T::Color:
                case T::Number:
                    Label(base, f.label.c_str());
                    EditBox(base + 1, kColX, 160, 32);
                    NextRow();
                    break;
                case T::Font:
                case T::String:
                default:
                    Label(base, f.label.c_str());
                    EditBox(base + 1, kColX, right - kColX, 1024);
                    NextRow();
                    break;
            }
            if (!f.description.empty()) {
                Text(base + 3, CtlKind::Note, f.description, kColX, right - kColX, 20);
                NextRow(22);
            }
        }
        if (!fields.empty()) {
            NextRow(6);
            Button(kWidgetReset, L"Reset to defaults", kMargin, 150);
            NextRow();
        }
    }

    void Created() override {
        if (!pkg_) return;
        const auto& fields = pkg_->manifest.settings;
        for (size_t k = 0; k < fields.size() && k < static_cast<size_t>(kWidgetFieldMax); ++k) {
            if (fields[k].type != Widgets::SettingField::Type::Choice) continue;
            for (const auto& c : fields[k].choices) ComboAdd(kWidgetFieldBase + 10 * static_cast<int>(k) + 1, c.second.c_str());
        }
    }

    void Load() override {
        // The dock's widget items may have changed (another dock picked, items added / removed).
        if (Signature() != builtFor_) {
            Window().RequestRebuild(*this);
            return;
        }
        HWND pick = Item(kWidgetPick);
        SendMessageW(pick, CB_RESETCONTENT, 0, 0);
        picks_.clear();
        const auto& items = Cfg().Edit().items;
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].Kind() != ItemKind::Widget) continue;
            const std::wstring text = Widgets::DisplayName(items[i].id) + L" (item " + std::to_wstring(i + 1) + L")";
            SendMessageW(pick, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            if (static_cast<int>(i) == item_) SetComboSel(kWidgetPick, static_cast<int>(picks_.size()));
            picks_.push_back(static_cast<int>(i));
        }
        Enable(kWidgetPick, !picks_.empty());
        if (!pkg_ || item_ < 0) return;
        json::JsonObject values{nullptr};
        try {
            json::JsonObject::TryParse(JsonUtil::Utf8ToWide(Widgets::MergeSettings(pkg_->manifest, Entry().settings)), values);
        } catch (...) {
        }
        if (!values) return;
        const auto& fields = pkg_->manifest.settings;
        for (size_t k = 0; k < fields.size() && k < static_cast<size_t>(kWidgetFieldMax); ++k) {
            const Widgets::SettingField& f = fields[k];
            const int base = kWidgetFieldBase + 10 * static_cast<int>(k);
            const json::IJsonValue v = JsonUtil::Find(values, f.key.c_str());
            if (!v) continue;
            using T = Widgets::SettingField::Type;
            switch (f.type) {
                case T::Bool:
                    SetCheck(base + 1, v.ValueType() == json::JsonValueType::Boolean && v.GetBoolean());
                    break;
                case T::Int:
                    if (v.ValueType() == json::JsonValueType::Number) SetSpin(base + 2, static_cast<int>(std::lround(v.GetNumber())));
                    break;
                case T::Number:
                    if (v.ValueType() == json::JsonValueType::Number) SetText(base + 1, JsonUtil::Num(v.GetNumber()));
                    break;
                case T::Choice: {
                    if (v.ValueType() != json::JsonValueType::String) break;
                    const std::wstring s(v.GetString());
                    for (size_t c = 0; c < f.choices.size(); ++c) {
                        if (f.choices[c].first == s) SetComboSel(base + 1, static_cast<int>(c));
                    }
                    break;
                }
                default:
                    if (v.ValueType() == json::JsonValueType::String) SetText(base + 1, std::wstring(v.GetString()));
                    break;
            }
        }
    }

    void OnCommand(int id, int code, HWND) override {
        if (id == kWidgetPick && code == CBN_SELCHANGE) {
            const int sel = ComboSel(kWidgetPick);
            if (sel >= 0 && sel < static_cast<int>(picks_.size()) && picks_[static_cast<size_t>(sel)] != item_) {
                item_ = picks_[static_cast<size_t>(sel)];
                Window().RequestRebuild(*this);
            }
            return;
        }
        if (id == kWidgetReset && code == BN_CLICKED) {
            if (item_ < 0) return;
            Entry().settings.clear();
            Changed(SettingsScope::Pins);
            Load();
            return;
        }
        const Widgets::SettingField* f = FieldOf(id);
        if (!f) return;
        using T = Widgets::SettingField::Type;
        if (f->type == T::Bool && code == BN_CLICKED) {
            Store(*f, json::JsonValue::CreateBooleanValue(GetCheck(id)));
        } else if (f->type == T::Choice && code == CBN_SELCHANGE) {
            const int sel = ComboSel(id);
            if (sel >= 0 && sel < static_cast<int>(f->choices.size())) {
                Store(*f, json::JsonValue::CreateStringValue(f->choices[static_cast<size_t>(sel)].first));
            }
        }
    }

    void Spun(int spin, int pos) override {
        const Widgets::SettingField* f = FieldOf(spin - 1);
        if (f && f->type == Widgets::SettingField::Type::Int) Store(*f, json::JsonValue::CreateNumberValue(pos));
    }

    void Commit(int id) override {
        const Widgets::SettingField* f = FieldOf(id);
        if (!f) return;
        const std::wstring text = GetText(id);
        using T = Widgets::SettingField::Type;
        switch (f->type) {
            case T::Int: {
                int v = 0;
                if (ReadInt(id, static_cast<int>(std::max(-1e9, f->min)), static_cast<int>(std::min(1e9, f->max)), v)) {
                    Store(*f, json::JsonValue::CreateNumberValue(v));
                } else {
                    Load();
                }
                break;
            }
            case T::Number: {
                wchar_t* end = nullptr;
                const double d = std::wcstod(text.c_str(), &end);
                if (end == text.c_str() || !std::isfinite(d)) {
                    Load();
                    break;
                }
                Store(*f, json::JsonValue::CreateNumberValue(std::clamp(d, f->min, f->max)));
                break;
            }
            case T::Color: {
                uint32_t argb = 0;
                if (!ParseArgbColor(text, argb)) {
                    MessageBeep(MB_ICONWARNING);
                    Load();
                    break;
                }
                Store(*f, json::JsonValue::CreateStringValue(FormatArgbColor(argb)));
                break;
            }
            default:
                Store(*f, json::JsonValue::CreateStringValue(text));
                break;
        }
    }

private:
    DockItemEntry& Entry() { return Cfg().Edit().items[static_cast<size_t>(item_)]; }

    // item_ names a widget item of the edited dock (else the first one, or -1).
    void ResolveItem() {
        const auto& items = Cfg().Edit().items;
        if (item_ >= 0 && item_ < static_cast<int>(items.size()) && items[static_cast<size_t>(item_)].Kind() == ItemKind::Widget) {
            return;
        }
        item_ = -1;
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].Kind() == ItemKind::Widget) {
                item_ = static_cast<int>(i);
                return;
            }
        }
    }

    // What the table was generated for: the dock, the widget items and the chosen one's id.
    std::wstring Signature() {
        ResolveItem();
        std::wstring s = std::to_wstring(Cfg().editDock) + L"|" + std::to_wstring(item_);
        for (const DockItemEntry& e : Cfg().Edit().items) {
            if (e.Kind() == ItemKind::Widget) s += L"|" + e.id;
        }
        return s;
    }

    const Widgets::SettingField* FieldOf(int id) const {
        if (!pkg_ || id < kWidgetFieldBase) return nullptr;
        const int k = (id - kWidgetFieldBase) / 10, part = (id - kWidgetFieldBase) % 10;
        if (part != 1 || k >= static_cast<int>(pkg_->manifest.settings.size())) return nullptr;
        return &pkg_->manifest.settings[static_cast<size_t>(k)];
    }

    // Writes one value into the item's settings object and applies it.
    void Store(const Widgets::SettingField& f, const json::IJsonValue& value) {
        if (item_ < 0 || Loading()) return;
        DockItemEntry& e = Entry();
        json::JsonObject o{nullptr};
        try {
            if (e.settings.empty() || !json::JsonObject::TryParse(e.settings, o) || !o) o = json::JsonObject();
            o.SetNamedValue(f.key, value);
            const std::wstring text(o.Stringify());
            if (text == e.settings) return;
            e.settings = text;
        } catch (...) {
            return;
        }
        Changed(SettingsScope::Pins);  // the dock re-reads the item: onSettingsChanged
    }

    int item_ = -1;
    std::wstring builtFor_;
    std::shared_ptr<const Widgets::Package> pkg_;
    std::vector<int> picks_;  // combo rows -> item indexes
};

}  // namespace

std::unique_ptr<SettingsPage> MakeWidgetsPage(SettingsWindow& window) { return std::make_unique<WidgetsPage>(window); }

std::unique_ptr<SettingsPage> MakeWidgetSettingsPage(SettingsWindow& window) {
    return std::make_unique<WidgetSettingsPage>(window);
}
