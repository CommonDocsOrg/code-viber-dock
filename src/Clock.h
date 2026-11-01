#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <winrt/base.h>

#include <ctime>
#include <string>

// Formats the current time with a strftime-style format and lays it out with DirectWrite.
// Timer scheduling is a one-shot SetTimer aligned to the next minute (or second) boundary.
class Clock {
public:
    void Configure(const std::wstring& format, bool showDate);

    // Starts/restarts the aligned one-shot timer on hwnd. Call again from WM_TIMER.
    void ScheduleNextTick(HWND hwnd, UINT_PTR timerId) const;
    static void Stop(HWND hwnd, UINT_PTR timerId);

    // Re-formats the text. Returns true if it changed (layout must be rebuilt).
    bool UpdateText();

    // Builds text layouts for the current text at the given DPI scale. Returns the
    // pixel size of the text block (0x0 if the text is empty or on failure).
    D2D1_SIZE_U BuildLayout(IDWriteFactory* dwrite, float scale);
    void Draw(ID2D1DeviceContext* dc, float x, float y) const;

    void ReleaseLayout();

private:
    static std::wstring Format(const std::wstring& format, const tm& t);
    bool HasSeconds() const;

    std::wstring format_ = L"%H:%M";
    bool showDate_ = false;
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
