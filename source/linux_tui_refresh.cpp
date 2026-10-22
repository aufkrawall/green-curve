// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_tui_actions.cpp; do not compile separately.

// config.ini's GPU is resolved against the daemon's own adapter list the first
// time one is available: at start, or on the reconnect after a start without
// the daemon.  Only the live adapter it resolved to is ever sent; the stored
// record is not, because the daemon would follow it to a GPU its location role
// rules out (linux_configured_gpu_request_target).
static void tui_adopt_configured_gpu(TuiState* state) {
    LinuxConfiguredGpu* gpu = &state->configuredGpu;
    if (gpu->status != LINUX_CONFIGURED_GPU_PENDING &&
        gpu->status != LINUX_CONFIGURED_GPU_UNVALIDATED) return;
    const LinuxConfiguredGpuStatus before = gpu->status;
    linux_configured_gpu_resolve_against(state->configPath, gpu,
        state->service.snapshot.adapters, state->service.snapshot.adapterCount);
    if (gpu->status != LINUX_CONFIGURED_GPU_RESOLVED) {
        if (gpu->status != before)
            snprintf(state->status, sizeof(state->status),
                "Writes blocked: %s; click the GPU name to select one, or save one with --gpu",
                linux_configured_gpu_problem(gpu->status));
        return;
    }
    if (state->targetGpu.valid &&
        linux_gpu_identity_matches(&gpu->target, &state->targetGpu)) return;
    if (state->service.state.activeDesiredValid) {
        snprintf(state->status, sizeof(state->status),
                 "Another GPU has active settings; Reset it before selecting the saved GPU");
        return;
    }
    tui_refresh_service(state, false, &gpu->target);
}

bool tui_configured_gpu_allows_write(TuiState* state, const char* action) {
    if (linux_configured_gpu_request_target(&state->configuredGpu, true,
                                            nullptr)) return true;
    snprintf(state->status, sizeof(state->status),
             "%s blocked: %s; click the GPU name to select one first",
             action, linux_configured_gpu_problem(state->configuredGpu.status));
    linux_debug_logf("tui: %s blocked; config.ini GPU status=%s", action,
                     linux_configured_gpu_status_name(state->configuredGpu.status));
    return false;
}

bool tui_refresh_service(TuiState* state, bool userRequested,
                         const GpuAdapterInfo* requestedTarget) {
    if (!state) return false;
    ServiceResponse next = {};
    char error[256] = {};
    const GpuAdapterInfo* target = requestedTarget;
    if (!target && state->targetGpu.valid) target = &state->targetGpu;
    bool wasOnline = state->serviceOnline;
    ServiceResponse previous = state->service;
    LinuxDaemonReachability reachability = LINUX_DAEMON_REACHABILITY_UNKNOWN;
    if (!linux_daemon_get_state_ex(target, &next, &reachability,
                                   error, sizeof(error))) {
        state->nextTelemetryMs = tui_monotonic_ms() + 1500;
        // A read the daemon ACCEPTED and then failed to answer in time proves
        // it is busy, not absent.  Dropping serviceOnline here is the Linux
        // shape of the 2026-09-11 Windows incident: a long mutation on another
        // client would tear down live authority, detach the draft and block
        // Apply, then restore all of it one second later -- once per slow tick.
        // One stale frame with an honest status line is the correct
        // degradation; the daemon's own systemd watchdog resolves a genuinely
        // wedged daemon, and connect() starts failing when it does.
        if (wasOnline && !linux_daemon_failure_means_offline(reachability)) {
            set_daemon_failure(state,
                "Daemon is busy; showing the last state it published", error);
            return false;
        }
        state->serviceOnline = false;
        state->draftAttached = false;
        set_daemon_failure(state, "Daemon refresh failed", error);
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
    // A new automatic-restore lockout wins the status line once, with its
    // cause (protocol v29) and the action; until 2026-10 the TUI never said
    // that automatic restore had stopped.  Same edge rule as the Windows GUI,
    // against what this TUI last observed: an offline refresh in between is
    // not a transition, so a daemon restart or socket hiccup under the same
    // lockout does not re-announce it.
    const gc_u32 observedBefore = state->noticeObservedLockout;
    GcAutoRestoreNotice lockoutNotice = gc_auto_restore_notice_observe(
        &state->noticeObservedLockout, next.snapshot.autoRestoreLockoutReason);
    if (lockoutNotice == GC_AUTO_RESTORE_NOTICE_HANDBACK_INCOMPLETE) {
        snprintf(state->status, sizeof(state->status),
            "Automatic restore is off: %s",
            linux_auto_restore_lockout_reason_name(
                next.snapshot.autoRestoreLockoutReason));
    } else if (lockoutNotice == GC_AUTO_RESTORE_NOTICE_AUTO_APPLY_FAILED) {
        char title[64] = {}, body[256] = {};
        gc_auto_apply_failure_compose(&next.snapshot.autoApplyFailure,
            state->status, sizeof(state->status), title, sizeof(title),
            body, sizeof(body));
    } else if (lockoutNotice != GC_AUTO_RESTORE_NOTICE_NONE) {
        char title[64] = {}, body[256] = {};
        gc_auto_restore_lockout_compose(lockoutNotice, state->status,
            sizeof(state->status), title, sizeof(title), body, sizeof(body));
    }
    if (lockoutNotice != GC_AUTO_RESTORE_NOTICE_NONE) {
        linux_debug_logf("tui: automatic restore lockout notice (previous=%u reason=%u trigger=%u): %s\n",
            (unsigned int)observedBefore,
            (unsigned int)next.snapshot.autoRestoreLockoutReason,
            (unsigned int)next.snapshot.autoApplyFailure.trigger, state->status);
    }
    state->nextTelemetryMs = tui_monotonic_ms() + 1000;
    tui_adopt_configured_gpu(state);
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
