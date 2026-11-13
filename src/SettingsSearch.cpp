// Round 9 phase 9B: the settings search dialog (see SettingsSearch.h).

#include "SettingsSearch.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <map>
#include <memory>
#include <utility>

#include "Clock.h"
#include "JsonUtil.h"
#include "SettingsIds.h"
#include "WidgetPackage.h"
#include "resource.h"

namespace {

using namespace SettingsIds;
namespace json = winrt::Windows::Data::Json;

constexpr UINT kHideComboMessage = WM_APP + 42;  // posted: the in-place drop-down closes (after its own notification)

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::iswspace(s[i])) ++i;
    return s.substr(i);
}

std::vector<std::wstring> Words(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (std::iswspace(c)) {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::wstring TextOf(HWND h) {
    const int n = GetWindowTextLengthW(h);
    if (n <= 0) return {};
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    s.resize(static_cast<size_t>(GetWindowTextW(h, s.data(), n + 1)));
    return s;
}

// The generated page's visibleIf (SettingsPagesWidgets.cpp): every named field has that value now
// (round 23 phase 23A: an array, any of its values).
bool FieldVisible(const Widgets::SettingField& f, const json::JsonObject& values) {
    if (f.visibleIf.empty() || !values) return true;
    for (const auto& [key, want] : f.visibleIf) {
        const json::IJsonValue v = JsonUtil::Find(values, key.c_str());
        if (!v) return false;
        try {
            const std::wstring have(v.Stringify());
            const json::JsonValue w = json::JsonValue::Parse(want);
            if (w.ValueType() == json::JsonValueType::Array) {
                bool any = false;
                for (const json::IJsonValue& x : w.GetArray()) any = any || std::wstring(x.Stringify()) == have;
                if (!any) return false;
            } else if (have != std::wstring(w.Stringify())) {
                return false;
            }
        } catch (...) {
            return false;
        }
    }
    return true;
}

std::wstring CoordText(double v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.2f", v);
    return buf;
}

}  // namespace

std::wstring SettingsSearch::Row::Key() const {
    return std::to_wstring(static_cast<int>(kind)) + L"|" + dock + L"|" + std::to_wstring(item) + L"|" + std::to_wstring(page) + L"|" +
           std::to_wstring(id);
}

SettingsSearch::SettingsSearch(SettingsSearchHost host, SettingsWindow* owner) : host_(std::move(host)), owner_(owner) {}

SettingsSearch::~SettingsSearch() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (font_) DeleteObject(font_);
    if (layerBrush_) DeleteObject(layerBrush_);
    if (controlBrush_) DeleteObject(controlBrush_);
}

bool SettingsSearch::Create(HINSTANCE instance, const std::wstring& text) {
    instance_ = instance;
    HWND owner = owner_ ? owner_->Hwnd() : nullptr;
    if (!owner) return false;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    dpi_ = GetDpiForWindow(owner);
    if (dpi_ == 0) dpi_ = USER_DEFAULT_SCREEN_DPI;
    constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN;
    auto rectFor = [&](UINT dpi) {
        RECT r{0, 0, MulDiv(kWidthDip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
               MulDiv(kHeightDip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)};
        AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_CONTROLPARENT, dpi);
        // Centred over its owner, inside the owner's work area.
        RECT o{};
        GetWindowRect(owner, &o);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
        const int w = r.right - r.left, h = r.bottom - r.top;
        int x = (o.left + o.right - w) / 2, y = (o.top + o.bottom - h) / 2;
        x = std::max(static_cast<int>(mi.rcWork.left), std::min(x, static_cast<int>(mi.rcWork.right) - w));
        y = std::max(static_cast<int>(mi.rcWork.top), std::min(y, static_cast<int>(mi.rcWork.bottom) - h));
        return RECT{x, y, x + w, y + h};
    };
    RECT r = rectFor(dpi_);
    CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, L"Find a setting", style, r.left, r.top, r.right - r.left, r.bottom - r.top, owner,
                    nullptr, instance, this);
    if (!hwnd_) return false;
    if (const UINT actual = GetDpiForWindow(hwnd_); actual && actual != dpi_) {
        dpi_ = actual;
        r = rectFor(dpi_);
        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    auto make = [&](const wchar_t* cls, const wchar_t* t, DWORD s, DWORD ex, int id) {
        return CreateWindowExW(ex, cls, t, WS_CHILD | s, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
                               nullptr);
    };
    // The tab order is the creation order: the box, the list, Go to.
    edit_ = make(WC_EDITW, L"", WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, kSearchEdit);
    list_ = make(WC_LISTVIEWW, L"Settings",
                 WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                     LVS_NOSORTHEADER,
                 0, kSearchList);
    go_ = make(WC_BUTTONW, L"Go to", WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, kSearchGo);
    status_ = make(WC_STATICW, L"", WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS | SS_CENTERIMAGE, 0, kSearchStatus);
    combo_ = make(WC_COMBOBOXW, L"Value", WS_VSCROLL | CBS_DROPDOWNLIST, 0, kSearchCombo);
    if (!edit_ || !list_ || !go_ || !status_ || !combo_) {
        DestroyWindow(hwnd_);
        return false;
    }
    SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Find a setting: name, label or value"));
    SendMessageW(edit_, EM_SETLIMITTEXT, 200, 0);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP | LVS_EX_LABELTIP);
    SetWindowSubclass(list_, ListProc, 1, reinterpret_cast<DWORD_PTR>(this));
    const wchar_t* names[] = {L"Setting", L"Where", L"Value"};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<wchar_t*>(names[i]);
        col.cx = Px(150);
        col.iSubItem = i;
        ListView_InsertColumn(list_, i, &col);
    }
    CreateFonts();
    ApplyTheme();
    Layout();
    if (!text.empty()) SetWindowTextW(edit_, text.c_str());  // EN_CHANGE filters (no rows yet: nothing)
    const int n = GetWindowTextLengthW(edit_);
    SendMessageW(edit_, EM_SETSEL, static_cast<WPARAM>(n), n);
    lastFocus_ = edit_;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    SetFocus(edit_);
    // Round 18 phase 18D: the index is built after the dialog is on screen, one window per timer
    // tick (until 18D it was built here, before ShowWindow: with a dozen docks[] entries the
    // hidden settings windows took seconds to create and the dialog appeared only then). The first
    // paint before the first step, so the dialog never shows unpainted.
    UpdateWindow(hwnd_);
    Rebuild();
    return true;
}

