// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// service_protocol_validation.h — trust-boundary validation for a request, the
// published state envelope, and the response.
//
// Split out of service_protocol.h to keep that file inside the source-size
// ratchet; it is included from the bottom of service_protocol.h and is not
// meant to be included directly.  Everything here is pure and header-only, so
// the regression harness asserts it on both hosts.
//
// The rule these functions encode: a request and a response are each either
// wholly coherent or rejected.  Partial acceptance is what let a half-populated
// envelope be read as READY.

#ifndef GREEN_CURVE_SERVICE_PROTOCOL_VALIDATION_H
#define GREEN_CURVE_SERVICE_PROTOCOL_VALIDATION_H

// Why a request is refused, or nullptr when it is well formed.
//
// This IS the rule set; validate_service_request_for_ipc() below is exactly
// "no rule is broken", so a log line and the accept/refuse decision cannot
// drift apart.  The reason exists because the first two live rejections of this
// contract -- an installer settings restore and the reset that --service-remove
// performs -- logged only "malformed request", naming neither the field nor the
// caller's mistake, and both were mis-read as transport faults for weeks.
static inline const char* service_request_reject_reason(
    const ServiceRequest* r) {
    if (!r) return "no request";
    if (r->magic != SERVICE_PROTOCOL_MAGIC) return "wrong protocol magic";
    if (r->version != SERVICE_PROTOCOL_VERSION) return "wrong protocol version";
    if (r->command < SERVICE_CMD_PING ||
        r->command > SERVICE_CMD_SET_UPDATE_POLICY)
        return "unknown command";
    if (r->startupMode >= SERVICE_STARTUP_POLICY_MODE_COUNT)
        return "unknown startup policy mode";
    if (!r->callerPid) return "missing caller pid";
    if (r->resetOcBeforeApply > 1u) return "non-boolean resetOcBeforeApply";
    if (r->applyOrigin > SERVICE_APPLY_ORIGIN_DRIVER_RECOVERY)
        return "unknown apply origin";
    if (r->profileSource > SERVICE_PROFILE_SOURCE_AD_HOC)
        return "unknown profile source";
    if (r->profileSlot > 255u) return "profile slot out of range";
    if (!service_wire_string_is_terminated(
            r->source, (unsigned int)sizeof(r->source)))
        return "unterminated source string";
    if (!service_wire_string_is_terminated(
            r->path, (unsigned int)sizeof(r->path)))
        return "unterminated path string";
    if (!service_wire_string_is_terminated(r->targetGpu.name,
            (unsigned int)sizeof(r->targetGpu.name)))
        return "unterminated target GPU name";
    if (!service_gpu_bool_fields_valid(&r->targetGpu))
        return "non-boolean target GPU flags";
    if (!service_desired_bool_fields_valid(&r->desired))
        return "non-boolean desired settings flags";
    const gc_u32 allowedFlags = SERVICE_REQUEST_FLAG_INTERACTIVE |
        SERVICE_REQUEST_FLAG_SHARED_SLOT |
        (SERVICE_REQUEST_SHARED_SLOT_MASK <<
            SERVICE_REQUEST_SHARED_SLOT_SHIFT);
    if ((r->flags & ~allowedFlags) != 0) return "unknown request flags";
    bool anyPrecondition = r->expectedServiceInstanceId ||
        r->expectedGpuGeneration || r->expectedTopologySignature;
    bool basePreconditions = r->expectedServiceInstanceId &&
        r->expectedGpuGeneration;
    if (anyPrecondition && !basePreconditions)
        return "incomplete reconnect preconditions";
    if (anyPrecondition && !r->targetGpu.valid)
        return "reconnect preconditions without an exact target GPU";
    if (r->command == SERVICE_CMD_APPLY) {
        gc_u32 mutationDomains = service_desired_mutation_domains(&r->desired);
        if (!r->operationId) return "apply without an operation id";
        // The failure this names is the one that silently cost two upgrades
        // their settings: a client that read a READY envelope but never carried
        // its identity onto the request it built from it.
        if (!basePreconditions)
            return "apply without service instance / GPU generation preconditions";
        if (!r->targetGpu.valid) return "apply without an exact target GPU";
        if (service_mutation_domains_require_vf(mutationDomains) &&
            !r->expectedTopologySignature)
            return "VF apply without a curve topology signature";
        if (!service_apply_origin_is_client_apply(
                (ServiceApplyOrigin)r->applyOrigin))
            return "apply origin is not a client apply";
        if ((r->flags & SERVICE_REQUEST_FLAG_SHARED_SLOT) != 0) {
            gc_u32 encodedSlot = (r->flags >>
                SERVICE_REQUEST_SHARED_SLOT_SHIFT) &
                SERVICE_REQUEST_SHARED_SLOT_MASK;
            if (!encodedSlot ||
                r->profileSource != SERVICE_PROFILE_SOURCE_SHARED_SLOT ||
                r->profileSlot != encodedSlot)
                return "shared-slot apply disagrees with its encoded slot";
        }
    } else if (r->command == SERVICE_CMD_RESET) {
        if (!r->operationId) return "reset without an operation id";
        if (!basePreconditions || !r->expectedTopologySignature)
            return "reset without complete reconnect preconditions";
        if (!r->targetGpu.valid) return "reset without an exact target GPU";
        if (r->flags || r->resetOcBeforeApply ||
            r->profileSource || r->profileSlot)
            return "reset carries settings or profile fields";
    } else if (r->command == SERVICE_CMD_GET_OPERATION_RESULT) {
        if (!r->operationId) return "operation query without an operation id";
        if (r->flags || anyPrecondition)
            return "operation query carries mutation fields";
    } else if (r->command == SERVICE_CMD_LOGON_HANDOFF) {
        if (r->applyOrigin != SERVICE_APPLY_ORIGIN_LOGON)
            return "logon handoff without the logon origin";
        if (r->flags || r->operationId || anyPrecondition)
            return "logon handoff carries mutation fields";
    } else if (r->command == SERVICE_CMD_SET_STARTUP_POLICY) {
        // A boot-apply policy is configuration, not a mutation: it carries no
        // operation id and no live-state preconditions, because it is allowed
        // while the GPU is degraded or not yet selected. PROFILE must name a
        // real slot and an exact GPU; the other modes must carry neither, so a
        // stale target cannot ride along on a "none" request.
        if (r->operationId || r->flags || r->resetOcBeforeApply ||
            r->applyOrigin || r->profileSource || anyPrecondition)
            return "startup policy carries mutation fields";
        if (r->startupMode == SERVICE_STARTUP_POLICY_PROFILE) {
            if (!r->profileSlot || r->profileSlot > (gc_u32)CONFIG_NUM_SLOTS)
                return "startup profile policy without a real slot";
            if (!r->targetGpu.valid)
                return "startup profile policy without an exact target GPU";
        } else if (r->profileSlot || r->targetGpu.valid) {
            return "non-profile startup policy carries a slot or GPU";
        }
    } else if (r->command == SERVICE_CMD_REFRESH_STARTUP_PROFILE) {
        // Content-only update of an already-bound policy. It must name the slot
        // it claims to refresh and must carry NO GPU and NO startup mode: the
        // binding and the mode belong to the stored record, and accepting them
        // here would turn a refresh into a re-bind.
        if (r->operationId || r->flags || r->resetOcBeforeApply ||
            r->applyOrigin || r->profileSource || r->startupMode ||
            r->targetGpu.valid || anyPrecondition)
            return "startup profile refresh would re-bind the policy";
        if (!r->profileSlot || r->profileSlot > (gc_u32)CONFIG_NUM_SLOTS)
            return "startup profile refresh without a real slot";
    } else if (r->command == SERVICE_CMD_RESUME_RESTORE) {
        // A resume notification carries nothing at all. The settings, the write
        // target and the decision to write are the daemon's; this command is
        // only the edge that says the machine is back. Accepting a target GPU
        // or a settings payload here would turn a group-reachable socket
        // message into an apply nobody typed.
        if (r->operationId || r->flags || r->resetOcBeforeApply ||
            r->applyOrigin || r->profileSource || r->profileSlot ||
            r->startupMode || r->targetGpu.valid || anyPrecondition)
            return "resume restore carries mutation fields";
        // The daemon never reads `desired` for this command, but refusing a
        // populated one keeps the contract checkable at the boundary instead of
        // resting on a handler that happens not to look.
        if (service_desired_mutation_domains(&r->desired) != 0)
            return "resume restore carries settings";
    } else if (r->command == SERVICE_CMD_GET_UPDATE_STATE ||
               r->command == SERVICE_CMD_CHECK_FOR_UPDATE ||
               r->command == SERVICE_CMD_INSTALL_UPDATE ||
               r->command == SERVICE_CMD_SET_UPDATE_POLICY) {
        // THE UPDATER'S TRUST BOUNDARY, stated as a wire rule.
        //
        // The service ends a successful update by launching an installer with
        // SYSTEM rights. Everything that decides WHICH file that is -- the URL,
        // the release version, the asset name, the digest, the staging path --
        // is resolved by the service from constants compiled into the binary
        // and from a manifest whose signature it verified against a key that
        // has never been in GitHub Actions. None of it may come from the
        // caller, because the caller is an unprivileged GUI: a command that
        // accepted "which file" would be a local privilege escalation however
        // carefully the named file were checked afterwards.
        //
        // So these commands carry NO path, NO settings, NO GPU, NO profile
        // identity and NO preconditions. Refusing them here rather than
        // ignoring them in the handler keeps the contract checkable at the
        // boundary instead of resting on four handlers that happen not to look.
        if (r->operationId || r->flags || r->resetOcBeforeApply ||
            r->applyOrigin || r->profileSource || r->profileSlot ||
            r->startupMode || r->targetGpu.valid || anyPrecondition)
            return "update command carries mutation fields";
        if (service_desired_mutation_domains(&r->desired) != 0)
            return "update command carries settings";
        if (r->path[0]) return "update command carries a path";
        if (r->command == SERVICE_CMD_SET_UPDATE_POLICY) {
            // The only update command with client data, and it is two integers.
            // An out-of-range interval is refused rather than clamped: the
            // clamp exists for a hand-edited config file, whereas a request is
            // machine-written and a bad one means the client is confused.
            if (r->updateAutoCheck > GC_UPDATE_AUTO_CHECK_ON)
                return "unknown update auto-check mode";
            if (r->updateIntervalSeconds < GC_UPDATE_INTERVAL_MIN_SECONDS ||
                r->updateIntervalSeconds > GC_UPDATE_INTERVAL_MAX_SECONDS)
                return "update interval out of range";
        } else if (r->updateAutoCheck || r->updateIntervalSeconds) {
            return "update command carries policy fields";
        }
    } else if (r->operationId || r->flags || r->resetOcBeforeApply ||
        r->applyOrigin || r->profileSource || r->profileSlot ||
        r->startupMode || r->updateAutoCheck || r->updateIntervalSeconds ||
        anyPrecondition) {
        return "read-only command carries mutation fields";
    }
    return nullptr;
}

