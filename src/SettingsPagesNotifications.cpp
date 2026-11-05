// Global > Notifications (round 4 phase 9): tray balloons in the centre, clearOnActivate, the poll
// interval, Windows' access status (with a button to its privacy setting) and the apps whose badges
// are shown (notifications.excluded). A dock's own badges and notification centre item are on its
// window's Behaviour tab since round 5 phase D (no Global page acts on one dock).

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "SettingsForm.h"
#include "SettingsIds.h"

namespace {

using namespace SettingsIds;

constexpr int kListH = 180;
constexpr int kButtonW = 200;
constexpr int kNumberW = 72;

class NotificationsPage : public FormPage {
public:
    explicit NotificationsPage(SettingsWindow& w) : FormPage(w, L"Notifications", L'\xEA8F') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Notifications");
        HelpTopic(L"guide-settings.md#notifications");
        // Round 8 phase 8A: one-line notes and hints, the explanations as their tooltips; round 8
        // phase 8D: in cards (Centre, Access, Badges per app).
        Section(kNotifCentreSection, L"Centre", L"guide-settings.md#notification-centre");
        Ctl& centre = Text(kNotifCentreNote, CtlKind::Note, L"A widget: drag it onto a dock from that dock's Contents tab.",
                           kMargin, right - kMargin, 20);
        centre.maxLines = 2;
        Tip(kNotifCentreNote,
            L"The notification centre is a widget (a bell with the unread count; a click opens the list: click a "
            L"notification to open its app, the cross or Delete dismisses it, Clear all dismisses every one): drag it "
            L"onto a dock from the Contents tab of that dock's own window, where the Behaviour tab has its badges. The "
            L"scriptable Notification bell widget opens the same list (its \"A click opens\" setting).");
        NextRow(24);
        Check(kNotifBalloons, kNotifBalloonsCaption, L"Tray balloons in the centre instead of Windows toasts",
              L"With the tray takeover on (Tray icons)", kNotifBalloonsNote);
        static constexpr wchar_t kBalloonsTip[] =
            L"With the tray takeover on (Tray icons): an app's tray balloon becomes a notification of the dock's list "
            L"(and badges that app); Windows no longer shows it as a toast.";
        Tip(kNotifBalloons, kBalloonsTip);
        Tip(kNotifBalloonsCaption, kBalloonsTip);
        Tip(kNotifBalloonsNote, kBalloonsTip);
        NextRow();
        Check(kNotifClearOnActivate, kNotifClearOnActivateCaption, L"Clear an app's badge when it is opened from the dock");
        Tip(kNotifClearOnActivateCaption, L"Opening an app from the dock marks its notifications read, so its badge goes. On by default.");
        NextRow();
        Label(kNotifPollLabel, L"Check for notifications every", L"Only while something needs it", kNotifPollNote);
        Tip(kNotifPollLabel,
            L"Windows gives no change notice to an app that is not installed from a package, so the dock asks for the "
            L"list this often (every 5 s after a minute without change, every second while the centre is open), and only "
            L"while badges, the centre or a widget need it. Each check costs a few tens of milliseconds of CPU.");
        Spin(kNotifPollEdit, kNotifPollSpin, kColX, kNumberW, 1, 60, 1);
        Text(kNotifPollUnit, CtlKind::Note, L"seconds", kColX + kNumberW + 8, 120);
        NextRow();
        Section(kNotifStatusSection, L"Access", L"guide-settings.md#notification-access");
        Label(kNotifStatusLabel, L"Status");
        Text(kNotifStatus, CtlKind::Value, L"", kColX, right - kColX, 40);
        NextRow(44);
        Button(kNotifOpenSettings, L"Open Windows settings", kColX, kButtonW);
        NextRow();
        Text(kNotifDndNote, CtlKind::Note, L"Do not disturb: use Windows' quick settings.", kMargin, right - kMargin, 20);
        Tip(kNotifDndNote, L"Do not disturb has no public interface for apps, so the centre has no switch for it (Windows' "
                           L"quick settings have one).");
        NextRow(22);
        Section(kNotifAppsSection, L"Badges per app", L"guide-settings.md#badges-per-app");
        Add(kNotifAppList, CtlKind::List, WC_LISTVIEWW, L"Apps with badges",
            WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
            RECT{kMargin, y_, right, y_ + kListH});
        Tip(kNotifAppList, L"The apps of every dock and every app with a notification now, with their unread count. Clear an app's "
                           L"check box and it never shows a badge on any dock. Every app is checked by default.");
        NextRow(kListH + 8);
        Ctl& apps = Text(kNotifAppsNote, CtlKind::Note, L"Unchecked apps never show a badge (on any dock).", kMargin, right - kMargin, 20);
        apps.maxLines = 2;
        Tip(kNotifAppsNote, L"The apps on every dock and every app with a notification now. Unchecked apps never show a "
                            L"badge (on any dock).");
        NextRow(22);
    }