void SettingsSearch::Activate(const std::wstring& text) {
    if (!hwnd_) return;
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    if (!text.empty()) {
        SetWindowTextW(edit_, text.c_str());
        const int n = GetWindowTextLengthW(edit_);
        SendMessageW(edit_, EM_SETSEL, static_cast<WPARAM>(n), n);
        Filter();
    }
    SetFocus(edit_);
}

// --- the index -----------------------------------------------------------------------------------

void SettingsSearch::Rebuild() {
    rebuildPosted_ = false;
    if (!hwnd_) return;
    if (!indexing_) {
        // The selection survives a rebuild (a restarted build keeps the key it had).
        const int sel = Selected();
        indexKeep_ = sel >= 0 ? rows_[shown_[static_cast<size_t>(sel)]].Key() : std::wstring();
        indexPartial_ = rows_.empty();  // the first build fills the list as it goes
    }
    indexing_ = true;
    indexStep_ = 0;
    indexCount_ = host_.windowCount ? host_.windowCount() : 0;
    indexRows_.clear();
    UpdateStatus();
    SetTimer(hwnd_, kIndexTimerId, USER_TIMER_MINIMUM, nullptr);
}

void SettingsSearch::IndexStep() {
    if (!hwnd_ || !indexing_) return;
    if (indexStep_ < indexCount_) {
        // One window per message: creating a hidden settings window (every page, hundreds of
        // controls) is the slow part, and between two of them the dialog paints and takes keys.
        const size_t i = indexStep_++;
        if (SettingsWindow* w = host_.window ? host_.window(i) : nullptr; w && w->Hwnd()) IndexWindow(*w, i, indexRows_);
        if (indexPartial_) {
            // The list grows under the user; a row they selected meanwhile stays selected.
            const int sel = Selected();
            const std::wstring keep = sel > 0 ? rows_[shown_[static_cast<size_t>(sel)]].Key() : std::wstring();
            rows_ = indexRows_;
            Filter();
            for (size_t k = 0; !keep.empty() && k < shown_.size(); ++k) {
                if (rows_[shown_[k]].Key() == keep) {
                    SetSelected(static_cast<int>(k));
                    break;
                }
            }
        } else {
            UpdateStatus();
        }
        if (hwnd_) SetTimer(hwnd_, kIndexTimerId, USER_TIMER_MINIMUM, nullptr);
        return;
    }
    FinishIndex();
}

void SettingsSearch::FinishIndex() {
    IndexItems(indexRows_);
    rows_ = std::move(indexRows_);
    indexRows_.clear();
    indexing_ = false;
    indexPartial_ = false;
    Filter();
    if (!indexKeep_.empty()) {
        for (size_t k = 0; k < shown_.size(); ++k) {
            if (rows_[shown_[k]].Key() == indexKeep_) {
                SetSelected(static_cast<int>(k));
                break;
            }
        }
    }
    indexKeep_.clear();
}

void SettingsSearch::IndexWindow(SettingsWindow& w, size_t number, std::vector<Row>& rows) {
    w.EnsureFresh();  // a hidden window's controls catch up with the config first (18D)
    const std::wstring name = w.SearchName();
    for (SettingsWindow::SearchEntry& e : w.SearchEntries()) {
        Row r;
        r.kind = w.Kind();
        r.window = number;
        r.dock = w.Kind() == SettingsKind::Global ? std::wstring() : w.DockId();
        r.item = w.Kind() == SettingsKind::Item ? w.ItemIndex() : SIZE_MAX;
        r.page = e.page;
        r.id = e.id;
        r.ctl = e.kind;
        r.setting = std::move(e.setting);
        r.value = std::move(e.value);
        r.tip = std::move(e.tip);
        r.enabled = e.enabled;
        r.choices = std::move(e.choices);
        r.choice = e.choice;
        const std::wstring page = w.PageName(e.page);
        r.where = name + L" \x203A " + page + (e.card.empty() || e.card == page ? std::wstring() : L" \x203A " + e.card);
        r.label = Lower(r.setting);
        r.hay = Lower(r.setting + L"\n" + r.where + L"\n" + r.value + L"\n" + e.extra + L"\n" + r.tip);
        rows.push_back(std::move(r));
    }
}

