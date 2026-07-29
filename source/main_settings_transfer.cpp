// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Carrying live settings across a program upgrade.
//
// The setup program cannot read the service's own protected state — that state
// is deliberately internal, versioned, and, by the auto-restore policy, "intent,
// not permission to write".  So the transfer runs entirely through this client:
//
//   greencurve.exe --export-active-settings <file>   (before the upgrade)
//   greencurve.exe --apply-settings-file  <file>     (after it, new build)
//
// The export asks the running service for the intent it is currently holding
// and writes it to a standalone file.  The apply loads that file and performs a
// perfectly ordinary explicit CLI Apply — the same operation a user typing
// `--gpu-offset ...` would get, typed as SERVICE_APPLY_ORIGIN_CLI.  Nothing here
// gives the service a new way to write to the GPU on its own: replaying a
// snapshot without an explicit request remains impossible, which is exactly the
// property the event-only restore contract exists to protect.
//
// The file is written with the normal profile serializer at slot 1, so it is
// the same format (and the same validation) as a saved profile rather than a
// second, less-tested encoding.

#ifndef GREEN_CURVE_SERVICE_BINARY

// The snapshot file holds one profile; slot 1 keeps it compatible with the
// existing reader/writer without inventing a parallel section name.
#define SETTINGS_TRANSFER_SLOT 1

// Write the settings the service currently holds to `path`.
//
// Fails when nothing is applied.  That is a meaningful answer, not an error to
// paper over: an upgrade with nothing active must not "restore" a synthesized
// stock profile onto the GPU afterwards.
static bool settings_transfer_export(const char* path, char* result, size_t resultSize) {
    if (!path || !path[0]) {
        set_message(result, resultSize, "No export path was given");
        return false;
    }
    if (!g_app.backgroundServiceAvailable) {
        set_message(result, resultSize,
            "Background service is not available; there are no active settings to export");
        return false;
    }

    DesiredSettings active = {};
    char err[256] = {};
    if (!refresh_service_snapshot_and_active_desired(err, sizeof(err), &active)) {
        set_message(result, resultSize, "%s", err[0] ? err : "Failed reading the background service state");
        return false;
    }
    if (!g_app.serviceActiveDesiredValid) {
        set_message(result, resultSize,
            "The background service is not holding any applied settings; nothing to export");
        debug_log("settings transfer: export skipped because the service reports no active intent\n");
        return false;
    }

    int curvePoints = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (active.hasCurvePoint[i]) curvePoints++;
    }
    debug_log("settings transfer: exporting active intent gpu=%dMHz mem=%dMHz power=%d%% "
        "lockCi=%d curvePoints=%d fanMode=%d to %s\n",
        active.hasGpuOffset ? active.gpuOffsetMHz : 0,
        active.hasMemOffset ? active.memOffsetMHz : 0,
        active.hasPowerLimit ? active.powerLimitPct : 0,
        active.hasLock ? active.lockCi : -1,
        curvePoints,
        active.hasFan ? active.fanMode : -1,
        path);

    if (!save_profile_to_config(path, SETTINGS_TRANSFER_SLOT, &active, err, sizeof(err))) {
        set_message(result, resultSize, "%s", err[0] ? err : "Failed writing the settings snapshot");
        return false;
    }
    // Read the snapshot back before reporting success.  The caller (setup) is
    // about to stop this process and every other Green Curve process on the
    // machine, so an unreadable file discovered later cannot be regenerated.
    DesiredSettings verify = {};
    if (!load_profile_from_config(path, SETTINGS_TRANSFER_SLOT, &verify, err, sizeof(err))) {
        set_message(result, resultSize,
            "The settings snapshot could not be read back: %s", err[0] ? err : "unknown error");
        return false;
    }
    set_message(result, resultSize, "Active settings exported to %s", path);
    return true;
}

// How long to wait for a freshly started service to finish bringing the GPU up.
//
// Setup applies immediately after `--service-install` returns, and the SCM
// reports RUNNING as soon as the pipe listener and lifecycle worker are ready —
// which is *before* NVML/NvAPI have produced a READY GPU phase. Without this
// wait the very first restore after an upgrade lost the race and was rejected.
//
// This is a prerequisite wait, not a retried write: the auto-restore contract
// says "missing prerequisites may be retried; an actual hardware write may not",
// and the write below still happens exactly once, only after the service has
// published a coherent READY envelope.
#define SETTINGS_TRANSFER_READY_TIMEOUT_MS 60000
#define SETTINGS_TRANSFER_READY_RETRY_MS 250

