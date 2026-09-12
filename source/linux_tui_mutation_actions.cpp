// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_tui_mutation_actions.cpp -- the TUI actions that change something
// outside the editor: the GPU (Apply/Reset), the daemon's boot-apply policy,
// and the live exports written to disk.
//
// Split out of linux_tui_actions.cpp for the source-size ratchet, along the
// seam that already existed: everything before it edits a draft in memory,
// everything here leaves the process.  That is also why the outcome wording
// lives here -- an Apply whose outcome could not be read back is reported as
// UNKNOWN, never as failed, because the write may well have committed.
//
// Included inside linux_tui_actions.cpp's anonymous namespace; do not compile
// separately.

void apply_to_gpu(TuiState* state) {
    if (!state->serviceOnline) {
        snprintf(state->status, sizeof(state->status),
                 "Apply blocked: daemon is offline");
        return;
    }
    if (!state->draftAttached ||
        !linux_tui_draft_binding_matches(&state->draftBinding,
                                         &state->service,
                                         &state->desired)) {
        state->draftAttached = false;
        snprintf(state->status, sizeof(state->status),
                 "Apply blocked: this draft is detached from the current daemon/GPU generation; reload or restage it after review");
        return;
    }
    DesiredSettings normalized = state->desired;
    if (normalized.hasFan && normalized.fanMode == FAN_MODE_CURVE)
        fan_curve_normalize(&normalized.fanCurve);
    gc_u32 requestedDomains = service_desired_mutation_domains(&normalized);
    gc_u32 missingDomains = requestedDomains &
        ~state->service.snapshot.health.availableMutationDomains;
    if (requestedDomains == 0 || missingDomains != 0) {
        snprintf(state->status, sizeof(state->status),
            requestedDomains == 0
                ? "Apply blocked: the draft contains no GPU mutation"
                : "Apply blocked before write: requested domains=0x%02x available=0x%02x missing=0x%02x",
            requestedDomains,
            state->service.snapshot.health.availableMutationDomains,
            missingDomains);
        return;
    }
    char fanError[160] = {};
    if (normalized.hasFan && normalized.fanMode == FAN_MODE_CURVE &&
        !fan_curve_validate(&normalized.fanCurve, fanError, sizeof(fanError))) {
        set_daemon_failure(state, "Fan curve is invalid", fanError);
        return;
    }
    snprintf(state->status, sizeof(state->status), "Applying staged settings...");
    tui_render(state);
    ServiceResponse response = {};
    char result[512] = {};
    bool ok = linux_daemon_apply_checked(
        state->targetGpu.valid ? &state->targetGpu : nullptr,
        &normalized, true, &state->service.state, &response,
        result, sizeof(result));
    if (!ok) {
        // An unrecovered operation is NOT a failed one.  The write may have
        // committed; reporting it as "Apply failed" with the draft still dirty
        // is what invited a duplicate hardware write with a fresh operation ID.
        // Refresh either way, so the user sees what the GPU actually holds.
        if (response.operationId != 0 &&
            response.operationState == SERVICE_OPERATION_OUTCOME_UNKNOWN) {
            set_daemon_failure(state, "Apply outcome unknown", result);
            tui_refresh_service(state, false);
            return;
        }
        set_daemon_failure(state, "Apply failed", result);
        if (response.status == SERVICE_STATUS_STALE_STATE)
            tui_refresh_service(state, false);
        return;
    }
    state->service = response;
    state->serviceOnline = true;
    desired_from_live_response(response, &state->desired);
    state->acceptedDesired = state->desired;
    bind_current_draft(state);
    state->dirty = false;
    snprintf(state->status, sizeof(state->status), "%s",
             result[0] ? result : "Applied staged settings");
}

void reset_gpu(TuiState* state) {
    if (!state->serviceOnline) {
        snprintf(state->status, sizeof(state->status),
                 "Reset blocked: daemon is offline");
        return;
    }
    if (state->service.state.gpuPhase != SERVICE_GPU_PHASE_READY) {
        snprintf(state->status, sizeof(state->status),
                 "Reset blocked: GPU is not in READY state (phase=%u)",
                 (unsigned int)state->service.state.gpuPhase);
        return;
    }
    if (!linux_reset_preflight_domains_supported(
            state->service.snapshot.health.availableMutationDomains,
            state->service.snapshot.powerLimitDefaultmW)) {
        snprintf(state->status, sizeof(state->status),
                 "Reset blocked: core mutation domains unavailable (available=0x%02x required=0x%02x)",
                 state->service.snapshot.health.availableMutationDomains,
                 linux_reset_required_mutation_domains());
        return;
    }
    snprintf(state->status, sizeof(state->status), "Resetting GPU controls...");
    tui_render(state);
    ServiceResponse response = {};
    char result[512] = {};
    bool ok = linux_daemon_reset_checked(
        state->targetGpu.valid ? &state->targetGpu : nullptr,
        &state->service.state, &response, result, sizeof(result));
    if (!ok) {
        // Same rule as Apply: an unrecovered outcome is unknown, not failed.
        if (response.operationId != 0 &&
            response.operationState == SERVICE_OPERATION_OUTCOME_UNKNOWN) {
            set_daemon_failure(state, "Reset outcome unknown", result);
            tui_refresh_service(state, false);
            return;
        }
        set_daemon_failure(state, "GPU reset failed", result);
        if (response.status == SERVICE_STATUS_STALE_STATE)
            tui_refresh_service(state, false);
        return;
    }
    state->service = response;
    desired_from_live_response(response, &state->desired);
    state->acceptedDesired = state->desired;
    bind_current_draft(state);
    state->dirty = false;
    snprintf(state->status, sizeof(state->status), "%s",
             result[0] ? result : "GPU reset to driver defaults");
}