void SettingsSearch::IndexItems(std::vector<Row>& rows) {
    if (!host_.config) return;
    const Config& cfg = *host_.config;
    std::map<std::wstring, std::shared_ptr<const Widgets::Package>> packages;
    std::vector<Widgets::SettingField> clockSchema, weatherSchema;
    bool clockRead = false, weatherRead = false, zonesRead = false;
    std::vector<Clock::TimeZoneInfo> zones;
    for (const DockConfig& dock : cfg.docks) {
        // A standalone widget's settings are its Dock window's Widget tab (indexed with the window).
        if (dock.IsWidgetWindow() || dock.IsFolderWindow()) continue;
        for (size_t k = 0; k < dock.items.size(); ++k) {
            const DockItemEntry& e = dock.items[k];
            const ItemKind kind = e.Kind();
            Widgets::Manifest builtIn;
            const Widgets::Manifest* m = nullptr;
            std::wstring name, text;
            if (kind == ItemKind::Clock) {
                if (!clockRead) clockSchema = Widgets::BuiltInSchema(ItemKind::Clock);
                clockRead = true;
                builtIn.name = L"Clock";
                builtIn.settings = clockSchema;
                m = &builtIn;
                const ClockConfig c = ClockOf(cfg, e);
                name = c.label.empty() ? std::wstring(L"Clock") : L"Clock \x00B7 " + c.label;
                text = ClockSettingsJson(c);
            } else if (kind == ItemKind::Weather) {
                if (!weatherRead) weatherSchema = Widgets::BuiltInSchema(ItemKind::Weather);
                weatherRead = true;
                builtIn.name = L"Weather";
                builtIn.settings = weatherSchema;
                m = &builtIn;
                const WeatherConfig w = WeatherOf(cfg, e);
                const std::wstring place = !w.label.empty() ? w.label : w.location;
                name = place.empty() ? std::wstring(L"Weather") : L"Weather \x00B7 " + place;
                text = WeatherSettingsJson(w);
            } else if (kind == ItemKind::Widget) {
                auto it = packages.find(e.id);
                if (it == packages.end()) {
                    std::wstring error;
                    it = packages.emplace(e.id, Widgets::Load(e.id, error)).first;
                }
                if (!it->second) continue;
                m = &it->second->manifest;
                name = m->name.empty() ? e.id : m->name;
                text = JsonUtil::Utf8ToWide(Widgets::MergeSettings(*m, e.settings));
            } else {
                continue;  // a folder / group / tray item's page is not generated: not indexed
            }
            json::JsonObject values{nullptr};
            try {
                if (!json::JsonObject::TryParse(text, values)) values = nullptr;
            } catch (...) {
                values = nullptr;
            }
            const std::wstring where = name + L" on " + dock.id;
            const std::wstring untitled = m->name.empty() ? std::wstring(L"Settings") : m->name;
            const auto& fields = m->settings;
            // Round 10 phase 10C: a schema with both sections is two tabs (MakeWidgetSettingsPages):
            // an Appearance field is on the second, and the Where column names the tab.
            const bool tabs = Widgets::SectionMask(fields, static_cast<size_t>(kWidgetFieldMax)) == 3;
            for (size_t f = 0; f < fields.size() && f < static_cast<size_t>(kWidgetFieldMax); ++f) {
                const Widgets::SettingField& s = fields[f];
                if (!FieldVisible(s, values)) continue;  // not on the page now
                using T = Widgets::SettingField::Type;
                Row r;
                r.kind = SettingsKind::Item;
                r.dock = dock.id;
                r.item = k;
                const bool appearance = s.section == Widgets::SettingField::Section::Appearance;
                r.page = tabs && appearance ? kItemAppearancePage : kItemPage;
                r.id = s.type == T::Place ? kPlaceResults : kWidgetFieldBase + 10 * static_cast<int>(f) + 1;
                r.ctl = s.type == T::Bool                                                            ? CtlKind::Check
                        : s.type == T::Choice || s.type == T::TimeZone || s.type == T::Place ? CtlKind::Combo
                                                                                               : CtlKind::Edit;
                r.setting = s.label.empty() ? s.key : s.label;
                r.where = where + L" \x203A " + (tabs ? std::wstring(Widgets::SectionName(s.section)) + L" \x203A " : std::wstring()) +
                          (s.group.empty() ? untitled : s.group);
                r.tip = s.description;
                const json::IJsonValue v = values ? JsonUtil::Find(values, s.key.c_str()) : nullptr;
                const bool isString = v && v.ValueType() == json::JsonValueType::String;
                const bool isNumber = v && v.ValueType() == json::JsonValueType::Number;
                switch (s.type) {
                    case T::Bool:
                        r.value = v && v.ValueType() == json::JsonValueType::Boolean && v.GetBoolean() ? L"On" : L"Off";
                        break;
                    case T::Int:
                        if (isNumber) r.value = std::to_wstring(static_cast<long long>(std::llround(v.GetNumber())));
                        break;
                    case T::Number:
                        if (isNumber) r.value = JsonUtil::Num(v.GetNumber());
                        break;
                    case T::Choice: {
                        const std::wstring cur = isString ? std::wstring(v.GetString()) : std::wstring();
                        r.value = cur;
                        for (size_t c = 0; c < s.choices.size(); ++c) {
                            if (s.choices[c].first == cur) {
                                r.value = s.choices[c].second;
                                r.choice = static_cast<int>(c);
                            }
                        }
                        // In place only when the page lists exactly these rows (a perf metric list is the sampler's).
                        if (!s.ChoiceFromPerf()) {
                            for (const auto& c : s.choices) r.choices.push_back(c.second);
                        }
                        break;
                    }
                    case T::TimeZone: {
                        const std::wstring cur = isString ? std::wstring(v.GetString()) : std::wstring();
                        r.value = L"Local time";
                        if (!cur.empty()) {
                            if (!zonesRead) zones = Clock::TimeZones();
                            zonesRead = true;
                            r.value = cur;
                            for (const Clock::TimeZoneInfo& z : zones) {
                                if (_wcsicmp(z.key.c_str(), cur.c_str()) == 0) r.value = z.display;
                            }
                        }
                        break;
                    }
                    case T::Place: {
                        const WeatherConfig w = kind == ItemKind::Weather ? WeatherOf(cfg, e) : ScriptWeatherOf(cfg, dock, e);
                        r.value = !w.location.empty() ? w.location
                                  : w.Configured()    ? CoordText(w.latitude) + L", " + CoordText(w.longitude)
                                                      : std::wstring(L"None yet");
                        break;
                    }
                    default:
                        if (isString) r.value = v.GetString();
                        break;
                }
                if (!r.value.empty() && !s.unit.empty() && (s.type == T::Int || s.type == T::Number)) r.value += L" " + s.unit;
                r.label = Lower(r.setting);
                r.hay = Lower(r.setting + L"\n" + r.where + L"\n" + r.value + L"\n" + s.description + L"\n" + s.key + L"\n" + s.group);  // r.where carries the tab (10C)
                rows.push_back(std::move(r));
            }
        }
    }
}

