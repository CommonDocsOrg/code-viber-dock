#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <winrt/base.h>

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "ClockFaces.h"

// Formats the current time with a strftime-style format and lays it out with DirectWrite.
// Timer scheduling is a one-shot SetTimer aligned to the next minute (or second) boundary.
class Clock {
public:
    // A Windows time zone (EnumDynamicTimeZoneInformation): its key name ("Tokyo Standard Time",
    // what clock.timeZone holds), display name ("(UTC+09:00) Osaka, Sapporo, Tokyo") and standard
    // bias (minutes; UTC = local + bias).
    struct TimeZoneInfo {
        std::wstring key;
        std::wstring display;
        long bias = 0;
    };
    // Every zone of this Windows, sorted as Windows lists them: by UTC offset, then display name.
    static std::vector<TimeZoneInfo> TimeZones();
    // The zone with key name `key` (case-insensitive). False if there is none.
    static bool FindTimeZone(const std::wstring& key, DYNAMIC_TIME_ZONE_INFORMATION& out);
    // Minutes zone `key` is ahead of the local time now ("" / unknown: 0).
    static int ZoneOffsetMinutes(const std::wstring& key);

    void Configure(const std::wstring& format, bool showDate);
    // Round 5 phase A: the zone shown ("" = the local time; an unknown key reads as local) and a
    // label on the date's line (before the date, or alone under the time without it). Call after
    // Configure.
    void SetTimeZone(const std::wstring& key);
    void SetLabel(const std::wstring& label);
    // Minutes the shown zone is ahead of the local time (0 for the local time).
    int OffsetMinutes() const;
    bool Zoned() const { return zoned_; }
    // ARGB colours of the time and date lines (theme.clockText / clockDateText).
    void SetColors(uint32_t text, uint32_t date) {
        textColor_ = text;
        dateColor_ = date;
    }

    // A custom face (ClockFaces, round 4 phase 14) with its colours resolved for the dock's theme;
    // null: the classic rendering (unchanged). `rawFormat` / `showSeconds`: clock.format and
    // clock.showSeconds (a face places the seconds itself). Call after Configure.
    void SetFace(std::shared_ptr<const ClockFace> face, const ClockFaceColors& colors, const std::wstring& rawFormat,
                 bool showSeconds);
    bool HasFace() const { return face_ != nullptr; }

    // Round 20 phase 20A: the item's own look over the face's (or the digital look's): a size
    // factor on every text size (and a face's line gap, padding and corner radius), a font family
    // for every line ("" the face's), the time's weight (0 the face's) and the time line's / the
    // date line's colour (0 = the face's / the theme's; the time's covers its seconds and AM / PM
    // ranges too). Rebuilds the layouts. Call after Configure / SetFace.
    struct Look {
        float scale = 1.0f;
        std::wstring font;
        int weight = 0;
        uint32_t timeColor = 0, dateColor = 0;
        bool operator==(const Look&) const = default;
    };
    void SetLook(const Look& look);
    const Look& GetLook() const { return look_; }

    // Starts/restarts the aligned one-shot timer on hwnd. Call again from WM_TIMER.
    void ScheduleNextTick(HWND hwnd, UINT_PTR timerId) const;
    static void Stop(HWND hwnd, UINT_PTR timerId);

    // Vertical docks: every space of the time and date text breaks the line ("12:34" over "PM"),
    // so the block stays narrow. Rebuilds the layouts when it changes.
    void SetStacked(bool stacked) {
        if (stacked == stacked_) return;
        stacked_ = stacked;
        timeLayout_ = nullptr;
        dateLayout_ = nullptr;
        if (face_) faceLines_.clear();  // the lines depend on it (an inline face stacks)
    }

    // Re-formats the text. Returns true if it changed (layout must be rebuilt).
    bool UpdateText();

    // Builds text layouts for the current text at the given DPI scale. Returns the
    // pixel size of the text block (0x0 if the text is empty or on failure).
    D2D1_SIZE_U BuildLayout(IDWriteFactory* dwrite, float scale);
    void Draw(ID2D1DeviceContext* dc, float x, float y) const;
    // The vertical centre of the last line (the date's, else the time's) in the block, px.
    float LastLineCentre() const {
        if (face_) return faceLastCentre_;
        return dateLayout_ ? timeHeight_ + (static_cast<float>(size_.height) - timeHeight_) / 2.0f
                           : static_cast<float>(size_.height) / 2.0f;
    }
    uint32_t DateColor() const { return dateColor_; }

    void ReleaseLayout();

    // strftime-style formatting that never terminates on a bad specifier (falls back to %H:%M).
    static std::wstring Format(const std::wstring& format, const tm& t);
    // The format shows seconds (%S, %T, %X, %c, %r): the clock ticks every second.
    static bool FormatHasSeconds(const std::wstring& format);
    // clock.format with clock.showSeconds applied: ":%S" right after the last minutes specifier
    // (%M, %#M, %R; none: at the end), unless the format shows seconds already.
    static std::wstring EffectiveFormat(const std::wstring& format, bool showSeconds);

private:
    // --- a custom face ---
    struct FaceRange {
        UINT32 start = 0, length = 0;
        std::wstring font;  // "" the line's
        int weight = 0;     // 0 the line's
        float size = 0.0f;  // DIPs (0 the line's)
        uint32_t color = 0;
    };
    struct FaceLine {
        std::wstring text;
        std::wstring font;
        int weight = 400;
        float size = 16.0f;  // DIPs
        uint32_t color = 0;
        std::vector<FaceRange> ranges;
        bool SameText(const FaceLine& o) const;
        // Built by BuildLayout.
        winrt::com_ptr<IDWriteTextFormat> format;
        std::wstring formatKey;
        winrt::com_ptr<IDWriteTextLayout> layout;
        float y = 0.0f, height = 0.0f;  // px in the block (inside the padding)
    };
    std::vector<FaceLine> FaceTexts(const tm& t) const;
    D2D1_SIZE_U BuildFaceLayout(IDWriteFactory* dwrite, float scale);
    void DrawFace(ID2D1DeviceContext* dc, float x, float y) const;

    std::shared_ptr<const ClockFace> face_;
    ClockFaceColors faceColors_;
    Look look_;  // round 20 phase 20A
    std::wstring rawFormat_ = L"%H:%M";
    bool rawSeconds_ = false;
    std::vector<FaceLine> faceLines_;
    float faceLastCentre_ = 0.0f;
    float padX_ = 0.0f, padY_ = 0.0f;  // px

    // The time now in the shown zone (the local time without one). False on failure.
    bool Now(tm& out) const;
    // The date line is drawn (the date, the label or both).
    bool SecondLine() const { return showDate_ || !label_.empty(); }
    // The date line's text from its date part (label first, " · " between them).
    std::wstring DateLine(const std::wstring& date) const;

    std::wstring format_ = L"%H:%M";
    bool showDate_ = false;
    bool stacked_ = false;
    bool zoned_ = false;
    DYNAMIC_TIME_ZONE_INFORMATION zone_{};
    std::wstring label_;
    uint32_t textColor_ = 0xF2FFFFFF;
    uint32_t dateColor_ = 0xB6FFFFFF;
    std::wstring timeText_;
    std::wstring dateText_;

    float scale_ = 0.0f;
    winrt::com_ptr<IDWriteTextFormat> timeFormat_;
    winrt::com_ptr<IDWriteTextFormat> dateFormat_;
    winrt::com_ptr<IDWriteTextLayout> timeLayout_;
    winrt::com_ptr<IDWriteTextLayout> dateLayout_;
    D2D1_SIZE_U size_{};
    float timeHeight_ = 0.0f;
};
