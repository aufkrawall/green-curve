// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the Windows production exact writer/precheck using fake driver calls.
#include "gpu_core.h"
#include "vf_backends.h"
#include "vf_offset_range_policy.h"
#include "curve_preservation_precision.h"
#include "vf_exact_control_write.h"
#include "apply_correction_budget_policy.h"
#include "apply_correction_policy.h"
#include "curve_feasibility_message.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace {
using ULONGLONG = unsigned long long;
using NvApiFunc = int (*)(void*, void*);
struct App {
    const VfBackendSpec* vfBackend = &g_vfBackendPascal;
    void* gpuHandle = nullptr;
    unsigned char vfMask[32]{};
    VFCurvePoint curve[VF_NUM_POINTS]{};
    int freqOffsets[VF_NUM_POINTS]{};
    gc_i64 freqOffsetsHz[VF_NUM_POINTS]{};
    gc_i64 curveFrequencyHz[VF_NUM_POINTS]{};
    bool vfFrequenciesHzValid = true;
    bool vfOffsetsHzValid = true, loaded = true;
} g_app;
struct Driver {
    int raw[VF_NUM_POINTS]{};
    int gets = 0, sets = 0, rebuilds = 0;
    bool failRead = false, failSet = false, ignore = false, one = false, metadataBad = false;
    bool exactAvailable = true, untouched = true;
    bool modelFrequency = false;
    gc_i64 baseHz[VF_NUM_POINTS]{};
    gc_i64 shiftAfterWriteHz = 0;
} driver;
static void debug_log(const char*, ...) {}
static void set_last_apply_phase(const char*) {}
static void set_message(char* out, size_t size, const char* format, ...) {
    va_list args; va_start(args, format); vsnprintf(out, size, format, args); va_end(args);
}
static bool apply_plan_mem_offset_write(const App&, int) { return true; }
static VfOffsetRange vf_offset_range_current() { return vf_offset_range_fallback(); }
static ULONGLONG GetTickCount64() { return 50; }
static void rebuild_visible_map() { ++driver.rebuilds; }
static bool nvapi_read_curve() {
    if (driver.failRead) return false;
    if (driver.modelFrequency) for (int i = 0; i < 8; ++i) {
        gc_i64 hz = 0; vf_graphics_offset_hz(g_app.vfBackend, driver.raw[i], &hz);
        g_app.curveFrequencyHz[i] = driver.baseHz[i] + hz;
        g_app.curve[i].freq_kHz = (unsigned int)(g_app.curveFrequencyHz[i] / 1000);
    }
    return true;
}
static bool nvapi_read_offsets() {
    if (driver.failRead) return false;
    g_app.vfOffsetsHzValid = driver.exactAvailable;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        g_app.freqOffsets[i] = vf_graphics_offset_khz(g_app.vfBackend, driver.raw[i]);
        vf_graphics_offset_hz(g_app.vfBackend, driver.raw[i], &g_app.freqOffsetsHz[i]);
    }
    return true;
}
static int get(void*, void* data) {
    ++driver.gets; if (driver.failRead) return -9;
    auto* bytes = (unsigned char*)data; const auto& b = *g_app.vfBackend;
    if (driver.metadataBad) bytes[0] ^= 1;
    for (unsigned int i = 0; i < VF_NUM_POINTS; ++i)
        memcpy(bytes + b.controlEntryBaseOffset + i * b.controlEntryStride + b.controlEntryDeltaOffset, &driver.raw[i], 4);
    return 0;
}
static int set(void*, void* data) {
    ++driver.sets; if (driver.failSet) return -1;
    const auto* bytes = (const unsigned char*)data; const auto& b = *g_app.vfBackend;
    bool changed = false;
    for (unsigned int i = 0; i < VF_NUM_POINTS; ++i) {
        int value = 0; memcpy(&value, bytes + b.controlEntryBaseOffset + i * b.controlEntryStride + b.controlEntryDeltaOffset, 4);
        if (!(bytes[b.controlMaskOffset + i / 8] & (1u << (i % 8)))) {
            driver.untouched &= value == driver.raw[i]; continue;
        }
        if (driver.ignore || (driver.one && changed)) continue;
        driver.raw[i] = value; changed = true;
    }
    driver.baseHz[2] += driver.shiftAfterWriteHz; driver.shiftAfterWriteHz = 0;
    return 0;
}
static void* nvapi_qi(unsigned int id) {
    if (id == g_app.vfBackend->getControlId) return (void*)get;
    if (id == g_app.vfBackend->setControlId) return (void*)set;
    return nullptr;
}
#include "gpu_backend_exact_curve.h"
static void apply_curve_plan_log_line(const char*) {}
static CurvePlanInput apply_curve_plan_input(const DesiredSettings* d, LockMode mode,
    const VFCurvePoint* curve, const int* offsets, CurvePlanEvidence evidence,
    ApplyLockPretailProvenance provenance, bool stock, int, int* componentKHz, gc_i64* componentHz) {
    CurvePlanInput in{};
    in.curve = curve; in.programmedOffsetsKHz = offsets; in.desired = d;
    in.floor = vf_floor_topology_compute(curve, offsets, VF_NUM_POINTS);
    in.evidence = evidence; in.lockMode = mode; in.range = {-1000000, 1000000, CURVE_PLAN_RANGE_POLICY_FALLBACK};
    in.tailStrategy = mode == LOCK_MODE_HARD ? CURVE_PLAN_TAIL_ANCHOR_ONLY : CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET;
    in.provenanceMode = provenance; in.unownedAtStock = stock; in.prefixAppliesToHardLock = true;
    in.gpuComponentKHz = componentKHz; in.gpuComponentHz = componentHz;
    in.offsetGridHz = vf_graphics_offset_grid_hz(g_app.vfBackend); in.lockGridHz = 1000000;
    return in;
}
static bool read_live_curve_snapshot_settled(int, int, bool* offsetsOk) {
    *offsetsOk = nvapi_read_offsets(); return *offsetsOk && nvapi_read_curve();
}
#include "gpu_backend_apply_authoring.h"
static void initialize() {
    driver = {}; g_app = {}; g_app.gpuHandle = &driver;
    for (int i = 0; i < 8; ++i) {
        g_app.vfMask[i / 8] |= (unsigned char)(1u << (i % 8));
        g_app.curve[i] = {1500000, 650000u + (unsigned int)i * 6250};
    }
    driver.raw[7] = 30001;
    nvapi_read_offsets();
}
static int windows_authoring_cases() {
    DesiredSettings routeRequest{};
    const auto legacyRoute = curve_point_plan_gpu_offset_routing(true, true, 47, 3, 100, 0);
    const auto retained = windows_graphics_offset_route(routeRequest, legacyRoute);
    if (retained.gpuPolicyViaCurveBatch != legacyRoute.gpuPolicyViaCurveBatch ||
        retained.zeroPriorScalarBeforeCurveBatch != legacyRoute.zeroPriorScalarBeforeCurveBatch ||
        retained.curveBatchGpuOffsetMHz != 47) return 8866;
    routeRequest.hasGpuOffset = 1; routeRequest.authoring.graphicsPrecise = 1;
    routeRequest.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT;
    routeRequest.authoring.graphicsOffsetHz = -500;
    auto route = windows_graphics_offset_route(routeRequest, legacyRoute);
    if (!route.gpuPolicyViaCurveBatch || route.shouldApplyGpuOffset || !route.lastApplyUsedGpuOffset ||
        route.zeroPriorScalarBeforeCurveBatch || route.curveBatchGpuOffsetMHz) return 8867;
    routeRequest.authoring.graphicsOffsetHz = 0;
    route = windows_graphics_offset_route(routeRequest, legacyRoute);
    if (!route.gpuPolicyViaCurveBatch || route.shouldApplyGpuOffset || route.lastApplyUsedGpuOffset) return 8868;
    routeRequest.authoring.graphicsSurface = DESIRED_GRAPHICS_GLOBAL;
    route = windows_graphics_offset_route(routeRequest, legacyRoute);
    if (route.gpuPolicyViaCurveBatch || !route.shouldApplyGpuOffset || route.zeroPriorScalarBeforeCurveBatch) return 8869;
    initialize(); driver.modelFrequency = true;
    for (int i = 0; i < 8; ++i) driver.baseHz[i] = 600000500LL + i * 100000000LL;
    nvapi_read_curve();
    DesiredSettings d{}; d.authoring.topologyBound = 1; d.authoring.topologyCount = 8;
    for (int i = 0; i < 8; ++i) d.authoring.voltageUv[i] = g_app.curve[i].volt_uV;
    d.curvePointCaptured[0] = d.curvePointCaptured[1] = d.curvePointCaptured[3] = 1;
    d.curvePointCapturedOffsetHz[0] = 500; d.curvePointCapturedOffsetHz[1] = -500;
    d.hasCurvePoint[2] = 1; d.curvePointMHz[2] = 820;
    d.authoring.pointFrequencyPrecise[2] = 1; d.authoring.pointFrequencyHz[2] = 820001000;
    CurveAuthoringTargets targets; char why[256]{};
    if (!apply_build_authoring_curve_targets(d, LOCK_MODE_NONE, false, true, g_app.curve,
            g_app.freqOffsets, g_app.curveFrequencyHz, g_app.freqOffsetsHz, CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE,
            nullptr, targets, why, sizeof(why)) || targets.offsetHz[2] != 20000500 ||
        !targets.preservedMask[7] || targets.offsetHz[7] != 15000500 || targets.capturedMask[7]) return 8810;
    const auto original = targets;
    if (!apply_authoring_precheck_before_writes(&d, false, why, sizeof(why)) || driver.sets) return 8861;
    ++d.authoring.voltageUv[7];
    if (apply_authoring_precheck_before_writes(&d, false, why, sizeof(why)) || driver.sets) return 8862;
    --d.authoring.voltageUv[7]; d.curvePointCapturedOffsetHz[0] = 250;
    if (apply_authoring_precheck_before_writes(&d, false, why, sizeof(why)) || driver.sets) return 8863;
    d.curvePointCapturedOffsetHz[0] = 500;
    d.authoring.pointFrequencyHz[2] = 2000000000; d.curvePointMHz[2] = 2000; d.resetOcBeforeApply = 1;
    if (!apply_authoring_precheck_before_writes(&d, false, why, sizeof(why)) || driver.sets) return 8864;
    d.resetOcBeforeApply = 0;
    if (apply_authoring_precheck_before_writes(&d, false, why, sizeof(why)) || driver.sets) return 8865;
    d.authoring.pointFrequencyHz[2] = 820001000; d.curvePointMHz[2] = 820;
    driver.raw[7] = 0; // The memory setter cleared an unowned remainder.
    if (!apply_curve_offsets_exact_hz(targets.offsetHz, targets.mask, 2, 0, targets.voltageUv) ||
        !apply_verify_authoring_curve_targets(targets, LOCK_MODE_NONE, -1, why, sizeof(why)) ||
        driver.raw[0] != 1 || driver.raw[1] != -1 || driver.raw[7] != 30001) return 8811;
    driver.baseHz[2] += 20000000; nvapi_read_curve();
    if (apply_verify_authoring_curve_targets(targets, LOCK_MODE_NONE, -1, why, sizeof(why))) return 8812;
    // A second shift requires another correction. Every pass keeps captured
    // raw controls and the original unowned preimage, with no MHz rewrite.
    driver.raw[7] = 0; nvapi_read_offsets(); nvapi_read_curve(); driver.shiftAfterWriteHz = 10000000;
    if (!apply_correct_authoring_curve_targets(d, LOCK_MODE_NONE, false, true, targets, 0, 0, why, sizeof(why)) ||
        driver.raw[0] != 1 || driver.raw[1] != -1 || driver.raw[2] != -19999 || driver.raw[7] != 30001 ||
        targets.absoluteHz[2] != 820001000 || targets.offsetHz[7] != original.offsetHz[7] ||
        d.authoring.pointFrequencyHz[2] != 820001000 || !windows_authoring_curve_shape_matches(original, targets)) return 8813;
    auto tally = apply_authoring_curve_boost_tally(targets, LOCK_MODE_NONE, -1);
    if (tally.applied != 4 || tally.failed || tally.userApplied != 3) return 8820;
    g_app.freqOffsetsHz[0] = 0;
    tally = apply_authoring_curve_boost_tally(targets, LOCK_MODE_NONE, -1);
    if (tally.applied != 3 || tally.failed != 1 || tally.firstFailedCi != 0 ||
        apply_verify_authoring_curve_targets(targets, LOCK_MODE_NONE, -1, why, sizeof(why))) return 8821;
    g_app.freqOffsetsHz[0] = 500;
    // Exact tolerance boundaries cannot gain half a kHz by display rounding.
    driver.baseHz[2] += 8000000; nvapi_read_curve();
    if (!apply_verify_authoring_curve_targets(targets, LOCK_MODE_NONE, -1, why, sizeof(why))) return 8814;
    driver.baseHz[2] += 500; nvapi_read_curve();
    if (apply_verify_authoring_curve_targets(targets, LOCK_MODE_NONE, -1, why, sizeof(why))) return 8815;
    const int writes = driver.sets;
    ++g_app.curve[6].volt_uV;
    if (apply_correct_authoring_curve_targets(d, LOCK_MODE_NONE, false, true, targets, 0, 0, why, sizeof(why)) ||
        driver.sets != writes) return 8816;
    --g_app.curve[6].volt_uV; g_app.vfFrequenciesHzValid = false;
    if (apply_correct_authoring_curve_targets(d, LOCK_MODE_NONE, false, true, targets, 0, 0, why, sizeof(why)) ||
        driver.sets != writes) return 8817;
    g_app.vfFrequenciesHzValid = true; g_app.vfOffsetsHzValid = false;
    if (apply_verify_authoring_curve_targets(targets, LOCK_MODE_NONE, -1, why, sizeof(why))) return 8818;
    g_app.vfOffsetsHzValid = true; ++d.authoring.pointFrequencyHz[2];
    if (apply_build_authoring_curve_targets(d, LOCK_MODE_NONE, false, true, g_app.curve, g_app.freqOffsets,
            g_app.curveFrequencyHz, g_app.freqOffsetsHz, CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE,
            &original, targets, why, sizeof(why)) || driver.sets != writes) return 8819;
    --d.authoring.pointFrequencyHz[2];
    CurveAuthoringTargets partial;
    if (!apply_build_authoring_curve_targets(d, LOCK_MODE_NONE, false, false, g_app.curve, g_app.freqOffsets,
            g_app.curveFrequencyHz, g_app.freqOffsetsHz, CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE,
            nullptr, partial, why, sizeof(why)) || partial.pointCount != 4 || partial.mask[7]) return 8822;
    const auto saved = partial;
    g_app.vfMask[0] &= (unsigned char)~128u;
    if (apply_build_authoring_curve_targets(d, LOCK_MODE_NONE, false, true, g_app.curve, g_app.freqOffsets,
            g_app.curveFrequencyHz, g_app.freqOffsetsHz, CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE,
            nullptr, partial, why, sizeof(why)) || memcmp(&saved, &partial, sizeof(saved)) ||
        driver.sets != writes) return 8823;
    g_app.vfMask[0] |= 128;
    // A raw offset before a flatten is judged over its current physical
    // baseline. A sub-kHz prefix conflict cannot vanish in MHz projections.
    auto flat = partial; flat.absoluteMask[3] = true; flat.absoluteHz[3] = 900000000;
    g_app.curveFrequencyHz[0] = 900000500; g_app.curve[0].freq_kHz = 900000;
    if (windows_verify_authoring_curve_targets(g_app.curve, g_app.curveFrequencyHz, g_app.freqOffsetsHz,
            true, true, flat, LOCK_MODE_FLATTEN, 3, why, sizeof(why))) return 8824;
    driver.baseHz[2] -= 500; nvapi_read_curve();
    if (!windows_verify_authoring_curve_targets(g_app.curve, g_app.curveFrequencyHz, g_app.freqOffsetsHz,
            true, true, flat, LOCK_MODE_FLATTEN, 3, why, sizeof(why))) return 8825;
    // A hard pin owns its frequency tail, but preserved tail controls remain
    // exact raw authority even where ordinary VF targets are skipped.
    auto hard = original; hard.preservedMask[7] = true;
    g_app.freqOffsetsHz[7] = 0;
    if (windows_verify_authoring_curve_targets(g_app.curve, g_app.curveFrequencyHz, g_app.freqOffsetsHz,
            true, true, hard, LOCK_MODE_HARD, 4, why, sizeof(why))) return 8826;
    g_app.freqOffsetsHz[7] = original.offsetHz[7];
    // Unchanged raw requests terminate without another setter; a moved
    // physical baseline at an absolute point can be corrected independently.
    targets = original; driver.baseHz[2] = 800000500;
    driver.raw[2] = 40001; driver.raw[0] = 0;
    nvapi_read_offsets(); nvapi_read_curve();
    const int beforeUnchanged = driver.sets;
    if (apply_correct_authoring_curve_targets(d, LOCK_MODE_NONE, false, true, targets, 0, 0, why, sizeof(why)) ||
        driver.sets != beforeUnchanged) return 8827;
    return 0;
}
}