void SettingsSearch::Filter() {
    if (comboRow_ >= 0) HideCombo(false);
    query_ = Lower(Trim(edit_ ? TextOf(edit_) : std::wstring()));
    const std::vector<std::wstring> words = Words(query_);
    // Ranked: the label starts with the query, then the label has it (or all its words), then the
    // rest that has every word somewhere (hint, tooltip, card, page, window, value, key).
    std::vector<std::pair<int, size_t>> ranked;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& r = rows_[i];
        int tier = 0;
        if (!words.empty()) {
            if (!std::all_of(words.begin(), words.end(), [&](const std::wstring& w) { return r.hay.find(w) != std::wstring::npos; })) continue;
            if (r.label.compare(0, query_.size(), query_) == 0) {
                tier = 0;
            } else if (r.label.find(query_) != std::wstring::npos ||
                       std::all_of(words.begin(), words.end(), [&](const std::wstring& w) { return r.label.find(w) != std::wstring::npos; })) {
                tier = 1;
            } else {
                tier = 2;
            }
        }
        ranked.emplace_back(tier, i);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    shown_.clear();
    for (const auto& [tier, i] : ranked) shown_.push_back(i);
    if (list_) {
        ListView_SetItemCountEx(list_, static_cast<int>(shown_.size()), 0);
        SetColumns();  // the scroll bar may have come or gone
        InvalidateRect(list_, nullptr, TRUE);
        SetSelected(shown_.empty() ? -1 : 0);
    }
    UpdateStatus();
}

void SettingsSearch::SetSelected(int row) {
    if (!list_) return;
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (row < 0 || row >= static_cast<int>(shown_.size())) return;
    ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list_, row, FALSE);
}

int SettingsSearch::Selected() const {
    if (!list_) return -1;
    const int sel = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    return sel >= 0 && sel < static_cast<int>(shown_.size()) ? sel : -1;
}

void SettingsSearch::UpdateStatus() {
    if (!status_) return;
    std::wstring s;
    const size_t total = rows_.size();
    if (indexing_) {
        // Round 18 phase 18D: the build in progress ("Indexing settings… 3 of 12 windows", with the
        // rows read so far once there are any; a rebuild keeps the old rows and counts them).
        const std::wstring progress = L"Indexing settings\x2026 " + std::to_wstring(std::min(indexStep_, indexCount_)) + L" of " +
                                      std::to_wstring(indexCount_) + L" windows";
        if (total == 0) {
            s = progress;
        } else if (query_.empty()) {
            s = std::to_wstring(total) + L" settings so far \x00B7 " + progress;
        } else {
            s = std::to_wstring(shown_.size()) + L" of " + std::to_wstring(total) + L" so far \x00B7 " + progress;
        }
    } else if (query_.empty()) {
        s = std::to_wstring(total) + (total == 1 ? L" setting" : L" settings");
    } else if (shown_.empty()) {
        s = L"No setting matches \x201C" + Trim(TextOf(edit_)) + L"\x201D (of " + std::to_wstring(total) + L")";
    } else {
        s = std::to_wstring(shown_.size()) + L" of " + std::to_wstring(total) + L" settings";
    }
    SetWindowTextW(status_, s.c_str());
}

// --- Go to and changes in place ---------------------------------------------------------------

