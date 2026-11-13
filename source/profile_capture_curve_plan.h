// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "hardware_capture.h"
#include "curve_feasibility_plan.h"

struct ProfileCaptureCurvePlan {
    VFCurvePoint curve[VF_NUM_POINTS]{};
    int offsetsKHz[VF_NUM_POINTS]{}, componentKHz[VF_NUM_POINTS]{};
    gc_i64 frequencyHz[VF_NUM_POINTS]{}, offsetsHz[VF_NUM_POINTS]{}, componentHz[VF_NUM_POINTS]{};
    CurvePlanInput input{};
};

// Keep the capture's bound floor, membership and physical values. Never infer a
// new floor from today's frequencies or substitute observed MHz for authority.
// The input points into storage and the caller's desired value; do not copy it.
static inline bool profile_capture_curve_plan_input(const HardwareCaptureObservation& o,
    const DesiredSettings& desired, ProfileCaptureCurvePlan& storage) {
    storage = {};
    if (!o.valid || !hardware_capture_observation_valid(o) || !(o.capturedDomains & PROFILE_DOMAIN_VF_CURVE) ||
        !o.topologyComplete || !o.pointCount ||
        !o.vfLimits.valid || !o.vfLimits.gridValid || !o.vfLimits.backendWritable ||
        o.vfLimits.minimumHz % 1000 || o.vfLimits.maximumHz % 1000 ||
        o.vfLimits.minimumHz / 1000 < INT_MIN || o.vfLimits.maximumHz / 1000 > INT_MAX ||
        !desired.authoring.topologyBound || desired.gpuOffsetExcludeLowCount ||
        !desired_authoring_valid(&desired) || !desired_curve_intent_valid(&desired)) return false;
    auto& in = storage.input;
    auto& floor = in.floor; floor.firstPopulated = -1;
    bool seenWritable = false;
    gc_u8 writableState[VF_NUM_POINTS]{};
    for (gc_u32 at = 0; at < o.pointCount; ++at) {
        const auto& p = o.points[at]; const int ci = (int)p.index;
        if (!p.frequencyValid || !p.offsetValid || p.voltageUv > UINT_MAX ||
            p.frequencyHz / 1000 <= 0 || p.frequencyHz / 1000 > UINT_MAX ||
            p.offsetHz / 1000 < INT_MIN || p.offsetHz / 1000 > INT_MAX || !p.writableState) return false;
        storage.curve[ci] = {(unsigned int)(p.frequencyHz / 1000), (unsigned int)p.voltageUv};
        storage.offsetsKHz[ci] = (int)(p.offsetHz / 1000);
        storage.frequencyHz[ci] = p.frequencyHz; storage.offsetsHz[ci] = p.offsetHz;
        writableState[ci] = p.writableState;
        if (ci + 1 > floor.graphicsEnd) floor.graphicsEnd = ci + 1;
    }
    gc_i64 previousVoltage = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (desired.authoring.voltageUv[ci] != storage.curve[ci].volt_uV) return false;
        if (!storage.curve[ci].freq_kHz) continue;
        if (storage.curve[ci].volt_uV <= previousVoltage) return false;
        previousVoltage = storage.curve[ci].volt_uV;
        if (floor.firstPopulated < 0) floor.firstPopulated = floor.floorEnd = ci;
        if (writableState[ci] == 1) {
            if (!seenWritable) floor.floorEnd = ci;
            seenWritable = true;
        } else {
            // The shared planner models a leading bound floor. Other read-only
            // arrangements need an explicit backend model, never a guess.
            if (seenWritable) return false;
            ++floor.floorRun;
        }
        const bool vfComponent = desired.hasGpuOffset && desired.authoring.graphicsPrecise &&
            desired.authoring.graphicsSurface == DESIRED_GRAPHICS_VF_COMPONENT;
        const bool excluded = desired.authoring.identityExclusions && desired.authoring.graphicsExcluded[ci];
        if (vfComponent && !excluded && writableState[ci] == 1) {
            storage.componentHz[ci] = desired_graphics_hz(desired);
            storage.componentKHz[ci] = (int)(storage.componentHz[ci] / 1000);
        }
    }
    if (!seenWritable) return false;
    floor.valid = true;
    in.curve = storage.curve; in.programmedOffsetsKHz = storage.offsetsKHz;
    in.frequencyHz = storage.frequencyHz; in.programmedOffsetsHz = storage.offsetsHz;
    in.gpuComponentKHz = storage.componentKHz; in.gpuComponentHz = storage.componentHz;
    // Only an independently observed zero scalar proves absence of shaping.
    const bool scalarKnownZero = o.composition == CAPTURE_COMPOSITION_INDEPENDENT && o.graphics.valid && !o.graphics.value;
    in.evidence = curve_plan_classify_pre_write_evidence(in.curve, in.programmedOffsetsKHz, floor, scalarKnownZero, in.programmedOffsetsHz);
    in.range = {(int)(o.vfLimits.minimumHz / 1000), (int)(o.vfLimits.maximumHz / 1000),
        (CurvePlanRangeSource)o.vfLimits.rangeSource};
    in.desired = &desired; in.lockMode = desired.hasLock ? desired.lockMode : LOCK_MODE_NONE;
    in.tailStrategy = in.lockMode == LOCK_MODE_HARD ? CURVE_PLAN_TAIL_ABSOLUTE_TARGET : CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET;
    in.provenanceMode = APPLY_LOCK_PRETAIL_PROVENANCE_STORED; // Bound authoring projections have their own evidence rule.
    in.prefixAppliesToHardLock = o.osFamily == PROFILE_SOURCE_OS_WINDOWS;
    in.offsetGridHz = (int)o.vfLimits.gridHz; in.lockGridHz = 1000000;
    return true;
}
