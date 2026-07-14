// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static void refresh_live_fan_telemetry(bool redrawControls) {
    if (!g_app.isServiceProcess) {
        if (g_app.applyInFlight) {
            debug_log_on_change("fan telemetry: deferred while the GUI owns an active GPU mutation\n");
            sync_fan_ui_from_cached_state(redrawControls);
            return;
        }
        bool wasAvailable = g_app.backgroundServiceAvailable;
        char detail[128] = {};
        ServiceSnapshot snapshot = {};
        if (!wasAvailable) {
            // Service was known to be down: do a lightweight health check.
            if (!refresh_background_service_state()) {
                sync_fan_ui_from_cached_state(redrawControls);
                return;
            }
            // Service is back: fetch a full snapshot and update everything.
            char snapDetail[256] = {};
            if (!service_client_get_snapshot(&snapshot, snapDetail, sizeof(snapDetail))) {
                sync_fan_ui_from_cached_state(redrawControls);
                update_all_gui_for_service_state();
                return;
            }
            apply_service_snapshot_to_app(&snapshot);
            sync_fan_ui_from_cached_state(redrawControls);
            update_all_gui_for_service_state();
            return;
        }
        if (!service_client_get_telemetry(&snapshot, detail, sizeof(detail))) {
            sync_fan_ui_from_cached_state(redrawControls);
            if (!g_app.backgroundServiceAvailable) update_all_gui_for_service_state();
            return;
        }
        apply_service_snapshot_to_app(&snapshot);
        sync_fan_ui_from_cached_state(redrawControls);
        // Routine telemetry must not overwrite in-progress VF edits. A first
        // control build or authoritative reset is the narrow full-refresh case.
        if ((g_app.numVisible > 0 && !g_app.hEditsMhz[0]) || g_guiForceFullRefresh) {
            g_guiForceFullRefresh = false;
            update_all_gui_for_service_state();
        }
        return;
    }
    char detail[128] = {};
    if (!nvml_read_fans(detail, sizeof(detail))) return;
    sync_fan_ui_from_cached_state(redrawControls);
}
