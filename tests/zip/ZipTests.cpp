// Round 29 phase 29B: the zip reader (src/Zip.cpp) against the cases of tests/zip (written by
// make_cases.py): every <name>.zip with a <name>.expected beside it is read with the widget
// installer's limits and each file entry extracted; the result must be exactly the expected list
// ("ok" and "<name> <size> <crc32>" per file) or an error containing the expected text. Plus a few
// direct checks of NormalizeName and Inflate. Called by WinDockTests' wmain (IcsTests.cpp).

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Zip.h"

namespace {

int g_zipFailures = 0;

void Expect(bool ok, const std::string& what) {
    if (!ok) {
        ++g_zipFailures;
        std::printf("FAIL zip: %s\n", what.c_str());
    }
}

std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string ReadAll(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

void Direct() {
    std::string out, error;
    Expect(Zip::NormalizeName("w\\lib\\a.js", false, out, error) && out == "w/lib/a.js", "a backslash name is read as a package path");
    Expect(Zip::NormalizeName("w/assets/", false, out, error) && out == "w/assets/", "a folder keeps its slash");
    Expect(!Zip::NormalizeName("w/./a.js", false, out, error), "a '.' segment is refused");
    Expect(!Zip::NormalizeName("lpt9.js", false, out, error), "LPT9 with an extension is reserved");
    Expect(Zip::NormalizeName("console.js", false, out, error), "console.js is not a device name");
    Expect(!Zip::NormalizeName("w/a ", false, out, error), "a trailing space is refused");
    // Inflate: a fixed-code block "abc" made by hand (zlib: compressobj(9, DEFLATED, -15) of b"abc").
    const unsigned char abc[] = {0x4B, 0x4C, 0x4A, 0x06, 0x00};
    out.clear();
    Expect(Zip::Inflate(abc, sizeof(abc), out, 16, error) && out == "abc", "inflate a fixed block");
    out.clear();
    Expect(!Zip::Inflate(abc, sizeof(abc), out, 2, error) && out.empty(), "the output cap stops the decoder (and nothing is kept)");
    out.clear();
    Expect(!Zip::Inflate(abc, 2, out, 16, error), "truncated input fails");
    const unsigned char badType[] = {0x07};  // last block, type 3
    out.clear();
    Expect(!Zip::Inflate(badType, sizeof(badType), out, 16, error), "block type 3 fails");
    const unsigned char stored[] = {0x01, 0x03, 0x00, 0xFC, 0xFF, 'x', 'y', 'z'};
    out.clear();
    Expect(Zip::Inflate(stored, sizeof(stored), out, 16, error) && out == "xyz", "inflate a stored block");
    const unsigned char storedBad[] = {0x01, 0x03, 0x00, 0xFC, 0xFE, 'x', 'y', 'z'};
    out.clear();
    Expect(!Zip::Inflate(storedBad, sizeof(storedBad), out, 16, error), "a stored block with a wrong complement fails");
    Expect(Zip::Crc32("123456789", 9) == 0xCBF43926u, "CRC-32 check value");
}

void RunCase(const std::wstring& folder, const std::wstring& name) {
    const std::string archive = ReadAll(folder + L"\\" + name + L".zip");
    std::istringstream expected(ReadAll(folder + L"\\" + name + L".expected"));
    // A checkout may give the .expected files CRLF line ends (core.autocrlf): read them without the CR.
    auto line = [&](std::string& s) {
        if (!std::getline(expected, s)) return false;
        if (!s.empty() && s.back() == '\r') s.pop_back();
        return true;
    };
    std::string first;
    line(first);
    std::vector<std::string> got;
    std::string error;
    std::vector<Zip::Entry> entries;
    Zip::Limits limits;  // the widget installer's (Widgets::PrepareZipInstall uses the defaults)
    bool ok = Zip::ReadDirectory(archive, limits, entries, error);
    for (size_t i = 0; ok && i < entries.size(); ++i) {
        if (entries[i].directory) continue;
        std::string data;
        ok = Zip::Extract(archive, entries[i], data, error);
        if (ok) {
            char tail[64];
            std::snprintf(tail, sizeof(tail), " %zu %08x", data.size(), Zip::Crc32(data.data(), data.size()));
            got.push_back(entries[i].name + tail);
        }
    }
    const std::string label = Narrow(name);
    if (first.rfind("error ", 0) == 0) {
        const std::string want = first.substr(6);
        Expect(!ok, label + ": expected an error (" + want + "), it was read");
        if (!ok) Expect(error.find(want) != std::string::npos, label + ": error \"" + error + "\" lacks \"" + want + "\"");
        std::printf("%s %s: %s\n", !ok && error.find(want) != std::string::npos ? "ok  " : "FAIL", label.c_str(), error.c_str());
        return;
    }
    Expect(ok, label + ": " + error);
    std::vector<std::string> want;
    for (std::string s; line(s);) {
        if (!s.empty()) want.push_back(s);
    }
    Expect(got == want, label + ": the extracted files differ");
    std::printf("%s %s: %zu files\n", ok && got == want ? "ok  " : "FAIL", label.c_str(), got.size());
}

}  // namespace

// The cases of `folder`; returns the failures.
int RunZipTests(const std::wstring& folder, int& cases) {
    Direct();
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((folder + L"\\*.expected").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            name.resize(name.size() - 9);
            RunCase(folder, name);
            ++cases;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return g_zipFailures;
}
