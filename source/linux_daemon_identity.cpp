// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static gc_u64 g_daemonServiceInstanceId = 0;
static gc_u64 g_daemonStateRevision = 0;

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

static bool daemon_selected_startup_topology(LinuxGpuController& ctl) {
    if (!ctl.gpuReady || !ctl.gpu.writeIdentityResolved || !ctl.gpu.vfSnapshotFresh || !ctl.gpu.numPopulated ||
        ctl.startupPolicy.mode != SERVICE_STARTUP_POLICY_PROFILE) return false;
    return daemon_stored_gpu_index(ctl, ctl.startupPolicy.targetGpu,
        (GpuLocationRole)ctl.startupPolicy.targetLocationRole, ctl.startupPolicy.targetUuid) ==
        (int)ctl.gpu.selectedAdapterIndex;
}

// Protocol v40 adapter summaries (service_adapter_summary.h): every adapter's
// physical UUID NVML reported, the card holding the active intent, and the
// runtime summary of the one GPU this daemon serves. The telemetry fallback
// of a multi-GPU host is not served.
static void daemon_publish_adapter_summaries(const LinuxGpuController& ctl, ServiceResponse* response) {
    ServiceSnapshot& s = response->snapshot;
    const unsigned int count = s.adapterCount < MAX_GPU_ADAPTERS ? s.adapterCount : MAX_GPU_ADAPTERS;
    ServiceGpuUuid uuids[MAX_GPU_ADAPTERS] = {};
    for (unsigned int i = 0; i < count && i < ctl.gpu.adapterCount; ++i)
        service_gpu_uuid_parse(ctl.gpu.adapterUuids[i].text, &uuids[i]);
    int owner = -1;
    for (unsigned int i = 0; ctl.hasActiveDesired && i < count; ++i)
        if (linux_gpu_identity_matches(&ctl.activeTarget, &s.adapters[i]) && daemon_intent_uuid_names(ctl, i))
            owner = owner == -1 ? (int)i : -2;
    // A pending fan handback belongs to the card its ownership marker names.
    int fanHandback = -1;
    if (ctl.fanOwnershipHandbackPending || ctl.fanOwnershipHandbackDeferred) {
        const auto marker = linux_gpu_restore_selection(&ctl.fanOwnershipRecord.targetGpu,
            (GpuLocationRole)ctl.fanOwnershipRecord.targetLocationRole, s.adapters, count,
            &ctl.fanOwnershipRecord.targetUuid, ctl.gpu.adapterUuids, nullptr);
        fanHandback = marker.writeResolved ? marker.index : -1;
    }
    ServiceAdapterSummaryInputs in = {};
    in.uuids = uuids;
    in.count = count;
    in.ownerIndex = owner;
    in.fanHandbackIndex = fanHandback;
    in.servedIndex = ctl.gpu.writeIdentityResolved && s.selectedAdapterIndex < count
        ? (int)s.selectedAdapterIndex : -1;
    in.uncertain = ctl.stateUncertain;
    in.fanOwned = s.fanCurveRuntimeActive || s.fanFixedRuntimeActive;
    in.fanFault = s.fanFault.kind != SERVICE_FAN_FAULT_NONE;
    in.gpuPhase = response->state.gpuPhase;
    in.healthReason = s.health.reason;
    in.autoRestoreLockoutReason = s.autoRestoreLockoutReason;
    const unsigned int withheld = service_adapter_summaries_build(in, s.adapterSummaries);
    static unsigned int lastWithheld = 0;
    if (withheld != lastWithheld) {
        dlog("daemon adapters: %u shared UUID(s) withheld; owner=%d served=%d fanHandback=%d\n",
             withheld, owner, in.servedIndex, fanHandback);
        lastWithheld = withheld;
    }
}

static void daemon_stamp_state_envelope(LinuxGpuController& ctl, ServiceResponse* response, bool passive = false) {
    // Rebuild snapshot and controls together under g_lock.  This keeps the
    // envelope atomic: identity, topology, live controls and active intent all
    // describe the same selected-GPU lifetime.
    //
    // This runs at the end of EVERY request, so snapshot, controlState and
    // desired belong to it alone -- whatever a handler wrote into them is
    // overwritten here.  A handler with something else to publish needs its own
    // response member; resp->startupProfile exists because the boot-apply
    // snapshot was borrowing resp->desired and losing that race silently.
    // Passive capture publishes its dedicated result, with no extra getters,
    // cache publication or ownership handback through normal snapshot assembly.
    if (!passive) populate_snapshot(ctl, &response->snapshot, &response->controlState);
    if (ctl.hasActiveDesired) response->desired = ctl.activeDesired;
    response->state.serviceInstanceId = daemon_service_instance_id();
    response->state.stateRevision = ++g_daemonStateRevision;
    response->state.gpuGeneration = ctl.gpuGeneration;
    response->state.gpuPhase = SERVICE_GPU_PHASE_DEVICE_MISSING;
    response->state.validSections = SERVICE_STATE_SECTION_ACTIVE_INTENT;
    response->state.activeDesiredValid = ctl.hasActiveDesired;
    // Published on every envelope so a client renders the boot-apply policy
    // without a second round trip.  The in-memory copy is the record the daemon
    // committed, so a failed store never advertises a policy it will not honor.
    response->state.startupPolicyMode = ctl.startupPolicy.mode;
    response->state.startupPolicySlot = ctl.startupPolicy.profileSlot;
    // A multi-GPU backend may use adapter 0 for read-only telemetry before an
    // exact write target is chosen. Do not publish that fallback as selected.
    if (ctl.gpu.writeIdentityResolved &&
        response->snapshot.adapterCount > 0 &&
        response->snapshot.selectedAdapterIndex <
            response->snapshot.adapterCount &&
        response->snapshot.adapters[
            response->snapshot.selectedAdapterIndex].valid &&
        (linux_gpu_bdf_valid(&response->snapshot.adapters[
             response->snapshot.selectedAdapterIndex]) ||
         response->snapshot.adapters[
             response->snapshot.selectedAdapterIndex].pciInfoValid)) {
        response->state.validSections |= SERVICE_STATE_SECTION_ADAPTER_IDENTITY;
    }
    if (response->snapshot.loaded && response->snapshot.numPopulated > 0) {
        response->state.validSections |= SERVICE_STATE_SECTION_CURVE_TOPOLOGY;
        response->state.topologySignature =
            service_snapshot_topology_signature(&response->snapshot);
    }
    if (response->startupProfileValid && (response->state.validSections & SERVICE_STATE_SECTION_ADAPTER_IDENTITY) &&
        (response->state.validSections & SERVICE_STATE_SECTION_CURVE_TOPOLOGY))
        response->startupTopologyBound = gc_bool8_from_bool(daemon_selected_startup_topology(ctl));
    if (response->controlState.valid)
        response->state.validSections |= SERVICE_STATE_SECTION_APPLIED_CONTROLS;
    if (response->snapshot.gpuTemperatureValid ||
        response->snapshot.fanCount > 0)
        response->state.validSections |= SERVICE_STATE_SECTION_FAN_TELEMETRY;
    const gc_u32 ready = SERVICE_STATE_SECTION_READY_REQUIRED;
    if (ctl.gpuReady && !ctl.stateUncertain &&
        (response->state.validSections & ready) == ready) {
        response->state.gpuPhase = SERVICE_GPU_PHASE_READY;
    } else if (ctl.gpuReady) {
        response->state.gpuPhase = SERVICE_GPU_PHASE_DEGRADED;
    }
    if (!passive) daemon_publish_adapter_summaries(ctl, response);
}
