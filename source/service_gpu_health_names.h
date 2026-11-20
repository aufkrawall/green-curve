// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Display names for ServiceGpuHealth reasons, split out of service_protocol.h
// (size ratchet); included by it right after the health record.
#ifndef GREEN_CURVE_SERVICE_GPU_HEALTH_NAMES_H
#define GREEN_CURVE_SERVICE_GPU_HEALTH_NAMES_H

static inline const char* service_gpu_health_reason_name(gc_u32 reason) {
    switch (reason) {
        case SERVICE_GPU_HEALTH_NONE: return "healthy";
        case SERVICE_GPU_HEALTH_NVML_UNAVAILABLE: return "NVML unavailable";
        case SERVICE_GPU_HEALTH_NVAPI_LIBRARY_UNAVAILABLE: return "NvAPI library unavailable";
        case SERVICE_GPU_HEALTH_NVAPI_INIT_FAILED: return "NvAPI initialization failed";
        case SERVICE_GPU_HEALTH_NVAPI_ENUM_FAILED: return "NvAPI enumeration failed";
        case SERVICE_GPU_HEALTH_NVAPI_HANDLE_UNRESOLVED: return "NvAPI GPU handle unresolved";
        case SERVICE_GPU_HEALTH_ARCHITECTURE_UNAVAILABLE: return "GPU architecture unavailable";
        case SERVICE_GPU_HEALTH_VF_INFO_FAILED: return "VF info read failed";
        case SERVICE_GPU_HEALTH_VF_STATUS_FAILED: return "VF status read failed";
        case SERVICE_GPU_HEALTH_VF_CONTROL_FAILED: return "VF control read failed";
        case SERVICE_GPU_HEALTH_VF_STRUCTURE_INVALID: return "VF data failed structural validation";
        case SERVICE_GPU_HEALTH_STATE_UNCERTAIN: return "daemon state is uncertain";
        default: return "unknown GPU health failure";
    }

#endif // GREEN_CURVE_SERVICE_GPU_HEALTH_NAMES_H
