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

// What the TUI does about its selection after an answered refresh: nothing
// (no selection, unresolved, or already shown), switch to it with one
// targeted refresh, or hold, because the daemon keeps the shown GPU while it
// holds active settings and refuses to select another
// (linux_gpu_switch_preserves_active_intent).  A hold is re-evaluated after
// every answered refresh, so a Reset elsewhere lets the switch go ahead.
enum LinuxTuiGpuAdoption {
    LINUX_TUI_GPU_ADOPTION_NONE = 0,
    LINUX_TUI_GPU_ADOPTION_SWITCH,
    LINUX_TUI_GPU_ADOPTION_HELD,
};

static inline LinuxTuiGpuAdoption linux_tui_gpu_adoption(
    const LinuxConfiguredGpu* gpu, const GpuAdapterInfo* shown,
    bool daemonHoldsActiveSettings) {
    if (linux_tui_shown_gpu_write(gpu, shown) !=
        LINUX_TUI_SHOWN_GPU_WRITE_NOT_SHOWN) return LINUX_TUI_GPU_ADOPTION_NONE;
    return daemonHoldsActiveSettings ? LINUX_TUI_GPU_ADOPTION_HELD
                                     : LINUX_TUI_GPU_ADOPTION_SWITCH;
}

static inline const char* linux_tui_gpu_adoption_name(
    LinuxTuiGpuAdoption adoption) {
    switch (adoption) {
    case LINUX_TUI_GPU_ADOPTION_NONE: return "none";
    case LINUX_TUI_GPU_ADOPTION_SWITCH: return "switch";
    case LINUX_TUI_GPU_ADOPTION_HELD: return "held";
    }
    return "invalid";
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
