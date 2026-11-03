#include "InputDialog.h"

#include <cwctype>
#include <vector>

namespace InputDialog {

namespace {

constexpr int kPromptId = 101;

struct State {
    std::wstring prompt;
    std::wstring* text = nullptr;
    size_t maxLength = 128;
};

// DLGTEMPLATE / DLGITEMTEMPLATE writer (WORD-aligned strings, DWORD-aligned items).
class TemplateWriter {
public:
    void Word(WORD w) { data_.push_back(w); }
    void DWord(DWORD d) {
        Word(LOWORD(d));
        Word(HIWORD(d));
    }
    void String(const wchar_t* s) {
        for (; *s; ++s) Word(static_cast<WORD>(*s));
        Word(0);
    }
    void AlignDWord() {
        if (data_.size() % 2) Word(0);
    }
    void Item(DWORD style, short x, short y, short cx, short cy, WORD id, WORD classAtom, const wchar_t* text) {
        AlignDWord();
        DWord(style | WS_CHILD | WS_VISIBLE);
        DWord(0);  // exStyle
        Word(static_cast<WORD>(x));
        Word(static_cast<WORD>(y));
        Word(static_cast<WORD>(cx));
        Word(static_cast<WORD>(cy));
        Word(id);
        Word(0xFFFF);
        Word(classAtom);  // 0x0080 button, 0x0081 edit, 0x0082 static
        String(text);
        Word(0);  // no creation data
    }
    const DLGTEMPLATE* Get() const { return reinterpret_cast<const DLGTEMPLATE*>(data_.data()); }
    std::vector<WORD>& Data() { return data_; }

private:
    std::vector<WORD> data_;
};

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    State* state = reinterpret_cast<State*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            state = reinterpret_cast<State*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
            SetDlgItemTextW(dlg, kPromptId, state->prompt.c_str());
            SendDlgItemMessageW(dlg, kEditId, EM_LIMITTEXT, static_cast<WPARAM>(state->maxLength), 0);
            SetDlgItemTextW(dlg, kEditId, state->text->c_str());
            SendDlgItemMessageW(dlg, kEditId, EM_SETSEL, 0, -1);
            // Centred on the owner's monitor (the dock is a strip at the screen edge).
            RECT r{};
            GetWindowRect(dlg, &r);
            MONITORINFO mi{sizeof(mi)};
            HWND owner = GetWindow(dlg, GW_OWNER);
            if (GetMonitorInfoW(MonitorFromWindow(owner ? owner : dlg, MONITOR_DEFAULTTOPRIMARY), &mi)) {
                const LONG w = r.right - r.left, h = r.bottom - r.top;
                SetWindowPos(dlg, HWND_TOPMOST, (mi.rcWork.left + mi.rcWork.right - w) / 2,
                             (mi.rcWork.top + mi.rcWork.bottom - h) / 2, 0, 0, SWP_NOSIZE);
            }
            SetFocus(GetDlgItem(dlg, kEditId));
            return FALSE;  // the focus was set
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK && state) {
                const int n = GetWindowTextLengthW(GetDlgItem(dlg, kEditId));
                std::wstring t(static_cast<size_t>(n), L'\0');
                if (n > 0) GetDlgItemTextW(dlg, kEditId, t.data(), n + 1);
                while (!t.empty() && std::iswspace(t.back())) t.pop_back();
                size_t start = 0;
                while (start < t.size() && std::iswspace(t[start])) ++start;
                *state->text = t.substr(start);
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            return FALSE;
        default:
            return FALSE;
    }
}

}  // namespace

bool Ask(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& text, size_t maxLength) {
    TemplateWriter t;
    // DLGTEMPLATE: style, exStyle, item count, x, y, cx, cy (dialog units), menu, class, title, font.
    t.DWord(DS_MODALFRAME | DS_SETFONT | DS_SHELLFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    t.DWord(WS_EX_TOPMOST);
    t.Word(4);
    t.Word(0);
    t.Word(0);
    t.Word(220);
    t.Word(70);
    t.Word(0);  // no menu
    t.Word(0);  // the default class
    t.String(title.c_str());
    t.Word(9);
    t.String(L"Segoe UI");
    t.Item(SS_LEFT, 8, 8, 204, 10, kPromptId, 0x0082, L"");
    t.Item(WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 8, 22, 204, 14, kEditId, 0x0081, L"");
    t.Item(WS_TABSTOP | BS_DEFPUSHBUTTON, 104, 48, 52, 14, IDOK, 0x0080, L"OK");
    t.Item(WS_TABSTOP | BS_PUSHBUTTON, 160, 48, 52, 14, IDCANCEL, 0x0080, L"Cancel");
    std::wstring edited = text;
    State state{prompt, &edited, maxLength};
    const INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), t.Get(), owner, &DialogProc,
                                              reinterpret_cast<LPARAM>(&state));
    if (r != IDOK) return false;
    text = edited;
    return true;
}

}  // namespace InputDialog
