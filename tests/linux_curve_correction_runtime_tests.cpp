// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "linux_backend.h"
#include "linux_curve_targets.h"
#include "curve_authoring_targets.h"
#include "linux_backend_curve_correction.h"
#include "vf_control_write_policy.h"
#include <cstdio>
namespace {
struct LinuxApplyTransactionContext {
    LinuxGpuState* gpu;
    const DesiredSettings* desired;
    const DesiredSettings* committedIntent;
    bool transitionBuild;
    LinuxCurveTargetBuildResult plannedBuild;
    int curveTargets[VF_NUM_POINTS];
    bool curveMask[VF_NUM_POINTS];
};
static int writes;
static bool writeOk;
static void lb_log(const char*, ...) {}
static void curve_offset_range_khz(LinuxGpuState*, int* mn, int* mx) {
    *mn = -1000000;
    *mx = 1000000;
}
static LinuxCurveTargetBuildResult linux_apply_build_curve(LinuxApplyTransactionContext*, int*, bool, bool) {
    return {};
}
static bool nvapi_read_vf_snapshot(LinuxGpuState*, char*, size_t) { return true; }
static bool linux_apply_verify_curve_write(LinuxApplyTransactionContext*, bool) { return false; }
static bool apply_curve_offsets_verified(LinuxGpuState* g, const int*, const bool*, int, const gc_i64*,
                                         const bool*, VfControlWritePolicy) {
    ++writes;
    g->curve[3].freq_kHz = 1900000;
    // One typed point landed, another projected point lost its requested offset.
    g->freqOffsets[4] = 0;
    return writeOk;
}
#include "linux_backend_curve_correction_runtime.h"
} // namespace

int run_linux_curve_correction_runtime_tests() {
    LinuxGpuState g{};
    DesiredSettings d{};
    LinuxApplyTransactionContext c{};
    c.gpu = &g;
    c.desired = &d;
    g.vfSnapshotFresh = g.vfFrequenciesHzValid = g.vfOffsetsHzValid = true;
    for (int i = 1; i <= 4; ++i) {
        g.curve[i].freq_kHz = 1600000 + 50000 * i;
        g.curve[i].volt_uV = 700000 + 6250 * i;
    }
    d.hasGpuOffset = 1;
    d.gpuOffsetMHz = 90;
    d.hasCurvePoint[3] = 1;
    d.curvePointMHz[3] = 1900;
    c.plannedBuild =
        linux_build_curve_targets(g.curve, g.freqOffsets, &d, -1000000, c.curveTargets, c.curveMask);
    for (int i = 1; i <= 4; ++i) {
        g.freqOffsets[i] = c.curveTargets[i];
        g.curve[i].freq_kHz += g.freqOffsets[i];
    }
    g.curve[3].freq_kHz -= 20000;
    const bool corrected = linux_apply_correct_curve_write(&c, VF_WRITE_EXISTING_STRATEGY, 0);
    if (corrected || writes != 1)
        return 12701;
    // A successful low-level write must also survive the full snapshot.
    writeOk = true;
    writes = 0;
    g.curve[3].freq_kHz -= 20000;
    if (linux_apply_correct_curve_write(&c, VF_WRITE_EXISTING_STRATEGY, 0) || !writes)
        return 12702;
    c.curveTargets[3] = 100000;
    g.curve[3].freq_kHz = 1800000;
    g.freqOffsets[3] = 100000;
    g.vfSnapshotFresh = g.vfOffsetsHzValid = g.vfFrequenciesHzValid = false;
    writes = 0;
    if (linux_apply_correct_curve_write(&c, VF_WRITE_EXISTING_STRATEGY, 0) || writes)
        return 12703;
    g.vfSnapshotFresh = g.vfOffsetsHzValid = g.vfFrequenciesHzValid = true;
    d.hasGpuOffset = 0;
    d.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    d.curvePointCaptured[2] = 1;
    d.curvePointCapturedOffsetHz[2] = 15000000;
    memset(g.freqOffsets, 0, sizeof(g.freqOffsets));
    memset(g.freqOffsetsHz, 0, sizeof(g.freqOffsetsHz));
    c.plannedBuild =
        linux_build_curve_targets(g.curve, g.freqOffsets, &d, -1000000, c.curveTargets, c.curveMask);
    g.freqOffsetsHz[2] = 15000000;
    g.freqOffsets[2] = 15000;
    g.curve[3].freq_kHz = 1880000;
    writes = 0;
    const bool single = linux_apply_correct_curve_write(&c, VF_WRITE_SINGLE_BATCH, 0);
    if (single || writes || !c.plannedBuild.pointCapturedIntent[2] || curve_authoring_needs_exact_targets(&d))
        return 12704;
    return 0;
}
