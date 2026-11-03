// The Calendar page (round 4 phase 13): config.calendars. The feeds (check box = enabled, the
// service's status per feed), fields to add a feed or edit the selected one (name, URL, colour,
// refresh interval), Remove, Refresh now (= DockCommand 135) and Show dots.

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <string>
#include <vector>

#include "CalendarService.h"
#include "ContextMenu.h"
#include "SettingsForm.h"
#include "SettingsIds.h"

namespace {

using namespace SettingsIds;

constexpr int kListH = 160;
constexpr int kButtonW = 140;
constexpr int kNumberW = 72;

class CalendarPage : public FormPage {
public:
    explicit CalendarPage(SettingsWindow& w) : FormPage(w, L"Calendar feeds", L'\xE787') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Calendar feeds");
        Text(kCalNote, CtlKind::Note,
             L"Calendars as ICS feeds: a web address (https://, http:// or webcal://, such as Google Calendar's "
             L"\"secret address in iCal format\" or Outlook's published calendar), or an .ics file on this PC (re-read "
             L"when it changes). Events show in the clock's month calendar and in widgets such as Agenda.",
             kMargin, right - kMargin, 58);
        NextRow(62);
        Section(kCalSection, L"Feeds");
        Add(kCalFeedList, CtlKind::List, WC_LISTVIEWW, L"Calendar feeds",
            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
            RECT{kMargin, y_, right, y_ + kListH});
        NextRow(kListH + 8);
        Button(kCalRemove, L"Remove", kMargin, kButtonW);
        Button(kCalRefreshNow, L"Refresh now", kMargin + kButtonW + 8, kButtonW);
        NextRow(kRowH + 10);
        Section(kCalEditSection, L"Add or edit a feed");
        Label(kCalNameLabel, L"Name");
        EditBox(kCalNameEdit, kColX, right - kColX, 128);
        NextRow();
        Label(kCalUrlLabel, L"Address or file");
        EditBox(kCalUrlEdit, kColX, right - kColX, 4096);
        NextRow();
        Label(kCalColorLabel, L"Colour (#AARRGGBB)");
        EditBox(kCalColorEdit, kColX, 120, 9);
        NextRow();
        Label(kCalRefreshLabel, L"Check for changes every");
        Spin(kCalRefreshEdit, kCalRefreshSpin, kColX, kNumberW, 5, 1440, 5);
        Text(kCalRefreshUnit, CtlKind::Note, L"minutes (web feeds)", kColX + kNumberW + 8, 200);
        NextRow();
        Label(kCalReminderLabel, L"Remind me before events");
        Spin(kCalReminderEdit, kCalReminderSpin, kColX, kNumberW, 0, 1440, 5);
        Text(kCalReminderUnit, CtlKind::Note, L"minutes (0: the default below)", kColX + kNumberW + 8, 260);
        NextRow();
        Check(kCalEnable, kCalEnableCaption, L"Enabled");
        NextRow();
        Button(kCalAdd, L"Add feed", kMargin, kButtonW);
        Button(kCalSave, L"Save changes", kMargin + kButtonW + 8, kButtonW);
        NextRow(kRowH + 6);
        Text(kCalFieldsNote, CtlKind::Note, L"", kMargin, right - kMargin, 40);
        NextRow(44);
        Section(kCalOptionsSection, L"Display");
        Check(kCalShowDots, kCalShowDotsCaption, L"Show dots on days with events (month calendar, clock)");
        NextRow();
        Label(kCalDefaultReminderLabel, L"Default reminder");
        Spin(kCalDefaultReminderEdit, kCalDefaultReminderSpin, kColX, kNumberW, 0, 1440, 5);
        Text(kCalDefaultReminderUnit, CtlKind::Note, L"minutes before (0: no reminders)", kColX + kNumberW + 8, 260);
        NextRow();
        Text(kCalReminderNote, CtlKind::Note,
             L"A reminder shows a banner beside the dock (Snooze 5 min, Dismiss) before each timed event of a feed with "
             L"reminders. All-day events have none.",
             kMargin, right - kMargin, 40);
        NextRow(44);
    }

    void Created() override {
        HWND list = Item(kCalFeedList);
        if (!list) return;
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        const wchar_t* names[] = {L"Name", L"Address", L"Every", L"Status"};
        for (int i = 0; i < 4; ++i) {
            col.pszText = const_cast<wchar_t*>(names[i]);
            col.cx = 100;
            col.iSubItem = i;
            ListView_InsertColumn(list, i, &col);
        }
    }

