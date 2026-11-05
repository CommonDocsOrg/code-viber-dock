// Round 9 phase 9D: the Help window and its Markdown -> RTF converter (see HelpWindow.h).

#include "HelpWindow.h"

#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>

#include "SettingsIds.h"
#include "resource.h"

// --- the guides and the converter -------------------------------------------------------------

namespace HelpDoc {

const std::vector<Guide>& Guides() {
    static const std::vector<Guide> kGuides = {
        {L"quick-start.md", L"Quick start", IDR_DOC_QUICKSTART},
        {L"guide-settings.md", L"Settings", IDR_DOC_SETTINGS},
        {L"guide-docks.md", L"Docks and displays", IDR_DOC_DOCKS},
        {L"guide-items.md", L"Items and widgets", IDR_DOC_ITEMS},
        {L"troubleshooting.md", L"Troubleshooting", IDR_DOC_TROUBLESHOOTING},
        {L"README.md", L"All guides", IDR_DOC_INDEX},
    };
    return kGuides;
}

int GuideIndex(const std::wstring& file) {
    std::wstring name = file;
    if (const size_t slash = name.find_last_of(L"/\\"); slash != std::wstring::npos) name = name.substr(slash + 1);
    const std::vector<Guide>& g = Guides();
    for (size_t i = 0; i < g.size(); ++i) {
        if (_wcsicmp(name.c_str(), g[i].file) == 0) return static_cast<int>(i);
    }
    return -1;
}

std::wstring Slug(const std::wstring& heading) {
    std::wstring s;
    for (wchar_t c : heading) {
        if (c == L' ') {
            s += L'-';
        } else if (c == L'-' || c == L'_' || std::iswalnum(c)) {
            s += static_cast<wchar_t>(std::towlower(c));
        }
    }
    return s;
}

namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The RTF and, beside it, the text the RichEdit will hold (one character per paragraph mark).
struct Writer {
    std::string rtf;
    std::wstring plain;
    void Raw(const char* s) { rtf += s; }
    void Raw(const std::string& s) { rtf += s; }
    void Char(wchar_t c) {
        plain += c;
        if (c == L'\\' || c == L'{' || c == L'}') {
            rtf += '\\';
            rtf += static_cast<char>(c);
        } else if (c == L'\t') {
            rtf += "\\tab ";
        } else if (c >= 0x20 && c < 0x80) {
            rtf += static_cast<char>(c);
        } else {
            rtf += "\\u" + std::to_string(static_cast<int16_t>(c)) + "?";  // \uN is a signed 16-bit value
        }
    }
    void Text(const std::wstring& s) {
        for (wchar_t c : s) Char(c);
    }
    void Par() {
        rtf += "\\par\n";
        plain += L'\r';
    }
};

std::string Color(COLORREF c) {
    return "\\red" + std::to_string(GetRValue(c)) + "\\green" + std::to_string(GetGValue(c)) + "\\blue" + std::to_string(GetBValue(c)) + ";";
}

// Inline markup: `code`, **bold**, *italic*, [text](target), backslash escapes.
void Inline(Writer& w, const std::wstring& s, std::vector<Link>& links) {
    size_t i = 0;
    while (i < s.size()) {
        const wchar_t c = s[i];
        if (c == L'\\' && i + 1 < s.size() && std::iswpunct(s[i + 1])) {
            w.Char(s[i + 1]);
            i += 2;
            continue;
        }
        if (c == L'`') {
            const size_t end = s.find(L'`', i + 1);
            if (end != std::wstring::npos) {
                w.Raw("{\\f1\\fs19\\highlight5 ");
                w.Text(s.substr(i + 1, end - i - 1));
                w.Raw("}");
                i = end + 1;
                continue;
            }
        }
        if (c == L'*' && i + 1 < s.size() && s[i + 1] == L'*') {
            const size_t end = s.find(L"**", i + 2);
            if (end != std::wstring::npos) {
                w.Raw("{\\b ");
                Inline(w, s.substr(i + 2, end - i - 2), links);
                w.Raw("}");
                i = end + 2;
                continue;
            }
        }
        if (c == L'*' && i + 1 < s.size() && s[i + 1] != L' ') {
            size_t end = i + 1;
            while (end < s.size() && !(s[end] == L'*' && (end + 1 >= s.size() || s[end + 1] != L'*'))) ++end;
            if (end < s.size() && end > i + 1) {
                w.Raw("{\\i ");
                Inline(w, s.substr(i + 1, end - i - 1), links);
                w.Raw("}");
                i = end + 1;
                continue;
            }
        }
        if (c == L'[') {
            const size_t close = s.find(L"](", i + 1);
            const size_t paren = close == std::wstring::npos ? std::wstring::npos : s.find(L')', close + 2);
            if (close != std::wstring::npos && paren != std::wstring::npos) {
                Link link;
                link.target = s.substr(close + 2, paren - close - 2);
                w.Raw("{\\cf3\\ul ");
                link.cpMin = static_cast<LONG>(w.plain.size());
                Inline(w, s.substr(i + 1, close - i - 1), links);
                link.cpMax = static_cast<LONG>(w.plain.size());
                w.Raw("}");
                link.text = w.plain.substr(static_cast<size_t>(link.cpMin));
                links.push_back(std::move(link));
                i = paren + 1;
                continue;
            }
        }
        w.Char(c);
        ++i;
    }
}

const char* kBody = "\\pard\\plain\\sa140\\sl276\\slmult1 \\f0\\fs20\\cf1 ";

}  // namespace

