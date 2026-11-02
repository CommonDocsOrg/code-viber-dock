#pragma once

#include <windows.h>

#include <string>

#include "AppIdentity.h"

// Custom-icon helpers shared by the dock (item menu) and the settings window (Dock contents page):
// the two pickers, where they start for a pin, and HICONs from image files for the settings list.
namespace IconUtil {

// .png .jpg .jpeg .bmp .gif: decoded with WIC (anything else is read as an icon resource).
bool IsImageFile(const std::wstring& file);

// An image file decoded with WIC, scaled to fit px x px (aspect kept, centred on a transparent
// square) as a 32 bpp alpha icon. The caller destroys it. Null on failure. Needs COM.
HICON HiconFromImageFile(const std::wstring& file, int px);

// Where the pickers start for a pin: its current icon resource (custom, or a shortcut's icon
// location), else the item's own icon source (the app's exe; shell32.dll's folder or Recycle
// Bin icon for those kinds; shell32.dll otherwise). `index` is an icon index (never negative).
void StartIconFor(const AppIdentity::ResolvedPin& pin, std::wstring& file, int& index);
// Where the image picker starts: the pin's current custom icon file when it is an image or an
// .ico (its folder is opened), else empty (the dialog's last folder).
std::wstring StartImageFor(const AppIdentity::ResolvedPin& pin);

// The shell's Change Icon dialog (PickIconDlg: .exe / .dll / .ico with an index), pre-selecting
// file / index. On OK, `spec` is the `icon` value to store: the absolute path ("file,index";
// an .ico with index 0 as the bare path). Returns false when cancelled. Modal to `owner`.
bool ChooseIconResource(HWND owner, const std::wstring& file, int index, std::wstring& spec);

// An open-file dialog for images and icon files (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.ico;*.exe;
// *.dll). An image or an .ico is used as picked; any other file (an .exe, a .dll) opens the
// Change Icon dialog on it to pick the index. `startFile`'s folder is the initial folder when it
// exists. Returns false when cancelled. Modal to `owner`.
bool ChooseIconFile(HWND owner, const std::wstring& startFile, std::wstring& spec);

}  // namespace IconUtil
