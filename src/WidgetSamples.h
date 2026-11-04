#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Round 6 phase 6E: the sample data a widget's providers answer with in preview mode (the settings
// window's widget drawer and Widget library: WidgetPreview, dock.preview). Every answer has exactly
// the shape of the real provider's (docs/widgets.md); none of them starts a service or reads the
// machine. The numbers are fixed (series: a deterministic sine wave with noise), so two renders of
// the same widget at the same minute are the same picture. Edit them here: docs/widgets.md
// "Preview mode" lists them.
namespace WidgetSamples {

// audio.get(): 62 %, not muted, "Speakers (Sample)", a microphone at 80 %.
std::string AudioJson();

// media.get(): "Sample Song" by "Sample Artist" from "Sample Album", playing, 73 s of 214.
std::string MediaJson();
// media.art(): the token's serial ("media:art/1"), and the art itself: a 64 x 64 gradient square
// (a 24-bit BMP, decoded by the canvas like a package image).
constexpr uint64_t kArtSerial = 1;
const std::string& MediaArtBytes();

// The notifications provider's state: polling, 3 unread items (two from "Mail", 5 and 20 minutes
// ago, one from "Calendar", an hour ago), newest first.
std::string NotificationsJson();

// The calendar provider: one feed "Sample" in the first feed colour; today "Stand-up" 09:30 - 09:45
// and "Lunch with Sam" 12:30 - 13:30 (local time); other days are empty for eventsOn, while next()
// rolls over to tomorrow's copies once today's are over (the Agenda never previews "No events").
// `what`: "feeds", "events", "eventsOn", "next", "refresh", "openPopup" (the last two: null).
bool CalendarCall(const std::string& what, const std::string& args, std::string& out, std::wstring& error);

// weather.get(): sunny (code 0), 21 °C / 70 °F, "Sample City", fresh (fetched 5 minutes ago),
// 24 hourly columns from the current hour and 7 days from today. `label`: the widget's own label.
std::string WeatherJson(bool imperial, const std::wstring& label);

// perf.get() (serial 1), perf.metrics() (serial 1), perf.hwinfo() (not available: "preview"), and
// perf.series(key, n): up to 60 samples ending at the snapshot's value.
std::string PerfSnapshotJson();
std::string PerfMetricsJson();
std::string PerfHwinfoJson();
void PerfSeries(const std::string& key, size_t n, std::vector<double>& out);

}  // namespace WidgetSamples
