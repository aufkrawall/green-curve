// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

struct ImmutablePublishedServiceState {
    ServiceStateEnvelope state;
    ServiceSnapshot snapshot;
    DesiredSettings desired;
    ControlState controlState;
};

static SRWLOCK g_servicePublishedStateLock = SRWLOCK_INIT;
static ImmutablePublishedServiceState g_servicePublishedState = {};

static bool service_initialize_state_identity() {
#ifdef _WIN32
    gc_u64 instanceId = 0;
    for (int attempt = 0; attempt < 4 && instanceId == 0; ++attempt) {
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&instanceId),
                sizeof(instanceId), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            instanceId = 0;
            break;
        }
    }
    if (!instanceId) return false;
    g_serviceInstanceId = instanceId;
    InterlockedExchange64(&g_serviceStateRevision, 0);
    InterlockedExchange64(&g_serviceGpuGeneration, 1);
    InterlockedExchange(&g_serviceGpuPhase, SERVICE_GPU_PHASE_STARTING);
    AcquireSRWLockExclusive(&g_servicePublishedStateLock);
    memset(&g_servicePublishedState, 0, sizeof(g_servicePublishedState));
    g_servicePublishedState.state.serviceInstanceId = instanceId;
    g_servicePublishedState.state.gpuGeneration = 1;
    g_servicePublishedState.state.gpuPhase = SERVICE_GPU_PHASE_STARTING;
    ReleaseSRWLockExclusive(&g_servicePublishedStateLock);
    return true;
#else
    return false;
#endif
}

static void service_publish_gpu_phase(ServiceGpuPhase phase,
    bool advanceGeneration, const char* reason) {
    AcquireSRWLockExclusive(&g_servicePublishedStateLock);
    if (advanceGeneration) InterlockedIncrement64(&g_serviceGpuGeneration);
    LONG previous = InterlockedExchange(&g_serviceGpuPhase, (LONG)phase);
    gc_u64 revision = (gc_u64)InterlockedIncrement64(
        &g_serviceStateRevision);
    gc_u64 generation = (gc_u64)InterlockedCompareExchange64(
        &g_serviceGpuGeneration, 0, 0);
    g_servicePublishedState.state.serviceInstanceId = g_serviceInstanceId;
    g_servicePublishedState.state.stateRevision = revision;
    g_servicePublishedState.state.gpuGeneration = generation;
    g_servicePublishedState.state.gpuPhase = (gc_u32)phase;
    if (phase != SERVICE_GPU_PHASE_READY) {
        g_servicePublishedState.state.validSections &=
            SERVICE_STATE_SECTION_ADAPTER_IDENTITY |
            SERVICE_STATE_SECTION_ACTIVE_INTENT;
        g_servicePublishedState.state.topologySignature = 0;
        g_servicePublishedState.snapshot.initialized = false;
        g_servicePublishedState.snapshot.loaded = false;
        g_servicePublishedState.controlState.valid = false;
    }
    ReleaseSRWLockExclusive(&g_servicePublishedStateLock);
    if (previous != (LONG)phase || advanceGeneration) {
        debug_log("service state: phase %ld -> %u instance=%llu gpuGeneration=%llu (%s)\n",
            (long)previous, (unsigned int)phase,
            (unsigned long long)g_serviceInstanceId,
            (unsigned long long)InterlockedCompareExchange64(
                &g_serviceGpuGeneration, 0, 0),
            reason && reason[0] ? reason : "state transition");
    }
}

static GpuAdapterInfo g_servicePublishedSelectedGpu = {};
static bool g_servicePublishedSelectedGpuObserved = false;
static volatile LONG g_servicePublishedSelectedGpuAuthority = 0;

static bool service_gpu_identity_equal(const GpuAdapterInfo* a,
    const GpuAdapterInfo* b) {
    if (!a || !b || !a->valid || !b->valid) return false;
    return a->deviceId == b->deviceId &&
        a->subSystemId == b->subSystemId &&
        a->pciRevisionId == b->pciRevisionId &&
        a->extDeviceId == b->extDeviceId &&
        a->pciDomain == b->pciDomain && a->pciBus == b->pciBus &&
        a->pciDevice == b->pciDevice &&
        a->pciFunction == b->pciFunction;
}

static void service_mark_selected_gpu_authority_lost(
    ServiceGpuPhase phase, const char* reason) {
    InterlockedExchange(&g_servicePublishedSelectedGpuAuthority, 0);
    service_publish_gpu_phase(phase, true, reason);
}

static void service_mark_selected_gpu_recovering(const char* reason) {
    bool authorityWasLive = InterlockedExchange(
        &g_servicePublishedSelectedGpuAuthority, 0) != 0;
    service_publish_gpu_phase(SERVICE_GPU_PHASE_RECOVERING,
        authorityWasLive, reason);
}

