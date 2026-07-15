// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static gc_u64 g_daemonServiceInstanceId = 0;
static gc_u64 g_daemonStateRevision = 0;
static gc_u64 g_daemonGpuGeneration = 1;

static gc_u64 daemon_service_instance_id() {
    if (g_daemonServiceInstanceId) return g_daemonServiceInstanceId;
    gc_u64 value = 0;
    ssize_t result;
    do {
        result = getrandom(&value, sizeof(value), 0);
    } while (result < 0 && errno == EINTR);
    if (result != (ssize_t)sizeof(value) || !value) {
        value = ((gc_u64)(unsigned int)getpid() << 32) ^
            (gc_u64)time(nullptr) ^ (gc_u64)(uintptr_t)&value;
    }
    g_daemonServiceInstanceId = value ? value : 1;
    return g_daemonServiceInstanceId;
}

static void daemon_stamp_state_envelope(ServiceResponse* response) {
    // Rebuild snapshot and controls together under g_lock.  This keeps the
    // envelope atomic: identity, topology, live controls and active intent all
    // describe the same selected-GPU lifetime.
    populate_snapshot(&response->snapshot, &response->controlState);
    if (g_hasActiveDesired) response->desired = g_activeDesired;
    response->state.serviceInstanceId = daemon_service_instance_id();
    response->state.stateRevision = ++g_daemonStateRevision;
    response->state.gpuGeneration = g_daemonGpuGeneration;
    response->state.gpuPhase = SERVICE_GPU_PHASE_DEVICE_MISSING;
    response->state.validSections = SERVICE_STATE_SECTION_ACTIVE_INTENT;
    response->state.activeDesiredValid = g_hasActiveDesired;
    // A multi-GPU backend may use adapter 0 for read-only telemetry before an
    // exact write target is chosen. Do not publish that fallback as selected.
    if (g_gpu.writeIdentityResolved &&
        response->snapshot.adapterCount > 0 &&
        response->snapshot.selectedAdapterIndex <
            response->snapshot.adapterCount &&
        response->snapshot.adapters[
            response->snapshot.selectedAdapterIndex].valid) {
        response->state.validSections |= SERVICE_STATE_SECTION_ADAPTER_IDENTITY;
    }
    if (response->snapshot.loaded && response->snapshot.numPopulated > 0) {
        response->state.validSections |= SERVICE_STATE_SECTION_CURVE_TOPOLOGY;
        response->state.topologySignature =
            service_snapshot_topology_signature(&response->snapshot);
    }
    if (response->controlState.valid)
        response->state.validSections |= SERVICE_STATE_SECTION_APPLIED_CONTROLS;
    if (response->snapshot.gpuTemperatureValid ||
        response->snapshot.fanCount > 0)
        response->state.validSections |= SERVICE_STATE_SECTION_FAN_TELEMETRY;
    const gc_u32 ready = SERVICE_STATE_SECTION_READY_REQUIRED;
    if (g_gpuReady && !g_stateUncertain &&
        (response->state.validSections & ready) == ready) {
        response->state.gpuPhase = SERVICE_GPU_PHASE_READY;
    } else if (g_gpuReady) {
        response->state.gpuPhase = SERVICE_GPU_PHASE_DEGRADED;
    }
}
