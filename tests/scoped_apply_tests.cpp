// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "gpu_core.h"
#include "platform.h"
#include "service_scoped_apply_policy.h"
#include "service_lifecycle_policy.h"
#include "linux_curve_targets.h"
#include <cstring>

int run_scoped_apply_tests() {
    VFCurvePoint curve[VF_NUM_POINTS]{};
    int offsets[VF_NUM_POINTS]{};
    for (int ci = 0; ci < 8; ++ci)
        curve[ci] = {1000000u + (unsigned)ci * 100000u, 600000u + (unsigned)ci * 25000u};
    curve[8] = {5000000, 500000}; // foreign clock domain
    VfFloorTopology floor = vf_floor_topology_compute(curve, offsets, VF_NUM_POINTS);
    DesiredSettings old{};
    old.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    old.hasPowerLimit = old.hasMemOffset = old.hasFan = 1;
    old.powerLimitPct = 80; old.memOffsetMHz = 123; old.fanMode = FAN_MODE_FIXED; old.fanPercent = 40;
    old.curvePointCaptured[2] = old.curvePointCaptured[3] = 1;
    old.curvePointCapturedOffsetHz[2] = -15000500;
    old.hasLock = old.lockTracksAnchor = 1;
    old.lockCi = 5; old.lockMHz = 1500; old.lockMode = LOCK_MODE_HARD;
    DesiredSettings request{};
    request.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    request.hasPowerLimit = 1; request.powerLimitPct = 70;
    ServiceScopedApplyPlan plan{};
    char why[192]{};
    if (!service_build_scoped_apply_plan(&old, request, nullptr, {}, false, &plan, why, sizeof(why)) ||
        !plan.owned.hasLock || !plan.owned.curvePointCaptured[3] || plan.owned.powerLimitPct != 70 ||
        plan.hardware.hasLock || plan.hardware.hasMemOffset || plan.hardware.hasFan ||
        plan.hardware.curvePointCaptured[3] || plan.cleanupPoints) return 8110;
    request = {}; request.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    request.curvePointCaptured[1] = 1; request.curvePointCapturedOffsetHz[1] = -500;
    if (!service_build_scoped_apply_plan(&old, request, curve, floor, false, &plan, why, sizeof(why)) ||
        !plan.owned.curvePointCaptured[2] || !plan.owned.curvePointCaptured[3] ||
        !plan.hardware.hasLock || plan.hardware.lockMHz != 1500 || plan.hardware.lockTracksAnchor ||
        !plan.retainedLock || plan.hardware.hasPowerLimit || plan.hardware.hasMemOffset) return 8111;
    request.curvePointCaptured[6] = 1;
    ServiceScopedApplyPlan untouched = plan;
    if (service_build_scoped_apply_plan(&old, request, curve, floor, false, &plan, why, sizeof(why)) ||
        std::memcmp(&plan, &untouched, sizeof(plan)) || !std::strstr(why, "retained lock")) return 8112;
    request = {}; request.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED | DESIRED_SCOPE_REPLACE_VF_POINTS;
    request.curvePointCaptured[1] = 1;
    if (!service_build_scoped_apply_plan(&old, request, curve, floor, false, &plan, why, sizeof(why)) ||
        plan.owned.curvePointCaptured[2] || plan.owned.curvePointCaptured[3] ||
        !plan.owned.hasLock || plan.cleanupPoints != 2 ||
        !plan.hardware.curvePointCaptured[2] || plan.hardware.curvePointCapturedOffsetHz[2] ||
        plan.hardware.curvePointCaptured[5]) return 8113;
    request = {}; request.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
    request.hasFan = 1; request.fanAuto = 1; request.fanMode = FAN_MODE_AUTO;
    if (!service_build_scoped_apply_plan(&old, request, curve, floor, false, &plan, why, sizeof(why)) ||
        plan.owned.hasLock || plan.owned.hasMemOffset || plan.owned.hasPowerLimit ||
        !plan.owned.hasFan || plan.cleanupPoints != 5 || plan.hardware.resetOcBeforeApply ||
        !plan.hardware.hasLock || plan.hardware.lockMode != LOCK_MODE_NONE ||
        !plan.hardware.hasMemOffset || plan.hardware.memOffsetMHz ||
        plan.hardware.powerLimitPct != 100 || plan.hardware.curvePointCaptured[8]) return 8114;
    // Cleanup reaches the real production Linux target compiler, including
    // an owned zero; it never becomes part of the new durable point set.
    int targets[VF_NUM_POINTS]{}; bool mask[VF_NUM_POINTS]{};
    const LinuxCurveTargetBuildResult compiled = linux_build_curve_targets(curve, offsets,
        &plan.hardware, -1000000, targets, mask);
    if (compiled.pointCount != 5 || !compiled.pointCapturedIntent[2] ||
        targets[2] || !mask[7] || mask[8] || desired_curve_point_present(&plan.owned, 2)) return 8115;
    request = {}; request.applicationScope = DESIRED_SCOPE_EXPLICIT_RELEASE;
    request.hasPowerLimit = 1; request.curvePointCaptured[2] = 1;
    if (!service_build_scoped_apply_plan(&old, request, curve, floor, false, &plan, why, sizeof(why)) ||
        plan.owned.hasPowerLimit || plan.owned.curvePointCaptured[2] ||
        !plan.owned.curvePointCaptured[3] || !plan.owned.hasMemOffset || !plan.owned.hasFan ||
        plan.cleanupPoints != 1 || plan.hardware.powerLimitPct != 100 ||
        plan.owned.applicationScope == DESIRED_SCOPE_EXPLICIT_RELEASE) return 8116;
    DesiredSettings restore{};
    if (!service_build_full_restore_request(&plan.owned, &restore) || restore.resetOcBeforeApply ||
        restore.applicationScope == DESIRED_SCOPE_EXPLICIT_RELEASE || !restore.hasMemOffset) return 8117;
    // An independent NVML graphics scalar remains available without VF.
    request = {}; request.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    request.hasGpuOffset = 1; request.gpuOffsetMHz = 100;
    if (!service_build_scoped_apply_plan(nullptr, request, nullptr, {}, false,
            &plan, why, sizeof(why)) || !plan.hardware.hasGpuOffset) return 8118;
    // XBAR frequency and voltage are independently included fields.
    old.hasXbarOffsetKhz = old.hasXbarMsvddOffsetUv = 1;
    old.xbarOffsetKhz = 123000; old.xbarMsvddOffsetUv = -1234;
    request = {}; request.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    request.hasXbarOffsetKhz = 1; request.xbarOffsetKhz = 456000;
    if (!service_build_scoped_apply_plan(&old, request, nullptr, {}, false, &plan, why, sizeof(why)) ||
        plan.owned.xbarOffsetKhz != 456000 || !plan.owned.hasXbarMsvddOffsetUv ||
        plan.owned.xbarMsvddOffsetUv != -1234 || plan.hardware.hasXbarMsvddOffsetUv) return 8119;
    request.resetOcBeforeApply = 1;
    if (service_build_scoped_apply_plan(&old, request, curve, floor, false, &plan, why, sizeof(why))) return 8120;
    request.resetOcBeforeApply = 0; request.applicationScope = DESIRED_SCOPE_REPLACE_VF_POINTS;
    if (service_build_scoped_apply_plan(&old, request, curve, floor, false, &plan, why, sizeof(why))) return 8121;
    request.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
    old.curvePointCaptured[8] = 1; // foreign domain cannot be cleaned by guessing
    if (service_build_scoped_apply_plan(&old, request, curve, floor, false, &plan, why, sizeof(why))) return 8122;
    ServiceRequest wire{};
    wire.magic = SERVICE_PROTOCOL_MAGIC; wire.version = SERVICE_PROTOCOL_VERSION;
    wire.command = SERVICE_CMD_APPLY; wire.callerPid = 1; wire.operationId = 1;
    wire.applyOrigin = SERVICE_APPLY_ORIGIN_CLI; wire.targetGpu.valid = 1;
    wire.expectedServiceInstanceId = wire.expectedGpuGeneration = 1;
    wire.desired.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
    if (!service_request_reject_reason(&wire)) return 8123;
    wire.expectedTopologySignature = 1;
    if (service_request_reject_reason(&wire)) return 8124;
    wire.desired.resetOcBeforeApply = 1;
    if (!service_request_reject_reason(&wire)) return 8125;
    wire.desired.resetOcBeforeApply = 0; wire.desired.hasPowerLimit = 1;
    wire.desired.powerLimitPct = 0;
    if (!service_request_reject_reason(&wire)) return 8126;
    old = {}; request = {}; old.hasMemOffset = 1; old.memOffsetMHz = 123;
    request.applicationScope = DESIRED_SCOPE_EXPLICIT_RELEASE; request.hasMemOffset = 1;
    if (!service_build_scoped_apply_plan(&old, request, nullptr, {}, false,
            &plan, why, sizeof(why)) || service_scoped_has_owned_intent(plan.owned) ||
        !plan.hardware.hasMemOffset || plan.hardware.memOffsetMHz) return 8131;
    old = {}; old.hasLock = 1; old.lockMode = LOCK_MODE_FLATTEN; old.lockCi = 5; old.lockMHz = 1500;
    request = {}; request.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
    if (!service_build_scoped_apply_plan(&old, request, curve, floor, false,
            &plan, why, sizeof(why)) || !plan.releasesCurveCeiling ||
        plan.hardware.resetOcBeforeApply || !plan.hardware.hasLock || plan.owned.hasLock) return 8132;
    request.hasLock = 1; request.lockMode = LOCK_MODE_NONE; request.lockCi = -1;
    if (!service_build_scoped_apply_plan(&old, request, curve, floor, false,
            &plan, why, sizeof(why)) || plan.owned.hasLock || !plan.hardware.hasLock ||
        !plan.releasesCurveCeiling) return 8138;
    // Selective VF ownership is separate from a replacement uniform scalar.
    old = {}; old.hasGpuOffset = 1; old.gpuOffsetMHz = 200; old.gpuOffsetExcludeLowCount = 3;
    old.hasCurvePoint[1] = 1; old.curvePointMHz[1] = 1700;
    request = {}; request.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
    request.hasGpuOffset = 1; request.gpuOffsetMHz = 100;
    if (!service_build_scoped_apply_plan(&old, request, curve, floor, false,
            &plan, why, sizeof(why), true) || plan.graphicsOffsetViaVf || plan.cleanupPoints != 6 ||
        !plan.hardware.curvePointCaptured[1] || !plan.hardware.curvePointCaptured[7] ||
        plan.hardware.curvePointCaptured[0] || plan.hardware.curvePointCaptured[2]) return 8139;
    const LinuxCurveTargetBuildResult scalarCleanup = linux_build_curve_targets(curve, offsets,
        &plan.hardware, -1000000, targets, mask, 1);
    if (scalarCleanup.composedGpuOffset || scalarCleanup.pointCount != 6 || mask[0] || mask[2] ||
        !mask[1] || !mask[7] || targets[1] || targets[7]) return 8140;
    old = {}; old.hasGpuOffset = 1; old.gpuOffsetMHz = 300;
    request = {}; request.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    request.hasCurvePoint[2] = 1; request.curvePointMHz[2] = 1400;
    if (!service_build_scoped_apply_plan(&old, request, curve, floor, false,
            &plan, why, sizeof(why), true) || plan.graphicsOffsetViaVf ||
        plan.hardware.hasGpuOffset || !plan.owned.hasGpuOffset) return 8141;
    return 0;
}
