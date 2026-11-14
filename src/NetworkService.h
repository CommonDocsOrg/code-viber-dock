#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Round 27 phase 27B: the `network` provider of the widget API (permission "network"): what the
// taskbar's network icon showed, which hiding the taskbar loses. The connection Windows uses for
// the internet (Windows.Networking.Connectivity: NetworkInformation::GetInternetConnectionProfile):
// whether there is one, its connectivity level (internet / local only / captive or constrained /
// none), the kind of its adapter (IANA interface type: ethernet, Wi-Fi, cellular, a VPN-style
// virtual / tunnel / PPP adapter), for Wi-Fi the network name (WlanConnectionProfileDetails::
// GetConnectedSsid) and the signal bars (ConnectionProfile::GetSignalBars, 0 - 5), and whether it
// is metered (ConnectionCost::NetworkCostType Fixed / Variable: free with the profile).
//
// Why these APIs (round 27, from Microsoft's documentation): wlanapi was the first plan (SSID and a
// 0 - 100 quality through WlanQueryInterface's current connection, signal changes through
// WlanRegisterNotification's MSM source). Since Windows 11 24H2 both need the user's precise
// location consent ("Changes to API behavior for Wi-Fi access and location"): without it they fail
// with ERROR_ACCESS_DENIED, and the first call of an app shows a one-time location prompt, which a
// network icon must not cause. That page recommends GetConnectedSsid for the SSID instead. The
// signal is therefore the bars Windows itself shows (0 - 5), not a percentage.
// INetworkListManager's connection points give connectivity only (no kind, name or signal), and
// NotifyNetworkConnectivityHintChange gives connectivity and cost only: one WinRT profile read
// gives everything, and its one event (NetworkStatusChanged) covers connect / disconnect, internet
// gained / lost, a cost change and the internet profile changing (switching Wi-Fi <-> ethernet).
// NOT covered by an event (documented events list no such case): the signal bars changing while
// nothing else does; they are re-read at every event and whenever the provider wakes (Wake).
//
// Event-driven, nothing polls, no timer. NetworkStatusChanged arrives on a thread-pool thread; the
// profile is read right there (the reads are RPCs into the network list service: never on the UI
// thread after the first), and the result is posted to the owner window as kMessage (lParam: a
// State*; the UI thread keeps the newest, compares, and calls `changed` only on a difference).
// Quiet (SetQuiet, WidgetRuntime: no user of the provider can be seen, or the session is paused):
// events only set a flag; Wake (leaving quiet) reads once if something came meanwhile. Started by
// WidgetRuntime for the first widget that uses `network`, stopped when the last one goes.
class NetworkService {
public:
    static constexpr UINT kMessage = WM_APP + 81;  // lParam: State* (the receiver frees it)

    struct State {
        bool available = false;    // the provider could read the connection state
        bool connected = false;    // there is a connection profile for the internet
        int level = 0;             // 0 none, 1 local access only, 2 constrained (a captive portal), 3 internet
        std::wstring kind;         // "ethernet", "wifi", "cellular", "vpn", "other", "none"
        std::wstring name;         // Wi-Fi: the SSID; otherwise the profile's name ("Network 2"); "" none
        int bars = -1;             // 0 - 5 for Wi-Fi / cellular, -1 unknown / wired
        bool metered = false;      // the cost type is Fixed or Variable
        bool operator==(const State&) const = default;
    };

    NetworkService() = default;
    NetworkService(const NetworkService&) = delete;
    NetworkService& operator=(const NetworkService&) = delete;
    ~NetworkService() { Stop(); }

    // Reads the state once (synchronously: the first network.get() answers with it) and subscribes
    // to NetworkStatusChanged. False if WinRT's connectivity API is not available.
    bool Start(HWND owner);
    void Stop();
    bool Active() const { return shared_ != nullptr; }
    const State& Get() const { return state_; }

    // Quiet: events are only noted. Leaving quiet reads again (posted) when one was noted.
    void SetQuiet(bool quiet);
    bool Quiet() const { return quiet_; }

    // kMessage: takes the newest state; `changed` when it differs. True if handled.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void SetChanged(std::function<void()> changed) { changed_ = std::move(changed); }
    uint64_t Reads() const;          // profile reads since the process started (automation)

    // The state now, read on the calling thread (exceptions caught: available false).
    static State Read();
    // network.get()'s object (docs/widgets.md "network"), UTF-8 JSON; WidgetSamples uses it too.
    static std::string Json(const State& s);

private:
    struct Shared;
    // Reads on a thread-pool thread (Crash::Job), then posts; at most one reader at a time (a request
    // while one runs makes it read once more).
    static void RequestRead(const std::shared_ptr<Shared>& shared);

    HWND owner_ = nullptr;
    std::shared_ptr<Shared> shared_;
    int64_t token_ = 0;  // the NetworkStatusChanged registration (winrt::event_token.value)
    bool quiet_ = false;
    State state_;
    std::function<void()> changed_;
};
