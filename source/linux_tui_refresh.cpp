// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Included by linux_tui_actions.cpp; do not compile separately.

static const GpuAdapterInfo* tui_shown_gpu(const TuiState* state) {
    return state->targetGpu.valid ? &state->targetGpu : nullptr;
}

static bool tui_refresh_from_daemon(TuiState* state, bool userRequested,
                                    const GpuAdapterInfo* requestedTarget,
                                    LinuxDaemonSendOutcome* outcomeOut,
                                    char* errorOut, size_t errorOutSize);

static LinuxTuiGpuAdoption tui_gpu_adoption_step(const TuiState* state) {
    const LinuxConfiguredGpu* gpu = &state->configuredGpu;
    return linux_tui_gpu_adoption(gpu, tui_shown_gpu(state),
        state->service.state.activeDesiredValid != 0, state->dirty,
        linux_tui_switch_refusal_matches(&state->switchRefusal, &gpu->target,
                                         &state->service));
}

static bool tui_gpu_adoption_blocks(LinuxTuiGpuAdoption step) {
    return step != LINUX_TUI_GPU_ADOPTION_NONE &&
           step != LINUX_TUI_GPU_ADOPTION_SWITCH;
}

// Both automatic adoption and an explicit selector step remember an answered
// refusal. Busy requests remain retryable on the next answered refresh.
static bool tui_switch_to_configured_gpu(TuiState* state, bool userRequested) {
    const GpuAdapterInfo target = state->configuredGpu.target;
    LinuxDaemonSendOutcome outcome = {};
    char error[192] = {};
    const bool refreshed = tui_refresh_from_daemon(state, userRequested,
        &target, &outcome, error, sizeof(error));
    if (!refreshed && outcome.answered) {
        state->switchRefusal = linux_tui_switch_refusal(&target, &state->service);
        snprintf(state->switchRefusalReason, sizeof(state->switchRefusalReason),
                 "%s", error[0] ? error : "no reason given");
        linux_debug_logf("tui: switch to the selected GPU refused: %s "
                         "(instance=%llu generation=%llu activeSettings=%d); "
                         "asked again when the daemon's state changes",
                         state->switchRefusalReason,
                         (unsigned long long)state->service.state.serviceInstanceId,
                         (unsigned long long)state->service.state.gpuGeneration,
                         state->service.state.activeDesiredValid ? 1 : 0);
    }
    return refreshed && state->targetGpu.valid &&
        linux_gpu_identity_matches(&target, &state->targetGpu);
}

// A daemon whose GPU backend did not come up publishes no adapters, so
// config.ini's GPU stays unchecked, and only a request that names a GPU makes
// it try the backend again.  Ask once per daemon instance (again after an
// explicit Refresh) with the stored record, then check the adapters it now
// publishes; writes still wait for that check.
static void tui_bring_up_daemon_backend(TuiState* state, bool userRequested) {
    LinuxConfiguredGpu* gpu = &state->configuredGpu;
    if (userRequested) state->backendBringUpAsked = false;
    const GpuAdapterInfo* stored = linux_configured_gpu_bring_up_target(gpu,
        state->service.snapshot.adapterCount);
    if (!stored) return;
    const gc_u64 instance = state->service.state.serviceInstanceId;
    if (state->backendBringUpAsked && state->backendBringUpInstance == instance)
        return;
    state->backendBringUpAsked = true;
    state->backendBringUpInstance = instance;
    const GpuAdapterInfo request = *stored;
    LinuxDaemonSendOutcome outcome = {};
    char error[192] = {};
    const bool refreshed = tui_refresh_from_daemon(state, false, &request,
        &outcome, error, sizeof(error));
    linux_debug_logf("tui: asked the daemon (instance=%llu) to bring its GPU "
                     "backend up for config.ini's GPU: ok=%d answered=%d "
                     "adapters=%u%s%s",
                     (unsigned long long)instance, refreshed ? 1 : 0,
                     outcome.answered ? 1 : 0,
                     state->service.snapshot.adapterCount,
                     error[0] ? " error=" : "", error);
    if (refreshed && state->service.snapshot.adapterCount > 0)
        linux_configured_gpu_resolve_against(state->configPath, gpu,
            state->service.snapshot.adapters,
            state->service.snapshot.adapterCount);
}

