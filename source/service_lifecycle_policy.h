// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

// Pure, platform-neutral lifecycle reducer.  Windows control handlers feed
// events into this state machine, while deterministic unit tests use it with a
// fake clock/hardware writer.  It deliberately contains no waits, allocation,
// filesystem access, or hardware calls.

static inline bool service_intent_owns_vf_cleanup(
    const DesiredSettings* intent) {
    if (!intent) return false;
    if (intent->hasLock || intent->hasGpuOffset) return true;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (intent->hasCurvePoint[ci]) return true;
    }
    return false;
}

static inline bool service_request_replaces_lock_domain(
    const DesiredSettings* request) {
    if (!request) return false;
    if (request->resetOcBeforeApply || request->hasGpuOffset ||
        request->hasLock) return true;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (request->hasCurvePoint[ci]) return true;
    }
    return false;
}

static inline bool service_build_full_restore_request(
    const DesiredSettings* activeIntent,
    DesiredSettings* requestOut)
{
    if (!activeIntent || !requestOut) return false;
    *requestOut = *activeIntent;
    // Reset-to-stock is part of replaying Green Curve-owned VF/GPU-offset
    // policy, because those writes are derived from a clean curve baseline.
    // Never reset unrelated OC state for a sparse lock-, memory-, power-, or
    // fan-only intent: missing fields may belong to another tool.
    bool ownsVfPolicy = activeIntent->hasGpuOffset;
    for (int ci = 0; ci < VF_NUM_POINTS && !ownsVfPolicy; ++ci) {
        ownsVfPolicy = activeIntent->hasCurvePoint[ci] != 0;
    }
    requestOut->resetOcBeforeApply = ownsVfPolicy;
    return true;
}

// Build the hardware transition for selecting a complete named profile. Any
// controls omitted by the new profile but owned by the previous Green Curve
// intent are returned to defaults exactly once. The caller must publish
// `nextIntent`, not this cleanup request, as the new ownership declaration.
static inline bool service_build_profile_transition_request(
    const DesiredSettings* previousIntent,
    const DesiredSettings* nextIntent,
    DesiredSettings* requestOut) {
    if (!nextIntent || !requestOut) return false;
    *requestOut = *nextIntent;
    bool nextOwnsVf = nextIntent->hasGpuOffset;
    for (int ci = 0; ci < VF_NUM_POINTS && !nextOwnsVf; ++ci) {
        nextOwnsVf = nextIntent->hasCurvePoint[ci] != 0;
    }
    bool previousOwnedVf = service_intent_owns_vf_cleanup(previousIntent);
    requestOut->resetOcBeforeApply = nextOwnsVf || previousOwnedVf;
    if (!previousIntent) return true;
    if (previousIntent->hasGpuOffset && !nextIntent->hasGpuOffset) {
        requestOut->hasGpuOffset = true;
        requestOut->gpuOffsetMHz = 0;
        requestOut->gpuOffsetExcludeLowCount = 0;
    }
    if (previousIntent->hasMemOffset && !nextIntent->hasMemOffset) {
        requestOut->hasMemOffset = true;
        requestOut->memOffsetMHz = 0;
    }
    if (previousIntent->hasPowerLimit && !nextIntent->hasPowerLimit) {
        requestOut->hasPowerLimit = true;
        requestOut->powerLimitPct = 100;
    }
    if (previousIntent->hasFan && !nextIntent->hasFan) {
        requestOut->hasFan = true;
        requestOut->fanAuto = true;
        requestOut->fanMode = FAN_MODE_AUTO;
        requestOut->fanPercent = 0;
        memset(&requestOut->fanCurve, 0, sizeof(requestOut->fanCurve));
    }
    return true;
}

struct ServiceLifecycleIdentity {
    gc_bool8 valid;
    gc_u32 sessionId;
    gc_u64 authenticationId;
    char sid[184];
};

static inline bool service_lifecycle_identity_equal(
    const ServiceLifecycleIdentity* a,
    const ServiceLifecycleIdentity* b)
{
    if (!a || !b || !a->valid || !b->valid) return false;
    return a->sessionId == b->sessionId &&
           a->authenticationId == b->authenticationId &&
           a->sid[0] && b->sid[0] && strcmp(a->sid, b->sid) == 0;
}

