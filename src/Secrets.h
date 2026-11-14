#pragma once

#include <string>

// Round 29 phase 29C: the values of a widget's `secret` settings (API keys, tokens) at rest. In
// config.json (and so in its backups, the bad-copy, undo snapshots kept in memory) a secret is
// "dpapi:<base64>": the UTF-8 value protected with DPAPI for the current Windows user
// (CryptProtectData with an entropy of ViberDock's own and CRYPTPROTECT_UI_FORBIDDEN), so the file
// alone, copied to another account or machine, does not give it away. The script gets the plain
// value in `settings` (Widgets::MergeSettings unprotects it). Layout and dock exports drop every
// protected value (Config::DockJson): ciphertext bound to this user is useless elsewhere and still
// a secret here.
//
// What this does not do: hide the value from programs running as the same user (they can call
// CryptUnprotectData too), or from the widget itself (it needs the value). It keeps the key out of
// files people copy, sync and attach to bug reports.
namespace Secrets {

constexpr wchar_t kPrefix[] = L"dpapi:";

bool IsProtected(const std::wstring& stored);
// `plain` -> "dpapi:<base64>". False when DPAPI failed (NoteFailure is set).
bool Protect(const std::wstring& plain, std::wstring& stored);
// "dpapi:<base64>" -> the plain value. False when it is not one or cannot be unprotected (another
// user's, another machine's, damaged): the setting then reads as empty.
bool Unprotect(const std::wstring& stored, std::wstring& plain);

// DPAPI refused to protect a value this session (the value was not saved): true once, for the
// one notice the app shows (App::OnSettingsChanged).
bool TakeFailureNotice();

}  // namespace Secrets