static ServiceGpuPhase service_effective_gpu_phase_locked() {
    ServiceGpuPhase phase = (ServiceGpuPhase)InterlockedCompareExchange(
        &g_serviceGpuPhase, 0, 0);
    if (InterlockedExchangeAdd(&g_serviceReapplyInProgress, 0) != 0)
        return SERVICE_GPU_PHASE_REAPPLYING;
    if (g_serviceControlledRecoveryValidated ||
        InterlockedExchangeAdd(&g_serviceRestartPreparing, 0) != 0 ||
        InterlockedExchangeAdd(&g_serviceRestartRequested, 0) != 0) {
        return SERVICE_GPU_PHASE_RECOVERING;
    }
    if (phase == SERVICE_GPU_PHASE_READY && (!g_app.loaded || !g_app.gpuHandle))
        return SERVICE_GPU_PHASE_DEGRADED;
    return phase;
}

// Build one complete version locally, then atomically replace the immutable
// published version. Readers copy exactly one version; no response can combine
// topology, controls, intent, or phase from different publications.
static void populate_service_state_response(ServiceResponse* response) {
    if (!response) return;
    DWORD snapshotLockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    service_auto_restore_is_locked_out(&snapshotLockoutReason);

    ImmutablePublishedServiceState candidate = {};
    EnterCriticalSection(&g_appLock);
    populate_service_snapshot_locked(&candidate.snapshot, snapshotLockoutReason);
    populate_control_state_locked(&candidate.controlState);
    if (g_serviceHasActiveDesired) candidate.desired = g_serviceActiveDesired;
    AcquireSRWLockExclusive(&g_servicePublishedStateLock);

    ServiceStateEnvelope& state = candidate.state;
    state.serviceInstanceId = g_serviceInstanceId;
    ServiceGpuPhase effectivePhase = service_effective_gpu_phase_locked();
    if (g_servicePublishedState.state.gpuPhase == SERVICE_GPU_PHASE_READY &&
        effectivePhase != SERVICE_GPU_PHASE_READY) {
        InterlockedIncrement64(&g_serviceGpuGeneration);
        InterlockedExchange(&g_serviceGpuPhase, (LONG)effectivePhase);
        debug_log("service state: READY authority lost while publishing response; gpuGeneration=%llu phase=%u\n",
            (unsigned long long)InterlockedCompareExchange64(
                &g_serviceGpuGeneration, 0, 0),
            (unsigned int)effectivePhase);
    }
    state.gpuGeneration = (gc_u64)InterlockedCompareExchange64(
        &g_serviceGpuGeneration, 0, 0);
    state.gpuPhase = (gc_u32)effectivePhase;
    state.activeDesiredValid = g_serviceHasActiveDesired;
    state.validSections = SERVICE_STATE_SECTION_ACTIVE_INTENT;

    bool adapterValid = candidate.snapshot.adapterCount > 0 &&
        candidate.snapshot.selectedAdapterIndex < candidate.snapshot.adapterCount &&
        candidate.snapshot.selectedAdapterIndex < MAX_GPU_ADAPTERS &&
        candidate.snapshot.adapters[candidate.snapshot.selectedAdapterIndex].valid;
    if (adapterValid) {
        const GpuAdapterInfo& selected = candidate.snapshot.adapters[
            candidate.snapshot.selectedAdapterIndex];
        if (g_servicePublishedSelectedGpuObserved &&
            !service_gpu_identity_equal(&g_servicePublishedSelectedGpu, &selected)) {
            InterlockedIncrement64(&g_serviceGpuGeneration);
            debug_log("service state: authoritative selected GPU identity changed; gpuGeneration=%llu\n",
                (unsigned long long)InterlockedCompareExchange64(
                    &g_serviceGpuGeneration, 0, 0));
        }
        g_servicePublishedSelectedGpu = selected;
        g_servicePublishedSelectedGpuObserved = true;
        // A cached selected identity remains useful while the device is
        // missing/recovering, but it is not live hardware authority.  Restoring
        // this flag from a non-READY response would make the later arrival look
        // like a missed removal and advance the generation a second time.
        InterlockedExchange(&g_servicePublishedSelectedGpuAuthority,
            state.gpuPhase == SERVICE_GPU_PHASE_READY ? 1 : 0);
        state.validSections |= SERVICE_STATE_SECTION_ADAPTER_IDENTITY;
    } else {
        bool hadIdentityAuthority = InterlockedExchange(
            &g_servicePublishedSelectedGpuAuthority, 0) != 0;
        if (hadIdentityAuthority ||
            state.gpuPhase == SERVICE_GPU_PHASE_READY) {
            InterlockedIncrement64(&g_serviceGpuGeneration);
            debug_log("service state: authoritative selected GPU identity became unavailable; gpuGeneration=%llu\n",
                (unsigned long long)InterlockedCompareExchange64(
                    &g_serviceGpuGeneration, 0, 0));
        }
        if (state.gpuPhase == SERVICE_GPU_PHASE_READY) {
            state.gpuPhase = SERVICE_GPU_PHASE_DEGRADED;
            InterlockedExchange(&g_serviceGpuPhase,
                SERVICE_GPU_PHASE_DEGRADED);
        }
    }
    state.gpuGeneration = (gc_u64)InterlockedCompareExchange64(
        &g_serviceGpuGeneration, 0, 0);
    if (state.gpuPhase == SERVICE_GPU_PHASE_READY && candidate.snapshot.loaded) {
        state.validSections |= SERVICE_STATE_SECTION_CURVE_TOPOLOGY |
            SERVICE_STATE_SECTION_FAN_TELEMETRY;
        if (candidate.controlState.valid)
            state.validSections |= SERVICE_STATE_SECTION_APPLIED_CONTROLS;
    }
    if ((state.validSections & SERVICE_STATE_SECTION_CURVE_TOPOLOGY) != 0)
        state.topologySignature =
            service_snapshot_topology_signature(&candidate.snapshot);
    state.stateRevision = (gc_u64)InterlockedIncrement64(
        &g_serviceStateRevision);
    g_servicePublishedState = candidate;
    response->state = g_servicePublishedState.state;
    response->snapshot = g_servicePublishedState.snapshot;
    response->desired = g_servicePublishedState.desired;
    response->controlState = g_servicePublishedState.controlState;
    ReleaseSRWLockExclusive(&g_servicePublishedStateLock);
    LeaveCriticalSection(&g_appLock);

    debug_log_on_change(
        "service envelope: instance=%llu revision=%llu gpuGeneration=%llu phase=%u valid=0x%02X topology=%llu desired=%d\n",
        (unsigned long long)response->state.serviceInstanceId,
        (unsigned long long)response->state.stateRevision,
        (unsigned long long)response->state.gpuGeneration,
        response->state.gpuPhase, response->state.validSections,
        (unsigned long long)response->state.topologySignature,
        response->state.activeDesiredValid ? 1 : 0);
}