// config.ini's GPU is resolved against the daemon's own adapter list the first
// time one is available: at start, or on the reconnect after a start without
// the daemon.  Only the live adapter it resolved to is ever sent; the stored
// record is not, because the daemon would follow it to a GPU its location role
// rules out (linux_configured_gpu_request_target).  The resolved GPU, or one
// chosen with `--gpu`, is then switched to with one targeted refresh after
// every answered refresh until it is the GPU shown: the TUI writes to the GPU
// it shows, so until then every write is blocked (linux_tui_shown_gpu_write).
// linux_tui_gpu_adoption() says when not to switch (held, deferred, refused,
// unresolved); each of those states says why once, on the transition.
// Runs only at the end of an untargeted refresh, so the switch below does not
// recurse and the selector's own choice is not undone within its refresh.
static void tui_adopt_configured_gpu(TuiState* state, bool userRequested) {
    LinuxConfiguredGpu* gpu = &state->configuredGpu;
    const LinuxConfiguredGpuStatus before = gpu->status;
    if (gpu->status == LINUX_CONFIGURED_GPU_PENDING ||
        gpu->status == LINUX_CONFIGURED_GPU_UNVALIDATED)
        linux_configured_gpu_resolve_against(state->configPath, gpu,
            state->service.snapshot.adapters,
            state->service.snapshot.adapterCount);
    tui_bring_up_daemon_backend(state, userRequested);
    // An explicit Refresh asks a refused switch again.
    if (userRequested && state->switchRefusal.valid) {
        linux_debug_logf("tui: refresh asks the refused GPU switch again");
        state->switchRefusal = LinuxTuiSwitchRefusal{};
    }
    LinuxTuiGpuAdoption step = tui_gpu_adoption_step(state);
    if (step == LINUX_TUI_GPU_ADOPTION_SWITCH) {
        tui_switch_to_configured_gpu(state, false);
        step = tui_gpu_adoption_step(state);
    }
    // A blocked selection whose problem changed (unchecked -> not present)
    // says so again even though the step stayed the same.
    const bool problemChanged = step == LINUX_TUI_GPU_ADOPTION_BLOCKED &&
                                gpu->status != before;
    if (step == state->gpuAdoption && !problemChanged) return;
    char selected[32] = {}, shown[32] = {};
    format_linux_gpu_bdf(&gpu->target, selected, sizeof(selected));
    format_linux_gpu_bdf(&state->targetGpu, shown, sizeof(shown));
    linux_debug_logf("tui: selected GPU adoption %s -> %s (selected=%s "
                     "status=%s shown=%s activeSettings=%d dirty=%d)",
                     linux_tui_gpu_adoption_name(state->gpuAdoption),
                     linux_tui_gpu_adoption_name(step), selected,
                     linux_configured_gpu_status_name(gpu->status), shown,
                     state->service.state.activeDesiredValid ? 1 : 0,
                     state->dirty ? 1 : 0);
    switch (step) {
    case LINUX_TUI_GPU_ADOPTION_BLOCKED:
        snprintf(state->status, sizeof(state->status),
                 "Writes blocked: %s; click the GPU name to select one, or "
                 "save one with --gpu", linux_configured_gpu_problem(gpu->status));
        break;
    case LINUX_TUI_GPU_ADOPTION_HELD:
        snprintf(state->status, sizeof(state->status),
                 "Writes blocked: GPU %s has active settings, so the daemon "
                 "keeps it instead of the selected GPU %s; select %s with the "
                 "GPU name and Reset it to switch", shown, selected, shown);
        break;
    case LINUX_TUI_GPU_ADOPTION_DEFERRED:
        snprintf(state->status, sizeof(state->status),
                 "Writes blocked: this window shows GPU %s, not the selected "
                 "GPU %s; reset or save staged changes to switch to it",
                 shown, selected);
        break;
    case LINUX_TUI_GPU_ADOPTION_REFUSED:
        snprintf(state->status, sizeof(state->status),
                 "Writes blocked: the daemon refused to switch to the selected "
                 "GPU %s (%s); click the GPU name to choose one, or Refresh to "
                 "ask again", selected, state->switchRefusalReason);
        break;
    case LINUX_TUI_GPU_ADOPTION_NONE:
        if (tui_gpu_adoption_blocks(state->gpuAdoption))
            snprintf(state->status, sizeof(state->status),
                     "GPU %s is shown; writes reach it again", shown);
        break;
    case LINUX_TUI_GPU_ADOPTION_SWITCH:
        // Not answered (busy): the refresh's own status line says so, and the
        // next answered refresh asks again.
        break;
    }
    state->gpuAdoption = step;
}

bool tui_configured_gpu_allows_write(TuiState* state, const char* action) {
    const LinuxTuiShownGpuWrite verdict = linux_tui_shown_gpu_write(
        &state->configuredGpu, tui_shown_gpu(state));
    if (verdict == LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED) return true;
    char selected[32] = {}, shown[32] = {};
    format_linux_gpu_bdf(&state->configuredGpu.target, selected,
                         sizeof(selected));
    format_linux_gpu_bdf(&state->targetGpu, shown, sizeof(shown));
    if (verdict == LINUX_TUI_SHOWN_GPU_WRITE_NOT_SHOWN)
        snprintf(state->status, sizeof(state->status),
                 "%s blocked: this window shows GPU %s, not the selected GPU "
                 "%s; click the GPU name to choose one", action, shown,
                 selected);
    else
        snprintf(state->status, sizeof(state->status),
                 "%s blocked: %s; click the GPU name to select one first",
                 action,
                 linux_configured_gpu_problem(state->configuredGpu.status));
    linux_debug_logf("tui: %s blocked; verdict=%s config.ini GPU status=%s "
                     "selected=%s shown=%s", action,
                     linux_tui_shown_gpu_write_name(verdict),
                     linux_configured_gpu_status_name(state->configuredGpu.status),
                     selected, shown);
    return false;
}

