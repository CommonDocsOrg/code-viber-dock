#include "InputService.h"

#include <shlwapi.h>

#include <algorithm>
#include <cwctype>

#include "JsonUtil.h"

namespace {

std::wstring LocaleInfo(const wchar_t* tag, LCTYPE type) {
    wchar_t buf[LOCALE_NAME_MAX_LENGTH * 2]{};
    if (!tag || !*tag || GetLocaleInfoEx(tag, type, buf, static_cast<int>(std::size(buf))) <= 0) return {};
    return buf;
}

std::wstring Hex8(uint32_t v) {
    wchar_t buf[12];
    swprintf_s(buf, L"%08X", v);
    return buf;
}

// The registry's name of a keyboard layout: HKLM\SYSTEM\CurrentControlSet\Control\Keyboard Layouts\
// <KLID> "Layout Display Name" (an indirect "@%SystemRoot%\system32\input.dll,-5000" string), else
// its "Layout Text".
std::wstring LayoutName(const std::wstring& klid) {
    HKEY key = nullptr;
    const std::wstring path = L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\" + klid;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
    std::wstring out;
    wchar_t buf[512]{};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(key, nullptr, L"Layout Display Name", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &size) ==
            ERROR_SUCCESS &&
        buf[0] == L'@') {
        wchar_t shown[256]{};
        if (SUCCEEDED(SHLoadIndirectString(buf, shown, static_cast<UINT>(std::size(shown)), nullptr))) out = shown;
    }
    if (out.empty()) {
        size = sizeof(buf) - sizeof(wchar_t);
        if (RegGetValueW(key, nullptr, L"Layout Text", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) out = buf;
    }
    RegCloseKey(key);
    return out;
}

// The KLID ("00000409", "00010409", "E0010411") of an HKL. The high word is the device handle: a
// plain layout's KLID is that word (0000xxxx); 0xFnnn names a layout variant by its "Layout Id"
// (looked up among the Keyboard Layouts keys); 0xEnnn is an IME, whose KLID is the whole HKL.
// (Documented in parts only: KLIDs on "Keyboard Identifiers and Input Method Editors", the
// device-handle encoding is how Windows has built HKLs since NT 4; it only picks a display name.)
std::wstring KlidOf(HKL hkl) {
    const auto v = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hkl));
    const uint32_t device = v >> 16;
    if ((device & 0xF000) == 0xE000) return Hex8(v);
    if ((device & 0xF000) != 0xF000) return Hex8(device);
    const uint32_t layoutId = device & 0x0FFF;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts", 0, KEY_READ, &root) != ERROR_SUCCESS) {
        return Hex8(v & 0xFFFF);
    }
    std::wstring found;
    wchar_t name[16];
    for (DWORD i = 0; found.empty(); ++i) {
        DWORD n = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(root, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t id[16]{};
        DWORD size = sizeof(id) - sizeof(wchar_t);
        if (RegGetValueW(root, name, L"Layout Id", RRF_RT_REG_SZ, nullptr, id, &size) != ERROR_SUCCESS) continue;
        if (wcstoul(id, nullptr, 16) == layoutId && (wcstoul(name, nullptr, 16) & 0xFFFF) == (v & 0xFFFF)) found = name;
    }
    RegCloseKey(root);
    return found.empty() ? Hex8(v & 0xFFFF) : found;
}

}  // namespace

bool InputService::Start() {
    if (active_) return true;
    active_ = true;
    missed_ = false;
    Refresh(false);
    return true;
}

void InputService::Stop() {
    active_ = false;
    state_ = State{};
    lastForeign_ = nullptr;
    missed_ = false;
}

HWND InputService::Target() const {
    const HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (fg && pid != GetCurrentProcessId()) return fg;
    return lastForeign_ && IsWindow(lastForeign_) ? lastForeign_ : fg;
}

const InputService::Layout& InputService::Describe(HKL hkl) {
    auto it = names_.find(hkl);
    if (it != names_.end()) return it->second;
    Layout l;
    l.hkl = hkl;
    l.id = Hex8(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hkl)));
    wchar_t tag[LOCALE_NAME_MAX_LENGTH]{};
    const LANGID lang = LOWORD(reinterpret_cast<uintptr_t>(hkl));
    if (LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), tag, LOCALE_NAME_MAX_LENGTH, 0) > 0) l.tag = tag;
    l.code = LocaleInfo(tag, LOCALE_SISO639LANGNAME2);
    for (wchar_t& c : l.code) c = static_cast<wchar_t>(std::towupper(c));
    l.region = LocaleInfo(tag, LOCALE_SISO3166CTRYNAME);
    l.language = LocaleInfo(tag, LOCALE_SLOCALIZEDDISPLAYNAME);
    l.layout = LayoutName(KlidOf(hkl));
    if (l.code.empty()) l.code = l.tag.empty() ? l.id.substr(4) : l.tag.substr(0, 2);
    if (l.language.empty()) l.language = l.tag.empty() ? l.id : l.tag;
    return names_.emplace(hkl, std::move(l)).first->second;
}