static inline bool validate_service_request_for_ipc(ServiceRequest* r) {
    if (service_request_reject_reason(r) != nullptr) return false;
    validate_gpu_adapter_info_for_ipc(&r->targetGpu);
    validate_desired_settings_for_ipc(&r->desired);
    return true;
}

static inline bool service_snapshot_bool_fields_valid(
    const ServiceSnapshot* snapshot) {
    if (!snapshot) return false;
    const gc_bool8* flags[] = {
        &snapshot->initialized, &snapshot->loaded, &snapshot->fanSupported,
        &snapshot->fanRangeKnown, &snapshot->fanIsAuto,
        &snapshot->fanCurveRuntimeActive, &snapshot->fanFixedRuntimeActive,
        &snapshot->gpuOffsetRangeKnown, &snapshot->memOffsetRangeKnown,
        &snapshot->curveOffsetRangeKnown, &snapshot->gpuTemperatureValid,
        &snapshot->vfReadSupported, &snapshot->vfWriteSupported,
        &snapshot->vfBestGuess, &snapshot->hasLock,
        &snapshot->lockTracksAnchor,
        &snapshot->selectedAdapterOrdinalFallback,
        &snapshot->lastApplyUsedGpuOffset, &snapshot->serviceInRecovery,
        &snapshot->serviceReapplyInProgress,
        &snapshot->health.vfSnapshotFresh,
        &snapshot->health.recoveryAttempted,
        &snapshot->health.recoverySucceeded,
    };
    for (const gc_bool8* flag : flags)
        if (*flag > 1) return false;
    for (unsigned int i = 0; i < snapshot->adapterCount &&
            i < MAX_GPU_ADAPTERS; ++i) {
        const GpuAdapterInfo& gpu = snapshot->adapters[i];
        if (gpu.valid > 1 || gpu.pciInfoValid > 1 ||
            gpu.vfReadSupported > 1 || gpu.vfWriteSupported > 1 ||
            gpu.vfBestGuess > 1) return false;
    }
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i)
        if (snapshot->activeFanCurve.points[i].enabled > 1) return false;
    return true;
}