// Cycle the daemon's boot-apply policy.  Choosing "profile N" snapshots the
// *saved* profile, never the unsaved draft: what gets written at the next boot
// has to be something the user can read back out of config.ini.
void apply_startup_policy_cycle(TuiState* state) {
    if (!state->serviceOnline) {
        snprintf(state->status, sizeof(state->status),
                 "Startup policy requires a reachable daemon");
        return;
    }
    unsigned int current = state->service.state.startupPolicyMode;
    unsigned int next =
        current == SERVICE_STARTUP_POLICY_RESTORE_LAST ? SERVICE_STARTUP_POLICY_NONE
        : current == SERVICE_STARTUP_POLICY_NONE ? SERVICE_STARTUP_POLICY_PROFILE
        : SERVICE_STARTUP_POLICY_RESTORE_LAST;

    char result[512] = {};
    bool ok = false;
    if (next == SERVICE_STARTUP_POLICY_PROFILE) {
        if (!state->targetGpu.valid) {
            snprintf(state->status, sizeof(state->status),
                     "Select a GPU before binding a startup profile to it");
            return;
        }
        DesiredSettings saved = {};
        char error[256] = {};
        if (!load_profile_from_config_path(state->configPath,
                                           state->currentSlot, &saved,
                                           error, sizeof(error))) {
            set_daemon_failure(state, "Startup profile load failed", error);
            return;
        }
        normalize_desired_settings_for_ui(&saved);
        char label[64] = {};
        snprintf(label, sizeof(label), "profile %d", state->currentSlot);
        ok = linux_daemon_set_startup_policy(next, state->currentSlot, label,
                                             &state->targetGpu, &saved,
                                             result, sizeof(result));
    } else {
        ok = linux_daemon_set_startup_policy(next, 0, nullptr, nullptr, nullptr,
                                             result, sizeof(result));
    }
    if (!ok) {
        set_daemon_failure(state, "Startup policy update failed", result);
        return;
    }
    // Re-read rather than assume: the envelope the daemon publishes is the only
    // authority for what the control now shows.
    tui_refresh_service(state, false);
    tui_refresh_startup_snapshot(state);
    snprintf(state->status, sizeof(state->status), "%s",
             result[0] ? result : "Startup policy updated");
}

// A profile write is only half the job while the daemon boot-applies that same
// slot: the daemon holds a snapshot of it and cannot read config.ini itself, so
// an un-pushed edit silently reverts at the next boot.  Called after every path
// that changes the *content* of a slot; it never re-binds the policy.
void sync_startup_snapshot_after_profile_write(TuiState* state, int slot,
                                               bool slotStillHasContent) {
    if (!state) return;
    LinuxStartupSyncResult sync = linux_startup_sync_after_profile_write(
        state->configPath, slot, slotStillHasContent, state->serviceOnline,
        state->serviceOnline ? state->service.state.startupPolicyMode
                             : (unsigned int)SERVICE_STARTUP_POLICY_RESTORE_LAST,
        state->serviceOnline ? state->service.state.startupPolicySlot : 0u);
    if (sync.action == STARTUP_SNAPSHOT_SYNC_NONE) return;
    size_t used = strlen(state->status);
    if (used + 1 < sizeof(state->status)) {
        snprintf(state->status + used, sizeof(state->status) - used, " • %s",
                 sync.message);
    }
    if (sync.action != STARTUP_SNAPSHOT_SYNC_UNREACHABLE) {
        tui_refresh_service(state, false);
        tui_refresh_startup_snapshot(state);
    }
}

void export_live(TuiState* state, bool json) {
    if (!state->serviceOnline) {
        snprintf(state->status, sizeof(state->status),
                 "Live export requires a daemon snapshot");
        return;
    }
    char path[LINUX_PATH_MAX] = {};
    const char* slash = strrchr(state->configPath, '/');
    int directoryLength = slash ? (int)(slash - state->configPath) : 0;
    if (directoryLength > 0) {
        snprintf(path, sizeof(path), "%.*s/%s", directoryLength,
                 state->configPath,
                 json ? "greencurve-live.json" : "greencurve-live.txt");
    } else {
        snprintf(path, sizeof(path), "%s",
                 json ? "greencurve-live.json" : "greencurve-live.txt");
    }
    FILE* file = fopen(path, "wb");
    if (!file) {
        snprintf(state->status, sizeof(state->status),
                 "Cannot create live export: %s", path);
        return;
    }
    if (json) print_linux_live_state_json(file, &state->service);
    else print_linux_live_state_text(file, &state->service);
    bool flushed = fflush(file) == 0;
    bool closed = fclose(file) == 0;
    bool ok = flushed && closed;
    snprintf(state->status, sizeof(state->status), "%s: %s",
             ok ? "Live VF export written" : "Live VF export failed", path);
}