Rendered Render(const std::string& markdownUtf8, const Colors& colors) {
    Rendered out;
    Writer w;
    w.Raw("{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\fswiss Segoe UI;}{\\f1\\fmodern Consolas;}}{\\colortbl;");
    w.Raw(Color(colors.text) + Color(colors.secondary) + Color(colors.link) + Color(colors.code) + Color(colors.codeBack) + "}\n");
    w.Raw("\\viewkind4\\uc1\n");
    const std::wstring md = Widen(markdownUtf8);
    std::vector<std::wstring> lines;
    for (size_t at = 0; at <= md.size();) {
        size_t nl = md.find(L'\n', at);
        if (nl == std::wstring::npos) nl = md.size();
        std::wstring line = md.substr(at, nl - at);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(std::move(line));
        at = nl + 1;
    }
    std::wstring para;
    bool afterList = false;
    auto flush = [&] {
        if (para.empty()) return;
        w.Raw(afterList ? "\\pard\\plain\\sb100\\sa140\\sl276\\slmult1 \\f0\\fs20\\cf1 " : kBody);
        Inline(w, para, out.links);
        w.Par();
        para.clear();
        afterList = false;
    };
    std::vector<std::wstring> code;
    bool inCode = false;
    for (const std::wstring& line : lines) {
        size_t indent = 0;
        while (indent < line.size() && line[indent] == L' ') ++indent;
        const std::wstring t = line.substr(indent);
        if (inCode) {
            if (t.rfind(L"```", 0) == 0) {
                // The block: monospace, indented, in the code colour.
                for (size_t k = 0; k < code.size(); ++k) {
                    w.Raw(k + 1 == code.size() ? "\\pard\\plain\\li420\\sa160 \\f1\\fs18\\cf4 " : "\\pard\\plain\\li420\\sa0 \\f1\\fs18\\cf4 ");
                    std::wstring text = code[k];
                    for (size_t p = text.find(L'\t'); p != std::wstring::npos; p = text.find(L'\t', p)) text.replace(p, 1, L"    ");
                    w.Text(text);
                    w.Par();
                }
                code.clear();
                inCode = false;
                afterList = false;
            } else {
                code.push_back(line);
            }
            continue;
        }
        if (t.rfind(L"```", 0) == 0) {
            flush();
            inCode = true;
            continue;
        }
        if (t.empty()) {
            flush();
            continue;
        }
        size_t hashes = 0;
        while (indent == 0 && hashes < t.size() && t[hashes] == L'#') ++hashes;
        if (hashes >= 1 && hashes <= 6 && hashes < t.size() && t[hashes] == L' ') {
            flush();
            static const char* kHeading[] = {
                "\\pard\\plain\\sb120\\sa180\\keepn \\f0\\fs36\\b\\cf1 ",
                "\\pard\\plain\\sb360\\sa120\\keepn \\f0\\fs29\\b\\cf1 ",
                "\\pard\\plain\\sb260\\sa90\\keepn \\f0\\fs25\\b\\cf1 ",
                "\\pard\\plain\\sb200\\sa70\\keepn \\f0\\fs22\\b\\cf1 ",
            };
            w.Raw(kHeading[std::min<size_t>(hashes, 4) - 1]);
            Heading h;
            h.level = static_cast<int>(hashes);
            h.cp = static_cast<LONG>(w.plain.size());
            std::wstring text = t.substr(hashes + 1);
            while (!text.empty() && text.back() == L' ') text.pop_back();
            Inline(w, text, out.links);
            h.text = w.plain.substr(static_cast<size_t>(h.cp));
            w.Par();
            // GitHub's anchors: the slug, made unique with "-1", "-2" ...
            std::wstring slug = Slug(h.text);
            const std::wstring base = slug;
            for (int n = 1; std::any_of(out.headings.begin(), out.headings.end(), [&](const Heading& o) { return o.slug == slug; }); ++n) {
                slug = base + L"-" + std::to_wstring(n);
            }
            h.slug = slug;
            out.headings.push_back(std::move(h));
            afterList = false;
            continue;
        }
        const bool bullet = (t.rfind(L"- ", 0) == 0 || t.rfind(L"* ", 0) == 0);
        size_t digits = 0;
        while (digits < t.size() && std::iswdigit(t[digits])) ++digits;
        const bool numbered = digits > 0 && digits + 1 < t.size() && t[digits] == L'.' && t[digits + 1] == L' ';
        if (bullet || numbered) {
            flush();
            const int level = indent >= 2 ? 1 : 0;
            const int li = 400 + level * 380;
            w.Raw("\\pard\\plain\\fi-260\\li" + std::to_string(li) + "\\tx" + std::to_string(li) + "\\sa70\\sl264\\slmult1 \\f0\\fs20\\cf1 ");
            if (bullet) {
                w.Char(level ? L'\x25E6' : L'\x2022');
                w.Char(L'\t');
                Inline(w, t.substr(2), out.links);
            } else {
                w.Text(t.substr(0, digits + 1));
                w.Char(L'\t');
                Inline(w, t.substr(digits + 2), out.links);
            }
            w.Par();
            afterList = true;
            continue;
        }
        if (!para.empty()) para += L' ';
        para += t;
    }
    flush();
    if (inCode) {  // an unclosed block: as it is
        for (const std::wstring& line : code) {
            w.Raw("\\pard\\plain\\li420\\sa0 \\f1\\fs18\\cf4 ");
            w.Text(line);
            w.Par();
        }
    }
    w.Raw("}");
    out.rtf = std::move(w.rtf);
    out.plain = std::move(w.plain);
    return out;
}

}  // namespace HelpDoc