static inline bool service_control_bool_fields_valid(
    const ControlState* control) {
    if (!control || control->valid > 1 || control->hasGpuOffset > 1 ||
        control->gpuOffsetReadbackValid > 1 ||
        control->hasMemOffset > 1 || control->memOffsetReadbackValid > 1 ||
        control->hasPowerLimit > 1 || control->powerLimitReadbackValid > 1 ||
        control->hasFan > 1 || control->fanPolicyReadbackValid > 1 ||
        control->fanTargetReadbackValid > 1) return false;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; ++i)
        if (control->fanCurve.points[i].enabled > 1) return false;
    return true;
}

// Canonicalize every boolean and bounded field a published snapshot carries.
//
// Moved here from service_protocol.h (2026-08-14) when the v19 updater pushed
// that file past its size limit.  This is where it belonged anyway: this header
// is by its own description the trust-boundary validation for a request, the
// published state envelope, and the response, and a snapshot validator is all
// three's neighbour rather than a wire declaration.
static inline void validate_service_snapshot_for_ipc(ServiceSnapshot* s) {
    if (!s) return;
    canonicalize_gc_bool8(&s->initialized);
    canonicalize_gc_bool8(&s->loaded);
    canonicalize_gc_bool8(&s->fanSupported);
    canonicalize_gc_bool8(&s->fanRangeKnown);
    canonicalize_gc_bool8(&s->fanIsAuto);
    canonicalize_gc_bool8(&s->fanCurveRuntimeActive);
    canonicalize_gc_bool8(&s->fanFixedRuntimeActive);
    canonicalize_gc_bool8(&s->gpuOffsetRangeKnown);
    canonicalize_gc_bool8(&s->memOffsetRangeKnown);
    canonicalize_gc_bool8(&s->curveOffsetRangeKnown);
    canonicalize_gc_bool8(&s->gpuTemperatureValid);
    canonicalize_gc_bool8(&s->vfReadSupported);
    canonicalize_gc_bool8(&s->vfWriteSupported);
    canonicalize_gc_bool8(&s->vfBestGuess);
    canonicalize_gc_bool8(&s->hasLock);
    canonicalize_gc_bool8(&s->lockTracksAnchor);
    canonicalize_gc_bool8(&s->selectedAdapterOrdinalFallback);
    canonicalize_gc_bool8(&s->lastApplyUsedGpuOffset);
    canonicalize_gc_bool8(&s->serviceInRecovery);
    canonicalize_gc_bool8(&s->serviceReapplyInProgress);
    canonicalize_gc_bool8(&s->health.vfSnapshotFresh);
    canonicalize_gc_bool8(&s->health.recoveryAttempted);
    canonicalize_gc_bool8(&s->health.recoverySucceeded);
    if (s->activeProfileSource > SERVICE_PROFILE_SOURCE_AD_HOC) {
        s->activeProfileSource = SERVICE_PROFILE_SOURCE_NONE;
        s->activeProfileSlot = 0;
    }
    if (s->activeProfileSlot > 255u) s->activeProfileSlot = 0;
    if (s->lastLifecycleTrigger > SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY) {
        s->lastLifecycleTrigger = SERVICE_LIFECYCLE_TRIGGER_NONE;
    }
    if (s->lastLifecycleResult > SERVICE_LIFECYCLE_RESULT_FAILED) {
        s->lastLifecycleResult = SERVICE_LIFECYCLE_RESULT_NONE;
    }
    if (s->autoRestoreLockoutReason > SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED) {
        s->autoRestoreLockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED;
    }
    if (s->adapterCount > MAX_GPU_ADAPTERS) s->adapterCount = MAX_GPU_ADAPTERS;
    if (s->selectedAdapterIndex >= MAX_GPU_ADAPTERS) s->selectedAdapterIndex = 0;
    for (unsigned int i = 0; i < s->adapterCount && i < MAX_GPU_ADAPTERS; i++) {
        validate_gpu_adapter_info_for_ipc(&s->adapters[i]);
    }
    validate_fan_curve_flags_for_ipc(&s->activeFanCurve);
}

