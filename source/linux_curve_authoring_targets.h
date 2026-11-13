// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "linux_curve_targets.h"
#include "curve_authoring_targets.h"

static inline LinuxCurveTargetBuildResult linux_build_authoring_curve_targets(const CurvePlanInput& in,
    const VfBackendSpec* backend, const unsigned char* membership, bool composeGraphics,
    int* offsetsKHz, bool* mask, CurveAuthoringTargetIssue& issue, bool allowDeferredBaseline = false) {
    LinuxCurveTargetBuildResult result{};
    CurveAuthoringTargets precise;
    if (!offsetsKHz || !mask || !curve_authoring_build_targets(in, backend, membership, composeGraphics,
            precise, issue, allowDeferredBaseline)) {
        result.failed = true; return result;
    }
    result.pointCount = precise.pointCount;
    result.deferred = precise.requiresFreshBaseline;
    result.composedGpuOffset = composeGraphics && in.desired->hasGpuOffset;
    result.lockTail = in.desired->hasLock && in.lockMode != LOCK_MODE_NONE;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        offsetsKHz[i] = (int)(precise.offsetHz[i] / 1000); mask[i] = precise.mask[i];
        result.pointMask[i] = precise.mask[i];
        result.pointVoltageUv[i] = i < in.floor.graphicsEnd ? in.curve[i].volt_uV : 0;
        result.pointAbsoluteIntent[i] = precise.absoluteMask[i];
        result.pointTargetMHz[i] = (unsigned int)(precise.absoluteHz[i] / 1000000);
        result.pointCapturedIntent[i] = precise.capturedMask[i];
        result.pointCapturedOffsetHz[i] = precise.offsetHz[i];
        result.pointExactOffset[i] = precise.mask[i]; result.pointExactOffsetHz[i] = precise.offsetHz[i];
        result.pointFrequencyPrecise[i] = precise.absoluteMask[i]; result.pointTargetHz[i] = precise.absoluteHz[i];
    }
    return result;
}