// --- the window ----------------------------------------------------------------------------------

namespace {

constexpr int kWidthDip = 820, kHeightDip = 620;  // the client's default size
constexpr int kMinWidthDip = 560, kMinHeightDip = 400;
constexpr int kMargin = 12;
constexpr int kListW = 196;
constexpr int kRowDip = 34;

COLORREF Mix(COLORREF a, COLORREF b, int percentA) {
    return RGB((GetRValue(a) * percentA + GetRValue(b) * (100 - percentA)) / 100,
               (GetGValue(a) * percentA + GetGValue(b) * (100 - percentA)) / 100,
               (GetBValue(a) * percentA + GetBValue(b) * (100 - percentA)) / 100);
}

std::string ReadResource(int id) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    const void* bytes = data ? LockResource(data) : nullptr;
    const DWORD size = res ? SizeofResource(nullptr, res) : 0;
    std::string s = bytes && size ? std::string(static_cast<const char*>(bytes), size) : std::string();
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

struct StreamState {
    const std::string* text;
    size_t at;
};

DWORD CALLBACK StreamIn(DWORD_PTR cookie, LPBYTE buffer, LONG size, LONG* done) {
    auto* s = reinterpret_cast<StreamState*>(cookie);
    const size_t n = std::min(static_cast<size_t>(size), s->text->size() - s->at);
    if (n) memcpy(buffer, s->text->data() + s->at, n);
    s->at += n;
    *done = static_cast<LONG>(n);
    return 0;
}

std::wstring TextRange(HWND edit, LONG from, LONG to) {
    if (to <= from) return {};
    std::wstring buf(static_cast<size_t>(to - from) + 1, L'\0');
    TEXTRANGEW tr{{from, to}, buf.data()};
    const LRESULT n = SendMessageW(edit, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&tr));
    buf.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return buf;
}

}  // namespace

