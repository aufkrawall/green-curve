// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included inside linux_tui_actions.cpp's anonymous namespace.

void open_startup_picker(TuiState* state) {
    state->startupPicker.open = true;
    state->focusIndex = -1;
    char error[256] = {};
    state->startupPicker.bankReadable = load_saved_profile_slots(
        state->configPath, state->startupPicker.savedSlots, error, sizeof(error));
    if (!state->startupPicker.bankReadable) {
        linux_debug_logf("startup-picker: could not read saved slots");
        set_daemon_failure(state, "Cannot read saved profiles", error);
    } else if (!state->serviceOnline) {
        snprintf(state->status, sizeof(state->status), "Startup policy requires a reachable daemon");
    }
}

// The picker supplies the slot explicitly. Read it again at selection time to
// snapshot the saved values and catch changes to the bank while it was open.
bool apply_startup_policy(TuiState* state, unsigned int mode, int slot) {
    if (!state->serviceOnline) {
        snprintf(state->status, sizeof(state->status),
                 "Startup policy requires a reachable daemon");
        return false;
    }

    char result[512] = {};
    bool ok;
    if (mode == SERVICE_STARTUP_POLICY_PROFILE) {
        if (!tui_configured_gpu_allows_write(state, "Startup profile bind"))
            return false;
        if (!state->targetGpu.valid) {
            snprintf(state->status, sizeof(state->status),
                     "Select a GPU before binding a startup profile to it");
            return false;
        }
        DesiredSettings saved = {};
        char error[256] = {};
        auto liveTopo = std::make_unique<ProfileDestinationTopology>();
        const bool haveLive = linux_profile_destination_from_snapshot(&state->service.snapshot,
                                                                      liveTopo.get());
        if (!load_profile_from_config_path_for_destination(state->configPath, slot,
                haveLive ? liveTopo.get() : nullptr, &saved, error, sizeof(error))) {
            linux_debug_logf("startup-picker: failed to load slot=%d live=%d", slot, haveLive ? 1 : 0);
            set_daemon_failure(state, "Startup profile load failed", error);
            return false;
        }
        char label[64] = {};
        snprintf(label, sizeof(label), "profile %d", slot);
        ok = linux_daemon_set_startup_policy(mode, slot, label, &state->targetGpu,
                                             &saved, result, sizeof(result));
    } else {
        ok = linux_daemon_set_startup_policy(mode, 0, nullptr, nullptr, nullptr,
                                             result, sizeof(result));
    }
    linux_debug_logf("startup-picker: mode=%u slot=%d updated=%d", mode, slot, ok ? 1 : 0);
    if (!ok) {
        set_daemon_failure(state, "Startup policy update failed", result);
        return false;
    }
    // Read back the daemon's answer for the status shown on the control.
    tui_refresh_service(state, false);
    tui_refresh_startup_snapshot(state);
    snprintf(state->status, sizeof(state->status), "%s",
             result[0] ? result : "Startup policy updated");
    return true;
}