// The GPU selector (a click on the GPU name).  It steps from the selected GPU,
// not the shown one (linux_tui_selector_origin), so a GPU the daemon keeps for
// its active settings can itself be selected -- and Reset -- while another one
// is selected.  An explicit choice replaces config.ini's GPU for this session
// and lifts its write block once the daemon shows it; `--gpu` persists one.
bool tui_select_gpu_step(TuiState* state, int delta) {
    if (!state) return false;
    if (state->dirty) {
        snprintf(state->status, sizeof(state->status),
                 "Reset or save staged changes before switching GPUs");
        return false;
    }
    const unsigned int count = state->service.snapshot.adapterCount;
    if (!state->serviceOnline || count == 0) {
        snprintf(state->status, sizeof(state->status),
                 "No daemon GPU list is available");
        return false;
    }
    bool hasSelectedTarget = (state->service.state.validSections &
        SERVICE_STATE_SECTION_ADAPTER_IDENTITY) != 0;
    unsigned int origin = state->service.snapshot.selectedAdapterIndex;
    if (linux_tui_selector_origin(&state->configuredGpu,
            state->service.snapshot.adapters, count, &origin))
        hasSelectedTarget = true;
    const int next = linux_next_gpu_selection_index(hasSelectedTarget, origin,
                                                    count, delta);
    linux_debug_logf("tui: GPU selector step=%d origin=%u daemonSelected=%u "
                     "next=%d adapters=%u adoption=%s", delta, origin,
                     state->service.snapshot.selectedAdapterIndex, next, count,
                     linux_tui_gpu_adoption_name(state->gpuAdoption));
    if (next < 0) {
        snprintf(state->status, sizeof(state->status),
                 "GPU selection state is invalid; refresh the daemon");
        return false;
    }
    const GpuAdapterInfo chosen = state->service.snapshot.adapters[next];
    // The choice advances even when the daemon must keep showing another GPU.
    // The shown-GPU write gate remains responsible for mutation authority.
    state->configuredGpu.target = chosen;
    state->configuredGpu.status = LINUX_CONFIGURED_GPU_EXPLICIT;
    state->configuredGpu.role = linux_gpu_location_role_for_selection(
        &chosen, state->service.snapshot.adapters, count);
    state->switchRefusal = LinuxTuiSwitchRefusal{};
    state->switchRefusalReason[0] = 0;
    linux_debug_logf("tui: selector choice advanced to adapter=%d; waiting for the shown GPU to match",
                     next);
    return tui_switch_to_configured_gpu(state, true);
}

bool tui_refresh_service(TuiState* state, bool userRequested,
                         const GpuAdapterInfo* requestedTarget) {
    return tui_refresh_from_daemon(state, userRequested, requestedTarget,
                                   nullptr, nullptr, 0);
}

static bool tui_refresh_from_daemon(TuiState* state, bool userRequested,
                                    const GpuAdapterInfo* requestedTarget,
                                    LinuxDaemonSendOutcome* outcomeOut,
                                    char* errorOut, size_t errorOutSize) {
    if (outcomeOut) *outcomeOut = LinuxDaemonSendOutcome{};
    if (errorOut && errorOutSize) errorOut[0] = 0;
    if (!state) return false;
    ServiceResponse next = {};
    char error[256] = {};
    const GpuAdapterInfo* target = requestedTarget;
    if (!target && state->targetGpu.valid) target = &state->targetGpu;
    bool wasOnline = state->serviceOnline;
    ServiceResponse previous = state->service;
    LinuxDaemonSendOutcome outcome = {};
    if (!linux_daemon_get_state_ex(target, &next, &outcome,
                                   error, sizeof(error))) {
        if (outcomeOut) *outcomeOut = outcome;
        if (errorOut && errorOutSize)
            snprintf(errorOut, errorOutSize, "%s", error);
        state->nextTelemetryMs = tui_monotonic_ms() + 1500;
        // A read the daemon ACCEPTED and then failed to answer in time proves
        // it is busy, not absent.  Dropping serviceOnline here is the Linux
        // shape of the 2026-09-11 Windows incident: a long mutation on another
        // client would tear down live authority, detach the draft and block
        // Apply, then restore all of it one second later -- once per slow tick.
        // One stale frame with an honest status line is the correct
        // degradation; the daemon's own systemd watchdog resolves a genuinely
        // wedged daemon, and connect() starts failing when it does.  An
        // ANSWERED failure is a refusal (a GPU switch the daemon will not
        // make), not a busy daemon, and says so.
        if (wasOnline && !linux_daemon_failure_means_offline(outcome.reachability)) {
            set_daemon_failure(state, outcome.answered
                ? "Daemon refused the request; showing the last state it published"
                : "Daemon is busy; showing the last state it published", error);
            return false;
        }
        state->serviceOnline = false;
        state->draftAttached = false;
        // A reconnect says again why writes are blocked.
        state->gpuAdoption = LINUX_TUI_GPU_ADOPTION_NONE;
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
    if (!requestedTarget) tui_adopt_configured_gpu(state, userRequested);
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
