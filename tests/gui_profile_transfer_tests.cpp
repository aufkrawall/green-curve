// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#include "profile_transfer_window.h"
#include "profile_transfer_picker.h"
#include "gui_profile_transfer_work.h"
#include "app_shared.h"
#include <map>
#include <cstring>
namespace gui_profile_transfer_fixture {
static AppData g_app{};
static int g_dpi = 96, discards = 0, refreshes = 0;
static bool sourceOpen = true;
struct SourceWindow {
    HWND window = nullptr;
    bool inputPending = true;
    int reloads = 0;
    std::string input = "unfinished-";
    HWND handle() const { return window; }
    bool pending() const { return inputPending; }
    void reload() {
        ++reloads;
        inputPending = false;
        input.clear();
    }
    void show() {}
};
static std::unique_ptr<SourceWindow> g_profileSourceWindow;
struct Window : ::ProfileTransferWindow {
    using ::ProfileTransferWindow::ProfileTransferWindow;
    bool create(HWND owner, int dpi, int slot, bool dirty) {
        return ::ProfileTransferWindow::create(owner, dpi, slot, dirty, false);
    }
};
using ProfileTransferWindow = Window;
static std::unique_ptr<GuiProfileTransferWork> queued;
static std::unique_ptr<GuiProfileTransferWork> finished;
static void debug_log(const char *, ...) {}
static void set_profile_status_text(const char *, ...) {}
static bool gui_profile_capture_active() { return false; }
static void gui_profile_source_refresh() { ++refreshes; }
static void gui_profile_source_invalidate_review(const char *) {}
static bool gui_profile_source_open() { return sourceOpen; }
static int selected_profile_slot_from_combo() {
    return g_app.hProfileCombo ? (int)SendMessageW(g_app.hProfileCombo, CB_GETCURSEL, 0, 0) + 1 : 4;
}
static void invalidate_tray_profile_cache() {}
static void refresh_profile_controls_from_config() {
    if (g_app.hProfileCombo)
        SendMessageW(g_app.hProfileCombo, CB_SETCURSEL, 0, 0);
}
static void apply_system_titlebar_theme(HWND) {}
static void gui_draft_discard() {
    ++discards;
    g_app.guiStateDirty = false;
    g_app.guiHasUserModifiedValues = false;
}
static bool fixture_choose_file(HWND, bool, std::string &path, char *, size_t) {
    path = "input.gcprofile";
    return true;
}
static bool gui_service_io_queue_transfer(std::unique_ptr<GuiProfileTransferWork> &work, char *error, size_t size) {
    if (queued || finished) {
        std::snprintf(error, size, "fixture queue occupied");
        return false;
    }
    queued = std::move(work);
    return true;
}
static void gui_service_io_cancel_transfer(gc_u64 token) { (void)gui_profile_transfer_cancel_pending(queued, token); }
static std::unique_ptr<GuiProfileTransferWork> gui_service_io_take_transfer() { return std::move(finished); }
#define profile_transfer_choose_file fixture_choose_file
#include "gui_profile_transfer.cpp"
#undef profile_transfer_choose_file
struct IO {
    std::map<std::string, std::string> files;
    int writes = 0, commits = 0;
    bool read(const std::string &path, std::string &bytes, size_t maximum, bool missing, ProfileCodecError &error) {
        const auto it = files.find(path);
        if (it == files.end()) {
            bytes.clear();
            return missing || profile_codec::fail(&error, 0, "missing fixture file");
        }
        bytes = it->second;
        return bytes.size() <= maximum;
    }
    bool distinct(const std::string &a, const std::string &b, ProfileCodecError &) { return a != b; }
    bool legacy(const std::string &bytes, int slot, ProfileDocument &d, bool &found, ProfileCodecError &error) {
        return profile_legacy_native_slot_decode(bytes, PROFILE_LEGACY_WINDOWS_INI, slot, d, found, &error);
    }
    bool write(const std::string &path, const std::string &bytes, ProfileCodecError &) {
        ++writes;
        files[path] = bytes;
        return true;
    }
    bool commit(const std::string &path, const ProfileBankEdit &edit, ProfileCodecError &error) {
        ++commits;
        return profile_bank_commit_edit(
            edit,
            [&](std::string &bytes) {
                bytes = files[path];
                return true;
            },
            [&](const std::string &bytes) { return write(path, bytes, error); }, &error);
    }
};
static int run() {
    struct Cleanup {
        ~Cleanup() {
            g_profileTransferWindow.reset();
            queued.reset();
            finished.reset();
            g_profileTransferReview.reset();
            if (g_profileSourceWindow && g_profileSourceWindow->window)
                DestroyWindow(g_profileSourceWindow->window);
            g_profileSourceWindow.reset();
            if (g_app.hMainWnd)
                DestroyWindow(g_app.hMainWnd);
            g_app.hMainWnd = nullptr;
        }
    } cleanup;
    g_app.hMainWnd = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
    g_profileSourceWindow = std::make_unique<SourceWindow>();
    g_profileSourceWindow->window = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr,
                                                    GetModuleHandleW(nullptr), nullptr);
    if (!g_app.hMainWnd || !g_profileSourceWindow->window)
        return 9510;
    g_app.hProfileCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST, 0, 0, 100, 100,
                                          g_app.hMainWnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    for (int i = 0; i < 5; ++i)
        SendMessageW(g_app.hProfileCombo, CB_ADDSTRING, 0, (LPARAM)L"Slot");
    SendMessageW(g_app.hProfileCombo, CB_SETCURSEL, 3, 0);
    std::strcpy(g_app.configPath, "bank.ini");
    g_app.guiSource.valid = true;
    g_app.guiSource.unsaved = true;
    g_app.guiSource.value = profile_document_make_empty();
    g_app.guiSource.value.domains = PROFILE_DOMAIN_MEMORY;
    g_app.guiSource.value.hasMemoryOffset = true;
    g_app.guiSource.value.memoryOffsetHz = 17000000;
    const auto old = g_app.guiSource.value;
    IO io;
    io.files["bank.ini"] = "[profiles]\napp_launch_slot=5\n[preferences]\nkept=yes\n";
    auto bank = std::make_unique<ProfileBank>();
    bank->count = 5;
    for (int i = 0; i < 5; ++i) {
        auto &d = bank->documents[i];
        d = profile_document_make_empty();
        d.documentId = i + 1;
        std::snprintf(d.displayName, sizeof(d.displayName), "Transfer %d", i + 1);
        d.domains = PROFILE_DOMAIN_MEMORY;
        d.hasMemoryOffset = true;
        d.memoryOffsetHz = -123500000 + i;
        d.memorySurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    }
    std::string bytes;
    if (!profile_bank_encode(*bank, bytes))
        return 9511;
    io.files["input.gcprofile"] = bytes;
    gui_profile_transfer_begin();
    if (!gui_profile_transfer_active() || IsWindowEnabled(g_app.hMainWnd) ||
        IsWindowEnabled(g_profileSourceWindow->window) || g_profileSourceWindow->input != "unfinished-" || discards)
        return 9512;
    auto &pane = *g_profileTransferWindow;
    const auto click = [&](int id) {
        SendMessageW(pane.handle(), WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)pane.control(id));
    };
    const auto select = [&](int id, int index) {
        SendMessageW(pane.control(id), CB_SETCURSEL, index, 0);
        SendMessageW(pane.handle(), WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), (LPARAM)pane.control(id));
    };
    const auto deliver = [&](bool notify = true) {
        auto work = std::move(queued);
        if (work) {
            gui_profile_transfer_execute(*work, io);
            finished = std::move(work);
            if (notify)
                gui_profile_transfer_drain();
        }
    };
    if (GetFocus() != pane.control(Window::CLOSE))
        return 9513;
    MSG enter{};
    enter.message = WM_KEYDOWN;
    enter.wParam = VK_RETURN;
    enter.hwnd = pane.control(Window::CLOSE);
    if (!pane.route(enter) || gui_profile_transfer_active() || !IsWindowEnabled(g_app.hMainWnd))
        return 9538;
    gui_profile_transfer_begin();
    select(Window::MODE, 1);
    if (SendMessageW(pane.control(Window::SLOT), CB_GETCURSEL, 0, 0) != 0)
        return 9514;
    click(Window::BROWSE);
    select(Window::SLOT, 5);
    select(Window::SOURCE, 2);
    SetFocus(pane.control(Window::PATH));
    enter.hwnd = pane.control(Window::PATH);
    (void)pane.route(enter);
    if (queued)
        return 9539;
    SetFocus(pane.control(Window::PREVIEW));
    enter.hwnd = pane.control(Window::PREVIEW);
    (void)pane.route(enter);
    if (!queued || !pane.busy() || queued->committing || queued->options.slot != 5 ||
        queued->options.sourceRecord != 3 || io.writes)
        return 9515;
    SendMessageW(pane.handle(), WM_COMMAND, MAKEWPARAM(Window::CLOSE, BN_SETFOCUS),
                 (LPARAM)pane.control(Window::CLOSE));
    if (!gui_profile_transfer_active() || !pane.busy() || !queued)
        return 9540;
    auto stale = std::move(queued);
    click(Window::CLOSE);
    if (gui_profile_transfer_active() || !IsWindowEnabled(g_app.hMainWnd) ||
        !IsWindowEnabled(g_profileSourceWindow->window))
        return 9516;
    if (io.writes || discards || !profile_document_semantically_equal(old, g_app.guiSource.value))
        return 9517;
    gui_profile_transfer_begin();
    select(Window::MODE, 1);
    click(Window::BROWSE);
    select(Window::SLOT, 5);
    select(Window::SOURCE, 2);
    click(Window::PREVIEW);
    gui_profile_transfer_execute(*stale, io);
    finished = std::move(stale);
    gui_profile_transfer_drain();
    if (!pane.busy() || !queued || g_profileTransferReview)
        return 9532;
    deliver();
    if (pane.busy() || !g_profileTransferReview || !g_profileTransferReview->preview->ready ||
        !IsWindowEnabled(pane.control(Window::COMMIT)) ||
        SendMessageW(pane.control(Window::LOAD), BM_GETCHECK, 0, 0) != BST_UNCHECKED)
        return 9518;
    click(Window::COMMIT);
    if (!queued || !pane.committing() || IsWindowEnabled(pane.control(Window::CLOSE)))
        return 9519;
    SendMessageW(pane.handle(), WM_CLOSE, 0, 0);
    if (!gui_profile_transfer_active())
        return 9520;
    deliver(false);
    if (!pane.committing() || !finished || queued || !gui_profile_transfer_active())
        return 9541;
    gui_profile_transfer_drain();
    if (gui_profile_transfer_active() || io.writes != 1 || io.commits != 1 || discards ||
        g_profileSourceWindow->input != "unfinished-" ||
        !profile_document_semantically_equal(old, g_app.guiSource.value) || selected_profile_slot_from_combo() != 4)
        return 9521;
    gui_profile_transfer_begin();
    select(Window::MODE, 1);
    click(Window::BROWSE);
    select(Window::KIND, 1);
    click(Window::PREVIEW);
    deliver();
    SendMessageW(pane.control(Window::LOAD), BM_SETCHECK, BST_CHECKED, 0);
    click(Window::LOAD);
    if (IsWindowEnabled(pane.control(Window::COMMIT)))
        return 9522;
    gui_profile_transfer_commit(nullptr, true, false);
    if (queued || discards)
        return 9523;
    SendMessageW(pane.control(Window::DISCARD), BM_SETCHECK, BST_CHECKED, 0);
    click(Window::DISCARD);
    click(Window::COMMIT);
    deliver();
    if (gui_profile_transfer_active() || discards != 1 || !g_app.guiSource.valid || g_app.guiSource.unsaved ||
        g_app.guiSource.value.memoryOffsetHz != -123500000 || g_profileSourceWindow->reloads != 1)
        return 9524;
    gui_profile_transfer_begin();
    select(Window::MODE, 1);
    click(Window::BROWSE);
    select(Window::SLOT, 2);
    click(Window::PREVIEW);
    deliver();
    io.files["bank.ini"] += "\n[concurrent]\nkept=yes\n";
    const auto concurrent = io.files["bank.ini"];
    SendMessageW(pane.control(Window::LOAD), BM_SETCHECK, BST_UNCHECKED, 0);
    click(Window::LOAD);
    click(Window::COMMIT);
    deliver();
    if (!gui_profile_transfer_active() || pane.busy() || IsWindowEnabled(pane.control(Window::COMMIT)) ||
        io.files["bank.ini"] != concurrent)
        return 9525;
    click(Window::BACK);
    select(Window::MODE, 0);
    SetWindowTextW(pane.control(Window::PATH), L"output.gcprofile");
    click(Window::PREVIEW);
    deliver();
    if (!g_profileTransferReview || !g_profileTransferReview->preview->ready ||
        g_profileTransferReview->preview->exportBytes.empty())
        return 9533;
    const auto frozenExport = g_profileTransferReview->preview->exportBytes;
    io.files["bank.ini"] += "\n[new-preference]\nkept=yes\n";
    click(Window::COMMIT);
    deliver();
    if (gui_profile_transfer_active() || io.files["output.gcprofile"] != frozenExport || discards != 1)
        return 9534;
    g_app.guiSource.unsaved = true;
    g_profileSourceWindow->input = "still unfinished";
    g_profileSourceWindow->inputPending = true;
    const auto beforeOpenFailure = g_app.guiSource.value;
    gui_profile_transfer_begin();
    select(Window::MODE, 1);
    select(Window::SLOT, 3);
    click(Window::BROWSE);
    click(Window::PREVIEW);
    deliver();
    SendMessageW(pane.control(Window::LOAD), BM_SETCHECK, BST_CHECKED, 0);
    click(Window::LOAD);
    SendMessageW(pane.control(Window::DISCARD), BM_SETCHECK, BST_CHECKED, 0);
    click(Window::DISCARD);
    sourceOpen = false;
    click(Window::COMMIT);
    deliver();
    sourceOpen = true;
    if (gui_profile_transfer_active() || discards != 1 || !g_app.guiSource.unsaved ||
        !profile_document_semantically_equal(beforeOpenFailure, g_app.guiSource.value) ||
        g_profileSourceWindow->input != "still unfinished" || !IsWindowEnabled(g_app.hMainWnd))
        return 9537;
    gui_profile_transfer_begin();
    select(Window::MODE, 1);
    select(Window::SLOT, 2);
    SetWindowTextW(pane.control(Window::PATH), L"\x66f2\x7ebf file.gcprofile");
    SetFocus(pane.control(Window::PATH));
    SendMessageW(pane.control(Window::PATH), EM_SETSEL, 1, 3);
    RECT bounds{0, 0, 600, 450};
    SendMessageW(pane.handle(), WM_DPICHANGED, MAKELONG(144, 144), (LPARAM)&bounds);
    wchar_t text[64]{};
    GetWindowTextW(pane.control(Window::PATH), text, 64);
    DWORD start = 0, end = 0;
    SendMessageW(pane.control(Window::PATH), EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    if (std::wcscmp(text, L"\x66f2\x7ebf file.gcprofile") || start != 1 || end != 3)
        return 9526;
    RECT field{}, client{};
    GetWindowRect(pane.control(Window::PATH), &field);
    MapWindowPoints(nullptr, pane.handle(), (POINT *)&field, 2);
    GetClientRect(pane.handle(), &client);
    if (field.left < 0 || field.top < 0 || field.bottom > client.bottom)
        return 9531;
    click(Window::PREVIEW);
    if (!queued || queued->file != "\xe6\x9b\xb2\xe7\xba\xbf file.gcprofile")
        return 9527;
    click(Window::CLOSE);
    if (queued || gui_profile_transfer_active() || !IsWindowEnabled(g_app.hMainWnd))
        return 9528;
    auto pending = std::make_unique<GuiProfileTransferWork>();
    pending->token = 17;
    pending->committing = true;
    if (gui_profile_transfer_cancel_pending(pending, 17) || !pending)
        return 9529;
    pending->committing = false;
    if (gui_profile_transfer_cancel_pending(pending, 16) || !pending ||
        !gui_profile_transfer_cancel_pending(pending, 17) || pending)
        return 9530;
    gui_profile_transfer_begin();
    SetFocus(pane.control(Window::CLOSE));
    SendMessageW(pane.handle(), WM_DPICHANGED, MAKELONG(192, 192), (LPARAM)&bounds);
    GetWindowRect(pane.control(Window::CLOSE), &field);
    MapWindowPoints(nullptr, pane.handle(), (POINT *)&field, 2);
    GetClientRect(pane.handle(), &client);
    if (field.left < 0 || field.top < 0 || field.right > client.right || field.bottom > client.bottom)
        return 9535;
    click(Window::CLOSE);
    return 0;
}
} // namespace gui_profile_transfer_fixture
int run_gui_profile_transfer_tests() { return gui_profile_transfer_fixture::run(); }
#else
int run_gui_profile_transfer_tests() { return 0; }
#endif
