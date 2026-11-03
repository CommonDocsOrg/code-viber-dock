#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// iCalendar (RFC 5545) for the calendar feeds (round 4 phase 13): our own parser and recurrence
// expander, no dependency. The subset:
//  - content lines: unfolding (CRLF or LF + space / tab), parameters (quoted values too), text
//    escapes (\n \N \, \; \\), UTF-8 with or without a BOM;
//  - VEVENT: UID, SUMMARY, DESCRIPTION, LOCATION, URL, DTSTART, DTEND, DURATION, STATUS
//    (CANCELLED), RRULE, EXDATE, RDATE (dates / date-times; a PERIOD's start), RECURRENCE-ID
//    (overrides of one instance; RANGE=THISANDFUTURE is read as that one instance), VALARM (only
//    the first TRIGGER's offset before the start, as a reminder hint);
//  - times: DATE (all-day), UTC ("Z"), floating (the calendar's X-WR-TIMEZONE if it names a zone we
//    know, else the local zone), TZID with the file's VTIMEZONE (its STANDARD / DAYLIGHT
//    observances: DTSTART, TZOFFSETFROM / TO, RRULE, RDATE). A TZID without a VTIMEZONE falls
//    back to the Windows time zone of that name: a Windows key itself ("W. Europe Standard
//    Time"), an IANA name through a built-in table of common zones, then through ICU (icu.dll's
//    ucal_getWindowsTimeZoneID, Windows 10 1903+), "/mozilla.org/.../Europe/Berlin" style
//    prefixes stripped; a name that still is unknown is read as the local zone (Calendar::unknownZones);
//  - RRULE: FREQ DAILY / WEEKLY / MONTHLY / YEARLY, INTERVAL, COUNT, UNTIL, BYDAY (ordinals
//    such as 2MO, -1FR in MONTHLY / YEARLY), BYMONTHDAY (negative too), BYMONTH, BYYEARDAY,
//    BYSETPOS, WKST. Not supported: SECONDLY / MINUTELY / HOURLY (the event shows once), BYWEEKNO,
//    BYHOUR / BYMINUTE / BYSECOND (ignored: one instance per day at DTSTART's time).
// Nonexistent local times (a spring-forward gap) use the offset before the gap, ambiguous ones the
// first (daylight) occurrence (RFC 5545 3.3.5).
namespace Ics {

// Days since 1970-01-01 (proleptic Gregorian) <-> a civil date.
int64_t DaysFromCivil(int y, int m, int d);
void CivilFromDays(int64_t days, int& y, int& m, int& d);
int DaysInMonth(int y, int m);
// 0 = Monday ... 6 = Sunday (RFC 5545 MO ... SU).
int WeekdayOf(int64_t days);
inline int64_t FloorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }
// Wall-clock seconds (days * 86400 + seconds of the day) -> its day.
inline int64_t DayOf(int64_t wall) { return FloorDiv(wall, 86400); }

// A time zone: wall-clock seconds <-> UTC seconds (both counted from 1970-01-01 00:00).
class Zone {
public:
    virtual ~Zone() = default;
    virtual int64_t ToUtc(int64_t wall) const = 0;
    virtual int64_t ToWall(int64_t utc) const = 0;
};
std::shared_ptr<const Zone> UtcZone();
// The Windows time zone with this registry key name ("Pacific Standard Time"); null if unknown.
std::shared_ptr<const Zone> WindowsZone(const std::wstring& key);
// The machine's current time zone (its dynamic DST rules).
std::shared_ptr<const Zone> LocalZone();
// A TZID without a VTIMEZONE: a Windows key, an IANA name (table, then ICU), prefixes stripped.
// Null if unknown.
std::shared_ptr<const Zone> ResolveZoneName(const std::wstring& tzid);
// The Windows key of an IANA zone name ("" unknown): the built-in table, then ICU.
std::wstring WindowsKeyForIana(const std::wstring& iana);

struct Time {
    enum class Kind { None, Date, Utc, Floating, Zoned };
    Kind kind = Kind::None;
    int64_t wall = 0;   // Date: days * 86400; Utc: UTC seconds; else wall-clock seconds
    std::wstring tzid;  // Zoned
    bool Valid() const { return kind != Kind::None; }
    bool IsDate() const { return kind == Kind::Date; }
};

struct Rule {
    enum class Freq { None, Daily, Weekly, Monthly, Yearly, Unsupported };
    Freq freq = Freq::None;
    int interval = 1;
    int count = 0;  // 0: no COUNT
    Time until;     // None: no UNTIL
    struct Day {
        int ordinal = 0;  // 0: every such weekday; 2MO = 2, -1FR = -1
        int weekday = 0;  // 0 = MO ... 6 = SU
    };
    std::vector<Day> byDay;
    std::vector<int> byMonthDay, byMonth, byYearDay, bySetPos;
    int wkst = 0;
};
// Parses an RRULE value ("FREQ=WEEKLY;BYDAY=MO,WE"). False if it has no usable FREQ.
bool ParseRule(const std::wstring& value, Rule& out);

struct Event {
    std::wstring uid, summary, description, location, url;
    Time start, end;
    bool hasDuration = false;
    int64_t durationDays = 0;  // DURATION's nominal days / weeks part (wall-clock days)
    int64_t durationSecs = 0;  // ... and its exact part
    bool hasRule = false;
    Rule rule;
    std::vector<Time> exdates, rdates;
    Time recurrenceId;  // set: an override of one instance of the event with this UID
    bool cancelled = false;
    int reminderMin = -1;  // the first VALARM's trigger, minutes before the start (-1: none)
    bool IsOverride() const { return recurrenceId.Valid(); }
};

struct Calendar {
    std::wstring name;      // X-WR-CALNAME
    std::wstring floatingZone;  // X-WR-TIMEZONE
    std::shared_ptr<const Zone> floating;  // ... resolved (null: floating times are local)
    std::vector<Event> events;  // masters and overrides, file order
    std::map<std::wstring, std::shared_ptr<const Zone>> zones;  // every TZID used (VTIMEZONE or fallback)
    std::vector<std::wstring> fallbackZones;  // TZIDs resolved through the Windows zones
    std::vector<std::wstring> unknownZones;   // TZIDs read as the local zone
    size_t skipped = 0;     // malformed lines / properties ignored
};

// Parses an iCalendar text. False (and `error`) if it holds no VCALENDAR. Never throws.
bool Parse(const std::string& text, Calendar& out, std::wstring* error = nullptr);

struct Occurrence {
    int64_t start = 0, end = 0;  // UTC seconds; all-day: the local midnights of the dates
    int64_t firstDay = 0;        // the local days it touches, inclusive
    int64_t lastDay = 0;
    int64_t originalStart = 0;   // UTC (all-day: the date * 86400) of the instance before an override
    uint32_t event = 0;          // index into Calendar::events (the override's when overridden)
    bool allDay = false;
    bool recurring = false;
};

// The occurrences overlapping [from, to) (UTC seconds), sorted by start (all-day first on the same
// start). `local` is the zone of all-day dates and of floating times. At most `limit` (the rest
// are dropped, the earliest kept).
std::vector<Occurrence> Expand(const Calendar& cal, int64_t from, int64_t to, const Zone& local, size_t limit = 20000);

// Helpers shared with the tests and the service.
// "20261103T090000Z" style text of a UTC instant / "20261103" of a day.
std::wstring FormatUtc(int64_t utc);
std::wstring FormatDay(int64_t day);

}  // namespace Ics
