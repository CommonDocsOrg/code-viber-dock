#include "NetworkService.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>

#include <algorithm>

#include "Crash.h"
#include "JsonUtil.h"

namespace nc = winrt::Windows::Networking::Connectivity;

// What the thread-pool side shares with the service (kept alive by every pending reader, so a Stop
// while one runs is safe: the reader then posts to a window that ignores it, or fails to post).
struct NetworkService::Shared {
    std::atomic<HWND> owner{nullptr};
    std::atomic<bool> quiet{false};
    std::atomic<bool> missed{false};   // an event came while quiet
    std::atomic<int> reading{0};       // 0 idle, 1 a reader runs, 2 it must read once more
    std::atomic<uint64_t> reads{0};
};

namespace {

// IANA ifType numbers (RFC / IANA "ifType-MIB"; ipifcons.h has the same values as IF_TYPE_*).
std::wstring KindOf(uint32_t iana, bool wlan, bool wwan) {
    if (wlan || iana == 71) return L"wifi";                 // IF_TYPE_IEEE80211
    if (wwan || iana == 243 || iana == 244) return L"cellular";  // IF_TYPE_WWANPP / WWANPP2
    if (iana == 6) return L"ethernet";                      // IF_TYPE_ETHERNET_CSMACD
    // PPP (23), a tunnel (131) or a proprietary virtual adapter (53): how VPN clients (the Windows
    // one, WireGuard, OpenVPN's TAP / wintun) present themselves. Hyper-V's vEthernet is type 6.
    if (iana == 23 || iana == 131 || iana == 53) return L"vpn";
    return L"other";
}

}  // namespace

NetworkService::State NetworkService::Read() {
    State s;
    try {
        const nc::ConnectionProfile p = nc::NetworkInformation::GetInternetConnectionProfile();
        s.available = true;
        if (!p) {
            s.kind = L"none";
            return s;
        }
        s.connected = true;
        switch (p.GetNetworkConnectivityLevel()) {
            case nc::NetworkConnectivityLevel::InternetAccess: s.level = 3; break;
            case nc::NetworkConnectivityLevel::ConstrainedInternetAccess: s.level = 2; break;
            case nc::NetworkConnectivityLevel::LocalAccess: s.level = 1; break;
            default: s.level = 0; break;
        }
        const bool wlan = p.IsWlanConnectionProfile();
        const bool wwan = p.IsWwanConnectionProfile();
        uint32_t iana = 0;
        if (const nc::NetworkAdapter a = p.NetworkAdapter()) iana = a.IanaInterfaceType();
        s.kind = KindOf(iana, wlan, wwan);
        if (wlan) {
            // Round 27 phase 27B: the documented replacement for WlanQueryInterface's SSID (no
            // location consent; see the header).
            if (const nc::WlanConnectionProfileDetails d = p.WlanConnectionProfileDetails()) s.name = d.GetConnectedSsid().c_str();
        }
        if (s.name.empty()) s.name = p.ProfileName().c_str();
        if (wlan || wwan) {
            if (const auto bars = p.GetSignalBars()) s.bars = static_cast<int>(bars.Value());
        }
        if (const nc::ConnectionCost c = p.GetConnectionCost()) {
            const nc::NetworkCostType t = c.NetworkCostType();
            s.metered = t == nc::NetworkCostType::Fixed || t == nc::NetworkCostType::Variable;
        }
    } catch (...) {
        // An RPC to the network list service failed (it restarts, the session ends): unknown.
        s = State{};
        s.kind = L"none";
    }
    return s;
}

