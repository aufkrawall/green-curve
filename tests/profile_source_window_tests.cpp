// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#include "profile_source_window.h"
#include <cstring>
namespace {
struct Host {
    int saves = 0, changes = 0, closes = 0, confirms = 0, checks = 0, inputChanges = 0, previewChanges = 0;
    gc_u32 graphics = 0, memory = 0;
    int captureReads = 0, captureAccepts = 0, captureCancels = 0, domainChanges = 0, mapped = 0;
    int converts = 0, lastConvertKind = -1, restores = 0, prepares = 0, lastPrepareMode = -1;
    gc_u32 captureDomains = 0;
    bool partial = false, discardCapture = false;
    ProfileCaptureRepresentation representation = PROFILE_CAPTURE_UNDECIDED;
    bool discard = false, failSave = false;
    ProfileDocument saved{};
    ProfileSourceWindow* window = nullptr;
};
static bool save(void* context, const ProfileDocument& document, char* error, size_t size) {
    auto& host = *(Host*)context; ++host.saves;
    if (host.failSave) { snprintf(error, size, "fixture refusal"); return false; }
    host.saved = document; return true;
}
static bool confirm(void* context) { auto& h = *(Host*)context; ++h.confirms; return h.discard; }
static void changed(void* context) { ++((Host*)context)->changes; }
static void closed(void* context) { ++((Host*)context)->closes; }
static void check(void* context, const ProfileDocument& document, gc_u32 graphics, gc_u32 memory) {
    auto& h = *(Host*)context; ++h.checks; h.saved = document; h.graphics = graphics; h.memory = memory;
}
static void input_changed(void* context) {
    auto& h = *(Host*)context; ++h.inputChanges;
    if (h.window) h.window->clear_report();
}
static void preview_changed(void* context) { ++((Host*)context)->previewChanges; input_changed(context); }
static void capture_read(void* context, gc_u32 domains) { auto& h = *(Host*)context; ++h.captureReads; h.captureDomains = domains; }
static bool capture_accept(void* context, bool partial, bool discard, ProfileCaptureRepresentation representation) {
    auto& h = *(Host*)context; ++h.captureAccepts; h.partial = partial; h.discardCapture = discard; h.representation = representation; return false;
}
static void capture_cancel(void* context) { ++((Host*)context)->captureCancels; }
static void domains_changed(void* context) { ++((Host*)context)->domainChanges; }
static void mapped(void* context) { ++((Host*)context)->mapped; }
static void convert_domain(void* context, int kind) {
    auto& h = *(Host*)context; ++h.converts; h.lastConvertKind = kind;
}
static void restore_exact(void* context) { ++((Host*)context)->restores; }
static void prepare_curve(void* context, int mode) {
    auto& h = *(Host*)context; ++h.prepares; h.lastPrepareMode = mode;
}
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
    callbacks.check = check; callbacks.inputChanged = input_changed;
    callbacks.previewChanged = preview_changed;
    callbacks.acceptMapped = mapped;
    callbacks.captureRead = capture_read; callbacks.captureAccept = capture_accept;
    callbacks.captureCancel = capture_cancel; callbacks.captureDomainsChanged = domains_changed;
    callbacks.convertDomain = convert_domain; callbacks.restoreExact = restore_exact; callbacks.prepareCurve = prepare_curve;
    ProfileSourceWindow window(*model, callbacks);
    host->window = &window;
    if (!window.create(nullptr, 96, false) || IsWindowVisible(window.handle())) return 9330;
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::USE_MAPPED, 0);
    if (host->mapped || IsWindowEnabled(window.control(ProfileSourceWindow::USE_MAPPED))) return 9653;
    window.report({"Reviewed settings"}); window.mapped_ready(true);
    SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(ProfileSourceWindow::USE_MAPPED, BN_SETFOCUS), (LPARAM)window.control(ProfileSourceWindow::USE_MAPPED));
    if (host->mapped || !IsWindowEnabled(window.control(ProfileSourceWindow::USE_MAPPED))) return 9654;
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::USE_MAPPED, 0);
    if (host->mapped != 1 || profile_editor_document_dirty(*model)) return 9655;
    MSG key{}; key.hwnd = window.control(ProfileSourceWindow::USE_MAPPED); key.message = WM_KEYDOWN; key.wParam = VK_RETURN;
    window.route(key);
    if (host->mapped != 2) return 9673;
    window.clear_report();
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::USE_MAPPED, 0);
    if (host->mapped != 2 || IsWindowEnabled(window.control(ProfileSourceWindow::USE_MAPPED))) return 9656;
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
    if (IsWindowEnabled(window.control(ProfileSourceWindow::GRAPHICS_ROUTE)) ||
        !IsWindowEnabled(window.control(ProfileSourceWindow::MEMORY_ROUTE))) return 9387;
    SendMessageW(window.control(ProfileSourceWindow::MEMORY_ROUTE), CB_SETCURSEL, 1, 0);
    SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(ProfileSourceWindow::MEMORY_ROUTE, CBN_SELCHANGE), 0);
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::CHECK, 0);
    if (host->checks != 1 || host->saved.points[0].kind != PROFILE_POINT_CAPTURED_OFFSET || host->changes ||
        host->memory != CLOCK_OFFSET_SURFACE_P0 || host->graphics || host->previewChanges != 1 ||
        d.memorySurface != CLOCK_OFFSET_SURFACE_GLOBAL || profile_editor_document_dirty(*model)) return 9364;
    SetWindowTextW(window.control(ProfileSourceWindow::INPUT), L"1200.123456");
    MSG enter{}; enter.hwnd = window.control(ProfileSourceWindow::INPUT); enter.message = WM_KEYDOWN; enter.wParam = VK_RETURN;
    if (!window.route(enter) || host->changes != 1 || d.points[0].kind != PROFILE_POINT_ABSOLUTE_FREQUENCY ||
        d.points[0].absoluteFrequencyHz != 1200123456 || d.points[1].capturedOffsetHz != -100000500 ||
        d.memoryOffsetHz != -123500000 || d.power.ratio.denominator != 3 || model->revision != 2) return 9335;
    SetWindowTextW(window.control(ProfileSourceWindow::INPUT), L"1.");
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::CHECK, 0);
    if (host->checks != 1 || !host->inputChanges) return 9365;
    if (!window.pending() || window.save() || host->saves != 1 || d.points[0].absoluteFrequencyHz != 1200123456) return 9336;
    select(window, memory);
    if (text(window.control(ProfileSourceWindow::INPUT)) != L"1." || SendMessageW(window.control(ProfileSourceWindow::LIST), LB_GETCURSEL, 0, 0) != point) return 9337;
    SendMessageW(window.control(ProfileSourceWindow::INPUT), EM_SETSEL, 1, 2);
    RECT placement{0, 0, 320, 250}; SendMessageW(window.handle(), WM_DPICHANGED, MAKEWPARAM(144, 144), (LPARAM)&placement);
    DWORD start = 0, end = 0; SendMessageW(window.control(ProfileSourceWindow::INPUT), EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    if (!window.pending() || text(window.control(ProfileSourceWindow::INPUT)) != L"1." || start != 1 || end != 2) return 9338;
    for (int id : {ProfileSourceWindow::HELP, ProfileSourceWindow::GRAPHICS_TITLE, ProfileSourceWindow::MEMORY_TITLE}) {
        const HWND help = window.control(id);
        HDC dc = GetDC(help); if (!dc) return 9349;
        HGDIOBJ oldFont = SelectObject(dc, (HFONT)SendMessageW(help, WM_GETFONT, 0, 0));
        const auto label = text(help); SIZE labelSize{};
        const bool measured = GetTextExtentPoint32W(dc, label.c_str(), (int)label.size(), &labelSize) != FALSE;
        SelectObject(dc, oldFont); ReleaseDC(help, dc); RECT helpRect{}; GetClientRect(help, &helpRect);
        if (!measured || labelSize.cx > helpRect.right || labelSize.cy > helpRect.bottom) {
            fprintf(stderr, "source-window label id=%d measured=%d text=%ldx%ld bounds=%ldx%ld\n", id,
                measured ? 1 : 0, labelSize.cx, labelSize.cy, helpRect.right, helpRect.bottom);
            return 9349;
        }
    }
    SendMessageW(window.handle(), WM_VSCROLL, SB_TOP, 0); SendMessageW(window.handle(), WM_HSCROLL, SB_LEFT, 0);
    const HWND memoryChoice = window.control(ProfileSourceWindow::MEMORY_ROUTE);
    SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(ProfileSourceWindow::MEMORY_ROUTE, CBN_SETFOCUS), (LPARAM)memoryChoice);
    RECT choiceRect{}, viewport{}; GetWindowRect(memoryChoice, &choiceRect);
    MapWindowPoints(nullptr, window.handle(), (POINT*)&choiceRect, 2); GetClientRect(window.handle(), &viewport);
    if (choiceRect.left < 0 || choiceRect.top < 0 || choiceRect.right > viewport.right || choiceRect.bottom > viewport.bottom ||
        !window.pending() || text(window.control(ProfileSourceWindow::INPUT)) != L"1.") return 9393;
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
    const std::vector<std::string> report{"Destination check", "Precise request=123500001 Hz; trailing maximum=3000000000 Hz"};
    window.report(report);
    const auto visible = [](HWND child) { return (GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) != 0; };
    if (visible(window.control(ProfileSourceWindow::LIST)) || !visible(window.control(ProfileSourceWindow::REPORT)) ||
        IsWindowEnabled(window.control(ProfileSourceWindow::INPUT)) ||
        text(window.control(ProfileSourceWindow::REPORT)).find(L"maximum=3000000000") == std::wstring::npos) return 9366;
    const LONG_PTR reportStyle = GetWindowLongPtrW(window.control(ProfileSourceWindow::REPORT), GWL_STYLE);
    if (!(reportStyle & ES_READONLY) || !(reportStyle & ES_MULTILINE) || !(reportStyle & WS_VSCROLL)) return 9367;
    SendMessageW(window.control(ProfileSourceWindow::MEMORY_ROUTE), CB_SETCURSEL, 2, 0);
    SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(ProfileSourceWindow::MEMORY_ROUTE, CBN_SELCHANGE), 0);
    if (visible(window.control(ProfileSourceWindow::VIEW)) || !visible(window.control(ProfileSourceWindow::LIST)) ||
        host->previewChanges != 2 || d.memorySurface != CLOCK_OFFSET_SURFACE_GLOBAL || profile_editor_document_dirty(*model)) return 9388;
    window.report(report);
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::VIEW, 0);
    if (!visible(window.control(ProfileSourceWindow::LIST)) || !IsWindowEnabled(window.control(ProfileSourceWindow::INPUT)) ||
        text(window.control(ProfileSourceWindow::INPUT)) != L"-123.500001") return 9368;
    SetWindowTextW(window.control(ProfileSourceWindow::INPUT), L"invalid");
    if (visible(window.control(ProfileSourceWindow::VIEW)) || !window.pending() ||
        text(window.control(ProfileSourceWindow::INPUT)) != L"invalid") return 9369;
    SetWindowTextW(window.control(ProfileSourceWindow::INPUT), L"-123.500001");
    if (!window.save()) return 9370;
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    if (window.handle() || host->closes != 1 || host->confirms != 1) return 9345;
    if (!window.create(nullptr, 96, false)) return 9346;
    d.domains |= PROFILE_DOMAIN_GRAPHICS_OFFSET; d.graphicsOffset.present = true;
    window.reload();
    if (!IsWindowEnabled(window.control(ProfileSourceWindow::GRAPHICS_ROUTE)) ||
        SendMessageW(window.control(ProfileSourceWindow::GRAPHICS_ROUTE), CB_GETCOUNT, 0, 0) != 4 ||
        SendMessageW(window.control(ProfileSourceWindow::MEMORY_ROUTE), CB_GETCURSEL, 0, 0) != 0) return 9391;
    const gc_u32 expected[] = {0, CLOCK_OFFSET_SURFACE_P0, CLOCK_OFFSET_SURFACE_GLOBAL, DESIRED_GRAPHICS_VF_COMPONENT};
    for (int choice = 1; choice <= 3; ++choice) {
        SendMessageW(window.control(ProfileSourceWindow::GRAPHICS_ROUTE), CB_SETCURSEL, choice, 0);
        SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(ProfileSourceWindow::GRAPHICS_ROUTE, CBN_SELCHANGE), 0);
        SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::CHECK, 0);
        if (host->graphics != expected[choice] || host->memory || d.graphicsOffset.offsetHz) return 9392;
    }
    select(window, point); SetWindowTextW(window.control(ProfileSourceWindow::INPUT), L"invalid");
    model->unsaved = true; host->discard = true;
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    if (window.handle() || host->confirms != 2 || host->closes != 2 || window.pending()) return 9347;
    // Capture is a temporary pane: it never stages/saves the hidden source's
    // invalid input, and independent omission/discard decisions gate acceptance.
    d = profile_document_make_empty(); d.domains = PROFILE_DOMAIN_MEMORY;
    d.hasMemoryOffset = true; d.memoryOffsetHz = -123500000; d.memorySurface = CLOCK_OFFSET_SURFACE_P0;
    model->saved = d; model->unsaved = false;
    if (!window.create(nullptr, 96, false)) return 9397;
    select(window, 3); SetWindowTextW(window.control(ProfileSourceWindow::INPUT), L"unfinished");
    const int savedCount = host->saves, checkedCount = host->checks;
    window.begin_capture(true);
    if (!window.capture_active() || visible(window.control(ProfileSourceWindow::INPUT)) ||
        !visible(window.control(ProfileSourceWindow::CAPTURE_READ)) || !window.pending() ||
        window.save() || host->saves != savedCount || text(window.control(ProfileSourceWindow::INPUT)) != L"unfinished") return 9398;
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::CHECK, 0);
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::STAGE, 0);
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::CAPTURE_ACCEPT, 0);
    if (host->checks != checkedCount || host->captureAccepts || d.memoryOffsetHz != -123500000) return 9399;
    for (int i = 0; i < 9; ++i) if (SendMessageW(window.control(ProfileSourceWindow::CAPTURE_DOMAIN_FIRST + i), BM_GETCHECK, 0, 0) !=
        (i < 5 ? BST_CHECKED : BST_UNCHECKED)) return 9400;
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::CAPTURE_READ, 0);
    if (host->captureReads != 1 || host->captureDomains != 31) return 9401;
    window.capture_report({"Snapshot", "Pin unavailable; settings omitted"}, true, PROFILE_DOMAIN_LOCK);
    RECT capturePlacement{0, 0, 320, 250};
    SendMessageW(window.handle(), WM_DPICHANGED, MAKEWPARAM(144, 144), (LPARAM)&capturePlacement);
    if (!window.capture_active() || !window.pending() || text(window.handle()) != L"Capture current GPU settings" ||
        text(window.control(ProfileSourceWindow::INPUT)) != L"unfinished" ||
        text(window.control(ProfileSourceWindow::REPORT)).find(L"Pin unavailable") == std::wstring::npos) return 9432;
    const HWND omissionChoice = window.control(ProfileSourceWindow::CAPTURE_ALLOW_OMISSIONS);
    SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(ProfileSourceWindow::CAPTURE_ALLOW_OMISSIONS, BN_SETFOCUS), (LPARAM)omissionChoice);
    RECT omissionRect{}; GetWindowRect(omissionChoice, &omissionRect);
    MapWindowPoints(nullptr, window.handle(), (POINT*)&omissionRect, 2); GetClientRect(window.handle(), &viewport);
    if (omissionRect.left < 0 || omissionRect.left >= viewport.right || omissionRect.top < 0 || omissionRect.bottom > viewport.bottom) return 9433;
    const auto mark = [&](int id, bool checked) {
        SendMessageW(window.control(id), BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)window.control(id));
    };
    if (IsWindowEnabled(window.control(ProfileSourceWindow::CAPTURE_ACCEPT))) return 9402;
    mark(ProfileSourceWindow::CAPTURE_ALLOW_OMISSIONS, true);
    if (IsWindowEnabled(window.control(ProfileSourceWindow::CAPTURE_ACCEPT))) return 9403;
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::CAPTURE_ACCEPT, 0);
    if (host->captureAccepts) return 9404;
    mark(ProfileSourceWindow::CAPTURE_DISCARD, true);
    if (!IsWindowEnabled(window.control(ProfileSourceWindow::CAPTURE_ACCEPT))) return 9405;
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::CAPTURE_ACCEPT, 0);
    if (host->captureAccepts != 1 || !host->partial || !host->discardCapture ||
        d.memoryOffsetHz != -123500000 || !window.pending() || profile_editor_document_dirty(*model)) return 9406;
    if (host->representation != PROFILE_CAPTURE_EXACT) return 9720;
    window.capture_report({"Conditional flatten; pin unknown"}, true, PROFILE_DOMAIN_LOCK,
        profile_capture_representation_bit(PROFILE_CAPTURE_EXACT) | profile_capture_representation_bit(PROFILE_CAPTURE_FLATTEN));
    const auto chooseRepresentation = [&](int choice) {
        SendMessageW(window.control(ProfileSourceWindow::CAPTURE_REPRESENTATION), CB_SETCURSEL, choice, 0);
        SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(ProfileSourceWindow::CAPTURE_REPRESENTATION, CBN_SELCHANGE),
            (LPARAM)window.control(ProfileSourceWindow::CAPTURE_REPRESENTATION));
    };
    chooseRepresentation(2);
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::CAPTURE_ACCEPT, 0);
    if (IsWindowEnabled(window.control(ProfileSourceWindow::CAPTURE_ACCEPT)) || host->captureAccepts != 1 ||
        text(window.control(ProfileSourceWindow::INPUT)) != L"unfinished") return 9721;
    chooseRepresentation(1);
    if (!IsWindowEnabled(window.control(ProfileSourceWindow::CAPTURE_ACCEPT)) || text(window.control(ProfileSourceWindow::CAPTURE_ACCEPT)) != L"Use flatten") return 9722;
    MSG captureKey{}; captureKey.hwnd = window.control(ProfileSourceWindow::CAPTURE_ACCEPT); captureKey.message = WM_KEYDOWN; captureKey.wParam = VK_RETURN;
    window.route(captureKey);
    if (host->captureAccepts != 2 || host->representation != PROFILE_CAPTURE_FLATTEN ||
        profile_editor_document_dirty(*model) || !window.pending()) return 9723;
    window.capture_report({"Representable flatten"}, true, PROFILE_DOMAIN_LOCK,
        profile_capture_representation_bit(PROFILE_CAPTURE_EXACT) | profile_capture_representation_bit(PROFILE_CAPTURE_FLATTEN), PROFILE_CAPTURE_FLATTEN);
    if (SendMessageW(window.control(ProfileSourceWindow::CAPTURE_REPRESENTATION), CB_GETCURSEL, 0, 0) != 1) return 9724;
    RECT representationPlacement{0, 0, 320, 250};
    SendMessageW(window.handle(), WM_DPICHANGED, MAKEWPARAM(192, 192), (LPARAM)&representationPlacement);
    if (SendMessageW(window.control(ProfileSourceWindow::CAPTURE_REPRESENTATION), CB_GETCURSEL, 0, 0) != 1 ||
        text(window.control(ProfileSourceWindow::INPUT)) != L"unfinished" || !window.pending()) return 9733;
    mark(ProfileSourceWindow::CAPTURE_DOMAIN_FIRST + 1, false);
    if (host->domainChanges != 1 || IsWindowEnabled(window.control(ProfileSourceWindow::CAPTURE_ACCEPT)) ||
        SendMessageW(window.control(ProfileSourceWindow::CAPTURE_ALLOW_OMISSIONS), BM_GETCHECK, 0, 0) != BST_UNCHECKED) return 9407;
    if (SendMessageW(window.control(ProfileSourceWindow::CAPTURE_REPRESENTATION), CB_GETCURSEL, 0, 0) != 0 ||
        text(window.control(ProfileSourceWindow::CAPTURE_ACCEPT)) != L"Use exact") return 9734;
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    if (!window.handle() || window.capture_active() || !window.pending() || host->captureCancels != 1 ||
        host->closes != 2 || host->confirms != 2 || !visible(window.control(ProfileSourceWindow::INPUT)) ||
        text(window.control(ProfileSourceWindow::INPUT)) != L"unfinished") return 9408;
    window.begin_capture(true);
    for (int i = 0; i < 9; ++i) mark(ProfileSourceWindow::CAPTURE_DOMAIN_FIRST + i, false);
    SendMessageW(window.handle(), WM_COMMAND, ProfileSourceWindow::CAPTURE_READ, 0);
    if (IsWindowEnabled(window.control(ProfileSourceWindow::CAPTURE_READ)) || host->captureReads != 1) return 9409;
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    // With no canonical source, cancel closes only the capture pane. It never
    // calls source.closed(), which would discard/rebase the separate live draft.
    model->valid = false; window.reload(); window.begin_capture(false);
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    if (window.handle() || host->captureCancels != 3 || host->closes != 2 || host->confirms != 2) return 9410;
    model->valid = true;
    if (!window.create(nullptr, 96, false)) return 9735;
    window.begin_capture(false);
    window.capture_report({"Representable flatten"}, true, 0,
        profile_capture_representation_bit(PROFILE_CAPTURE_EXACT) | profile_capture_representation_bit(PROFILE_CAPTURE_FLATTEN), PROFILE_CAPTURE_FLATTEN);
    MSG cancelKey{}; cancelKey.hwnd = window.control(ProfileSourceWindow::CLOSE); cancelKey.message = WM_KEYDOWN; cancelKey.wParam = VK_RETURN;
    window.route(cancelKey);
    if (window.capture_active() || host->captureAccepts != 2 || host->captureCancels != 4) return 9736;

    // UNIT-07 & CAP-10 ProfileSourceWindow controls: conversion combobox/button and restore exact button.
    auto flat = profile_document_make_empty();
    flat.domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK;
    flat.topologyCount = 2; flat.sourceTopologyComplete = true;
    flat.topology[0] = {10, 22, 662000, 1}; flat.topology[1] = {11, 23, 668250, 1};
    flat.points[0] = {PROFILE_POINT_CAPTURED_OFFSET, 0, 15000500, false, 0, true, 15000500};
    flat.points[1] = {PROFILE_POINT_ABSENT, 0, 0, false, 0, true, -25000500};
    flat.lock.present = true; flat.lock.mode = LOCK_MODE_FLATTEN;
    flat.lock.anchorTopologyId = 11; flat.lock.targetFrequencyHz = 1000000000;
    model->value = model->saved = flat; window.reload();
    if (!IsWindowEnabled(window.control(ProfileSourceWindow::RESTORE_EXACT)) ||
        IsWindowEnabled(window.control(ProfileSourceWindow::CONVERT_DOMAIN))) return 9411;
    SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(ProfileSourceWindow::RESTORE_EXACT, BN_CLICKED), 0);
    if (host->restores != 1) return 9412;
    model->value.points[1].hasObservedOffset = false; window.reload();
    if (IsWindowEnabled(window.control(ProfileSourceWindow::RESTORE_EXACT))) return 9413;
    auto plan = std::make_unique<ProfileConversionPlan>(); plan->count = 2;
    plan->items[0].kind = PROFILE_CONVERT_MEMORY; plan->items[0].convertible = true;
    plan->items[0].batch.affectedCount = 1; plan->items[0].batch.maximumError = 500;
    plan->items[1].kind = PROFILE_CONVERT_XBAR_VOLTAGE; plan->items[1].unresolved = true;
    snprintf(plan->items[1].why, sizeof(plan->items[1].why), "voltage conversion needs per-domain review");
    window.report({"Check output"}); window.conversion_options(*plan);
    if (!IsWindowEnabled(window.control(ProfileSourceWindow::CONVERSION_CHOICE)) ||
        !IsWindowEnabled(window.control(ProfileSourceWindow::CONVERT_DOMAIN))) return 9414;
    SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(ProfileSourceWindow::CONVERT_DOMAIN, BN_CLICKED), 0);
    if (host->converts != 1 || host->lastConvertKind != PROFILE_CONVERT_MEMORY) return 9415;
    SendMessageW(window.control(ProfileSourceWindow::CONVERSION_CHOICE), CB_SETCURSEL, 1, 0);
    SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(ProfileSourceWindow::CONVERSION_CHOICE, CBN_SELCHANGE), 0);
    if (IsWindowEnabled(window.control(ProfileSourceWindow::CONVERT_DOMAIN)) ||
        text(window.control(ProfileSourceWindow::STATUS)).find(L"voltage") == std::wstring::npos) return 9416;

    // HELP-01..08 ProfileSourceWindow controls: ASSIST_CHOICE + PREPARE_CURVE enabled on report & clear on invalidation.
    if (IsWindowEnabled(window.control(ProfileSourceWindow::ASSIST_CHOICE)) ||
        IsWindowEnabled(window.control(ProfileSourceWindow::PREPARE_CURVE))) return 9850;
    window.report({"Curve locked off-grid"}); window.assist_ready(true);
    if (!IsWindowEnabled(window.control(ProfileSourceWindow::ASSIST_CHOICE)) ||
        !IsWindowEnabled(window.control(ProfileSourceWindow::PREPARE_CURVE))) return 9851;
    SendMessageW(window.control(ProfileSourceWindow::ASSIST_CHOICE), CB_SETCURSEL, PROFILE_ASSIST_COMBINED, 0);
    SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(ProfileSourceWindow::PREPARE_CURVE, BN_CLICKED), 0);
    if (host->prepares != 1 || host->lastPrepareMode != PROFILE_ASSIST_COMBINED) return 9852;
    window.clear_report();
    if (IsWindowEnabled(window.control(ProfileSourceWindow::ASSIST_CHOICE)) ||
        IsWindowEnabled(window.control(ProfileSourceWindow::PREPARE_CURVE))) return 9853;
    return 0;
}
#else
int run_profile_source_window_tests() { return 0; }
#endif