static inline bool service_lifecycle_identity_equal_session_and_user(
    const ServiceLifecycleIdentity* a,
    const ServiceLifecycleIdentity* b)
{
    // ------------------------------------------------------------------
    // Security rationale: ignores authentication LUID deliberately.
    //
    // The authenticationId (TOKEN_STATISTICS.AuthenticationId) extracted
    // from OpenProcessToken at handoff time can differ from the one
    // obtained via WTSQueryUserToken at apply time on the same session
    // (early-boot token refresh, Fast Startup, specific Windows configs).
    //
    // Comparing only sessionId + user SID is safe because:
    //   1. WTS_SESSION_LOGOFF already cancels pending logon state when
    //      the logon session ends — the auth LUID check was redundant.
    //   2. The reducer's LOGON event sequencing replaces old pending
    //      state when a new LOGON arrives for the same session/user.
    //   3. sessionId + SID still prevent cross-user and cross-session
    //      misapplication (Fast User Switch, different user, etc.).
    //
    // Without this relaxed check, scheduled-task handoffs silently fail
    // on early boot when the task process token and the live session
    // token report different authentication LUIDs for the same user.
    // ------------------------------------------------------------------
    if (!a || !b || !a->valid || !b->valid) return false;
    return a->sessionId == b->sessionId &&
           a->sid[0] && b->sid[0] && strcmp(a->sid, b->sid) == 0;
}

enum ServiceLifecycleEventType {
    SERVICE_LIFECYCLE_EVENT_NONE = 0,
    SERVICE_LIFECYCLE_EVENT_WTS_LOGON,
    SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF,
    SERVICE_LIFECYCLE_EVENT_LOGOFF,
    SERVICE_LIFECYCLE_EVENT_PREREQUISITE_SIGNAL,
    SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_STARTED,
    SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_FINISHED,
    SERVICE_LIFECYCLE_EVENT_SUSPEND,
    SERVICE_LIFECYCLE_EVENT_RESUME,
    SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED,
    SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED,
    SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY,
    SERVICE_LIFECYCLE_EVENT_DRIVER_PROOF_SIGNAL,
    SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED,
    SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_FINISHED,
    SERVICE_LIFECYCLE_EVENT_DEVNODES_CHANGED,
    SERVICE_LIFECYCLE_EVENT_EXPLICIT_SUPERSEDE,
    SERVICE_LIFECYCLE_EVENT_LOCKOUT,
    SERVICE_LIFECYCLE_EVENT_STOP,
};

struct ServiceLifecycleEvent {
    ServiceLifecycleEventType type;
    ServiceLifecycleIdentity identity;
    gc_bool8 success;
    gc_bool8 writeAttempted;
    gc_bool8 driverProofReady;
    gc_bool8 controlledRecoveryValidated;
};

struct ServiceLifecycleState {
    gc_bool8 stopped;
    gc_bool8 lockedOut;
    gc_bool8 logonPending;
    gc_bool8 logonWriteIssued;
    gc_bool8 standbyPending;
    gc_bool8 standbyWriteIssued;
    gc_bool8 driverPending;
    gc_bool8 driverWriteIssued;
    gc_bool8 suspendArmed;
    gc_u64 logonGeneration;
    gc_u64 suspendGeneration;
    gc_u64 resumedSuspendGeneration;
    ServiceLifecycleTrigger pendingLogonTrigger;
    ServiceLifecycleIdentity pendingLogonIdentity;
    ServiceLifecycleIdentity lastAppliedLogonIdentity;
};

struct ServiceLifecycleDecision {
    gc_bool8 wakeWorker;
    gc_bool8 attemptLogonPrerequisites;
    gc_bool8 authorizeLogonWrite;
    gc_bool8 authorizeStandbyWrite;
    gc_bool8 authorizeDriverWrite;
    gc_bool8 readOnlyReenumerate;
    gc_bool8 coalesced;
    gc_bool8 cancelled;
    ServiceLifecycleTrigger trigger;
    ServiceLifecycleResult result;
};

