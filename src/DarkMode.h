#pragma once

#include <windows.h>

// Light / dark chrome for the settings window. Follows the Windows *app* mode
// (Settings > Personalization > Colors > "Choose your default app mode"), not the dock theme.
//
// Documented pieces: DWMWA_USE_IMMERSIVE_DARK_MODE (caption), DWMWA_SYSTEMBACKDROP_TYPE (Mica),
// SetWindowTheme with the "DarkMode_Explorer" / "DarkMode_CFD" visual-style classes (controls),
// WM_CTLCOLOR* with the palette below. Undocumented, optional: uxtheme ordinals 133 / 135 / 136
// for dark popups (edit context menus, combo lists), only through TryEnableDarkPopups() and
// AllowDarkForWindow(). Every piece may be unavailable; the window then simply stays lighter.
namespace DarkMode {

struct Palette {
    bool dark = false;
    bool highContrast = false;
    COLORREF base = RGB(0xF3, 0xF3, 0xF3);      // window / nav column without Mica
    COLORREF layer = RGB(0xFB, 0xFB, 0xFB);     // content pane behind the controls
    COLORREF text = RGB(0x1A, 0x1A, 0x1A);
    COLORREF textSecondary = RGB(0x5D, 0x5D, 0x5D);
    COLORREF textDisabled = RGB(0xA0, 0xA0, 0xA0);
    COLORREF control = RGB(0xFF, 0xFF, 0xFF);   // edit / list background
    COLORREF accent = RGB(0x00, 0x5F, 0xB8);    // nav selection indicator
    COLORREF link = RGB(0x00, 0x3E, 0x92);      // hyperlink text (accent text colour)
    // Nav pills: premultiplied over the nav background (Mica or `base`).
    COLORREF pill = RGB(0, 0, 0);
    BYTE pillHoverAlpha = 0x09;
    BYTE pillSelectedAlpha = 0x0F;
};

// HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme == 0.
bool AppsUseDarkTheme();
bool HighContrastOn();
// The palette for the current system state (high contrast: system colours, never dark).
Palette CurrentPalette();

// Caption colour (DWMWA_USE_IMMERSIVE_DARK_MODE). Harmless where unsupported.
void SetDarkCaption(HWND hwnd, bool dark);
// DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_MAINWINDOW (Mica) or DWMSBT_AUTO. Returns false when the
// attribute is not supported (Windows 10, Windows 11 before 22H2).
bool SetMicaBackdrop(HWND hwnd, bool mica);

// Themes one control for the palette: "DarkMode_Explorer" (buttons, check boxes, radios,
// up-downs, scroll bars, SysLink, ListView), "DarkMode_CFD" (combo boxes and their drop-down
// list, edits); light mode restores the default theme. Trackbars are left alone (no dark theme).
void ThemeControl(HWND control, bool dark);
// A window's own scroll bars.
void ThemeScrollBars(HWND hwnd, bool dark);

// Undocumented (uxtheme ordinal 135 SetPreferredAppMode + 136 FlushMenuThemes): lets popups
// owned by windows that AllowDarkForWindow() follow the app mode. Gated on build >= 18362 and
// resolved with GetProcAddress; returns false (doing nothing) when unavailable. enable = false
// restores the default mode so the dock's own menus are exactly as before. Calling it again
// with true re-flushes the menu themes (after an app-mode change).
bool TryEnableDarkPopups(bool enable);
// Undocumented (ordinal 133 AllowDarkModeForWindow). No-op unless TryEnableDarkPopups succeeded.
void AllowDarkForWindow(HWND hwnd, bool allow);
// The undocumented popup support was found (for diagnostics).
bool DarkPopupsAvailable();
// Round 13 phase 13B: a popup menu owned by `owner` is drawn dark right now (dark popups on, the
// app mode dark, not high contrast, the owner allowed; ordinal 137 IsDarkModeAllowedForWindow).
// MenuIcons picks the glyph colour with it.
bool MenusDarkFor(HWND owner);

}  // namespace DarkMode
