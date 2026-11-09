#pragma once

// Small helpers shared by the config and theme readers / writers: UTF-8 file I/O, typed lookups
// on Windows.Data.Json objects that tolerate missing keys and wrong types, and the pieces of the
// hand-written (deterministic, human-friendly) JSON writer.

#include <Unknwn.h>
#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <cwchar>
#include <limits>
#include <string>
#include <vector>

namespace JsonUtil {

namespace json = winrt::Windows::Data::Json;

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Reads a whole file (< 16 MB) and strips a UTF-8 BOM. False if it cannot be read.
inline bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (16 << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() ||
             (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
    }
    CloseHandle(h);
    if (ok && out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return ok;
}

// Writes `data` to `path` via a temp file + MoveFileEx, so readers never see a partial file.
// Round 12 phase 12A: `error` (optional) gets the Win32 error of the step that failed (0 on
// success), so the app can say why a save failed (a read-only or locked config.json makes the
// MoveFileEx fail with ERROR_ACCESS_DENIED; a full disk the WriteFile with ERROR_DISK_FULL).
inline bool WriteFileAtomic(const std::wstring& path, const std::string& data, DWORD* error = nullptr) {
    if (error) *error = 0;
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = GetLastError();
        return false;
    }
    DWORD written = 0;
    DWORD failed = 0;
    bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    if (!ok) failed = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
    if (!FlushFileBuffers(h) && ok) {
        failed = GetLastError();
        ok = false;
    }
    CloseHandle(h);
    if (ok && !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        failed = GetLastError();
        ok = false;
    }
    if (!ok) DeleteFileW(tmp.c_str());
    if (error && !ok) *error = failed ? failed : ERROR_GEN_FAILURE;
    return ok;
}

// Parses a whole file as a JSON object. False (and `out` null) if unreadable or malformed.
inline bool ReadObjectFile(const std::wstring& path, json::JsonObject& out) {
    out = nullptr;
    std::string bytes;
    if (!ReadFileUtf8(path, bytes)) return false;
    try {
        json::JsonObject root{nullptr};
        if (!json::JsonObject::TryParse(Utf8ToWide(bytes), root) || !root) return false;
        out = root;
        return true;
    } catch (...) {
        return false;
    }
}

// --- typed lookups that tolerate missing keys and wrong types ---
inline json::IJsonValue Find(const json::JsonObject& o, const wchar_t* key) {
    if (!o.HasKey(key)) return nullptr;
    return o.GetNamedValue(key);
}

inline void ReadNumber(const json::JsonObject& o, const wchar_t* key, double& v) {
    auto j = Find(o, key);
    if (j && j.ValueType() == json::JsonValueType::Number) v = j.GetNumber();
}

inline void ReadInt(const json::JsonObject& o, const wchar_t* key, int& v) {
    double d = v;
    ReadNumber(o, key, d);
    if (d > -1e9 && d < 1e9) v = static_cast<int>(d);
}

inline void ReadBool(const json::JsonObject& o, const wchar_t* key, bool& v) {
    auto j = Find(o, key);
    if (j && j.ValueType() == json::JsonValueType::Boolean) v = j.GetBoolean();
}

inline void ReadString(const json::JsonObject& o, const wchar_t* key, std::wstring& v) {
    auto j = Find(o, key);
    if (j && j.ValueType() == json::JsonValueType::String) v = std::wstring(j.GetString());
}

inline json::JsonObject ReadObject(const json::JsonObject& o, const wchar_t* key) {
    auto j = Find(o, key);
    if (j && j.ValueType() == json::JsonValueType::Object) return j.GetObject();
    return nullptr;
}

// Null when the key is missing or not an array.
inline json::JsonArray ReadArray(const json::JsonObject& o, const wchar_t* key) {
    auto j = Find(o, key);
    if (j && j.ValueType() == json::JsonValueType::Array) return j.GetArray();
    return nullptr;
}

// An array of numbers -> `v` (replaced). Elements that are not numbers (e.g. null for a missing
// sample) become NaN, so indices stay aligned with sibling arrays. Missing key or not an array:
// `v` is left unchanged and the result is false.
inline bool ReadNumbers(const json::JsonObject& o, const wchar_t* key, std::vector<double>& v) {
    const json::JsonArray a = ReadArray(o, key);
    if (!a) return false;
    std::vector<double> out;
    out.reserve(a.Size());
    for (const auto& item : a) {
        out.push_back(item.ValueType() == json::JsonValueType::Number ? item.GetNumber()
                                                                       : std::numeric_limits<double>::quiet_NaN());
    }
    v = std::move(out);
    return true;
}

// An array of strings -> `v` (replaced); non-string elements become "". Missing key or not an
// array: `v` is left unchanged and the result is false.
inline bool ReadStrings(const json::JsonObject& o, const wchar_t* key, std::vector<std::wstring>& v) {
    const json::JsonArray a = ReadArray(o, key);
    if (!a) return false;
    std::vector<std::wstring> out;
    out.reserve(a.Size());
    for (const auto& item : a) {
        out.push_back(item.ValueType() == json::JsonValueType::String ? std::wstring(item.GetString())
                                                                       : std::wstring());
    }
    v = std::move(out);
    return true;
}

// --- writer (deterministic key order, human-friendly indentation) ---
inline std::wstring Quote(const std::wstring& s) {
    return std::wstring(json::JsonValue::CreateStringValue(s).Stringify());
}

inline std::wstring Num(double d) {
    wchar_t buf[64];
    swprintf_s(buf, L"%.6g", d);
    return buf;
}

inline std::wstring Bool(bool b) { return b ? L"true" : L"false"; }

}  // namespace JsonUtil
