// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "gpu_core.h"
#include "service_protocol.h"

namespace {

ServiceResponse family_response() {
    ServiceResponse response = {};
    response.magic = SERVICE_PROTOCOL_MAGIC;
    response.version = SERVICE_PROTOCOL_VERSION;
    response.state.serviceInstanceId = 1;
    response.state.stateRevision = 1;
    response.state.gpuGeneration = 1;
    return response;
}

void write_family_bytes(GpuFamily* field, int value) {
    static_assert(sizeof(GpuFamily) == sizeof(int) && sizeof(int) == 4,
                  "GPU family keeps its 32-bit wire layout");
    // Model a transport read without first converting untrusted bytes to an
    // enum value. The production validator must be the first reader.
    memcpy(field, &value, sizeof(value));
}

}  // namespace

int run_gpu_family_wire_tests() {
    const int invalid[] = {-1, GPU_FAMILY_BLACKWELL + 1, 7, 8, 57825,
                           INT_MIN, INT_MAX};
    for (int value : invalid) {
        ServiceResponse response = family_response();
        write_family_bytes(&response.snapshot.gpuFamily, value);
        if (validate_service_response_for_ipc(&response)) return 7280;

        response = family_response();
        response.snapshot.adapterCount = 1;
        response.snapshot.adapters[0].valid = 1;
        write_family_bytes(&response.snapshot.adapters[0].family, value);
        if (validate_service_response_for_ipc(&response)) return 7281;

        // Refusals inspect the empty-payload shape before the envelope. Both
        // family fields must also be safe to inspect on that path.
        response = {};
        response.magic = SERVICE_PROTOCOL_MAGIC;
        response.version = SERVICE_PROTOCOL_VERSION;
        response.status = SERVICE_STATUS_ERROR;
        response.outcomeSeverity = SERVICE_OUTCOME_SEVERITY_ERROR;
        write_family_bytes(&response.snapshot.gpuFamily, value);
        if (validate_service_response_for_ipc(&response)) return 7282;

        response.snapshot.gpuFamily = GPU_FAMILY_UNKNOWN;
        write_family_bytes(&response.snapshot.adapters[MAX_GPU_ADAPTERS - 1].family,
                           value);
        if (validate_service_response_for_ipc(&response)) return 7283;
    }

    // Unknown and every supported family keep their existing acceptance, on
    // both snapshot and adapter paths, including validation's second pass.
    for (int value = GPU_FAMILY_UNKNOWN; value <= GPU_FAMILY_BLACKWELL; ++value) {
        ServiceResponse response = family_response();
        response.snapshot.adapterCount = 1;
        response.snapshot.adapters[0].valid = 1;
        write_family_bytes(&response.snapshot.gpuFamily, value);
        write_family_bytes(&response.snapshot.adapters[0].family, value);
        if (!validate_service_response_for_ipc(&response) ||
            !validate_service_response_for_ipc(&response)) return 7284;
    }
    return 0;
}

#if defined(GC_GPU_FAMILY_WIRE_FIXTURE_MAIN)
int main() {
    const int result = run_gpu_family_wire_tests();
    printf("GPU family wire tests: %s (assertion=%d)\n",
           result ? "FAILED" : "passed", result);
    return result ? 1 : 0;
}
#endif