static inline ServiceLifecycleDecision service_lifecycle_reduce(
    ServiceLifecycleState* state,
    const ServiceLifecycleEvent* event)
{
    ServiceLifecycleDecision decision = {};
    if (!state || !event) return decision;

    if (event->type == SERVICE_LIFECYCLE_EVENT_STOP) {
        state->stopped = true;
        state->logonPending = false;
        state->standbyPending = false;
        state->driverPending = false;
        state->pendingLogonTrigger = SERVICE_LIFECYCLE_TRIGGER_NONE;
        memset(&state->pendingLogonIdentity, 0, sizeof(state->pendingLogonIdentity));
        decision.cancelled = true;
        return decision;
    }
    if (state->stopped) return decision;

    if (event->type == SERVICE_LIFECYCLE_EVENT_LOCKOUT) {
        state->lockedOut = true;
        state->logonPending = false;
        state->standbyPending = false;
        state->driverPending = false;
        state->pendingLogonTrigger = SERVICE_LIFECYCLE_TRIGGER_NONE;
        memset(&state->pendingLogonIdentity, 0, sizeof(state->pendingLogonIdentity));
        decision.cancelled = true;
        decision.result = SERVICE_LIFECYCLE_RESULT_LOCKED_OUT;
        return decision;
    }
    if (event->type == SERVICE_LIFECYCLE_EVENT_EXPLICIT_SUPERSEDE) {
        state->logonPending = false;
        state->standbyPending = false;
        state->driverPending = false;
        state->logonWriteIssued = false;
        state->standbyWriteIssued = false;
        state->driverWriteIssued = false;
        state->pendingLogonTrigger = SERVICE_LIFECYCLE_TRIGGER_NONE;
        memset(&state->pendingLogonIdentity, 0, sizeof(state->pendingLogonIdentity));
        decision.cancelled = true;
        decision.result = SERVICE_LIFECYCLE_RESULT_SUPERSEDED;
        return decision;
    }

    switch (event->type) {
        case SERVICE_LIFECYCLE_EVENT_WTS_LOGON:
        case SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF: {
            decision.trigger = event->type == SERVICE_LIFECYCLE_EVENT_TASK_HANDOFF
                ? SERVICE_LIFECYCLE_TRIGGER_TASK_HANDOFF
                : SERVICE_LIFECYCLE_TRIGGER_WTS_LOGON;
            if (state->lockedOut) {
                decision.result = SERVICE_LIFECYCLE_RESULT_LOCKED_OUT;
                break;
            }
            if (!event->identity.valid) {
                decision.result = SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY;
                break;
            }
            if (service_lifecycle_identity_equal(
                    &state->lastAppliedLogonIdentity, &event->identity)) {
                decision.coalesced = true;
                decision.result = SERVICE_LIFECYCLE_RESULT_APPLIED;
                break;
            }
            if (state->logonPending && service_lifecycle_identity_equal(
                    &state->pendingLogonIdentity, &event->identity)) {
                decision.coalesced = true;
            } else {
                state->logonPending = true;
                state->logonWriteIssued = false;
                state->pendingLogonIdentity = event->identity;
                state->pendingLogonTrigger = decision.trigger;
                state->logonGeneration++;
                if (state->logonGeneration == 0) state->logonGeneration = 1;
            }
            decision.wakeWorker = true;
            decision.attemptLogonPrerequisites = !state->logonWriteIssued;
            decision.result = SERVICE_LIFECYCLE_RESULT_PENDING;
            break;
        }

        case SERVICE_LIFECYCLE_EVENT_LOGOFF:
            if (service_lifecycle_identity_equal(
                    &state->pendingLogonIdentity, &event->identity)) {
                state->logonPending = false;
                state->logonWriteIssued = false;
                memset(&state->pendingLogonIdentity, 0, sizeof(state->pendingLogonIdentity));
                state->pendingLogonTrigger = SERVICE_LIFECYCLE_TRIGGER_NONE;
                decision.cancelled = true;
            }
            // Clear completed identity too: a later authentication using a
            // reused numeric session (and even the same account) is a new login.
            if (service_lifecycle_identity_equal(
                    &state->lastAppliedLogonIdentity, &event->identity)) {
                memset(&state->lastAppliedLogonIdentity, 0, sizeof(state->lastAppliedLogonIdentity));
            }
            decision.result = decision.cancelled
                ? SERVICE_LIFECYCLE_RESULT_CANCELLED_LOGOFF
                : SERVICE_LIFECYCLE_RESULT_NONE;
            break;

        case SERVICE_LIFECYCLE_EVENT_PREREQUISITE_SIGNAL:
            if (state->logonPending && !state->logonWriteIssued && !state->lockedOut) {
                decision.wakeWorker = true;
                decision.attemptLogonPrerequisites = true;
                decision.result = SERVICE_LIFECYCLE_RESULT_PENDING;
            }
            break;

        case SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_STARTED:
            if (state->logonPending && !state->logonWriteIssued &&
                service_lifecycle_identity_equal(
                    &state->pendingLogonIdentity, &event->identity) &&
                !state->lockedOut) {
                state->logonWriteIssued = true;
                decision.authorizeLogonWrite = true;
                decision.trigger = state->pendingLogonTrigger;
            }
            break;

        case SERVICE_LIFECYCLE_EVENT_LOGON_WRITE_FINISHED:
            if (state->logonPending && state->logonWriteIssued &&
                service_lifecycle_identity_equal(
                    &state->pendingLogonIdentity, &event->identity)) {
                state->logonWriteIssued = false;
                if (!event->writeAttempted) {
                    decision.result = SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY;
                } else {
                    if (event->success) {
                        state->lastAppliedLogonIdentity = state->pendingLogonIdentity;
                        decision.result = SERVICE_LIFECYCLE_RESULT_APPLIED;
                    } else {
                        decision.result = SERVICE_LIFECYCLE_RESULT_FAILED;
                    }
                    // A real hardware write is terminal and never retried.
                    state->logonPending = false;
                    memset(&state->pendingLogonIdentity, 0, sizeof(state->pendingLogonIdentity));
                    state->pendingLogonTrigger = SERVICE_LIFECYCLE_TRIGGER_NONE;
                }
            }
            break;

        case SERVICE_LIFECYCLE_EVENT_SUSPEND:
            state->suspendGeneration++;
            if (state->suspendGeneration == 0) state->suspendGeneration = 1;
            state->suspendArmed = true;
            break;

        case SERVICE_LIFECYCLE_EVENT_RESUME:
            if (!state->lockedOut && state->suspendArmed &&
                state->resumedSuspendGeneration != state->suspendGeneration) {
                state->resumedSuspendGeneration = state->suspendGeneration;
                state->suspendArmed = false;
                if (state->driverPending) {
                    decision.coalesced = true; // confirmed recovery dominates
                } else {
                    state->standbyPending = true;
                    state->standbyWriteIssued = false;
                    decision.wakeWorker = true;
                    decision.trigger = SERVICE_LIFECYCLE_TRIGGER_STANDBY_RESUME;
                    decision.result = SERVICE_LIFECYCLE_RESULT_PENDING;
                }
            } else if (!state->lockedOut && state->standbyPending &&
                !state->standbyWriteIssued) {
                // Windows commonly sends AUTOMATIC first and SUSPEND later for
                // one resume. Coalesce them into one generation/write, but use
                // the later notification as a real readiness cue if the first
                // serialized probe found the driver not ready.
                decision.coalesced = true;
                if (!state->driverPending) {
                    decision.wakeWorker = true;
                    decision.trigger =
                        SERVICE_LIFECYCLE_TRIGGER_STANDBY_RESUME;
                    decision.result = SERVICE_LIFECYCLE_RESULT_PENDING;
                }
            }
            break;

        case SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_STARTED:
            if (state->standbyPending && !state->standbyWriteIssued &&
                !state->driverPending && !state->lockedOut) {
                state->standbyWriteIssued = true;
                decision.authorizeStandbyWrite = true;
                decision.trigger = SERVICE_LIFECYCLE_TRIGGER_STANDBY_RESUME;
            }
            break;

        case SERVICE_LIFECYCLE_EVENT_STANDBY_WRITE_FINISHED:
            if (state->standbyPending && state->standbyWriteIssued) {
                state->standbyWriteIssued = false;
                if (!event->writeAttempted) {
                    decision.result = SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY;
                } else {
                    state->standbyPending = false;
                    decision.result = event->success
                        ? SERVICE_LIFECYCLE_RESULT_APPLIED
                        : SERVICE_LIFECYCLE_RESULT_FAILED;
                }
            }
            break;

        case SERVICE_LIFECYCLE_EVENT_DRIVER_RECOVERY:
            if (!state->lockedOut) {
                if (state->driverPending) {
                    decision.coalesced = true;
                } else {
                    state->driverPending = true;
                    state->driverWriteIssued = false;
                }
                state->standbyPending = false;
                state->standbyWriteIssued = false;
                decision.wakeWorker = event->driverProofReady && !state->driverWriteIssued;
                decision.trigger = SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY;
                decision.result = SERVICE_LIFECYCLE_RESULT_PENDING;
            } else {
                decision.result = SERVICE_LIFECYCLE_RESULT_LOCKED_OUT;
            }
            break;

        case SERVICE_LIFECYCLE_EVENT_DRIVER_PROOF_SIGNAL:
            if (state->driverPending && !state->driverWriteIssued &&
                event->driverProofReady && !state->lockedOut) {
                decision.wakeWorker = true;
                decision.trigger = SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY;
            }
            break;

        case SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_STARTED:
            if (state->driverPending && !state->driverWriteIssued &&
                event->driverProofReady && event->controlledRecoveryValidated &&
                !state->lockedOut) {
                state->driverWriteIssued = true;
                decision.authorizeDriverWrite = true;
                decision.trigger = SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY;
            }
            break;

        case SERVICE_LIFECYCLE_EVENT_DRIVER_WRITE_FINISHED:
            if (state->driverPending && state->driverWriteIssued) {
                state->driverWriteIssued = false;
                if (!event->writeAttempted) {
                    decision.result = SERVICE_LIFECYCLE_RESULT_TRANSIENT_NOT_READY;
                } else {
                    state->driverPending = false;
                    decision.result = event->success
                        ? SERVICE_LIFECYCLE_RESULT_APPLIED
                        : SERVICE_LIFECYCLE_RESULT_FAILED;
                }
            }
            break;

        case SERVICE_LIFECYCLE_EVENT_DEVNODES_CHANGED:
            decision.readOnlyReenumerate = true;
            // Global devnode churn is diagnostic only.  It never sets a
            // restore-pending bit or authorizes a write.
            break;

        default:
            break;
    }
    return decision;
}
