#include "Hotkey.h"

#include <cwchar>
#include <iterator>

namespace Hotkey {

namespace {

struct Named {
    UINT vk;
    const wchar_t* name;
};

// Keys with a name of their own (letters and digits are their character).
const Named kKeys[] = {
    {VK_SPACE, L"Space"},      {VK_TAB, L"Tab"},           {VK_ESCAPE, L"Esc"},         {VK_RETURN, L"Enter"},
    {VK_INSERT, L"Insert"},    {VK_DELETE, L"Delete"},     {VK_HOME, L"Home"},          {VK_END, L"End"},
    {VK_PRIOR, L"PageUp"},     {VK_NEXT, L"PageDown"},     {VK_LEFT, L"Left"},          {VK_UP, L"Up"},
    {VK_RIGHT, L"Right"},      {VK_DOWN, L"Down"},         {VK_PAUSE, L"Pause"},        {VK_SNAPSHOT, L"PrintScreen"},
    {VK_BACK, L"Backspace"},   {VK_MULTIPLY, L"Num*"},     {VK_ADD, L"Num+"},           {VK_SUBTRACT, L"Num-"},
    {VK_DECIMAL, L"Num."},     {VK_DIVIDE, L"Num/"},       {VK_OEM_3, L"`"},            {VK_OEM_MINUS, L"-"},
    {VK_OEM_PLUS, L"="},       {VK_OEM_4, L"["},           {VK_OEM_6, L"]"},            {VK_OEM_5, L"\\"},
    {VK_OEM_1, L";"},          {VK_OEM_7, L"'"},           {VK_OEM_COMMA, L","},        {VK_OEM_PERIOD, L"."},
    {VK_OEM_2, L"/"},
};
// Accepted when read, never written.
const Named kAliases[] = {
    {VK_ESCAPE, L"Escape"}, {VK_RETURN, L"Return"}, {VK_DELETE, L"Del"}, {VK_INSERT, L"Ins"},
    {VK_PRIOR, L"PgUp"},    {VK_NEXT, L"PgDn"},     {VK_SNAPSHOT, L"PrtSc"},
};

bool Same(std::wstring_view a, const wchar_t* b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b, -1, TRUE) == CSTR_EQUAL;
}

UINT ModifierOf(std::wstring_view part) {
    if (Same(part, L"Win") || Same(part, L"Windows")) return MOD_WIN;
    if (Same(part, L"Ctrl") || Same(part, L"Control")) return MOD_CONTROL;
    if (Same(part, L"Alt")) return MOD_ALT;
    if (Same(part, L"Shift")) return MOD_SHIFT;
    return 0;
}

UINT KeyOf(std::wstring_view part) {
    if (part.size() == 1) {
        const wchar_t c = part[0];
        if (c >= L'a' && c <= L'z') return static_cast<UINT>(c - L'a' + L'A');
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return static_cast<UINT>(c);
    }
    if (part.size() >= 2 && part.size() <= 3 && (part[0] == L'F' || part[0] == L'f')) {
        int n = 0;
        for (size_t i = 1; i < part.size(); ++i) {
            if (part[i] < L'0' || part[i] > L'9') return 0;
            n = n * 10 + (part[i] - L'0');
        }
        return n >= 1 && n <= 24 ? static_cast<UINT>(VK_F1 + n - 1) : 0;
    }
    if (part.size() == 4 && (part[0] == L'N' || part[0] == L'n') && Same(part.substr(0, 3), L"Num") && part[3] >= L'0' &&
        part[3] <= L'9') {
        return static_cast<UINT>(VK_NUMPAD0 + (part[3] - L'0'));
    }
    for (const Named& k : kKeys) {
        if (Same(part, k.name)) return k.vk;
    }
    for (const Named& k : kAliases) {
        if (Same(part, k.name)) return k.vk;
    }
    return 0;
}

}  // namespace

bool IsModifierKey(UINT vk) {
    switch (vk) {
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
            return true;
        default:
            return false;
    }
}

std::wstring KeyName(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Num" + std::to_wstring(vk - VK_NUMPAD0);
    for (const Named& k : kKeys) {
        if (k.vk == vk) return k.name;
    }
    return {};
}

bool Parse(std::wstring_view text, Combo& out) {
    Combo c;
    // Split on '+' (only the key name "Num+" contains one, at the very end).
    size_t start = 0;
    bool any = false;
    while (start <= text.size()) {
        size_t end = text.find(L'+', start);
        // "...+Num+": the last '+' belongs to the key's name.
        if (end != std::wstring_view::npos && end >= start + 3 && Same(text.substr(end - 3, 3), L"Num") && end + 1 == text.size()) {
            end = text.size();
        }
        if (end == std::wstring_view::npos) end = text.size();
        std::wstring_view part = text.substr(start, end - start);
        while (!part.empty() && part.front() == L' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == L' ') part.remove_suffix(1);
        if (part.empty()) return false;
        any = true;
        if (const UINT mod = ModifierOf(part)) {
            c.mods |= mod;
        } else {
            const UINT vk = KeyOf(part);
            if (!vk || c.vk) return false;  // unknown, or a second key
            c.vk = vk;
        }
        if (end >= text.size()) break;
        start = end + 1;
    }
    if (!any || !c.vk) return false;
    out = c;
    return true;
}

std::wstring Format(const Combo& combo) {
    const std::wstring key = IsModifierKey(combo.vk) ? std::wstring() : KeyName(combo.vk);
    if (key.empty()) return {};
    std::wstring s;
    if (combo.mods & MOD_WIN) s += L"Win+";
    if (combo.mods & MOD_CONTROL) s += L"Ctrl+";
    if (combo.mods & MOD_ALT) s += L"Alt+";
    if (combo.mods & MOD_SHIFT) s += L"Shift+";
    return s + key;
}

UINT HeldModifiers() {
    UINT mods = 0;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
    if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
    return mods;
}

}  // namespace Hotkey
