// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#include "profile_control_feasibility.h"
#include <stdio.h>

namespace {
HardwareCaptureObservation observation() {
    HardwareCaptureObservation o{}; o.valid = o.topologyComplete = 1;
    o.schemaVersion = HARDWARE_CAPTURE_SCHEMA_CURRENT; o.requestedDomains = 511; o.gpuGeneration = 1;
    o.binding.valid = 1; o.osFamily = PROFILE_SOURCE_OS_WINDOWS;
    for (int i = 0; i < 9; ++i) hardware_capture_domain(o, 1 << i, CAPTURE_UNSUPPORTED);
    o.pointCount = 4;
    for (int i = 0; i < 4; ++i) {
        auto& p = o.points[i]; p.index = (gc_u32)i + 2; p.voltageUv = 650000 + i * 50000;
        p.writableState = p.offsetValid = p.frequencyValid = 1;
        p.frequencyHz = 900000000 + (gc_i64)i * 50000000;
    }
    o.vfLimits = {1, 1, 1, 1, VF_OFFSET_RANGE_SOURCE_NVML_GRAPHICS, -1000000000, 1000000000, 500};
    o.composition = CAPTURE_COMPOSITION_INDEPENDENT;
    o.graphics.valid = o.graphics.writable = 1; o.graphics.grid = 1000000; o.graphics.surface = CLOCK_OFFSET_SURFACE_P0;
    o.memory = o.graphics; o.memory.grid = 500000;
    o.power.currentValid = o.power.defaultValid = o.power.writable = o.power.rangeValid = 1;
    o.power.currentMw = o.power.defaultMw = 250000; o.power.minimumMw = 125000; o.power.maximumMw = 300000; o.power.gridMw = 1;
    hardware_capture_domain(o, PROFILE_DOMAIN_VF_CURVE, CAPTURE_COMPLETE);
    hardware_capture_domain(o, PROFILE_DOMAIN_GRAPHICS_OFFSET, CAPTURE_COMPLETE);
    hardware_capture_domain(o, PROFILE_DOMAIN_MEMORY, CAPTURE_COMPLETE);
    hardware_capture_domain(o, PROFILE_DOMAIN_POWER, CAPTURE_COMPLETE);
    return o;
}
DesiredSettings bound(const HardwareCaptureObservation& o) {
    DesiredSettings d{}; d.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    d.authoring.topologyBound = 1; d.authoring.topologyCount = o.pointCount;
    for (gc_u32 i = 0; i < o.pointCount; ++i) d.authoring.voltageUv[o.points[i].index] = o.points[i].voltageUv;
    return d;
}
}
int run_profile_control_feasibility_tests() {
    auto o = observation(); auto d = bound(o);
    if (!hardware_capture_observation_valid(o) || !desired_authoring_valid(&d)) return 8915;
    d.curvePointCaptured[2] = 1; d.curvePointCapturedOffsetHz[2] = -500;
    auto p = profile_control_plan(d, o);
    if (!p.curveChecked || p.disposition != PROFILE_CONTROL_READY) return 8916;
    d.curvePointCapturedOffsetHz[2] = -250; p = profile_control_plan(d, o);
    if (p.disposition != PROFILE_CONTROL_REFUSED || !p.curve.issues[0].proven ||
        p.curve.issues[0].reason != CURVE_PLAN_REASON_POINT_GRID) return 8917;
    d.curvePointCapturedOffsetHz[2] = -1000000500; p = profile_control_plan(d, o);
    if (p.disposition != PROFILE_CONTROL_REFUSED) return 8918;
    d = bound(o); d.hasLock = 1; d.lockCi = 3; d.lockMHz = 1000; d.lockMode = LOCK_MODE_FLATTEN;
    d.authoring.lockPrecise = 1; d.authoring.lockFrequencyHz = 1000000000;
    d.hasGpuOffset = 1; d.authoring.graphicsPrecise = 1; d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT;
    d.hasCurvePoint[2] = d.curvePointFromGpuOffset[2] = 1; d.curvePointMHz[2] = 2000;
    p = profile_control_plan(d, o);
    if (p.disposition != PROFILE_CONTROL_READY || p.curve.disposition != CURVE_PLAN_REPRESENTABLE) return 8919;
    d.lockMHz = 800; d.authoring.lockFrequencyHz = 800000000; d.curvePointMHz[2] = 100;
    p = profile_control_plan(d, o);
    if (p.disposition != PROFILE_CONTROL_REFUSED || p.curve.issues[0].reason != CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU ||
        !p.curve.issues[0].proven || p.curve.issues[0].targetHz != 900000000) return 8920;
    o.graphics.value = -1000000; p = profile_control_plan(d, o);
    if (p.disposition != PROFILE_CONTROL_CONDITIONAL || p.curve.issues[0].proven ||
        p.curve.disposition != CURVE_PLAN_NEEDS_FRESH_BASELINE) return 8921;
    d.hasCurvePoint[2] = d.curvePointFromGpuOffset[2] = 0; d.curvePointMHz[2] = 0; d.curvePointCaptured[2] = 1;
    p = profile_control_plan(d, o);
    if (p.disposition != PROFILE_CONTROL_CONDITIONAL || p.curve.issues[0].kind != CURVE_PLAN_POINT_CAPTURED) return 8922;
    d.curvePointCaptured[2] = 0; d.hasCurvePoint[2] = 1; d.curvePointMHz[2] = 1300;
    d.authoring.pointFrequencyPrecise[2] = 1; d.authoring.pointFrequencyHz[2] = 1300000000;
    p = profile_control_plan(d, o);
    if (p.disposition != PROFILE_CONTROL_REFUSED || !p.curve.issues[0].proven) return 8923;
    ++d.authoring.voltageUv[2];
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_UNAVAILABLE) return 8924;
    --d.authoring.voltageUv[2]; o.vfLimits = {};
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_UNAVAILABLE) return 8925;
    o = observation(); d = bound(o); d.hasCurvePoint[2] = 1; d.curvePointMHz[2] = 900;
    d.authoring.pointFrequencyPrecise[2] = 1; d.authoring.pointFrequencyHz[2] = 900000250;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_REFUSED) return 8926;
    d = bound(o); d.hasLock = 1; d.lockCi = 3; d.lockMode = LOCK_MODE_HARD; d.lockMHz = 1000;
    d.authoring.lockPrecise = 1; d.authoring.lockFrequencyHz = 1000000500;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_REFUSED) return 8927;
    d = {}; d.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    if (!desired_memory_set_precise(&d, -500000, CLOCK_OFFSET_SURFACE_GLOBAL) ||
        profile_control_plan(d, o).disposition != PROFILE_CONTROL_UNAVAILABLE) return 8928;
    d = {}; d.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    if (!desired_power_set_ratio(&d, tuning_power_ratio_reduce(249, 250)) ||
        profile_control_plan(d, o).disposition != PROFILE_CONTROL_READY) return 8929;
    o.power.maximumMw = 248000; p = profile_control_plan(d, o);
    if (p.disposition != PROFILE_CONTROL_REFUSED || p.issues[0].kind != PROFILE_CONTROL_RANGE ||
        p.issues[0].requested != 249000 || p.issues[0].unit != PROFILE_CONTROL_MW) return 8930;
    o = observation(); d = {}; d.applicationScope = DESIRED_SCOPE_UPDATE_INCLUDED;
    if (!desired_memory_set_precise(&d, -500000, CLOCK_OFFSET_SURFACE_P0) ||
        profile_control_plan(d, o).disposition != PROFILE_CONTROL_READY) return 8931;
    o.memory.grid = 0;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_UNAVAILABLE) return 8932;
    d = {}; d.hasFan = 1; d.fanMode = FAN_MODE_AUTO; d.fanAuto = 1;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_CONDITIONAL) return 8933;
    d = {}; d.hasXbarMsvddOffsetUv = 1; d.xbarMsvddOffsetUv = 125;
    o.xbarVoltage.valid = o.xbarVoltage.writable = 1; o.xbarVoltage.grid = 1000;
    hardware_capture_domain(o, PROFILE_DOMAIN_XBAR, CAPTURE_COMPLETE); p = profile_control_plan(d, o);
    if (p.disposition != PROFILE_CONTROL_REFUSED || p.issues[0].unit != PROFILE_CONTROL_UV) return 8934;
    d = {}; desired_power_set_ratio(&d, tuning_power_ratio_reduce(1, 1)); o.power.defaultMw = (gc_i64)UINT_MAX + 1;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_UNAVAILABLE) return 8935;
    o = observation(); d = bound(o); d.curvePointCaptured[2] = 1; o.topologyComplete = 0; o.vfLimits = {};
    hardware_capture_domain(o, PROFILE_DOMAIN_VF_CURVE, CAPTURE_UNSUPPORTED);
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_UNAVAILABLE) return 8936;
    o = observation(); d = bound(o); d.curvePointCaptured[2] = 1; o.points[0].writableState = 2;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_REFUSED) return 8937;
    o = observation(); d = bound(o); d.curvePointCaptured[2] = 1; o.points[1].voltageUv = o.points[0].voltageUv;
    d.authoring.voltageUv[3] = o.points[1].voltageUv;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_UNAVAILABLE) return 8938;
    o = observation(); d = {}; desired_power_set_ratio(&d, tuning_power_ratio_reduce(249999, 250000));
    ++o.power.defaultMw; p = profile_control_plan(d, o);
    if (p.disposition != PROFILE_CONTROL_REFUSED || p.issues[0].kind != PROFILE_CONTROL_POWER_CONVERSION) return 8939;
    o = observation(); d = bound(o); d.curvePointCaptured[2] = 1; o.points[2].writableState = 2;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_UNAVAILABLE) return 8940;
    o = observation(); d = {}; d.hasGpuOffset = 1; d.authoring.graphicsPrecise = 1;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_P0; d.authoring.graphicsOffsetHz = 500000;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_REFUSED) return 8943;
    d = bound(o); d.hasGpuOffset = 1; d.authoring.graphicsPrecise = 1;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT; d.authoring.graphicsOffsetHz = 250;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_REFUSED) return 8944;
    for (gc_u32 i = 0; i < o.pointCount; ++i) d.curvePointCaptured[o.points[i].index] = 1;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_READY) return 8945;
    d = bound(o); d.hasGpuOffset = 1; d.authoring.graphicsPrecise = 1;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_P0; d.authoring.graphicsOffsetHz = 250;
    d.hasCurvePoint[2] = d.curvePointFromGpuOffset[2] = 1; d.curvePointMHz[2] = 900;
    desired_memory_set_precise(&d, -500000, CLOCK_OFFSET_SURFACE_P0);
    o.memory.rangeValid = 1; o.memory.minimum = 0; o.memory.maximum = 1000000;
    desired_power_set_ratio(&d, tuning_power_ratio_reduce(249999, 250000)); ++o.power.defaultMw;
    d.hasXbarOffsetKhz = d.hasXbarMsvddOffsetUv = d.hasSysClkOffsetKhz = d.hasVideoClkOffsetKhz = 1;
    d.hasFan = d.fanAuto = 1; d.fanMode = FAN_MODE_AUTO; o.vfLimits.gridValid = 0; o.vfLimits.gridHz = 0;
    p = profile_control_plan(d, o);
    if (p.disposition != PROFILE_CONTROL_REFUSED || p.issueCount != 9 || p.omittedIssues != 1) return 8946;
    o = observation(); d = bound(o); o.points[0].writableState = 2;
    d.hasGpuOffset = 1; d.gpuOffsetMHz = 200; d.authoring.graphicsPrecise = 1;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT; d.authoring.graphicsOffsetHz = 200000000;
    d.hasLock = 1; d.lockCi = 3; d.lockMHz = 950; d.lockMode = LOCK_MODE_FLATTEN;
    d.authoring.lockPrecise = 1; d.authoring.lockFrequencyHz = 950000000;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_READY) return 8947;
    o = observation(); d = bound(o);
    o.graphicsAlternate = o.graphics; o.graphicsAlternate.surface = CLOCK_OFFSET_SURFACE_GLOBAL;
    o.memoryAlternate = o.memory; o.memoryAlternate.surface = CLOCK_OFFSET_SURFACE_GLOBAL;
    d.hasGpuOffset = 1; d.gpuOffsetMHz = -17; d.authoring.graphicsPrecise = 1;
    d.authoring.graphicsSurface = DESIRED_GRAPHICS_GLOBAL; d.authoring.graphicsOffsetHz = -17000000;
    desired_memory_set_precise(&d, -123500000, CLOCK_OFFSET_SURFACE_GLOBAL);
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_READY) return 9111;
    o.graphics.value = 1000000;
    p = profile_control_plan(d, o);
    if (p.disposition != PROFILE_CONTROL_CONDITIONAL || p.issueCount != 1 ||
        p.issues[0].kind != PROFILE_CONTROL_RELATIONSHIP) return 9112;
    o.graphics.value = 0; o.graphicsAlternate.rangeValid = 1; o.graphicsAlternate.minimum = 0; o.graphicsAlternate.maximum = 2000000;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_REFUSED) return 9113;
    o.graphicsAlternate.rangeValid = 0; o.graphicsAlternate.minimum = o.graphicsAlternate.maximum = 0;
    o.graphicsAlternate.valid = 0;
    if (profile_control_plan(d, o).disposition != PROFILE_CONTROL_UNAVAILABLE) return 9114;
    return 0;
}
#ifdef GC_PROFILE_CONTROL_FIXTURE_MAIN
int main() { const int result = run_profile_control_feasibility_tests(); if (result) fprintf(stderr, "profile controls: %d\n", result); return result; }
#endif
