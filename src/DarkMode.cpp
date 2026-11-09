#include "DarkMode.h"

#include <dwmapi.h>
#include <uxtheme.h>

#include <cwchar>
#include <iterator>

namespace DarkMode {

namespace {

constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kAccentKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent";

// Windows 11 SDK values, defined here so older SDK headers still build.
constexpr DWORD kUseImmersiveDarkMode = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kSystemBackdropType = 38;    // DWMWA_SYSTEMBACKDROP_TYPE
constexpr int kBackdropAuto = 0;             // DWMSBT_AUTO
constexpr int kBackdropMainWindow = 2;       // DWMSBT_MAINWINDOW (Mica)

// uxtheme ordinals (Windows 10 1903+). 135's parameter is an enum there (in 1809 it was a BOOL,
// hence the build gate).
enum class PreferredAppMode { Default = 0, AllowDark = 1 };
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI*)();
using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
using IsDarkModeAllowedForWindowFn = bool(WINAPI*)(HWND);

struct Undocumented {
    bool resolved = false;
    SetPreferredAppModeFn setPreferredAppMode = nullptr;
    FlushMenuThemesFn flushMenuThemes = nullptr;
    AllowDarkModeForWindowFn allowDarkModeForWindow = nullptr;
    IsDarkModeAllowedForWindowFn isDarkModeAllowedForWindow = nullptr;  // round 13 phase 13B (ordinal 137, optional)
    bool enabled = false;  // SetPreferredAppMode(AllowDark) is in effect
    // Round 13 phase 13C: EnableDarkMenus turned it on at startup (the app mode was dark): it stays
    // on for the process (TryEnableDarkPopups(false) from the last settings window keeps it).
    bool sticky = false;
};
Undocumented g_ux;

DWORD BuildNumber() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return 0;
    auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fn) return 0;
    OSVERSIONINFOW v{sizeof(v)};
    return fn(&v) == 0 ? v.dwBuildNumber : 0;
}

void ResolveUndocumented() {
    if (g_ux.resolved) return;
    g_ux.resolved = true;
    if (BuildNumber() < 18362) return;
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    g_ux.setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    g_ux.flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    g_ux.allowDarkModeForWindow =
        reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(ux, MAKEINTRESOURCEA(133)));
    g_ux.isDarkModeAllowedForWindow =
        reinterpret_cast<IsDarkModeAllowedForWindowFn>(GetProcAddress(ux, MAKEINTRESOURCEA(137)));
    if (!g_ux.setPreferredAppMode || !g_ux.allowDarkModeForWindow) {
        g_ux.setPreferredAppMode = nullptr;
        g_ux.allowDarkModeForWindow = nullptr;
        g_ux.flushMenuThemes = nullptr;
        g_ux.isDarkModeAllowedForWindow = nullptr;
    }
}

// The system accent palette (8 RGBA entries: light 3, light 2, light 1, base, dark 1 ... dark 3).
// Windows 11 uses light 2 (indicators) / light 3 (text) on dark surfaces and dark 1 / dark 2 on
// light ones.
bool AccentFromPalette(int index, COLORREF& out) {
    BYTE data[32]{};
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, kAccentKey, L"AccentPalette", RRF_RT_REG_BINARY, nullptr, data, &size) !=
            ERROR_SUCCESS ||
        size < 32) {
        return false;
    }
    const BYTE* c = data + index * 4;
    out = RGB(c[0], c[1], c[2]);
    return true;
}

const wchar_t* ThemeNameFor(const wchar_t* cls) {
    if (_wcsicmp(cls, L"ComboBox") == 0 || _wcsicmp(cls, L"ComboLBox") == 0 || _wcsicmp(cls, L"Edit") == 0) {
        return L"DarkMode_CFD";
    }
    if (_wcsicmp(cls, L"msctls_trackbar32") == 0 || _wcsicmp(cls, L"Static") == 0) return nullptr;
    return L"DarkMode_Explorer";  // Button, msctls_updown32, SysLink, SysListView32, ScrollBar
}

}  // namespace

bool AppsUseDarkTheme() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
                     &size) != ERROR_SUCCESS) {
        return false;
    }
    return value == 0;
}

