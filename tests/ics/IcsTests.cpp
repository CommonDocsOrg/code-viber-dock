// WinDockTests: the ICS parser / expander against hand-written cases (round 4 phase 13).
//
//   WinDockTests.exe [folder]   (default: the folder of this source, tests\ics)
//
// Every <name>.ics with a <name>.expected beside it is parsed and expanded into the window the
// .expected file names ("# window: <from> <to>", UTC "YYYYMMDDTHHMMSSZ"), with the local zone it
// names ("# local: <Windows zone key>"), and the occurrences are compared line by line with the
// file: "<start>/<end> <summary>" in UTC, all-day ones "D<first day>/<day after the last>
// <summary>". Plus a few direct checks of the date / duration helpers. Exit code: failures.

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Ics.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool ReadAll(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

int64_t ParseUtc(const std::string& s) {
    // "YYYYMMDDTHHMMSSZ"
    if (s.size() < 16) return 0;
    const int y = std::stoi(s.substr(0, 4)), m = std::stoi(s.substr(4, 2)), d = std::stoi(s.substr(6, 2));
    const int hh = std::stoi(s.substr(9, 2)), mm = std::stoi(s.substr(11, 2)), ss = std::stoi(s.substr(13, 2));
    return Ics::DaysFromCivil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
}

void Direct() {
    // Day arithmetic round trips over 1600 - 2400.
    bool ok = true;
    for (int64_t day = Ics::DaysFromCivil(1600, 1, 1); day < Ics::DaysFromCivil(2400, 1, 1); day += 13) {
        int y = 0, m = 0, d = 0;
        Ics::CivilFromDays(day, y, m, d);
        if (Ics::DaysFromCivil(y, m, d) != day) ok = false;
    }
    Check(ok, "civil date round trip");
    Check(Ics::WeekdayOf(Ics::DaysFromCivil(2026, 11, 3)) == 1, "2026-11-03 is a Tuesday");
    Check(Ics::DaysInMonth(2028, 2) == 29 && Ics::DaysInMonth(2100, 2) == 28 && Ics::DaysInMonth(2000, 2) == 29, "leap years");
    Ics::Rule r;
    Check(Ics::ParseRule(L"FREQ=MONTHLY;BYDAY=-1FR,2MO;INTERVAL=3;WKST=SU", r) && r.freq == Ics::Rule::Freq::Monthly &&
              r.byDay.size() == 2 && r.byDay[0].ordinal == -1 && r.byDay[0].weekday == 4 && r.byDay[1].ordinal == 2 &&
              r.byDay[1].weekday == 0 && r.interval == 3 && r.wkst == 6,
          "RRULE parse");
    Check(!Ics::ParseRule(L"BYDAY=MO", r), "RRULE without FREQ");
    Check(Ics::WindowsKeyForIana(L"America/New_York") == L"Eastern Standard Time", "IANA table");
    Check(Ics::ResolveZoneName(L"Asia/Kathmandu") != nullptr, "IANA via ICU (Asia/Kathmandu is not in the table)");
    // A minimal calendar: the VALARM offset, text unescaping, no VCALENDAR.
    Ics::Calendar cal;
    Check(Ics::Parse("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:a\r\nDTSTART:20260101T100000Z\r\nSUMMARY:a\\nb\r\n"
                     "BEGIN:VALARM\r\nTRIGGER;RELATED=START:-P1DT2H\r\nEND:VALARM\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n",
                     cal) &&
              cal.events.size() == 1 && cal.events[0].reminderMin == 26 * 60 && cal.events[0].summary == L"a\nb",
          "VALARM offset, escapes");
    Check(!Ics::Parse("hello", cal), "not a calendar");
}

void RunCase(const std::wstring& folder, const std::wstring& name) {
    std::string ics, expected;
    if (!ReadAll(folder + L"\\" + name + L".ics", ics) || !ReadAll(folder + L"\\" + name + L".expected", expected)) {
        std::printf("FAIL %s: cannot read the files\n", Narrow(name).c_str());
        ++g_failures;
        return;
    }
    int64_t from = 0, to = 0;
    std::wstring localKey = L"Pacific Standard Time";
    std::vector<std::string> want;
    std::istringstream in(expected);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("# window: ", 0) == 0) {
            from = ParseUtc(line.substr(10, 16));
            to = ParseUtc(line.substr(27, 16));
        } else if (line.rfind("# local: ", 0) == 0) {
            const std::string k = line.substr(9);
            localKey.assign(k.begin(), k.end());
        } else if (!line.empty() && line[0] != '#') {
            want.push_back(line);
        }
    }
    auto local = Ics::WindowsZone(localKey);
    if (!local) {
        std::printf("FAIL %s: unknown local zone\n", Narrow(name).c_str());
        ++g_failures;
        return;
    }
    Ics::Calendar cal;
    std::wstring error;
    if (!Ics::Parse(ics, cal, &error)) {
        std::printf("FAIL %s: %s\n", Narrow(name).c_str(), Narrow(error).c_str());
        ++g_failures;
        return;
    }
    std::vector<std::string> got;
    for (const Ics::Occurrence& o : Ics::Expand(cal, from, to, *local)) {
        std::wstring s = o.allDay ? L"D" + Ics::FormatDay(o.firstDay) + L"/" + Ics::FormatDay(o.lastDay + 1)
                                  : Ics::FormatUtc(o.start) + L"/" + Ics::FormatUtc(o.end);
        got.push_back(Narrow(s + L" " + cal.events[o.event].summary));
    }
    bool same = got == want;
    std::printf("%s %s: %zu occurrences (%zu expected)\n", same ? "ok  " : "FAIL", Narrow(name).c_str(), got.size(), want.size());
    if (!same) {
        ++g_failures;
        const size_t n = std::max(got.size(), want.size());
        for (size_t i = 0; i < n; ++i) {
            const std::string& g = i < got.size() ? got[i] : std::string("(none)");
            const std::string& w = i < want.size() ? want[i] : std::string("(none)");
            if (g != w) std::printf("  #%zu got  %s\n      want %s\n", i, g.c_str(), w.c_str());
        }
    }
    for (const std::wstring& z : cal.unknownZones) std::printf("  unknown zone %s\n", Narrow(z).c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::wstring folder;
    if (argc > 1) {
        folder = argv[1];
    } else {
        std::string src = __FILE__;
        const size_t cut = src.find_last_of("\\/");
        folder.assign(src.begin(), src.begin() + static_cast<ptrdiff_t>(cut == std::string::npos ? 0 : cut));
    }
    Direct();
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((folder + L"\\*.expected").c_str(), &fd);
    int cases = 0;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            name.resize(name.size() - 9);
            RunCase(folder, name);
            ++cases;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::printf("%d case files, %d failures\n", cases, g_failures);
    return g_failures;
}
