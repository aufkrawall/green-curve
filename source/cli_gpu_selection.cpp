// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// CLI half of the client/service GPU selection agreement
// (gpu_selection_sync_policy.h). The CLI has no window to keep in step, so
// before a config-driven action it asks the service once, synchronously, to
// show the saved GPU, adopts that snapshot, and then validates exactly as the
// GUI does. Included by main.cpp ahead of entry.cpp.

static bool validate_configured_gpu_selection_for_cli(char* err,
    size_t errSize) {
    if (err && errSize) err[0] = 0;
    unsigned int resolved = 0;
    ConfiguredGpuSelection configured = {};
    char loadErr[256] = {};
    bool needsSwitch = false;
    if (g_app.backgroundServiceAvailable && g_app.serviceSnapshotAuthoritative &&
        g_app.adapterCount > 1 &&
        load_configured_gpu_selection(g_app.configPath, &configured, loadErr,
            sizeof(loadErr))) {
        resolved = configured.legacyIndex;
        needsSwitch = resolve_configured_gpu_selection(&configured,
                g_app.adapters, g_app.adapterCount, &resolved) ==
                CONFIGURED_GPU_RESOLVE_STABLE &&
            resolved < g_app.adapterCount && resolved < MAX_GPU_ADAPTERS &&
            resolved != g_app.selectedGpuIndex;
    }
    if (needsSwitch) {
        ServiceRequest request = {};
        ServiceResponse response = {};
        char readErr[256] = {};
        bool read = service_client_build_state_request(SERVICE_CMD_GET_SNAPSHOT,
            &response, "CLI saved GPU selection", &request, readErr,
            sizeof(readErr));
        if (read) {
            request.targetGpu = g_app.adapters[resolved];
            read = service_send_state_read_request(&request, &response, true,
                    readErr, sizeof(readErr)) &&
                service_client_check_state_response(&response, readErr,
                    sizeof(readErr)) &&
                service_client_check_ready_state(&response, readErr,
                    sizeof(readErr));
        }
        if (read) apply_ready_service_envelope_to_app(&response);
        if (!read || g_app.selectedGpuIndex != resolved) {
            const char* reason = !read ? readErr :
                (response.message[0] && strncmp(response.message, "snapshot ", 9)
                    ? response.message
                    : "The background service kept its current GPU.");
            debug_log("gpu selection: CLI could not show saved nvapi=%u (shown=%u read=%d): %s\n",
                resolved, g_app.selectedGpuIndex, read ? 1 : 0,
                reason[0] ? reason : "unknown");
            set_message(err, errSize, "The saved GPU (GPU %u) is not active: %s",
                resolved, reason[0] ? reason : "unknown error");
            return false;
        }
        debug_log("gpu selection: CLI asked the service to show saved nvapi=%u\n",
            resolved);
    }
    return validate_configured_gpu_selection_for_client(err, errSize);
}
