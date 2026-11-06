#include "SettingsWindow.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cwchar>
#include <iterator>

#include "Backdrop.h"
#include "SettingsIds.h"
#include "SettingsPreview.h"
#include "resource.h"

namespace {

bool FontFamilyExists(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    EnumFontFamiliesExW(
        dc, &lf,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) -> int {
            *reinterpret_cast<bool*>(lp) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

std::wstring WindowText(HWND hwnd) {
    const int n = GetWindowTextLengthW(hwnd);
    if (n <= 0) return {};
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    s.resize(static_cast<size_t>(GetWindowTextW(hwnd, s.data(), n + 1)));
    return s;
}

// An editable combo box selects all of its edit text on WM_SETTEXT, on resizing and on a theme
// change, and the selection stays highlighted while it does not have the focus: clear it.
void UnselectComboText(const Ctl& c) {
    if (c.kind != CtlKind::Combo || !c.hwnd || (GetWindowLongW(c.hwnd, GWL_STYLE) & 0x3) != CBS_DROPDOWN) return;
    HWND focus = GetFocus();
    if (focus == c.hwnd || (focus && IsChild(c.hwnd, focus))) return;
    SendMessageW(c.hwnd, CB_SETEDITSEL, 0, MAKELPARAM(-1, 0));
}

}  // namespace

// --- SettingsPage ----------------------------------------------------------------------------

SettingsPage::SettingsPage(SettingsWindow& window, const wchar_t* name, wchar_t glyph)
    : window_(window), name_(name), glyph_(glyph) {}

HWND SettingsPage::Item(int id) const {
    const Ctl* c = Find(id);
    return c ? c->hwnd : nullptr;
}

const Ctl* SettingsPage::Find(int id) const {
    for (const Ctl& c : table_) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

const Ctl* SettingsPage::Find(HWND hwnd) const {
    if (!hwnd) return nullptr;
    for (const Ctl& c : table_) {
        if (c.hwnd == hwnd) return &c;
    }
    return nullptr;
}

int SettingsPage::Width() const {
    int w = window_.ContentWidthDip();
    if (scrollBar_) {
        const int sb = GetSystemMetricsForDpi(SM_CXVSCROLL, window_.Dpi());
        w -= (sb * USER_DEFAULT_SCREEN_DPI + static_cast<int>(window_.Dpi()) - 1) / static_cast<int>(window_.Dpi());
    }
    return w;
}

Ctl& SettingsPage::Add(int id, CtlKind kind, const wchar_t* cls, std::wstring text, DWORD style, RECT dip) {
    if (table_.empty()) buildW_ = Width();
    if (!pendingHelp_.empty() && reflow_ && kind != CtlKind::Title && card_ >= 0 && card_ == pendingHelpCard_) {
        // Round 9 phase 9D: an UntitledCard(topic)'s "?" goes into its first control's row, centred
        // on that control's line (a drop-down: its selection field).
        const int h = kind == CtlKind::Combo ? kRowH - 8 : std::min(static_cast<int>(dip.bottom - dip.top), kRowH);
        const std::wstring topic = std::move(pendingHelp_);
        pendingHelp_.clear();
        const int keep = y_;
        y_ = dip.top + (h - kHelpDip) / 2 - 2;  // HelpButton places it 2 DIPs under y_
        HelpButton(topic.c_str(), nullptr);
        y_ = keep;
    }
    Ctl c;
    c.id = id;
    c.kind = kind;
    c.cls = cls;
    c.text = std::move(text);
    c.style = style | WS_GROUP;  // every control is its own group except radios 2..n (Radios)
    if (reflow_) {
        c.row = row_;
        // A hint row is kHintRowH tall: the controls after its label sit in its middle.
        if (hintRow_ == row_) OffsetRect(&dip, 0, (kHintRowH - kRowH) / 2);
        if (kind != CtlKind::Title) {
            if (card_ < 0 && autoCard_) {
                cards_.push_back(Card{});
                card_ = static_cast<int>(cards_.size()) - 1;
            }
            c.card = card_;
        }
    }
    // Round 10 phase 10D: also on a page that lays itself out (the Contents tab's Anchor and
    // offset): its rows are gone while the expander is closed; the page places them.
    c.expander = expander_;
    c.dip = c.base = dip;
    table_.push_back(std::move(c));
    return table_.back();
}

void SettingsPage::ResetTable() {
    table_.clear();
    cards_.clear();
    hairlines_.clear();
    y_ = 0;
    row_ = 0;
    hintRow_ = -1;
    card_ = -1;
    autoCard_ = true;
    expander_ = 0;
    nextAutoId_ = 0x6000;
    pendingHelp_.clear();
    pendingHelpCard_ = -1;
    buildW_ = Width();
}

void SettingsPage::HelpButton(const wchar_t* topic, const wchar_t* title) {
    if (!topic || !*topic || !reflow_) return;
    if (card_ < 0) {  // no card open yet: the untitled one the next control would open
        cards_.push_back(Card{});
        card_ = static_cast<int>(cards_.size()) - 1;
    }
    std::wstring name = title && *title ? title : name_.c_str();
    const size_t dot = name.find(L" \x00B7 ");  // "Bar · 2 changed"
    if (dot != std::wstring::npos) name.resize(dot);
    const int right = Width() - kMargin;
    Ctl& c = Add(SettingsIds::kHelpButtonBase + card_, CtlKind::Help, WC_BUTTONW, L"Help: " + name, WS_TABSTOP | BS_OWNERDRAW,
                 RECT{right - kHelpDip, y_ + 2, right, y_ + 2 + kHelpDip});
    c.topic = topic;
    c.tip = L"Help: " + name;
}

const std::wstring& SettingsPage::Topic() const {
    if (!topic_.empty()) return topic_;
    firstTopic_.clear();
    for (const Ctl& c : table_) {
        if (c.kind == CtlKind::Help && !c.topic.empty()) {
            firstTopic_ = c.topic;
            break;
        }
    }
    return firstTopic_;
}

void SettingsPage::Title(const wchar_t* text) {
    if (window_.Kind() != SettingsKind::Global) {
        // Round 8 phase 8A: the tab or the header names the page; the first card starts at 16.
        y_ = std::max(y_, 24);
        return;
    }
    Add(SettingsIds::kPageTitle, CtlKind::Title, WC_STATICW, text, SS_LEFT | SS_NOPREFIX,
        RECT{kMargin, 14, Width() - kMargin, 56});
    ++row_;
    y_ = reflow_ ? 72 : 64;
}

void SettingsPage::Section(int id, const wchar_t* text, const wchar_t* topic) {
    int gap = 0;
    if (reflow_) {
        // A new card: 26 DIPs under the last one (y_ is a row's bottom + kRowGap after NextRow).
        const bool content = std::any_of(table_.begin(), table_.end(), [](const Ctl& c) { return c.card >= 0; });
        if (content) gap = 26;
        y_ += gap;
        if (std::any_of(table_.begin(), table_.end(), [&](const Ctl& c) { return c.row == row_; })) ++row_;
        cards_.push_back(Card{id, {}});
        card_ = static_cast<int>(cards_.size()) - 1;
    } else if (y_ > 64) {
        y_ += 10;
    }
    Ctl& c = Add(id, CtlKind::Section, WC_STATICW, text, SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE,
                 RECT{kMargin, y_, Width() - kMargin, y_ + 24});
    c.lo = gap;  // the reflow keeps this gap when the rows above it collapse
    if (topic) HelpButton(topic, text);  // round 9 phase 9D: in the header's row (c is not used after this)
    y_ += 28;
    ++row_;
}

void SettingsPage::EndCard() {
    if (std::any_of(table_.begin(), table_.end(), [&](const Ctl& c) { return c.row == row_; })) ++row_;
    card_ = -1;
    autoCard_ = false;
}

void SettingsPage::UntitledCard(const wchar_t* topic) {
    if (!reflow_) return;
    if (std::any_of(table_.begin(), table_.end(), [](const Ctl& c) { return c.card >= 0; })) y_ += 26;
    if (std::any_of(table_.begin(), table_.end(), [&](const Ctl& c) { return c.row == row_; })) ++row_;
    cards_.push_back(Card{});
    card_ = static_cast<int>(cards_.size()) - 1;
    // Round 9 phase 9D: the "?" waits for the card's first control (Add) and shares its row.
    pendingHelp_ = topic ? topic : L"";
    pendingHelpCard_ = card_;
}

void SettingsPage::Label(int id, const wchar_t* text) {
    Add(id, CtlKind::Label, WC_STATICW, text, SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS,
        RECT{kMargin, y_, kMargin + labelW_, y_ + kRowH});
}

void SettingsPage::Label(int id, const wchar_t* text, const wchar_t* hint, int hintId) {
    if (hintId == 0) hintId = nextAutoId_++;
    Ctl& label = Add(id, CtlKind::Label, WC_STATICW, text, SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                     RECT{kMargin, y_ + 3, kMargin + labelW_, y_ + 24});
    label.buddy = hintId;
    Add(hintId, CtlKind::Note, WC_STATICW, hint ? hint : L"", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS,
        RECT{kMargin, y_ + 24, kMargin + labelW_, y_ + 42})
        .hint = true;
    hintRow_ = row_;
}

void SettingsPage::Tip(int id, std::wstring text) {
    int hint = 0;
    for (Ctl& c : table_) {
        if (c.id != id) continue;
        c.tip = text;
        // Statics get the mouse (for the tooltip's subclass) only with SS_NOTIFY.
        if (c.cls && wcscmp(c.cls, WC_STATICW) == 0) c.style |= SS_NOTIFY;
        if (c.kind == CtlKind::Label && c.buddy) hint = c.buddy;
    }
    if (hint) Tip(hint, std::move(text));
}

void SettingsPage::SetTip(int id, const std::wstring& text) {
    for (Ctl& c : table_) {
        if (c.id != id || c.tip == text) continue;
        c.tip = text;
        // The tool exists when the control had a tip at creation (CreateTips): its text is copied.
        if (!tip_ || !c.hwnd) continue;
        TTTOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.hwnd = hwnd_;
        ti.uId = reinterpret_cast<UINT_PTR>(c.hwnd);
        ti.lpszText = c.tip.data();
        SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

void SettingsPage::Expander(int id, const wchar_t* text) {
    Add(id, CtlKind::Expander, WC_BUTTONW, text, WS_TABSTOP | BS_OWNERDRAW, RECT{kMargin, y_ + 4, Width() - kMargin, y_ + kRowH - 4});
    NextRow();
    expander_ = id;
}

void SettingsPage::HeadingExpander(int id, const wchar_t* text, bool openAtFirst, const wchar_t* topic) {
    if (openAtFirst && std::find(expanderDefaults_.begin(), expanderDefaults_.end(), id) == expanderDefaults_.end()) {
        expanderDefaults_.push_back(id);
        if (!ExpanderOpen(id)) openExpanders_.push_back(id);  // no relayout: the table is being built
    }
    // The first row of a card after another card (UntitledCard): like a Section it keeps the gap
    // between the cards when the rows above it are gone (a closed card above).
    const bool startsCard = card_ >= 0 && std::none_of(table_.begin(), table_.end(), [&](const Ctl& t) { return t.card == card_; });
    const bool after = std::any_of(table_.begin(), table_.end(), [&](const Ctl& t) { return t.card >= 0 && t.card != card_; });
    Ctl& c = Add(id, CtlKind::Expander, WC_BUTTONW, text, WS_TABSTOP | BS_OWNERDRAW,
                 RECT{kMargin, y_ + 2, Width() - kMargin, y_ + kRowH - 2});
    c.heading = true;
    c.lo = startsCard && after ? 26 : 0;
    if (topic) {
        // Round 9 phase 9D: the "?" at the right end of the heading's row (not one of its rows:
        // it stays while the card is closed).
        y_ += 4;
        HelpButton(topic, text);
        y_ -= 4;
    }
    NextRow();
    expander_ = id;
}

void SettingsPage::EndExpander() {
    if (std::any_of(table_.begin(), table_.end(), [&](const Ctl& c) { return c.row == row_; })) ++row_;
    expander_ = 0;
}

Ctl& SettingsPage::Text(int id, CtlKind kind, std::wstring text, int x, int w, int h) {
    const DWORD style = SS_LEFT | SS_NOPREFIX | (h == kRowH ? SS_CENTERIMAGE | SS_ENDELLIPSIS : 0);
    Ctl& c = Add(id, kind, WC_STATICW, std::move(text), style, RECT{x, y_, x + w, y_ + h});
    // Round 8 phase 8A: a multi-line note or value is as tall as its text (measured at layout time);
    // like an edit control it breaks a word longer than a line (a path).
    c.measured = reflow_ && h != kRowH && (kind == CtlKind::Note || kind == CtlKind::Value);
    if (c.measured) c.style |= SS_EDITCONTROL;
    return c;
}

void SettingsPage::Trackbar(int id, int x, int w, int lo, int hi, int page) {
    Ctl& c = Add(id, CtlKind::Trackbar, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_BOTH | TBS_NOTICKS,
                 RECT{x, y_ + 2, x + w, y_ + kRowH - 2});
    c.lo = lo;
    c.hi = hi;
    c.step = page;
}

void SettingsPage::Spin(int editId, int spinId, int x, int w, int lo, int hi, int step) {
    constexpr int kSpinW = 18;
    Ctl& e = Add(editId, CtlKind::Edit, WC_EDITW, L"", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                 RECT{x, y_ + 4, x + w - kSpinW, y_ + kRowH - 4});
    e.exStyle = WS_EX_CLIENTEDGE;
    e.buddy = spinId;
    e.hi = 6;  // text limit
    Ctl& s = Add(spinId, CtlKind::Spin, UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_NOTHOUSANDS | UDS_HOTTRACK,
                 RECT{x + w - kSpinW, y_ + 4, x + w, y_ + kRowH - 4});
    s.buddy = editId;
    s.lo = lo;
    s.hi = hi;
    s.step = step;
}

void SettingsPage::Combo(int id, int x, int w, bool editable) {
    const DWORD style = WS_TABSTOP | WS_VSCROLL | (editable ? CBS_DROPDOWN | CBS_AUTOHSCROLL : CBS_DROPDOWNLIST);
    // The height is the drop-down's; the selection field takes its height from the font.
    Add(id, CtlKind::Combo, WC_COMBOBOXW, L"", style, RECT{x, y_ + 4, x + w, y_ + 4 + 220});
}

void SettingsPage::Check(int id, int captionId, const wchar_t* text, int x) {
    Ctl& b = Add(id, CtlKind::Check, WC_BUTTONW, text, WS_TABSTOP | BS_AUTOCHECKBOX | BS_NOTIFY,
                 RECT{x, y_, x + kCaptionDx, y_ + kRowH});
    b.buddy = captionId;
    Ctl& c = Add(captionId, CtlKind::Caption, WC_STATICW, text, SS_OWNERDRAW | SS_NOTIFY,
                 RECT{x + kCaptionDx, y_, Width() - kMargin, y_ + kRowH});
    c.buddy = id;
}

void SettingsPage::Check(int id, int captionId, const wchar_t* text, const wchar_t* hint, int hintId, int x) {
    if (hintId == 0) hintId = nextAutoId_++;
    // The box and its caption on the label's line of a hint row, the hint under the caption.
    Ctl& b = Add(id, CtlKind::Check, WC_BUTTONW, text, WS_TABSTOP | BS_AUTOCHECKBOX | BS_NOTIFY,
                 RECT{x, y_ + 3, x + kCaptionDx, y_ + 24});
    b.buddy = captionId;
    Ctl& c = Add(captionId, CtlKind::Caption, WC_STATICW, text, SS_OWNERDRAW | SS_NOTIFY,
                 RECT{x + kCaptionDx, y_ + 3, Width() - kMargin, y_ + 24});
    c.buddy = id;
    Add(hintId, CtlKind::Note, WC_STATICW, hint ? hint : L"", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS,
        RECT{x + kCaptionDx, y_ + 24, Width() - kMargin, y_ + 42})
        .hint = true;
    hintRow_ = row_;
}

void SettingsPage::Radios(std::initializer_list<RadioDef> radios, int x) {
    const int top = y_;
    // One reflow row per radio (with its caption).
    if (std::any_of(table_.begin(), table_.end(), [&](const Ctl& c) { return c.row == row_; })) ++row_;
    const int row0 = row_;
    bool first = true;
    for (const RadioDef& r : radios) {
        Ctl& b = Add(r.id, CtlKind::Radio, WC_BUTTONW, r.text, WS_TABSTOP | BS_AUTORADIOBUTTON | BS_NOTIFY,
                     RECT{x, y_, x + kCaptionDx, y_ + kRowH});
        if (!first) b.style &= ~static_cast<DWORD>(WS_GROUP);
        b.buddy = r.captionId;
        first = false;
        y_ += kRowH;
        ++row_;
    }
    y_ = top;
    row_ = row0;
    for (const RadioDef& r : radios) {
        Ctl& c = Add(r.captionId, CtlKind::Caption, WC_STATICW, r.text, SS_OWNERDRAW | SS_NOTIFY,
                     RECT{x + kCaptionDx, y_, Width() - kMargin, y_ + kRowH});
        c.buddy = r.id;
        y_ += kRowH;
        ++row_;
    }
    y_ += kRowGap;
}

void SettingsPage::Button(int id, const wchar_t* text, int x, int w) {
    Add(id, CtlKind::Button, WC_BUTTONW, text, WS_TABSTOP | BS_PUSHBUTTON, RECT{x, y_ + 2, x + w, y_ + kRowH - 2});
}

void SettingsPage::Link(int id, std::wstring markup, int x, int w) {
    Add(id, CtlKind::Link, WC_LINK, std::move(markup), WS_TABSTOP, RECT{x, y_ + 7, x + w, y_ + kRowH - 3});
}

Config& SettingsPage::Cfg() { return *window_.Host().config; }
SettingsHost& SettingsPage::Host() { return window_.Host(); }
DockConfig& SettingsPage::Dock() { return window_.Dock(); }
size_t SettingsPage::DockIndex() const { return window_.DockIndex(); }
bool SettingsPage::Loading() const { return window_.Loading(); }

void SettingsPage::Changed(SettingsScope scope) { ChangedFor(scope, window_.DockIndex()); }

void SettingsPage::ChangedFor(SettingsScope scope, size_t dock) {
    if (window_.Loading()) return;
    if (window_.Host().changed) window_.Host().changed(scope, dock);
    window_.ConfigApplied();
    window_.PeersChanged(this);  // round 9 phase 9A: the window's other pages follow
}

void SettingsPage::SetText(int id, const std::wstring& text) {
    for (Ctl& m : table_) {
        if (m.id != id || !m.measured || m.maxLines <= 0) continue;
        // A note of at most maxLines lines: the reflow shows (a shortened) Ctl::text.
        if (m.text == text) return;
        m.text = text;
        if (m.hwnd) SetWindowTextW(m.hwnd, text.c_str());
        if (hwnd_) Relayout();
        return;
    }
    // Round 10 phase 10B: a label's / caption's table text follows too (a caption is clickable
    // only over its text, measured from Ctl::text at layout; QueryState 285 reads labels there):
    // the Contents tab's inspector renames its rows per selection.
    bool captionChanged = false;
    for (Ctl& m : table_) {
        if (m.id != id || (m.kind != CtlKind::Caption && m.kind != CtlKind::Label) || m.text == text) continue;
        m.text = text;
        captionChanged = m.kind == CtlKind::Caption;
    }
    const Ctl* c = Find(id);
    if (captionChanged && c && c->hwnd && hwnd_) {
        SetWindowTextW(c->hwnd, text.c_str());
        InvalidateRect(c->hwnd, nullptr, TRUE);
        Relayout();
        return;
    }
    if (!c || !c->hwnd || WindowText(c->hwnd) == text) return;
    SetWindowTextW(c->hwnd, text.c_str());
    UnselectComboText(*c);
    // A measured note that needs another height: the rows under it move (round 8 phase 8A).
    if (c->measured && hwnd_ && !c->collapsed) {
        const int h = window_.MeasureTextDip(text, c->dip.right - c->dip.left, c->maxLines);
        if (h != c->dip.bottom - c->dip.top) Relayout();
    }
}

std::wstring SettingsPage::GetText(int id) const { return WindowText(Item(id)); }

void SettingsPage::SetCheck(int id, bool checked) {
    if (HWND h = Item(id)) SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool SettingsPage::GetCheck(int id) const {
    HWND h = Item(id);
    return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void SettingsPage::SetSpin(int spinId, int value) {
    if (HWND h = Item(spinId)) SendMessageW(h, UDM_SETPOS32, 0, value);
}

int SettingsPage::GetSpin(int spinId) const {
    HWND h = Item(spinId);
    return h ? static_cast<int>(SendMessageW(h, UDM_GETPOS32, 0, 0)) : 0;
}

void SettingsPage::SetTrack(int id, int pos) {
    if (HWND h = Item(id)) SendMessageW(h, TBM_SETPOS, TRUE, pos);
}

int SettingsPage::GetTrack(int id) const {
    HWND h = Item(id);
    return h ? static_cast<int>(SendMessageW(h, TBM_GETPOS, 0, 0)) : 0;
}

void SettingsPage::Enable(int id, bool enable) {
    const Ctl* c = Find(id);
    if (!c || !c->hwnd) return;
    EnableWindow(c->hwnd, enable);
    if (c->buddy) {
        if (HWND b = Item(c->buddy)) {
            if (c->kind == CtlKind::Edit) EnableWindow(b, enable);
            InvalidateRect(b, nullptr, TRUE);  // a caption greys out with its button
        }
    }
}

void SettingsPage::Show(int id, bool show) {
    Ctl* ctl = nullptr;
    for (Ctl& c : table_) {
        if (c.id == id) ctl = &c;
    }
    HWND h = ctl ? ctl->hwnd : nullptr;
    if (ctl) ctl->shown = show;
    // Gone (collapsed, a closed expander): shown again with the rest when it comes back.
    if (!h || ctl->goneApplied) return;
    // The style bit, not IsWindowVisible: the page itself may be hidden.
    if (((GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0) == show) return;
    if (!show && GetFocus() == h) {
        // Keep the keyboard focus on the page: the next control in the tab order.
        if (HWND next = GetNextDlgTabItem(hwnd_, h, FALSE); next && next != h) SetFocus(next);
    }
    ShowWindow(h, show ? SW_SHOWNA : SW_HIDE);
}

void SettingsPage::Collapse(std::initializer_list<int> ids, bool hidden) {
    bool changed = false;
    for (int id : ids) {
        int buddy = 0;
        int sectionCard = -1;
        for (Ctl& c : table_) {
            if (c.id != id) continue;
            if (c.collapsed != hidden) changed = true;
            c.collapsed = hidden;
            // A check box / radio with its caption, an edit with its up-down, a label with its hint.
            if (c.kind == CtlKind::Check || c.kind == CtlKind::Radio || c.kind == CtlKind::Edit || c.kind == CtlKind::Label) buddy = c.buddy;
            // Round 9 phase 9E: a card's heading takes its "?" with it (a collapsed card left an
            // empty card with only the "?", e.g. a folder's Item window).
            if (c.kind == CtlKind::Section && c.card >= 0) sectionCard = c.card;
            // Round 10 phase 10D: so does a heading expander (Position's Exact offsets for a half circle).
            if (c.kind == CtlKind::Expander && c.heading && c.card >= 0) sectionCard = c.card;
        }
        for (Ctl& c : table_) {
            const bool help = sectionCard >= 0 && c.kind == CtlKind::Help && c.card == sectionCard;
            if (!help && (!buddy || c.id != buddy)) continue;
            if (c.collapsed != hidden) changed = true;
            c.collapsed = hidden;
        }
    }
    if (changed) Relayout();
}

void SettingsPage::Collapse(int id, bool hidden) { Collapse({id}, hidden); }

bool SettingsPage::ExpanderOpen(int id) const {
    return std::find(openExpanders_.begin(), openExpanders_.end(), id) != openExpanders_.end();
}

void SettingsPage::SetExpander(int id, bool open) {
    if (ExpanderOpen(id) == open) return;
    if (open) {
        openExpanders_.push_back(id);
    } else {
        openExpanders_.erase(std::remove(openExpanders_.begin(), openExpanders_.end(), id), openExpanders_.end());
    }
    Relayout();
    if (HWND h = Item(id)) InvalidateRect(h, nullptr, TRUE);
}

void SettingsPage::SetExpanderText(int id, const std::wstring& text) {
    for (Ctl& c : table_) {
        if (c.id != id || c.kind != CtlKind::Expander || c.text == text) continue;
        c.text = text;
        if (!c.hwnd) return;
        // The accessible name: a heading keeps its text, a "More" expander says "Less" while open.
        if (c.heading || !ExpanderOpen(id)) SetWindowTextW(c.hwnd, text.c_str());
        if (c.heading) {
            InvalidateRect(c.hwnd, nullptr, TRUE);
        } else {
            Relayout();  // its width follows its text
        }
        return;
    }
}

std::wstring SettingsPage::ChangedTitle(const wchar_t* title, int n) {
    return n > 0 ? std::wstring(title) + L" \x00B7 " + std::to_wstring(n) + L" changed" : std::wstring(title);
}

void SettingsPage::ScrollIntoView(int id) {
    if (HWND h = Item(id)) window_.EnsureVisible(h);
}

int SettingsPage::NoteHeightDip(int id, int widthDip) const {
    const Ctl* c = Find(id);
    if (!c) return 0;
    return window_.MeasureTextDip(c->hwnd && c->maxLines <= 0 ? WindowText(c->hwnd) : c->text, widthDip, c->maxLines);
}

void SettingsPage::MoveCtl(int id, RECT dip) {
    for (Ctl& c : table_) {
        if (c.id == id) c.dip = c.base = dip;
    }
}

void SettingsPage::PaintBackground(HDC dc) {
    if (cards_.empty() && hairlines_.empty() && flashRow_ < 0) return;
    const SettingsWindow& w = window_;
    const DarkMode::Palette& p = w.Colors();
    auto mix = [](COLORREF a, COLORREF b, int pa) {
        auto ch = [&](BYTE x, BYTE y) { return static_cast<BYTE>((x * pa + y * (100 - pa)) / 100); };
        return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
    };
    const COLORREF border = p.highContrast ? p.text : mix(p.text, p.layer, 14);
    const COLORREF hair = p.highContrast ? p.textDisabled : mix(p.text, p.layer, 9);
    HBRUSH fill = CreateSolidBrush(w.cardColor_);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, fill);
    HGDIOBJ op = SelectObject(dc, pen);
    const int radius = w.Px(6) * 2;
    for (const Card& card : cards_) {
        if (IsRectEmpty(&card.dip)) continue;
        RoundRect(dc, w.Px(card.dip.left), w.Px(card.dip.top) - scrollPx_, w.Px(card.dip.right),
                  w.Px(card.dip.bottom) - scrollPx_, radius, radius);
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    DeleteObject(fill);
    HBRUSH line = CreateSolidBrush(hair);
    const int t = std::max(1, w.Px(1));
    for (const Hairline& h : hairlines_) {
        const RECT r{w.Px(h.left), w.Px(h.y) - scrollPx_, w.Px(h.right), w.Px(h.y) - scrollPx_ + t};
        FillRect(dc, &r, line);
    }
    DeleteObject(line);
    // Round 9 phase 9B: the row the settings search went to, in the accent fading out.
    if (flashRow_ >= 0 && p.highContrast && !IsRectEmpty(&flashDip_)) {
        // High contrast: a frame in the highlight colour (no mixed colours), for as long as it lasts.
        RECT r{w.Px(flashDip_.left), w.Px(flashDip_.top) - scrollPx_, w.Px(flashDip_.right), w.Px(flashDip_.bottom) - scrollPx_};
        HBRUSH frame = CreateSolidBrush(p.accent);
        for (int k = 0; k < std::max(2, w.Px(2)); ++k) {
            FrameRect(dc, &r, frame);
            InflateRect(&r, -1, -1);
        }
        DeleteObject(frame);
    } else if (flashRow_ >= 0 && w.flashBrush_ && !IsRectEmpty(&flashDip_)) {
        const RECT r{w.Px(flashDip_.left), w.Px(flashDip_.top) - scrollPx_, w.Px(flashDip_.right), w.Px(flashDip_.bottom) - scrollPx_};
        HPEN none = CreatePen(PS_SOLID, 1, w.flashColor_);
        HGDIOBJ ob2 = SelectObject(dc, w.flashBrush_);
        HGDIOBJ op2 = SelectObject(dc, none);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, w.Px(4) * 2, w.Px(4) * 2);
        SelectObject(dc, ob2);
        SelectObject(dc, op2);
        DeleteObject(none);
    }
}

void SettingsPage::Relayout() {
    if (!hwnd_) return;
    window_.LayoutPage(*this);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
}

HWND SettingsPage::Owner() const { return GetAncestor(hwnd_, GA_ROOT); }

bool SettingsPage::ReadInt(int editId, int lo, int hi, int& value) {
    const std::wstring text = GetText(editId);
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    const long v = std::wcstol(text.c_str(), &end, 10);
    if (end == text.c_str()) return false;
    value = static_cast<int>(std::clamp<long>(v, lo, hi));
    if (const Ctl* e = Find(editId); e && e->buddy) {
        SetSpin(e->buddy, value);  // also rewrites the (clamped) text
    } else {
        SetText(editId, std::to_wstring(value));
    }
    return true;
}

// --- SettingsWindow: creation ----------------------------------------------------------------

namespace {
// DarkMode::TryEnableDarkPopups is process-wide: on while any settings window is open.
int g_darkPopupWindows = 0;
}  // namespace

SettingsWindow::SettingsWindow(SettingsHost host, SettingsKind kind, std::wstring dock, size_t item)
    : host_(std::move(host)), kind_(kind), dockId_(std::move(dock)), item_(item) {
    scratch_.items.clear();
}

bool SettingsWindow::FocusCuesHidden(HWND hwnd) {
    return (SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
}

SettingsWindow::~SettingsWindow() {
    if (hwnd_) DestroyWindow(hwnd_);  // WM_DESTROY / WM_NCDESTROY release everything
    ClearPreviews();
}

size_t SettingsWindow::DockIndex() const {
    const Config* c = host_.config;
    if (!c) return SIZE_MAX;
    for (size_t i = 0; i < c->docks.size(); ++i) {
        if (c->docks[i].id == dockId_) return i;
    }
    return kind_ == SettingsKind::Global && !c->docks.empty() ? 0 : SIZE_MAX;
}

DockConfig& SettingsWindow::Dock() {
    const size_t i = DockIndex();
    return i != SIZE_MAX ? host_.config->docks[i] : scratch_;
}

bool SettingsWindow::IsFolderWindow() const {
    const size_t i = DockIndex();
    return kind_ == SettingsKind::Dock && i != SIZE_MAX && host_.config->docks[i].IsFolderWindow();
}

bool SettingsWindow::IsWidgetWindow() const {
    const size_t i = DockIndex();
    return kind_ == SettingsKind::Dock && i != SIZE_MAX && host_.config->docks[i].IsWidgetWindow();
}

void SettingsWindow::SelectDock(const std::wstring& id) {
    if (kind_ != SettingsKind::Global || id == dockId_) return;
    dockId_ = id;
}

DockItemEntry* SettingsWindow::TargetItem() {
    if (kind_ != SettingsKind::Item) return nullptr;
    DockConfig& d = Dock();
    return item_ < d.items.size() ? &d.items[item_] : nullptr;
}

void SettingsWindow::Retarget(size_t item) {
    if (kind_ != SettingsKind::Item || item == item_) return;
    item_ = item;
    if (!hwnd_) return;
    for (size_t i = 0; i < pages_.size(); ++i) {
        pages_[i]->SelectItem(static_cast<int>(item));
        RebuildPage(static_cast<int>(i));
    }
    UpdateIdentity();
}

COLORREF SettingsWindow::DockColor() const {
    const size_t i = DockIndex();
    const uint32_t argb = i != SIZE_MAX ? host_.config->DockColor(i) : DockPaletteColor(0);
    return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

SIZE SettingsWindow::ClientDip() const {
    if (clientDip_.cx > 0 && clientDip_.cy > 0) return clientDip_;
    const int k = KindIndex();
    SettingsWindowSize s = host_.config ? host_.config->settings.windows[k] : SettingsWindowSize{};
    if (s.w <= 0 || s.h <= 0) s = SettingsUiConfig::kDefault[k];
    const SIZE m = MinClientDip();
    return SIZE{std::clamp(s.w, static_cast<int>(m.cx), SettingsUiConfig::kMaximum),
                std::clamp(s.h, static_cast<int>(m.cy), SettingsUiConfig::kMaximum)};
}

SIZE SettingsWindow::MinClientDip() const {
    const SettingsWindowSize m = SettingsUiConfig::kMinimum[KindIndex()];
    return SIZE{m.w, m.h};
}

void SettingsWindow::SaveClientSize() {
    if (!hwnd_ || !host_.config) return;
    SIZE px{};
    if (IsZoomed(hwnd_) || IsIconic(hwnd_)) {
        // The restored size: the normal position less the frame.
        WINDOWPLACEMENT wpl{sizeof(wpl)};
        if (!GetWindowPlacement(hwnd_, &wpl)) return;
        RECT frame{0, 0, 0, 0};
        AdjustWindowRectExForDpi(&frame, kStyle, FALSE, 0, dpi_);
        px.cx = (wpl.rcNormalPosition.right - wpl.rcNormalPosition.left) - (frame.right - frame.left);
        px.cy = (wpl.rcNormalPosition.bottom - wpl.rcNormalPosition.top) - (frame.bottom - frame.top);
    } else {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        px = SIZE{rc.right, rc.bottom};
    }
    if (px.cx <= 0 || px.cy <= 0) return;
    const int k = KindIndex();
    const SIZE m = MinClientDip();
    SettingsWindowSize s{std::clamp(MulDiv(px.cx, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi_)), static_cast<int>(m.cx),
                                    SettingsUiConfig::kMaximum),
                         std::clamp(MulDiv(px.cy, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi_)), static_cast<int>(m.cy),
                                    SettingsUiConfig::kMaximum)};
    if (s == SettingsUiConfig::kDefault[k]) s = SettingsWindowSize{};
    SettingsWindowSize& saved = host_.config->settings.windows[k];
    if (s == saved) return;
    saved = s;
    if (host_.changed) host_.changed(SettingsScope::WindowSize, DockIndex());
}

bool SettingsWindow::PreviewOn(int index) const {
    switch (kind_) {
        case SettingsKind::Global: return index == SettingsIds::kPageThemes || index == SettingsIds::kPageAnimation;
        // Round 8 phase 8B: only the Appearance tab (what the strip draws); Position has the silhouette.
        case SettingsKind::Dock: return !IsFolderWindow() && !IsWidgetWindow() && index == SettingsIds::kTabAppearance;
        case SettingsKind::Item:
        default: return false;
    }
}

HWND SettingsWindow::DefaultFocus() const {
    if (nav_) return nav_;
    if (tabs_) return tabs_;
    if (SettingsPage* page = ActivePage(); page && page->hwnd_) {
        // Round 9 phase 9D: the first setting, not a card's "?" button (it stays in the tab order).
        HWND first = GetNextDlgTabItem(page->hwnd_, nullptr, FALSE);
        for (HWND h = first; h;) {
            const Ctl* c = page->Find(h);
            if (!c || c->kind != CtlKind::Help) return h;
            h = GetNextDlgTabItem(page->hwnd_, h, FALSE);
            if (h == first) break;
        }
        if (first) return first;
    }
    return hwnd_;
}

bool SettingsWindow::RegisterClasses(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW nav{sizeof(nav)};
    nav.lpfnWndProc = NavProcThunk;
    nav.hInstance = instance;
    nav.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    nav.lpszClassName = kNavClassName;
    if (!RegisterClassExW(&nav) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW page{sizeof(page)};
    page.lpfnWndProc = PageProcThunk;
    page.hInstance = instance;
    page.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    page.lpszClassName = kPageClassName;
    if (!RegisterClassExW(&page) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW band{sizeof(band)};
    band.lpfnWndProc = BandProcThunk;
    band.hInstance = instance;
    band.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    band.lpszClassName = kBandClassName;
    if (!RegisterClassExW(&band) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW tabs{sizeof(tabs)};
    tabs.lpfnWndProc = TabsProcThunk;
    tabs.hInstance = instance;
    tabs.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    tabs.lpszClassName = kTabsClassName;
    if (!RegisterClassExW(&tabs) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW find{sizeof(find)};  // round 9 phase 9B: the nav column's Find a setting… box
    find.lpfnWndProc = FindProcThunk;
    find.hInstance = instance;
    find.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    find.lpszClassName = kFindClassName;
    if (!RegisterClassExW(&find) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    registered = true;
    return true;
}

SIZE SettingsWindow::WindowSizeFor(UINT dpi) const {
    const SIZE c = ClientDip();
    RECT r{0, 0, MulDiv(c.cx, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
           MulDiv(c.cy, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)};
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, dpi);
    return SIZE{r.right - r.left, r.bottom - r.top};
}

RECT SettingsWindow::InitialRect() const {
    const SIZE s = WindowSizeFor(dpi_);
    RECT mon = host_.dockMonitor ? host_.dockMonitor() : RECT{};
    HMONITOR hmon = IsRectEmpty(&mon) ? MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)
                                      : MonitorFromRect(&mon, MONITOR_DEFAULTTOPRIMARY);
    // An Item window: 40 DIPs right / down of its dock's window when that is open.
    const RECT anchor = kind_ == SettingsKind::Item && host_.anchor ? host_.anchor() : RECT{};
    if (!IsRectEmpty(&anchor)) hmon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hmon, &mi);
    const RECT& work = mi.rcWork;  // the full monitor while the dock hides the taskbar
    int x = work.left + (work.right - work.left - s.cx) / 2;
    int y = work.top + (work.bottom - work.top - s.cy) / 2;
    if (!IsRectEmpty(&anchor)) {
        x = anchor.left + Px(kItemOffsetDip);
        y = anchor.top + Px(kItemOffsetDip);
        x = std::min(x, static_cast<int>(work.right) - static_cast<int>(s.cx));
        y = std::min(y, static_cast<int>(work.bottom) - static_cast<int>(s.cy));
    } else if (host_.dockBar) {
        // Never over the dock: kAboveBarDip from the bar at rest, on the side toward the middle
        // of the monitor (above a bottom dock, below a top one, beside a left / right one).
        const RECT bar = host_.dockBar();
        if (!IsRectEmpty(&bar)) {
            const int gap = Px(kAboveBarDip);
            if (bar.right - bar.left >= bar.bottom - bar.top) {
                if (bar.top > (work.top + work.bottom) / 2) {
                    y = std::min(y, static_cast<int>(bar.top - s.cy) - gap);
                } else {
                    y = std::max(y, static_cast<int>(bar.bottom) + gap);
                }
            } else if (bar.left > (work.left + work.right) / 2) {
                x = std::min(x, static_cast<int>(bar.left - s.cx) - gap);
            } else {
                x = std::max(x, static_cast<int>(bar.right) + gap);
            }
        }
    }
    x = std::min(x, static_cast<int>(work.right) - static_cast<int>(s.cx));
    y = std::min(y, static_cast<int>(work.bottom) - static_cast<int>(s.cy));
    y = std::max(y, static_cast<int>(work.top));
    x = std::max(x, static_cast<int>(work.left));
    return RECT{x, y, x + s.cx, y + s.cy};
}

bool SettingsWindow::Create(HINSTANCE instance, bool show) {
    instance_ = instance;
    static bool commonControls = false;
    if (!commonControls) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS |
                                                  ICC_LINK_CLASS | ICC_LISTVIEW_CLASSES};
        commonControls = InitCommonControlsEx(&icc) != FALSE;
    }
    if (!RegisterClasses(instance)) return false;
    if (kind_ == SettingsKind::Item) {
        // The page is chosen by the item's kind: it must exist.
        if (!TargetItem()) return false;
    } else if (kind_ == SettingsKind::Dock && DockIndex() == SIZE_MAX) {
        return false;
    }

    // Process-wide (undocumented, optional): popups of windows allowed dark follow the app mode.
    darkPopups_ = g_darkPopupWindows > 0 || DarkMode::TryEnableDarkPopups(true);
    if (darkPopups_) ++g_darkPopupWindows;

    dpi_ = host_.dockDpi ? std::max<UINT>(host_.dockDpi(), USER_DEFAULT_SCREEN_DPI / 2) : USER_DEFAULT_SCREEN_DPI;
    RECT r = InitialRect();
    // No owner: an owned window of the topmost, non-activating dock would inherit topmost.
    CreateWindowExW(0, kClassName, L"WinDock settings", kStyle, r.left, r.top, r.right - r.left, r.bottom - r.top,
                    nullptr, nullptr, instance, this);
    if (!hwnd_) {
        if (darkPopups_ && --g_darkPopupWindows == 0) DarkMode::TryEnableDarkPopups(false);
        darkPopups_ = false;
        return false;
    }
    // The monitor the window landed on decides its DPI; normally the dock's.
    if (const UINT actual = GetDpiForWindow(hwnd_); actual != dpi_) {
        clientDip_ = SIZE{};  // measured at the wrong DPI: the saved / default size again
        dpi_ = actual;
        r = InitialRect();
        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    palette_ = DarkMode::CurrentPalette();
    CreateBrushes();
    UpdateIcons();

    if (kind_ == SettingsKind::Global) {
        // Round 9 phase 9B: the Find a setting… box first (the first tab stop), then the page list.
        find_ = CreateWindowExW(0, kFindClassName, L"Find a setting\x2026", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP, 0, 0, 0,
                                0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kFindBox)), instance, this);
        nav_ = CreateWindowExW(0, kNavClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP, 0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kNav)), instance, this);
    }
    CreateBands(false);
    pages_ = CreateSettingsPages(*this);
    // Round 10 phase 10C: an Item window with two pages (a widget's Content and Appearance) has
    // the Dock window's tab strip too.
    if ((kind_ == SettingsKind::Dock || kind_ == SettingsKind::Item) && pages_.size() > 1) {
        tabs_ = CreateWindowExW(0, kTabsClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP, 0, 0, 0, 0, hwnd_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kTabs)), instance, this);
    }
    for (size_t i = 0; i < pages_.size(); ++i) {
        if (kind_ == SettingsKind::Item) pages_[i]->SelectItem(static_cast<int>(item_));
        if (!CreatePage(*pages_[i], static_cast<int>(i))) {
            DestroyWindow(hwnd_);
            return false;
        }
    }
    if (kind_ == SettingsKind::Global || (kind_ == SettingsKind::Dock && !IsFolderWindow() && !IsWidgetWindow())) {
        preview_ = std::make_unique<SettingsPreview>();
        if (preview_->Create(instance, hwnd_, SettingsIds::kPreview, host_.config, dpi_)) {
            // Global: the global theme on the first dock's items (round 5 phase D: not the Docks
            // and displays page's selection); a Dock window: its dock.
            preview_->SetDock(
                [this]() -> const DockConfig* {
                    if (kind_ == SettingsKind::Global) {
                        for (const DockConfig& d : host_.config->docks) {
                            if (d.IsDock()) return &d;
                        }
                        return nullptr;
                    }
                    return DockIndex() != SIZE_MAX ? &Dock() : nullptr;
                },
                kind_ == SettingsKind::Global);
        } else {
            preview_.reset();  // the window works without it
        }
    }
    if (kind_ == SettingsKind::Item) CreateBands(true);
    UpdateItemIcon();
    CreateFonts();  // also hands them to the controls
    ApplyTheme();
    Layout();
    Refresh();
    active_ = -1;
    SelectPage(0);
    if (!show) return true;  // round 9 phase 9B: hidden for the search's index (Activate shows it)
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    if (!lastFocus_) SetFocus(DefaultFocus());
    return true;
}

bool SettingsWindow::CreatePage(SettingsPage& page, int index) {
    CreateWindowExW(WS_EX_CONTROLPARENT, kPageClassName, page.Name().c_str(), WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0,
                    hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(SettingsIds::kPageBase + index)), instance_,
                    &page);
    if (!page.hwnd_) return false;
    page.ResetTable();
    page.Build();
    CreateControls(page);
    page.Created();
    CreateTips(page);
    return true;
}

void SettingsWindow::CreateTips(SettingsPage& page) {
    if (page.tip_) DestroyWindow(page.tip_);
    page.tip_ = nullptr;
    const bool any = std::any_of(page.table_.begin(), page.table_.end(), [](const Ctl& c) { return c.hwnd && !c.tip.empty(); });
    if (!any || !hwnd_) return;
    // Owned by the settings window (destroyed with it); one per page.
    page.tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, instance_, nullptr);
    if (!page.tip_) return;
    SendMessageW(page.tip_, TTM_SETMAXTIPWIDTH, 0, Px(360));
    SendMessageW(page.tip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    if (font_) SendMessageW(page.tip_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    for (Ctl& c : page.table_) {
        if (!c.hwnd || c.tip.empty()) continue;
        TTTOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = page.hwnd_;
        ti.uId = reinterpret_cast<UINT_PTR>(c.hwnd);
        ti.lpszText = c.tip.data();
        SendMessageW(page.tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    // Round 9 phase 9A: a disabled control gets no mouse input and a tooltip shows nothing for a
    // disabled window, so a greyed control's tooltip (which says why it is greyed) is a rect tool
    // of the page, placed over the disabled control the pointer is on (PageProc, WM_MOUSEMOVE).
    TTTOOLINFOW proxy{};
    proxy.cbSize = sizeof(proxy);
    proxy.hwnd = page.hwnd_;
    proxy.uId = SettingsPage::kDisabledTipId;
    proxy.lpszText = const_cast<wchar_t*>(L"");
    SendMessageW(page.tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&proxy));
    page.tipProxy_ = nullptr;
    ThemeTips(page.tip_);
}

void SettingsWindow::ThemeTips(HWND tip) const {
    if (!tip) return;
    // Dark tooltips like the lists' (the Explorer dark visual style); light mode: the default.
    DarkMode::AllowDarkForWindow(tip, palette_.dark);
    SetWindowTheme(tip, palette_.dark ? L"DarkMode_Explorer" : nullptr, nullptr);
}

void SettingsWindow::CreateControls(SettingsPage& page) {
    for (Ctl& c : page.table_) {
        c.hwnd = CreateWindowExW(c.exStyle, c.cls, c.text.c_str(), WS_CHILD | WS_VISIBLE | c.style, 0, 0, 0, 0,
                                 page.hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(c.id)), instance_, nullptr);
        if (!c.hwnd) continue;
        switch (c.kind) {
            case CtlKind::List:
                ListView_SetExtendedListViewStyle(c.hwnd, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
                SetWindowSubclass(c.hwnd, ListSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
                break;
            case CtlKind::Trackbar:
                SendMessageW(c.hwnd, TBM_SETRANGEMIN, FALSE, c.lo);
                SendMessageW(c.hwnd, TBM_SETRANGEMAX, FALSE, c.hi);
                SendMessageW(c.hwnd, TBM_SETPAGESIZE, 0, c.step);
                SendMessageW(c.hwnd, TBM_SETLINESIZE, 0, 1);
                break;
            case CtlKind::Spin: {
                SendMessageW(c.hwnd, UDM_SETBUDDY, reinterpret_cast<WPARAM>(page.Item(c.buddy)), 0);
                SendMessageW(c.hwnd, UDM_SETRANGE32, static_cast<WPARAM>(c.lo), c.hi);
                UDACCEL accel[] = {{0, static_cast<UINT>(c.step)}, {2, static_cast<UINT>(c.step * 5)}};
                SendMessageW(c.hwnd, UDM_SETACCEL, std::size(accel), reinterpret_cast<LPARAM>(accel));
                break;
            }
            case CtlKind::Edit:
                if (c.hi > 0) SendMessageW(c.hwnd, EM_SETLIMITTEXT, static_cast<WPARAM>(c.hi), 0);
                break;
            case CtlKind::Help:  // round 9 phase 9D: hover tracking for the accent
                SetWindowSubclass(c.hwnd, HelpSubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
                break;
            case CtlKind::Link: {
                // Links in the WM_CTLCOLORSTATIC text colour (the palette's link colour) instead
                // of the fixed dark blue, which is unreadable on the dark layer.
                LITEM item{};
                item.mask = LIF_ITEMINDEX | LIF_STATE;
                item.iLink = 0;
                item.state = item.stateMask = LIS_DEFAULTCOLORS;
                SendMessageW(c.hwnd, LM_SETITEM, 0, reinterpret_cast<LPARAM>(&item));
                break;
            }
            default:
                break;
        }
    }
}

void SettingsWindow::Activate() {
    if (!hwnd_) return;
    if (!IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);  // round 9 phase 9B: created hidden for a search
    } else if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    SetForegroundWindow(hwnd_);
}

void SettingsWindow::Refresh() {
    if (!hwnd_) return;
    loading_ = true;
    for (auto& page : pages_) page->Load();
    loading_ = false;
    if (preview_) preview_->Invalidate();
    UpdateIdentity();
}

void SettingsWindow::ShowPage(int index) {
    if (!hwnd_ || index < 0 || index >= static_cast<int>(pages_.size())) return;
    SelectPage(index);
}

void SettingsWindow::WeatherChanged() {
    if (!hwnd_) return;
    loading_ = true;
    for (auto& page : pages_) page->OnWeatherChanged();
    loading_ = false;
}

void SettingsWindow::NotificationsChanged() {
    if (!hwnd_) return;
    loading_ = true;
    for (auto& page : pages_) page->OnNotificationsChanged();
    loading_ = false;
}

void SettingsWindow::CalendarChanged() {
    if (!hwnd_) return;
    loading_ = true;
    for (auto& page : pages_) page->OnCalendarChanged();
    loading_ = false;
}

void SettingsWindow::ConfigApplied() {
    if (preview_) preview_->Invalidate();
}

void SettingsWindow::PeersChanged(const SettingsPage* source) {
    if (!hwnd_ || loading_) return;
    loading_ = true;
    for (auto& page : pages_) {
        if (page.get() != source && page->Hwnd()) page->OnPeerChanged();
    }
    loading_ = false;
}

void SettingsWindow::StyleChanged() {
    loading_ = true;
    for (auto& page : pages_) page->OnStyleChanged();
    loading_ = false;
}

// --- resources -------------------------------------------------------------------------------

void SettingsWindow::CreateFonts() {
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
    HFONT oldFont = font_, oldSemibold = fontSemibold_, oldTitle = fontTitle_, oldIcons = fontIcons_, oldCaption = fontCaption_;
    font_ = CreateFontIndirectW(&lf);
    LOGFONTW semibold = lf;
    semibold.lfWeight = FW_SEMIBOLD;
    fontSemibold_ = CreateFontIndirectW(&semibold);
    // Round 8 phase 8D: the nav's group captions, a little smaller than the message font.
    LOGFONTW caption = semibold;
    caption.lfHeight = lf.lfHeight < 0 ? std::min(-1L, lf.lfHeight * 5 / 6) : -Px(10);
    fontCaption_ = CreateFontIndirectW(&caption);
    LOGFONTW title = semibold;
    title.lfHeight = lf.lfHeight < 0 ? lf.lfHeight * 2 : -Px(24);
    fontTitle_ = CreateFontIndirectW(&title);
    static const wchar_t* iconFace = FontFamilyExists(L"Segoe Fluent Icons")  ? L"Segoe Fluent Icons"
                                     : FontFamilyExists(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets"
                                                                              : nullptr;
    fontIcons_ = nullptr;
    if (iconFace) {
        LOGFONTW icons{};
        icons.lfHeight = -Px(16);
        icons.lfWeight = FW_NORMAL;
        icons.lfCharSet = DEFAULT_CHARSET;
        icons.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(icons.lfFaceName, iconFace);
        fontIcons_ = CreateFontIndirectW(&icons);
    }
    for (auto& page : pages_) {
        for (const Ctl& c : page->table_) {
            if (!c.hwnd) continue;
            HFONT f = c.kind == CtlKind::Title ? fontTitle_ : c.kind == CtlKind::Section ? fontSemibold_ : font_;
            SendMessageW(c.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
        }
        if (page->tip_) {
            SendMessageW(page->tip_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
            SendMessageW(page->tip_, TTM_SETMAXTIPWIDTH, 0, Px(360));
        }
    }
    // The header / footer bands' buttons and links.
    for (HWND band : {header_, footer_}) {
        for (HWND c = band ? GetWindow(band, GW_CHILD) : nullptr; c; c = GetWindow(c, GW_HWNDNEXT)) {
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        }
    }
    if (headerTip_) {  // round 8 phase 8C: the Item header's description tooltip
        SendMessageW(headerTip_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        SendMessageW(headerTip_, TTM_SETMAXTIPWIDTH, 0, Px(360));
    }
    for (HFONT f : {oldFont, oldSemibold, oldTitle, oldIcons, oldCaption}) {
        if (f) DeleteObject(f);
    }
}

void SettingsWindow::DestroyFonts() {
    for (HFONT* f : {&font_, &fontSemibold_, &fontTitle_, &fontIcons_, &fontCaption_}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

void SettingsWindow::CreateBrushes() {
    DestroyBrushes();
    layerBrush_ = CreateSolidBrush(palette_.layer);
    baseBrush_ = CreateSolidBrush(palette_.base);
    controlBrush_ = CreateSolidBrush(palette_.control);
    bandColor_ = BandColorFor();
    bandBrush_ = CreateSolidBrush(bandColor_);
    // Round 8 phase 8A: a card is the layer mixed 4 % toward the text (high contrast: the layer).
    const COLORREF a = palette_.text, b = palette_.layer;
    const int pa = palette_.highContrast ? 0 : 4;
    auto ch = [&](BYTE x, BYTE y) { return static_cast<BYTE>((x * pa + y * (100 - pa)) / 100); };
    cardColor_ = RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
    cardBrush_ = CreateSolidBrush(cardColor_);
}

COLORREF SettingsWindow::BandColorFor() const {
    // The bands: the content layer, tinted with the dock's colour in a Dock / Item window.
    if (kind_ == SettingsKind::Global || palette_.highContrast) return palette_.layer;
    const COLORREF a = DockColor(), b = palette_.layer;
    const int pa = palette_.dark ? 14 : 10;
    auto ch = [&](BYTE x, BYTE y) { return static_cast<BYTE>((x * pa + y * (100 - pa)) / 100); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

void SettingsWindow::DestroyBrushes() {
    for (HBRUSH* b : {&layerBrush_, &baseBrush_, &controlBrush_, &bandBrush_, &cardBrush_, &flashBrush_}) {
        if (*b) DeleteObject(*b);
        *b = nullptr;
    }
}

void SettingsWindow::UpdateIcons() {
    HICON iconSmall = nullptr, iconBig = nullptr;
    LoadIconWithScaleDown(instance_, MAKEINTRESOURCEW(IDI_APP), GetSystemMetricsForDpi(SM_CXSMICON, dpi_),
                          GetSystemMetricsForDpi(SM_CYSMICON, dpi_), &iconSmall);
    LoadIconWithScaleDown(instance_, MAKEINTRESOURCEW(IDI_APP), GetSystemMetricsForDpi(SM_CXICON, dpi_),
                          GetSystemMetricsForDpi(SM_CYICON, dpi_), &iconBig);
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall));
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig));
    DestroyIcons();
    iconSmall_ = iconSmall;
    iconBig_ = iconBig;
}

void SettingsWindow::DestroyIcons() {
    if (iconSmall_) DestroyIcon(iconSmall_);
    if (iconBig_) DestroyIcon(iconBig_);
    iconSmall_ = iconBig_ = nullptr;
}

void SettingsWindow::ApplyTheme() {
    palette_ = DarkMode::CurrentPalette();
    const bool dark = palette_.dark;
    CreateBrushes();
    if (darkPopups_) {
        DarkMode::TryEnableDarkPopups(true);  // flushes the menu themes
        DarkMode::AllowDarkForWindow(hwnd_, dark);
    }
    DarkMode::SetDarkCaption(hwnd_, dark);
    ApplyCaptionColor();
    ApplyBackdrop();
    for (HWND band : {header_, footer_}) {
        for (HWND c = band ? GetWindow(band, GW_CHILD) : nullptr; c; c = GetWindow(c, GW_HWNDNEXT)) {
            DarkMode::ThemeControl(c, dark);  // buttons and links
        }
    }
    ThemeTips(headerTip_);
    for (auto& page : pages_) {
        if (!page->hwnd_) continue;
        DarkMode::ThemeScrollBars(page->hwnd_, dark);
        ThemeTips(page->tip_);
        for (const Ctl& c : page->table_) {
            if (!c.hwnd) continue;
            switch (c.kind) {
                case CtlKind::Button:
                case CtlKind::Check:
                case CtlKind::Radio:
                case CtlKind::Edit:
                case CtlKind::Spin:
                case CtlKind::Combo:
                case CtlKind::Link:
                    DarkMode::ThemeControl(c.hwnd, dark);
                    break;
                case CtlKind::Trackbar:
                    // No dark theme (custom-drawn in dark mode: DrawTrackbar), but a trackbar
                    // caches its background brush: a theme reset makes it ask WM_CTLCOLORSTATIC again.
                    SetWindowTheme(c.hwnd, nullptr, nullptr);
                    break;
                case CtlKind::List:
                    ThemeList(c.hwnd);
                    break;
                default:
                    break;
            }
            UnselectComboText(c);
        }
    }
    if (preview_) {
        // The strip sits on the content layer, with a hairline between it and the page.
        const COLORREF a = palette_.layer, b = palette_.text;
        auto mix = [](BYTE x, BYTE y) { return static_cast<BYTE>((x * 88 + y * 12) / 100); };
        preview_->SetColors(a, RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
                                   mix(GetBValue(a), GetBValue(b))));
    }
    StyleChanged();
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void SettingsWindow::ThemeList(HWND list) {
    const bool dark = palette_.dark;
    DarkMode::AllowDarkForWindow(list, dark);
    SetWindowTheme(list, dark ? L"DarkMode_ItemsView" : L"Explorer", nullptr);
    if (HWND header = ListView_GetHeader(list)) {
        DarkMode::AllowDarkForWindow(header, dark);
        SetWindowTheme(header, dark ? L"DarkMode_ItemsView" : L"Explorer", nullptr);
    }
    ListView_SetBkColor(list, palette_.control);
    ListView_SetTextBkColor(list, CLR_NONE);  // rows on the list background (no lighter bands)
    ListView_SetTextColor(list, palette_.text);
    InvalidateRect(list, nullptr, TRUE);
}

LRESULT CALLBACK SettingsWindow::ListSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                                  DWORD_PTR data) {
    auto* self = reinterpret_cast<SettingsWindow*>(data);
    if (msg == WM_NOTIFY && self && self->palette_.dark) {
        // The header's dark theme keeps dark item text: draw it in the palette's text colour.
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == ListView_GetHeader(hwnd)) {
            auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lp);
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(cd->hdc, self->palette_.text);
                return CDRF_DODEFAULT;
            }
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, ListSubclassProc, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT SettingsWindow::DrawTrackbar(NMCUSTOMDRAW& cd) {
    // Trackbars have no dark visual style: paint the channel and the thumb (Windows 11 shapes).
    if (!palette_.dark || palette_.highContrast) return CDRF_DODEFAULT;
    if (cd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    HWND tb = cd.hdr.hwndFrom;
    const bool enabled = IsWindowEnabled(tb) != FALSE;
    const COLORREF accent = enabled ? palette_.accent : palette_.textDisabled;
    auto fill = [&](const RECT& r, COLORREF color, int radius) {
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ oldBrush = SelectObject(cd.hdc, brush);
        HGDIOBJ oldPen = SelectObject(cd.hdc, pen);
        if (radius > 0) {
            RoundRect(cd.hdc, r.left, r.top, r.right, r.bottom, radius, radius);
        } else {
            Ellipse(cd.hdc, r.left, r.top, r.right, r.bottom);
        }
        SelectObject(cd.hdc, oldBrush);
        SelectObject(cd.hdc, oldPen);
        DeleteObject(pen);
        DeleteObject(brush);
    };
    RECT thumb{};
    SendMessageW(tb, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    const int thumbX = (thumb.left + thumb.right) / 2;
    switch (cd.dwItemSpec) {
        case TBCD_CHANNEL: {
            const int h = std::max(2, Px(4));
            const int cy = (cd.rc.top + cd.rc.bottom) / 2;
            const RECT left{cd.rc.left, cy - h / 2, std::max(static_cast<int>(cd.rc.left) + h, thumbX), cy - h / 2 + h};
            const RECT right{std::min(static_cast<int>(cd.rc.right) - h, thumbX), left.top, cd.rc.right, left.bottom};
            fill(right, RGB(0x9A, 0x9A, 0x9A), h);
            fill(left, accent, h);
            return CDRF_SKIPDEFAULT;
        }
        case TBCD_THUMB: {
            RECT client{};
            GetClientRect(tb, &client);
            const int d = std::min({Px(18), static_cast<int>(client.bottom - client.top),
                                    static_cast<int>(thumb.right - thumb.left) + Px(8)});
            const int cy = (thumb.top + thumb.bottom) / 2;
            const RECT outer{thumbX - d / 2, cy - d / 2, thumbX - d / 2 + d, cy - d / 2 + d};
            fill(outer, RGB(0x45, 0x45, 0x45), 0);
            const bool hot = (cd.uItemState & (CDIS_HOT | CDIS_SELECTED)) != 0;
            const int inner = std::max(4, d * (hot ? 6 : 5) / 10);
            const RECT in{thumbX - inner / 2, cy - inner / 2, thumbX - inner / 2 + inner, cy - inner / 2 + inner};
            fill(in, accent, 0);
            return CDRF_SKIPDEFAULT;
        }
        case TBCD_TICS:
            return CDRF_SKIPDEFAULT;
        default:
            return CDRF_DODEFAULT;
    }
}

void SettingsWindow::ApplyBackdrop() {
    // Mica shows only where the client area is transparent (frame extended), and GDI controls
    // cannot paint alpha: so Mica behind the caption and the nav column only.
    // Dock / Item windows have no nav column (their caption has the dock's colour).
    const bool want = kind_ == SettingsKind::Global && kNavMica && !palette_.highContrast &&
                      Backdrop::SystemAllowsTransparency();
    bool mica = want && DarkMode::SetMicaBackdrop(hwnd_, true);
    if (!mica) DarkMode::SetMicaBackdrop(hwnd_, false);
    MARGINS margins{mica ? Px(kNavWidthDip) : 0, 0, 0, 0};
    if (FAILED(DwmExtendFrameIntoClientArea(hwnd_, &margins)) && mica) {
        mica = false;
        DarkMode::SetMicaBackdrop(hwnd_, false);
    }
    mica_ = mica;
    if (nav_) InvalidateRect(nav_, nullptr, TRUE);
}

int SettingsWindow::CheckGlyphPx(bool radio) const {
    SIZE s{};
    if (HTHEME theme = OpenThemeDataForDpi(hwnd_, L"BUTTON", dpi_)) {
        GetThemePartSize(theme, nullptr, radio ? BP_RADIOBUTTON : BP_CHECKBOX, 1, nullptr, TS_DRAW, &s);
        CloseThemeData(theme);
    }
    if (s.cx <= 0) s.cx = GetSystemMetricsForDpi(SM_CXMENUCHECK, dpi_);
    return s.cx;
}

// --- layout ----------------------------------------------------------------------------------

void SettingsWindow::Layout() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    // Global: the nav column on the left, the subtitle band over the pages. Dock: the header and
    // the tab strip over the pages. Item: the header over, the footer under the page.
    const int navW = nav_ ? Px(kNavWidthDip) : 0;
    // Round 9 phase 9B: the Find a setting… box over the page list.
    const int findH = find_ ? std::min(Px(kNavFindDip), static_cast<int>(rc.bottom)) : 0;
    if (find_) SetWindowPos(find_, nullptr, Px(12), Px(12), std::max(0, navW - Px(24)), Px(30), SWP_NOZORDER | SWP_NOACTIVATE);
    if (nav_) SetWindowPos(nav_, nullptr, 0, findH, navW, std::max(0, static_cast<int>(rc.bottom) - findH), SWP_NOZORDER | SWP_NOACTIVATE);
    int top = 0;
    int bottom = rc.bottom;
    if (header_) {
        const int h = Px(kind_ == SettingsKind::Global ? kSubtitleDip
                         : kind_ == SettingsKind::Dock ? kDockHeaderDip
                                                       : kItemHeaderDip);
        SetWindowPos(header_, nullptr, navW, 0, std::max(0, static_cast<int>(rc.right) - navW), h, SWP_NOZORDER | SWP_NOACTIVATE);
        top = h;
    }
    if (tabs_) {
        SetWindowPos(tabs_, nullptr, 0, top, rc.right, Px(kTabsDip), SWP_NOZORDER | SWP_NOACTIVATE);
        top += Px(kTabsDip);
    }
    if (footer_) {
        const int h = std::min(Px(kItemFooterDip), bottom - top);
        SetWindowPos(footer_, nullptr, 0, bottom - h, rc.right, h, SWP_NOZORDER | SWP_NOACTIVATE);
        bottom -= h;
    }
    const int strip = std::min(Px(kFooterDip), std::max(0, bottom - top));
    if (preview_) {
        SetWindowPos(preview_->Hwnd(), nullptr, navW, bottom - strip, std::max(0, static_cast<int>(rc.right) - navW), strip,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (size_t i = 0; i < pages_.size(); ++i) {
        SettingsPage& page = *pages_[i];
        if (!page.hwnd_) continue;
        const int h = std::max(0, bottom - top - (preview_ && PreviewOn(static_cast<int>(i)) ? strip : 0));
        SetWindowPos(page.hwnd_, nullptr, navW, top, rc.right - navW, h, SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutPage(page);
        // Round 8 phase 8A: a resized window: list columns, the Contents tab's own layout.
        if (page.layoutW_ != page.Width()) {
            const bool first = page.layoutW_ == 0;
            page.layoutW_ = page.Width();
            if (!first || page.Width() != page.buildW_) page.OnResized();
        }
    }
    LayoutBands();
    InvalidateRect(hwnd_, nullptr, TRUE);
    if (nav_) InvalidateRect(nav_, nullptr, TRUE);
    if (find_) InvalidateRect(find_, nullptr, TRUE);
    if (tabs_) InvalidateRect(tabs_, nullptr, TRUE);
}

int SettingsWindow::MeasureDip(HDC dc, const std::wstring& text, int widthDip, int maxLines) const {
    if (text.empty() || widthDip <= 0) return 0;
    RECT r{0, 0, std::max(1, Px(widthDip)), 0};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_EXPANDTABS | DT_NOPREFIX | DT_CALCRECT);
    int px = r.bottom - r.top;
    if (maxLines > 0) {
        TEXTMETRICW tm{};
        if (GetTextMetricsW(dc, &tm) && tm.tmHeight > 0) px = std::min(px, static_cast<int>(tm.tmHeight) * maxLines);
    }
    // DIPs, rounded up, and a DIP of air (a static draws exactly where DrawText measured).
    const int dpi = static_cast<int>(dpi_);
    return (px * USER_DEFAULT_SCREEN_DPI + dpi - 1) / dpi + 1;
}

int SettingsWindow::MeasureTextDip(const std::wstring& text, int widthDip, int maxLines) const {
    HDC dc = GetDC(nullptr);
    if (!dc) return 0;
    HGDIOBJ old = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    const int h = MeasureDip(dc, text, widthDip, maxLines);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return h;
}

void SettingsWindow::Reflow(SettingsPage& page, HDC dc) {
    std::vector<Ctl>& t = page.table_;
    page.hairlines_.clear();
    for (SettingsPage::Card& card : page.cards_) card.dip = RECT{};
    if (!page.reflow_) {
        for (Ctl& c : t) c.dip = c.base;
        return;
    }
    constexpr int kM = SettingsPage::kMargin;
    const int W = page.Width();
    const int bw = page.buildW_ > 0 ? page.buildW_ : W;
    const int dx = W - bw;
    const int buildRight = bw - kM;
    auto effBottom = [](const Ctl& c, const RECT& r) -> int { return static_cast<int>(c.kind == CtlKind::Combo ? r.top + SettingsPage::kRowH - 4 : r.bottom); };
    auto gone = [&](const Ctl& c) { return c.collapsed || (c.expander != 0 && !page.ExpanderOpen(c.expander)); };

    int rows = 0;
    for (const Ctl& c : t) rows = std::max(rows, c.row + 1);
    std::vector<std::vector<size_t>> byRow(static_cast<size_t>(rows));
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i].row >= 0) {
            byRow[static_cast<size_t>(t[i].row)].push_back(i);
        } else {
            // A fixed control (round 8 phase 8B: the Position tab's silhouette): it never moves
            // vertically; at the right margin it follows the page's width.
            t[i].dip = t[i].base;
            if (t[i].base.right == buildRight) OffsetRect(&t[i].dip, dx, 0);
        }
    }

    // Horizontal: controls at the right margin follow the page's width. A narrow one in the
    // right half moves (never over its row's other controls); a wide one stretches, and so does
    // a control that ends just before a moving one.
    for (const std::vector<size_t>& row : byRow) {
        std::vector<char> move(row.size(), 0), stretch(row.size(), 0), follow(row.size(), 0);
        for (size_t k = 0; k < row.size(); ++k) {
            const Ctl& c = t[row[k]];
            if (c.base.right != buildRight) continue;
            if (c.base.left > bw / 2 && c.kind != CtlKind::Section && c.kind != CtlKind::Title) {
                move[k] = 1;
            } else {
                stretch[k] = 1;
            }
        }
        // Round 8 phase 8D: a check box / radio whose caption moves goes with it (Docks and
        // displays' "Lock all docks" at the right of its toolbar).
        for (size_t k = 0; k < row.size(); ++k) {
            const Ctl& c = t[row[k]];
            if ((c.kind != CtlKind::Check && c.kind != CtlKind::Radio) || move[k] || stretch[k]) continue;
            for (size_t m = 0; m < row.size(); ++m) {
                if (move[m] && t[row[m]].id == c.buddy) follow[k] = 1;
            }
        }
        for (size_t k = 0; k < row.size(); ++k) {
            if (move[k] || stretch[k] || follow[k]) continue;
            const Ctl& c = t[row[k]];
            for (size_t m = 0; m < row.size(); ++m) {
                if ((move[m] || follow[m]) && c.base.right <= t[row[m]].base.left && c.base.right >= t[row[m]].base.left - 16) stretch[k] = 1;
            }
        }
        int fixedRight = INT_MIN;
        for (size_t k = 0; k < row.size(); ++k) {
            Ctl& c = t[row[k]];
            c.dip = c.base;
            if (stretch[k]) c.dip.right = std::max(static_cast<int>(c.dip.left) + 8, static_cast<int>(c.base.right) + dx);
            if (!move[k] && !stretch[k] && !follow[k] && c.kind != CtlKind::Caption) fixedRight = std::max(fixedRight, static_cast<int>(c.base.right));
        }
        for (size_t k = 0; k < row.size(); ++k) {
            if (!move[k]) continue;
            Ctl& c = t[row[k]];
            const int w = c.base.right - c.base.left;
            int left = c.base.left + dx;
            if (fixedRight != INT_MIN) left = std::max(left, fixedRight + 8 + (c.kind == CtlKind::Caption ? SettingsPage::kCaptionDx : 0));
            c.dip.left = left;
            c.dip.right = left + w;
        }
        for (size_t k = 0; k < row.size(); ++k) {
            if (!follow[k]) continue;
            Ctl& c = t[row[k]];
            for (size_t m = 0; m < row.size(); ++m) {
                const Ctl& cap = t[row[m]];
                if (!move[m] || cap.id != c.buddy) continue;
                const int shiftX = cap.dip.left - cap.base.left;
                OffsetRect(&c.dip, shiftX, 0);
            }
        }
        // Round 9 phase 9E: a slider's value ("1.00×") past the right margin (the Global window's
        // Animation speed at its minimum size showed "1..") pulls in: the slider before it gets
        // shorter (down to 80 DIPs) and the value moves left by as much.
        for (size_t k : row) {
            Ctl& v = t[k];
            if (v.kind != CtlKind::Value || v.dip.right <= W - kM) continue;
            for (size_t m : row) {
                Ctl& tb = t[m];
                if (tb.kind != CtlKind::Trackbar || tb.dip.right > v.dip.left) continue;
                const int take = std::min(static_cast<int>(v.dip.right) - (W - kM), std::max(0, static_cast<int>(tb.dip.right - tb.dip.left) - 80));
                tb.dip.right -= take;
                OffsetRect(&v.dip, -take, 0);
                break;
            }
        }
        // Text never runs past the right margin (a narrow window): it wraps or ends with "…".
        for (size_t k : row) {
            Ctl& c = t[k];
            const bool text = c.kind == CtlKind::Label || c.kind == CtlKind::Note || c.kind == CtlKind::Value ||
                              c.kind == CtlKind::Caption || c.kind == CtlKind::Section;
            if (text && c.dip.right > W - kM) c.dip.right = std::max(static_cast<int>(c.dip.left) + 8, W - kM);
        }
        // Round 9 phase 9D: a card's "?" button (at the right margin) keeps its row's other controls
        // out of its place: a header / stretching control ends 6 DIPs before it, one at the right
        // margin moves left of it.
        for (size_t h : row) {
            if (t[h].kind != CtlKind::Help) continue;
            const int stop = t[h].dip.left - 6;
            for (size_t k = 0; k < row.size(); ++k) {
                Ctl& c = t[row[k]];
                if (row[k] == h || c.dip.right <= stop) continue;
                if (move[k] || c.dip.left >= stop) {
                    OffsetRect(&c.dip, stop - c.dip.right, 0);
                } else {
                    c.dip.right = std::max(static_cast<int>(c.dip.left) + 8, stop);
                }
            }
        }
    }

    // Vertical: each row moves by what the rows above it grew (measured notes) or lost (rows
    // whose controls are all gone take no height).
    int shift = 0;
    std::vector<int> rowTop(static_cast<size_t>(rows), INT_MIN), rowBottom(static_cast<size_t>(rows), INT_MIN);
    for (int r = 0; r < rows; ++r) {
        const std::vector<size_t>& row = byRow[static_cast<size_t>(r)];
        if (row.empty()) continue;
        int top = INT_MAX, bottom = INT_MIN;
        bool allGone = true;
        for (size_t i : row) {
            top = std::min(top, static_cast<int>(t[i].base.top));
            bottom = std::max(bottom, effBottom(t[i], t[i].base));
            if (!gone(t[i])) allGone = false;
        }
        int nextTop = INT_MIN, nextGap = 0;
        for (int n = r + 1; n < rows && nextTop == INT_MIN; ++n) {
            for (size_t i : byRow[static_cast<size_t>(n)]) {
                nextTop = nextTop == INT_MIN ? static_cast<int>(t[i].base.top) : std::min(nextTop, static_cast<int>(t[i].base.top));
                // The gap between two cards (a Section; round 8 phase 8D: a card's heading expander).
                if (t[i].kind == CtlKind::Section || (t[i].kind == CtlKind::Expander && t[i].heading)) nextGap = t[i].lo;
            }
        }
        for (size_t i : row) {
            t[i].dip.top = t[i].base.top + shift;
            t[i].dip.bottom = t[i].base.bottom + shift;
        }
        if (allGone) {
            // Its whole pitch, but not the gap before a card that follows.
            shift -= nextTop != INT_MIN ? std::max(0, nextTop - top - nextGap) : bottom - top;
            continue;
        }
        int newBottom = INT_MIN, newTop = INT_MAX;
        for (size_t i : row) {
            Ctl& c = t[i];
            if (gone(c)) continue;
            if (c.measured && c.maxLines > 0) {
                // At most maxLines lines: the text (Ctl::text, the full one) shortened with "…"
                // at a word when it needs more (its Tip has all of it).
                const int w = c.dip.right - c.dip.left;
                const int full = MeasureDip(dc, c.text, w, 0), cap = MeasureDip(dc, c.text, w, c.maxLines);
                std::wstring shown = c.text;
                if (full > cap) {
                    size_t lo = 0, hi = c.text.size();
                    while (lo < hi) {  // the longest prefix (cut at a space) that fits with "…"
                        const size_t mid = (lo + hi + 1) / 2;
                        if (MeasureDip(dc, c.text.substr(0, mid) + L"\x2026", w, 0) <= cap) {
                            lo = mid;
                        } else {
                            hi = mid - 1;
                        }
                    }
                    size_t cut = c.text.find_last_of(L' ', lo);
                    if (cut == std::wstring::npos || cut == 0) cut = lo;
                    shown = c.text.substr(0, cut);
                    while (!shown.empty() && (shown.back() == L' ' || shown.back() == L',' || shown.back() == L'.')) shown.pop_back();
                    shown += L"\x2026";
                }
                if (c.hwnd && WindowText(c.hwnd) != shown) SetWindowTextW(c.hwnd, shown.c_str());
                c.dip.bottom = c.dip.top + std::min(full, cap);
            } else if (c.measured) {
                const std::wstring text = c.hwnd ? WindowText(c.hwnd) : c.text;
                c.dip.bottom = c.dip.top + MeasureDip(dc, text, c.dip.right - c.dip.left, 0);
            }
            newBottom = std::max(newBottom, effBottom(c, c.dip));
            newTop = std::min(newTop, static_cast<int>(c.dip.top));
        }
        rowTop[static_cast<size_t>(r)] = newTop;
        rowBottom[static_cast<size_t>(r)] = newBottom;
        shift += newBottom - (bottom + shift);
    }

    // The cards: around their visible controls (the header 8 DIPs in, else 10), the margin less
    // kCardPadX on each side.
    const int cardL = kM - SettingsPage::kCardPadX, cardR = W - kM + SettingsPage::kCardPadX;
    for (size_t k = 0; k < page.cards_.size(); ++k) {
        int top = INT_MAX, bottom = INT_MIN;
        bool header = false;
        for (const Ctl& c : t) {
            if (c.card != static_cast<int>(k) || gone(c) || !c.shown) continue;
            if (c.kind == CtlKind::Section && c.id == page.cards_[k].section) header = true;
            if (c.measured && c.dip.bottom <= c.dip.top) continue;  // an empty note
            top = std::min(top, static_cast<int>(c.dip.top));
            bottom = std::max(bottom, effBottom(c, c.dip));
        }
        if (top == INT_MAX) continue;
        page.cards_[k].dip = RECT{cardL, top - (header ? 8 : 10), cardR, bottom + 10};
    }

    // Hairlines between a card's rows: above a row that starts with a label, a check box, the
    // first radio of a group, a button or another control (not above notes, values, captions,
    // links or the radios after the first), never right under the card's header.
    int prevCard = -1, prevBottom = INT_MIN;
    bool prevHeader = false;
    for (int r = 0; r < rows; ++r) {
        if (rowTop[static_cast<size_t>(r)] == INT_MIN) continue;
        const Ctl* first = nullptr;
        for (size_t i : byRow[static_cast<size_t>(r)]) {
            if (!gone(t[i])) {
                first = &t[i];
                break;
            }
        }
        if (!first) continue;
        const int card = first->card;
        const bool header = first->kind == CtlKind::Section || (first->kind == CtlKind::Expander && first->heading);
        bool line = false;
        switch (first->kind) {
            case CtlKind::Label:
            case CtlKind::Check:
            case CtlKind::Button:
            case CtlKind::Combo:
            case CtlKind::Edit:
            case CtlKind::Trackbar:
            case CtlKind::List:
            case CtlKind::Expander:
            case CtlKind::Swatch:
            case CtlKind::Custom:
                line = true;
                break;
            case CtlKind::Radio:
                line = (first->style & WS_GROUP) != 0;
                break;
            default:
                break;
        }
        const int top = rowTop[static_cast<size_t>(r)];
        if (line && card >= 0 && card == prevCard && !prevHeader && prevBottom != INT_MIN && prevBottom <= top) {
            page.hairlines_.push_back(SettingsPage::Hairline{(prevBottom + top) / 2, kM, W - kM});
        }
        if (card != prevCard || !(first->kind == CtlKind::Note || first->kind == CtlKind::Value) || prevBottom == INT_MIN) {
            prevBottom = rowBottom[static_cast<size_t>(r)];
        } else {
            prevBottom = std::max(prevBottom, rowBottom[static_cast<size_t>(r)]);
        }
        if (first->kind != CtlKind::Note && first->kind != CtlKind::Value) prevHeader = header;
        prevCard = card;
    }
    // A fixed control inside a card (the silhouette) cuts the hairlines beside it short.
    for (const Ctl& c : t) {
        if (c.row >= 0 || c.card < 0 || gone(c) || !c.shown) continue;
        for (SettingsPage::Hairline& h : page.hairlines_) {
            if (h.y >= c.dip.top - 6 && h.y <= c.dip.bottom + 6 && h.right > c.dip.left - 12) h.right = std::max(h.left, static_cast<int>(c.dip.left) - 12);
        }
    }
}

void SettingsWindow::LayoutPage(SettingsPage& page) {
    HDC dc = GetDC(page.hwnd_);
    HGDIOBJ oldFont = dc ? SelectObject(dc, font_) : nullptr;
    auto gone = [&](const Ctl& c) { return c.collapsed || (c.expander != 0 && !page.ExpanderOpen(c.expander)); };
    auto content = [&] {
        int bottom = 0;
        for (const Ctl& c : page.table_) {
            if (gone(c)) continue;
            const int b = c.kind == CtlKind::Combo ? c.dip.top + SettingsPage::kRowH - 4 : c.dip.bottom;
            bottom = std::max(bottom, Px(b));
        }
        for (const SettingsPage::Card& card : page.cards_) {
            if (!IsRectEmpty(&card.dip)) bottom = std::max(bottom, Px(card.dip.bottom));
        }
        return bottom + Px(page.cards_.empty() ? SettingsPage::kMargin : 16);
    };
    RECT pr{};
    GetWindowRect(page.hwnd_, &pr);
    const int view = pr.bottom - pr.top;
    // The cards and hairlines before: when they change the page's background is painted again.
    std::vector<RECT> oldCards;
    for (const SettingsPage::Card& card : page.cards_) oldCards.push_back(card.dip);
    std::vector<SettingsPage::Hairline> oldLines = page.hairlines_;
    if (dc) Reflow(page, dc);
    page.contentPx_ = content();
    // A page that scrolls is narrower by its scroll bar (Width()): lay it out again for that (a
    // page with a layout of its own: OnResized, below).
    bool fixedResized = false;
    if (dc && (page.contentPx_ > view) != page.scrollBar_) {
        page.scrollBar_ = page.contentPx_ > view;
        if (page.reflow_) {
            Reflow(page, dc);
            page.contentPx_ = content();
        } else {
            fixedResized = page.layoutW_ != 0;
        }
    }
    page.scrollPx_ = std::clamp(page.scrollPx_, 0, std::max(0, page.contentPx_ - view));
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, page.contentPx_ - 1),
                  static_cast<UINT>(std::max(0, view)), page.scrollPx_, 0};
    SetScrollInfo(page.hwnd_, SB_VERT, &si, TRUE);

    // Gone controls are hidden; back, they show as Show() left them.
    for (Ctl& c : page.table_) {
        if (!c.hwnd) continue;
        const bool g = gone(c);
        if (g && !c.goneApplied) {
            if (GetFocus() == c.hwnd) {
                if (HWND next = GetNextDlgTabItem(page.hwnd_, c.hwnd, FALSE); next && next != c.hwnd) SetFocus(next);
            }
            ShowWindow(c.hwnd, SW_HIDE);
            c.goneApplied = true;
        } else if (!g && c.goneApplied) {
            c.goneApplied = false;
            if (c.shown) ShowWindow(c.hwnd, SW_SHOWNA);
        }
    }

    const int checkPx = CheckGlyphPx(false) + 1;
    const int radioPx = CheckGlyphPx(true) + 1;
    HDWP dwp = BeginDeferWindowPos(static_cast<int>(page.table_.size()));
    for (const Ctl& c : page.table_) {
        if (!c.hwnd) continue;
        RECT r{Px(c.dip.left), Px(c.dip.top) - page.scrollPx_, Px(c.dip.right), Px(c.dip.bottom) - page.scrollPx_};
        if (c.kind == CtlKind::Check) r.right = r.left + checkPx;
        if (c.kind == CtlKind::Radio) r.right = r.left + radioPx;
        if (c.kind == CtlKind::Caption && dc) {
            // Clickable only over its text (plus a little), like a real check box caption.
            SIZE ext{};
            GetTextExtentPoint32W(dc, c.text.c_str(), static_cast<int>(c.text.size()), &ext);
            r.right = std::min(r.right, r.left + ext.cx + Px(6));
        }
        if (c.kind == CtlKind::Expander && !c.heading && dc) {
            // As wide as its longer text ("▸ <text>" / "▾ Less"; a heading: its whole row).
            const std::wstring closed = L"\x25B8 " + c.text, open = L"\x25BE Less";
            SIZE a{}, b{};
            GetTextExtentPoint32W(dc, closed.c_str(), static_cast<int>(closed.size()), &a);
            GetTextExtentPoint32W(dc, open.c_str(), static_cast<int>(open.size()), &b);
            r.right = std::min(r.right, r.left + std::max(a.cx, b.cx) + Px(10));
        }
        if (dwp) {
            dwp = DeferWindowPos(dwp, c.hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPos(c.hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (dwp) EndDeferWindowPos(dwp);
    for (const Ctl& c : page.table_) UnselectComboText(c);
    if (dc) {
        SelectObject(dc, oldFont);
        ReleaseDC(page.hwnd_, dc);
    }
    bool repaint = oldCards.size() != page.cards_.size() || oldLines.size() != page.hairlines_.size();
    for (size_t k = 0; !repaint && k < oldCards.size(); ++k) repaint = !EqualRect(&oldCards[k], &page.cards_[k].dip);
    for (size_t k = 0; !repaint && k < oldLines.size(); ++k) {
        const SettingsPage::Hairline &a = oldLines[k], &b = page.hairlines_[k];
        repaint = a.y != b.y || a.left != b.left || a.right != b.right;
    }
    // The statics inside a card paint its fill: they repaint too.
    if (repaint) RedrawWindow(page.hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    if (fixedResized && page.layoutW_ != page.Width()) {
        page.layoutW_ = page.Width();
        page.OnResized();  // lays the page out again (Relayout)
    }
}

void SettingsWindow::ScrollPage(SettingsPage& page, int px) {
    RECT pr{};
    GetWindowRect(page.hwnd_, &pr);
    px = std::clamp(px, 0, std::max(0, page.contentPx_ - static_cast<int>(pr.bottom - pr.top)));
    if (px == page.scrollPx_) return;
    page.scrollPx_ = px;
    LayoutPage(page);
    RedrawWindow(page.hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void SettingsWindow::EnsureVisible(HWND control) {
    SettingsPage* page = PageOf(control);
    if (!page) return;
    const Ctl* c = page->Find(control);
    if (!c) c = page->Find(GetParent(control));  // a combo box's edit
    if (!c) return;
    RECT pr{};
    GetWindowRect(page->hwnd_, &pr);
    const int view = pr.bottom - pr.top;
    const int top = Px(c->dip.top) - Px(8);
    const int bottom = Px(c->kind == CtlKind::Combo ? c->dip.top + SettingsPage::kRowH : c->dip.bottom) + Px(8);
    if (top < page->scrollPx_) {
        ScrollPage(*page, top);
    } else if (bottom > page->scrollPx_ + view) {
        ScrollPage(*page, bottom - view);
    }
}

SettingsPage* SettingsWindow::ActivePage() const {
    return active_ >= 0 && active_ < static_cast<int>(pages_.size()) ? pages_[static_cast<size_t>(active_)].get() : nullptr;
}

SettingsPage* SettingsWindow::PageOf(HWND control) const {
    for (HWND h = control; h && h != hwnd_; h = GetParent(h)) {
        for (const auto& page : pages_) {
            if (page->hwnd_ == h) return page.get();
        }
    }
    return nullptr;
}

void SettingsWindow::SelectPage(int index) {
    index = std::clamp(index, 0, static_cast<int>(pages_.size()) - 1);
    if (index == active_ || !pages_[static_cast<size_t>(index)]->InNav()) return;  // a retired page
    HWND focus = GetFocus();
    SettingsPage* old = ActivePage();
    const bool focusInOld = old && focus && IsChild(old->hwnd_, focus);
    active_ = index;
    SettingsPage* page = ActivePage();
    ShowWindow(page->hwnd_, SW_SHOWNA);
    if (old) ShowWindow(old->hwnd_, SW_HIDE);
    if (preview_) ShowWindow(preview_->Hwnd(), PreviewOn(index) ? SW_SHOWNA : SW_HIDE);
    if (focusInOld) SetFocus(DefaultFocus());
    // Accessibility: the nav's / tab strip's name is the selection.
    for (HWND selector : {nav_, tabs_}) {
        if (!selector) continue;
        SetWindowTextW(selector, page->Name().c_str());
        InvalidateRect(selector, nullptr, TRUE);
    }
}

void SettingsWindow::OnEnterKey() {
    HWND focus = GetFocus();
    SettingsPage* page = PageOf(focus);
    if (!page || loading_) return;
    const Ctl* c = page->Find(focus);
    if (!c) c = page->Find(GetParent(focus));  // the edit inside an editable combo box
    if (c && (c->kind == CtlKind::Edit || c->kind == CtlKind::Combo)) page->OnCommit(c->id);
}

bool SettingsWindow::PreTranslateMessage(MSG& msg) {
    if (!hwnd_ || !msg.hwnd || (msg.hwnd != hwnd_ && !IsChild(hwnd_, msg.hwnd))) return false;
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB && GetKeyState(VK_CONTROL) < 0 && pages_.size() > 1) {
        // Ctrl+Tab / Ctrl+Shift+Tab: the next / previous page (tab), wrapping around.
        const int step = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
        int next = NavStep(active_, step);
        if (next == active_) next = NavStep(step > 0 ? -1 : static_cast<int>(pages_.size()), step);
        SelectPage(next);
        return true;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == 'F' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0 &&
        GetKeyState(VK_SHIFT) >= 0) {
        OpenSearch(L"");  // round 9 phase 9B: Ctrl+F, the settings search
        return true;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_F1 && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_SHIFT) >= 0) {
        OpenHelp(CurrentHelpTopic());  // round 9 phase 9D: the Help at this page's / tab's topic
        return true;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
        // Enter on an expander toggles it (Space does as a button press); on a "?" it opens the Help.
        if (SettingsPage* page = PageOf(msg.hwnd)) {
            if (const Ctl* c = page->Find(msg.hwnd); c && c->kind == CtlKind::Expander) {
                ToggleExpander(*page, c->id);
                return true;
            }
            if (const Ctl* c = page->Find(msg.hwnd); c && c->kind == CtlKind::Help) {
                OpenHelp(c->topic);
                return true;
            }
        }
    }
    if (!IsDialogMessageW(hwnd_, &msg)) return false;
    if (hwnd_ && msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) EnsureVisible(GetFocus());
    return true;
}

// --- window procedures -----------------------------------------------------------------------

LRESULT CALLBACK SettingsWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(msg, wp, lp);
}

void SettingsWindow::RequestRebuild(const SettingsPage& page) {
    for (size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].get() == &page && hwnd_) PostMessageW(hwnd_, kRebuildMessage, i, 0);
    }
}

// --- widget previews (round 6 phase 6E) ------------------------------------------------------------

void SettingsWindow::ClearPreviews() {
    for (auto& [id, img] : previews_) {
        if (img.bitmap) DeleteObject(img.bitmap);
    }
    previews_.clear();
}

void SettingsWindow::CheckPreviewStamp() {
    const uint64_t stamp = host_.previewStamp ? host_.previewStamp() : 0;
    if (stamp == previewStamp_ && dpi_ == previewDpi_) return;
    ClearPreviews();
    previewStamp_ = stamp;
    previewDpi_ = dpi_;
}

namespace {
std::wstring PreviewKey(const std::wstring& id, SIZE fit, float zoom) {
    return id + L"|" + std::to_wstring(fit.cx) + L"x" + std::to_wstring(fit.cy) + L"z" + std::to_wstring(static_cast<int>(zoom * 100.0f));
}
}  // namespace

const SettingsWindow::PreviewImage* SettingsWindow::PreviewEntry(const std::wstring& id, SIZE fit, float zoom) {
    CheckPreviewStamp();
    const auto it = previews_.find(PreviewKey(id, fit, zoom));
    return it == previews_.end() ? nullptr : &it->second;
}

const SettingsWindow::PreviewImage* SettingsWindow::Preview(const std::wstring& id, SIZE fit, bool render, float zoom) {
    CheckPreviewStamp();
    const std::wstring key = PreviewKey(id, fit, zoom);
    auto it = previews_.find(key);
    if (it == previews_.end()) {
        if (!render || !host_.renderPreview || !hwnd_) return nullptr;
        SettingsWidgetPreview r = host_.renderPreview(id, static_cast<float>(dpi_) / 96.0f, fit, zoom);
        ++previewRenders_;
        PreviewImage img;
        img.bitmap = r.bitmap;
        img.px = r.px;
        img.failed = r.bitmap == nullptr;
        img.ms = r.ms;
        it = previews_.emplace(key, img).first;
    }
    return it->second.failed ? nullptr : &it->second;
}

int SettingsWindow::PreviewFailures() const {
    std::vector<std::wstring> ids;
    for (const auto& [key, img] : previews_) {
        if (!img.failed) continue;
        const std::wstring id = key.substr(0, key.find(L'|'));
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    return static_cast<int>(ids.size());
}

void SettingsWindow::InvalidatePreviews() {
    ClearPreviews();
    if (hwnd_) RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void SettingsWindow::PaintPreview(HDC dc, const RECT& box, const PreviewImage& img, COLORREF under) const {
    const int bw = box.right - box.left, bh = box.bottom - box.top;
    if (!img.bitmap || bw <= 0 || bh <= 0 || img.px.cx <= 0 || img.px.cy <= 0) return;
    // The panel: the theme's background (its colour, at 60 % over what is under it).
    const uint32_t argb = host_.previewPanel ? host_.previewPanel() : 0xFF1E1E1E;
    const COLORREF theme = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    auto mix = [](COLORREF a, COLORREF b, int pa) {
        auto ch = [&](BYTE x, BYTE y) { return static_cast<BYTE>((x * pa + y * (100 - pa)) / 100); };
        return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
    };
    const COLORREF panel = mix(theme, under, 60);
    HBRUSH brush = CreateSolidBrush(panel);
    HPEN pen = CreatePen(PS_SOLID, 1, mix(theme, under, 75));
    HGDIOBJ ob = SelectObject(dc, brush);
    HGDIOBJ op = SelectObject(dc, pen);
    const int radius = Px(6) * 2;
    RoundRect(dc, box.left, box.top, box.right, box.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    DeleteObject(brush);
    // The bitmap fitted inside a small inset, aspect kept, centred; never enlarged past 1:1.
    const int pad = std::max(1, Px(3));
    const int aw = std::max(1, bw - 2 * pad), ah = std::max(1, bh - 2 * pad);
    double k = std::min(static_cast<double>(aw) / img.px.cx, static_cast<double>(ah) / img.px.cy);
    k = std::min(k, 1.0);
    const int w = std::max(1, static_cast<int>(std::lround(img.px.cx * k))), h = std::max(1, static_cast<int>(std::lround(img.px.cy * k)));
    const int x = box.left + (bw - w) / 2, y = box.top + (bh - h) / 2;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, img.bitmap);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    GdiAlphaBlend(dc, x, y, w, h, mem, 0, 0, img.px.cx, img.px.cy, blend);
    SelectObject(mem, old);
    DeleteDC(mem);
}

bool SettingsWindow::QueryPages(int code, LPARAM arg, LRESULT& out) {
    if (!hwnd_) return false;
    for (auto& page : pages_) {
        if (page->hwnd_ && page->Query(code, arg, out)) return true;
    }
    return false;
}

void SettingsWindow::RebuildPage(int index) {
    if (!hwnd_ || index < 0 || index >= static_cast<int>(pages_.size())) return;
    SettingsPage& page = *pages_[static_cast<size_t>(index)];
    if (!page.hwnd_) return;
    const bool focusIn = [&] {
        HWND f = GetFocus();
        return f && IsChild(page.hwnd_, f);
    }();
    if (focusIn) SetFocus(nav_ ? nav_ : tabs_ ? tabs_ : hwnd_);
    if (lastFocus_ && IsChild(page.hwnd_, lastFocus_)) lastFocus_ = nullptr;
    if (page.tip_) DestroyWindow(page.tip_);
    page.tip_ = nullptr;
    for (Ctl& c : page.table_) {
        if (c.hwnd) DestroyWindow(c.hwnd);
    }
    page.ResetTable();
    page.scrollPx_ = 0;
    page.Build();
    CreateControls(page);
    page.Created();
    const bool dark = palette_.dark;
    DarkMode::ThemeScrollBars(page.hwnd_, dark);
    for (const Ctl& c : page.table_) {
        if (!c.hwnd) continue;
        HFONT f = c.kind == CtlKind::Title ? fontTitle_ : c.kind == CtlKind::Section ? fontSemibold_ : font_;
        SendMessageW(c.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
        switch (c.kind) {
            case CtlKind::Button:
            case CtlKind::Check:
            case CtlKind::Radio:
            case CtlKind::Edit:
            case CtlKind::Spin:
            case CtlKind::Combo:
            case CtlKind::Link:
                DarkMode::ThemeControl(c.hwnd, dark);
                break;
            case CtlKind::Trackbar:
                SetWindowTheme(c.hwnd, nullptr, nullptr);
                break;
            case CtlKind::List:
                ThemeList(c.hwnd);
                break;
            default:
                break;
        }
    }
    CreateTips(page);
    LayoutPage(page);
    loading_ = true;
    page.Load();
    loading_ = false;
    page.OnStyleChanged();
    RedrawWindow(page.hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    if (nav_) InvalidateRect(nav_, nullptr, TRUE);  // a page name may have changed
    if (tabs_) InvalidateRect(tabs_, nullptr, TRUE);
    if (focusIn && !nav_ && !tabs_ && index == active_) SetFocus(DefaultFocus());  // an Item window: its first control
}

LRESULT SettingsWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    HWND hwnd = hwnd_;
    if (msg == kRebuildMessage) {
        RebuildPage(static_cast<int>(wp));
        return 0;
    }
    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                if (HWND f = GetFocus(); f && IsChild(hwnd_, f)) lastFocus_ = f;
                if (host_.activated) host_.activated(false);
            } else {
                // Active: App keeps this window's dock shown so changes are visible (auto-hide
                // resumes once another window is activated). Not while minimized.
                if (host_.activated) host_.activated(!HIWORD(wp));
                if (!HIWORD(wp)) {
                    HWND f = lastFocus_ && IsWindow(lastFocus_) && IsWindowVisible(lastFocus_) &&
                                     IsWindowEnabled(lastFocus_)
                                 ? lastFocus_
                                 : DefaultFocus();
                    SetFocus(f);
                }
            }
            return 0;

        case WM_SETFOCUS:
            if (HWND f = lastFocus_ && IsWindow(lastFocus_) && IsWindowVisible(lastFocus_) ? lastFocus_ : DefaultFocus();
                f != hwnd_) {
                SetFocus(f);
            }
            return 0;

        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL) {  // Escape (IsDialogMessage)
                PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                return 0;
            }
            if (LOWORD(wp) == IDOK) {  // Enter outside a push button: commit the focused edit
                OnEnterKey();
                return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        // Round 8 phase 8A: resizable; everything follows the client size, the last size of each
        // kind of window is saved when a resize ends (not on every WM_SIZE of a drag).
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const SIZE m = MinClientDip();
            RECT r{0, 0, Px(m.cx), Px(m.cy)};
            AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, dpi_);
            mmi->ptMinTrackSize = POINT{r.right - r.left, r.bottom - r.top};
            return 0;
        }

        case WM_SIZE:
            if (wp != SIZE_MINIMIZED && LOWORD(lp) > 0 && HIWORD(lp) > 0) {
                const int dpi = static_cast<int>(dpi_);
                clientDip_ = SIZE{MulDiv(LOWORD(lp), USER_DEFAULT_SCREEN_DPI, dpi), MulDiv(HIWORD(lp), USER_DEFAULT_SCREEN_DPI, dpi)};
                if (!pages_.empty()) Layout();
                // A resize by a program (SetWindowPos) or Aero snap ends here; a drag at WM_EXITSIZEMOVE.
                if (!inSizeMove_ && wp == SIZE_RESTORED && IsWindowVisible(hwnd_)) SaveClientSize();
            }
            return 0;

        case WM_ENTERSIZEMOVE:
            inSizeMove_ = true;
            return 0;

        case WM_EXITSIZEMOVE:
            inSizeMove_ = false;
            SaveClientSize();
            return 0;

        case WM_DPICHANGED: {
            const UINT old = dpi_;
            dpi_ = HIWORD(wp);
            for (auto& page : pages_) page->scrollPx_ = MulDiv(page->scrollPx_, static_cast<int>(dpi_), static_cast<int>(old));
            CreateFonts();
            UpdateIcons();
            UpdateItemIcon();
            if (preview_) preview_->SetDpi(dpi_);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyBackdrop();  // the nav width in pixels changed
            Layout();
            StyleChanged();  // image lists at the new DPI
            RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
        }

        case WM_GETDPISCALEDSIZE: {
            // Exact: the client stays kWidthDip x kHeightDip (the caption does not scale linearly).
            *reinterpret_cast<SIZE*>(lp) = WindowSizeFor(static_cast<UINT>(wp));
            return TRUE;
        }

        case WM_SETTINGCHANGE:
            if (wp == SPI_SETHIGHCONTRAST ||
                (lp && CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lp), -1, L"ImmersiveColorSet", -1,
                                            TRUE) == CSTR_EQUAL)) {
                ApplyTheme();  // app mode, accent, transparency or high contrast changed
            } else if (wp == SPI_SETNONCLIENTMETRICS) {
                CreateFonts();
                Layout();
            }
            loading_ = true;
            for (auto& page : pages_) page->OnSettingChange(wp, lp);
            loading_ = false;
            return 0;

        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            ApplyTheme();
            return 0;

        case WM_DWMCOMPOSITIONCHANGED:
            ApplyBackdrop();
            return 0;

        case WM_DISPLAYCHANGE:
            loading_ = true;
            for (auto& page : pages_) page->OnDisplayChange();
            loading_ = false;
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            RECT nav{0, 0, nav_ ? Px(kNavWidthDip) : 0, rc.bottom};
            if (nav_) FillRect(dc, &nav, mica_ ? static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)) : baseBrush_);
            RECT content{nav.right, 0, rc.right, rc.bottom};
            FillRect(dc, &content, layerBrush_);
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_DESTROY:
            if (host_.activated) host_.activated(false);
            return 0;

        case WM_NCDESTROY: {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            hwnd_ = nullptr;
            nav_ = nullptr;
            find_ = nullptr;
            tabs_ = nullptr;
            header_ = nullptr;
            footer_ = nullptr;
            headerTip_ = nullptr;  // owned by the window: already destroyed
            lastFocus_ = nullptr;
            if (itemIcon_) DestroyIcon(itemIcon_);
            itemIcon_ = nullptr;
            ClearPreviews();
            for (auto& page : pages_) {
                page->hwnd_ = nullptr;
                page->tip_ = nullptr;  // owned by the window: already destroyed
                for (Ctl& c : page->table_) c.hwnd = nullptr;
            }
            preview_.reset();  // its window (a child) is already gone, with its D2D objects
            DestroyFonts();
            DestroyBrushes();
            DestroyIcons();
            // The last settings window: the dock's menus as before.
            if (darkPopups_ && --g_darkPopupWindows == 0) DarkMode::TryEnableDarkPopups(false);
            darkPopups_ = false;
            if (host_.destroyed) host_.destroyed();
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --- pages -----------------------------------------------------------------------------------

LRESULT CALLBACK SettingsWindow::PageProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsPage* page = nullptr;
    if (msg == WM_NCCREATE) {
        page = static_cast<SettingsPage*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        page->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(page));
    } else {
        page = reinterpret_cast<SettingsPage*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!page) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        page->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return page->window_.PageProc(*page, msg, wp, lp);
}

LRESULT SettingsWindow::CtlColor(UINT msg, HDC hdc, HWND control, const SettingsPage& page) {
    const Ctl* c = page.Find(control);
    COLORREF text = palette_.text;
    COLORREF back = palette_.layer;
    HBRUSH brush = layerBrush_;
    if (c && c->card >= 0 && static_cast<size_t>(c->card) < page.cards_.size() && !IsRectEmpty(&page.cards_[static_cast<size_t>(c->card)].dip)) {
        back = cardColor_;  // round 8 phase 8A: inside a card
        brush = cardBrush_;
    }
    if (HBRUSH flash = FlashBrush(page, c)) {  // round 9 phase 9B: the row the search flashes
        back = flashColor_;
        brush = flash;
    }
    const bool enabled = IsWindowEnabled(control) != FALSE;
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX || (c && c->kind == CtlKind::Edit) ||
        (!c && msg == WM_CTLCOLORSTATIC)) {
        // Edits (read-only / disabled ones send CTLCOLORSTATIC), combo boxes' edits and lists.
        back = palette_.control;
        brush = controlBrush_;
    } else if (c && c->kind == CtlKind::Note) {
        text = palette_.textSecondary;
    } else if (c && c->kind == CtlKind::Link) {
        text = palette_.link;
    }
    if (!enabled) text = palette_.textDisabled;
    SetTextColor(hdc, text);
    SetBkColor(hdc, back);
    return reinterpret_cast<LRESULT>(brush);
}

void SettingsWindow::DrawCaption(const SettingsPage& page, const DRAWITEMSTRUCT& di) {
    const Ctl* c = page.Find(di.hwndItem);
    HWND button = c ? page.Item(c->buddy) : nullptr;
    const bool enabled = !button || IsWindowEnabled(button);
    const bool card = c && c->card >= 0 && static_cast<size_t>(c->card) < page.cards_.size() &&
                      !IsRectEmpty(&page.cards_[static_cast<size_t>(c->card)].dip);
    HBRUSH flash = FlashBrush(page, c);
    FillRect(di.hDC, &di.rcItem, flash ? flash : card ? cardBrush_ : layerBrush_);
    SetBkMode(di.hDC, TRANSPARENT);
    SetTextColor(di.hDC, enabled ? palette_.text : palette_.textDisabled);
    HGDIOBJ old = SelectObject(di.hDC, font_);
    const std::wstring text = WindowText(di.hwndItem);
    RECT r = di.rcItem;
    DrawTextW(di.hDC, text.c_str(), static_cast<int>(text.size()), &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (button && GetFocus() == button && !FocusCuesHidden(button)) {
        RECT fr = di.rcItem;
        DrawTextW(di.hDC, text.c_str(), static_cast<int>(text.size()), &fr,
                  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        const int h = fr.bottom - fr.top;
        fr.top = (di.rcItem.top + di.rcItem.bottom - h) / 2 - 1;
        fr.bottom = fr.top + h + 2;
        fr.left -= 2;
        fr.right = std::min<LONG>(fr.right + 2, di.rcItem.right);
        SetTextColor(di.hDC, palette_.text);
        SetBkColor(di.hDC, card ? cardColor_ : palette_.layer);
        DrawFocusRect(di.hDC, &fr);
    }
    SelectObject(di.hDC, old);
}

void SettingsWindow::DrawExpander(const SettingsPage& page, const DRAWITEMSTRUCT& di) {
    const Ctl* c = page.Find(di.hwndItem);
    if (!c) return;
    const bool card = c->card >= 0 && static_cast<size_t>(c->card) < page.cards_.size() &&
                      !IsRectEmpty(&page.cards_[static_cast<size_t>(c->card)].dip);
    FillRect(di.hDC, &di.rcItem, card ? cardBrush_ : layerBrush_);
    const bool open = page.ExpanderOpen(c->id);
    // Round 8 phase 8D: a heading keeps its text and reads like a section header.
    const std::wstring text = c->heading ? (open ? L"\x25BE  " : L"\x25B8  ") + c->text
                              : open     ? std::wstring(L"\x25BE Less")
                                         : L"\x25B8 " + c->text;
    SetBkMode(di.hDC, TRANSPARENT);
    SetTextColor(di.hDC, (di.itemState & ODS_DISABLED) ? palette_.textDisabled : c->heading ? palette_.text : palette_.link);
    HGDIOBJ old = SelectObject(di.hDC, c->heading ? fontSemibold_ : font_);
    RECT r = di.rcItem;
    r.left += Px(2);
    DrawTextW(di.hDC, text.c_str(), static_cast<int>(text.size()), &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if ((di.itemState & ODS_FOCUS) && !(di.itemState & ODS_NOFOCUSRECT)) {
        RECT fr = r;
        DrawTextW(di.hDC, text.c_str(), static_cast<int>(text.size()), &fr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        const int h = fr.bottom - fr.top;
        fr.top = (di.rcItem.top + di.rcItem.bottom - h) / 2 - 1;
        fr.bottom = fr.top + h + 2;
        fr.left -= 2;
        fr.right = std::min<LONG>(fr.right + 2, di.rcItem.right);
        SetTextColor(di.hDC, palette_.text);
        SetBkColor(di.hDC, card ? cardColor_ : palette_.layer);
        DrawFocusRect(di.hDC, &fr);
    }
    SelectObject(di.hDC, old);
}

void SettingsWindow::DrawHelp(const SettingsPage& page, const DRAWITEMSTRUCT& di) {
    const Ctl* c = page.Find(di.hwndItem);
    const bool card = c && c->card >= 0 && static_cast<size_t>(c->card) < page.cards_.size() &&
                      !IsRectEmpty(&page.cards_[static_cast<size_t>(c->card)].dip);
    const COLORREF back = card ? cardColor_ : palette_.layer;
    const bool hot = di.hwndItem == helpHot_ || (di.itemState & ODS_SELECTED) != 0;
    const COLORREF ink = hot ? palette_.accent : palette_.highContrast ? palette_.text : palette_.textSecondary;
    const RECT r = di.rcItem;
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    // The ring, anti-aliased (a signed distance per pixel), blended over the card's colour.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(di.hDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        const float cx = w / 2.0f, cy = h / 2.0f;
        const float radius = std::min(w, h) / 2.0f - 0.5f;
        const float thick = std::max(1.0f, static_cast<float>(Px(1)) * 1.25f);
        auto mix = [](COLORREF a, COLORREF b, float pa) {
            return RGB(static_cast<BYTE>(GetRValue(a) * pa + GetRValue(b) * (1 - pa) + 0.5f),
                       static_cast<BYTE>(GetGValue(a) * pa + GetGValue(b) * (1 - pa) + 0.5f),
                       static_cast<BYTE>(GetBValue(a) * pa + GetBValue(b) * (1 - pa) + 0.5f));
        };
        // Pressed or hot: a faint fill in the accent inside the ring.
        const COLORREF fill = hot && !palette_.highContrast ? mix(palette_.accent, back, 0.14f) : back;
        auto* px = static_cast<RGBQUAD*>(bits);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float fx = static_cast<float>(x) + 0.5f - cx, fy = static_cast<float>(y) + 0.5f - cy;
                const float d = std::sqrt(fx * fx + fy * fy);
                const float inside = std::clamp(radius - d + 0.5f, 0.0f, 1.0f);  // the disc
                const float ring = std::clamp(std::min(radius - d, d - (radius - thick)) + 0.5f, 0.0f, 1.0f);
                COLORREF col = mix(fill, back, inside);
                col = mix(ink, col, ring);
                px[y * w + x] = RGBQUAD{GetBValue(col), GetGValue(col), GetRValue(col), 0};
            }
        }
        HDC mem = CreateCompatibleDC(di.hDC);
        HGDIOBJ old = SelectObject(mem, dib);
        BitBlt(di.hDC, r.left, r.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    } else {
        HBRUSH b = CreateSolidBrush(back);
        FillRect(di.hDC, &r, b);
        DeleteObject(b);
    }
    if (dib) DeleteObject(dib);
    SetBkMode(di.hDC, TRANSPARENT);
    SetTextColor(di.hDC, ink);
    HGDIOBJ oldFont = SelectObject(di.hDC, fontSemibold_ ? fontSemibold_ : font_);
    RECT tr = r;
    DrawTextW(di.hDC, L"?", 1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(di.hDC, oldFont);
    if ((di.itemState & ODS_FOCUS) && !(di.itemState & ODS_NOFOCUSRECT)) {
        SetTextColor(di.hDC, palette_.text);
        SetBkColor(di.hDC, back);
        DrawFocusRect(di.hDC, &r);
    }
}

LRESULT CALLBACK SettingsWindow::HelpSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<SettingsWindow*>(data);
    switch (msg) {
        case WM_MOUSEMOVE:
            if (self->helpHot_ != hwnd) {
                HWND old = self->helpHot_;
                self->helpHot_ = hwnd;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                if (old && IsWindow(old)) InvalidateRect(old, nullptr, FALSE);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (self->helpHot_ == hwnd) {
                self->helpHot_ = nullptr;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_ERASEBKGND:
            return 1;  // the owner draw paints every pixel
        case WM_NCDESTROY:
            if (self->helpHot_ == hwnd) self->helpHot_ = nullptr;
            RemoveWindowSubclass(hwnd, HelpSubclassProc, id);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void SettingsWindow::OpenHelp(const std::wstring& topic) {
    if (hwnd_ && host_.openHelp) host_.openHelp(topic.empty() ? std::wstring(L"guide-settings.md") : topic);
}

std::wstring SettingsWindow::CurrentHelpTopic() const {
    if (const SettingsPage* p = ActivePage()) {
        if (const std::wstring& t = p->Topic(); !t.empty()) return t;
    }
    return L"guide-settings.md";
}

int SettingsWindow::CardsWithoutHelp(std::vector<int>* ids) const {
    int count = 0;
    for (size_t p = 0; p < pages_.size(); ++p) {
        const SettingsPage& page = *pages_[p];
        if (!page.hwnd_ || !page.InNav() || !page.reflow_) continue;
        for (size_t k = 0; k < page.cards_.size(); ++k) {
            const int card = static_cast<int>(k);
            const bool used = std::any_of(page.table_.begin(), page.table_.end(), [&](const Ctl& c) { return c.card == card && c.kind != CtlKind::Help; });
            const bool help = std::any_of(page.table_.begin(), page.table_.end(), [&](const Ctl& c) { return c.card == card && c.kind == CtlKind::Help; });
            if (!used || help) continue;
            ++count;
            if (ids) ids->push_back(static_cast<int>(p) * 100 + card);  // page index * 100 + the card's index
        }
    }
    return count;
}

int SettingsWindow::UnexplainedInputs(std::vector<std::wstring>* gaps, std::vector<int>* ids) const {
    int count = 0;
    for (const auto& pagePtr : pages_) {
        const SettingsPage& page = *pagePtr;
        if (!page.hwnd_ || !page.InNav()) continue;
        const std::vector<Ctl>& t = page.table_;
        auto input = [](CtlKind k) {
            return k == CtlKind::Check || k == CtlKind::Radio || k == CtlKind::Combo || k == CtlKind::Edit || k == CtlKind::Spin ||
                   k == CtlKind::Trackbar || k == CtlKind::List || k == CtlKind::Custom;
        };
        auto text = [](const Ctl& c) { return c.hwnd && !c.measured ? WindowText(c.hwnd) : c.text; };
        // A tooltip on the control, its caption / buddy, or anything else of its row; or a hint in its row.
        auto explainedOne = [&](const Ctl& c) {
            if (!c.tip.empty()) return true;
            if (c.buddy) {
                if (const Ctl* b = page.Find(c.buddy); b && !b->tip.empty()) return true;
            }
            if (c.row < 0) return false;
            for (const Ctl& o : t) {
                if (o.row != c.row || o.kind == CtlKind::Help) continue;
                if (!o.tip.empty()) return true;
                if (o.hint && !text(o).empty()) return true;
            }
            return false;
        };
        auto labelled = [&](const Ctl& c) {
            if (c.kind == CtlKind::Check || c.kind == CtlKind::Radio) {
                const Ctl* cap = page.Find(c.buddy);
                return (cap && !cap->text.empty()) || !c.text.empty();
            }
            if (c.kind == CtlKind::Spin) return true;  // its edit's row says it
            if (c.row >= 0) {
                for (const Ctl& o : t) {
                    if (o.row == c.row && (o.kind == CtlKind::Label || o.kind == CtlKind::Caption) && !o.text.empty()) return true;
                }
            }
            // A list / custom control names itself (its accessible name under its card's heading).
            if (c.kind == CtlKind::List || c.kind == CtlKind::Custom) return !c.text.empty();
            // An edit with a cue banner ("Search widgets") says what it is.
            if (c.kind == CtlKind::Edit && c.hwnd) {
                wchar_t cue[64]{};
                if (SendMessageW(c.hwnd, EM_GETCUEBANNER, reinterpret_cast<WPARAM>(cue), static_cast<LPARAM>(std::size(cue))) && cue[0]) return true;
            }
            // A control the page lays out itself (no rows): a label on its line, left of it.
            if (c.row < 0) {
                for (const Ctl& o : t) {
                    if ((o.kind == CtlKind::Label || o.kind == CtlKind::Caption) && !o.text.empty() && o.dip.top < c.dip.bottom &&
                        o.dip.bottom > c.dip.top && o.dip.right <= c.dip.left + 4) {
                        return true;
                    }
                }
                return !c.text.empty();
            }
            return false;
        };
        for (size_t i = 0; i < t.size(); ++i) {
            const Ctl& c = t[i];
            if (!input(c.kind) || !c.hwnd) continue;
            bool explained = explainedOne(c);
            if (c.kind == CtlKind::Radio) {
                if (!(c.style & WS_GROUP)) continue;  // the group counts once, at its first radio
                for (size_t k = i + 1; !explained && k < t.size() && t[k].kind == CtlKind::Radio && !(t[k].style & WS_GROUP); ++k) {
                    explained = explainedOne(t[k]);
                }
            }
            if (c.kind == CtlKind::Spin) {
                // Explained with its edit (the same row).
                if (const Ctl* e = page.Find(c.buddy)) explained = explained || explainedOne(*e);
            }
            const bool named = labelled(c);
            if (explained && named) continue;
            ++count;
            if (ids) ids->push_back(c.id);
            if (gaps) {
                gaps->push_back(page.Name() + L": " + std::to_wstring(c.id) + (explained ? L"" : L" no tip") + (named ? L"" : L" no label") +
                                L" (" + c.text + L")");
            }
        }
    }
    return count;
}

void SettingsWindow::ToggleExpander(SettingsPage& page, int id) {
    page.SetExpander(id, !page.ExpanderOpen(id));
    page.OnExpanderToggled(id);  // round 10 phase 10D: the Contents tab places its rows again
    if (HWND h = page.Item(id)) {
        // The accessible name says what a press does.
        const Ctl* c = page.Find(id);
        SetWindowTextW(h, page.ExpanderOpen(id) && !c->heading ? L"Less" : c->text.c_str());
        InvalidateRect(h, nullptr, TRUE);
        if (page.ExpanderOpen(id)) EnsureVisible(h);
    }
}

int SettingsWindow::ExpanderState(int id) const {
    for (const auto& page : pages_) {
        if (const Ctl* c = page->Find(id); c && c->kind == CtlKind::Expander) return page->ExpanderOpen(id) ? 1 : 0;
    }
    return -1;
}

const SettingsPage* SettingsWindow::PageOfConst(HWND control) const { return PageOf(control); }

COLORREF SettingsWindow::SurfaceColor(HWND control) const {
    const SettingsPage* page = PageOfConst(control);
    const Ctl* c = page ? page->Find(control) : nullptr;
    if (c && c->card >= 0 && static_cast<size_t>(c->card) < page->cards_.size() &&
        !IsRectEmpty(&page->cards_[static_cast<size_t>(c->card)].dip)) {
        return cardColor_;
    }
    return palette_.layer;
}

LRESULT SettingsWindow::PageProc(SettingsPage& page, UINT msg, WPARAM wp, LPARAM lp) {
    HWND hwnd = page.hwnd_;
    switch (msg) {
        case WM_MOUSEMOVE:
            // Round 9 phase 9A: a disabled control gets no mouse input (it comes to the page), so its
            // tooltip (TTF_SUBCLASS) would never show, and a greyed control's tooltip says why it is
            // greyed: the page's rect tool (CreateTips) takes its place and text, and the page's
            // moves are relayed to the tooltip (elsewhere the rect is empty: nothing shows).
            if (page.tip_) {
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                HWND child = ChildWindowFromPointEx(hwnd, pt, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
                const Ctl* c = child && child != hwnd && !IsWindowEnabled(child) ? page.Find(child) : nullptr;
                if (c && c->tip.empty()) c = nullptr;
                HWND over = c ? child : nullptr;
                if (over != page.tipProxy_) {
                    // The page's rect tool moves over this disabled control with its text (or away).
                    page.tipProxy_ = over;
                    TTTOOLINFOW ti{};
                    ti.cbSize = sizeof(ti);
                    ti.hwnd = hwnd;
                    ti.uId = SettingsPage::kDisabledTipId;
                    if (over) {
                        GetWindowRect(over, &ti.rect);
                        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&ti.rect), 2);
                    }
                    ti.lpszText = const_cast<wchar_t*>(c ? c->tip.c_str() : L"");
                    SendMessageW(page.tip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
                    SendMessageW(page.tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
                }
                MSG relay{hwnd, msg, wp, lp, static_cast<DWORD>(GetMessageTime()), {}};
                GetCursorPos(&relay.pt);
                SendMessageW(page.tip_, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&relay));
            }
            break;
        case WM_COMMAND: {
            HWND ctl = reinterpret_cast<HWND>(lp);
            const Ctl* c = page.Find(ctl);
            if (!c) {
                // Round 8 phase 8B: a menu command (lParam 0): the page's own menus post their
                // items' ids (the old buttons' ids), and so can automation.
                if (!lp && !loading_) page.OnCommand(LOWORD(wp), BN_CLICKED, nullptr);
                if (!lp) return 0;
                break;
            }
            const int code = HIWORD(wp);
            if (c->kind == CtlKind::Caption) {
                if (code == STN_CLICKED || code == STN_DBLCLK) {
                    HWND button = page.Item(c->buddy);
                    if (button && IsWindowEnabled(button)) SendMessageW(button, BM_CLICK, 0, 0);
                }
                return 0;
            }
            if ((c->kind == CtlKind::Check || c->kind == CtlKind::Radio) && (code == BN_SETFOCUS || code == BN_KILLFOCUS)) {
                if (HWND caption = page.Item(c->buddy)) InvalidateRect(caption, nullptr, TRUE);
                return 0;
            }
            if (c->kind == CtlKind::Expander) {
                if (code == BN_CLICKED) ToggleExpander(page, c->id);
                return 0;
            }
            if (c->kind == CtlKind::Help) {  // round 9 phase 9D: a card's "?"
                if (code == BN_CLICKED) OpenHelp(c->topic);
                return 0;
            }
            // Statics with a tooltip (SS_NOTIFY) report clicks: not the pages' business.
            if (c->kind == CtlKind::Label || c->kind == CtlKind::Note || c->kind == CtlKind::Value ||
                c->kind == CtlKind::Section || c->kind == CtlKind::Title) {
                return 0;
            }
            if (loading_) return 0;
            if (c->kind == CtlKind::Edit && code == EN_KILLFOCUS) {
                page.OnCommit(c->id);
                return 0;
            }
            if (c->kind == CtlKind::Combo && code == CBN_KILLFOCUS &&
                (GetWindowLongW(ctl, GWL_STYLE) & 0x3) == CBS_DROPDOWN) {
                page.OnCommit(c->id);
                return 0;
            }
            page.OnCommand(c->id, code, ctl);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->code == NM_CUSTOMDRAW) {
                if (const Ctl* c = page.Find(nm->hwndFrom); c && c->kind == CtlKind::Trackbar) {
                    return DrawTrackbar(*reinterpret_cast<NMCUSTOMDRAW*>(lp));
                }
            }
            if (loading_) return 0;
            return page.OnNotify(*nm);
        }
        case WM_HSCROLL:
        case WM_VSCROLL:
            if (lp) {
                if (const Ctl* c = page.Find(reinterpret_cast<HWND>(lp)); c && !loading_) page.OnScroll(c->id, LOWORD(wp));
                return 0;
            }
            if (msg == WM_VSCROLL) {
                SCROLLINFO si{sizeof(si), SIF_ALL};
                GetScrollInfo(hwnd, SB_VERT, &si);
                int pos = page.scrollPx_;
                switch (LOWORD(wp)) {
                    case SB_LINEUP: pos -= Px(32); break;
                    case SB_LINEDOWN: pos += Px(32); break;
                    case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
                    case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
                    case SB_THUMBTRACK:
                    case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                    case SB_TOP: pos = 0; break;
                    case SB_BOTTOM: pos = page.contentPx_; break;
                    default: break;
                }
                ScrollPage(page, pos);
            }
            return 0;
        case WM_MOUSEWHEEL:
            ScrollPage(page, page.scrollPx_ - MulDiv(GET_WHEEL_DELTA_WPARAM(wp), Px(48), WHEEL_DELTA));
            return 0;
        case WM_TIMER:  // round 8 phase 8C: a page's own timer (the place search's typing pause)
            if (static_cast<UINT_PTR>(wp) == SettingsPage::kFlashTimerId) {
                FlashStep(page);  // round 9 phase 9B: the search's row flash
                return 0;
            }
            page.OnTimer(static_cast<UINT_PTR>(wp));
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
            return CtlColor(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), page);
        case WM_DRAWITEM: {
            const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (di->CtlType == ODT_STATIC) {
                DrawCaption(page, *di);
                return TRUE;
            }
            if (const Ctl* c = page.Find(di->hwndItem); c && c->kind == CtlKind::Expander) {
                DrawExpander(page, *di);
                return TRUE;
            }
            if (const Ctl* c = page.Find(di->hwndItem); c && c->kind == CtlKind::Help) {
                DrawHelp(page, *di);
                return TRUE;
            }
            if (di->CtlType == ODT_BUTTON && page.OnDrawItem(*di)) return TRUE;
            break;
        }
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, layerBrush_);
            page.PaintBackground(reinterpret_cast<HDC>(wp));
            return 1;
        }
        case WM_PRINTCLIENT: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, layerBrush_);
            page.PaintBackground(reinterpret_cast<HDC>(wp));
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --- the settings search (round 9 phase 9B) --------------------------------------------------

namespace {

COLORREF MixColor(COLORREF a, COLORREF b, int pa) {
    auto ch = [&](BYTE x, BYTE y) { return static_cast<BYTE>((x * pa + y * (100 - pa)) / 100); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

bool IsSearchInput(CtlKind k) {
    return k == CtlKind::Check || k == CtlKind::Radio || k == CtlKind::Combo || k == CtlKind::Edit || k == CtlKind::Trackbar;
}

bool IsDropDownList(HWND combo) { return combo && (GetWindowLongW(combo, GWL_STYLE) & 0x3) == CBS_DROPDOWNLIST; }

// A drop-down's shown text: the selected row (a list), else its edit's text.
std::wstring ComboText(HWND combo) {
    if (!combo) return {};
    const LRESULT sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return IsDropDownList(combo) ? std::wstring() : WindowText(combo);
    const LRESULT n = SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(sel), 0);
    if (n == CB_ERR || n <= 0) return {};
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    const LRESULT got = SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(sel), reinterpret_cast<LPARAM>(s.data()));
    s.resize(got == CB_ERR ? 0 : static_cast<size_t>(got));
    return s;
}

void Append(std::wstring& to, const std::wstring& s) {
    if (s.empty()) return;
    if (!to.empty()) to += L' ';
    to += s;
}

}  // namespace

std::wstring SettingsWindow::SearchName() {
    switch (kind_) {
        case SettingsKind::Global: return L"App";  // round 10 phase 10A: the App settings window
        case SettingsKind::Item: return ItemName() + L" on " + dockId_;
        case SettingsKind::Dock:
        default:
            if (IsFolderWindow()) return L"Folder window " + dockId_;
            if (IsWidgetWindow()) return L"Widget " + dockId_;
            return L"Dock " + dockId_;
    }
}

std::wstring SettingsWindow::PageName(int index) const {
    return index >= 0 && index < static_cast<int>(pages_.size()) ? pages_[static_cast<size_t>(index)]->Name() : std::wstring();
}

std::vector<SettingsWindow::SearchEntry> SettingsWindow::SearchEntries() {
    std::vector<SearchEntry> out;
    if (!hwnd_) return out;
    for (size_t p = 0; p < pages_.size(); ++p) {
        const SettingsPage& page = *pages_[p];
        if (!page.hwnd_ || !page.InNav() || !page.reflow_) continue;  // the Contents tab lays itself out: not a list of settings
        const std::vector<Ctl>& t = page.table_;
        auto off = [](const Ctl& c) { return !c.hwnd || c.collapsed || !c.shown; };  // does not apply now
        auto cardTitle = [&](int card) -> std::wstring {
            if (card < 0 || static_cast<size_t>(card) >= page.cards_.size()) return {};
            if (const int s = page.cards_[static_cast<size_t>(card)].section) {
                if (const Ctl* h = page.Find(s)) return h->text;
            }
            for (const Ctl& h : t) {
                if (h.card == card && h.kind == CtlKind::Expander && h.heading) {
                    const size_t dot = h.text.find(L" \x00B7 ");  // "Bar · 2 changed"
                    return dot == std::wstring::npos ? h.text : h.text.substr(0, dot);
                }
            }
            return {};
        };
        // What a row says besides its controls: its notes (a label's hint, a unit) and tooltips.
        auto rowExtra = [&](int row) {
            std::wstring s;
            if (row < 0) return s;
            for (const Ctl& c : t) {
                if (c.row != row) continue;
                if (c.kind == CtlKind::Note && c.hwnd) Append(s, c.measured && c.maxLines > 0 ? c.text : WindowText(c.hwnd));
                Append(s, c.tip);
            }
            return s;
        };
        std::vector<int> rowsDone;
        for (size_t i = 0; i < t.size(); ++i) {
            const Ctl& c = t[i];
            if (!IsSearchInput(c.kind) || off(c)) continue;
            SearchEntry e;
            e.page = static_cast<int>(p);
            e.id = c.id;
            e.kind = c.kind;
            e.card = cardTitle(c.card);
            e.enabled = IsWindowEnabled(c.hwnd) != FALSE;
            if (c.kind == CtlKind::Check || c.kind == CtlKind::Radio) {
                // Its caption; a radio's value is the chosen caption of its group.
                e.setting = c.text;
                const Ctl* caption = page.Find(c.buddy);
                e.tip = !c.tip.empty() ? c.tip : caption ? caption->tip : std::wstring();
                e.extra = rowExtra(c.row);
                if (c.kind == CtlKind::Check) {
                    e.value = SendMessageW(c.hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"On" : L"Off";
                } else {
                    size_t first = i, last = i;
                    while (first > 0 && t[first].kind == CtlKind::Radio && !(t[first].style & WS_GROUP)) --first;
                    while (last + 1 < t.size() && t[last + 1].kind == CtlKind::Radio && !(t[last + 1].style & WS_GROUP)) ++last;
                    for (size_t k = first; k <= last; ++k) {
                        if (t[k].kind == CtlKind::Radio && t[k].hwnd && SendMessageW(t[k].hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                            e.value = t[k].text;
                        }
                    }
                }
            } else {
                // One entry per row: its label, its first input control (the one Go to focuses).
                if (c.row >= 0) {
                    if (std::find(rowsDone.begin(), rowsDone.end(), c.row) != rowsDone.end()) continue;
                    rowsDone.push_back(c.row);
                }
                const Ctl* label = nullptr;
                const Ctl* valueText = nullptr;
                std::vector<const Ctl*> inputs;
                for (const Ctl& o : t) {
                    if (c.row < 0 ? &o != &c : o.row != c.row) continue;
                    if (o.kind == CtlKind::Label && !label) label = &o;
                    if (o.kind == CtlKind::Value && !off(o) && !valueText) valueText = &o;
                    if ((o.kind == CtlKind::Edit || o.kind == CtlKind::Combo || o.kind == CtlKind::Trackbar) && !off(o)) inputs.push_back(&o);
                }
                e.setting = label ? label->text : e.card;
                if (e.setting.empty()) continue;  // nothing names it
                e.tip = label && !label->tip.empty() ? label->tip : c.tip;
                e.extra = rowExtra(c.row);
                if (valueText && GetWindowTextLengthW(valueText->hwnd) > 0) {
                    e.value = WindowText(valueText->hwnd);  // "1.50x"
                } else {
                    const bool text = std::any_of(inputs.begin(), inputs.end(), [](const Ctl* o) { return o->kind != CtlKind::Trackbar; });
                    for (const Ctl* o : inputs) {
                        std::wstring v;
                        if (o->kind == CtlKind::Edit) {
                            v = WindowText(o->hwnd);
                            // A unit right of it ("DIPs", "%", "minutes").
                            for (const Ctl& u : t) {
                                if (u.row == o->row && u.row >= 0 && u.kind == CtlKind::Note && !off(u) && u.base.left >= o->base.right &&
                                    u.base.left <= o->base.right + 24 && !v.empty()) {
                                    v += L" " + WindowText(u.hwnd);
                                }
                            }
                        } else if (o->kind == CtlKind::Combo) {
                            v = ComboText(o->hwnd);
                        } else if (!text) {
                            v = std::to_wstring(SendMessageW(o->hwnd, TBM_GETPOS, 0, 0));
                        }
                        if (v.empty()) continue;
                        if (!e.value.empty()) e.value += L" \x00B7 ";
                        e.value += v;
                    }
                }
                if (c.kind == CtlKind::Combo && IsDropDownList(c.hwnd)) {
                    const LRESULT n = SendMessageW(c.hwnd, CB_GETCOUNT, 0, 0);
                    for (LRESULT k = 0; k < n && k < 400; ++k) {
                        const LRESULT len = SendMessageW(c.hwnd, CB_GETLBTEXTLEN, static_cast<WPARAM>(k), 0);
                        std::wstring s(len > 0 ? static_cast<size_t>(len) + 1 : 1, L'\0');
                        const LRESULT got = len > 0 ? SendMessageW(c.hwnd, CB_GETLBTEXT, static_cast<WPARAM>(k), reinterpret_cast<LPARAM>(s.data())) : 0;
                        s.resize(got > 0 ? static_cast<size_t>(got) : 0);
                        e.choices.push_back(std::move(s));
                    }
                    e.choice = static_cast<int>(SendMessageW(c.hwnd, CB_GETCURSEL, 0, 0));
                }
            }
            out.push_back(std::move(e));
        }
    }
    return out;
}

bool SettingsWindow::SearchReveal(int pageIndex, int id, RECT* screen) {
    if (!hwnd_ || pageIndex < 0 || pageIndex >= static_cast<int>(pages_.size())) return false;
    SettingsPage& page = *pages_[static_cast<size_t>(pageIndex)];
    const Ctl* c = page.Find(id);
    if (!c || !c->hwnd) return false;
    SelectPage(pageIndex);
    if (c->expander && !page.ExpanderOpen(c->expander)) ToggleExpander(page, c->expander);  // opens and scrolls to it
    c = page.Find(id);
    if (!c || !c->hwnd) return false;
    HWND target = c->hwnd;
    EnsureVisible(target);
    // Show the keyboard focus (a click opened the search: the cues may be hidden).
    SendMessageW(hwnd_, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
    if (IsWindowVisible(target) && IsWindowEnabled(target)) {
        SetFocus(target);
        lastFocus_ = target;
        if (c->kind == CtlKind::Edit) SendMessageW(target, EM_SETSEL, 0, -1);
    }
    FlashRow(pageIndex, id);
    if (screen) {
        GetWindowRect(target, screen);
        if (c->kind == CtlKind::Combo) screen->bottom = screen->top + Px(SettingsPage::kRowH - 8);
    }
    return true;
}

bool SettingsWindow::SearchChange(int pageIndex, int id, int choice) {
    if (!hwnd_ || loading_ || pageIndex < 0 || pageIndex >= static_cast<int>(pages_.size())) return false;
    SettingsPage& page = *pages_[static_cast<size_t>(pageIndex)];
    const Ctl* c = page.Find(id);
    HWND h = c ? c->hwnd : nullptr;
    if (!h || !IsWindowEnabled(h) || !page.hwnd_) return false;
    // The page hears what a click would send (PageProc -> OnCommand): BN_CLICKED / CBN_SELCHANGE.
    switch (c->kind) {
        case CtlKind::Check: {
            const bool on = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SendMessageW(h, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
            SendMessageW(page.hwnd_, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(h));
            return true;
        }
        case CtlKind::Radio: {
            if (SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED) return true;  // chosen already
            const std::vector<Ctl>& t = page.table_;
            size_t at = 0;
            while (at < t.size() && t[at].id != id) ++at;
            size_t first = at, last = at;
            while (first > 0 && t[first].kind == CtlKind::Radio && !(t[first].style & WS_GROUP)) --first;
            while (last + 1 < t.size() && t[last + 1].kind == CtlKind::Radio && !(t[last + 1].style & WS_GROUP)) ++last;
            for (size_t k = first; k <= last && k < t.size(); ++k) {
                if (t[k].kind == CtlKind::Radio && t[k].hwnd) SendMessageW(t[k].hwnd, BM_SETCHECK, k == at ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            SendMessageW(page.hwnd_, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(h));
            return true;
        }
        case CtlKind::Combo: {
            if (!IsDropDownList(h)) return false;
            const LRESULT n = SendMessageW(h, CB_GETCOUNT, 0, 0);
            if (choice < 0 || choice >= n) return false;
            if (SendMessageW(h, CB_GETCURSEL, 0, 0) == choice) return true;
            SendMessageW(h, CB_SETCURSEL, static_cast<WPARAM>(choice), 0);
            SendMessageW(page.hwnd_, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(h));
            return true;
        }
        default:
            return false;
    }
}

void SettingsWindow::FlashRow(int pageIndex, int id) {
    if (!hwnd_ || pageIndex < 0 || pageIndex >= static_cast<int>(pages_.size())) return;
    // One flash at a time in a window.
    for (auto& other : pages_) {
        if (other->flashRow_ < 0 || !other->hwnd_) continue;
        other->flashStart_ = 0;  // ends at its next step
        FlashStep(*other);
    }
    SettingsPage& page = *pages_[static_cast<size_t>(pageIndex)];
    const Ctl* c = page.Find(id);
    if (!c || !page.hwnd_) return;
    RECT band = c->dip;
    if (c->kind == CtlKind::Combo) band.bottom = band.top + SettingsPage::kRowH - 4;
    if (c->row >= 0) {
        for (const Ctl& o : page.table_) {
            if (o.row != c->row || o.collapsed || !o.shown || (o.expander && !page.ExpanderOpen(o.expander))) continue;
            RECT r = o.dip;
            if (o.kind == CtlKind::Combo) r.bottom = r.top + SettingsPage::kRowH - 4;
            UnionRect(&band, &band, &r);
        }
    }
    // Across the card (the page's margins less the card's padding), a little taller than the row.
    const int w = page.Width();
    band.left = SettingsPage::kMargin - SettingsPage::kCardPadX + 3;
    band.right = w - SettingsPage::kMargin + SettingsPage::kCardPadX - 3;
    band.top -= 3;
    band.bottom += 3;
    page.flashRow_ = c->row >= 0 ? c->row : INT_MAX;
    page.flashDip_ = band;
    page.flashStart_ = GetTickCount64();
    SetTimer(page.hwnd_, SettingsPage::kFlashTimerId, 30, nullptr);
    FlashStep(page);
}

void SettingsWindow::FlashStep(SettingsPage& page) {
    if (!page.hwnd_) return;
    RECT px{Px(page.flashDip_.left), Px(page.flashDip_.top) - page.scrollPx_, Px(page.flashDip_.right),
            Px(page.flashDip_.bottom) - page.scrollPx_};
    InflateRect(&px, Px(2), Px(2));
    const ULONGLONG elapsed = page.flashStart_ ? GetTickCount64() - page.flashStart_ : SettingsPage::kFlashMs;
    if (page.flashRow_ < 0 || elapsed >= SettingsPage::kFlashMs) {
        // The end: nothing of it stays, no timer runs.
        KillTimer(page.hwnd_, SettingsPage::kFlashTimerId);
        page.flashRow_ = -1;
        page.flashDip_ = RECT{};
        page.flashStart_ = 0;
        if (flashBrush_) DeleteObject(flashBrush_);
        flashBrush_ = nullptr;
        RedrawWindow(page.hwnd_, &px, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        return;
    }
    // The accent at 30 % over what is under the row (a card or the layer), fading out.
    bool inCard = false;
    for (const Ctl& c : page.table_) {
        if (c.row == page.flashRow_ && c.card >= 0 && static_cast<size_t>(c.card) < page.cards_.size() &&
            !IsRectEmpty(&page.cards_[static_cast<size_t>(c.card)].dip)) {
            inCard = true;
        }
    }
    const double left = 1.0 - static_cast<double>(elapsed) / SettingsPage::kFlashMs;
    const int pct = static_cast<int>(std::lround(30.0 * left));
    const COLORREF color = MixColor(palette_.accent, inCard ? cardColor_ : palette_.layer, pct);
    if (!palette_.highContrast && (color != flashColor_ || !flashBrush_)) {
        if (flashBrush_) DeleteObject(flashBrush_);
        flashBrush_ = CreateSolidBrush(color);
        flashColor_ = color;
    }
    RedrawWindow(page.hwnd_, &px, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

HBRUSH SettingsWindow::FlashBrush(const SettingsPage& page, const Ctl* c) {
    if (!c || page.flashRow_ < 0 || c->row != page.flashRow_ || !flashBrush_ || palette_.highContrast) return nullptr;
    // Statics that paint their background (a trackbar keeps the brush it got first: never this one).
    switch (c->kind) {
        case CtlKind::Label:
        case CtlKind::Note:
        case CtlKind::Value:
        case CtlKind::Caption:
        case CtlKind::Section:
            return flashBrush_;
        default:
            return nullptr;
    }
}

void SettingsWindow::OpenSearch(const std::wstring& text) {
    if (hwnd_ && host_.openSearch) host_.openSearch(text);
}
