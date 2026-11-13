// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_backend.h"
#include "service_scoped_apply_policy.h"
namespace linux_authoring_factory_fixture {
static int reads, writes;
static void lb_log(const char*, ...) {}
static void curve_offset_range_khz(LinuxGpuState*, int* minimum, int* maximum) {
    *minimum = -1000000; *maximum = 1000000;
}
static bool desired_gpu_offset_uses_curve(const DesiredSettings* d) {
    return d && service_scoped_graphics_offset_in_vf(*d, false, true);
}
static bool nvapi_read_curve(LinuxGpuState*) { ++reads; return true; }
static bool nvapi_read_vf_snapshot(LinuxGpuState*, char*, size_t) { return false; }
static bool apply_curve_offsets_verified(LinuxGpuState*, const int*, const bool*, int, const gc_i64*, const bool*) {
    ++writes; return false;
}
#include "linux_backend_curve_build.h"
#include "linux_backend_apply_context.h"
#include "linux_backend_curve_plan.h"
static int run() {
    reads = writes = 0;
    LinuxGpuState gpu{}; DesiredSettings d{}; LinuxHardwareSnapshot snapshot{};
    gpu.backend = &g_vfBackendPascal; gpu.vfMask[0] = 15;
    gpu.vfFrequenciesHzValid = gpu.vfOffsetsHzValid = true;
    d.resetOcBeforeApply = 1; d.authoring.topologyBound = 1; d.authoring.topologyCount = 4;
    for (int i = 0; i < 4; ++i) {
        gpu.curve[i] = {(unsigned)(600000 + i * 100000), (unsigned)(650000 + i * 10000)};
        gpu.curveFrequencyHz[i] = (gc_i64)gpu.curve[i].freq_kHz * 1000 + 500;
        d.authoring.voltageUv[i] = gpu.curve[i].volt_uV;
    }
    d.hasGpuOffset = 1; d.authoring.graphicsPrecise = 1; d.authoring.graphicsOffsetHz = 500;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT;
    d.authoring.identityExclusions = 1; d.authoring.graphicsExcluded[0] = 1;
    d.curvePointCaptured[1] = 1; d.curvePointCapturedOffsetHz[1] = -500;
    d.hasCurvePoint[2] = 1; d.curvePointMHz[2] = 2000;
    d.authoring.pointFrequencyPrecise[2] = 1; d.authoring.pointFrequencyHz[2] = 2000000000;
    d.hasCurvePoint[3] = d.curvePointFromGpuOffset[3] = 1;
    snapshot.gpuOffsetValid = true;
    LinuxApplyTransactionContext context{};
    context.gpu = &gpu; context.desired = &d; context.snapshot = &snapshot; context.graphicsRouteOverride = 2;
    context.plannedBuild = linux_apply_build_curve(&context, nullptr);
    if (context.plannedBuild.failed || !context.plannedBuild.deferred || context.plannedBuild.pointCount != 3 ||
        context.plannedBuild.pointExactOffsetHz[2] || writes) return 8799;
    for (int i = 0; i < 4; ++i) { gpu.curve[i].freq_kHz += 700000; gpu.curveFrequencyHz[i] += 700000000; }
    context.rebuildCurveTargets = true;
    if (!linux_apply_rebuild_curve_targets(&context) || context.plannedBuild.deferred ||
        context.plannedBuild.pointExactOffsetHz[2] != 499999500 || context.curveTargets[2] != 499999 ||
        context.plannedBuild.pointTargetHz[2] != 2000000000 || reads != 1 || writes) return 8800;
    gpu.vfFrequenciesHzValid = false;
    if (linux_apply_rebuild_curve_targets(&context) || context.plannedBuild.pointExactOffsetHz[2] != 499999500 || writes)
        return 8801;
    gpu.vfFrequenciesHzValid = true;
    ++gpu.curve[2].volt_uV;
    if (linux_apply_rebuild_curve_targets(&context) || writes) return 8802;
    --gpu.curve[2].volt_uV;
    DesiredSettings empty{}; int targets[VF_NUM_POINTS]{}; bool mask[VF_NUM_POINTS]{}; int cleanup = 0;
    gpu.freqOffsetsHz[0] = 500;
    auto release = build_curve_transition_targets(&gpu, &d, &empty, targets, mask, &cleanup);
    if (release.failed || release.deferred || cleanup != 3 || release.pointCount != 3 || mask[0] ||
        !release.pointExactOffset[1] || !release.pointExactOffset[2] || !release.pointExactOffset[3] ||
        release.pointExactOffsetHz[1] || release.pointExactOffsetHz[2] || release.pointExactOffsetHz[3] || writes) return 8803;
    return 0;
}
}
static int linux_authoring_factory_cases() { return linux_authoring_factory_fixture::run(); }
