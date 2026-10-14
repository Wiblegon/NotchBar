#pragma once
// A real text field: a native EDIT control hosted in a small owned popup that sits exactly over
// our custom-drawn bar. This gives IME, selection, clipboard, undo, Turkish input and
// accessibility (UI Automation) for free, instead of re-implementing a text editor.
#include <windows.h>

#include <functional>
#include <string>

namespace nb::ui {

class TextInput {
public:
    struct Callbacks {
        std::function<void()> onChange;
        std::function<bool(UINT vk)> onKey;     // return true if handled (Up/Down/Enter/Esc/Tab...)
        std::function<void(HWND newFocus)> onFocusLost;
        std::function<void(bool inside)> onHover;
    };

    ~TextInput();
    bool Create(HWND owner, const wchar_t* cue, Callbacks cb);
    void Place(const RECT& screenRect, float fontPx, COLORREF bg);
    void Show(bool visible, bool focus);
    void SetAlpha(BYTE a);
    std::wstring Text() const;
    void SetText(const std::wstring& t, bool selectAll = false);
    void Focus();
    bool HasFocus() const;
    HWND Host() const { return host_; }
    HWND Edit() const { return edit_; }

private:
    static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    HWND host_ = nullptr, edit_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH brush_ = nullptr;
    COLORREF bg_ = RGB(24, 25, 28);
    float fontPx_ = 0;
    bool hover_ = false;
    Callbacks cb_;
};

} // namespace nb::ui