// Fetch a READY envelope and project it onto app state.
//
// An APPLY request is only valid when it carries the service instance id, GPU
// generation, topology signature, and selected adapter from a READY envelope
// (see service_request_reject_reason). Skipping this step is what made the
// first upgrade restore fail with "Service request contains invalid protocol
// fields": the request went out with all four preconditions still zero.
//
// Reading the envelope is only half of it — the restore is not ready to run
// until that envelope has actually been adopted as the identity the apply will
// stamp (service_client_identity_complete). Treating the read alone as
// readiness is what left the next two upgrades failing exactly as before.
static bool settings_transfer_wait_for_ready_service(char* result, size_t resultSize) {
    ULONGLONG deadline = GetTickCount64() + SETTINGS_TRANSFER_READY_TIMEOUT_MS;
    char err[256] = {};
    unsigned int attempts = 0;
    for (;;) {
        attempts++;
        refresh_background_service_state();
        if (g_app.backgroundServiceAvailable) {
            ServiceResponse stateResponse = {};
            err[0] = 0;
            if (service_client_get_ready_state(&stateResponse, 5000,
                    "CLI settings transfer", err, sizeof(err))) {
                apply_ready_service_envelope_to_app(&stateResponse);
                if (service_client_identity_complete(&g_syncClientStateIdentity)) {
                    debug_log("settings transfer: service reported READY after %u attempt(s)\n", attempts);
                    return true;
                }
                set_message(err, sizeof(err),
                    "The service reported ready but published no state identity to bind the apply to");
            }
        } else {
            set_message(err, sizeof(err), "Background service is not available");
        }
        if (GetTickCount64() >= deadline) {
            set_message(result, resultSize,
                "The background service did not become ready within %d seconds: %s",
                SETTINGS_TRANSFER_READY_TIMEOUT_MS / 1000,
                err[0] ? err : "unknown reason");
            debug_log("settings transfer: giving up waiting for a READY service after %u attempt(s): %s\n",
                attempts, err[0] ? err : "unknown");
            return false;
        }
        if (attempts == 1) {
            debug_log("settings transfer: waiting for the service to reach READY (%s)\n",
                err[0] ? err : "not available yet");
        }
        Sleep(SETTINGS_TRANSFER_READY_RETRY_MS);
    }
}

// Apply a file written by settings_transfer_export as an explicit CLI Apply.
static bool settings_transfer_apply(const char* path, char* result, size_t resultSize) {
    if (!path || !path[0]) {
        set_message(result, resultSize, "No settings file was given");
        return false;
    }
    if (!is_profile_slot_saved(path, SETTINGS_TRANSFER_SLOT)) {
        set_message(result, resultSize, "The settings file %s does not contain a saved profile", path);
        return false;
    }

    DesiredSettings desired = {};
    char err[256] = {};
    if (!load_profile_from_config(path, SETTINGS_TRANSFER_SLOT, &desired, err, sizeof(err))) {
        set_message(result, resultSize, "%s", err[0] ? err : "Failed reading the settings file");
        return false;
    }
    // The exported intent legitimately has no curve when the user only changed
    // power or fan settings, so a curve is not required here — unlike a logon
    // profile, which must be complete before it is applied unattended.
    if (!desired_settings_have_explicit_state(&desired, false, err, sizeof(err))) {
        set_message(result, resultSize, "%s", err[0] ? err : "The settings file is incomplete");
        return false;
    }

    // The service is normally seconds old at this point, so establish the apply
    // preconditions before anything is sent.
    if (!settings_transfer_wait_for_ready_service(result, resultSize)) return false;
    char gpuErr[256] = {};
    if (!validate_configured_gpu_selection_for_client(gpuErr, sizeof(gpuErr))) {
        set_message(result, resultSize, "%s",
            gpuErr[0] ? gpuErr : "The configured GPU identity is unavailable");
        return false;
    }

    int curvePoints = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desired.hasCurvePoint[i]) curvePoints++;
    }
    debug_log("settings transfer: applying restored intent gpu=%dMHz mem=%dMHz power=%d%% "
        "lockCi=%d curvePoints=%d fanMode=%d from %s\n",
        desired.hasGpuOffset ? desired.gpuOffsetMHz : 0,
        desired.hasMemOffset ? desired.memOffsetMHz : 0,
        desired.hasPowerLimit ? desired.powerLimitPct : 0,
        desired.hasLock ? desired.lockCi : -1,
        curvePoints,
        desired.hasFan ? desired.fanMode : -1,
        path);

    set_pending_operation_source("CLI settings transfer");
    // The snapshot describes absolute intent, so the GPU is returned to stock
    // first: without it, a curve restored on top of whatever the freshly started
    // service found would compound offsets instead of reproducing the state the
    // user had before the upgrade.
    desired.resetOcBeforeApply = true;
    // AD_HOC rather than the original slot: the settings are being restored from
    // a transfer file, and claiming a profile slot here would make the GUI show
    // a slot as applied when the file may no longer match that slot's contents.
    return apply_desired_settings(&desired, false,
        SERVICE_APPLY_ORIGIN_CLI, SERVICE_PROFILE_SOURCE_AD_HOC, 0,
        result, resultSize);
}

#endif // GREEN_CURVE_SERVICE_BINARY
