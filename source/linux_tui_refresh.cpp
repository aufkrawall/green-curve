// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_tui_actions.cpp; do not compile separately.

bool tui_refresh_service(TuiState* state, bool userRequested,
                         const GpuAdapterInfo* requestedTarget) {
    if (!state) return false;
    ServiceResponse next = {};
    char error[256] = {};
    const GpuAdapterInfo* target = requestedTarget;
    if (!target && state->targetGpu.valid) target = &state->targetGpu;
    bool wasOnline = state->serviceOnline;
    ServiceResponse previous = state->service;
    if (!linux_daemon_get_state(target, &next, error, sizeof(error))) {
        state->serviceOnline = false;
        state->draftAttached = false;
        set_daemon_failure(state, "Daemon refresh failed", error);
        state->nextTelemetryMs = tui_monotonic_ms() + 1500;
        return false;
    }

    bool hadDirtyDraft = state->dirty;
    bool bindingStillValid = linux_tui_draft_binding_matches(
        &state->draftBinding, &next, &state->desired);
    bool healthChanged = !wasOnline ||
        previous.state.gpuPhase != next.state.gpuPhase ||
        previous.snapshot.health.reason != next.snapshot.health.reason ||
        previous.snapshot.health.driverStatus !=
            next.snapshot.health.driverStatus ||
        previous.snapshot.health.availableMutationDomains !=
            next.snapshot.health.availableMutationDomains;
    IntentReadbackStatus previousReadback = wasOnline
        ? compare_intent_to_readback(&previous) : IntentReadbackStatus{};
    IntentReadbackStatus nextReadback = compare_intent_to_readback(&next);
    bool readbackChanged = !wasOnline ||
        previousReadback.divergedDomains != nextReadback.divergedDomains ||
        previousReadback.unavailableDomains != nextReadback.unavailableDomains;
    state->service = next;
    state->serviceOnline = true;
    const GpuAdapterInfo* active = selected_adapter(next);
    if (active) state->targetGpu = *active;

    if (!hadDirtyDraft) {
        desired_from_live_response(next, &state->desired);
        state->acceptedDesired = state->desired;
        state->dirty = false;
        bind_current_draft(state);
        if (!wasOnline || state->selectedPoint < 0 ||
            state->selectedPoint >= VF_NUM_POINTS ||
            next.snapshot.curve[state->selectedPoint].freq_kHz == 0) {
            state->selectedPoint = useful_initial_point(next, state->desired);
        }
        // Keep the offset inside the list, but never drag it back to the
        // selection: this runs once a second, so it silently undid any scroll
        // that had moved past the selected point. Re-showing the selection is
        // the reveal-on-select path's job, and it fires here too whenever the
        // line above actually changed selectedPoint.
        state->vfScroll = tui_clamp_vf_scroll(*state, state->vfScroll);
    } else if (bindingStillValid) {
        DesiredSettings accepted = {};
        desired_from_live_response(next, &accepted);
        state->acceptedDesired = accepted;
        state->draftAttached = true;
        tui_recompute_dirty(state);
    } else {
        state->draftAttached = false;
    }

    if (userRequested) {
        if (!state->draftAttached) {
            snprintf(state->status, sizeof(state->status),
                "Live state refreshed, but the staged draft is detached from this daemon/GPU/topology generation; reload or restage after review");
        } else if (nextReadback.divergedDomains) {
            snprintf(state->status, sizeof(state->status),
                "Hardware override detected: configured intent differs from live readback; Green Curve will not reapply without an explicit Apply");
        } else if (nextReadback.unavailableDomains) {
            snprintf(state->status, sizeof(state->status),
                "Live GPU state refreshed; some configured domains have no trustworthy hardware readback");
        } else if (next.state.gpuPhase == SERVICE_GPU_PHASE_READY) {
            snprintf(state->status, sizeof(state->status),
                     "Live GPU state refreshed; VF and independent domains are ready");
        } else {
            set_gpu_health_status(state,
                "Daemon online; draft is attached only to advertised safe domains");
        }
    } else if (!wasOnline) {
        if (state->draftAttached &&
            next.state.gpuPhase == SERVICE_GPU_PHASE_READY)
            snprintf(state->status, sizeof(state->status),
                     "Daemon reconnected; GPU ready");
        else if (state->draftAttached)
            set_gpu_health_status(state, "Daemon reconnected in degraded mode");
        else
            snprintf(state->status, sizeof(state->status),
                "Daemon reconnected, but the existing draft is detached and requires review");
    } else if (healthChanged &&
               next.state.gpuPhase != SERVICE_GPU_PHASE_READY) {
        set_gpu_health_status(state, "GPU health changed");
    } else if (readbackChanged && nextReadback.divergedDomains) {
        snprintf(state->status, sizeof(state->status),
            "Hardware override detected; configured intent is preserved for review and is not automatically reapplied");
    } else if (readbackChanged && previousReadback.divergedDomains &&
               !nextReadback.divergedDomains &&
               !nextReadback.unavailableDomains) {
        snprintf(state->status, sizeof(state->status),
            "Hardware readback now matches configured intent");
    }
    state->nextTelemetryMs = tui_monotonic_ms() + 1000;
    return true;
}

// The boot-apply snapshot is daemon-side configuration, not telemetry: it is
// re-read only where it can have moved (TUI start, a profile write, a policy
// change), never on the 1 Hz tick that tui_refresh_service() drives.
void tui_refresh_startup_snapshot(TuiState* state) {
    if (!state) return;
    LinuxStartupSnapshotReport report = linux_startup_snapshot_report(
        state->configPath, state->serviceOnline,
        state->serviceOnline ? state->service.state.startupPolicyMode
                             : (unsigned int)SERVICE_STARTUP_POLICY_RESTORE_LAST,
        state->serviceOnline ? state->service.state.startupPolicySlot : 0u);
    state->startupSnapshot = report.snapshot;
    state->startupSnapshotKnown = report.snapshotKnown;
    state->startupSnapshotState = report.state;
}
