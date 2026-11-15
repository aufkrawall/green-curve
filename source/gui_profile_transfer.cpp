// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_transfer_window.h"
#include "profile_transfer_picker.h"

static std::unique_ptr<ProfileTransferWindow> g_profileTransferWindow;
static std::unique_ptr<GuiProfileTransferWork> g_profileTransferReview;
static gc_u64 g_profileTransferToken = 0, g_profileTransferPendingToken = 0;
static bool g_profileTransferLoad = false, g_profileTransferSourceWasEnabled = false;
static bool gui_profile_transfer_active() { return g_profileTransferWindow && g_profileTransferWindow->handle(); }
static bool gui_profile_transfer_dirty() {
    return g_app.guiSource.valid ? profile_editor_document_dirty(g_app.guiSource) ||
        (g_profileSourceWindow && g_profileSourceWindow->pending()) : g_app.guiStateDirty || g_app.guiHasUserModifiedValues;
}
static void gui_profile_transfer_restore_parent() {
    EnableWindow(g_app.hMainWnd, TRUE);
    if (g_profileSourceWindow && g_profileSourceWindow->handle() && g_profileTransferSourceWasEnabled)
        EnableWindow(g_profileSourceWindow->handle(), TRUE);
    g_profileTransferSourceWasEnabled = false;
}
static void gui_profile_transfer_cancel(void*) {
    gui_service_io_cancel_transfer(g_profileTransferPendingToken);
    g_profileTransferPendingToken = 0; g_profileTransferReview.reset();
    gui_profile_transfer_restore_parent();
    set_profile_status_text("Transfer cancelled. The previous draft and unfinished input were kept.");
    debug_log("profile-import: closed\n");
}
static bool gui_profile_transfer_choose(void*, bool importing, std::string& path, char* error, size_t size) {
    return profile_transfer_choose_file(g_profileTransferWindow->handle(), importing, path, error, size);
}
static void gui_profile_transfer_prepare(void*, const ProfileEditorTransferOptions& options, const std::string& path) {
    if (!gui_profile_transfer_active() || g_profileTransferWindow->busy()) return;
    if (g_profileTransferToken == ~(gc_u64)0) { g_profileTransferWindow->notice("Reopen the application before transferring again"); return; }
    auto work = std::make_unique<GuiProfileTransferWork>(); work->token = ++g_profileTransferToken;
    work->options = options; work->file = path; work->config = g_app.configPath;
    char error[256]{}; const auto token = work->token;
    g_profileTransferReview.reset();
    if (!gui_service_io_queue_transfer(work, error, sizeof(error))) { g_profileTransferWindow->notice(error); return; }
    g_profileTransferPendingToken = token; g_profileTransferWindow->set_busy(false);
}
static void gui_profile_transfer_commit(void*, bool load, bool discard) {
    if (!gui_profile_transfer_active() || g_profileTransferWindow->busy() || !g_profileTransferReview || !g_profileTransferReview->preview ||
        !g_profileTransferReview->preview->ready) return;
    if (load && gui_profile_transfer_dirty() && !discard) { g_profileTransferWindow->require_discard(); return; }
    if (g_profileTransferToken == ~(gc_u64)0) { g_profileTransferWindow->notice("Reopen the application before transferring again"); return; }
    auto& work = g_profileTransferReview; work->token = ++g_profileTransferToken; work->committing = true;
    char error[256]{}; const auto token = work->token;
    if (!gui_service_io_queue_transfer(work, error, sizeof(error))) { work->committing = false; g_profileTransferWindow->notice(error); return; }
    g_profileTransferLoad = load; g_profileTransferPendingToken = token; g_profileTransferWindow->set_busy(true);
}
static void gui_profile_transfer_completed(std::unique_ptr<GuiProfileTransferWork> work) {
    if (!work) return;
    if (!gui_profile_transfer_active() || work->token != g_profileTransferPendingToken) {
        debug_log("profile-import: stale result discarded token=%llu\n", (unsigned long long)work->token); return;
    }
    g_profileTransferPendingToken = 0;
    if (!work->committing) {
        if (work->preview) {
            work->preview->lines.insert(work->preview->lines.begin(), std::string(work->options.importing ? "Read from: " : "Write to (replaces existing file): ") + work->file);
            g_profileTransferWindow->report(work->preview->lines, work->success, gui_profile_transfer_dirty());
        } else { g_profileTransferWindow->queue_failed(); g_profileTransferWindow->notice("File preview failed; previous draft kept"); }
        g_profileTransferReview = std::move(work); return;
    }
    if (!work->success) {
        work->preview->lines.emplace_back(std::string("Transfer failed: ") + work->preview->error.message);
        g_profileTransferWindow->report(work->preview->lines, false, gui_profile_transfer_dirty());
        g_profileTransferReview = std::move(work); return;
    }
    bool loaded = false;
    if (work->options.importing) {
        const int selectedSlot = selected_profile_slot_from_combo();
        invalidate_tray_profile_cache(); refresh_profile_controls_from_config();
        if (g_app.hProfileCombo) SendMessageW(g_app.hProfileCombo, CB_SETCURSEL, selectedSlot - 1, 0);
        if (g_profileTransferLoad && work->preview && !work->preview->documents.empty() && gui_profile_source_open()) {
            auto candidate = std::make_unique<ProfileEditorCandidate>();
            profile_editor_source_candidate(work->preview->documents.front(), *candidate);
            gui_profile_source_invalidate_review("Imported source loaded; check the destination before Apply");
            gui_draft_discard(); g_app.guiSource = candidate->source;
            if (g_app.hProfileCombo) SendMessageW(g_app.hProfileCombo, CB_SETCURSEL, work->preview->edit.destinations[0] - 1, 0);
            g_profileSourceWindow->reload(); g_profileSourceWindow->show(); loaded = true;
        }
    }
    gui_profile_transfer_restore_parent(); DestroyWindow(g_profileTransferWindow->handle());
    gui_profile_source_refresh();
    set_profile_status_text("%s. %s", work->options.importing ? "Imported reviewed profiles atomically" : "Exported reviewed saved profiles",
        loaded ? "First imported source loaded; Apply requires destination review." :
        work->options.importing && g_profileTransferLoad ? "Could not open the source editor; the previous draft was kept." : "The editor draft was kept.");
}
static void gui_profile_transfer_drain() {
    auto work = gui_service_io_take_transfer();
    if (work) gui_profile_transfer_completed(std::move(work));
}
static void gui_profile_transfer_begin() {
    if (gui_profile_transfer_active()) { ShowWindow(g_profileTransferWindow->handle(), SW_SHOWNORMAL); return; }
    if (gui_profile_capture_active() || g_app.applyInFlight) { set_profile_status_text("Finish the pending capture or GPU operation before transferring profiles."); return; }
    if (!g_profileTransferWindow) {
        ProfileTransferWindowCallbacks callbacks{};
        callbacks.choose = gui_profile_transfer_choose; callbacks.prepare = gui_profile_transfer_prepare;
        callbacks.commit = gui_profile_transfer_commit; callbacks.cancel = gui_profile_transfer_cancel; callbacks.created = apply_system_titlebar_theme;
        g_profileTransferWindow = std::make_unique<ProfileTransferWindow>(callbacks);
    }
    if (!g_profileTransferWindow->create(g_app.hMainWnd, g_dpi, selected_profile_slot_from_combo(), gui_profile_transfer_dirty())) {
        set_profile_status_text("Could not open profile transfer; previous draft kept."); return;
    }
    gui_profile_source_invalidate_review("Transfer opened; check the destination again before Apply");
    g_profileTransferSourceWasEnabled = g_profileSourceWindow && g_profileSourceWindow->handle() && IsWindowEnabled(g_profileSourceWindow->handle());
    if (g_profileTransferSourceWasEnabled) EnableWindow(g_profileSourceWindow->handle(), FALSE);
    EnableWindow(g_app.hMainWnd, FALSE); debug_log("profile-import: opened\n");
}