    void OnResized() override { OnStyleChanged(); }  // round 8 phase 8A: the list's columns

    void Created() override {
        HWND list = Item(kNotifAppList);
        if (!list) return;
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        const wchar_t* names[] = {L"App", L"Unread", L"Key"};
        for (int i = 0; i < 3; ++i) {
            col.pszText = const_cast<wchar_t*>(names[i]);
            col.cx = 120;
            col.iSubItem = i;
            ListView_InsertColumn(list, i, &col);
        }
    }

    void OnStyleChanged() override {
        HWND list = Item(kNotifAppList);
        const Ctl* c = Find(kNotifAppList);
        if (!list || !c) return;
        const int total = Window().Px(c->dip.right - c->dip.left) - GetSystemMetricsForDpi(SM_CXVSCROLL, Window().Dpi()) - 4;
        ListView_SetColumnWidth(list, 0, total * 40 / 100);
        ListView_SetColumnWidth(list, 1, total * 12 / 100);
        ListView_SetColumnWidth(list, 2, total - total * 40 / 100 - total * 12 / 100);
    }

    void Load() override {
        SetCheck(kNotifBalloons, Cfg().notifications.balloons);
        SetCheck(kNotifClearOnActivate, Cfg().notifications.clearOnActivate);
        SetSpin(kNotifPollSpin, Cfg().notifications.pollS);
        LoadStatus();
        FillList();
    }

    void OnNotificationsChanged() override {
        LoadStatus();
        RefreshCounts();
    }

    void OnCommand(int id, int code, HWND) override {
        if (code != BN_CLICKED) return;
        NotificationsConfig& n = Cfg().notifications;
        switch (id) {
            case kNotifBalloons:
                n.balloons = GetCheck(kNotifBalloons);
                Changed(SettingsScope::Notifications);
                break;
            case kNotifClearOnActivate:
                n.clearOnActivate = GetCheck(kNotifClearOnActivate);
                Changed(SettingsScope::SaveOnly);
                break;
            case kNotifOpenSettings:
                ShellExecuteW(nullptr, L"open", L"ms-settings:privacy-notifications", nullptr, nullptr, SW_SHOWNORMAL);
                break;
            default:
                break;
        }
    }

