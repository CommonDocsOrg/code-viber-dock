#include "Secrets.h"

#include <windows.h>
#include <dpapi.h>
#include <wincrypt.h>

#include <atomic>
#include <cstring>
#include <vector>

#include "JsonUtil.h"

namespace Secrets {

namespace {

// Ties the blobs to this use: another program's CryptUnprotectData of the same user needs it too
// (not a secret: it only keeps a DPAPI blob of another program from being taken for one of ours).
constexpr char kEntropy[] = "ViberDock widget secret setting v1";

std::atomic<bool> g_failed{false};
std::atomic<bool> g_told{false};

}  // namespace

bool IsProtected(const std::wstring& stored) { return stored.rfind(kPrefix, 0) == 0; }

bool Protect(const std::wstring& plain, std::wstring& stored) {
    stored.clear();
    const std::string utf8 = JsonUtil::WideToUtf8(plain);
    DATA_BLOB in{static_cast<DWORD>(utf8.size()), reinterpret_cast<BYTE*>(const_cast<char*>(utf8.data()))};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"ViberDock widget setting", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        g_failed = true;
        return false;
    }
    DWORD chars = 0;
    bool ok = CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &chars) != FALSE;
    std::wstring b64(chars, L'\0');
    ok = ok && CryptBinaryToStringW(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &chars);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (!ok) {
        g_failed = true;
        return false;
    }
    b64.resize(chars);
    stored = std::wstring(kPrefix) + b64;
    return true;
}

bool Unprotect(const std::wstring& stored, std::wstring& plain) {
    plain.clear();
    if (!IsProtected(stored) || stored.size() > 16384) return false;
    const std::wstring b64 = stored.substr(std::wcslen(kPrefix));
    DWORD bytes = 0;
    if (!CryptStringToBinaryW(b64.c_str(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr)) return false;
    std::vector<BYTE> blob(bytes);
    if (!CryptStringToBinaryW(b64.c_str(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64, blob.data(), &bytes, nullptr, nullptr)) {
        return false;
    }
    DATA_BLOB in{bytes, blob.data()};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    const std::string utf8(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    plain = JsonUtil::Utf8ToWide(utf8);
    return true;
}

bool TakeFailureNotice() {
    if (!g_failed || g_told.exchange(true)) return false;
    return true;
}

}  // namespace Secrets