HelpWindow::HelpWindow(std::function<void()> destroyed) : destroyed_(std::move(destroyed)) {}

HelpWindow::~HelpWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (font_) DeleteObject(font_);
    if (layerBrush_) DeleteObject(layerBrush_);
    if (controlBrush_) DeleteObject(controlBrush_);
}

bool HelpWindow::Create(HINSTANCE instance, HWND nearWindow) {
    instance_ = instance;
    static HMODULE richEdit = LoadLibraryW(L"Msftedit.dll");  // kept for the process (the class lives in it)
    if (!richEdit) return false;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    HMONITOR mon = nullptr;
    if (nearWindow && IsWindowVisible(nearWindow)) {
        mon = MonitorFromWindow(nearWindow, MONITOR_DEFAULTTONEAREST);
    } else {
        POINT pt{};
        GetCursorPos(&pt);
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    UINT dx = USER_DEFAULT_SCREEN_DPI, dy = USER_DEFAULT_SCREEN_DPI;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) dx = USER_DEFAULT_SCREEN_DPI;
    dpi_ = dx;
    constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    constexpr DWORD exStyle = WS_EX_APPWINDOW | WS_EX_CONTROLPARENT;
    auto rectFor = [&](UINT dpi) {
        RECT r{0, 0, MulDiv(kWidthDip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
               MulDiv(kHeightDip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)};
        AdjustWindowRectExForDpi(&r, style, FALSE, exStyle, dpi);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        const RECT o = mi.rcWork;
        const int w = std::min<int>(r.right - r.left, o.right - o.left), h = std::min<int>(r.bottom - r.top, o.bottom - o.top);
        const int x = (o.left + o.right - w) / 2, y = (o.top + o.bottom - h) / 2;
        return RECT{x, y, x + w, y + h};
    };
    RECT r = rectFor(dpi_);
    CreateWindowExW(exStyle, kClassName, L"WinDock Help", style, r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                    instance, this);
    if (!hwnd_) return false;
    if (const UINT actual = GetDpiForWindow(hwnd_); actual && actual != dpi_) {
        dpi_ = actual;
        r = rectFor(dpi_);
        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    CreateFonts();
    // The tab order is the creation order: the list, then the guide.
    list_ = CreateWindowExW(0, WC_LISTBOXW, L"Guides",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                                LBS_NOINTEGRALHEIGHT,
                            0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kHelpList)), instance, nullptr);
    view_ = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOOLEDRAGDROP,
                            0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kHelpView)), instance, nullptr);
    if (!list_ || !view_) {
        DestroyWindow(hwnd_);
        return false;
    }
    SetWindowSubclass(view_, ViewSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(view_, EM_AUTOURLDETECT, FALSE, 0);
    SendMessageW(view_, EM_SETTARGETDEVICE, 0, 0);  // wrap at the window's width
    SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    for (const HelpDoc::Guide& g : HelpDoc::Guides()) SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g.title));
    SendMessageW(list_, LB_SETITEMHEIGHT, 0, Px(kRowDip));
    ApplyTheme();
    Layout();
    return true;
}