    void Spun(int spin, int pos) override {
        if (spin == kNotifPollSpin) SetPoll(pos);
    }
    void Commit(int id) override {
        if (id != kNotifPollEdit) return;
        int value = 0;
        if (ReadInt(id, 1, 60, value)) {
            SetPoll(value);
        } else {
            SetSpin(kNotifPollSpin, Cfg().notifications.pollS);
        }
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) != kNotifAppList || nm.code != LVN_ITEMCHANGED || Loading() || filling_) return 0;
        const auto& change = reinterpret_cast<const NMLISTVIEW&>(nm);
        if (!(change.uChanged & LVIF_STATE) || ((change.uNewState ^ change.uOldState) & LVIS_STATEIMAGEMASK) == 0) return 0;
        if (change.iItem < 0 || static_cast<size_t>(change.iItem) >= apps_.size()) return 0;
        const bool shown = ((change.uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
        std::vector<std::wstring>& ex = Cfg().notifications.excluded;
        const std::wstring& key = apps_[static_cast<size_t>(change.iItem)].key;
        const auto it = std::find(ex.begin(), ex.end(), key);
        if (shown && it != ex.end()) {
            ex.erase(it);
        } else if (!shown && it == ex.end()) {
            ex.push_back(key);
        } else {
            return 0;
        }
        Changed(SettingsScope::Notifications);
        return 0;
    }

private:
    void SetPoll(int value) {
        value = std::clamp(value, 1, 60);
        SetSpin(kNotifPollSpin, value);
        if (Cfg().notifications.pollS == value) return;
        Cfg().notifications.pollS = value;
        Changed(SettingsScope::Notifications);
    }

    void LoadStatus() {
        const SettingsNotificationStatus st = Host().notificationStatus ? Host().notificationStatus() : SettingsNotificationStatus{};
        std::wstring text;
        switch (st.state) {
            case 1: text = L"Reading notifications (" + std::to_wstring(st.listed) + L" listed, " + std::to_wstring(st.unread) + L" unread)"; break;
            case 2: text = st.reason.empty() ? std::wstring(L"Access denied") : st.reason; break;
            default: text = L"Off: nothing on the docks needs notifications"; break;
        }
        SetText(kNotifStatus, text);
    }

    // Every dock's apps, then the apps with notifications (by key, once each).
    void FillList() {
        HWND list = Item(kNotifAppList);
        if (!list) return;
        filling_ = true;
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        apps_ = Host().notificationApps ? Host().notificationApps() : std::vector<SettingsNotificationApp>{};
        const std::vector<std::wstring>& ex = Cfg().notifications.excluded;
        // Excluded keys of apps not shown now stay listed (so they can be checked again).
        for (const std::wstring& k : ex) {
            if (std::none_of(apps_.begin(), apps_.end(), [&](const SettingsNotificationApp& a) { return a.key == k; })) {
                apps_.push_back(SettingsNotificationApp{k, k, 0});
            }
        }
        for (size_t i = 0; i < apps_.size(); ++i) {
            const SettingsNotificationApp& a = apps_[i];
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            std::wstring name = a.name;
            item.pszText = name.data();
            const int at = ListView_InsertItem(list, &item);
            std::wstring unread = std::to_wstring(a.unread);
            ListView_SetItemText(list, at, 1, unread.data());
            std::wstring key = a.key;
            ListView_SetItemText(list, at, 2, key.data());
            ListView_SetCheckState(list, at, std::find(ex.begin(), ex.end(), a.key) == ex.end());
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        filling_ = false;
    }

    // The unread column follows the list; new apps join it (the rows are rebuilt only then).
    void RefreshCounts() {
        HWND list = Item(kNotifAppList);
        if (!list) return;
        const std::vector<SettingsNotificationApp> now =
            Host().notificationApps ? Host().notificationApps() : std::vector<SettingsNotificationApp>{};
        bool added = false;
        for (const SettingsNotificationApp& a : now) {
            if (std::none_of(apps_.begin(), apps_.end(), [&](const SettingsNotificationApp& b) { return b.key == a.key; })) added = true;
        }
        if (added) {
            FillList();
            return;
        }
        filling_ = true;
        for (size_t i = 0; i < apps_.size(); ++i) {
            size_t unread = 0;
            for (const SettingsNotificationApp& a : now) {
                if (a.key == apps_[i].key) unread = a.unread;
            }
            std::wstring text = std::to_wstring(unread);
            ListView_SetItemText(list, static_cast<int>(i), 1, text.data());
        }
        filling_ = false;
    }

    std::vector<SettingsNotificationApp> apps_;  // the list's rows
    bool filling_ = false;
};

}  // namespace

std::unique_ptr<SettingsPage> MakeNotificationsPage(SettingsWindow& window) {
    return std::make_unique<NotificationsPage>(window);
}
