// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#include "profile_source_window.h"
#include <cstring>
namespace {
struct Host {
    int saves = 0, changes = 0, closes = 0, confirms = 0;
    bool discard = false, failSave = false;
    ProfileDocument saved{};
};
static bool save(void* context, const ProfileDocument& document, char* error, size_t size) {
    auto& host = *(Host*)context; ++host.saves;
    if (host.failSave) { snprintf(error, size, "fixture refusal"); return false; }
    host.saved = document; return true;
}
static bool confirm(void* context) { auto& h = *(Host*)context; ++h.confirms; return h.discard; }
static void changed(void* context) { ++((Host*)context)->changes; }
static void closed(void* context) { ++((Host*)context)->closes; }
static void select(ProfileSourceWindow& window, int row) {
    SendMessageW(window.control(ProfileSourceWindow::LIST), LB_SETCURSEL, row, 0);
    SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(ProfileSourceWindow::LIST, LBN_SELCHANGE), (LPARAM)window.control(ProfileSourceWindow::LIST));
}
static std::wstring text(HWND control) {
    wchar_t value[512]{}; GetWindowTextW(control, value, 512); return value;
}
}
int run_profile_source_window_tests() {
    auto model = std::make_unique<ProfileEditorDocument>(); model->valid = true;
    auto& d = model->value; d = profile_document_make_empty();
    snprintf(d.displayName, sizeof(d.displayName), "Source \xe6\x9b\xb2\xe7\xb7\x9a");
    d.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER;
    d.sourceTopologyComplete = true; d.topologyCount = 2;
    d.topology[0] = {42, 77, 650000, 1}; d.topology[1] = {43, 78, 700000, 2};
    d.points[0].kind = d.points[1].kind = PROFILE_POINT_CAPTURED_OFFSET;
    d.points[0].hasObservedFrequency = true; d.points[0].observedFrequencyHz = 1200123456;
    d.points[1].capturedOffsetHz = -100000500;
    d.hasMemoryOffset = true; d.memoryOffsetHz = -123500000; d.memorySurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    d.power.present = true; d.power.ratio = {2, 3}; model->saved = d;
    auto host = std::make_unique<Host>();
    ProfileSourceWindowCallbacks callbacks{host.get(), save, confirm, changed, closed, nullptr};
    ProfileSourceWindow window(*model, callbacks);
    if (!window.create(nullptr, 96, false) || IsWindowVisible(window.handle())) return 9330;
    const LONG_PTR statusStyle = GetWindowLongPtrW(window.control(ProfileSourceWindow::STATUS), GWL_STYLE);
    if (!(statusStyle & ES_READONLY) || !(statusStyle & ES_MULTILINE) || !(statusStyle & WS_VSCROLL)) return 9348;
    const auto rows = profile_editor_source_rows(d); int point = -1, memory = -1, readOnly = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].field == PROFILE_EDIT_POINT && rows[i].editable) point = (int)i;
        if (rows[i].field == PROFILE_EDIT_MEMORY && rows[i].editable) memory = (int)i;
        if (rows[i].text.find("source read-only") != std::string::npos) readOnly = (int)i;
    }
    if (point < 0 || memory < 0 || readOnly < 0 || SendMessageW(window.control(ProfileSourceWindow::LIST), LB_GETCOUNT, 0, 0) != (LRESULT)rows.size()) return 9331;
    wchar_t name[512]{}; SendMessageW(window.control(ProfileSourceWindow::LIST), LB_GETTEXT, 0, (LPARAM)name);
    if (std::wstring(name) != L"Name: Source \u66f2\u7dda") return 9332;
    select(window, point);
    if (text(window.control(ProfileSourceWindow::INPUT)) != L"1200.123456" || window.pending() || profile_editor_document_dirty(*model)) return 9333;
    if (!window.save() || host->saves != 1 || host->saved.points[0].kind != PROFILE_POINT_CAPTURED_OFFSET || host->changes) return 9334;
    SetWindowTextW(window.control(ProfileSourceWindow::INPUT), L"1200.123456");
    MSG enter{}; enter.hwnd = window.control(ProfileSourceWindow::INPUT); enter.message = WM_KEYDOWN; enter.wParam = VK_RETURN;
    if (!window.route(enter) || host->changes != 1 || d.points[0].kind != PROFILE_POINT_ABSOLUTE_FREQUENCY ||
        d.points[0].absoluteFrequencyHz != 1200123456 || d.points[1].capturedOffsetHz != -100000500 ||
        d.memoryOffsetHz != -123500000 || d.power.ratio.denominator != 3 || model->revision != 2) return 9335;
    SetWindowTextW(window.control(ProfileSourceWindow::INPUT), L"1.");
    if (!window.pending() || window.save() || host->saves != 1 || d.points[0].absoluteFrequencyHz != 1200123456) return 9336;
    select(window, memory);
    if (text(window.control(ProfileSourceWindow::INPUT)) != L"1." || SendMessageW(window.control(ProfileSourceWindow::LIST), LB_GETCURSEL, 0, 0) != point) return 9337;
    SendMessageW(window.control(ProfileSourceWindow::INPUT), EM_SETSEL, 1, 2);
    RECT placement{0, 0, 320, 250}; SendMessageW(window.handle(), WM_DPICHANGED, MAKEWPARAM(144, 144), (LPARAM)&placement);
    DWORD start = 0, end = 0; SendMessageW(window.control(ProfileSourceWindow::INPUT), EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    if (!window.pending() || text(window.control(ProfileSourceWindow::INPUT)) != L"1." || start != 1 || end != 2) return 9338;
    const HWND help = window.control(ProfileSourceWindow::HELP);
    HDC dc = GetDC(help); if (!dc) return 9349;
    HGDIOBJ oldFont = SelectObject(dc, (HFONT)SendMessageW(help, WM_GETFONT, 0, 0));
    const auto label = text(help); SIZE labelSize{};
    const bool measured = GetTextExtentPoint32W(dc, label.c_str(), (int)label.size(), &labelSize) != FALSE;
    SelectObject(dc, oldFont); ReleaseDC(help, dc); RECT helpRect{}; GetClientRect(help, &helpRect);
    if (!measured || labelSize.cx > helpRect.right || labelSize.cy > helpRect.bottom) return 9349;
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    if (!window.handle() || host->confirms != 1 || host->closes || text(window.control(ProfileSourceWindow::INPUT)) != L"1.") return 9339;
    SetWindowTextW(window.control(ProfileSourceWindow::INPUT), L"1300.000001");
    if (!window.route(enter) || window.pending() || d.points[0].absoluteFrequencyHz != 1300000001) return 9340;
    select(window, readOnly);
    if (IsWindowEnabled(window.control(ProfileSourceWindow::INPUT)) || IsWindowEnabled(window.control(ProfileSourceWindow::STAGE))) return 9341;
    select(window, memory); SetWindowTextW(window.control(ProfileSourceWindow::INPUT), L"-123.500001");
    host->failSave = true;
    if (window.save() || host->saves != 2 || !profile_editor_document_dirty(*model) || d.memoryOffsetHz != -123500001) return 9342;
    host->failSave = false;
    if (!window.save() || host->saves != 3 || profile_editor_document_dirty(*model) || model->unsaved ||
        host->saved.memoryOffsetHz != -123500001 || host->saved.memorySurface != CLOCK_OFFSET_SURFACE_GLOBAL) return 9343;
    SendMessageW(window.handle(), WM_VSCROLL, SB_BOTTOM, 0); SendMessageW(window.handle(), WM_HSCROLL, SB_RIGHT, 0);
    RECT button{}, client{}; GetWindowRect(window.control(ProfileSourceWindow::CLOSE), &button);
    MapWindowPoints(nullptr, window.handle(), (POINT*)&button, 2); GetClientRect(window.handle(), &client);
    if (button.right > client.right || button.bottom > client.bottom || button.left < 0 || button.top < 0) return 9344;
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    if (window.handle() || host->closes != 1 || host->confirms != 1) return 9345;
    if (!window.create(nullptr, 96, false)) return 9346;
    select(window, point); SetWindowTextW(window.control(ProfileSourceWindow::INPUT), L"invalid");
    model->unsaved = true; host->discard = true;
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    if (window.handle() || host->confirms != 2 || host->closes != 2 || window.pending()) return 9347;
    return 0;
}
#else
int run_profile_source_window_tests() { return 0; }
#endif
