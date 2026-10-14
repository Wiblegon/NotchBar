#include "pch.h"

#include "ui/TextInput.h"

namespace nb::ui {
namespace {
const wchar_t* kHostClass = L"NotchBar.TextInput";
}

TextInput::~TextInput() {
    if (host_) DestroyWindow(host_);
    if (font_) DeleteObject(font_);
    if (brush_) DeleteObject(brush_);
}

bool TextInput::Create(HWND owner, const wchar_t* cue, Callbacks cb) {
    cb_ = std::move(cb);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = HostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
        wc.lpszClassName = kHostClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    host_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kHostClass, L"Arama", WS_POPUP, 0, 0, 10, 10, owner,
                            nullptr, GetModuleHandleW(nullptr), this);
    if (!host_) return false;
    SetLayeredWindowAttributes(host_, 0, 255, LWA_ALPHA);
    edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10, host_, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
    SendMessageW(edit_, EM_SETLIMITTEXT, 512, 0);
    SetWindowSubclass(edit_, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    brush_ = CreateSolidBrush(bg_);
    return true;
}

void TextInput::Place(const RECT& r, float fontPx, COLORREF bg) {
    if (bg != bg_) {
        bg_ = bg;
        if (brush_) DeleteObject(brush_);
        brush_ = CreateSolidBrush(bg_);
    }
    if (fontPx != fontPx_) {
        fontPx_ = fontPx;
        if (font_) DeleteObject(font_);
        LOGFONTW lf{};
        lf.lfHeight = -static_cast<LONG>(std::lround(fontPx));
        lf.lfWeight = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Segoe UI Variable Text");
        font_ = CreateFontIndirectW(&lf);
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(host_, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE);
    // Vertically center the single-line edit inside the host.
    HDC dc = GetDC(edit_);
    HGDIOBJ old = SelectObject(dc, font_);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(edit_, dc);
    int eh = tm.tmHeight + 2;
    MoveWindow(edit_, 0, (h - eh) / 2, w, eh, TRUE);
    InvalidateRect(host_, nullptr, TRUE);
}

void TextInput::Show(bool visible, bool focus) {
    if (!visible) {
        ShowWindow(host_, SW_HIDE);
        return;
    }
    ShowWindow(host_, SW_SHOWNOACTIVATE);
    if (focus) Focus();
}

void TextInput::Focus() {
    if (!SetForegroundWindow(host_)) {
        // Foreground lock: borrow the current foreground thread's input state briefly.
        HWND fg = GetForegroundWindow();
        DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        DWORD me = GetCurrentThreadId();
        if (fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE)) {
            BringWindowToTop(host_);
            SetForegroundWindow(host_);
            AttachThreadInput(me, fgThread, FALSE);
        }
    }
    SetFocus(edit_);
}

bool TextInput::HasFocus() const { return GetFocus() == edit_; }

void TextInput::SetAlpha(BYTE a) { SetLayeredWindowAttributes(host_, 0, a, LWA_ALPHA); }

std::wstring TextInput::Text() const {
    int n = GetWindowTextLengthW(edit_);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(edit_, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

void TextInput::SetText(const std::wstring& t, bool selectAll) {
    SetWindowTextW(edit_, t.c_str());
    if (selectAll) SendMessageW(edit_, EM_SETSEL, 0, -1);
    else SendMessageW(edit_, EM_SETSEL, t.size(), t.size());
}

LRESULT CALLBACK TextInput::HostProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<TextInput*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<TextInput*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(h, m, wp, lp);
    switch (m) {
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(236, 236, 238));
        SetBkColor(dc, self->bg_);
        return reinterpret_cast<LRESULT>(self->brush_);
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect(reinterpret_cast<HDC>(wp), &r, self->brush_);
        return 1;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && self->cb_.onChange) self->cb_.onChange();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && self->cb_.onFocusLost) self->cb_.onFocusLost(reinterpret_cast<HWND>(lp));
        else if (LOWORD(wp) != WA_INACTIVE) SetFocus(self->edit_);
        return 0;
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    }
    return DefWindowProcW(h, m, wp, lp);
}

LRESULT CALLBACK TextInput::EditProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<TextInput*>(ref);
    switch (m) {
    case WM_KEYDOWN:
        switch (wp) {
        case VK_UP:
        case VK_DOWN:
        case VK_RETURN:
        case VK_ESCAPE:
        case VK_TAB:
        case VK_PRIOR:
        case VK_NEXT:
            if (self->cb_.onKey && self->cb_.onKey(static_cast<UINT>(wp))) return 0;
            break;
        case 'C':
            // Ctrl+C with no selection: let the owner copy the selected result (e.g. calculation).
            if (GetKeyState(VK_CONTROL) < 0) {
                DWORD s = 0, e = 0;
                SendMessageW(h, EM_GETSEL, reinterpret_cast<WPARAM>(&s), reinterpret_cast<LPARAM>(&e));
                if (s == e && self->cb_.onKey && self->cb_.onKey('C' | 0x10000)) return 0;
            }
            break;
        }
        break;
    case WM_CHAR:
        if (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB) return 0; // no beep
        if (wp == 1 && GetKeyState(VK_CONTROL) < 0) { // Ctrl+A select all
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (!self->hover_) {
            self->hover_ = true;
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            if (self->cb_.onHover) self->cb_.onHover(true);
        }
        break;
    case WM_MOUSELEAVE:
        self->hover_ = false;
        if (self->cb_.onHover) self->cb_.onHover(false);
        break;
    }
    return DefSubclassProc(h, m, wp, lp);
}

} // namespace nb::ui
