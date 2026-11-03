#pragma once

#include <windows.h>

#include <string>
#include <string_view>

// Global hotkeys as text (round 6 phase 6A: dashboard.hotkey). "Mod+Mod+Key": the modifiers Win,
// Ctrl, Alt and Shift (any order and case when read; "Windows" / "Control" also accepted), then one
// key: A-Z, 0-9, F1-F24, Space, Tab, Esc, Enter, Insert, Delete, Home, End, PageUp, PageDown, Left,
// Up, Right, Down, Pause, PrintScreen, Num0-Num9, Num*, Num+, Num-, Num., Num/ and the punctuation
// keys ` - = [ ] \ ; ' , . / (the US layout's names for those keys). The canonical spelling
// (Format) lists the modifiers as Win, Ctrl, Alt, Shift.
namespace Hotkey {

struct Combo {
    UINT mods = 0;  // MOD_WIN | MOD_CONTROL | MOD_ALT | MOD_SHIFT
    UINT vk = 0;    // a virtual-key code (never a modifier key)
    bool operator==(const Combo&) const = default;
};

// False for empty text, an unknown name, no key, two keys or a modifier as the key.
bool Parse(std::wstring_view text, Combo& out);
// "" for a key without a name (vk 0, a modifier key, an unknown key).
std::wstring Format(const Combo& combo);
// A hotkey capture box (Settings > General, the dashboard hotkey) carries this window property.
// While it has the keyboard focus, App hands a WM_HOTKEY of its own combination to it as
// kCaptureMessage (wParam: MOD_* bits, lParam: the virtual key) instead of acting on it: the
// registered combination never reaches the box as key messages.
inline constexpr wchar_t kCaptureProp[] = L"WinDock.HotkeyCapture";
inline constexpr UINT kCaptureMessage = WM_APP + 60;

// The modifiers held now (GetKeyState), as MOD_* bits.
UINT HeldModifiers();
// The key's name as Format writes it ("" when it has none).
std::wstring KeyName(UINT vk);
bool IsModifierKey(UINT vk);

}  // namespace Hotkey