void NetworkService::RequestRead(const std::shared_ptr<Shared>& shared) {
    // 0 -> 1 starts a reader; a running one (1) is told to read once more (2).
    int expected = 0;
    if (!shared->reading.compare_exchange_strong(expected, 1)) {
        shared->reading.store(2);
        return;
    }
    auto* keep = new std::shared_ptr<Shared>(shared);
    const BOOL ok = TrySubmitThreadpoolCallback(
        [](PTP_CALLBACK_INSTANCE, void* context) {
            std::unique_ptr<std::shared_ptr<Shared>> owned(static_cast<std::shared_ptr<Shared>*>(context));
            Shared& sh = **owned;
            Crash::Job(
                L"network read",
                [&] {
                    // A Win32 pool thread has no apartment: the WinRT calls need one (the MTA).
                    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    for (;;) {
                        auto* state = new State(Read());
                        ++sh.reads;
                        const HWND h = sh.owner.load();
                        if (!h || !PostMessageW(h, kMessage, 0, reinterpret_cast<LPARAM>(state))) delete state;
                        int one = 1;
                        if (sh.reading.compare_exchange_strong(one, 0)) break;  // nobody asked meanwhile
                        sh.reading.store(1);  // it was 2: once more
                    }
                    if (SUCCEEDED(co)) CoUninitialize();
                },
                [&] { sh.reading.store(0); });
        },
        keep, nullptr);
    if (!ok) {
        delete keep;
        shared->reading.store(0);
    }
}

bool NetworkService::Start(HWND owner) {
    if (Active()) return true;
    owner_ = owner;
    state_ = Read();
    if (!state_.available) return false;
    auto shared = std::make_shared<Shared>();
    shared->owner.store(owner);
    shared->quiet.store(quiet_);
    try {
        // The handler runs on a thread-pool thread and holds only the shared block.
        std::weak_ptr<Shared> weak = shared;
        const winrt::event_token t = nc::NetworkInformation::NetworkStatusChanged([weak](const winrt::Windows::Foundation::IInspectable&) {
            const std::shared_ptr<Shared> sh = weak.lock();
            if (!sh || !sh->owner.load()) return;
            if (sh->quiet.load()) {
                sh->missed.store(true);  // round 26 rule: nothing for nobody; Wake reads once
                return;
            }
            RequestRead(sh);
        });
        token_ = t.value;
    } catch (...) {
        return false;
    }
    shared_ = std::move(shared);
    return true;
}

void NetworkService::Stop() {
    if (!shared_) return;
    try {
        nc::NetworkInformation::NetworkStatusChanged(winrt::event_token{token_});
    } catch (...) {
    }
    token_ = 0;
    shared_->owner.store(nullptr);  // a reader still running posts nothing (its state is freed)
    shared_.reset();
    state_ = State{};
}

void NetworkService::SetQuiet(bool quiet) {
    if (quiet_ == quiet) return;
    quiet_ = quiet;
    if (!shared_) return;
    shared_->quiet.store(quiet);
    if (!quiet && shared_->missed.exchange(false)) RequestRead(shared_);  // something came while quiet
}

std::string NetworkService::Json(const State& s) {
    static const wchar_t* const kStates[] = {L"none", L"local", L"constrained", L"internet"};
    const std::wstring state = s.connected ? kStates[std::clamp(s.level, 0, 3)] : L"none";
    std::wstring o = L"{\"available\":" + std::wstring(s.available ? L"true" : L"false") +
                     L",\"connected\":" + (s.connected ? L"true" : L"false") +
                     L",\"internet\":" + (s.connected && s.level == 3 ? L"true" : L"false") +
                     L",\"state\":" + JsonUtil::Quote(state) + L",\"kind\":" + JsonUtil::Quote(s.kind.empty() ? L"none" : s.kind) +
                     L",\"name\":" + JsonUtil::Quote(s.name) +
                     L",\"bars\":" + (s.bars >= 0 ? std::to_wstring(s.bars) : L"null") +
                     L",\"signal\":" + (s.bars >= 0 ? std::to_wstring(s.bars * 20) : L"null") +
                     L",\"metered\":" + (s.metered ? L"true" : L"false") + L"}";
    return JsonUtil::WideToUtf8(o);
}

uint64_t NetworkService::Reads() const { return shared_ ? shared_->reads.load() : 0; }

bool NetworkService::HandleMessage(UINT msg, WPARAM, LPARAM lp) {
    if (msg != kMessage) return false;
    std::unique_ptr<State> s(reinterpret_cast<State*>(lp));
    if (!s || !Active()) return true;  // stopped since it was posted
    if (*s == state_) return true;     // e.g. a cost or profile event that changed nothing shown
    state_ = *s;
    if (changed_) changed_();
    return true;
}