static inline bool validate_service_state_envelope_for_ipc(
    ServiceStateEnvelope* state, ServiceSnapshot* snapshot,
    DesiredSettings* desired, ControlState* controlState) {
    if (!state || !snapshot || !desired || !controlState) return false;
    if (state->activeDesiredValid > 1 ||
        state->startupPolicyMode >= SERVICE_STARTUP_POLICY_MODE_COUNT ||
        state->startupPolicySlot > (gc_u32)CONFIG_NUM_SLOTS ||
        // A published slot is meaningful only for the PROFILE mode; anything
        // else reporting one is an incoherent envelope, not a benign extra.
        ((state->startupPolicyMode == SERVICE_STARTUP_POLICY_PROFILE) !=
         (state->startupPolicySlot != 0)) ||
        !service_snapshot_bool_fields_valid(snapshot) ||
        !service_desired_bool_fields_valid(desired) ||
        !service_control_bool_fields_valid(controlState)) return false;
    for (unsigned int i = 0; i < sizeof(state->reservedBool); ++i)
        if (state->reservedBool[i] != 0) return false;
    if (snapshot->adapterCount > MAX_GPU_ADAPTERS ||
        snapshot->numPopulated < 0 || snapshot->numPopulated > VF_NUM_POINTS ||
        snapshot->fanCount > MAX_GPU_FANS ||
        (snapshot->adapterCount > 0 &&
         snapshot->selectedAdapterIndex >= snapshot->adapterCount) ||
        snapshot->gpuFamily < GPU_FAMILY_UNKNOWN ||
        snapshot->gpuFamily > GPU_FAMILY_BLACKWELL ||
        snapshot->lockMode < LOCK_MODE_NONE ||
        snapshot->lockMode > LOCK_MODE_HARD ||
        snapshot->activeFanMode < FAN_MODE_AUTO ||
        snapshot->activeFanMode > FAN_MODE_CURVE ||
        snapshot->activeProfileSource > SERVICE_PROFILE_SOURCE_AD_HOC ||
        snapshot->activeProfileSlot > 255u ||
        snapshot->lastLifecycleTrigger >
            SERVICE_LIFECYCLE_TRIGGER_DRIVER_RECOVERY ||
        snapshot->lastLifecycleResult > SERVICE_LIFECYCLE_RESULT_FAILED ||
        snapshot->autoRestoreLockoutReason >
            SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED ||
        snapshot->health.reason > SERVICE_GPU_HEALTH_STATE_UNCERTAIN ||
        snapshot->health.architectureSource >
            SERVICE_GPU_ARCH_SOURCE_FUTURE_GUESS ||
        (snapshot->health.availableMutationDomains &
            ~SERVICE_MUTATION_DOMAIN_ALL) != 0 ||
        snapshot->health.capabilityMemoryTopology >
            SERVICE_GPU_MEMORY_TOPOLOGY_MAX ||
        (snapshot->health.capabilityDomainsPacked &
            ~SERVICE_GPU_CAPABILITY_PACKED_MASK) != 0 ||
        !service_wire_string_is_terminated(snapshot->health.detail,
            (unsigned int)sizeof(snapshot->health.detail)) ||
        !service_wire_string_is_terminated(snapshot->gpuName,
            (unsigned int)sizeof(snapshot->gpuName)) ||
        !service_wire_string_is_terminated(snapshot->ownerUser,
            (unsigned int)sizeof(snapshot->ownerUser))) return false;
    for (unsigned int i = 0; i < snapshot->adapterCount; ++i) {
        if (!service_wire_string_is_terminated(snapshot->adapters[i].name,
                (unsigned int)sizeof(snapshot->adapters[i].name)) ||
            snapshot->adapters[i].family < GPU_FAMILY_UNKNOWN ||
            snapshot->adapters[i].family > GPU_FAMILY_BLACKWELL) return false;
    }
    if ((controlState->hasFan &&
         (controlState->fanMode < FAN_MODE_AUTO ||
          controlState->fanMode > FAN_MODE_CURVE ||
          controlState->fanFixedPercent < 0 ||
          controlState->fanFixedPercent > 100 ||
          controlState->fanCurrentPercent < 0 ||
          controlState->fanCurrentPercent > 100))) return false;
    canonicalize_gc_bool8(&state->activeDesiredValid);
    validate_service_snapshot_for_ipc(snapshot);
    validate_desired_settings_for_ipc(desired);
    validate_control_state_for_ipc(controlState);
    if (state->gpuPhase > SERVICE_GPU_PHASE_DEGRADED) return false;
    if ((state->validSections & ~SERVICE_STATE_SECTION_ALL) != 0) return false;
    if (state->serviceInstanceId == 0 || state->stateRevision == 0 ||
        state->gpuGeneration == 0) return false;
    if (state->gpuPhase == SERVICE_GPU_PHASE_READY &&
        (state->validSections & SERVICE_STATE_SECTION_READY_REQUIRED) !=
            SERVICE_STATE_SECTION_READY_REQUIRED) return false;
    if (state->gpuPhase == SERVICE_GPU_PHASE_READY &&
        (!snapshot->initialized || !snapshot->loaded ||
         snapshot->numPopulated <= 0)) return false;
    if ((state->validSections & SERVICE_STATE_SECTION_CURVE_TOPOLOGY) != 0 &&
        state->topologySignature == 0) return false;
    if ((state->validSections & SERVICE_STATE_SECTION_CURVE_TOPOLOGY) == 0 &&
        state->topologySignature != 0) return false;
    if (state->activeDesiredValid &&
        (state->validSections & SERVICE_STATE_SECTION_ACTIVE_INTENT) == 0)
        return false;
    if ((state->validSections & SERVICE_STATE_SECTION_APPLIED_CONTROLS) != 0 &&
        !controlState->valid) return false;
    if ((state->validSections & SERVICE_STATE_SECTION_ADAPTER_IDENTITY) != 0 &&
        (snapshot->adapterCount == 0 ||
         !snapshot->adapters[snapshot->selectedAdapterIndex].valid))
        return false;
    if ((state->validSections & SERVICE_STATE_SECTION_CURVE_TOPOLOGY) != 0) {
        int populated = 0;
        for (int i = 0; i < VF_NUM_POINTS; ++i)
            if (snapshot->curve[i].freq_kHz != 0) ++populated;
        if (populated != snapshot->numPopulated || populated == 0 ||
            state->topologySignature !=
                service_snapshot_topology_signature(snapshot)) return false;
    }
    return true;
}

