// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static void refresh_live_fan_telemetry(bool redrawControls) {
    if (!g_app.isServiceProcess) {
#ifndef GREEN_CURVE_SERVICE_BINARY
        if (g_app.applyInFlight) {
            debug_log_on_change("fan telemetry: deferred while the GUI owns an active GPU mutation\n");
            return;
        }
        if (gui_service_model_ready(&g_app.guiServiceModel))
            gui_service_io_queue_telemetry(redrawControls);
        else
            gui_service_io_queue_full_sync("telemetry reconnect backstop");
#endif
        return;
    }
    char detail[128] = {};
    if (!nvml_read_fans(detail, sizeof(detail))) return;
    sync_fan_ui_from_cached_state(redrawControls);
}
