// The Weather settings page: the widget on / off, the place (a search against Open-Meteo's
// geocoding, or latitude / longitude typed in), units, the refresh interval, a "Refresh now"
// button and the service's status line.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#include "ContextMenu.h"
#include "SettingsForm.h"
#include "SettingsIds.h"
#include "SettingsWindow.h"
#include "Weather.h"

namespace {

using namespace SettingsIds;

// 4 decimals (~11 m), trailing zeros dropped.
std::wstring FormatCoord(double v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.4f", v);
    std::wstring s = buf;
    while (!s.empty() && s.back() == L'0') s.pop_back();
    if (!s.empty() && s.back() == L'.') s.pop_back();
    if (s == L"-0") s = L"0";
    return s;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::iswspace(s[i])) ++i;
    return s.substr(i);
}

// What the config keeps (and saves): 4 decimals.
double RoundCoord(double v) { return std::round(v * 1e4) / 1e4; }

// "47.6", "-122,33" (a decimal comma is accepted) -> value in [lo, hi]. False if not a number.
bool ParseCoord(std::wstring text, double lo, double hi, double& value) {
    text = Trim(std::move(text));
    for (wchar_t& c : text) {
        if (c == L',') c = L'.';
    }
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    const double v = std::wcstod(text.c_str(), &end);
    if (!end || *end != L'\0' || !std::isfinite(v) || v < lo || v > hi) return false;
    value = RoundCoord(v);
    return true;
}

class WeatherPage : public FormPage {
public:
    explicit WeatherPage(SettingsWindow& w) : FormPage(w, L"Weather", L'\xE753') {}

protected:
    void Build() override {
        const int right = Width() - kMargin;
        Title(L"Weather");
        Section(kSectionWeather, L"Weather widget");
        Check(kWeatherShow, kWeatherShowCaption, L"Show the weather next to the clock");
        NextRow();

        Section(kSectionWeatherPlace, L"Location");
        Label(kWeatherSearchLabel, L"Find a place");
        EditBox(kWeatherSearchEdit, kColX, right - kColX - 112, 100);
        Button(kWeatherSearch, L"Search", right - 104, 104);
        NextRow();
        Label(kWeatherResultsLabel, L"Results");
        Combo(kWeatherResultsCombo, kColX, right - kColX, false);
        NextRow(kRowH);
        Text(kWeatherSearchNote, CtlKind::Note, L"Type a city and press Enter or Search.", kColX, right - kColX, 20);
        NextRow(22);
        Label(kWeatherLocationLabel, L"Name shown");
        EditBox(kWeatherLocationEdit, kColX, right - kColX, 128);
        NextRow();
        Label(kWeatherLatLabel, L"Latitude");
        EditBox(kWeatherLatEdit, kColX, 120, 16);
        NextRow();
        Label(kWeatherLonLabel, L"Longitude");
        EditBox(kWeatherLonEdit, kColX, 120, 16);
        NextRow();

        Section(kSectionWeatherUpdates, L"Updates");
        Label(kWeatherUnitsLabel, L"Units");
        Combo(kWeatherUnitsCombo, kColX, 220, false);
        NextRow();
        Label(kWeatherRefreshLabel, L"Refresh every");
        IntSpin(kWeatherRefreshEdit, kWeatherRefreshSpin, kColX, 90, 5, 360, 5,
                [](Config& c) -> int& { return c.weather.refreshMinutes; }, SettingsScope::Weather);
        Text(kWeatherRefreshUnit, CtlKind::Note, L"minutes", kColX + 98, 80);
        NextRow();
        Label(kWeatherStatusLabel, L"Status");
        Text(kWeatherStatus, CtlKind::Value, L"", kColX, right - kColX);
        NextRow();
        Button(kWeatherRefreshNow, L"Refresh now", kColX, 130);
        NextRow();
        Link(kWeatherCredit, L"Weather data by <a>Open-Meteo.com</a> (CC BY 4.0)", kMargin, right - kMargin);
        NextRow();
    }

    void Created() override {
        ComboAdd(kWeatherUnitsCombo, L"Metric (°C, km/h)");
        ComboAdd(kWeatherUnitsCombo, L"Imperial (°F, mph)");
    }

    void Load() override {
        const WeatherConfig& w = Cfg().weather;
        LoadInts();
        SetCheck(kWeatherShow, Cfg().Main().Has(ItemKind::Weather));
        SetText(kWeatherLocationEdit, w.location);
        SetText(kWeatherLatEdit, FormatCoord(w.latitude));
        SetText(kWeatherLonEdit, FormatCoord(w.longitude));
        SetComboSel(kWeatherUnitsCombo, w.units == Units::Imperial ? 1 : 0);
        UpdateStatus();
    }

    void OnWeatherChanged() override { UpdateStatus(); }

