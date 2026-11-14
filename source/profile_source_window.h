// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_editor_document.h"
#include "profile_editor_source_rows.h"
#include <algorithm>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0600
#endif
#include <windows.h>
#include <commctrl.h>
#include <strsafe.h>
#include "win32_utf8_paths.h"
#include "theme_palette.h"

struct ProfileSourceWindowCallbacks {
    void* context = nullptr;
    bool (*save)(void*, const ProfileDocument&, char*, size_t) = nullptr;
    bool (*confirmDiscard)(void*) = nullptr;
    void (*changed)(void*) = nullptr;
    void (*closed)(void*) = nullptr;
    void (*created)(HWND) = nullptr;
};
// The native window edits only the canonical source. It has no driver, service
// or live-topology dependency; callbacks own storage and parent presentation.
class ProfileSourceWindow {
public:
    enum { LIST = 9101, INPUT, STAGE, SAVE, HELP, STATUS, TITLE, CLOSE = IDCANCEL };
    ProfileSourceWindow(ProfileEditorDocument& model, ProfileSourceWindowCallbacks callbacks)
        : model_(model), callbacks_(callbacks) {}
    ProfileSourceWindow(const ProfileSourceWindow&) = delete;
    ProfileSourceWindow& operator=(const ProfileSourceWindow&) = delete;
    ~ProfileSourceWindow() { if (window_) DestroyWindow(window_); release(); }
    HWND handle() const { return window_; }
    HWND control(int id) const { return window_ ? GetDlgItem(window_, id) : nullptr; }
    bool pending() const { return window_ && pending_; }
    bool route(MSG& message) const {
        if (!window_ || (message.hwnd != window_ && !IsChild(window_, message.hwnd))) return false;
        if (message.message == WM_KEYDOWN && (message.wParam == VK_ESCAPE ||
            (message.wParam == VK_RETURN && message.hwnd == input_) ||
            (message.wParam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)))) {
            SendMessageW(window_, WM_COMMAND, message.wParam == VK_ESCAPE ? CLOSE : message.wParam == VK_RETURN ? STAGE : SAVE, 0);
            return true;
        }
        return IsDialogMessageW(window_, &message) != FALSE;
    }
    bool create(HWND owner, int dpi, bool visible = true) {
        if (window_) { if (visible) show(); return true; }
        dpi_ = dpi >= 48 && dpi <= 768 ? dpi : 96;
        WNDCLASSEXW type{}; type.cbSize = sizeof(type); type.hInstance = GetModuleHandleW(nullptr);
        type.lpfnWndProc = dispatch; type.lpszClassName = L"GreenCurveProfileSource";
        type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
        RECT work{0, 0, 1280, 720};
        if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) work = monitor.rcWork;
        const int width = std::min(px(760), (int)(work.right - work.left));
        const int height = std::min(px(540), (int)(work.bottom - work.top));
        if (!CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"Profile source", WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VSCROLL,
            work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
            width, height, owner, nullptr, type.hInstance, this)) return false;
        if (callbacks_.created) callbacks_.created(window_);
        if (visible) show();
        return true;
    }
    void show() { if (window_) { ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_); } }
    void reload() {
        ++generation_; pending_ = false; selected_ = -1; rebuild(-1);
        status(L"Source draft; destination review is required before Apply");
    }
    bool save() {
        if (!model_.valid || !commit(false) || !callbacks_.save) return false;
        const auto generation = generation_; auto saved = std::make_unique<ProfileDocument>(model_.value);
        char error[256]{};
        if (!callbacks_.save(callbacks_.context, *saved, error, sizeof(error))) {
            GcWideUtf8Arg text(error[0] ? error : "Source save failed"); status(text.value); return false;
        }
        if (generation == generation_ && model_.valid) {
            model_.saved = *saved; model_.unsaved = false; update_title();
            status(L"Saved exact source intent; GPU settings were not applied");
        }
        return true;
    }