void SettingsSearch::GoTo() {
    int sel = Selected();
    if (sel < 0 && !shown_.empty()) sel = 0;
    if (sel < 0) {
        MessageBeep(MB_OK);
        return;
    }
    const Row r = rows_[shown_[static_cast<size_t>(sel)]];  // a copy: opening a window may rebuild the index
    SettingsWindow* w = host_.open ? host_.open(r.kind, r.dock, r.item, r.page) : nullptr;
    RECT target{};
    if (!w || !w->Hwnd() || !w->SearchReveal(r.page, r.id, &target)) {
        MessageBeep(MB_OK);
        return;
    }
    if (hwnd_) MoveOff(target, w->Hwnd());
    if (hwnd_ && !rebuildPosted_) rebuildPosted_ = PostMessageW(hwnd_, kRebuildMessage, 0, 0) != FALSE;
}

bool SettingsSearch::Change(const Row& r, int choice) {
    SettingsWindow* w = nullptr;
    if (r.kind == SettingsKind::Item) {
        w = host_.itemWindow ? host_.itemWindow(r.dock, r.item) : nullptr;
    } else if (host_.window && host_.windowCount && r.window < host_.windowCount()) {
        // Its window by number, if it is still the one the row was read from (the docks may have
        // changed since: the posted rebuild follows).
        SettingsWindow* x = host_.window(r.window);
        if (x && x->Hwnd() && x->Kind() == r.kind && (r.kind == SettingsKind::Global || x->DockId() == r.dock)) w = x;
    }
    if (!w || !w->Hwnd()) return false;
    const bool ok = w->SearchChange(r.page, r.id, choice);
    // A window created hidden never takes the activation (a page's handler could focus a control).
    if (hwnd_ && !IsWindowVisible(w->Hwnd()) && GetForegroundWindow() == w->Hwnd()) SetForegroundWindow(hwnd_);
    if (hwnd_ && !rebuildPosted_) rebuildPosted_ = PostMessageW(hwnd_, kRebuildMessage, 0, 0) != FALSE;
    return ok;
}

void SettingsSearch::ChangeSelected() {
    const int sel = Selected();
    if (sel < 0) return;
    const Row& r = rows_[shown_[static_cast<size_t>(sel)]];
    if (r.ctl == CtlKind::Check || r.ctl == CtlKind::Radio) {
        if (!r.enabled || !Change(r, -1)) MessageBeep(MB_OK);
        return;
    }
    if (r.ctl == CtlKind::Combo && !r.choices.empty()) {
        if (!r.enabled) {
            MessageBeep(MB_OK);
            return;
        }
        ShowCombo(sel);
    }
    // Edits, up-downs and trackbars: Go to only.
}

void SettingsSearch::ShowCombo(int row) {
    const Row& r = rows_[shown_[static_cast<size_t>(row)]];
    ListView_EnsureVisible(list_, row, FALSE);
    RECT cell{};
    if (!ListView_GetSubItemRect(list_, row, 2, LVIR_BOUNDS, &cell)) return;
    RECT client{};
    GetClientRect(list_, &client);
    const int w = std::max(static_cast<int>(cell.right - cell.left), Px(180));
    cell.right = std::min(static_cast<int>(client.right), static_cast<int>(cell.left) + w);
    cell.left = std::max(0, static_cast<int>(cell.right) - w);
    MapWindowPoints(list_, hwnd_, reinterpret_cast<POINT*>(&cell), 2);
    SendMessageW(combo_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(combo_, CB_RESETCONTENT, 0, 0);
    for (const std::wstring& c : r.choices) SendMessageW(combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.c_str()));
    SendMessageW(combo_, CB_SETCURSEL, static_cast<WPARAM>(r.choice), 0);
    SendMessageW(combo_, WM_SETREDRAW, TRUE, 0);
    SendMessageW(combo_, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(Px(220)), 0);
    comboRow_ = row;
    SetWindowPos(combo_, HWND_TOP, cell.left, cell.top - Px(1), cell.right - cell.left, Px(260), SWP_SHOWWINDOW);
    SetFocus(combo_);
    SendMessageW(combo_, CB_SHOWDROPDOWN, TRUE, 0);
}

void SettingsSearch::HideCombo(bool focusList) {
    if (comboRow_ < 0) return;
    comboRow_ = -1;
    const bool hadFocus = GetFocus() == combo_;
    ShowWindow(combo_, SW_HIDE);
    if ((focusList || hadFocus) && list_) SetFocus(list_);
}

void SettingsSearch::ApplyChoice(int row, int choice) {
    if (row < 0 || row >= static_cast<int>(shown_.size()) || choice < 0) return;
    const Row r = rows_[shown_[static_cast<size_t>(row)]];
    if (choice == r.choice) return;
    if (!Change(r, choice)) MessageBeep(MB_OK);
}