void HelpWindow::CreateFonts() {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    LOGFONTW lf{};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_)) {
        lf = ncm.lfMessageFont;
    } else {
        lf.lfHeight = -MulDiv(9, static_cast<int>(dpi_), 72);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
    }
    if (font_) DeleteObject(font_);
    font_ = CreateFontIndirectW(&lf);
}

void HelpWindow::ApplyTheme() {
    palette_ = DarkMode::CurrentPalette();
    const bool dark = palette_.dark;
    DarkMode::SetDarkCaption(hwnd_, dark);
    DarkMode::AllowDarkForWindow(hwnd_, dark);
    for (HWND c : {list_, view_}) {
        if (c) DarkMode::ThemeScrollBars(c, dark);
    }
    if (layerBrush_) DeleteObject(layerBrush_);
    if (controlBrush_) DeleteObject(controlBrush_);
    layerBrush_ = CreateSolidBrush(palette_.layer);
    controlBrush_ = CreateSolidBrush(palette_.control);
    if (view_) SendMessageW(view_, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(palette_.layer));
    // The guide's colours come with its RTF: convert it again (the reading position kept).
    if (view_ && loaded_ >= 0) {
        const LONG first = static_cast<LONG>(SendMessageW(view_, EM_LINEINDEX, SendMessageW(view_, EM_GETFIRSTVISIBLELINE, 0, 0), 0));
        const int g = loaded_;
        loaded_ = -1;
        Load(g);
        ScrollTo(std::max<LONG>(0, first));
    }
    if (hwnd_) RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void HelpWindow::Layout() {
    if (!hwnd_ || !list_ || !view_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    // Keep the reading position: the first visible character before, at the top again after.
    const bool keep = loaded_ >= 0 && IsWindowVisible(view_);
    const LONG first = keep ? static_cast<LONG>(SendMessageW(view_, EM_LINEINDEX, SendMessageW(view_, EM_GETFIRSTVISIBLELINE, 0, 0), 0)) : 0;
    const int m = Px(kMargin), lw = Px(kListW);
    const int h = std::max(0, static_cast<int>(rc.bottom) - 2 * m);
    const int vx = m + lw + Px(14);
    const int vw = std::max(0, static_cast<int>(rc.right) - vx - Px(4));
    SetWindowPos(list_, nullptr, m, m, lw, h, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(view_, nullptr, vx, Px(4), vw, std::max(0, static_cast<int>(rc.bottom) - Px(4)), SWP_NOZORDER | SWP_NOACTIVATE);
    // Room around the text inside the view (the scroll bar stays at the window's edge).
    RECT text{Px(10), Px(10), std::max(Px(10), vw - Px(18)), std::max(Px(10), static_cast<int>(rc.bottom) - Px(14))};
    SendMessageW(view_, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&text));
    if (keep) ScrollTo(first);
}

void HelpWindow::Load(int index) {
    const std::vector<HelpDoc::Guide>& guides = HelpDoc::Guides();
    if (!view_ || index < 0 || index >= static_cast<int>(guides.size())) return;
    if (loaded_ == index) return;
    const HelpDoc::Colors colors{palette_.text, palette_.textSecondary, palette_.link,
                                 palette_.highContrast ? palette_.text : Mix(palette_.text, palette_.accent, 72),
                                 palette_.highContrast ? palette_.layer : Mix(palette_.text, palette_.layer, 11)};
    std::string md = ReadResource(guides[static_cast<size_t>(index)].resource);
    if (md.empty()) md = "# " + std::string("Guide missing") + "\n\nThis guide is not in this build of WinDock.\n";
    doc_ = HelpDoc::Render(md, colors);
    SendMessageW(view_, WM_SETREDRAW, FALSE, 0);
    StreamState state{&doc_.rtf, 0};
    EDITSTREAM es{reinterpret_cast<DWORD_PTR>(&state), 0, StreamIn};
    SendMessageW(view_, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&es));
    // The positions were counted while converting; check each against the text the RichEdit holds
    // (and find it after the previous one if it moved).
    LONG from = 0;
    auto locate = [&](const std::wstring& text, LONG& cp) {
        if (text.empty()) return;
        if (TextRange(view_, cp, cp + static_cast<LONG>(text.size())) == text) {
            from = cp + static_cast<LONG>(text.size());
            return;
        }
        FINDTEXTEXW ft{{from, -1}, text.c_str(), {}};
        const LRESULT at = SendMessageW(view_, EM_FINDTEXTEXW, FR_DOWN | FR_MATCHCASE, reinterpret_cast<LPARAM>(&ft));
        if (at >= 0) {
            cp = static_cast<LONG>(at);
            from = ft.chrgText.cpMax;
        }
    };
    size_t hi = 0, li = 0;
    while (hi < doc_.headings.size() || li < doc_.links.size()) {
        const bool heading = li >= doc_.links.size() || (hi < doc_.headings.size() && doc_.headings[hi].cp <= doc_.links[li].cpMin);
        if (heading) {
            locate(doc_.headings[hi].text, doc_.headings[hi].cp);
            ++hi;
        } else {
            HelpDoc::Link& l = doc_.links[li];
            const LONG len = l.cpMax - l.cpMin;
            locate(l.text, l.cpMin);
            l.cpMax = l.cpMin + len;
            ++li;
        }
    }
    SendMessageW(view_, EM_SETSEL, 0, 0);
    SendMessageW(view_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(view_, nullptr, TRUE);
    loaded_ = index;
}

void HelpWindow::ScrollTo(LONG cp) {
    if (!view_) return;
    SendMessageW(view_, EM_SETSEL, static_cast<WPARAM>(cp), static_cast<LPARAM>(cp));
    const LONG line = static_cast<LONG>(SendMessageW(view_, EM_EXLINEFROMCHAR, 0, cp));
    const LONG first = static_cast<LONG>(SendMessageW(view_, EM_GETFIRSTVISIBLELINE, 0, 0));
    if (line != first) SendMessageW(view_, EM_LINESCROLL, 0, line - first);
}

bool HelpWindow::Anchor(const std::wstring& slug, LONG& cp) const {
    const std::wstring want = HelpDoc::Slug(slug);
    for (const HelpDoc::Heading& h : doc_.headings) {
        if (h.slug == want || h.slug == slug) {
            cp = h.cp;
            return true;
        }
    }
    return false;
}

void HelpWindow::OpenGuide(int index, const std::wstring& anchor) {
    const int n = static_cast<int>(HelpDoc::Guides().size());
    if (!hwnd_ || index < 0 || index >= n) return;
    guide_ = index;
    if (SendMessageW(list_, LB_GETCURSEL, 0, 0) != index) SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
    Load(index);
    LONG cp = 0;
    if (!anchor.empty()) Anchor(anchor, cp);
    ScrollTo(cp);
}

void HelpWindow::Open(const std::wstring& topic) {
    if (!hwnd_) return;
    std::wstring file = topic, anchor;
    if (const size_t hash = topic.find(L'#'); hash != std::wstring::npos) {
        file = topic.substr(0, hash);
        anchor = topic.substr(hash + 1);
    }
    int index = file.empty() ? (guide_ >= 0 ? guide_ : 0) : HelpDoc::GuideIndex(file);
    if (index < 0) index = 0;
    if (!IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);
    } else if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    Layout();
    OpenGuide(index, anchor);
    SetForegroundWindow(hwnd_);
    if (HWND f = GetFocus(); !f || !IsChild(hwnd_, f)) SetFocus(view_);
}

int HelpWindow::LinkAt(POINT pt) const {
    if (!view_ || doc_.links.empty()) return -1;
    POINTL p{pt.x, pt.y};
    const LONG cp = static_cast<LONG>(SendMessageW(view_, EM_CHARFROMPOS, 0, reinterpret_cast<LPARAM>(&p)));
    for (size_t k = 0; k < doc_.links.size(); ++k) {
        const HelpDoc::Link& l = doc_.links[k];
        if (cp < l.cpMin || cp >= l.cpMax) continue;
        // EM_CHARFROMPOS answers the nearest character: the point must be on it.
        POINTL a{}, b{};
        SendMessageW(view_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&a), cp);
        SendMessageW(view_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&b), cp + 1);
        const LONG right = b.y == a.y && b.x > a.x ? b.x : a.x + Px(12);
        if (pt.x >= a.x - 1 && pt.x <= right + 1 && pt.y >= a.y && pt.y < a.y + Px(26)) return static_cast<int>(k);
        return -1;
    }
    return -1;
}

