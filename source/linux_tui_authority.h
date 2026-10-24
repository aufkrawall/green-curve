// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Pure Linux TUI draft-authority policy.

#ifndef GREEN_CURVE_LINUX_TUI_AUTHORITY_H
#define GREEN_CURVE_LINUX_TUI_AUTHORITY_H

#include "linux_gpu_selection.h"

struct LinuxTuiDraftBinding {
    gc_u64 serviceInstanceId;
    gc_u64 gpuGeneration;
    gc_u64 topologySignature;
    gc_u32 mutationDomains;
    GpuAdapterInfo gpu;
    bool valid;
};

static inline const GpuAdapterInfo* linux_tui_authoritative_gpu(
    const ServiceResponse* response) {
    if (!response ||
        (response->state.validSections &
            SERVICE_STATE_SECTION_ADAPTER_IDENTITY) == 0 ||
        response->snapshot.selectedAdapterIndex >=
            response->snapshot.adapterCount ||
        response->snapshot.selectedAdapterIndex >= MAX_GPU_ADAPTERS)
        return nullptr;
    const GpuAdapterInfo* gpu = &response->snapshot.adapters[
        response->snapshot.selectedAdapterIndex];
    return gpu->valid &&
        (linux_gpu_bdf_valid(gpu) || gpu->pciInfoValid) ? gpu : nullptr;
}

// Whether the TUI may write while it shows `shown` (the GPU its live state,
// draft and mutation authority belong to, and the target every write sends).
// A selection that names a GPU (`--gpu`, a resolved config.ini record, the
// selector) allows a write only while exactly that GPU is shown.  Until the
// switch to it has been answered -- an explicit `--gpu` at start, a switch the
// daemon refuses because the shown GPU holds active settings, or one that
// failed as busy -- the shown GPU is another one, and a write would reach it.
enum LinuxTuiShownGpuWrite {
    LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED = 0,
    LINUX_TUI_SHOWN_GPU_WRITE_UNRESOLVED,  // config.ini's GPU did not resolve
    LINUX_TUI_SHOWN_GPU_WRITE_NOT_SHOWN,   // the selected GPU is not shown
};

static inline LinuxTuiShownGpuWrite linux_tui_shown_gpu_write(
    const LinuxConfiguredGpu* gpu, const GpuAdapterInfo* shown) {
    const GpuAdapterInfo* selected = nullptr;
    if (!linux_configured_gpu_request_target(gpu, true, &selected))
        return LINUX_TUI_SHOWN_GPU_WRITE_UNRESOLVED;
    if (!selected) return LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED;
    return shown && linux_gpu_identity_matches(selected, shown)
        ? LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED
        : LINUX_TUI_SHOWN_GPU_WRITE_NOT_SHOWN;
}

// What the TUI does about its selection after an answered refresh.  Every
// step but NONE and SWITCH leaves writes blocked and says why once, on the
// transition into it:
//   NONE      no selection, or the selected GPU is shown;
//   SWITCH    switch to it with one targeted refresh;
//   HELD      the daemon keeps the shown GPU while it holds active settings
//             and refuses to select another
//             (linux_gpu_switch_preserves_active_intent); the way out is to
//             select the shown GPU and Reset it (linux_tui_selector_origin);
//   DEFERRED  a staged draft belongs to the shown GPU, and switching would
//             detach it -- the selector refuses the same switch;
//   REFUSED   the daemon answered the last switch with a refusal, and
//             nothing it reports has changed since
//             (linux_tui_switch_refusal_matches), so asking again every
//             telemetry tick would only repeat the refusal;
//   BLOCKED   the selection did not resolve (config.ini's GPU is missing,
//             ambiguous, malformed or unchecked).
// Each step is re-evaluated after every answered refresh: a Reset elsewhere,
// a cleared draft or a change in the daemon's state lets the switch go ahead.
enum LinuxTuiGpuAdoption {
    LINUX_TUI_GPU_ADOPTION_NONE = 0,
    LINUX_TUI_GPU_ADOPTION_SWITCH,
    LINUX_TUI_GPU_ADOPTION_HELD,
    LINUX_TUI_GPU_ADOPTION_DEFERRED,
    LINUX_TUI_GPU_ADOPTION_REFUSED,
    LINUX_TUI_GPU_ADOPTION_BLOCKED,
};