int run_windows_vf_exact_tests() {
    if (int failure = windows_authoring_cases()) return failure;
    gc_i64 hz[VF_NUM_POINTS]{}; bool mask[VF_NUM_POINTS]{};
    initialize(); mask[0] = mask[1] = mask[2] = true;
    hz[0] = 15000500; hz[1] = -15000500; driver.raw[2] = 1;
    if (!apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.raw[0] != 30001 || driver.raw[1] != -30001 ||
        driver.raw[2] || driver.raw[7] != 30001 || !driver.untouched || driver.sets != 1 || driver.rebuilds != 1) return 8580;
    if (!apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.sets != 1) return 8581;
    initialize(); driver.one = true; hz[2] = 1000000;
    if (!apply_curve_offsets_exact_hz(hz, mask, 1, 0) || driver.sets != 3 || !driver.untouched || driver.raw[2] != 2000) return 8582;
    initialize(); hz[0] = 15000501;
    if (apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.sets) return 8583;
    hz[0] = 15000500; driver.metadataBad = true;
    if (apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.sets) return 8584;
    initialize(); driver.ignore = true;
    if (apply_curve_offsets_exact_hz(hz, mask, 1, 0) || driver.sets != 2) return 8585;
    initialize(); driver.failSet = true;
    if (apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.sets != 1) return 8586;
    initialize();
    if (apply_curve_offsets_exact_hz(hz, mask, 2, 49) || driver.sets) return 8587;
    initialize(); mask[8] = true;
    if (apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.sets) return 8588;
    mask[8] = false;
    DesiredSettings desired{}; desired.hasMemOffset = 1; desired.memOffsetMHz = 1;
    char why[192]{}; initialize(); driver.raw[0] = 1; nvapi_read_offsets();
    if (!curve_control_nonzero(0, true, 500) || curve_control_nonzero(1, true, 0) ||
        !apply_curve_preservation_precheck(&desired, why, sizeof(why)) || driver.sets) return 8589;
    driver.failRead = true;
    if (apply_curve_preservation_precheck(&desired, why, sizeof(why)) || driver.sets) return 8590;
    initialize(); driver.exactAvailable = false;
    if (apply_curve_preservation_precheck(&desired, why, sizeof(why)) || driver.sets) return 8591;
    initialize(); driver.raw[0] = 2000001;
    if (apply_curve_preservation_precheck(&desired, why, sizeof(why)) || driver.sets) return 8592;
    initialize(); driver.raw[0] = 1; g_app.vfMask[0] &= (unsigned char)~1u;
    if (apply_curve_preservation_precheck(&desired, why, sizeof(why)) || driver.sets) return 8593;
    initialize(); int old[VF_NUM_POINTS]{}, target[VF_NUM_POINTS]{}; gc_i64 original[VF_NUM_POINTS]{};
    bool tail[VF_NUM_POINTS]{}; original[0] = 500; original[1] = -500; original[2] = 15000500;
    old[2] = 15000; target[0] = 1000; target[1] = -1000; target[2] = 17000;
    desired.curvePointCaptured[2] = 1; desired.curvePointCapturedOffsetHz[2] = 0;
    if (!curve_preservation_targets(desired, mask, tail, target, old, original, true, hz) ||
        hz[0] != 1000500 || hz[1] != -1000500 || hz[2] != 0) return 8594;
    if (curve_preservation_targets(desired, mask, tail, target, old, original, false, hz)) return 8595;
    tail[0] = true; mask[1] = false;
    if (!curve_preservation_targets(desired, mask, tail, target, old, original, true, hz) || hz[0] != 1000000) return 8596;
    initialize(); desired = {}; desired.hasMemOffset = 1; desired.memOffsetMHz = 1;
    driver.raw[0] = 1; nvapi_read_offsets();
    mask[0] = true; mask[1] = mask[2] = false; tail[0] = false;
    target[0] = old[0] = 0; original[0] = g_app.freqOffsetsHz[0];
    if (!apply_curve_preservation_precheck(&desired, why, sizeof(why)) ||
        !curve_preservation_targets(desired, mask, tail, target, old, original, true, hz)) return 8597;
    driver.raw[0] = 0; // A memory setter reset an unowned half-kHz control.
    if (!apply_curve_offsets_exact_hz(hz, mask, 2, 0) || driver.raw[0] != 1 ||
        desired.curvePointCaptured[0] || desired.hasCurvePoint[0]) return 8598;
    initialize(); g_app.loaded = false; driver.failRead = true;
    if (!apply_curve_preservation_precheck(&desired, why, sizeof(why)) || driver.sets) return 8599;
    initialize(); unsigned int voltages[VF_NUM_POINTS]{};
    for (int i = 0; i < VF_NUM_POINTS; ++i) voltages[i] = g_app.curve[i].volt_uV;
    g_app.curve[0].volt_uV += 1250;
    if (apply_curve_offsets_exact_hz(hz, mask, 2, 0, voltages) || driver.sets) return 8600;
    // An absolute point can trigger correction after the first exact batch.
    // Every captured point must keep its original Hz authority in that pass.
    initialize(); desired = {};
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        mask[i] = i < 4; tail[i] = false;
        old[i] = g_app.freqOffsets[i]; original[i] = g_app.freqOffsetsHz[i];
        target[i] = 19000; voltages[i] = g_app.curve[i].volt_uV;
    }
    desired.curvePointCaptured[0] = desired.curvePointCaptured[1] = desired.curvePointCaptured[3] = 1;
    desired.curvePointCapturedOffsetHz[0] = 500; desired.curvePointCapturedOffsetHz[1] = -500;
    desired.hasCurvePoint[2] = 1; desired.curvePointMHz[2] = 1500;
    driver.raw[3] = 1;
    if (!apply_curve_correction_exact_hz(desired, target, mask, tail, old, original, true, 2, 0, voltages) ||
        driver.raw[0] != 1 || driver.raw[1] != -1 || driver.raw[2] != 38000 || driver.raw[3] ||
        driver.raw[7] != 30001 || !driver.untouched) return 8780;
    initialize(); desired.curvePointCapturedOffsetHz[0] = 250;
    if (apply_curve_correction_exact_hz(desired, target, mask, tail, old, original, true, 2, 0, voltages) ||
        driver.sets || driver.raw[0] || driver.raw[7] != 30001) return 8781;
    desired.curvePointCapturedOffsetHz[0] = 1000000500;
    if (apply_curve_correction_exact_hz(desired, target, mask, tail, old, original, true, 2, 0, voltages) ||
        driver.sets) return 8782;
    return 0;
}