// True when a response carries no state payload whatsoever.
//
// A refusal issued before the caller cleared the session/PID/integrity gates
// deliberately publishes nothing: the pipe ACL admits every local user, and
// only an authorized caller receives authoritative state.  That is a complete,
// well-formed answer, so it is checked as "all four payload members are still
// zero" -- byte-exact, so a half-populated envelope can never slip through as
// "absent".
static inline bool service_wire_block_is_zero(const void* block, size_t size) {
    const unsigned char* bytes = (const unsigned char*)block;
    for (size_t i = 0; i < size; ++i)
        if (bytes[i] != 0) return false;
    return true;
}

static inline bool service_response_payload_is_absent(
    const ServiceResponse* r) {
    return r &&
        service_wire_block_is_zero(&r->state, sizeof(r->state)) &&
        service_wire_block_is_zero(&r->snapshot, sizeof(r->snapshot)) &&
        service_wire_block_is_zero(&r->desired, sizeof(r->desired)) &&
        service_wire_block_is_zero(&r->controlState, sizeof(r->controlState)) &&
        service_wire_block_is_zero(&r->startupProfile,
            sizeof(r->startupProfile)) &&
        r->startupProfileValid == 0;
}

static inline bool validate_service_update_state_for_ipc(ServiceUpdateState* update) {
    if (!update) return false;
    canonicalize_gc_bool8(&update->packageStaged);
    canonicalize_gc_bool8(&update->packageVerified);
    canonicalize_gc_bool8(&update->installRunning);
    canonicalize_gc_bool8(&update->isInstalledCopy);
    canonicalize_gc_bool8(&update->workerRunning);
    canonicalize_gc_bool8(&update->guiShutdownRequested);
    for (unsigned int i = 0; i < sizeof(update->updateReserved); ++i)
        if (update->updateReserved[i] != 0) return false;
    if (update->phase > SERVICE_UPDATE_PHASE_FAILED) return false;
    if (update->decision > GC_UPDATE_DECISION_NO_ASSET) return false;
    if (update->autoCheck > GC_UPDATE_AUTO_CHECK_ON) return false;
    if (update->intervalSeconds != 0 &&
        (update->intervalSeconds < GC_UPDATE_INTERVAL_MIN_SECONDS ||
         update->intervalSeconds > GC_UPDATE_INTERVAL_MAX_SECONDS)) return false;
    if (update->lastRefusal > GC_UPDATE_INSTALL_ALREADY_RUNNING) return false;
    if (!service_wire_string_is_terminated(update->availableVersion,
            (unsigned int)sizeof(update->availableVersion)) ||
        !service_wire_string_is_terminated(update->installedVersion,
            (unsigned int)sizeof(update->installedVersion)) ||
        !service_wire_string_is_terminated(update->detail,
            (unsigned int)sizeof(update->detail))) return false;
    return true;
}

