// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Extended real-world GPU edge-case simulation suite (13085-13115):
//   - Cold-GPU mid-Apply Boost bin drift (+50 MHz on 12.5 MHz Pascal bins)
//   - Cold-GPU prefix-above-plateau refusal (+64 MHz cold boost)
//   - Cold high-boost Blackwell transition ceiling guard + 15 MHz fixed-point
//   - Ultra-deep low-clock undervolting (600 MHz Pascal / 750 MHz Blackwell)
//   - Weird-but-valid stepped S-curve (boost low, stock mid, underclock high)
//   - Negative single-point dip with monotonic driver clamp + loop termination
//   - Pascal 500 Hz sub-kHz fractional offset grid alignment & rejection
//   - Mixed typed-absolute + selective-GPU-offset + flatten under cold drift
//   - Turing idle floor-run anchor refusal vs first writable point acceptance

#include "gpu_generation_sim_harness.h"
#include "apply_clock_ceiling_policy.h"
#include "apply_correction_policy.h"
#include "gui_draft_desired_policy.h"
#include "gui_editor_base_policy.h"
#include "gui_pending_changes_policy.h"
#include "linux_curve_targets.h"

#include <memory>

namespace {
using namespace gpu_gen_sim;

static int test_cold_gpu_boost_drift_and_ceiling_guards() {
    // 13085-13090: Cold-GPU mid-Apply Boost drift on 12.5 MHz Pascal bins
    // plus cold prefix-above-plateau refusal.
    auto gp102 = std::make_unique<SimGpuDriver>();
    gp102->spec = make_pascal_gp102_spec(PROFILE_SOURCE_OS_WINDOWS);
    gp102->boostBinStepKHz = 12500;
    const VfOffsetRange pascalVf = vf_offset_range_for_vf_points(
        vf_offset_range_from_probe(true, -200000, 1200000));

    VFCurvePoint warmStock[VF_NUM_POINTS]{};
    int warmOffsets[VF_NUM_POINTS]{};
    gp102->evaluate_live_curve(warmStock, warmOffsets);

    auto d = std::make_unique<DesiredSettings>();
    d->hasLock = 1;
    d->lockMode = LOCK_MODE_FLATTEN;
    d->lockCi = 21;
    d->lockMHz = 1000;
    for (int i = 0; i < 21; ++i) {
        d->curvePointCaptured[i] = 1;
        d->curvePointCapturedOffsetHz[i] = 38000000LL;
    }
    int initTargets[VF_NUM_POINTS]{};
    bool initMask[VF_NUM_POINTS]{};
    linux_build_curve_targets(warmStock, warmOffsets, d.get(),
        vf_offset_range_flatten_floor_khz(pascalVf), initTargets, initMask);
    if (!gp102->apply_masked_offsets_khz(initTargets, initMask)) return 13085;

    // GPU cools down by +50 MHz (+4 Boost 3.0 bins) on the boost curve (>= 660 mV) mid-Apply!
    gp102->boostShiftKHz = 50000;
    gp102->boostShiftMinVoltUv = 660000u;
    VFCurvePoint coldLive[VF_NUM_POINTS]{};
    int coldOffsets[VF_NUM_POINTS]{};
    gp102->evaluate_live_curve(coldLive, coldOffsets);
    if (coldLive[79].freq_kHz > coldLive[21].freq_kHz) return 13086;

    bool lockedTailMask[VF_NUM_POINTS]{};
    bool explicitMask[VF_NUM_POINTS]{};
    bool origPop[VF_NUM_POINTS]{};
    unsigned char hasPt[VF_NUM_POINTS]{};
    unsigned int ptMHz[VF_NUM_POINTS]{};
    unsigned char fromGpu[VF_NUM_POINTS]{};
    unsigned int rbFreq[VF_NUM_POINTS]{};
    int rbOff[VF_NUM_POINTS]{};
    for (int i = 0; i < 80; ++i) {
        origPop[i] = true;
        rbFreq[i] = coldLive[i].freq_kHz;
        rbOff[i] = coldOffsets[i];
        if (i >= 21) {
            lockedTailMask[i] = true;
        } else {
            hasPt[i] = 1;
            fromGpu[i] = 1;
            ptMHz[i] = (warmStock[i].freq_kHz + 38000u) / 1000u;
        }
    }
    ApplyCorrectionRequest req{};
    req.pointCount = 80;
    req.hasLock = true;
    req.lockCi = 21;
    req.lockMHz = 1000;
    req.lockedTailMask = lockedTailMask;
    req.explicitCurveMask = explicitMask;
    req.originalCurvePopulated = origPop;
    req.hasCurvePoint = hasPt;
    req.curvePointMHz = ptMHz;
    req.fromGpuOffset = fromGpu;
    req.targetOffsetKHz = initTargets;
    ApplyCorrectionReadback rb{};
    rb.freqKHz = rbFreq;
    rb.programmedOffsetKHz = rbOff;

    int corrOut[VF_NUM_POINTS]{};
    bool corrMask[VF_NUM_POINTS]{};
    const ApplyCorrectionPlan plan = apply_correction_plan_pass(
        req, rb, pascalVf, corrOut, corrMask);
    if (!plan.haveCorrections || plan.clampedCount != 0 ||
        corrOut[0] != 38000 || corrOut[20] != 38000 ||
        corrOut[21] >= initTargets[21] || corrOut[79] != -2500000)
        return 13087;
    if (!gp102->apply_masked_offsets_khz(corrOut, corrMask)) return 13088;
    gp102->evaluate_live_curve(coldLive, coldOffsets);
    if (coldLive[21].freq_kHz > 1012500u || coldLive[79].freq_kHz != coldLive[21].freq_kHz)
        return 13089;

    // Very cold GPU (+64 MHz): pre-tail point ci=20 (937 + 64 + 38 = 1039 MHz)
    // exceeds 1000 MHz within one 51 MHz neighbor bin -> flagged conditional;
    // at +80 MHz (1055 MHz, excess 55 MHz > 51 MHz neighbor bin) -> proven refusal.
    gp102->reset_to_stock();
    gp102->boostShiftKHz = 64000;
    gp102->boostShiftMinVoltUv = 0;
    gp102->boostBinStepKHz = 0;
    VFCurvePoint veryColdStock[VF_NUM_POINTS]{};
    int zeroOff[VF_NUM_POINTS]{};
    gp102->evaluate_live_curve(veryColdStock, zeroOff);
    VfFloorBinding floor{};
    vf_floor_binding_update(&floor, veryColdStock, zeroOff, VF_NUM_POINTS, 0);
    CurvePlanInput coldPlanIn{};
    coldPlanIn.curve = veryColdStock;
    coldPlanIn.programmedOffsetsKHz = zeroOff;
    coldPlanIn.floor = floor.floor;
    coldPlanIn.evidence = CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE;
    coldPlanIn.range.minKHz = pascalVf.minKHz;
    coldPlanIn.range.maxKHz = pascalVf.maxKHz;
    coldPlanIn.range.source = CURVE_PLAN_RANGE_NVML_GRAPHICS;
    coldPlanIn.tailStrategy = CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET;
    coldPlanIn.desired = d.get();
    coldPlanIn.lockMode = LOCK_MODE_FLATTEN;
    coldPlanIn.offsetGridHz = 500;
    const CurvePlanResult coldCond = curve_plan_evaluate(coldPlanIn);
    if (coldCond.disposition != CURVE_PLAN_NEEDS_FRESH_BASELINE ||
        !plan_has_reason(coldCond, CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU))
        return 13090;
    gp102->boostShiftKHz = 80000;
    gp102->evaluate_live_curve(veryColdStock, zeroOff);
    const CurvePlanResult coldProven = curve_plan_evaluate(coldPlanIn);
    if (coldProven.disposition != CURVE_PLAN_REFUSED ||
        !plan_has_reason(coldProven, CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU))
        return 13090;

    // 13091-13092: Cold high-boost Blackwell transition ceiling guard +
    // verify-before-fixed-point exit under 15 MHz Boost bin snapping.
    const ApplyClockCeilingPlan cplan = apply_clock_ceiling_plan(
        true, true, LOCK_MODE_FLATTEN, 2800, true, true, true, 2482);
    if (!cplan.required || !cplan.arm || cplan.ceilingMHz != 2482u ||
        !apply_clock_ceiling_transition_must_refuse(
            cplan.required, APPLY_CEILING_ARM_REFUSED, cplan.reason) ||
        apply_clock_ceiling_transition_must_refuse(
            cplan.required, APPLY_CEILING_ARM_INSTALLED, cplan.reason))
        return 13091;

    int passes = 0;
    const ApplyCorrectionLoopOutcome loopOut = apply_run_correction_loop(
        4,
        [&](int) { return true; },          // mayStart
        [&](int) { ++passes; return true; }, // planWrite
        [&](int) { return passes >= 1; },   // classify: fixed point reached
        [&](int) { return true; });         // verify: within Boost bin tolerance
    if (!loopOut.verified || passes != 1) return 13092;
    return 0;
}

static int test_ultra_deep_low_clock_undervolts() {
    // 13093-13094: Extreme low-clock undervolt on Pascal (600 MHz @ ci=13, 581 mV,
    // requiring -1374 MHz on stock 1974 MHz tail).
    {
        auto gp102 = std::make_unique<SimGpuDriver>();
        gp102->spec = make_pascal_gp102_spec(PROFILE_SOURCE_OS_WINDOWS);
        VFCurvePoint stock[VF_NUM_POINTS]{};
        int zeroOff[VF_NUM_POINTS]{};
        gp102->evaluate_live_curve(stock, zeroOff);
        const VfOffsetRange vfRange = vf_offset_range_for_vf_points(
            vf_offset_range_from_probe(true, -200000, 1200000));

        auto d = std::make_unique<DesiredSettings>();
        d->hasLock = 1;
        d->lockMode = LOCK_MODE_FLATTEN;
        d->lockCi = 13;  // stock 582 MHz @ 581 mV -> +18 MHz to 600 MHz
        d->lockMHz = 600;
        VfFloorBinding floor{};
        vf_floor_binding_update(&floor, stock, zeroOff, VF_NUM_POINTS, 0);
        CurvePlanInput in{};
        in.curve = stock;
        in.programmedOffsetsKHz = zeroOff;
        in.floor = floor.floor;
        in.evidence = CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE;
        in.range.minKHz = vfRange.minKHz;
        in.range.maxKHz = vfRange.maxKHz;
        in.range.source = CURVE_PLAN_RANGE_NVML_GRAPHICS;
        in.tailStrategy = CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET;
        in.desired = d.get();
        in.lockMode = LOCK_MODE_FLATTEN;
        in.offsetGridHz = 500;
        if (curve_plan_evaluate(in).disposition != CURVE_PLAN_REPRESENTABLE) return 13093;

        int targets[VF_NUM_POINTS]{};
        bool mask[VF_NUM_POINTS]{};
        linux_build_curve_targets(stock, zeroOff, d.get(),
            vf_offset_range_flatten_floor_khz(vfRange), targets, mask);
        if (!gp102->apply_masked_offsets_khz(targets, mask)) return 13094;
        VFCurvePoint after[VF_NUM_POINTS]{};
        int afterOff[VF_NUM_POINTS]{};
        gp102->evaluate_live_curve(after, afterOff);
        if (after[13].freq_kHz != 600000u || after[79].freq_kHz != 600000u) return 13094;
    }

    // 13095-13096: Extreme low-clock undervolt on Blackwell (750 MHz @ ci=10,
    // requiring -2340 MHz on a 3090 MHz stock peak — inside -2500 MHz VF floor).
    {
        auto bw = std::make_unique<SimGpuDriver>();
        bw->spec = make_blackwell_spec(PROFILE_SOURCE_OS_LINUX);
        VFCurvePoint stock[VF_NUM_POINTS]{};
        int zeroOff[VF_NUM_POINTS]{};
        bw->evaluate_live_curve(stock, zeroOff);
        const VfOffsetRange vfRange = vf_offset_range_for_vf_points(
            vf_offset_range_from_probe(true, -1000000, 3000000));

        auto d = std::make_unique<DesiredSettings>();
        d->hasLock = 1;
        d->lockMode = LOCK_MODE_FLATTEN;
        d->lockCi = 10;  // stock 750 MHz
        d->lockMHz = 750;
        VfFloorBinding floor{};
        vf_floor_binding_update(&floor, stock, zeroOff, VF_NUM_POINTS, 0);
        CurvePlanInput in{};
        in.curve = stock;
        in.programmedOffsetsKHz = zeroOff;
        in.floor = floor.floor;
        in.evidence = CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE;
        in.range.minKHz = vfRange.minKHz;
        in.range.maxKHz = vfRange.maxKHz;
        in.range.source = CURVE_PLAN_RANGE_NVML_GRAPHICS;
        in.tailStrategy = CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET;
        in.desired = d.get();
        in.lockMode = LOCK_MODE_FLATTEN;
        in.offsetGridHz = 1000;
        if (curve_plan_evaluate(in).disposition != CURVE_PLAN_REPRESENTABLE) return 13095;

        int targets[VF_NUM_POINTS]{};
        bool mask[VF_NUM_POINTS]{};
        linux_build_curve_targets(stock, zeroOff, d.get(),
            vf_offset_range_flatten_floor_khz(vfRange), targets, mask);
        if (!bw->apply_masked_offsets_khz(targets, mask)) return 13096;
        VFCurvePoint after[VF_NUM_POINTS]{};
        int afterOff[VF_NUM_POINTS]{};
        bw->evaluate_live_curve(after, afterOff);
        if (after[10].freq_kHz != 750000u || after[127].freq_kHz != 750000u) return 13096;
    }
    return 0;
}

static int test_weird_valid_curves_and_grid_edge_cases() {
    // 13097-13098: Stepped S-curve without flatten lock on Pascal:
    // +50 MHz on low voltages (12..25), stock on mid (26..50), -150 MHz on high (51..79).
    {
        auto gp102 = std::make_unique<SimGpuDriver>();
        gp102->spec = make_pascal_gp102_spec(PROFILE_SOURCE_OS_WINDOWS);
        VFCurvePoint stock[VF_NUM_POINTS]{};
        int zeroOff[VF_NUM_POINTS]{};
        gp102->evaluate_live_curve(stock, zeroOff);
        const VfOffsetRange vfRange = vf_offset_range_for_vf_points(
            vf_offset_range_from_probe(true, -200000, 1200000));

        auto d = std::make_unique<DesiredSettings>();
        for (int i = 12; i <= 25; ++i) {
            d->hasCurvePoint[i] = 1;
            d->curvePointMHz[i] = stock[i].freq_kHz / 1000u + 50u;
        }
        for (int i = 51; i < 80; ++i) {
            d->hasCurvePoint[i] = 1;
            d->curvePointMHz[i] = stock[i].freq_kHz / 1000u - 150u;
        }
        VfFloorBinding floor{};
        vf_floor_binding_update(&floor, stock, zeroOff, VF_NUM_POINTS, 0);
        CurvePlanInput in{};
        in.curve = stock;
        in.programmedOffsetsKHz = zeroOff;
        in.floor = floor.floor;
        in.evidence = CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE;
        in.range.minKHz = vfRange.minKHz;
        in.range.maxKHz = vfRange.maxKHz;
        in.range.source = CURVE_PLAN_RANGE_NVML_GRAPHICS;
        in.desired = d.get();
        in.offsetGridHz = 500;
        if (curve_plan_evaluate(in).disposition != CURVE_PLAN_REPRESENTABLE) return 13097;

        int targets[VF_NUM_POINTS]{};
        bool mask[VF_NUM_POINTS]{};
        linux_build_curve_targets(stock, zeroOff, d.get(),
            vf_offset_range_flatten_floor_khz(vfRange), targets, mask);
        if (!gp102->apply_masked_offsets_khz(targets, mask)) return 13098;
        VFCurvePoint after[VF_NUM_POINTS]{};
        int afterOff[VF_NUM_POINTS]{};
        gp102->evaluate_live_curve(after, afterOff);
        if (after[15].freq_kHz != stock[15].freq_kHz + 50000u ||
            after[35].freq_kHz != stock[35].freq_kHz ||
            after[79].freq_kHz != stock[79].freq_kHz - 150000u)
            return 13098;
    }

    // 13099-13100: Negative single-point dip below lower-voltage neighbor on
    // Ampere: monotonic driver clamps live clock up to neighbor; correction loop
    // detects fixed point cleanly instead of spinning indefinitely.
    {
        auto amp = std::make_unique<SimGpuDriver>();
        amp->spec = make_ampere_spec(PROFILE_SOURCE_OS_WINDOWS);
        VFCurvePoint stock[VF_NUM_POINTS]{};
        int zeroOff[VF_NUM_POINTS]{};
        amp->evaluate_live_curve(stock, zeroOff);

        // Pull ci=30 down by -200 MHz so its raw target lies below ci=29.
        amp->set_physical_point_offset_khz(30, -200000);
        VFCurvePoint dipped[VF_NUM_POINTS]{};
        int dippedOff[VF_NUM_POINTS]{};
        amp->evaluate_live_curve(dipped, dippedOff);
        if (dipped[30].freq_kHz != dipped[29].freq_kHz) return 13099;

        int prevErr[VF_NUM_POINTS]{};
        for (int i = 0; i < VF_NUM_POINTS; ++i) prevErr[i] = -1;
        bool lockedTail[VF_NUM_POINTS]{};
        bool explicitMask[VF_NUM_POINTS]{};
        bool origPop[VF_NUM_POINTS]{};
        unsigned char hasPt[VF_NUM_POINTS]{};
        unsigned int ptMHz[VF_NUM_POINTS]{};
        unsigned char fromGpu[VF_NUM_POINTS]{};
        int targetOff[VF_NUM_POINTS]{};
        unsigned int rbFreq[VF_NUM_POINTS]{};
        int rbOff[VF_NUM_POINTS]{};
        for (int i = 0; i < 128; ++i) {
            origPop[i] = true;
            rbFreq[i] = dipped[i].freq_kHz;
            rbOff[i] = dippedOff[i];
        }
        explicitMask[30] = true;
        hasPt[30] = 1;
        ptMHz[30] = (stock[30].freq_kHz - 200000u) / 1000u;

        ApplyCorrectionRequest req{};
        req.pointCount = 128;
        req.lockedTailMask = lockedTail;
        req.explicitCurveMask = explicitMask;
        req.originalCurvePopulated = origPop;
        req.hasCurvePoint = hasPt;
        req.curvePointMHz = ptMHz;
        req.fromGpuOffset = fromGpu;
        req.targetOffsetKHz = targetOff;
        ApplyCorrectionReadback rb{rbFreq, rbOff};
        ApplyCorrectionPointReport reports[VF_NUM_POINTS]{};
        const VfOffsetRange ampVf = vf_offset_range_for_vf_points(
            vf_offset_range_from_probe(true, -500000, 1000000));
        // Pass 0 records initial error; pass 1 sees identical clamped readback -> fixedPoint=true.
        (void)apply_correction_classify_pass(req, rb, ampVf, 0, prevErr, reports);
        const ApplyCorrectionPassStats st1 =
            apply_correction_classify_pass(req, rb, ampVf, 1, prevErr, reports);
        if (!st1.fixedPoint || st1.unconverged == 0) return 13100;
    }

    // 13101-13102: Pascal 500 Hz (0.5 kHz) fractional grid exact preservation
    // vs 250 Hz unaligned refusal.
    {
        auto gp102 = std::make_unique<SimGpuDriver>();
        gp102->spec = make_pascal_gp102_spec(PROFILE_SOURCE_OS_WINDOWS);
        if (!gp102->set_physical_point_offset_hz(0, 37500500LL) ||
            gp102->rawControlDelta[0] != 75001 ||
            !gp102->set_physical_point_offset_hz(67, -1038000500LL) ||
            gp102->rawControlDelta[67] != -2076001)
            return 13101;
        // 250 Hz is not representable on Pascal's 500 Hz grid.
        if (gp102->set_physical_point_offset_hz(1, 37500250LL)) return 13102;
    }

    // 13103-13104: Mixed hand-typed absolute point + selective GPU offset +
    // flatten lock under mid-Apply cold Boost shift (+30 MHz): typed point steers
    // back to its exact MHz while offset-intent points hold +120 MHz.
    {
        auto amp = std::make_unique<SimGpuDriver>();
        amp->spec = make_ampere_spec(PROFILE_SOURCE_OS_WINDOWS);
        VFCurvePoint stock[VF_NUM_POINTS]{};
        int zeroOff[VF_NUM_POINTS]{};
        amp->evaluate_live_curve(stock, zeroOff);
        const VfOffsetRange ampVf = vf_offset_range_for_vf_points(
            vf_offset_range_from_probe(true, -500000, 1000000));

        bool lockedTail[VF_NUM_POINTS]{};
        bool explicitMask[VF_NUM_POINTS]{};
        bool origPop[VF_NUM_POINTS]{};
        unsigned char hasPt[VF_NUM_POINTS]{};
        unsigned int ptMHz[VF_NUM_POINTS]{};
        unsigned char fromGpu[VF_NUM_POINTS]{};
        int initTargetOff[VF_NUM_POINTS]{};
        unsigned int rbFreq[VF_NUM_POINTS]{};
        int rbOff[VF_NUM_POINTS]{};
        // Apply warm: ci=15 typed absolute 600 MHz (stock 555 -> +45 MHz),
        // ci=20..69 selective +120 MHz, ci=70..127 flatten 1800 MHz.
        amp->set_physical_point_offset_khz(15, 45000);
        for (int i = 20; i < 70; ++i) amp->set_physical_point_offset_khz(i, 120000);
        amp->set_physical_point_offset_khz(70, 1800000 - (int)stock[70].freq_kHz);
        for (int i = 71; i < 128; ++i) amp->set_physical_point_offset_khz(i, -2500000);

        // Cold Boost shift (+30 MHz) hits mid-Apply!
        amp->boostShiftKHz = 30000;
        VFCurvePoint coldLive[VF_NUM_POINTS]{};
        int coldOff[VF_NUM_POINTS]{};
        amp->evaluate_live_curve(coldLive, coldOff);
        for (int i = 0; i < 128; ++i) {
            origPop[i] = true;
            rbFreq[i] = coldLive[i].freq_kHz;
            rbOff[i] = coldOff[i];
            if (i >= 70) {
                lockedTail[i] = true;
            } else if (i == 15) {
                explicitMask[i] = true;
                hasPt[i] = 1;
                ptMHz[i] = 600;
                initTargetOff[i] = 45000;
            } else if (i >= 20) {
                hasPt[i] = 1;
                fromGpu[i] = 1;
                ptMHz[i] = (stock[i].freq_kHz + 120000u) / 1000u;
                initTargetOff[i] = 120000;
            }
        }
        ApplyCorrectionRequest req{};
        req.pointCount = 128;
        req.hasLock = true;
        req.lockCi = 70;
        req.lockMHz = 1800;
        req.gpuPolicyViaCurveBatch = true;
        req.lockedTailMask = lockedTail;
        req.explicitCurveMask = explicitMask;
        req.originalCurvePopulated = origPop;
        req.hasCurvePoint = hasPt;
        req.curvePointMHz = ptMHz;
        req.fromGpuOffset = fromGpu;
        req.targetOffsetKHz = initTargetOff;
        ApplyCorrectionReadback rb{rbFreq, rbOff};
        int corrOut[VF_NUM_POINTS]{};
        bool corrMask[VF_NUM_POINTS]{};
        const ApplyCorrectionPlan plan = apply_correction_plan_pass(
            req, rb, ampVf, corrOut, corrMask);
        // Typed absolute ci=15 backs off by -30 MHz (45 -> +15 MHz) to hold 600 MHz,
        // selective offset ci=30 holds +120 MHz, tail ci=127 holds -2500 MHz.
        if (plan.clampedCount != 0 || corrOut[15] != 15000 ||
            corrOut[30] != 120000 || corrOut[127] != -2500000)
            return 13103;
        if (!amp->apply_masked_offsets_khz(corrOut, corrMask)) return 13104;
        amp->evaluate_live_curve(coldLive, coldOff);
        if (coldLive[15].freq_kHz != 600000u || coldLive[70].freq_kHz != 1800000u ||
            coldLive[127].freq_kHz != 1800000u)
            return 13104;
    }

    // 13105-13106: Turing idle floor-run anchor refusal (ci=2 on 300 MHz floor)
    // vs first writable ascending point acceptance (ci=6).
    {
        auto tu = std::make_unique<SimGpuDriver>();
        tu->spec = make_turing_spec(PROFILE_SOURCE_OS_LINUX);
        VFCurvePoint stock[VF_NUM_POINTS]{};
        int zeroOff[VF_NUM_POINTS]{};
        tu->evaluate_live_curve(stock, zeroOff);
        const VfOffsetRange tuVf = vf_offset_range_for_vf_points(
            vf_offset_range_from_probe(true, -500000, 1000000));
        VfFloorBinding floor{};
        vf_floor_binding_update(&floor, stock, zeroOff, VF_NUM_POINTS, 0);

        auto reqFloor = std::make_unique<DesiredSettings>();
        reqFloor->hasLock = 1;
        reqFloor->lockMode = LOCK_MODE_FLATTEN;
        reqFloor->lockCi = 2;  // inside 300 MHz flat idle floor (ci=0..4)
        reqFloor->lockMHz = 400;
        CurvePlanInput in{};
        in.curve = stock;
        in.programmedOffsetsKHz = zeroOff;
        in.floor = floor.floor;
        in.evidence = CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE;
        in.range.minKHz = tuVf.minKHz;
        in.range.maxKHz = tuVf.maxKHz;
        in.range.source = CURVE_PLAN_RANGE_NVML_GRAPHICS;
        in.tailStrategy = CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET;
        in.desired = reqFloor.get();
        in.lockMode = LOCK_MODE_FLATTEN;
        in.offsetGridHz = 1000;
        const CurvePlanResult rFloor = curve_plan_evaluate(in);
        if (rFloor.disposition != CURVE_PLAN_REFUSED ||
            !plan_has_reason(rFloor, CURVE_PLAN_REASON_FLOOR_POINT))
            return 13105;

        reqFloor->lockCi = 6;  // first writable point above 300 MHz floor
        reqFloor->lockMHz = 450;
        const CurvePlanResult rOk = curve_plan_evaluate(in);
        if (rOk.disposition != CURVE_PLAN_REPRESENTABLE) return 13106;
    }

    // 13107-13110: Windows "Copy every curve point exactly" loaded over an
    // active locked curve: untouched captured points stay owned in preview/graph
    // (never released to stock) and bypass hand-typed monotonicity refusal when
    // thermal Boost rounding projects point 22 (1012 MHz) below point 21 (1013 MHz).
    {
        auto exactSlot = std::make_unique<DesiredSettings>();
        exactSlot->applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
        for (int i = 0; i <= 20; ++i) {
            exactSlot->curvePointCaptured[i] = 1;
            exactSlot->curvePointCapturedOffsetHz[i] = 38000000LL;
        }
        exactSlot->curvePointCaptured[21] = 1;
        exactSlot->curvePointCapturedOffsetHz[21] = 13000000LL;
        exactSlot->curvePointCaptured[22] = 1;
        exactSlot->curvePointCapturedOffsetHz[22] = -38000000LL;
        bool typed[VF_NUM_POINTS]{};
        if (!gui_editor_base_owns_captured_point(exactSlot.get(), typed, 21) ||
            !gui_editor_base_owns_captured_point(exactSlot.get(), typed, 22) ||
            gui_editor_base_owns_captured_point(exactSlot.get(), typed, 23) ||
            gui_editor_base_owns_captured_point(nullptr, typed, 21) ||
            gui_editor_base_owns_captured_point(exactSlot.get(), typed, -1))
            return 13107;
        // Hand-typing point 22 transfers it out of untouched-captured status.
        typed[22] = true;
        if (gui_editor_base_owns_captured_point(exactSlot.get(), typed, 22))
            return 13108;
        typed[22] = false;

        // Projected preview over shifted stock (point 21 = 1000+13 = 1013,
        // point 22 = 1050-38 = 1012): stays absoluteMHz=1012 in graph preview
        // rather than falling back to releasedToStock.
        const unsigned int p21 = gui_draft_desired_point_mhz(*exactSlot, 21, nullptr, 1000000LL, 0);
        const unsigned int p22 = gui_draft_desired_point_mhz(*exactSlot, 22, nullptr, 1050000LL, 0);
        if (p21 != 1013u || p22 != 1012u) return 13109;
        GuiGraphPreviewInput pin{};
        pin.ownedByEditor = gui_editor_base_owns_captured_point(exactSlot.get(), typed, 22);
        pin.releasedToStock = !pin.ownedByEditor;
        pin.draftValid = true;
        pin.draftMHz = p22;
        const GuiGraphPreviewPoint preview = gui_graph_preview_point(pin);
        if (preview.offsetFromStockBase || preview.absoluteMHz != 1012u)
            return 13110;
    }
    return 0;
}

}  // namespace

int run_gpu_generation_sim_edge_tests() {
    if (int rc = test_cold_gpu_boost_drift_and_ceiling_guards()) return rc;
    if (int rc = test_ultra_deep_low_clock_undervolts()) return rc;
    if (int rc = test_weird_valid_curves_and_grid_edge_cases()) return rc;
    return 0;
}