static inline LinuxTuiGpuAdoption linux_tui_gpu_adoption(
    const LinuxConfiguredGpu* gpu, const GpuAdapterInfo* shown,
    bool daemonHoldsActiveSettings, bool draftDirty, bool switchRefused) {
    switch (linux_tui_shown_gpu_write(gpu, shown)) {
    case LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED: return LINUX_TUI_GPU_ADOPTION_NONE;
    case LINUX_TUI_SHOWN_GPU_WRITE_UNRESOLVED:
        return LINUX_TUI_GPU_ADOPTION_BLOCKED;
    case LINUX_TUI_SHOWN_GPU_WRITE_NOT_SHOWN: break;
    }
    if (daemonHoldsActiveSettings) return LINUX_TUI_GPU_ADOPTION_HELD;
    if (draftDirty) return LINUX_TUI_GPU_ADOPTION_DEFERRED;
    if (switchRefused) return LINUX_TUI_GPU_ADOPTION_REFUSED;
    return LINUX_TUI_GPU_ADOPTION_SWITCH;
}

static inline const char* linux_tui_gpu_adoption_name(
    LinuxTuiGpuAdoption adoption) {
    switch (adoption) {
    case LINUX_TUI_GPU_ADOPTION_NONE: return "none";
    case LINUX_TUI_GPU_ADOPTION_SWITCH: return "switch";
    case LINUX_TUI_GPU_ADOPTION_HELD: return "held";
    case LINUX_TUI_GPU_ADOPTION_DEFERRED: return "deferred";
    case LINUX_TUI_GPU_ADOPTION_REFUSED: return "refused";
    case LINUX_TUI_GPU_ADOPTION_BLOCKED: return "blocked";
    }
    return "invalid";
}

// A switch the daemon ANSWERED with a refusal, and the daemon state it was
// refused in.  The switch is asked again only once one of these changes (a
// restart, another GPU generation or topology, active settings set or
// cleared), the selection changes, or the user refreshes; a refusal repeated
// once per telemetry tick costs a request each time and buries the reason
// under a status line that changes every second.  A switch that was not
// answered (busy, timed out) is not recorded and is retried on the next tick.
struct LinuxTuiSwitchRefusal {
    bool valid;
    GpuAdapterInfo target;
    gc_u64 serviceInstanceId;
    gc_u64 gpuGeneration;
    gc_u64 topologySignature;
    unsigned int adapterCount;
    bool activeSettings;
};

static inline LinuxTuiSwitchRefusal linux_tui_switch_refusal(
    const GpuAdapterInfo* target, const ServiceResponse* refusedIn) {
    LinuxTuiSwitchRefusal refusal = {};
    if (!target || !refusedIn) return refusal;
    refusal.valid = true;
    refusal.target = *target;
    refusal.serviceInstanceId = refusedIn->state.serviceInstanceId;
    refusal.gpuGeneration = refusedIn->state.gpuGeneration;
    refusal.topologySignature = refusedIn->state.topologySignature;
    refusal.adapterCount = refusedIn->snapshot.adapterCount;
    refusal.activeSettings = refusedIn->state.activeDesiredValid != 0;
    return refusal;
}

static inline bool linux_tui_switch_refusal_matches(
    const LinuxTuiSwitchRefusal* refusal, const GpuAdapterInfo* target,
    const ServiceResponse* current) {
    return refusal && refusal->valid && target && current &&
        linux_gpu_identity_matches(&refusal->target, target) &&
        refusal->serviceInstanceId == current->state.serviceInstanceId &&
        refusal->gpuGeneration == current->state.gpuGeneration &&
        refusal->topologySignature == current->state.topologySignature &&
        refusal->adapterCount == current->snapshot.adapterCount &&
        refusal->activeSettings == (current->state.activeDesiredValid != 0);
}

// The adapter the GPU selector steps from: the selected GPU when it names a
// listed adapter, otherwise the daemon's own selection (left in `*index`).
// Stepping from the daemon's selection made the shown GPU unreachable
// whenever another GPU was selected: while the daemon holds the shown GPU for
// its active settings it refuses every other choice, so the user could
// neither write to it (not selected) nor select it (one step from it is
// always another GPU), and its Reset -- the daemon's own condition for the
// switch -- could not be sent from the TUI at all.
static inline bool linux_tui_selector_origin(
    const LinuxConfiguredGpu* gpu, const GpuAdapterInfo* adapters,
    unsigned int count, unsigned int* index) {
    const GpuAdapterInfo* selected = nullptr;
    if (!index || !adapters ||
        !linux_configured_gpu_request_target(gpu, true, &selected) ||
        !selected) return false;
    for (unsigned int i = 0; i < count && i < MAX_GPU_ADAPTERS; ++i) {
        if (!linux_gpu_identity_matches(selected, &adapters[i])) continue;
        *index = i;
        return true;
    }
    return false;
}

static inline const char* linux_tui_shown_gpu_write_name(
    LinuxTuiShownGpuWrite verdict) {
    switch (verdict) {
    case LINUX_TUI_SHOWN_GPU_WRITE_ALLOWED: return "allowed";
    case LINUX_TUI_SHOWN_GPU_WRITE_UNRESOLVED: return "unresolved";
    case LINUX_TUI_SHOWN_GPU_WRITE_NOT_SHOWN: return "not-shown";
    }
    return "invalid";
}

