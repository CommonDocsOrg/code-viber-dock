#pragma once

#include <windows.h>

#include <string>

// A small modal text prompt (Rename group…): a title, a label, an edit box with `text`, OK /
// Cancel. Built from an in-memory dialog template (no resource), Segoe UI 9 pt, centred on the
// owner's monitor. Returns true (with the edited, trimmed text) on OK. Edit box control id 100.
namespace InputDialog {

inline constexpr int kEditId = 100;

bool Ask(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& text, size_t maxLength = 128);

}  // namespace InputDialog
