#pragma once

#include <windows.h>

#include "DockItems.h"

// Round 13 phase 13B: the small pictures of the dock's popup menus (ContextMenu), as 32-bit
// premultiplied ARGB DIB sections for MENUITEMINFO::hbmpItem (MIIM_BITMAP). Themed menus (light,
// dark and high contrast) draw such a bitmap with its alpha in the check / icon gutter and dim it
// for a disabled entry. No owner draw: the system keeps drawing the text, keyboard cues, the dark
// theme and the hot-tracking, and a bitmap costs one DIB per glyph per DPI.
//
// Everything runs on the UI thread (the menus are modal on it); nothing here is thread safe.
namespace MenuIcons {

// The menu picture size: 16 DIPs at `dpi`.
UINT SizeFor(UINT dpi);

// The glyph `codepoint` of "Segoe Fluent Icons" (Windows 11; "Segoe MDL2 Assets" when the face is
// not installed and GDI substitutes another one) in `colour`, 16 DIPs square. Cached by
// (codepoint, dpi, colour) until Shutdown(); the menu does not own it. Null on failure.
HBITMAP Glyph(wchar_t codepoint, UINT dpi, COLORREF colour);

// `icon` drawn at 16 DIPs (premultiplied; an icon without an alpha channel gets its alpha from
// the mask). Cached per HICON until ReleaseTransient(); the caller keeps owning `icon`. Null on
// failure.
HBITMAP FromIcon(HICON icon, UINT dpi);

// The exe's own icon (IDI_APP) at 16 DIPs: the dock menu's "App" entry. Cached until Shutdown().
HBITMAP AppIcon(UINT dpi);

// The colour a glyph is drawn in for a menu owned by `owner`: the dark menus' text colour while
// that menu is dark (DarkMode::MenusDarkFor), else GetSysColor(COLOR_MENUTEXT) (light and high
// contrast).
COLORREF TextColour(HWND owner);

// The cheapest HICON there is for a slot's menu header, `px` square or close to it (DrawIconEx
// scales): a non-image `iconFile` (SHDefExtractIconW), the Recycle Bin's stock icon (empty /
// full), the shell's icon for the pin's path (SHGetFileInfoW, the system image list: no
// extraction of its own) or an AUMID's (shell:AppsFolder\<aumid>), last a running window's own
// icon (WM_GETICON with a 50 ms timeout, then the class icon). Null when none of them gives one
// (the caller shows the kind's glyph). `owned`: the caller must DestroyIcon it (window and class
// icons are not owned).
HICON ItemIcon(const DockItem& item, UINT px, bool binEmpty, bool& owned);

// Frees the FromIcon bitmaps (after the menu that used them is destroyed).
void ReleaseTransient();
// Frees every cached bitmap (App::Shutdown).
void Shutdown();

}  // namespace MenuIcons