static bool service_mutation_preconditions_match(const ServiceRequest* request,
    char* err, size_t errSize) {
    if (!request) return false;
    bool any = request->expectedServiceInstanceId ||
        request->expectedGpuGeneration || request->expectedTopologySignature;
    if (!any) return true;
    if (!request->expectedServiceInstanceId ||
        !request->expectedGpuGeneration || !request->expectedTopologySignature) {
        set_message(err, errSize,
            "Mutation has an incomplete reconnect-safety precondition");
        return false;
    }

    gc_u64 currentGeneration = (gc_u64)InterlockedCompareExchange64(
        &g_serviceGpuGeneration, 0, 0);
    if (request->expectedServiceInstanceId != g_serviceInstanceId ||
        request->expectedGpuGeneration != currentGeneration) {
        set_message(err, errSize,
            "GPU state changed while the operation was queued; refresh and retry");
        return false;
    }

    ImmutablePublishedServiceState published = {};
    AcquireSRWLockShared(&g_servicePublishedStateLock);
    published = g_servicePublishedState;
    ReleaseSRWLockShared(&g_servicePublishedStateLock);
    bool selectedIdentityMatches = false;
    if (request->targetGpu.valid && published.snapshot.adapterCount > 0 &&
        published.snapshot.selectedAdapterIndex <
            published.snapshot.adapterCount &&
        published.snapshot.selectedAdapterIndex < MAX_GPU_ADAPTERS) {
        const GpuAdapterInfo& selected =
            published.snapshot.adapters[
                published.snapshot.selectedAdapterIndex];
        selectedIdentityMatches = selected.nvapiIndex ==
            request->targetGpu.nvapiIndex &&
            service_gpu_identity_equal(&selected, &request->targetGpu);
    }
    if (published.state.serviceInstanceId !=
            request->expectedServiceInstanceId ||
        published.state.gpuGeneration != request->expectedGpuGeneration ||
        published.state.gpuPhase != SERVICE_GPU_PHASE_READY ||
        !selectedIdentityMatches ||
        published.state.topologySignature !=
            request->expectedTopologySignature) {
        set_message(err, errSize,
            "Selected GPU identity, topology, or recovery state changed while the operation was queued; refresh and retry");
        return false;
    }
    return true;
}
