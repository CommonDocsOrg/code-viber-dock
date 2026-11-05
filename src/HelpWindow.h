#pragma once

// Round 9 phase 9D: the Help window (WinDock.Help, one per process): the guides of docs/ (embedded as
// RCDATA, IDR_DOC_*) listed at the left, the chosen one at the right in a read-only RichEdit fed by
// a small Markdown -> RTF converter (headings, paragraphs, bullet and numbered lists, bold, italic,
// inline and fenced code, links). A link to another guide ("guide-items.md#the-clock") opens it in
// the window at that heading (GitHub-style slugs); a web link opens the browser. Opened by F1 in a
// settings window (the page's topic), the cards' "?" buttons, the Global nav's Help row, the dock
// menu's Help…, About's Quick start guide and DockCommand 154. Nothing of it exists while it is
// closed; it runs no timers.

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "DarkMode.h"

namespace HelpDoc {

// The guides in list order (DockCommand 154's LOWORD, QueryState 287): 0 quick-start.md,
// 1 guide-settings.md, 2 guide-docks.md, 3 guide-items.md, 4 troubleshooting.md, 5 README.md (the
// index of the guides).
struct Guide {
    const wchar_t* file;   // "guide-settings.md"
    const wchar_t* title;  // "Settings"
    int resource;          // IDR_DOC_*
};
const std::vector<Guide>& Guides();
// The guide's index for a file name ("guide-items.md", case-insensitive; -1: not a guide).
int GuideIndex(const std::wstring& file);
// A heading's anchor as GitHub makes it: lower case, punctuation other than '-' / '_' dropped,
// spaces to '-'.
std::wstring Slug(const std::wstring& heading);

// A guide converted for the RichEdit: the RTF, and the headings and links with their character
// positions in the text the RichEdit holds (every paragraph ends with one character).
struct Heading {
    std::wstring text;
    std::wstring slug;  // made unique in the file as GitHub does ("-1", "-2" ...)
    int level = 1;
    LONG cp = 0;
};
struct Link {
    std::wstring text;
    std::wstring target;  // as written: "guide-x.md#a", "#a", "https://…"
    LONG cpMin = 0, cpMax = 0;
};
struct Rendered {
    std::string rtf;
    std::wstring plain;  // the text as the RichEdit should hold it ('\r' per paragraph)
    std::vector<Heading> headings;
    std::vector<Link> links;
};
struct Colors {
    COLORREF text, secondary, link, code, codeBack;
};
Rendered Render(const std::string& markdownUtf8, const Colors& colors);

}  // namespace HelpDoc

class HelpWindow {
public:
    static constexpr wchar_t kClassName[] = L"WinDock.Help";

    explicit HelpWindow(std::function<void()> destroyed);
    ~HelpWindow();
    HelpWindow(const HelpWindow&) = delete;
    HelpWindow& operator=(const HelpWindow&) = delete;

    // Creates the window (820 x 620 DIPs, centred on the monitor of `nearWindow`, else of the pointer); not shown yet.
    bool Create(HINSTANCE instance, HWND nearWindow);
    HWND Hwnd() const { return hwnd_; }
    // Shows the window at `topic` ("guide-settings.md#behaviour-tab", "quick-start.md", "#anchor" in
    // the current guide) and brings it to the front.
    void Open(const std::wstring& topic);
    void OpenGuide(int index, const std::wstring& anchor = {});
    int Guide() const { return guide_; }  // QueryState 287 (-1: none)
    // Tab / Shift+Tab between the list and the guide, Esc closes, Enter on a link follows it.
    bool PreTranslateMessage(MSG& msg);

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ViewSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    int Px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }
    void CreateFonts();
    void ApplyTheme();
    void Layout();
    void Load(int index);        // streams guide `index` into the view (the converted RTF)
    void ScrollTo(LONG cp);      // the line of `cp` at the top of the view
    bool Anchor(const std::wstring& slug, LONG& cp) const;
    int LinkAt(POINT client) const;  // the link under a point of the view (-1: none)
    int LinkAtSelection() const;
    void Follow(int link);
    void DrawListItem(const DRAWITEMSTRUCT& di);

    std::function<void()> destroyed_;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    HWND view_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    DarkMode::Palette palette_;
    HFONT font_ = nullptr;
    HBRUSH layerBrush_ = nullptr;
    HBRUSH controlBrush_ = nullptr;
    int guide_ = -1;
    int loaded_ = -1;  // the guide the view holds
    HelpDoc::Rendered doc_;
    static constexpr UINT kFollowMessage = WM_APP + 1;  // posted: wParam = link index (after the click)
};
