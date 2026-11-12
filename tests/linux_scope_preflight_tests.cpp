// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the production preflight with no readable/writable memory surface.
#include "linux_backend.h"
#include "vf_backends.h"

static void lb_log(const char*, ...) {}
static bool linux_clock_control_proven_absent(const LinuxGpuState*) { return false; }
static bool desired_gpu_offset_uses_curve(const DesiredSettings* d) {
    return d && d->hasGpuOffset && (d->gpuOffsetExcludeLowCount > 0 || d->hasLock || service_desired_has_curve_point(d));
}
static bool desired_has_curve_write(const DesiredSettings* d) { return service_desired_has_curve_point(d); }
static bool linux_power_request_is_inert(const DesiredSettings*, const LinuxHardwareSnapshot*, const LinuxGpuState*) { return false; }
static bool scope_fixture_curve_plausible(const LinuxGpuState*, char*, size_t) { return true; }
#define linux_backend_curve_plausible scope_fixture_curve_plausible
#include "linux_backend_preflight.h"
#undef linux_backend_curve_plausible

static int writes;
static nvmlReturn_t counted_reset(nvmlDevice_t) { ++writes; return NVML_SUCCESS; }
static nvmlReturn_t counted_offset(nvmlDevice_t, int) { ++writes; return NVML_SUCCESS; }

int run_linux_scope_preflight_tests() {
    LinuxGpuState g{};
    g.backend = &g_vfBackendFuture;
    g.nvmlReady = g.writeIdentityResolved = true;
    g.nvml.resetGpuLockedClocks = counted_reset;
    g.nvml.setGpcClkVfOffset = counted_offset;
    LinuxHardwareSnapshot snapshot{};
    snapshot.valid = snapshot.gpuOffsetValid = snapshot.curveValid = true;
    snapshot.availableMutationDomains = SERVICE_MUTATION_DOMAIN_RESET_BASELINE |
        SERVICE_MUTATION_DOMAIN_GPU_OFFSET | SERVICE_MUTATION_DOMAIN_VF_CURVE | SERVICE_MUTATION_DOMAIN_LOCK;
    DesiredSettings request{};
    request.resetOcBeforeApply = 1;
    request.hasCurvePoint[4] = 1; request.curvePointMHz[4] = 1700;
    char why[192]{};
    writes = 0;
    if (!linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 8133;
    request.hasMemOffset = 1;
    if (linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 8134;
    snapshot.availableMutationDomains |= SERVICE_MUTATION_DOMAIN_MEM_OFFSET;
    if (linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 8135;
    snapshot.memOffsetValid = true;
    if (linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 8136;
    g.nvml.setMemClkVfOffset = counted_offset;
    if (!linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 8137;
    request = {}; request.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
    request.hasGpuOffset = 1; request.gpuOffsetMHz = 100;
    request.curvePointCaptured[4] = 1; // compiled cleanup is independent of the scalar
    g.nvml.setGpcClkVfOffset = nullptr;
    if (linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why), 1) || writes) return 8142;
    g.nvml.setGpcClkVfOffset = counted_offset;
    snapshot.availableMutationDomains &= ~(gc_u32)SERVICE_MUTATION_DOMAIN_GPU_OFFSET;
    if (linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why), 1) || writes) return 8143;
    snapshot.availableMutationDomains |= SERVICE_MUTATION_DOMAIN_GPU_OFFSET;
    if (!linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why), 1) || writes) return 8144;
    return 0;
}
