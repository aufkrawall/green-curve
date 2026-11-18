// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Terminal GUI-process cleanup shared by normal WM_QUIT and the pre-show
// unsupported-GPU warning exit. The latter runs before the I/O coordinator is
// started, while normal shutdown gives it a bounded cancellation grace period.
static void cleanup_gui_process_runtime(bool coordinatorStarted) {
    bool coordinatorStopped =
        !coordinatorStarted || gui_mutation_shutdown();
    remove_tray_icon();
    release_single_instance_mutex();
    close_nvml();
    if (g_app.hNvApi) {
        FreeLibrary(g_app.hNvApi);
        g_app.hNvApi = nullptr;
    }
    for (int i = 0; i < 4; ++i) {
        if (!g_app.trayIcons[i]) continue;
        DestroyIcon(g_app.trayIcons[i]);
        g_app.trayIcons[i] = nullptr;
    }
    if (g_app.hWindowClassBrush) {
        DeleteObject(g_app.hWindowClassBrush);
        g_app.hWindowClassBrush = nullptr;
    }
    if (s_hUiFont) {
        DeleteObject(s_hUiFont);
        s_hUiFont = nullptr;
    }
    if (coordinatorStopped) {
        // Drains and joins the log writer so the last lines of the session reach
        // disk.  That is the last thing worth WAITING for at exit.
        debug_log_writer_stop();
    } else {
        debug_log("GUI shutdown: the I/O coordinator did not stop in time; "
                  "the session's final log lines may be lost\n");
    }
    // The process-lifetime critical sections are deliberately NOT deleted.
    //
    // gui_mutation_shutdown() gates exactly one worker: the mutation
    // coordinator.  Two others are detached and never joined -- the Updates
    // dialog's command worker and the logon/tray startup-sync thread -- and
    // both call debug_log() and enter_config_storage_lock() after their work
    // completes.  Deleting a CRITICAL_SECTION while a thread can still call
    // EnterCriticalSection on it is a heap use-after-free in the exiting
    // process, and a straggler is exactly what the `else` branch above exists to
    // tolerate.  Windows reclaims all of it at exit, and the debug-log writer's
    // own event is already treated this way (main_debug_log_writer.cpp), so this
    // makes the three locks consistent with the object they were meant to be.
}
