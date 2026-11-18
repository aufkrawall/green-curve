// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "platform.h"
#include "profile_editor_transfer.h"
#include <windows.h>
#include <strsafe.h>
#include "win32_utf8_paths.h"
#include "theme_palette.h"
#include <commctrl.h>
#include <algorithm>

struct ProfileTransferWindowCallbacks {
    void *context = nullptr;
    bool (*choose)(void *, bool, std::string &, char *, size_t) = nullptr;
    void (*prepare)(void *, const ProfileEditorTransferOptions &, const std::string &) = nullptr;
    void (*commit)(void *, bool, bool) = nullptr;
    void (*cancel)(void *) = nullptr;
    void (*created)(HWND) = nullptr;
};
class ProfileTransferWindow {
  public:
    enum {
        MODE = 9201,
        KIND,
        PATH,
        BROWSE,
        SOURCE,
        SLOT,
        FORMAT,
        MEMORY,
        CURVE,
        PREVIEW,
        COMMIT,
        BACK,
        LOAD,
        DISCARD,
        REPORT,
        STATUS,
        TITLE,
        LABEL_FIRST,
        LABEL_LAST = LABEL_FIRST + 7,
        CLOSE = IDCANCEL
    };
    explicit ProfileTransferWindow(ProfileTransferWindowCallbacks callbacks) : callbacks_(callbacks) {}
    ~ProfileTransferWindow() {
        if (window_)
            DestroyWindow(window_);
        release();
    }
    ProfileTransferWindow(const ProfileTransferWindow &) = delete;
    ProfileTransferWindow &operator=(const ProfileTransferWindow &) = delete;
    HWND handle() const { return window_; }
    HWND control(int id) const { return window_ ? GetDlgItem(window_, id) : nullptr; }
    bool busy() const { return busy_; }
    bool committing() const { return busy_ && committing_; }
    bool create(HWND owner, int dpi, int slot, bool dirty, bool visible = true) {
        if (window_) {
            ShowWindow(window_, SW_SHOWNORMAL);
            SetForegroundWindow(window_);
            return true;
        }
        dpi_ = dpi >= 48 && dpi <= 768 ? dpi : 96;
        slot_ = std::clamp(slot, 1, 5);
        needsDiscard_ = dirty;
        busy_ = committing_ = reviewing_ = ready_ = false;
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type);
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpfnWndProc = dispatch;
        type.lpszClassName = L"GreenCurveProfileTransfer";
        type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        RECT work{0, 0, 1280, 720};
        if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor))
            work = monitor.rcWork;
        const int width = std::min(px(740), (int)(work.right - work.left)),
                  height = std::min(px(580), (int)(work.bottom - work.top));
        if (!CreateWindowExW(
                WS_EX_TOOLWINDOW, type.lpszClassName, L"Transfer saved profiles",
                WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VSCROLL, work.left + (work.right - work.left - width) / 2,
                work.top + (work.bottom - work.top - height) / 2, width, height, owner, nullptr, type.hInstance, this))
            return false;
        if (callbacks_.created)
            callbacks_.created(window_);
        if (visible) {
            ShowWindow(window_, SW_SHOWNORMAL);
            SetForegroundWindow(window_);
        }
        SetFocus(control(CLOSE));
        return true;
    }
    bool route(MSG &message) const {
        if (!window_ || (message.hwnd != window_ && !IsChild(window_, message.hwnd)))
            return false;
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            SendMessageW(window_, WM_CLOSE, 0, 0);
            return true;
        }
        // Owner-drawn buttons do not advertise a default push-button to the
        // dialog manager. Enter must activate the focused action explicitly.
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
            HWND focus = GetFocus();
            const int id = focus ? GetDlgCtrlID(focus) : 0;
            if (focus && IsChild(window_, focus) && IsWindowEnabled(focus) &&
                (id == BROWSE || id == PREVIEW || id == COMMIT || id == BACK || id == CLOSE)) {
                SendMessageW(window_, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)focus);
                return true;
            }
        }
        return IsDialogMessageW(window_, &message) != FALSE;
    }
    void notice(const char *message) {
        GcWideUtf8Arg wide(message);
        SetWindowTextW(control(STATUS), wide.value ? wide.value : L"Transfer unavailable");
    }
    void set_busy(bool committing) {
        busy_ = true;
        committing_ = committing;
        ready_ = false;
        project();
        notice(committing ? "Writing the reviewed transfer; wait for the result"
                          : "Reading saved profile data for preview");
    }
    void report(const std::vector<std::string> &lines, bool ready, bool dirty) {
        busy_ = committing_ = false;
        reviewing_ = true;
        ready_ = ready;
        needsDiscard_ = dirty;
        SendMessageW(control(DISCARD), BM_SETCHECK, BST_UNCHECKED, 0);
        std::string text;
        for (const auto &line : lines) {
            text += line;
            text += "\r\n";
        }
        GcWideUtf8Arg wide(text.c_str());
        SetWindowTextW(control(REPORT), wide.value ? wide.value : L"Could not display transfer preview");
        project();
        layout();
        SetFocus(control(CLOSE));
        notice(ready ? "Review the file, replacements and assignments before confirming"
                     : "Transfer unavailable; go Back to change choices");
    }
    void require_discard() {
        busy_ = committing_ = false;
        needsDiscard_ = true;
        SendMessageW(control(DISCARD), BM_SETCHECK, BST_UNCHECKED, 0);
        project();
        notice("The draft changed; review its replacement before confirming");
        SetFocus(control(CLOSE));
    }
    void queue_failed() {
        busy_ = committing_ = false;
        project();
    }

  private:
    ProfileTransferWindowCallbacks callbacks_;
    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH background_ = nullptr, inputBrush_ = nullptr;
    int dpi_ = 96, slot_ = 1, scrollX_ = 0, scrollY_ = 0;
    bool busy_ = false, committing_ = false, reviewing_ = false, ready_ = false, needsDiscard_ = false,
         layingOut_ = false;
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    int selected(int id) const { return (int)SendMessageW(control(id), CB_GETCURSEL, 0, 0); }
    bool checked(int id) const { return SendMessageW(control(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    bool importing() const { return selected(MODE) == 1; }
    void release() {
        if (font_)
            DeleteObject(font_);
        if (background_)
            DeleteObject(background_);
        if (inputBrush_)
            DeleteObject(inputBrush_);
        font_ = nullptr;
        background_ = inputBrush_ = nullptr;
    }
    void refresh_font() {
        HFONT font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font)
            return;
        for (int id = MODE; id <= LABEL_LAST; ++id)
            SendMessageW(control(id), WM_SETFONT, (WPARAM)font, FALSE);
        SendMessageW(control(CLOSE), WM_SETFONT, (WPARAM)font, FALSE);
        if (font_)
            DeleteObject(font_);
        font_ = font;
    }
    bool create_controls() {
        background_ = CreateSolidBrush(COL_BG);
        inputBrush_ = CreateSolidBrush(COL_INPUT);
        const auto child = [&](const wchar_t *name, const wchar_t *text, DWORD style, int id) {
            return CreateWindowExW(0, name, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window_,
                                   (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        };
        child(L"STATIC", L"Transfer saved profiles without a GPU", 0, TITLE);
        const wchar_t *labels[] = {L"Direction",
                                   L"Records",
                                   L"File",
                                   L"Source record",
                                   L"Saved / destination slot",
                                   L"Legacy format",
                                   L"Legacy memory units",
                                   L"Legacy curve meaning"};
        for (int i = 0; i < 8; ++i)
            child(L"STATIC", labels[i], 0, LABEL_FIRST + i);
        for (int id : {MODE, KIND, SOURCE, SLOT, FORMAT, MEMORY, CURVE})
            child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
        const auto add = [&](int id, const wchar_t *label) {
            SendMessageW(control(id), CB_ADDSTRING, 0, (LPARAM)label);
        };
        add(MODE, L"Export to file");
        add(MODE, L"Import from file");
        add(KIND, L"One profile");
        add(KIND, L"Bank (up to five profiles)");
        add(SLOT, L"Choose destination");
        for (int i = 1; i <= 5; ++i) {
            wchar_t label[24]{};
            StringCchPrintfW(label, 24, L"%d", i);
            add(SOURCE, label);
            add(SLOT, label);
        }
        for (const auto *label : {L"Portable / use markers", L"Windows legacy INI", L"Linux legacy INI"})
            add(FORMAT, label);
        for (const auto *label : {L"Use markers", L"Physical MHz", L"Effective MHz"})
            add(MEMORY, label);
        for (const auto *label : {L"Use markers", L"Absolute", L"Absolute with origin", L"Base plus offset"})
            add(CURVE, label);
        for (int id : {MODE, KIND, SOURCE, FORMAT, MEMORY, CURVE})
            SendMessageW(control(id), CB_SETCURSEL, 0, 0);
        SendMessageW(control(SLOT), CB_SETCURSEL, slot_, 0);
        child(L"EDIT", L"profiles.gcprofile", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, PATH);
        SendMessageW(control(PATH), EM_SETLIMITTEXT, 4095, 0);
        child(L"BUTTON", L"Browse...", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, BROWSE);
        child(L"BUTTON", L"Preview", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, PREVIEW);
        child(L"BUTTON", L"Confirm import", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, COMMIT);
        child(L"BUTTON", L"Back", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, BACK);
        child(L"BUTTON", L"Cancel", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, CLOSE);
        child(L"BUTTON", L"Load first imported profile", BS_AUTOCHECKBOX | BS_NOTIFY | WS_TABSTOP, LOAD);
        child(L"BUTTON", L"Discard unsaved or unfinished draft", BS_AUTOCHECKBOX | BS_NOTIFY | WS_TABSTOP, DISCARD);
        SendMessageW(control(LOAD), BM_SETCHECK, needsDiscard_ ? BST_UNCHECKED : BST_CHECKED, 0);
        for (int id : {REPORT, STATUS})
            child(L"EDIT", L"", ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, id);
        for (int id = MODE; id <= LABEL_LAST; ++id)
            if (!control(id))
                return false;
        if (!control(CLOSE))
            return false;
        refresh_font();
        project();
        layout();
        notice("Choose records and file; Preview does not change saved settings");
        return true;
    }
    void project() {
        if (!control(PATH))
            return;
        for (int id : {MODE, KIND, PATH, BROWSE, SOURCE, SLOT, FORMAT, MEMORY, CURVE, PREVIEW}) {
            ShowWindow(control(id), reviewing_ ? SW_HIDE : SW_SHOW);
            EnableWindow(control(id), !busy_);
        }
        for (int id = LABEL_FIRST; id <= LABEL_LAST; ++id)
            ShowWindow(control(id), reviewing_ ? SW_HIDE : SW_SHOW);
        const bool all = selected(KIND) == 1, legacy = selected(FORMAT) > 0;
        EnableWindow(control(SOURCE), !busy_ && importing() && !all);
        EnableWindow(control(SLOT), !busy_ && !all);
        EnableWindow(control(MEMORY), !busy_ && legacy);
        EnableWindow(control(CURVE), !busy_ && legacy);
        ShowWindow(control(REPORT), reviewing_ ? SW_SHOW : SW_HIDE);
        ShowWindow(control(LOAD), reviewing_ && importing() ? SW_SHOW : SW_HIDE);
        ShowWindow(control(DISCARD), reviewing_ && importing() && checked(LOAD) && needsDiscard_ ? SW_SHOW : SW_HIDE);
        for (int id : {COMMIT, BACK}) {
            ShowWindow(control(id), reviewing_ ? SW_SHOW : SW_HIDE);
            EnableWindow(control(id), !busy_);
        }
        EnableWindow(control(LOAD), !busy_);
        EnableWindow(control(DISCARD), !busy_);
        EnableWindow(control(COMMIT),
                     !busy_ && ready_ && (!importing() || !checked(LOAD) || !needsDiscard_ || checked(DISCARD)));
        EnableWindow(control(CLOSE), !committing());
        SetWindowTextW(control(COMMIT), importing() ? L"Confirm import" : L"Write export");
    }
    GC_NOINLINE void prepare() {
        if (busy_ || reviewing_ || !callbacks_.prepare)
            return;
        auto wide = std::make_unique<wchar_t[]>(4096);
        auto path = std::make_unique<char[]>(4096);
        GetWindowTextW(control(PATH), wide.get(), 4096);
        if (!gc_wide_to_utf8(wide.get(), path.get(), 4096)) {
            notice("Choose a valid UTF-8 path shorter than 4096 bytes");
            return;
        }
        ProfileEditorTransferOptions options;
        options.importing = importing();
        options.all = selected(KIND) == 1;
        options.slot = options.all ? 1 : selected(SLOT);
        options.sourceRecord = selected(SOURCE) + 1;
        options.legacy = {(ProfileLegacyFormat)selected(FORMAT), (ProfileLegacyMemoryUnit)selected(MEMORY),
                          (ProfileLegacyCurveKind)selected(CURVE)};
        callbacks_.prepare(callbacks_.context, options, path.get());
    }
    void layout() {
        if (!control(PATH) || layingOut_)
            return;
        layingOut_ = true;
        RECT client{};
        for (int pass = 0; pass < 2; ++pass) {
            GetClientRect(window_, &client);
            const int w = std::max((int)client.right, px(420)), h = std::max((int)client.bottom, px(520));
            SCROLLINFO sx{sizeof(sx), SIF_RANGE | SIF_PAGE | SIF_POS, 0, w - 1, (UINT)client.right, scrollX_, 0};
            SCROLLINFO sy{sizeof(sy), SIF_RANGE | SIF_PAGE | SIF_POS, 0, h - 1, (UINT)client.bottom, scrollY_, 0};
            SetScrollInfo(window_, SB_HORZ, &sx, TRUE);
            SetScrollInfo(window_, SB_VERT, &sy, TRUE);
            GetScrollInfo(window_, SB_HORZ, &sx);
            GetScrollInfo(window_, SB_VERT, &sy);
            scrollX_ = sx.nPos;
            scrollY_ = sy.nPos;
        }
        GetClientRect(window_, &client);
        const int w = std::max((int)client.right, px(420)), h = std::max((int)client.bottom, px(520)), m = px(12),
                  gap = px(12), half = (w - 2 * m - gap) / 2;
        const auto place = [&](int id, int x, int y, int width, int height) {
            MoveWindow(control(id), x - scrollX_, y - scrollY_, width, height, TRUE);
        };
        place(TITLE, m, m, w - 2 * m, px(22));
        const int xs[] = {m, m + half + gap, m, m, m + half + gap, m, m, m + half + gap};
        const int ys[] = {44, 44, 108, 172, 172, 236, 300, 300};
        for (int i = 0; i < 8; ++i)
            place(LABEL_FIRST + i, xs[i], px(ys[i]), i == 2 || i == 5 ? w - 2 * m : half, px(22));
        for (int id : {MODE, KIND, SOURCE, SLOT, MEMORY, CURVE}) {
            const int i = id == MODE ? 0 : id == KIND ? 1 : id == SOURCE ? 3 : id == SLOT ? 4 : id == MEMORY ? 6 : 7;
            place(id, xs[i], px(ys[i] + 24), half, px(180));
        }
        place(PATH, m, px(132), w - 2 * m - px(100), px(28));
        place(BROWSE, w - m - px(92), px(132), px(92), px(28));
        place(FORMAT, m, px(260), w - 2 * m, px(180));
        place(REPORT, m, px(44), w - 2 * m, h - px(228));
        place(LOAD, m, h - px(164), w - 2 * m, px(24));
        place(DISCARD, m, h - px(136), w - 2 * m, px(24));
        place(PREVIEW, m, h - px(100), px(120), px(28));
        place(COMMIT, m, h - px(100), px(140), px(28));
        place(BACK, m + px(148), h - px(100), px(84), px(28));
        place(CLOSE, w - m - px(112), h - px(100), px(112), px(28));
        place(STATUS, m, h - px(64), w - 2 * m, px(56));
        layingOut_ = false;
    }
    void scroll(int bar, int command) {
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_ALL;
        GetScrollInfo(window_, bar, &info);
        int position = info.nPos;
        if (command == SB_LINEUP)
            position -= px(24);
        else if (command == SB_LINEDOWN)
            position += px(24);
        else if (command == SB_PAGEUP)
            position -= (int)info.nPage;
        else if (command == SB_PAGEDOWN)
            position += (int)info.nPage;
        else if (command == SB_THUMBTRACK || command == SB_THUMBPOSITION)
            position = info.nTrackPos;
        info.fMask = SIF_POS;
        info.nPos = position;
        SetScrollInfo(window_, bar, &info, TRUE);
        GetScrollInfo(window_, bar, &info);
        if (bar == SB_HORZ)
            scrollX_ = info.nPos;
        else
            scrollY_ = info.nPos;
        layout();
    }
    void reveal(HWND child) {
        RECT r{}, client{};
        GetWindowRect(child, &r);
        MapWindowPoints(nullptr, window_, (POINT *)&r, 2);
        GetClientRect(window_, &client);
        for (int bar : {SB_HORZ, SB_VERT}) {
            const int delta =
                bar == SB_HORZ
                    ? (r.right - r.left > client.right || r.left < 0 ? r.left : std::max(0L, r.right - client.right))
                    : (r.bottom - r.top > client.bottom || r.top < 0 ? r.top : std::max(0L, r.bottom - client.bottom));
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_POS;
            GetScrollInfo(window_, bar, &info);
            info.nPos += delta;
            SetScrollInfo(window_, bar, &info, TRUE);
            GetScrollInfo(window_, bar, &info);
            if (bar == SB_HORZ)
                scrollX_ = info.nPos;
            else
                scrollY_ = info.nPos;
        }
        layout();
    }
    void draw(const DRAWITEMSTRUCT &item) {
        const bool enabled = IsWindowEnabled(item.hwndItem) != FALSE;
        HBRUSH brush = CreateSolidBrush(!enabled                          ? COL_BUTTON_DISABLED
                                        : (item.itemState & ODS_SELECTED) ? COL_BUTTON_PRESSED
                                                                          : COL_BUTTON);
        FillRect(item.hDC, &item.rcItem, brush);
        DeleteObject(brush);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, enabled ? COL_BUTTON_LABEL : COL_LABEL);
        HGDIOBJ old = font_ ? SelectObject(item.hDC, font_) : nullptr;
        wchar_t label[64]{};
        GetWindowTextW(item.hwndItem, label, 64);
        RECT text = item.rcItem;
        DrawTextW(item.hDC, label, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (old)
            SelectObject(item.hDC, old);
        if (item.itemState & ODS_FOCUS) {
            RECT focus = item.rcItem;
            InflateRect(&focus, -px(3), -px(3));
            DrawFocusRect(item.hDC, &focus);
        }
    }
    void command(int id, int notification, HWND child) {
        const bool combo =
            id == MODE || id == KIND || id == SOURCE || id == SLOT || id == FORMAT || id == MEMORY || id == CURVE;
        const bool edit = id == PATH || id == REPORT || id == STATUS;
        if (child && (combo  ? notification == CBN_SETFOCUS
                      : edit ? notification == EN_SETFOCUS
                             : notification == BN_SETFOCUS))
            reveal(child);
        if (busy_) {
            if (id == CLOSE && notification == BN_CLICKED && !committing())
                SendMessageW(window_, WM_CLOSE, 0, 0);
            return;
        }
        if (combo && notification == CBN_SELCHANGE && !reviewing_) {
            if (id == MODE)
                SendMessageW(control(SLOT), CB_SETCURSEL, importing() ? 0 : slot_, 0);
            if (id == FORMAT) {
                SendMessageW(control(MEMORY), CB_SETCURSEL, 0, 0);
                SendMessageW(control(CURVE), CB_SETCURSEL, 0, 0);
            }
            ready_ = false;
            project();
            return;
        }
        if (notification != BN_CLICKED)
            return;
        if (id == CLOSE) {
            SendMessageW(window_, WM_CLOSE, 0, 0);
            return;
        }
        if (id == PREVIEW)
            prepare();
        else if (id == BROWSE && !reviewing_ && callbacks_.choose) {
            std::string path;
            char error[256]{};
            if (callbacks_.choose(callbacks_.context, importing(), path, error, sizeof(error))) {
                GcWideUtf8Arg wide(path.c_str());
                SetWindowTextW(control(PATH), wide.value);
            } else if (error[0])
                notice(error);
        } else if (id == BACK && reviewing_) {
            reviewing_ = ready_ = false;
            project();
            layout();
            SetFocus(control(CLOSE));
        } else if (id == LOAD || id == DISCARD) {
            if (id == LOAD)
                SendMessageW(control(DISCARD), BM_SETCHECK, BST_UNCHECKED, 0);
            project();
        } else if (id == COMMIT && reviewing_ && ready_ && IsWindowEnabled(control(COMMIT)) && callbacks_.commit)
            callbacks_.commit(callbacks_.context, importing() && checked(LOAD), checked(DISCARD));
    }
    static LRESULT CALLBACK dispatch(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        auto *self = (ProfileTransferWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (message == WM_NCCREATE) {
            self = (ProfileTransferWindow *)((CREATESTRUCTW *)l)->lpCreateParams;
            self->window_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self)
            return DefWindowProcW(hwnd, message, w, l);
        switch (message) {
        case WM_CREATE:
            return self->create_controls() ? 0 : -1;
        case WM_SIZE:
            self->layout();
            return 0;
        case WM_HSCROLL:
            self->scroll(SB_HORZ, LOWORD(w));
            return 0;
        case WM_VSCROLL:
            self->scroll(SB_VERT, LOWORD(w));
            return 0;
        case WM_COMMAND:
            self->command(LOWORD(w), HIWORD(w), (HWND)l);
            return 0;
        case WM_CLOSE:
            if (self->committing()) {
                self->notice("Wait for the confirmed file write to finish");
                return 0;
            }
            if (self->callbacks_.cancel)
                self->callbacks_.cancel(self->callbacks_.context);
            DestroyWindow(hwnd);
            return 0;
        case WM_DPICHANGED: {
            const int dpi = LOWORD(w);
            if (dpi >= 48 && dpi <= 768)
                self->dpi_ = dpi;
            self->refresh_font();
            const auto *r = (RECT *)l;
            if (r)
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            self->layout();
            HWND focus = GetFocus();
            if (focus && IsChild(hwnd, focus))
                self->reveal(focus);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT r{};
            GetClientRect(hwnd, &r);
            FillRect((HDC)w, &r, self->background_);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetTextColor((HDC)w, COL_TEXT);
            SetBkColor((HDC)w, COL_BG);
            return (LRESULT)self->background_;
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)w, COL_TEXT);
            SetBkColor((HDC)w, COL_INPUT);
            return (LRESULT)self->inputBrush_;
        case WM_DRAWITEM:
            self->draw(*(DRAWITEMSTRUCT *)l);
            return TRUE;
        case WM_NCDESTROY:
            self->window_ = nullptr;
            self->release();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(hwnd, message, w, l);
    }
};