void SettingsSearch::MoveOff(const RECT& target, HWND window) {
    RECT me{}, t = target, inter{};
    GetWindowRect(hwnd_, &me);
    InflateRect(&t, Px(12), Px(12));
    if (!IntersectRect(&inter, &me, &t)) return;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    RECT win{};
    GetWindowRect(window, &win);
    const int w = me.right - me.left, h = me.bottom - me.top, gap = Px(8);
    auto clampY = [&](int y) { return std::max(static_cast<int>(work.top), std::min(y, static_cast<int>(work.bottom) - h)); };
    auto clampX = [&](int x) { return std::max(static_cast<int>(work.left), std::min(x, static_cast<int>(work.right) - w)); };
    // Beside the settings window (right, left), else under / over the control.
    const POINT tries[] = {{win.right + gap, clampY(me.top)},
                           {win.left - gap - w, clampY(me.top)},
                           {clampX(me.left), t.bottom + gap},
                           {clampX(me.left), t.top - gap - h}};
    for (const POINT& p : tries) {
        const RECT c{p.x, p.y, p.x + w, p.y + h};
        if (c.left < work.left || c.right > work.right || c.top < work.top || c.bottom > work.bottom) continue;
        if (IntersectRect(&inter, &c, &t)) continue;
        SetWindowPos(hwnd_, nullptr, c.left, c.top, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
}

// --- look and layout ------------------------------------------------------------------------------

void SettingsSearch::CreateFonts() {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    LOGFONTW lf{};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_)) {
        lf = ncm.lfMessageFont;
    } else {
        lf.lfHeight = -MulDiv(9, static_cast<int>(dpi_), 72);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
    }
    HFONT old = font_;
    font_ = CreateFontIndirectW(&lf);
    for (HWND c : {edit_, list_, go_, status_, combo_}) {
        if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    if (old) DeleteObject(old);
}

void SettingsSearch::ApplyTheme() {
    palette_ = DarkMode::CurrentPalette();
    const bool dark = palette_.dark;
    DarkMode::SetDarkCaption(hwnd_, dark);
    DarkMode::AllowDarkForWindow(hwnd_, dark);  // the drop-down's list (while dark popups are on)
    for (HWND c : {edit_, go_, combo_}) {
        if (!c) continue;
        DarkMode::AllowDarkForWindow(c, dark);
        DarkMode::ThemeControl(c, dark);
    }
    if (list_) {
        DarkMode::AllowDarkForWindow(list_, dark);
        SetWindowTheme(list_, dark ? L"DarkMode_ItemsView" : L"Explorer", nullptr);
        if (HWND header = ListView_GetHeader(list_)) {
            DarkMode::AllowDarkForWindow(header, dark);
            SetWindowTheme(header, dark ? L"DarkMode_ItemsView" : L"Explorer", nullptr);
        }
        ListView_SetBkColor(list_, palette_.control);
        ListView_SetTextBkColor(list_, CLR_NONE);
        ListView_SetTextColor(list_, palette_.text);
        // The info tips: dark like the settings windows' tooltips.
        if (HWND tips = ListView_GetToolTips(list_)) {
            DarkMode::AllowDarkForWindow(tips, dark);
            SetWindowTheme(tips, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
            SendMessageW(tips, TTM_SETMAXTIPWIDTH, 0, Px(360));
        }
    }
    if (layerBrush_) DeleteObject(layerBrush_);
    if (controlBrush_) DeleteObject(controlBrush_);
    layerBrush_ = CreateSolidBrush(palette_.layer);
    controlBrush_ = CreateSolidBrush(palette_.control);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void SettingsSearch::Layout() {
    if (!hwnd_ || !edit_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int m = Px(12), editH = Px(26), buttonW = Px(96), buttonH = Px(28);
    const int w = std::max(0, static_cast<int>(rc.right) - 2 * m);
    const int buttonTop = std::max(m, static_cast<int>(rc.bottom) - m - buttonH);
    const int listTop = m + editH + Px(8);
    HDWP dwp = BeginDeferWindowPos(4);
    auto place = [&](HWND h, int x, int y, int cx, int cy) {
        if (dwp) dwp = DeferWindowPos(dwp, h, nullptr, x, y, std::max(0, cx), std::max(0, cy), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(edit_, m, m, w, editH);
    place(list_, m, listTop, w, buttonTop - Px(10) - listTop);
    place(go_, rc.right - m - buttonW, buttonTop, buttonW, buttonH);
    place(status_, m, buttonTop, static_cast<int>(rc.right) - m - buttonW - Px(12) - m, buttonH);
    if (dwp) EndDeferWindowPos(dwp);
    if (comboRow_ >= 0) HideCombo(false);
    SetColumns();
}

void SettingsSearch::SetColumns() {
    if (!list_) return;
    RECT rc{};
    GetClientRect(list_, &rc);
    // The room for a vertical scroll bar stays free (no horizontal scroll bar when one appears).
    int w = static_cast<int>(rc.right) - Px(2);
    if (!(GetWindowLongW(list_, GWL_STYLE) & WS_VSCROLL)) w -= GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);
    w = std::max(0, w);
    const int setting = w * 34 / 100, where = w * 42 / 100;
    ListView_SetColumnWidth(list_, 0, setting);
    ListView_SetColumnWidth(list_, 1, where);
    ListView_SetColumnWidth(list_, 2, std::max(Px(60), w - setting - where));
}

// --- keyboard ---------------------------------------------------------------------------------------

bool SettingsSearch::PreTranslateMessage(MSG& msg) {
    if (!hwnd_ || !msg.hwnd || (msg.hwnd != hwnd_ && !IsChild(hwnd_, msg.hwnd))) return false;
    if (msg.message == WM_KEYDOWN) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
        if (msg.wParam == 'F' && ctrl && !alt) {  // Ctrl+F again: the box, its text selected
            SetFocus(edit_);
            SendMessageW(edit_, EM_SETSEL, 0, -1);
            return true;
        }
        if (msg.hwnd == edit_ && (msg.wParam == VK_DOWN || msg.wParam == VK_NEXT) && !alt) {
            // Down from the box: into the list (its first row when none is selected).
            if (Selected() < 0 && !shown_.empty()) SetSelected(0);
            SetFocus(list_);
            return true;
        }
        if (msg.hwnd == list_ && msg.wParam == VK_SPACE && !ctrl && !alt) {
            ChangeSelected();
            return true;
        }
        if (msg.hwnd == list_ && msg.wParam == VK_UP && !ctrl && !alt && Selected() == 0) {
            SetFocus(edit_);  // up from the first row: back to the box
            return true;
        }
    }
    return IsDialogMessageW(hwnd_, &msg) != FALSE;
}

// --- window procedures -----------------------------------------------------------------------------

LRESULT CALLBACK SettingsSearch::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsSearch* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<SettingsSearch*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsSearch*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    const LRESULT r = self->WndProc(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        self->edit_ = self->list_ = self->go_ = self->status_ = self->combo_ = nullptr;
        if (self->host_.destroyed) self->host_.destroyed();  // the app frees the object later (posted)
    }
    return r;
}

LRESULT CALLBACK SettingsSearch::ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<SettingsSearch*>(data);
    switch (msg) {
        case WM_NOTIFY:
            if (self && self->palette_.dark) {
                // The header's dark theme keeps dark item text: the palette's text colour.
                auto* nm = reinterpret_cast<NMHDR*>(lp);
                if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == ListView_GetHeader(hwnd)) {
                    auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
                    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                    if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                        SetTextColor(cd->hdc, self->palette_.text);
                        return CDRF_DODEFAULT;
                    }
                }
            }
            break;
        case WM_LBUTTONDOWN:
            if (self) {
                // A click on the Value cell of the row that was selected already changes it (NM_CLICK).
                LVHITTESTINFO hit{};
                hit.pt = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                const int row = ListView_SubItemHitTest(hwnd, &hit);
                self->pressRow_ = row;
                self->pressWasSelected_ = row >= 0 && row == self->Selected();
            }
            break;
        case WM_CHAR:
            // Typing in the list goes on in the box.
            if (self && wp > L' ' && wp != 0x7F && GetKeyState(VK_CONTROL) >= 0) {
                SetFocus(self->edit_);
                SendMessageW(self->edit_, EM_SETSEL, static_cast<WPARAM>(-1), -1);
                SendMessageW(self->edit_, WM_CHAR, wp, lp);
                return 0;
            }
            if (wp == L' ') return 0;  // Space is a change (PreTranslateMessage), not a type-ahead
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, ListProc, id);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT SettingsSearch::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case kRebuildMessage:
            Rebuild();
            return 0;
        case WM_TIMER:  // round 18 phase 18D: the next index step
            if (wp == kIndexTimerId) {
                KillTimer(hwnd_, kIndexTimerId);
                IndexStep();
                return 0;
            }
            break;
        case kHideComboMessage:
            HideCombo(wp != 0);
            return 0;
        case DM_GETDEFID:
            return MAKELRESULT(kSearchGo, DC_HASDEFID);
        case WM_NEXTDLGCTL:  // as a dialog does (automation: WM_NEXTDLGCTL(control, TRUE) focuses it)
            if (LOWORD(lp) && wp && IsChild(hwnd_, reinterpret_cast<HWND>(wp))) {
                SetFocus(reinterpret_cast<HWND>(wp));
            } else if (!LOWORD(lp)) {
                if (HWND next = GetNextDlgTabItem(hwnd_, GetFocus(), wp != 0)) SetFocus(next);
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                if (HWND f = GetFocus(); f && IsChild(hwnd_, f) && f != combo_) lastFocus_ = f;
            } else if (!HIWORD(wp)) {
                SetFocus(lastFocus_ && IsWindow(lastFocus_) && IsWindowVisible(lastFocus_) ? lastFocus_ : edit_);
                // Back to the dialog: the values may have changed meanwhile (18D: not while the
                // index is being built: it reads the live values anyway, and the dialog's own
                // activation on Create would otherwise start the build twice).
                if (!rebuildPosted_ && !indexing_) rebuildPosted_ = PostMessageW(hwnd_, kRebuildMessage, 0, 0) != FALSE;
            }
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            RECT r{0, 0, Px(kMinWidthDip), Px(kMinHeightDip)};
            AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE)), FALSE, WS_EX_CONTROLPARENT, dpi_);
            mmi->ptMinTrackSize = POINT{r.right - r.left, r.bottom - r.top};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            CreateFonts();
            ApplyTheme();
            Layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETHIGHCONTRAST ||
                (lp && CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lp), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)) {
                ApplyTheme();
            } else if (wp == SPI_SETNONCLIENTMETRICS) {
                CreateFonts();
                Layout();
            }
            return 0;
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            ApplyTheme();
            return 0;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, palette_.text);
            SetBkColor(dc, palette_.control);
            return reinterpret_cast<LRESULT>(controlBrush_);
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, reinterpret_cast<HWND>(lp) == status_ ? palette_.textSecondary : palette_.text);
            SetBkColor(dc, palette_.layer);
            return reinterpret_cast<LRESULT>(layerBrush_);
        }
        case WM_CTLCOLORBTN:
            return reinterpret_cast<LRESULT>(layerBrush_);
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, layerBrush_);
            return 1;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == kSearchEdit && code == EN_CHANGE) {
                Filter();
                return 0;
            }
            if (id == kSearchCombo) {
                if (code == CBN_SELENDOK) {
                    const int row = comboRow_;
                    const int choice = static_cast<int>(SendMessageW(combo_, CB_GETCURSEL, 0, 0));
                    PostMessageW(hwnd_, kHideComboMessage, 1, 0);
                    ApplyChoice(row, choice);
                } else if (code == CBN_SELENDCANCEL) {
                    PostMessageW(hwnd_, kHideComboMessage, 1, 0);
                } else if (code == CBN_KILLFOCUS) {
                    PostMessageW(hwnd_, kHideComboMessage, 0, 0);
                }
                return 0;
            }
            if (id == IDCANCEL) {  // Esc (IsDialogMessage): the drop-down first, then the dialog
                if (comboRow_ >= 0) {
                    HideCombo(true);
                } else {
                    DestroyWindow(hwnd_);
                }
                return 0;
            }
            if (id == IDOK || (id == kSearchGo && (code == BN_CLICKED || lp == 0))) {
                if (comboRow_ >= 0) {
                    const int row = comboRow_;
                    const int choice = static_cast<int>(SendMessageW(combo_, CB_GETCURSEL, 0, 0));
                    HideCombo(true);
                    ApplyChoice(row, choice);
                } else {
                    GoTo();
                }
                return 0;
            }
            break;
        }
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->hwndFrom != list_) break;
            switch (nm->code) {
                case LVN_GETDISPINFOW: {
                    auto* di = reinterpret_cast<NMLVDISPINFOW*>(lp);
                    if (!(di->item.mask & LVIF_TEXT) || !di->item.pszText || di->item.cchTextMax <= 0) return 0;
                    const int i = di->item.iItem;
                    if (i < 0 || i >= static_cast<int>(shown_.size())) {
                        di->item.pszText[0] = L'\0';
                        return 0;
                    }
                    const Row& r = rows_[shown_[static_cast<size_t>(i)]];
                    const std::wstring& s = di->item.iSubItem == 0 ? r.setting : di->item.iSubItem == 1 ? r.where : r.value;
                    wcsncpy_s(di->item.pszText, static_cast<size_t>(di->item.cchTextMax), s.c_str(), _TRUNCATE);
                    return 0;
                }
                case LVN_GETINFOTIPW: {
                    auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lp);
                    if (tip->iItem < 0 || tip->iItem >= static_cast<int>(shown_.size()) || !tip->pszText || tip->cchTextMax <= 0) return 0;
                    const Row& r = rows_[shown_[static_cast<size_t>(tip->iItem)]];
                    std::wstring s = r.setting + L"\n" + r.where;
                    if (!r.value.empty()) s += L"\nValue: " + r.value;
                    if (!r.tip.empty()) s += L"\n\n" + r.tip;
                    wcsncpy_s(tip->pszText, static_cast<size_t>(tip->cchTextMax), s.c_str(), _TRUNCATE);
                    return 0;
                }
                case LVN_ODFINDITEMW:
                    return -1;  // no type-ahead in the list (typing goes to the box)
                case LVN_ITEMCHANGED: {
                    const auto* ch = reinterpret_cast<const NMLISTVIEW*>(lp);
                    if (comboRow_ >= 0 && ch->iItem != comboRow_ && (ch->uNewState & LVIS_SELECTED)) HideCombo(false);
                    return 0;
                }
                case NM_DBLCLK: {
                    const auto* ia = reinterpret_cast<const NMITEMACTIVATE*>(lp);
                    if (ia->iItem >= 0) GoTo();
                    return 0;
                }
                case NM_CLICK: {
                    const auto* ia = reinterpret_cast<const NMITEMACTIVATE*>(lp);
                    if (ia->iItem >= 0 && ia->iSubItem == 2 && ia->iItem == pressRow_ && pressWasSelected_) ChangeSelected();
                    return 0;
                }
                case NM_CUSTOMDRAW: {
                    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        const size_t i = static_cast<size_t>(cd->nmcd.dwItemSpec);
                        // A greyed setting reads greyed (its tooltip says why).
                        if (i < shown_.size() && !rows_[shown_[i]].enabled) cd->clrText = palette_.textDisabled;
                        // The selected row (what Enter / Go to / Space act on) in the accent, also while
                        // the box has the focus (the theme's unfocused selection is hardly visible).
                        // (uItemState's CDIS_SELECTED is not reliable for a report list: ask the list.)
                        if (!palette_.highContrast && i < shown_.size() &&
                            (ListView_GetItemState(list_, static_cast<int>(i), LVIS_SELECTED) & LVIS_SELECTED)) {
                            const bool focused = GetFocus() == list_;
                            const int pa = focused ? 38 : 26;
                            const COLORREF a = palette_.accent, b = palette_.control;
                            cd->clrTextBk = RGB((GetRValue(a) * pa + GetRValue(b) * (100 - pa)) / 100,
                                                (GetGValue(a) * pa + GetGValue(b) * (100 - pa)) / 100,
                                                (GetBValue(a) * pa + GetBValue(b) * (100 - pa)) / 100);
                            if (rows_[shown_[i]].enabled) cd->clrText = palette_.text;
                            cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
                        }
                        return CDRF_DODEFAULT;
                    }
                    return CDRF_DODEFAULT;
                }
                default:
                    break;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