static inline bool linux_tui_domains_available(
    const ServiceResponse* response, gc_u32 requestedDomains) {
    return response &&
        (requestedDomains &
         ~response->snapshot.health.availableMutationDomains) == 0;
}

static inline bool linux_tui_bind_draft(
    LinuxTuiDraftBinding* binding, const ServiceResponse* response,
    const DesiredSettings* desired) {
    if (!binding) return false;
    *binding = LinuxTuiDraftBinding{};
    const GpuAdapterInfo* gpu = linux_tui_authoritative_gpu(response);
    if (!response || !desired || !gpu ||
        response->state.serviceInstanceId == 0 ||
        response->state.gpuGeneration == 0) return false;
    gc_u32 domains = service_desired_mutation_domains(desired);
    if (!linux_tui_domains_available(response, domains)) return false;
    bool needsVf = service_mutation_domains_require_vf(domains);
    if (needsVf &&
        ((response->state.validSections &
            SERVICE_STATE_SECTION_CURVE_TOPOLOGY) == 0 ||
         response->state.topologySignature == 0)) return false;
    binding->serviceInstanceId = response->state.serviceInstanceId;
    binding->gpuGeneration = response->state.gpuGeneration;
    binding->topologySignature = needsVf
        ? response->state.topologySignature : 0;
    binding->mutationDomains = domains;
    binding->gpu = *gpu;
    binding->valid = true;
    return true;
}

static inline bool linux_tui_draft_binding_matches(
    const LinuxTuiDraftBinding* binding,
    const ServiceResponse* response,
    const DesiredSettings* desired) {
    if (!binding || !binding->valid || !response || !desired) return false;
    const GpuAdapterInfo* gpu = linux_tui_authoritative_gpu(response);
    gc_u32 currentDomains = service_desired_mutation_domains(desired);
    if (!gpu || binding->serviceInstanceId !=
                    response->state.serviceInstanceId ||
        binding->gpuGeneration != response->state.gpuGeneration ||
        !linux_gpu_identity_matches(&binding->gpu, gpu) ||
        !linux_tui_domains_available(response, currentDomains)) return false;
    if (service_mutation_domains_require_vf(currentDomains)) {
        return binding->topologySignature != 0 &&
            binding->topologySignature == response->state.topologySignature &&
            (response->state.validSections &
                SERVICE_STATE_SECTION_CURVE_TOPOLOGY) != 0;
    }
    return true;
}

static inline const char* linux_tui_gpu_phase_name(gc_u32 phase) {
    switch (phase) {
        case SERVICE_GPU_PHASE_STARTING: return "starting";
        case SERVICE_GPU_PHASE_DEVICE_MISSING: return "GPU missing";
        case SERVICE_GPU_PHASE_RECOVERING: return "recovering";
        case SERVICE_GPU_PHASE_REAPPLYING: return "reapplying";
        case SERVICE_GPU_PHASE_READY: return "ready";
        case SERVICE_GPU_PHASE_DEGRADED: return "degraded";
        default: return "invalid";
    }
}

static inline const char* linux_tui_health_remediation(gc_u32 reason) {
    switch (reason) {
        case SERVICE_GPU_HEALTH_NONE:
            return "Refresh to retry VF discovery";
        case SERVICE_GPU_HEALTH_NVML_UNAVAILABLE:
            return "Check the NVIDIA driver and journalctl -u greencurve";
        case SERVICE_GPU_HEALTH_NVAPI_LIBRARY_UNAVAILABLE:
            return "Install the matching NVIDIA driver library libnvidia-api.so.1, then restart the service";
        case SERVICE_GPU_HEALTH_NVAPI_INIT_FAILED:
        case SERVICE_GPU_HEALTH_NVAPI_ENUM_FAILED:
        case SERVICE_GPU_HEALTH_NVAPI_HANDLE_UNRESOLVED:
        case SERVICE_GPU_HEALTH_ARCHITECTURE_UNAVAILABLE:
            return "Refresh once; if it persists, restart the NVIDIA driver/service and run --self-test";
        case SERVICE_GPU_HEALTH_VF_INFO_FAILED:
        case SERVICE_GPU_HEALTH_VF_STATUS_FAILED:
        case SERVICE_GPU_HEALTH_VF_CONTROL_FAILED:
            return "Refresh to rebind; if it persists, run --self-test and inspect the service journal";
        case SERVICE_GPU_HEALTH_VF_STRUCTURE_INVALID:
            return "VF writes are blocked; run --self-test/--probe and report the exact driver statuses";
        case SERVICE_GPU_HEALTH_STATE_UNCERTAIN:
            return "Review live state, then use an explicit supported Apply or full Reset";
        default:
            return "Refresh and inspect journalctl -u greencurve";
    }
}

#endif // GREEN_CURVE_LINUX_TUI_AUTHORITY_H
