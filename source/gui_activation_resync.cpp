// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Re-read live GPU state when the user switches back to the window, so clock
// settings changed by another tool show up without pressing Refresh.  Event
// driven, never timed: see gui_activation_resync_policy.h for why nothing here
// may run while the window is in the tray or behind another application.
// Included from ui_main.cpp after gui_service_state.cpp and
// gui_mutation_worker.cpp, whose entry points it uses.

#include "gui_activation_resync_policy.h"

static ULONGLONG g_guiActivationResyncLastQueuedMs = 0;

static void gui_activation_resync_on_app_activated() {
    HWND hwnd = g_app.hMainWnd;
    GuiActivationResyncInput in;
    in.serviceClient = g_app.usingBackgroundService && !app_is_service_process();
    in.windowShown = hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd) &&
        !g_app.trayWindowHiddenIntent;
    // A modal dialog disables its owner for as long as it is up.
    in.modalActive = hwnd && !IsWindowEnabled(hwnd);
    in.modelReady = gui_service_model_ready(&g_app.guiServiceModel);
    in.liveAuthority = g_app.serviceSnapshotAuthoritative && g_app.loaded;
    in.serviceWorkOutstanding = gui_service_io_work_outstanding();
    in.draftDetached = g_app.guiDraft.detached && gui_state_dirty();
    in.nowMs = GetTickCount64();
    in.lastQueuedMs = g_guiActivationResyncLastQueuedMs;
    const GuiActivationResyncDecision decision = gui_activation_resync_decide(in);
    if (decision != GUI_ACTIVATION_RESYNC_QUEUE) {
        // Repeated identical skips (alt-tabbing while hidden or busy) log once.
        debug_log_on_change("GUI activation resync: %s (shown=%d modal=%d ready=%d"
                            " authoritative=%d busy=%d detachedDraft=%d)\n",
            gui_activation_resync_decision_name(decision),
            in.windowShown ? 1 : 0, in.modalActive ? 1 : 0, in.modelReady ? 1 : 0,
            in.liveAuthority ? 1 : 0, in.serviceWorkOutstanding ? 1 : 0,
            in.draftDetached ? 1 : 0);
        return;
    }
    g_guiActivationResyncLastQueuedMs = in.nowMs;
    debug_log("GUI activation resync: window activated; re-reading live GPU state"
              " (sinceLastMs=%llu dirty=%d)\n",
        in.lastQueuedMs ? (unsigned long long)(in.nowMs - in.lastQueuedMs) : 0ULL,
        gui_state_dirty() ? 1 : 0);
    // READY with live authority was checked above, so this is the
    // presentation-preserving read; no status line, unlike manual Refresh.
    gui_service_request_resync("window activation", false);
}