bool HighContrastOn() {
    HIGHCONTRASTW hc{sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

Palette CurrentPalette() {
    Palette p;
    if (HighContrastOn()) {
        p.highContrast = true;
        p.base = p.layer = p.control = GetSysColor(COLOR_WINDOW);
        p.text = p.textSecondary = GetSysColor(COLOR_WINDOWTEXT);
        p.textDisabled = GetSysColor(COLOR_GRAYTEXT);
        p.accent = GetSysColor(COLOR_HIGHLIGHT);
        p.link = GetSysColor(COLOR_HOTLIGHT);
        p.pill = GetSysColor(COLOR_HIGHLIGHT);
        p.pillHoverAlpha = 0x40;
        p.pillSelectedAlpha = 0x80;
        return p;
    }
    if (AppsUseDarkTheme()) {
        p.dark = true;
        p.base = RGB(0x20, 0x20, 0x20);
        p.layer = RGB(0x2B, 0x2B, 0x2B);
        p.text = RGB(0xFF, 0xFF, 0xFF);
        p.textSecondary = RGB(0xC8, 0xC8, 0xC8);
        p.textDisabled = RGB(0x78, 0x78, 0x78);
        p.control = RGB(0x37, 0x37, 0x37);
        p.accent = RGB(0x99, 0xEB, 0xFF);
        p.link = RGB(0x99, 0xEB, 0xFF);
        p.pill = RGB(0xFF, 0xFF, 0xFF);
        p.pillHoverAlpha = 0x0F;
        p.pillSelectedAlpha = 0x15;
    }
    COLORREF accent = 0;
    if (AccentFromPalette(p.dark ? 1 : 4, accent)) p.accent = accent;
    if (AccentFromPalette(p.dark ? 0 : 5, accent)) p.link = accent;
    return p;
}

void SetDarkCaption(HWND hwnd, bool dark) {
    const BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &value, sizeof(value));
}

bool SetMicaBackdrop(HWND hwnd, bool mica) {
    const int type = mica ? kBackdropMainWindow : kBackdropAuto;
    return SUCCEEDED(DwmSetWindowAttribute(hwnd, kSystemBackdropType, &type, sizeof(type)));
}

void ThemeControl(HWND control, bool dark) {
    wchar_t cls[64]{};
    if (!GetClassNameW(control, cls, static_cast<int>(std::size(cls)))) return;
    AllowDarkForWindow(control, dark);
    const wchar_t* name = ThemeNameFor(cls);
    if (!name) return;
    SetWindowTheme(control, dark ? name : nullptr, nullptr);
    if (_wcsicmp(cls, L"ComboBox") == 0) {
        COMBOBOXINFO info{sizeof(info)};
        if (GetComboBoxInfo(control, &info)) {
            if (info.hwndList) {
                // Dark scroll bar only. Not AllowDarkForWindow: on the drop-down list it turns
                // the selection highlight white under the (white) item text (build 26300).
                SetWindowTheme(info.hwndList, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
            }
            if (info.hwndItem && info.hwndItem != control) {
                AllowDarkForWindow(info.hwndItem, dark);
                SetWindowTheme(info.hwndItem, dark ? L"DarkMode_CFD" : nullptr, nullptr);
            }
        }
    }
}

void ThemeScrollBars(HWND hwnd, bool dark) {
    AllowDarkForWindow(hwnd, dark);
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
}

bool TryEnableDarkPopups(bool enable) {
    ResolveUndocumented();
    if (!g_ux.setPreferredAppMode) return false;
    if (!enable && !g_ux.enabled) return true;
    if (!enable && g_ux.sticky) return true;  // round 13 phase 13C: the dock's menus keep following dark mode
    g_ux.setPreferredAppMode(enable ? PreferredAppMode::AllowDark : PreferredAppMode::Default);
    if (g_ux.flushMenuThemes) g_ux.flushMenuThemes();
    g_ux.enabled = enable;
    return true;
}

void AllowDarkForWindow(HWND hwnd, bool allow) {
    if (!g_ux.enabled || !g_ux.allowDarkModeForWindow || !hwnd) return;
    g_ux.allowDarkModeForWindow(hwnd, allow);
}

bool DarkPopupsAvailable() {
    ResolveUndocumented();
    return g_ux.setPreferredAppMode != nullptr;
}

bool EnableDarkMenus() {
    if (HighContrastOn() || !AppsUseDarkTheme()) return false;  // light (or high contrast): as before
    if (!TryEnableDarkPopups(true)) return false;               // flushes the menu themes
    g_ux.sticky = true;
    return true;
}

bool DarkMenusOn() { return g_ux.sticky; }

bool MenusDarkFor(HWND owner) {
    // A popup menu is dark when the process allows dark popups (SetPreferredAppMode(AllowDark):
    // since round 13 phase 13C for good when the app mode was dark at startup, else while a settings
    // window is open), the app mode is dark, and its owner window was allowed
    // (AllowDarkModeForWindow; the dock, folder, widget and app windows since 13C, when dark at
    // startup). Without ordinal 137 the owner's flag is unknown: the windows 13C allows are assumed
    // allowed while the startup switch is on, any other owner light.
    if (!g_ux.enabled || !owner) return false;
    if (HighContrastOn() || !AppsUseDarkTheme()) return false;
    if (!g_ux.isDarkModeAllowedForWindow) return g_ux.sticky;
    return g_ux.isDarkModeAllowedForWindow(owner);
}

}  // namespace DarkMode
