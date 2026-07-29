// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Pure per-domain Linux mutation attachment checks.

#ifndef GREEN_CURVE_LINUX_MUTATION_AUTHORITY_H
#define GREEN_CURVE_LINUX_MUTATION_AUTHORITY_H

#include "linux_gpu_selection.h"

static inline bool linux_mutation_authority_matches(
    const ServiceRequest* request,
    gc_u64 currentServiceInstanceId,
    gc_u64 currentGpuGeneration,
    gc_u64 currentTopologySignature,
    const GpuAdapterInfo* currentGpu,
    gc_u32 requestedDomains) {
    if (!request || !currentGpu || request->expectedServiceInstanceId == 0 ||
        request->expectedGpuGeneration == 0 ||
        request->expectedServiceInstanceId != currentServiceInstanceId ||
        request->expectedGpuGeneration != currentGpuGeneration ||
        !linux_gpu_identity_matches(&request->targetGpu, currentGpu))
        return false;
    bool topologyRequired = request->command == SERVICE_CMD_RESET ||
        service_mutation_domains_require_vf(requestedDomains);
    return !topologyRequired ||
        (request->expectedTopologySignature != 0 &&
         request->expectedTopologySignature == currentTopologySignature);
}

#endif // GREEN_CURVE_LINUX_MUTATION_AUTHORITY_H
