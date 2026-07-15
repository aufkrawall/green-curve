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
        close_debug_log_file();
        DeleteCriticalSection(&g_configLock);
        DeleteCriticalSection(&g_appLock);
        DeleteCriticalSection(&g_debugLogLock);
    } else {
        // Windows reclaims these process-lifetime resources on exit. Keep them
        // valid for a bounded pipe operation that did not observe cancellation.
        debug_log("GUI shutdown: retaining process-lifetime synchronization for the exiting service I/O coordinator\n");
    }
}
