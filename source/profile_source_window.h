// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "profile_editor_document.h"
#include "profile_editor_source_rows.h"
#include "profile_capture_representation.h"
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
    void (*check)(void*, const ProfileDocument&, gc_u32, gc_u32) = nullptr;
    void (*inputChanged)(void*) = nullptr;
    void (*previewChanged)(void*) = nullptr;
    void (*acceptMapped)(void*) = nullptr;
    void (*captureRead)(void*, gc_u32) = nullptr;
    bool (*captureAccept)(void*, bool, bool, ProfileCaptureRepresentation) = nullptr;
    void (*captureCancel)(void*) = nullptr;
    void (*captureDomainsChanged)(void*) = nullptr;
};
// The native window edits only the canonical source. It has no driver, service
// or live-topology dependency; callbacks own storage and parent presentation.
class ProfileSourceWindow {
public:
    enum { LIST = 9101, INPUT, STAGE, SAVE, HELP, STATUS, TITLE, CHECK, REPORT, VIEW,
        GRAPHICS_ROUTE, MEMORY_ROUTE, GRAPHICS_TITLE, MEMORY_TITLE, USE_MAPPED,
        CAPTURE_READ = 9140, CAPTURE_ACCEPT, CAPTURE_ALLOW_OMISSIONS, CAPTURE_DISCARD, CAPTURE_DOMAIN_FIRST,
        CAPTURE_DOMAIN_LAST = CAPTURE_DOMAIN_FIRST + 8, CAPTURE_REPRESENTATION, CAPTURE_REPRESENTATION_TITLE, CLOSE = IDCANCEL };
    ProfileSourceWindow(ProfileEditorDocument& model, ProfileSourceWindowCallbacks callbacks)
        : model_(model), callbacks_(callbacks) {}
    ProfileSourceWindow(const ProfileSourceWindow&) = delete;
    ProfileSourceWindow& operator=(const ProfileSourceWindow&) = delete;
    ~ProfileSourceWindow() { if (window_) DestroyWindow(window_); release(); }
    HWND handle() const { return window_; }
    HWND control(int id) const { return window_ ? GetDlgItem(window_, id) : nullptr; }
    bool pending() const { return window_ && pending_; }
    void mapped_ready(bool ready) { mappedReady_ = ready; project_view(); }
    bool capture_active() const { return window_ && capture_; }
    void capture_require_discard() {
        captureNeedsDiscard_ = true; captureDiscard_ = false;
        SendMessageW(control(CAPTURE_DISCARD), BM_SETCHECK, BST_UNCHECKED, 0); project_view();
    }
    void begin_capture(bool needsDiscard) {
        capture_ = true; captureDomains_ = 31; captureNeedsDiscard_ = needsDiscard;
        capture_reset();
        for (int i = 0; i < 9; ++i) SendMessageW(control(CAPTURE_DOMAIN_FIRST + i), BM_SETCHECK,
            (captureDomains_ & (1u << i)) ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(window_, L"Capture current GPU settings"); SetWindowTextW(control(CLOSE), L"Cancel capture");
        SetWindowTextW(control(TITLE), L"Capture GPU controls");
        project_view(); layout(); SetFocus(control(CLOSE));
        notice("Choose settings; Read takes a passive snapshot without changing the previous draft");
    }
    void capture_reset() {
        captureReady_ = capturePartial_ = captureDiscard_ = false; captureOmissions_ = 0;
        captureRepresentation_ = PROFILE_CAPTURE_EXACT; captureRepresentations_ = 0;
        SendMessageW(control(CAPTURE_REPRESENTATION), CB_SETCURSEL, 0, 0);
        SetWindowTextW(control(CAPTURE_ACCEPT), L"Use exact");
        SendMessageW(control(CAPTURE_ALLOW_OMISSIONS), BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(control(CAPTURE_DISCARD), BM_SETCHECK, BST_UNCHECKED, 0);
        clear_report();
    }
    void capture_report(const std::vector<std::string>& lines, bool ready, gc_u32 omissions,
        unsigned int representations = profile_capture_representation_bit(PROFILE_CAPTURE_EXACT),
        ProfileCaptureRepresentation suggested = PROFILE_CAPTURE_EXACT) {
        captureReady_ = ready; captureOmissions_ = omissions;
        captureRepresentations_ = ready ? representations : 0u;
        captureRepresentation_ = (representations & profile_capture_representation_bit(suggested)) ? suggested : PROFILE_CAPTURE_EXACT;
        SendMessageW(control(CAPTURE_REPRESENTATION), CB_SETCURSEL, (int)captureRepresentation_ - 1, 0);
        capture_accept_label();
        report(lines); project_view();
    }
    bool route(MSG& message) const {
        if (!window_ || (message.hwnd != window_ && !IsChild(window_, message.hwnd))) return false;
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN && IsWindowEnabled(message.hwnd)) {
            const int id = GetDlgCtrlID(message.hwnd);
            if (id == STAGE || id == SAVE || id == CHECK || id == VIEW || id == USE_MAPPED || id == CLOSE ||
                id == CAPTURE_READ || id == CAPTURE_ACCEPT) {
                SendMessageW(window_, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)message.hwnd); return true;
            }
        }
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
        ++generation_; capture_ = false; pending_ = false; selected_ = -1; graphics_ = memory_ = 0;
        SetWindowTextW(control(CLOSE), L"Close source");
        SetWindowTextW(control(TITLE), L"Portable source draft");
        SendMessageW(control(GRAPHICS_ROUTE), CB_SETCURSEL, 0, 0); SendMessageW(control(MEMORY_ROUTE), CB_SETCURSEL, 0, 0);
        clear_report(); rebuild(-1);
        status(L"Source draft; destination review is required before Apply");
    }
    void notice(const char* message) { GcWideUtf8Arg text(message); status(text.value); }
    void clear_report() {
        reportAvailable_ = reportVisible_ = mappedReady_ = false;
        if (control(REPORT)) SetWindowTextW(control(REPORT), L"");
        project_view();
    }
    void report(const std::vector<std::string>& lines) {
        mappedReady_ = false;
        std::string body;
        for (const auto& line : lines) { body += line; body += "\r\n"; }
        GcWideUtf8Arg text(body.c_str());
        if (!text.valid_for(body.c_str())) { notice("Could not display the destination report"); return; }
        SetWindowTextW(control(REPORT), text.value);
        SendMessageW(control(REPORT), EM_SETSEL, 0, 0);
        reportAvailable_ = reportVisible_ = true; project_view();
    }
    bool save() {
        if (capture_) { notice("Cancel capture before saving the previous source draft"); return false; }
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
    bool reportAvailable_ = false, reportVisible_ = false, mappedReady_ = false;
    gc_u32 graphics_ = 0, memory_ = 0;
    bool capture_ = false, captureReady_ = false, capturePartial_ = false, captureDiscard_ = false, captureNeedsDiscard_ = false;
    gc_u32 captureDomains_ = 31, captureOmissions_ = 0;
    unsigned int captureRepresentations_ = 0;
    ProfileCaptureRepresentation captureRepresentation_ = PROFILE_CAPTURE_EXACT;
    unsigned long long generation_ = 0;
    std::vector<ProfileEditorSourceRow> rows_;
    int px(int value) const { return MulDiv(value, dpi_, 96); }
    void status(const wchar_t* text) { if (window_) SetWindowTextW(control(STATUS), text ? text : L""); }
    void update_title() {
        if (capture_) { SetWindowTextW(window_, L"Capture current GPU settings"); return; }
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
        for (int id : {LIST, INPUT, STAGE, SAVE, HELP, STATUS, TITLE, CLOSE, CHECK, REPORT, VIEW,
            GRAPHICS_ROUTE, MEMORY_ROUTE, GRAPHICS_TITLE, MEMORY_TITLE, USE_MAPPED,
            CAPTURE_READ, CAPTURE_ACCEPT, CAPTURE_ALLOW_OMISSIONS, CAPTURE_DISCARD, CAPTURE_REPRESENTATION, CAPTURE_REPRESENTATION_TITLE})
            SendMessageW(control(id), WM_SETFONT, (WPARAM)font_, FALSE);
        for (int id = CAPTURE_DOMAIN_FIRST; id <= CAPTURE_DOMAIN_LAST; ++id) SendMessageW(control(id), WM_SETFONT, (WPARAM)font_, FALSE);
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
        child(L"BUTTON", L"Check", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, CHECK);
        child(L"BUTTON", L"Use mapped draft", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, USE_MAPPED);
        child(L"BUTTON", L"Source", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, VIEW);
        child(L"BUTTON", L"Close source", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, CLOSE);
        child(L"EDIT", L"", ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, STATUS);
        child(L"EDIT", L"", ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, REPORT);
        child(L"STATIC", L"Check graphics control", 0, GRAPHICS_TITLE);
        child(L"STATIC", L"Check memory control", 0, MEMORY_TITLE);
        for (int id : {GRAPHICS_ROUTE, MEMORY_ROUTE}) {
            HWND combo = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
            for (const auto* label : {L"Auto", L"P0", L"Global"}) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)label);
            if (id == GRAPHICS_ROUTE) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"VF component");
        }
        for (int id : {LIST, INPUT, STAGE, SAVE, HELP, STATUS, TITLE, CLOSE, CHECK, REPORT, VIEW,
            GRAPHICS_ROUTE, MEMORY_ROUTE, GRAPHICS_TITLE, MEMORY_TITLE, USE_MAPPED}) if (!control(id)) return false;
        child(L"BUTTON", L"Read", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, CAPTURE_READ);
        child(L"BUTTON", L"Use exact", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, CAPTURE_ACCEPT);
        child(L"BUTTON", L"Allow omitted settings", BS_AUTOCHECKBOX | BS_NOTIFY | WS_TABSTOP, CAPTURE_ALLOW_OMISSIONS);
        child(L"BUTTON", L"Replace unsaved / unfinished draft", BS_AUTOCHECKBOX | BS_NOTIFY | WS_TABSTOP, CAPTURE_DISCARD);
        const wchar_t* domains[] = {L"VF points", L"Graphics", L"Memory", L"Power", L"Pin", L"Fan", L"XBAR", L"System", L"Video"};
        for (int i = 0; i < 9; ++i) child(L"BUTTON", domains[i], BS_AUTOCHECKBOX | BS_NOTIFY | WS_TABSTOP, CAPTURE_DOMAIN_FIRST + i);
        for (int id = CAPTURE_READ; id <= CAPTURE_DOMAIN_LAST; ++id) if (!control(id)) return false;
        child(L"STATIC", L"Representation", 0, CAPTURE_REPRESENTATION_TITLE);
        HWND representation = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, CAPTURE_REPRESENTATION);
        for (const auto* label : {L"Exact programmed offsets", L"Recognized flatten", L"Pin from labeled evidence"})
            SendMessageW(representation, CB_ADDSTRING, 0, (LPARAM)label);
        if (!control(CAPTURE_REPRESENTATION_TITLE) || !representation) return false;
        SendMessageW(control(REPORT), EM_SETLIMITTEXT, 128 * 1024, 0);
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
        select(false); EnableWindow(control(SAVE), model_.valid);
        EnableWindow(control(CHECK), model_.valid && callbacks_.check); project_view(); update_title();
        EnableWindow(control(GRAPHICS_ROUTE), model_.valid && model_.value.graphicsOffset.present);
        EnableWindow(control(MEMORY_ROUTE), model_.valid && model_.value.hasMemoryOffset);
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
        EnableWindow(input_, editable && !reportVisible_); EnableWindow(control(STAGE), editable && !reportVisible_);
        SetWindowTextW(control(HELP), editable && rows_[index].field == PROFILE_EDIT_POINT ?
            L"Absolute MHz replaces this point's intent" : editable ? L"Type physical offset MHz" : L"Select a source point or offset to edit");
        if (editable) { SendMessageW(input_, EM_SETSEL, 0, -1); }
    }
    void project_view() {
        if (!list_) return;
        for (int id : {HELP, INPUT, STAGE, SAVE, CHECK, GRAPHICS_ROUTE, MEMORY_ROUTE, GRAPHICS_TITLE, MEMORY_TITLE, USE_MAPPED})
            ShowWindow(control(id), capture_ ? SW_HIDE : SW_SHOW);
        for (int id = CAPTURE_READ; id <= CAPTURE_DOMAIN_LAST; ++id) ShowWindow(control(id), capture_ ? SW_SHOW : SW_HIDE);
        for (int id : {CAPTURE_REPRESENTATION, CAPTURE_REPRESENTATION_TITLE}) ShowWindow(control(id), capture_ ? SW_SHOW : SW_HIDE);
        EnableWindow(control(CAPTURE_REPRESENTATION), capture_ && captureReady_);
        EnableWindow(control(CAPTURE_READ), capture_ && captureDomains_ && callbacks_.captureRead);
        EnableWindow(control(CAPTURE_ACCEPT), capture_ && captureReady_ &&
            (captureRepresentations_ & profile_capture_representation_bit(captureRepresentation_)) && (!captureOmissions_ || capturePartial_) &&
            (!captureNeedsDiscard_ || captureDiscard_) && callbacks_.captureAccept);
        EnableWindow(control(CAPTURE_DISCARD), capture_ && captureNeedsDiscard_);
        EnableWindow(control(CAPTURE_ALLOW_OMISSIONS), capture_ && captureReady_ && captureOmissions_);
        if (capture_) { ShowWindow(list_, SW_HIDE); ShowWindow(control(VIEW), SW_HIDE); ShowWindow(control(REPORT), SW_SHOW); return; }
        ShowWindow(list_, reportVisible_ ? SW_HIDE : SW_SHOW);
        ShowWindow(control(REPORT), reportVisible_ ? SW_SHOW : SW_HIDE);
        ShowWindow(control(VIEW), reportAvailable_ ? SW_SHOW : SW_HIDE);
        SetWindowTextW(control(VIEW), reportVisible_ ? L"Source" : L"Report");
        const LRESULT index = selected_ < 0 ? LB_ERR : SendMessageW(list_, LB_GETITEMDATA, selected_, 0);
        const bool editable = !reportVisible_ && model_.valid && index >= 0 && (size_t)index < rows_.size() && rows_[index].editable;
        EnableWindow(input_, editable); EnableWindow(control(STAGE), editable);
        EnableWindow(control(USE_MAPPED), !capture_ && !pending_ && reportVisible_ && mappedReady_ && callbacks_.acceptMapped);
    }
    void preview_choice(int id) {
        if (!model_.valid || (id == GRAPHICS_ROUTE ? !model_.value.graphicsOffset.present : !model_.value.hasMemoryOffset)) return;
        const int selected = (int)SendMessageW(control(id), CB_GETCURSEL, 0, 0);
        if (selected < 0 || selected > (id == GRAPHICS_ROUTE ? 3 : 2)) return;
        const gc_u32 choice = selected == 1 ? CLOCK_OFFSET_SURFACE_P0 : selected == 2 ? CLOCK_OFFSET_SURFACE_GLOBAL :
            selected == 3 ? (gc_u32)DESIRED_GRAPHICS_VF_COMPONENT : 0u;
        auto& current = id == GRAPHICS_ROUTE ? graphics_ : memory_;
        if (current == choice) return;
        current = choice; clear_report();
        if (callbacks_.previewChanged) callbacks_.previewChanged(callbacks_.context);
        status(L"Preview control changed; source unchanged; Check reviews this choice");
    }
    void layout() {
        if (!list_ || layingOut_) return;
        layingOut_ = true; RECT client{};
        for (int pass = 0; pass < 2; ++pass) {
            GetClientRect(window_, &client);
            const int width = std::max((int)client.right, px(360)), height = std::max((int)client.bottom, px(capture_ ? 456 : 392));
            SCROLLINFO x{sizeof(x), SIF_RANGE | SIF_PAGE | SIF_POS, 0, width - 1, (UINT)client.right, scrollX_, 0};
            SCROLLINFO y{sizeof(y), SIF_RANGE | SIF_PAGE | SIF_POS, 0, height - 1, (UINT)client.bottom, scrollY_, 0};
            SetScrollInfo(window_, SB_HORZ, &x, TRUE); SetScrollInfo(window_, SB_VERT, &y, TRUE);
        }
        SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_POS;
        GetScrollInfo(window_, SB_HORZ, &info); scrollX_ = info.nPos; GetScrollInfo(window_, SB_VERT, &info); scrollY_ = info.nPos;
        GetClientRect(window_, &client); const int w = std::max((int)client.right, px(360)), h = std::max((int)client.bottom, px(capture_ ? 456 : 392));
        const int m = px(12), row = px(28), gap = px(8);
        const auto place = [&](int id, int x, int y, int width, int height) { MoveWindow(control(id), x - scrollX_, y - scrollY_, width, height, TRUE); };
        place(TITLE, m, m, w - 2*m - px(104), px(22)); place(VIEW, w - m - px(96), m, px(96), px(24));
        place(LIST, m, px(42), w - 2*m, h - px(308)); place(REPORT, m, px(42), w - 2*m, h - px(308));
        place(GRAPHICS_TITLE, m, h - px(262), w/2 - m - px(6), px(22));
        place(MEMORY_TITLE, w/2 + px(6), h - px(262), w/2 - m - px(6), px(22));
        place(GRAPHICS_ROUTE, m, h - px(238), w/2 - m - px(6), px(180));
        place(MEMORY_ROUTE, w/2 + px(6), h - px(238), w/2 - m - px(6), px(180));
        place(USE_MAPPED, m, h - px(202), px(180), row);
        place(HELP, m, h - px(166), w - 2*m, px(22));
        place(INPUT, m, h - px(138), w - 2*m - px(128) - gap, row); place(STAGE, w - m - px(128), h - px(138), px(128), row);
        place(SAVE, m, h - px(100), px(120), row); place(CHECK, m + px(128), h - px(100), px(80), row);
        place(CLOSE, w - m - px(120), h - px(100), px(120), row);
        place(STATUS, m, h - px(64), w - 2*m, px(56));
        if (capture_) {
            const int column = (w - 2*m) / 3;
            for (int i = 0; i < 9; ++i) place(CAPTURE_DOMAIN_FIRST + i, m + (i%3)*column, px(42) + (i/3)*px(28), column, px(24));
            place(REPORT, m, px(134), w - 2*m, h - px(350));
            place(CAPTURE_REPRESENTATION_TITLE, m, h - px(210), px(108), px(24));
            place(CAPTURE_REPRESENTATION, m + px(112), h - px(214), w - 2*m - px(112), px(180));
            place(CAPTURE_ALLOW_OMISSIONS, m, h - px(168), w - 2*m, px(24));
            place(CAPTURE_DISCARD, m, h - px(140), w - 2*m, px(24));
            place(CAPTURE_READ, m, h - px(104), px(80), row);
            place(CAPTURE_ACCEPT, m + px(88), h - px(104), px(128), row);
            place(CLOSE, w - m - px(112), h - px(104), px(112), row);
        }
        layingOut_ = false;
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
            const int delta = bar == SB_HORZ ? (r.right-r.left > client.right || r.left < 0 ? r.left : std::max(0L, r.right - client.right)) :
                (r.bottom-r.top > client.bottom || r.top < 0 ? r.top : std::max(0L, r.bottom - client.bottom));
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
    static bool focus_notification(int id, int notification) {
        switch (id) {
            case LIST: return notification == LBN_SETFOCUS;
            case INPUT: case REPORT: case STATUS: return notification == EN_SETFOCUS;
            case GRAPHICS_ROUTE: case MEMORY_ROUTE: case CAPTURE_REPRESENTATION: return notification == CBN_SETFOCUS;
            case STAGE: case SAVE: case CLOSE: case CHECK: case VIEW: case USE_MAPPED: return notification == BN_SETFOCUS;
            case CAPTURE_READ: case CAPTURE_ACCEPT: case CAPTURE_ALLOW_OMISSIONS: case CAPTURE_DISCARD: return notification == BN_SETFOCUS;
            default: return id >= CAPTURE_DOMAIN_FIRST && id <= CAPTURE_DOMAIN_LAST && notification == BN_SETFOCUS;
        }
    }
    bool capture_command(int id, int notification) {
        if (!capture_ || notification != BN_CLICKED) return false;
        if (id >= CAPTURE_DOMAIN_FIRST && id <= CAPTURE_DOMAIN_LAST) {
            const gc_u32 bit = 1u << (id - CAPTURE_DOMAIN_FIRST);
            if (SendMessageW(control(id), BM_GETCHECK, 0, 0) == BST_CHECKED) captureDomains_ |= bit; else captureDomains_ &= ~bit;
            capture_reset();
            if (callbacks_.captureDomainsChanged) callbacks_.captureDomainsChanged(callbacks_.context);
            notice("Capture settings changed; Read a new snapshot before accepting");
        } else if (id == CAPTURE_ALLOW_OMISSIONS || id == CAPTURE_DISCARD) {
            const bool checked = SendMessageW(control(id), BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (id == CAPTURE_ALLOW_OMISSIONS) capturePartial_ = checked; else captureDiscard_ = checked;
            project_view();
        } else if (id == CAPTURE_READ) {
            if (captureDomains_ && callbacks_.captureRead) {
                capture_reset(); callbacks_.captureRead(callbacks_.context, captureDomains_);
            }
        } else if (id == CAPTURE_ACCEPT) {
            if (captureReady_ && (captureRepresentations_ & profile_capture_representation_bit(captureRepresentation_)) &&
                (!captureOmissions_ || capturePartial_) && (!captureNeedsDiscard_ || captureDiscard_) && callbacks_.captureAccept)
                (void)callbacks_.captureAccept(callbacks_.context, capturePartial_, captureDiscard_, captureRepresentation_);
        } else return false;
        return true;
    }
    void capture_accept_label() {
        SetWindowTextW(control(CAPTURE_ACCEPT), captureRepresentation_ == PROFILE_CAPTURE_FLATTEN ? L"Use flatten" :
            captureRepresentation_ == PROFILE_CAPTURE_PIN ? L"Use pin" : L"Use exact");
    }
    void capture_choice() {
        const int choice = (int)SendMessageW(control(CAPTURE_REPRESENTATION), CB_GETCURSEL, 0, 0);
        if (!capture_ || choice < 0 || choice > 2) return;
        captureRepresentation_ = (ProfileCaptureRepresentation)(choice + 1);
        capture_accept_label(); project_view();
        notice(captureRepresentations_ & profile_capture_representation_bit(captureRepresentation_) ?
            "Representation selected; acceptance changes only the draft" :
            "This representation is unavailable in the reviewed snapshot; choose another or Read again");
    }
    void cancel_capture() {
        capture_ = false; captureReady_ = false; clear_report();
        SetWindowTextW(control(CLOSE), L"Close source"); SetWindowTextW(control(TITLE), L"Portable source draft");
        update_title(); layout();
        if (callbacks_.captureCancel) callbacks_.captureCancel(callbacks_.context);
        if (!model_.valid) DestroyWindow(window_);
        else notice("Capture cancelled; previous source and unfinished input kept");
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
                if (l && focus_notification(LOWORD(w), HIWORD(w))) self->reveal((HWND)l);
                if (self->capture_ && LOWORD(w) == CAPTURE_REPRESENTATION && HIWORD(w) == CBN_SELCHANGE) { self->capture_choice(); return 0; }
                if (self->capture_command(LOWORD(w), HIWORD(w))) return 0;
                if (self->capture_ && (LOWORD(w) == STAGE || LOWORD(w) == CHECK || LOWORD(w) == SAVE || LOWORD(w) == VIEW || LOWORD(w) == LIST || LOWORD(w) == USE_MAPPED)) return 0;
                if (LOWORD(w) == LIST && HIWORD(w) == LBN_SELCHANGE) self->select(true);
                else if (LOWORD(w) == INPUT && HIWORD(w) == EN_CHANGE && !self->updating_) {
                    self->pending_ = true; self->update_title();
                    if (self->callbacks_.inputChanged) self->callbacks_.inputChanged(self->callbacks_.context);
                }
                else if (LOWORD(w) == STAGE && HIWORD(w) == BN_CLICKED) (void)self->commit(true);
                else if (LOWORD(w) == SAVE && HIWORD(w) == BN_CLICKED) (void)self->save();
                else if (LOWORD(w) == CHECK && HIWORD(w) == BN_CLICKED && self->callbacks_.check && self->model_.valid && self->commit(false))
                    self->callbacks_.check(self->callbacks_.context, self->model_.value, self->graphics_, self->memory_);
                else if (LOWORD(w) == USE_MAPPED && HIWORD(w) == BN_CLICKED && !self->pending_ && self->reportVisible_ &&
                    self->mappedReady_ && self->callbacks_.acceptMapped) self->callbacks_.acceptMapped(self->callbacks_.context);
                else if ((LOWORD(w) == GRAPHICS_ROUTE || LOWORD(w) == MEMORY_ROUTE) && HIWORD(w) == CBN_SELCHANGE)
                    self->preview_choice(LOWORD(w));
                else if (LOWORD(w) == VIEW && HIWORD(w) == BN_CLICKED && self->reportAvailable_) {
                    if (!self->reportVisible_ && !self->commit(false)) return 0;
                    self->reportVisible_ = !self->reportVisible_; self->project_view();
                }
                else if (LOWORD(w) == CLOSE && HIWORD(w) == BN_CLICKED) SendMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            case WM_CLOSE:
                if (self->capture_) { self->cancel_capture(); return 0; }
                if ((self->pending_ || profile_editor_document_dirty(self->model_)) &&
                    (!self->callbacks_.confirmDiscard || !self->callbacks_.confirmDiscard(self->callbacks_.context))) return 0;
                if (self->callbacks_.closed) self->callbacks_.closed(self->callbacks_.context);
                DestroyWindow(hwnd); return 0;
            case WM_ERASEBKGND: { RECT r{}; GetClientRect(hwnd, &r); if (self->background_) FillRect((HDC)w, &r, self->background_); return 1; }
            case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
                SetTextColor((HDC)w, IsWindowEnabled((HWND)l) ? COL_TEXT : COL_LABEL);
                SetBkColor((HDC)w, COL_BG); return (LRESULT)self->background_;
            case WM_CTLCOLORLISTBOX: case WM_CTLCOLOREDIT:
                SetTextColor((HDC)w, COL_TEXT); SetBkColor((HDC)w, COL_INPUT); return (LRESULT)self->inputBrush_;
            case WM_DRAWITEM: self->draw(*(DRAWITEMSTRUCT*)l); return TRUE;
            case WM_NCDESTROY:
                self->window_ = self->list_ = self->input_ = nullptr; self->pending_ = false;
                self->release(); ++self->generation_;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); break;
        }
        return DefWindowProcW(hwnd, message, w, l);
    }
};