InputService::State InputService::Read() {
    State s;
    const HWND target = Target();
    const DWORD thread = target ? GetWindowThreadProcessId(target, nullptr) : 0;
    const HKL now = GetKeyboardLayout(thread);
    HKL list[64]{};
    const int n = GetKeyboardLayoutList(static_cast<int>(std::size(list)), list);
    s.available = now != nullptr || n > 0;
    for (int i = 0; i < n; ++i) {
        s.layouts.push_back(Describe(list[i]));
        if (list[i] == now) s.current = i;
    }
    if (now) s.active = Describe(now);
    ++refreshes_;
    return s;
}

void InputService::Refresh(bool notify) {
    State s = Read();
    if (s == state_ && state_.available) return;
    state_ = std::move(s);
    if (notify && changed_) changed_();
}

const InputService::State& InputService::Get() {
    if (active_) Refresh(true);
    return state_;
}

void InputService::OnShellEvent(WPARAM code, LPARAM lp) {
    if (!active_) return;
    if (code == HSHELL_WINDOWACTIVATED || code == HSHELL_RUDEAPPACTIVATED) {
        const HWND h = reinterpret_cast<HWND>(lp);
        DWORD pid = 0;
        if (h) GetWindowThreadProcessId(h, &pid);
        if (h && pid != GetCurrentProcessId()) lastForeign_ = h;
    } else if (code != HSHELL_LANGUAGE) {
        return;
    }
    if (quiet_) {
        missed_ = true;  // round 26 rule: nothing for nobody; SetQuiet(false) reads once
        return;
    }
    Refresh(true);
}

void InputService::SetQuiet(bool quiet) {
    if (quiet_ == quiet) return;
    quiet_ = quiet;
    if (!quiet && active_ && missed_) {
        missed_ = false;
        Refresh(true);
    }
}

std::string InputService::Json(const State& s) {
    auto layout = [](const Layout& l) {
        return L"{\"id\":" + JsonUtil::Quote(l.id) + L",\"tag\":" + JsonUtil::Quote(l.tag) + L",\"code\":" + JsonUtil::Quote(l.code) +
               L",\"region\":" + JsonUtil::Quote(l.region) + L",\"language\":" + JsonUtil::Quote(l.language) +
               L",\"layout\":" + JsonUtil::Quote(l.layout);
    };
    std::wstring o = L"{\"available\":" + std::wstring(s.available ? L"true" : L"false");
    // The active layout's fields at the top level (what an indicator draws), then the list.
    const Layout& a = s.active;
    o += L",\"id\":" + JsonUtil::Quote(a.id) + L",\"tag\":" + JsonUtil::Quote(a.tag) + L",\"code\":" + JsonUtil::Quote(a.code) +
         L",\"region\":" + JsonUtil::Quote(a.region) + L",\"language\":" + JsonUtil::Quote(a.language) +
         L",\"layout\":" + JsonUtil::Quote(a.layout) + L",\"current\":" + std::to_wstring(s.current) + L",\"layouts\":[";
    for (size_t i = 0; i < s.layouts.size(); ++i) {
        o += (i ? L"," : L"") + layout(s.layouts[i]) + L",\"current\":" + (static_cast<int>(i) == s.current ? L"true" : L"false") + L"}";
    }
    o += L"]}";
    return JsonUtil::WideToUtf8(o);
}

bool InputService::Select(const std::wstring& idOrTag, std::wstring& error) {
    if (!active_) Start();
    Refresh(true);
    const Layout* pick = nullptr;
    for (const Layout& l : state_.layouts) {
        if (_wcsicmp(l.id.c_str(), idOrTag.c_str()) == 0) pick = &l;
    }
    for (const Layout& l : state_.layouts) {
        if (!pick && _wcsicmp(l.tag.c_str(), idOrTag.c_str()) == 0) pick = &l;
    }
    if (!pick) {
        error = L"input.set: \"" + idOrTag + L"\" is not an installed layout (an id or tag from input.get().layouts)";
        return false;
    }
    const HWND target = Target();
    if (!target) {
        error = L"input.set: no window to switch";
        return false;
    }
    if (!PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(pick->hkl))) {
        error = L"input.set: Windows refused the request (" + std::to_wstring(GetLastError()) + L"; an app running as administrator?)";
        return false;
    }
    return true;  // the app switches when it handles the message; onChange follows when Windows reports it
}
