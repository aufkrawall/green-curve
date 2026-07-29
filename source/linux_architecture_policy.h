// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Pure architecture-source selection shared by Linux discovery and tests.

#ifndef GREEN_CURVE_LINUX_ARCHITECTURE_POLICY_H
#define GREEN_CURVE_LINUX_ARCHITECTURE_POLICY_H

#include "gpu_core.h"

struct LinuxArchitectureDecision {
    unsigned int architecture;
    GpuFamily family;
    unsigned int source;
};

static inline const char* linux_gpu_architecture_source_name(
    unsigned int source) {
    switch (source) {
        case SERVICE_GPU_ARCH_SOURCE_NVAPI: return "NvAPI";
        case SERVICE_GPU_ARCH_SOURCE_NVML: return "NVML";
        case SERVICE_GPU_ARCH_SOURCE_CACHED: return "cached";
        case SERVICE_GPU_ARCH_SOURCE_FUTURE_GUESS: return "future-guess";
        default: return "none";
    }
}

static inline GpuFamily linux_family_from_nvml_architecture(
    unsigned int architecture) {
    switch (architecture) {
        case NVML_DEVICE_ARCH_PASCAL: return GPU_FAMILY_PASCAL;
        case NVML_DEVICE_ARCH_TURING: return GPU_FAMILY_TURING;
        case NVML_DEVICE_ARCH_AMPERE: return GPU_FAMILY_AMPERE;
        case NVML_DEVICE_ARCH_ADA: return GPU_FAMILY_LOVELACE;
        case NVML_DEVICE_ARCH_BLACKWELL: return GPU_FAMILY_BLACKWELL;
        default: return GPU_FAMILY_UNKNOWN;
    }
}

static inline GpuFamily linux_family_from_nvapi_architecture(
    unsigned int architecture) {
    switch (architecture) {
        case NV_GPU_ARCHITECTURE_GP100: return GPU_FAMILY_PASCAL;
        case NV_GPU_ARCHITECTURE_TU100: return GPU_FAMILY_TURING;
        case NV_GPU_ARCHITECTURE_GA100: return GPU_FAMILY_AMPERE;
        case NV_GPU_ARCHITECTURE_AD100: return GPU_FAMILY_LOVELACE;
        case NV_GPU_ARCHITECTURE_GB200: return GPU_FAMILY_BLACKWELL;
        default: return GPU_FAMILY_UNKNOWN;
    }
}

static inline unsigned int linux_nvapi_architecture_for_family(
    GpuFamily family) {
    switch (family) {
        case GPU_FAMILY_PASCAL: return NV_GPU_ARCHITECTURE_GP100;
        case GPU_FAMILY_TURING: return NV_GPU_ARCHITECTURE_TU100;
        case GPU_FAMILY_AMPERE: return NV_GPU_ARCHITECTURE_GA100;
        case GPU_FAMILY_LOVELACE: return NV_GPU_ARCHITECTURE_AD100;
        case GPU_FAMILY_BLACKWELL: return NV_GPU_ARCHITECTURE_GB200;
        default: return 0;
    }
}

// A successful NvAPI result is authoritative and retained; NVML and cache are
// consulted only when that one query failed. Unknown NvAPI values deliberately
// select the future-guess path so fresh ABI reads, rather than a family label,
// remain the write gate.
static inline LinuxArchitectureDecision linux_choose_architecture(
    int nvapiStatus, unsigned int nvapiArchitecture,
    int nvmlStatus, unsigned int nvmlArchitecture,
    bool cachedValid, unsigned int cachedArchitecture,
    GpuFamily cachedFamily) {
    LinuxArchitectureDecision decision = {};
    decision.family = GPU_FAMILY_UNKNOWN;
    decision.source = SERVICE_GPU_ARCH_SOURCE_FUTURE_GUESS;
    if (nvapiStatus == 0 && nvapiArchitecture != 0) {
        decision.architecture = nvapiArchitecture;
        decision.family = linux_family_from_nvapi_architecture(
            nvapiArchitecture);
        decision.source = decision.family == GPU_FAMILY_UNKNOWN
            ? SERVICE_GPU_ARCH_SOURCE_FUTURE_GUESS
            : SERVICE_GPU_ARCH_SOURCE_NVAPI;
        return decision;
    }
    if (nvmlStatus == NVML_SUCCESS) {
        decision.family = linux_family_from_nvml_architecture(
            nvmlArchitecture);
        if (decision.family != GPU_FAMILY_UNKNOWN) {
            decision.architecture = linux_nvapi_architecture_for_family(
                decision.family);
            decision.source = SERVICE_GPU_ARCH_SOURCE_NVML;
            return decision;
        }
    }
    if (cachedValid) {
        decision.architecture = cachedArchitecture;
        decision.family = cachedFamily;
        decision.source = SERVICE_GPU_ARCH_SOURCE_CACHED;
    }
    return decision;
}

#endif // GREEN_CURVE_LINUX_ARCHITECTURE_POLICY_H
