// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// GUI startup warning tiers judged from the service snapshot's selected adapter.

#include <stdio.h>

#include "gpu_support_warning_policy.h"

namespace {

GpuAdapterInfo adapter(GpuFamily family, bool bestGuess) {
    GpuAdapterInfo info = {};
    info.valid = 1;
    info.pciInfoValid = 1;
    info.family = family;
    info.vfBestGuess = bestGuess ? 1 : 0;
    return info;
}

GpuCapabilityProbe from_snapshot(gc_u32 packed, gc_u32 topology) {
    GpuCapabilityProbe probe = {};
    gpu_capability_unpack_domains(&probe, packed, topology);
    return probe;
}

}  // namespace

int run_gpu_support_warning_tests() {
    // Packed capability words the GUI decoded from real service snapshots:
    // a CMP 40HX (Turing; XBAR, SYS and VIDEO refused) and an RTX 5070.
    const GpuCapabilityProbe turing =
        from_snapshot(0xA9554u, GPU_MEMORY_TOPOLOGY_DEDICATED);
    const GpuCapabilityProbe blackwell =
        from_snapshot(0x55554u, GPU_MEMORY_TOPOLOGY_DEDICATED);
    if (gpu_capability_missing_domains(&turing) != 0x380u ||
        gpu_capability_missing_board_domains(&turing) != 0) return 12230;
    // Refusing only the schema-gated clock domains is not a reduced surface.
    const GpuAdapterInfo turingCard = adapter(GPU_FAMILY_TURING, false);
    if (gpu_requires_limited_control_warning(GPU_FAMILY_TURING, &turing) ||
        gpu_warning_limited_control_applies(&turingCard, &turing)) return 12231;
    const GpuAdapterInfo blackwellCard = adapter(GPU_FAMILY_BLACKWELL, false);
    if (gpu_warning_limited_control_applies(&blackwellCard, &blackwell) ||
        gpu_warning_unrecognized_family_applies(&blackwellCard)) return 12232;

    // A refused or absent board domain still warns, advanced domains or not.
    GpuCapabilityProbe notebook = turing;
    gpu_capability_set(&notebook, SERVICE_MUTATION_DOMAIN_POWER, GPU_DOMAIN_CAP_REFUSED);
    if (!gpu_warning_limited_control_applies(&turingCard, &notebook) ||
        gpu_capability_missing_board_domains(&notebook) != SERVICE_MUTATION_DOMAIN_POWER)
        return 12233;
    GpuCapabilityProbe fanless = blackwell;
    gpu_capability_set(&fanless, SERVICE_MUTATION_DOMAIN_FAN, GPU_DOMAIN_CAP_ABSENT);
    if (!gpu_warning_limited_control_applies(&blackwellCard, &fanless)) return 12234;
    // A recognized discrete family on unified memory warns with nothing missing.
    const GpuCapabilityProbe unified =
        from_snapshot(0x55554u, GPU_MEMORY_TOPOLOGY_UNIFIED);
    if (!gpu_warning_limited_control_applies(&blackwellCard, &unified)) return 12235;

    // The unrecognized-family tier follows the snapshot's best-guess flag, and
    // the reduced-surface tier never doubles it.
    const GpuAdapterInfo future = adapter(GPU_FAMILY_UNKNOWN, true);
    if (!gpu_warning_unrecognized_family_applies(&future) ||
        gpu_warning_limited_control_applies(&future, &notebook)) return 12236;

    // No adopted snapshot identity: no tier fires.
    const GpuAdapterInfo none = {};
    GpuAdapterInfo unknownBestGuess = future;
    unknownBestGuess.valid = 0;
    if (gpu_warning_unrecognized_family_applies(&none) ||
        gpu_warning_unrecognized_family_applies(&unknownBestGuess) ||
        gpu_warning_limited_control_applies(&none, &notebook) ||
        gpu_warning_unrecognized_family_applies(nullptr) ||
        gpu_warning_limited_control_applies(nullptr, &notebook)) return 12237;
    return 0;
}

#ifdef GC_SUPPORT_WARNING_TEST_MAIN
int main() {
    const int result = run_gpu_support_warning_tests();
    printf("GPU support warning regression: %d\n", result);
    return result;
}
#endif
