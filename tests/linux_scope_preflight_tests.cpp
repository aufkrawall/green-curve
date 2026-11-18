// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the production preflight with no readable/writable memory surface.
#include "linux_backend.h"
#include "vf_backends.h"
#include <cstring>

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
static unsigned int powerDefault = 250000, powerMaximum = 190000;
static nvmlReturn_t power_current(nvmlDevice_t, unsigned int* v) { *v = 250000; return NVML_SUCCESS; }
static nvmlReturn_t power_default(nvmlDevice_t, unsigned int* v) { *v = powerDefault; return NVML_SUCCESS; }
static nvmlReturn_t power_range(nvmlDevice_t, unsigned int* lo, unsigned int* hi) {
    *lo = 100000; *hi = powerMaximum; return NVML_SUCCESS;
}
static nvmlReturn_t power_set(nvmlDevice_t, unsigned int) { ++writes; return NVML_SUCCESS; }

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
    request = {}; request.hasPowerLimit = true; request.powerLimitPct = 80;
    snapshot.powerValid = true;
    snapshot.availableMutationDomains |= SERVICE_MUTATION_DOMAIN_POWER;
    g.nvml.getPowerLimit = power_current; g.nvml.getPowerDefaultLimit = power_default;
    g.nvml.getPowerConstraints = power_range; g.nvml.setPowerLimit = power_set;
    if (linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 8197;
    powerMaximum = 300000;
    if (!linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 8198;
    powerDefault = 250001;
    if (linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 8199;
    powerDefault = 250000; powerMaximum = 190000;
    request = {}; request.hasMemOffset = 1; request.memOffsetMHz = 1;
    snapshot.curveOffsetsExact = false;
    if (linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 8621;
    snapshot.curveOffsetsExact = true;
    if (!linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 8622;
    snapshot.curveMask[4] = true; snapshot.curveOffsetsHz[4] = 500;
    if (linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 8623;
    snapshot.curveMask[4] = false; snapshot.curveOffsetsHz[4] = 0;

    // fan_request_policy.h: a notebook board answers the fan count with zero.
    // Every saved profile carries fan_mode=auto, so this undervolt request used
    // to be refused as "requested GPU domains are unavailable".
    request = {};
    request.hasCurvePoint[4] = 1; request.curvePointMHz[4] = 1700;
    request.hasFan = true; request.fanMode = FAN_MODE_AUTO; request.fanAuto = true;
    snapshot.fanValid = false; snapshot.fanCount = 0; snapshot.fanCountReported = true;
    snapshot.availableMutationDomains &= ~(gc_u32)SERVICE_MUTATION_DOMAIN_FAN;
    gpu_capability_set(&g.capability, SERVICE_MUTATION_DOMAIN_FAN, GPU_DOMAIN_CAP_ABSENT);
    if (linux_fan_request_disposition(&request, snapshot.fanCount, &g.capability) !=
            FAN_REQUEST_INERT_NO_FAN) return 12140;
    if (!linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 12141;
    // Fixed and Curve cannot exist there: refused by name, before any write.
    request.fanMode = FAN_MODE_FIXED; request.fanAuto = false; request.fanPercent = 50;
    why[0] = 0;
    if (linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes ||
        !std::strstr(why, "fan mode Fixed")) return 12142;
    request.fanMode = FAN_MODE_CURVE;
    if (linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 12143;
    // No evidence of absence keeps the domain gate.
    request.fanMode = FAN_MODE_AUTO; request.fanAuto = true;
    gpu_capability_set(&g.capability, SERVICE_MUTATION_DOMAIN_FAN, GPU_DOMAIN_CAP_UNPROBED);
    if (linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes) return 12144;
    // A reported fan whose state could not be read is still a fan: never inert.
    gpu_capability_set(&g.capability, SERVICE_MUTATION_DOMAIN_FAN, GPU_DOMAIN_CAP_REFUSED);
    snapshot.fanCount = 2;
    snapshot.availableMutationDomains |= SERVICE_MUTATION_DOMAIN_FAN;
    why[0] = 0;
    if (linux_backend_preflight(&g, &request, &snapshot, why, sizeof(why)) || writes ||
        !std::strstr(why, "cannot be snapshotted")) return 12145;
    return 0;
}
