#pragma once

#include <windows.h>

#include <string>
#include <vector>

// A small modal text prompt (Rename group…): a title, a label, an edit box with `text`, OK /
// Cancel. Built from an in-memory dialog template (no resource), Segoe UI 9 pt, centred on the
// owner's monitor. Returns true (with the edited, trimmed text) on OK. Edit box control id 100.
namespace InputDialog {

inline constexpr int kEditId = 100;

bool Ask(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& text, size_t maxLength = 128);

// Round 12 phase 12B: a check list (Apply to other docks…): a prompt, one check box per item (ids
// kCheckBase + index, at most 16), OK (greyed while nothing is checked) / Cancel. True on OK with
// `checked` (one entry per item) as the boxes were.
inline constexpr int kCheckBase = 200;
bool Choose(HWND owner, const std::wstring& title, const std::wstring& prompt, const std::vector<std::wstring>& items,
            std::vector<bool>& checked);

}  // namespace InputDialog