private:
    ProfileEditorDocument& model_;
    ProfileSourceWindowCallbacks callbacks_;
    HWND window_ = nullptr, list_ = nullptr, input_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH background_ = nullptr, inputBrush_ = nullptr;
    int dpi_ = 96, selected_ = -1, scrollX_ = 0, scrollY_ = 0;
    bool pending_ = false, updating_ = false, layingOut_ = false;
    unsigned long long generation_ = 0;
    std::vector<ProfileEditorSourceRow> rows_;
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    void status(const wchar_t* text) { if (window_) SetWindowTextW(control(STATUS), text ? text : L""); }
    void update_title() {
        SetWindowTextW(window_, pending_ || profile_editor_document_dirty(model_) ? L"Profile source *" : L"Profile source");
    }
    void release() {
        if (font_) DeleteObject(font_);
        if (background_) DeleteObject(background_);
        if (inputBrush_) DeleteObject(inputBrush_);
        font_ = nullptr; background_ = inputBrush_ = nullptr;
    }
    void refresh_font() {
        HFONT fresh = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!fresh) return;
        HFONT old = font_; font_ = fresh;
        for (int id : {LIST, INPUT, STAGE, SAVE, HELP, STATUS, TITLE, CLOSE})
            SendMessageW(control(id), WM_SETFONT, (WPARAM)font_, FALSE);
        if (old) DeleteObject(old);
    }
    bool create_controls() {
        background_ = CreateSolidBrush(COL_BG); inputBrush_ = CreateSolidBrush(COL_INPUT);
        const auto child = [&](const wchar_t* name, const wchar_t* text, DWORD style, int id) {
            return CreateWindowExW(0, name, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window_,
                (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        };
        child(L"STATIC", L"Portable source draft", 0, TITLE);
        list_ = child(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_BORDER | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, LIST);
        child(L"STATIC", L"Select a source point or offset to edit", 0, HELP);
        input_ = child(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, INPUT);
        child(L"BUTTON", L"Stage value", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, STAGE);
        child(L"BUTTON", L"Save source", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, SAVE);
        child(L"BUTTON", L"Close source", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, CLOSE);
        child(L"EDIT", L"", ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, STATUS);
        for (int id : {LIST, INPUT, STAGE, SAVE, HELP, STATUS, TITLE, CLOSE}) if (!control(id)) return false;
        SendMessageW(input_, EM_SETLIMITTEXT, 31, 0);
        if (!SetWindowSubclass(input_, input_proc, 1, (DWORD_PTR)this)) return false;
        refresh_font(); reload(); layout();
        return background_ && inputBrush_;
    }
    void rebuild(int selected) {
        if (!list_) return;
        updating_ = true; rows_.clear(); SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        if (model_.valid) rows_ = profile_editor_source_rows(model_.value);
        HDC dc = GetDC(list_); HGDIOBJ old = dc && font_ ? SelectObject(dc, font_) : nullptr; int maximum = 0;
        for (size_t i = 0; i < rows_.size(); ++i) {
            GcWideUtf8Arg text(rows_[i].text.c_str()); if (!text.valid_for(rows_[i].text.c_str())) continue;
            const LRESULT item = SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)text.value);
            if (item != LB_ERR && item != LB_ERRSPACE) SendMessageW(list_, LB_SETITEMDATA, (WPARAM)item, (LPARAM)i);
            SIZE size{}; if (dc && GetTextExtentPoint32W(dc, text.value, lstrlenW(text.value), &size)) maximum = std::max(maximum, (int)size.cx);
        }
        if (dc) { if (old) SelectObject(dc, old); ReleaseDC(list_, dc); }
        SendMessageW(list_, LB_SETHORIZONTALEXTENT, maximum + px(20), 0);
        selected_ = selected; SendMessageW(list_, LB_SETCURSEL, selected, 0); updating_ = false;
        select(false); EnableWindow(control(SAVE), model_.valid); update_title();
    }
    bool commit(bool force) {
        if (!pending_ && !force) return true;
        const LRESULT index = selected_ < 0 ? LB_ERR : SendMessageW(list_, LB_GETITEMDATA, selected_, 0);
        if (index < 0 || (size_t)index >= rows_.size() || !rows_[index].editable) return !pending_;
        wchar_t wide[64]{}; GetWindowTextW(input_, wide, 64); std::string text;
        for (const auto c : wide) { if (!c) break; if (c > 127) { text.clear(); break; } text += (char)c; }
        const auto row = rows_[index];
        if (!profile_editor_document_type(model_, row.field, row.index, text.c_str())) {
            status(L"Invalid value; enter exact MHz with up to six decimal places"); SetFocus(input_); return false;
        }
        pending_ = false; rebuild(selected_);
        status(L"Source intent staged; destination review is required before Apply");
        if (callbacks_.changed) callbacks_.changed(callbacks_.context);
        return true;
    }
    void select(bool commitPrevious) {
        if (updating_) return;
        const int next = (int)SendMessageW(list_, LB_GETCURSEL, 0, 0);
        if (commitPrevious && next != selected_ && !commit(false)) { SendMessageW(list_, LB_SETCURSEL, selected_, 0); return; }
        updating_ = true; SendMessageW(list_, LB_SETCURSEL, next, 0); updating_ = false;
        selected_ = next;
        const LRESULT index = next < 0 ? LB_ERR : SendMessageW(list_, LB_GETITEMDATA, next, 0);
        const bool editable = model_.valid && index >= 0 && (size_t)index < rows_.size() && rows_[index].editable;
        updating_ = true; std::string seed;
        if (editable) seed = profile_editor_source_seed(model_.value, rows_[index].field, rows_[index].index);
        const std::wstring text(seed.begin(), seed.end()); SetWindowTextW(input_, text.c_str());
        updating_ = false; pending_ = false;
        EnableWindow(input_, editable); EnableWindow(control(STAGE), editable);
        SetWindowTextW(control(HELP), editable && rows_[index].field == PROFILE_EDIT_POINT ?
            L"Absolute MHz replaces this point's intent" : editable ? L"Type physical offset MHz" : L"Select a source point or offset to edit");
        if (editable) { SendMessageW(input_, EM_SETSEL, 0, -1); }
    }
    void layout() {
        if (!list_ || layingOut_) return;
        layingOut_ = true; RECT client{};
        for (int pass = 0; pass < 2; ++pass) {
            GetClientRect(window_, &client);
            const int width = std::max((int)client.right, px(360)), height = std::max((int)client.bottom, px(320));
            SCROLLINFO x{sizeof(x), SIF_RANGE | SIF_PAGE | SIF_POS, 0, width - 1, (UINT)client.right, scrollX_, 0};
            SCROLLINFO y{sizeof(y), SIF_RANGE | SIF_PAGE | SIF_POS, 0, height - 1, (UINT)client.bottom, scrollY_, 0};
            SetScrollInfo(window_, SB_HORZ, &x, TRUE); SetScrollInfo(window_, SB_VERT, &y, TRUE);
        }
        SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_POS;
        GetScrollInfo(window_, SB_HORZ, &info); scrollX_ = info.nPos; GetScrollInfo(window_, SB_VERT, &info); scrollY_ = info.nPos;
        GetClientRect(window_, &client); const int w = std::max((int)client.right, px(360)), h = std::max((int)client.bottom, px(320));
        const int m = px(12), row = px(28), gap = px(8);
        const auto place = [&](int id, int x, int y, int width, int height) { MoveWindow(control(id), x - scrollX_, y - scrollY_, width, height, TRUE); };
        place(TITLE, m, m, w - 2*m, px(22)); place(LIST, m, px(42), w - 2*m, h - px(216));
        place(HELP, m, h - px(166), w - 2*m, px(22));
        place(INPUT, m, h - px(138), w - 2*m - px(128) - gap, row); place(STAGE, w - m - px(128), h - px(138), px(128), row);
        place(SAVE, m, h - px(100), px(120), row); place(CLOSE, w - m - px(120), h - px(100), px(120), row);
        place(STATUS, m, h - px(64), w - 2*m, px(56)); layingOut_ = false;
    }
    void scroll(int bar, int command) {
        SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_ALL; GetScrollInfo(window_, bar, &info);
        int position = info.nPos;
        if (command == SB_LINEUP) position -= px(24); else if (command == SB_LINEDOWN) position += px(24);
        else if (command == SB_PAGEUP) position -= (int)info.nPage; else if (command == SB_PAGEDOWN) position += (int)info.nPage;
        else if (command == SB_THUMBTRACK || command == SB_THUMBPOSITION) position = info.nTrackPos;
        else if (command == SB_TOP) position = info.nMin; else if (command == SB_BOTTOM) position = info.nMax;
        info.fMask = SIF_POS; info.nPos = position; SetScrollInfo(window_, bar, &info, TRUE);
        GetScrollInfo(window_, bar, &info); if (bar == SB_HORZ) scrollX_ = info.nPos; else scrollY_ = info.nPos;
        layout();
    }
    void reveal(HWND child) {
        RECT r{}; GetWindowRect(child, &r); MapWindowPoints(nullptr, window_, (POINT*)&r, 2);
        RECT client{}; GetClientRect(window_, &client);
        for (int bar : {SB_HORZ, SB_VERT}) {
            const int delta = bar == SB_HORZ ? (r.left < 0 ? r.left : std::max(0L, r.right - client.right)) :
                (r.top < 0 ? r.top : std::max(0L, r.bottom - client.bottom));
            SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_POS; GetScrollInfo(window_, bar, &info);
            info.nPos += delta; SetScrollInfo(window_, bar, &info, TRUE);
            GetScrollInfo(window_, bar, &info); if (bar == SB_HORZ) scrollX_ = info.nPos; else scrollY_ = info.nPos;
        }
        layout();
    }
    void draw(const DRAWITEMSTRUCT& item) {
        const bool enabled = IsWindowEnabled(item.hwndItem) != FALSE;
        const COLORREF color = !enabled ? COL_BUTTON_DISABLED : (item.itemState & ODS_SELECTED) ? COL_BUTTON_PRESSED : COL_BUTTON;
        HBRUSH brush = CreateSolidBrush(color); FillRect(item.hDC, &item.rcItem, brush); DeleteObject(brush);
        SetBkMode(item.hDC, TRANSPARENT); SetTextColor(item.hDC, enabled ? COL_BUTTON_LABEL : COL_LABEL);
        HGDIOBJ old = font_ ? SelectObject(item.hDC, font_) : nullptr; wchar_t label[64]{}; GetWindowTextW(item.hwndItem, label, 64);
        RECT text = item.rcItem; DrawTextW(item.hDC, label, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (old) SelectObject(item.hDC, old);
        if (item.itemState & ODS_FOCUS) { RECT focus = item.rcItem; InflateRect(&focus, -px(3), -px(3)); DrawFocusRect(item.hDC, &focus); }
    }
    static LRESULT CALLBACK input_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        auto* self = (ProfileSourceWindow*)data;
        if (message == WM_KEYDOWN && (w == VK_RETURN || (w == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)))) {
            SendMessageW(self->window_, WM_COMMAND, w == VK_RETURN ? STAGE : SAVE, 0); return 0;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, input_proc, 1);
        return DefSubclassProc(hwnd, message, w, l);
    }
    static LRESULT CALLBACK dispatch(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        auto* self = (ProfileSourceWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (message == WM_NCCREATE) {
            self = (ProfileSourceWindow*)((CREATESTRUCTW*)l)->lpCreateParams;
            self->window_ = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(hwnd, message, w, l);
        switch (message) {
            case WM_CREATE: return self->create_controls() ? 0 : -1;
            case WM_SIZE: self->layout(); return 0;
            case WM_HSCROLL: self->scroll(SB_HORZ, LOWORD(w)); return 0;
            case WM_VSCROLL: self->scroll(SB_VERT, LOWORD(w)); return 0;
            case WM_DPICHANGED: {
                const int dpi = LOWORD(w); if (dpi >= 48 && dpi <= 768) self->dpi_ = dpi;
                wchar_t text[64]{}; GetWindowTextW(self->input_, text, 64);
                DWORD start = 0, end = 0; SendMessageW(self->input_, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
                const bool pending = self->pending_; const LRESULT top = SendMessageW(self->list_, LB_GETTOPINDEX, 0, 0);
                self->refresh_font(); self->rebuild(self->selected_);
                self->updating_ = true; SetWindowTextW(self->input_, text); self->updating_ = false;
                SendMessageW(self->input_, EM_SETSEL, start, end); SendMessageW(self->list_, LB_SETTOPINDEX, top, 0);
                self->pending_ = pending; self->update_title(); const auto* r = (RECT*)l;
                if (r) SetWindowPos(hwnd, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                self->layout(); return 0;
            }
            case WM_COMMAND:
                if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == BN_SETFOCUS || HIWORD(w) == LBN_SETFOCUS) self->reveal((HWND)l);
                if (LOWORD(w) == LIST && HIWORD(w) == LBN_SELCHANGE) self->select(true);
                else if (LOWORD(w) == INPUT && HIWORD(w) == EN_CHANGE && !self->updating_) { self->pending_ = true; self->update_title(); }
                else if (LOWORD(w) == STAGE && HIWORD(w) == BN_CLICKED) (void)self->commit(true);
                else if (LOWORD(w) == SAVE && HIWORD(w) == BN_CLICKED) (void)self->save();
                else if (LOWORD(w) == CLOSE && HIWORD(w) == BN_CLICKED) SendMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            case WM_CLOSE:
                if ((self->pending_ || profile_editor_document_dirty(self->model_)) &&
                    (!self->callbacks_.confirmDiscard || !self->callbacks_.confirmDiscard(self->callbacks_.context))) return 0;
                if (self->callbacks_.closed) self->callbacks_.closed(self->callbacks_.context);
                DestroyWindow(hwnd); return 0;
            case WM_ERASEBKGND: { RECT r{}; GetClientRect(hwnd, &r); if (self->background_) FillRect((HDC)w, &r, self->background_); return 1; }
            case WM_CTLCOLORSTATIC: case WM_CTLCOLORLISTBOX: case WM_CTLCOLOREDIT:
                SetTextColor((HDC)w, COL_TEXT); SetBkColor((HDC)w, message == WM_CTLCOLORSTATIC ? COL_BG : COL_INPUT);
                return (LRESULT)(message == WM_CTLCOLORSTATIC ? self->background_ : self->inputBrush_);
            case WM_DRAWITEM: self->draw(*(DRAWITEMSTRUCT*)l); return TRUE;
            case WM_NCDESTROY:
                self->window_ = self->list_ = self->input_ = nullptr; self->pending_ = false;
                self->release(); ++self->generation_;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); break;
        }
        return DefWindowProcW(hwnd, message, w, l);
    }
};
