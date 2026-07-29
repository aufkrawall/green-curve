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