    void OnCommand(int id, int code, HWND) override {
        WeatherConfig& w = Cfg().weather;
        switch (id) {
            case kWeatherShow:
                if (code == BN_CLICKED) {
                    // Shown by a "weather" item: added (before the clock) / every one removed.
                    Cfg().Main().Show(ItemKind::Weather, GetCheck(id));
                    Changed(SettingsScope::Weather);
                    UpdateStatus();
                }
                break;
            case kWeatherSearch:
                if (code == BN_CLICKED) StartSearch();
                break;
            case kWeatherResultsCombo:
                if (code == CBN_SELCHANGE) ChoosePlace(ComboSel(id));
                break;
            case kWeatherUnitsCombo:
                if (code == CBN_SELCHANGE) {
                    const Units u = ComboSel(id) == 1 ? Units::Imperial : Units::Metric;
                    if (u != w.units) {
                        w.units = u;
                        Changed(SettingsScope::Weather);
                    }
                }
                break;
            case kWeatherRefreshNow:
                if (code == BN_CLICKED && Host().command) Host().command(ContextMenu::kRefreshWeather);
                break;
            default:
                break;
        }
    }

    LRESULT OnNotify(const NMHDR& nm) override {
        if (static_cast<int>(nm.idFrom) == kWeatherCredit && (nm.code == NM_CLICK || nm.code == NM_RETURN)) {
            AllowSetForegroundWindow(ASFW_ANY);
            ShellExecuteW(nullptr, L"open", L"https://open-meteo.com/", nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }

    void Commit(int id) override {
        WeatherConfig& w = Cfg().weather;
        switch (id) {
            case kWeatherSearchEdit:
                // Enter searches; leaving the box does not.
                if (GetKeyState(VK_RETURN) < 0) StartSearch();
                break;
            case kWeatherLocationEdit: {
                std::wstring name = Trim(GetText(id));
                if (name.size() > 128) name.resize(128);
                SetText(id, name);
                if (name != w.location) {
                    w.location = name;
                    Changed(SettingsScope::Weather);
                }
                break;
            }
            case kWeatherLatEdit:
            case kWeatherLonEdit: {
                const bool lat = id == kWeatherLatEdit;
                double& field = lat ? w.latitude : w.longitude;
                double v = field;
                if (!ParseCoord(GetText(id), lat ? -90.0 : -180.0, lat ? 90.0 : 180.0, v)) {
                    SetText(id, FormatCoord(field));  // not a valid coordinate: back to the current one
                    MessageBeep(MB_ICONWARNING);
                    break;
                }
                SetText(id, FormatCoord(v));
                if (std::fabs(v - field) > 1e-9) {
                    field = v;
                    Changed(SettingsScope::Weather);
                    UpdateStatus();
                }
                break;
            }
            default:
                break;
        }
    }

private:
    void EditBox(int id, int x, int w, int maxLength) {
        Ctl& e = Add(id, CtlKind::Edit, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, RECT{x, y_ + 4, x + w, y_ + kRowH - 4});
        e.exStyle = WS_EX_CLIENTEDGE;
        e.hi = maxLength;
    }

    void UpdateStatus() {
        if (Host().weatherStatus) SetText(kWeatherStatus, Host().weatherStatus());
    }

    void StartSearch() {
        const std::wstring query = Trim(GetText(kWeatherSearchEdit));
        if (query.empty()) {
            SetText(kWeatherSearchNote, L"Type a place name first.");
            return;
        }
        if (!Host().geocode) return;
        const bool started = Host().geocode(query, [this](const std::vector<WeatherService::Place>& places,
                                                          const std::wstring& error) { OnPlaces(places, error); });
        SetText(kWeatherSearchNote, started ? L"Searching…" : L"Could not start the search.");
    }

    void OnPlaces(const std::vector<WeatherService::Place>& places, const std::wstring& error) {
        places_ = places;
        HWND combo = Item(kWeatherResultsCombo);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (const WeatherService::Place& p : places_) {
            const std::wstring label = p.Label();
            ComboAdd(kWeatherResultsCombo, label.c_str());
        }
        if (!error.empty()) {
            SetText(kWeatherSearchNote, L"Search failed: " + error);
        } else if (places_.empty()) {
            SetText(kWeatherSearchNote, L"No places found.");
        } else {
            SetText(kWeatherSearchNote, std::to_wstring(places_.size()) +
                                            (places_.size() == 1 ? L" place found: choose it in the list."
                                                                 : L" places found: choose one in the list."));
        }
    }

    void ChoosePlace(int index) {
        if (index < 0 || index >= static_cast<int>(places_.size())) return;
        const WeatherService::Place& p = places_[static_cast<size_t>(index)];
        WeatherConfig& w = Cfg().weather;
        w.latitude = RoundCoord(p.latitude);
        w.longitude = RoundCoord(p.longitude);
        w.location = p.name.size() > 128 ? p.name.substr(0, 128) : p.name;
        SetText(kWeatherLocationEdit, w.location);
        SetText(kWeatherLatEdit, FormatCoord(w.latitude));
        SetText(kWeatherLonEdit, FormatCoord(w.longitude));
        Changed(SettingsScope::Weather);
        UpdateStatus();
    }

    std::vector<WeatherService::Place> places_;
};

}  // namespace

std::unique_ptr<SettingsPage> MakeWeatherPage(SettingsWindow& window) { return std::make_unique<WeatherPage>(window); }
