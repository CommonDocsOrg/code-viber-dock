#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

class WidgetInstance;

// The process-wide part of the widget runtime (owned by DockManager, bound to the app's hidden
// window like the other shared services): one timer scheduler for every widget's setTimeout /
// setInterval / tick (a single one-shot SetTimer for the earliest deadline; none while no widget
// waits for anything), and the list of live widget instances (reload all, automation, the
// Console). Every script runs on the UI thread; each widget instance has its own script runtime
// (WidgetInstance.h).
class WidgetRuntime {
public:
    static constexpr UINT_PTR kTimerId = 30;  // on the owner window

    WidgetRuntime() = default;
    WidgetRuntime(const WidgetRuntime&) = delete;
    WidgetRuntime& operator=(const WidgetRuntime&) = delete;

    void Initialize(HWND owner) { owner_ = owner; }
    void Shutdown();
    // The owner window's messages (WM_TIMER kTimerId). True if handled.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    // A timer of widget `w`: due in `ms` (repeat: every `ms`; aligned: at the next multiple of
    // `ms` of the local time of day, then every multiple). Replaces a timer with the same id.
    void Schedule(WidgetInstance* w, int id, uint32_t ms, bool repeat, bool aligned = false);
    void Cancel(WidgetInstance* w, int id);
    void CancelAll(WidgetInstance* w);
    size_t PendingTimers(const WidgetInstance* w) const;

    void Add(WidgetInstance* w);
    void Remove(WidgetInstance* w);
    const std::vector<WidgetInstance*>& Instances() const { return instances_; }
    bool Alive(const WidgetInstance* w) const;
    // Every widget instance reloads its package and script.
    void ReloadAll();
    // How often the scheduler's timer fired (automation: idle widgets never wake the process).
    uint64_t Wakeups() const { return wakeups_; }

private:
    struct Entry {
        WidgetInstance* w = nullptr;
        int id = 0;
        double due = 0.0;  // NowMs
        uint32_t period = 0;  // 0: one-shot
        bool aligned = false;
    };
    void Arm();
    void OnTimer();

    HWND owner_ = nullptr;
    std::vector<Entry> entries_;
    bool armed_ = false;
    double armedDue_ = 0.0;
    bool firing_ = false;
    uint64_t wakeups_ = 0;
    std::vector<WidgetInstance*> instances_;
};

// The `system` namespace of the widget API: local date / time and locale strings computed by the
// host (not by the script engine: QuickJS's Date applies the current DST offset to every date on
// Windows), in the user's locale.
namespace WidgetSystem {
struct LocalTime {
    int year = 0, month = 0, day = 0;  // month 1 - 12
    int hour = 0, minute = 0, second = 0, millisecond = 0;
    int weekday = 0;   // 0 = Sunday
    int yearDay = 0;   // 1 - 366
    bool dst = false;
    int utcOffset = 0;  // minutes east of UTC
};
LocalTime Now();
// strftime (wcsftime) of the local time now; a bad specifier falls back to "%H:%M" (Clock::Format).
std::wstring Format(const std::wstring& format);
std::wstring Locale();  // "en-US"
bool Hour12();          // the user's time format uses a 12-hour clock
int FirstDayOfWeek();   // 0 = Sunday
// Sunday first / January first.
std::vector<std::wstring> DayNames(bool abbreviated);
std::vector<std::wstring> MonthNames(bool abbreviated);
// The user's long / short date and time formats (GetDateFormatEx / GetTimeFormatEx) for now.
std::wstring FormatDate(bool longFormat);
std::wstring FormatTime(bool seconds);
}  // namespace WidgetSystem

// Monotonic milliseconds (QueryPerformanceCounter).
double WidgetNowMs();
// Milliseconds from now to the next multiple of `period` ms of the local time of day (+15 ms, so
// it lands just after the boundary).
uint32_t WidgetAlignedDelay(uint32_t period);