    void OnStyleChanged() override {
        HWND list = Item(kCalFeedList);
        const Ctl* c = Find(kCalFeedList);
        if (!list || !c) return;
        const int total = Window().Px(c->dip.right - c->dip.left) - GetSystemMetricsForDpi(SM_CXVSCROLL, Window().Dpi()) - 4;
        ListView_SetColumnWidth(list, 0, total * 22 / 100);
        ListView_SetColumnWidth(list, 1, total * 34 / 100);
        ListView_SetColumnWidth(list, 2, total * 10 / 100);
        ListView_SetColumnWidth(list, 3, total - total * 66 / 100);
    }

    void Load() override {
        SetCheck(kCalShowDots, Cfg().calendars.showDots);
        SetSpin(kCalDefaultReminderSpin, Cfg().calendars.defaultReminderMin);
        FillList();
        LoadFields(Selected());
    }

    void OnCalendarChanged() override { RefreshStatus(); }

    void OnCommand(int id, int code, HWND) override {
        if (code != BN_CLICKED) return;
        CalendarsConfig& c = Cfg().calendars;
        switch (id) {
            case kCalShowDots:
                c.showDots = GetCheck(kCalShowDots);
                Changed(SettingsScope::Calendars);
                break;
            case kCalRefreshNow:
                if (Host().command) Host().command(ContextMenu::kRefreshCalendars);
                break;
            case kCalRemove: {
                const int sel = Selected();
                if (sel < 0 || static_cast<size_t>(sel) >= c.feeds.size()) break;
                c.feeds.erase(c.feeds.begin() + sel);
                Changed(SettingsScope::Calendars);
                FillList();
                LoadFields(-1);
                break;
            }
            case kCalAdd:
            case kCalSave: {
                CalendarFeed f;
                std::wstring error;
                const int sel = Selected();
                if (id == kCalSave && (sel < 0 || static_cast<size_t>(sel) >= c.feeds.size())) {
                    SetText(kCalFieldsNote, L"Select a feed in the list to save changes to it.");
                    break;
                }
                if (id == kCalSave) f = c.feeds[static_cast<size_t>(sel)];
                if (!ReadFields(f, id == kCalAdd ? c.feeds.size() : static_cast<size_t>(sel), error)) {
                    SetText(kCalFieldsNote, error);
                    MessageBeep(MB_OK);
                    break;
                }
                if (id == kCalAdd) {
                    if (c.feeds.size() >= 32) {
                        SetText(kCalFieldsNote, L"At most 32 feeds.");
                        break;
                    }
                    c.feeds.push_back(f);
                } else {
                    c.feeds[static_cast<size_t>(sel)] = f;
                }
                Changed(SettingsScope::Calendars);
                FillList();
                Select(id == kCalAdd ? static_cast<int>(c.feeds.size()) - 1 : sel);
                SetText(kCalFieldsNote, id == kCalAdd ? L"Added." : L"Saved.");
                break;
            }
            default:
                break;
        }
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) != kCalFeedList || nm.code != LVN_ITEMCHANGED || Loading() || filling_) return 0;
        const auto& change = reinterpret_cast<const NMLISTVIEW&>(nm);
        if (!(change.uChanged & LVIF_STATE)) return 0;
        std::vector<CalendarFeed>& feeds = Cfg().calendars.feeds;
        if (change.iItem < 0 || static_cast<size_t>(change.iItem) >= feeds.size()) return 0;
        if ((change.uNewState & LVIS_SELECTED) && !(change.uOldState & LVIS_SELECTED)) LoadFields(change.iItem);
        if (((change.uNewState ^ change.uOldState) & LVIS_STATEIMAGEMASK) != 0 && (change.uOldState & LVIS_STATEIMAGEMASK)) {
            const bool on = ((change.uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
            CalendarFeed& f = feeds[static_cast<size_t>(change.iItem)];
            if (f.enabled != on) {
                f.enabled = on;
                Changed(SettingsScope::Calendars);
                if (change.iItem == Selected()) SetCheck(kCalEnable, on);
            }
        }
        return 0;
    }

private:
    int Selected() const {
        HWND list = Item(kCalFeedList);
        return list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1;
    }

    void Select(int row) {
        HWND list = Item(kCalFeedList);
        if (!list || row < 0) return;
        ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, row, FALSE);
    }

    // The fields from feed `row` (-1: empty, for a new feed).
    void LoadFields(int row) {
        const std::vector<CalendarFeed>& feeds = Cfg().calendars.feeds;
        CalendarFeed f;
        if (row >= 0 && static_cast<size_t>(row) < feeds.size()) {
            f = feeds[static_cast<size_t>(row)];
        } else {
            f.color = DefaultFeedColor(feeds.size());
        }
        SetText(kCalNameEdit, f.name);
        SetText(kCalUrlEdit, f.url);
        SetText(kCalColorEdit, FormatArgbColor(f.color));
        SetSpin(kCalRefreshSpin, f.refreshMin);
        SetSpin(kCalReminderSpin, f.reminderMin);
        SetCheck(kCalEnable, f.enabled);
        Enable(kCalSave, row >= 0);
        Enable(kCalRemove, row >= 0);
    }

    bool ReadFields(CalendarFeed& f, size_t index, std::wstring& error) {
        f.name = GetText(kCalNameEdit);
        f.url = GetText(kCalUrlEdit);
        bool file = false;
        std::wstring location, problem;
        if (!CalendarService::Classify(f.url, file, location, problem)) {
            error = L"Address: " + problem + L".";
            return false;
        }
        if (f.name.empty()) {
            // The file's name, else the host.
            const size_t cut = location.find_last_of(file ? L"\\" : L"/");
            f.name = file && cut != std::wstring::npos ? location.substr(cut + 1) : location;
            if (!file) {
                const size_t start = location.find(L"://");
                const size_t end = location.find(L'/', start == std::wstring::npos ? 0 : start + 3);
                if (start != std::wstring::npos) f.name = location.substr(start + 3, end == std::wstring::npos ? std::wstring::npos : end - start - 3);
            }
            if (f.name.size() > 64) f.name.resize(64);
            SetText(kCalNameEdit, f.name);
        }
        const std::wstring color = GetText(kCalColorEdit);
        if (color.empty()) {
            f.color = DefaultFeedColor(index);
        } else if (!ParseArgbColor(color, f.color)) {
            error = L"Colour: use #AARRGGBB or #RRGGBB.";
            return false;
        }
        int minutes = 30;
        if (!ReadInt(kCalRefreshEdit, 5, 1440, minutes)) {
            error = L"Interval: a number of minutes (5 - 1440).";
            return false;
        }
        f.refreshMin = minutes;
        int remind = 0;
        if (!ReadInt(kCalReminderEdit, 0, 1440, remind)) {
            error = L"Reminder: a number of minutes (0 - 1440).";
            return false;
        }
        f.reminderMin = remind;
        f.enabled = GetCheck(kCalEnable);
        return true;
    }

    void FillList() {
        HWND list = Item(kCalFeedList);
        if (!list) return;
        filling_ = true;
        const int sel = Selected();
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        const std::vector<CalendarFeed>& feeds = Cfg().calendars.feeds;
        const std::vector<SettingsCalendarFeed> status = Host().calendarFeeds ? Host().calendarFeeds() : std::vector<SettingsCalendarFeed>{};
        for (size_t i = 0; i < feeds.size(); ++i) {
            const CalendarFeed& f = feeds[i];
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            std::wstring name = f.name;
            item.pszText = name.data();
            const int at = ListView_InsertItem(list, &item);
            std::wstring url = f.url;
            ListView_SetItemText(list, at, 1, url.data());
            std::wstring every = std::to_wstring(f.refreshMin) + L" min";
            ListView_SetItemText(list, at, 2, every.data());
            std::wstring st = i < status.size() ? status[i].status : std::wstring();
            ListView_SetItemText(list, at, 3, st.data());
            ListView_SetCheckState(list, at, f.enabled);
        }
        if (sel >= 0 && static_cast<size_t>(sel) < feeds.size()) Select(sel);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        filling_ = false;
    }

    void RefreshStatus() {
        HWND list = Item(kCalFeedList);
        if (!list) return;
        const std::vector<SettingsCalendarFeed> status = Host().calendarFeeds ? Host().calendarFeeds() : std::vector<SettingsCalendarFeed>{};
        if (static_cast<size_t>(ListView_GetItemCount(list)) != Cfg().calendars.feeds.size()) {
            FillList();
            return;
        }
        filling_ = true;
        for (size_t i = 0; i < status.size(); ++i) {
            std::wstring st = status[i].status;
            ListView_SetItemText(list, static_cast<int>(i), 3, st.data());
        }
        filling_ = false;
    }

    void Spun(int spin, int pos) override {
        if (spin == kCalDefaultReminderSpin) SetDefaultReminder(pos);
    }
    void Commit(int id) override {
        if (id != kCalDefaultReminderEdit) return;
        int value = 0;
        if (ReadInt(id, 0, 1440, value)) {
            SetDefaultReminder(value);
        } else {
            SetSpin(kCalDefaultReminderSpin, Cfg().calendars.defaultReminderMin);
        }
    }
    void SetDefaultReminder(int value) {
        value = std::clamp(value, 0, 1440);
        SetSpin(kCalDefaultReminderSpin, value);
        if (Cfg().calendars.defaultReminderMin == value) return;
        Cfg().calendars.defaultReminderMin = value;
        Changed(SettingsScope::Calendars);
    }

    bool filling_ = false;
};

}  // namespace

std::unique_ptr<SettingsPage> MakeCalendarPage(SettingsWindow& window) { return std::make_unique<CalendarPage>(window); }