int HelpWindow::LinkAtSelection() const {
    CHARRANGE sel{};
    SendMessageW(view_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
    for (size_t k = 0; k < doc_.links.size(); ++k) {
        if (sel.cpMin >= doc_.links[k].cpMin && sel.cpMin < doc_.links[k].cpMax) return static_cast<int>(k);
    }
    return -1;
}

void HelpWindow::Follow(int link) {
    if (link < 0 || link >= static_cast<int>(doc_.links.size())) return;
    const std::wstring target = doc_.links[static_cast<size_t>(link)].target;
    if (target.rfind(L"http://", 0) == 0 || target.rfind(L"https://", 0) == 0 || target.rfind(L"mailto:", 0) == 0) {
        AllowSetForegroundWindow(ASFW_ANY);
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    std::wstring file = target, anchor;
    if (const size_t hash = target.find(L'#'); hash != std::wstring::npos) {
        file = target.substr(0, hash);
        anchor = target.substr(hash + 1);
    }
    if (file.empty()) {
        OpenGuide(guide_ >= 0 ? guide_ : 0, anchor);
        return;
    }
    if (const int index = HelpDoc::GuideIndex(file); index >= 0) {
        OpenGuide(index, anchor);
        return;
    }
    // Another document (the widget developer reference): the repository's copy next to the build
    // when there is one, else the project's page.
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    if (const size_t slash = dir.find_last_of(L'\\'); slash != std::wstring::npos) dir.resize(slash);
    std::wstring name = file;
    if (const size_t slash = name.find_last_of(L"/\\"); slash != std::wstring::npos) name = name.substr(slash + 1);
    const std::wstring local = dir + L"\\..\\..\\docs\\" + name;
    AllowSetForegroundWindow(ASFW_ANY);
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ShellExecuteW(nullptr, L"open", local.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        const std::wstring url = L"https://github.com/CommonDocsOrg/windows-dock/blob/master/docs/" + name + (anchor.empty() ? L"" : L"#" + anchor);
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

bool HelpWindow::PreTranslateMessage(MSG& msg) {
    if (!hwnd_ || !msg.hwnd || (msg.hwnd != hwnd_ && !IsChild(hwnd_, msg.hwnd))) return false;
    if (msg.message != WM_KEYDOWN) return false;
    if (msg.wParam == VK_ESCAPE) {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return true;
    }
    if (msg.wParam == VK_TAB && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0) {
        SetFocus(GetFocus() == list_ ? view_ : list_);
        return true;
    }
    return false;
}

void HelpWindow::DrawListItem(const DRAWITEMSTRUCT& di) {
    const std::vector<HelpDoc::Guide>& guides = HelpDoc::Guides();
    if (di.itemID >= guides.size()) return;
    HDC dc = di.hDC;
    const RECT r = di.rcItem;
    const bool selected = (di.itemState & ODS_SELECTED) != 0;
    const bool focused = GetFocus() == list_;
    COLORREF back = palette_.control;
    if (selected) back = palette_.highContrast ? GetSysColor(COLOR_HIGHLIGHT) : Mix(palette_.accent, palette_.control, focused ? 38 : 26);
    HBRUSH b = CreateSolidBrush(back);
    FillRect(dc, &r, b);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, selected && palette_.highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : palette_.text);
    HGDIOBJ old = SelectObject(dc, font_);
    RECT t{r.left + Px(12), r.top, r.right - Px(8), r.bottom};
    DrawTextW(dc, guides[di.itemID].title, -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
    if ((di.itemState & ODS_FOCUS) && !(di.itemState & ODS_NOFOCUSRECT)) DrawFocusRect(dc, &r);
}

LRESULT CALLBACK HelpWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HelpWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<HelpWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<HelpWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    const LRESULT r = self->WndProc(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = self->list_ = self->view_ = nullptr;
        if (self->destroyed_) self->destroyed_();  // the app frees the object later (posted)
    }
    return r;
}

LRESULT CALLBACK HelpWindow::ViewSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<HelpWindow*>(data);
    switch (msg) {
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (self->LinkAt(pt) >= 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONUP: {
            // A click (not a drag that selected text) on a link follows it, after the click is done.
            const int link = self->LinkAt(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            CHARRANGE sel{};
            SendMessageW(hwnd, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
            if (link >= 0 && sel.cpMin == sel.cpMax && self->hwnd_) PostMessageW(self->hwnd_, kFollowMessage, static_cast<WPARAM>(link), 0);
            return r;
        }
        case WM_KEYDOWN:
            if (wp == VK_RETURN) {
                if (const int link = self->LinkAtSelection(); link >= 0 && self->hwnd_) PostMessageW(self->hwnd_, kFollowMessage, static_cast<WPARAM>(link), 0);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, ViewSubclassProc, id);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT HelpWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            RECT r{0, 0, Px(kMinWidthDip), Px(kMinHeightDip)};
            AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE)), FALSE,
                                     static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_EXSTYLE)), dpi_);
            mmi->ptMinTrackSize = POINT{r.right - r.left, r.bottom - r.top};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            CreateFonts();
            if (list_) {
                SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
                SendMessageW(list_, LB_SETITEMHEIGHT, 0, Px(kRowDip));
            }
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETHIGHCONTRAST ||
                (lp && CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lp), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)) {
                ApplyTheme();
            }
            return 0;
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            ApplyTheme();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE && !HIWORD(wp) && view_) {
                if (HWND f = GetFocus(); !f || !IsChild(hwnd_, f)) SetFocus(view_);
            }
            if (list_) InvalidateRect(list_, nullptr, FALSE);
            return 0;
        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (mi->CtlID == static_cast<UINT>(SettingsIds::kHelpList)) {
                mi->itemHeight = static_cast<UINT>(Px(kRowDip));
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (di->CtlID == static_cast<UINT>(SettingsIds::kHelpList)) {
                DrawListItem(*di);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, palette_.text);
            SetBkColor(dc, palette_.control);
            return reinterpret_cast<LRESULT>(controlBrush_);
        }
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, layerBrush_);
            return 1;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == SettingsIds::kHelpList && HIWORD(wp) == LBN_SELCHANGE) {
                const LRESULT sel = SendMessageW(list_, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR && sel != guide_) OpenGuide(static_cast<int>(sel));
                return 0;
            }
            break;
        case kFollowMessage:
            Follow(static_cast<int>(wp));
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