// The boot-apply snapshot is coherent or the response is damaged: the flag is
// a boolean, its settings pass the same field checks as any other wire
// DesiredSettings, and it may only be advertised while the published policy
// actually is `profile N`.  Without that last rule a client could be handed a
// snapshot for a policy that no longer boots anything.
static inline bool service_response_startup_profile_is_coherent(
    const ServiceResponse* r) {
    if (!r) return false;
    if (r->startupProfileValid > 1) return false;
    for (unsigned int i = 0; i < sizeof(r->startupProfileReserved); ++i)
        if (r->startupProfileReserved[i] != 0) return false;
    if (!r->startupProfileValid)
        return service_wire_block_is_zero(&r->startupProfile,
            sizeof(r->startupProfile));
    return r->state.startupPolicyMode == SERVICE_STARTUP_POLICY_PROFILE &&
        r->state.startupPolicySlot != 0 &&
        service_desired_bool_fields_valid(&r->startupProfile);
}

static inline bool validate_service_response_for_ipc(ServiceResponse* r) {
    if (!r) return false;
    if (r->magic != SERVICE_PROTOCOL_MAGIC ||
        r->version != SERVICE_PROTOCOL_VERSION ||
        r->status > SERVICE_STATUS_STALE_STATE ||
        r->operationState > SERVICE_OPERATION_OUTCOME_UNKNOWN ||
        // Range AND agreement with `status` in one check: a severity that
        // disagrees is not a lesser answer to trust selectively, it means the
        // two fields describe different operations.  Checked before the
        // payload-free refusal shortcut below so it covers every response,
        // including one that carries no state at all.
        !service_outcome_severity_matches_status(r->status,
            r->outcomeSeverity) ||
        r->outcomeSeverityReserved != 0 ||
        !service_wire_string_is_terminated(r->serviceVersion,
            (unsigned int)sizeof(r->serviceVersion)) ||
        !service_wire_string_is_terminated(r->message,
            (unsigned int)sizeof(r->message))) return false;
    if (!validate_service_update_state_for_ipc(&r->update)) return false;
    // A refusal that publishes no state at all is the service's answer, not a
    // damaged one.  Rejecting it here is what turned every protocol-level and
    // authorization-level refusal into "Service response contains an invalid
    // state envelope" on the client, hiding the actual reason and downgrading
    // a definitive refusal into an unknown-outcome transport failure.  A
    // successful response must still carry a coherent envelope.
    if (r->status != SERVICE_STATUS_OK && service_response_payload_is_absent(r))
        return true;
    if (!validate_service_state_envelope_for_ipc(
            &r->state, &r->snapshot, &r->desired, &r->controlState))
        return false;
    // Checked after the envelope because its coherence rule reads the published
    // policy mode out of that envelope.
    if (!service_response_startup_profile_is_coherent(r)) return false;
    if (r->startupProfileValid)
        validate_desired_settings_for_ipc(&r->startupProfile);
    return true;
}

#endif // GREEN_CURVE_SERVICE_PROTOCOL_VALIDATION_H
