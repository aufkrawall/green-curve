// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Multi-generation hardware-free GPU simulation regression suite (13000-13083):
// VF negative floor policy, Windows --probe path resolution, Pascal GP102
// (Windows + Linux), Ampere, Blackwell, and positive-only board refusal.

#include "gpu_generation_sim_harness.h"
#include "apply_clock_ceiling_policy.h"
#include "apply_correction_policy.h"
#include "cli_output_path_policy.h"
#include "gui_draft_desired_policy.h"
#include "linux_curve_targets.h"
#include "linux_tui_layout.h"
#include "profile_capture_proposals.h"
#include "profile_slot_runtime.h"
#include "profile_summary.h"

#include <memory>
#include <string>

namespace {
using namespace gpu_gen_sim;

static int test_vf_offset_range_policy_and_probe_path() {
    // 13000-13008: VF negative floor widening vs scalar P0 graphics bounds.
    const VfOffsetRange pascalScalar = vf_offset_range_from_probe(true, -200000, 1200000);
    const VfOffsetRange pascalVf = vf_offset_range_for_vf_points(pascalScalar);
    if (!pascalVf.known || pascalVf.minKHz != -VF_OFFSET_RANGE_VF_NEG_FLOOR_KHZ ||
        pascalVf.maxKHz != 1200000 || !vf_offset_range_was_neg_widened(pascalScalar, pascalVf) ||
        !vf_offset_range_supports_flatten(pascalVf) ||
        !vf_offset_range_permits_khz(pascalVf, -1038000) ||
        vf_offset_range_permits_khz(pascalVf, 1201000))
        return 13000;

    const VfOffsetRange bwScalar = vf_offset_range_from_probe(true, -1000000, 3000000);
    const VfOffsetRange bwVf = vf_offset_range_for_vf_points(bwScalar);
    if (bwVf.minKHz != -VF_OFFSET_RANGE_VF_NEG_FLOOR_KHZ || bwVf.maxKHz != 3000000 ||
        !vf_offset_range_was_neg_widened(bwScalar, bwVf) ||
        !vf_offset_range_permits_khz(bwVf, -2045000))
        return 13001;

    const VfOffsetRange wideScalar = vf_offset_range_from_probe(true, -3000000, 2000000);
    const VfOffsetRange wideVf = vf_offset_range_for_vf_points(wideScalar);
    if (wideVf.minKHz != -3000000 || wideVf.maxKHz != 2000000 ||
        vf_offset_range_was_neg_widened(wideScalar, wideVf))
        return 13002;

    const VfOffsetRange posOnly = vf_offset_range_from_probe(true, 0, 300000);
    const VfOffsetRange posVf = vf_offset_range_for_vf_points(posOnly);
    if (posVf.minKHz != 0 || posVf.maxKHz != 300000 ||
        vf_offset_range_was_neg_widened(posOnly, posVf) ||
        vf_offset_range_supports_flatten(posVf))
        return 13003;

    const VfOffsetRange fb = vf_offset_range_fallback();
    const VfOffsetRange fbVf = vf_offset_range_for_vf_points(fb);
    if (fbVf.known || fbVf.minKHz != -VF_OFFSET_RANGE_FALLBACK_LIMIT_KHZ ||
        vf_offset_range_was_neg_widened(fb, fbVf))
        return 13004;

    long long mnHz = 0, mxHz = 0;
    vf_offset_range_for_vf_points_hz(true, -200000000LL, 1200000000LL, &mnHz, &mxHz);
    if (mnHz != -(long long)VF_OFFSET_RANGE_VF_NEG_FLOOR_KHZ * 1000LL || mxHz != 1200000000LL)
        return 13005;
    vf_offset_range_for_vf_points_hz(true, -3000000000LL, 2000000000LL, &mnHz, &mxHz);
    if (mnHz != -3000000000LL || mxHz != 2000000000LL) return 13006;
    vf_offset_range_for_vf_points_hz(true, 0LL, 300000000LL, &mnHz, &mxHz);
    if (mnHz != 0LL || mxHz != 300000000LL) return 13007;
    vf_offset_range_for_vf_points_hz(false, -200000000LL, 1200000000LL, &mnHz, &mxHz);
    if (mnHz != -1000000000LL || mxHz != 1000000000LL) return 13008;

    // 13009-13012: CLI --probe output path resolution stays inside userDataDir.
    char out[260]{};
    if (!cli_select_probe_output_path(false, "", "C:\\Profile\\AppData\\Local\\Green Curve",
            2026, 11, 23, 10, 30, 5, out, sizeof(out)) ||
        strcmp(out, "C:\\Profile\\AppData\\Local\\Green Curve\\greencurve_probe_20261123_103005.json") != 0)
        return 13009;
    if (!cli_format_default_probe_path("C:\\Profile\\Local\\", 2026, 11, 23, 9, 8, 7, out, sizeof(out)) ||
        strcmp(out, "C:\\Profile\\Local\\greencurve_probe_20261123_090807.json") != 0)
        return 13010;
    if (!cli_select_probe_output_path(true, "custom_probe.json", "C:\\Profile\\Local",
            2026, 11, 23, 10, 30, 5, out, sizeof(out)) ||
        strcmp(out, "custom_probe.json") != 0)
        return 13011;
    char tiny[8]{};
    if (cli_select_probe_output_path(true, "", "C:\\Profile", 2026, 11, 23, 1, 2, 3, out, sizeof(out)) ||
        cli_format_default_probe_path("C:\\Profile", 2026, 11, 23, 1, 2, 3, tiny, sizeof(tiny)))
        return 13012;
    return 0;
}

static int test_pascal_gp102_simulation(unsigned int osFamily, int errBase) {
    auto driver = std::make_unique<SimGpuDriver>();
    driver->spec = make_pascal_gp102_spec(osFamily);
    // Cool-card +64 MHz Boost 3.0 thermal shift on the upper curve (>= 740 mV),
    // matching real GP102 dumps: +38 MHz on points 0..20, +13 MHz -> 1000 MHz at
    // point 21 (662.5 mV), down to -1038 MHz on points 67..79.
    driver->boostShiftKHz = 64000;
    driver->boostShiftMinVoltUv = 740000u;
    driver->scalarMemOffsetMHz = -2000;
    for (int i = 0; i <= 20; ++i) driver->set_physical_point_offset_khz(i, 38000);
    for (int i = 21; i < 80; ++i) {
        const int pointBoost = driver->spec.baseVoltUv[i] >= driver->boostShiftMinVoltUv
            ? driver->boostShiftKHz : 0;
        const int baseKHz = (int)driver->spec.stockBaseKHz[i] + pointBoost;
        driver->set_physical_point_offset_khz(i, 1000000 - baseKHz);
    }
    if (driver->rawControlDelta[67] != -2076000) return errBase + 0;

    auto obs = std::make_unique<HardwareCaptureObservation>();
    const gc_u32 domains = PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET |
        PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER | PROFILE_DOMAIN_LOCK;
    if (!sim_collect_capture(*driver, domains, *obs)) return errBase + 1;
    if (obs->pointCount != 80 || obs->vfLimits.gridHz != 500 ||
        obs->vfLimits.minimumHz != -(gc_i64)VF_OFFSET_RANGE_VF_NEG_FLOOR_KHZ * 1000LL ||
        obs->vfLimits.maximumHz != 1200000000LL ||
        obs->points[0].offsetHz != 38000000LL ||
        obs->points[67].offsetHz != -1038000000LL)
        return errBase + 2;
    char capSummary[512]{};
    hardware_capture_summary(*obs, capSummary, sizeof(capSummary));
    if (!strstr(capSummary, "reported graphics bounds, VF negative floor"))
        return errBase + 3;

    auto proposals = std::make_unique<ProfileCaptureProposals>();
    char why[256]{};
    if (!profile_capture_prepare_proposals(*obs, {}, *proposals, why, sizeof(why)))
        return errBase + 4;
    if (proposals->exactControls.disposition == PROFILE_CONTROL_REFUSED ||
        !proposals->flatten.selectable() ||
        proposals->flatten.anchorVoltageUv != 662500u ||
        proposals->flatten.frequencyHz != 1000000000LL ||
        proposals->flatten.controls.disposition == PROFILE_CONTROL_REFUSED ||
        proposals->flatten.controls.curve.issueCount != 0)
        return errBase + 5;

    const ProfileCaptureSummary sum = profile_capture_summary(*obs, proposals.get());
    if (sum.curve.text != "Flat at 1000 MHz from 662.5 mV (+38 MHz on 21 lower points)")
        return errBase + 6;
    auto obsNonUniform = std::make_unique<HardwareCaptureObservation>(*obs);
    obsNonUniform->points[0].offsetHz = 50000000LL;
    const ProfileCaptureSummary sumMixed = profile_capture_summary(*obsNonUniform, proposals.get());
    if (sumMixed.curve.text != "Flat at 1000 MHz from 662.5 mV (21 lower points changed)")
        return errBase + 7;

    ProfileDocument doc{};
    if (!profile_capture_select_proposal(*proposals, PROFILE_CAPTURE_FLATTEN, doc, why, sizeof(why)))
        return errBase + 8;
    driver->reset_to_stock();
    driver->boostShiftKHz = 0;
    VFCurvePoint stockCurve[VF_NUM_POINTS]{};
    int stockOffsets[VF_NUM_POINTS]{};
    driver->evaluate_live_curve(stockCurve, stockOffsets);

    ProfileDestinationTopology liveTopo{};
    liveTopo.available = true;
    liveTopo.complete = true;
    liveTopo.gpuGeneration = 1;
    liveTopo.revision = 1;
    liveTopo.count = 80;
    for (int i = 0; i < 80; ++i) {
        liveTopo.points[i].index = i;
        liveTopo.points[i].voltageUv = stockCurve[i].volt_uV;
        liveTopo.points[i].graphics = true;
        liveTopo.points[i].writable = true;
    }

    auto resolved = std::make_unique<DesiredSettings>();
    if (!profile_slot_document_runtime(doc, &liveTopo, *resolved, why, sizeof(why)))
        return errBase + 9;
    if (!resolved->hasLock || resolved->lockMode != LOCK_MODE_FLATTEN ||
        resolved->lockCi != 21 || resolved->lockMHz != 1000 ||
        !resolved->curvePointCaptured[0] || resolved->curvePointCapturedOffsetHz[0] != 38000000LL ||
        !resolved->curvePointCaptured[20] || resolved->curvePointCapturedOffsetHz[20] != 38000000LL ||
        resolved->curvePointCaptured[21] || resolved->curvePointCaptured[22])
        return errBase + 10;

    bool fromLock = false, fromProj = false;
    if (gui_draft_desired_point_mhz(*resolved, 0, &fromLock, stockCurve[0].freq_kHz, 0, &fromProj) != 253u ||
        fromLock || !fromProj)
        return errBase + 11;
    if (gui_draft_desired_point_mhz(*resolved, 20, &fromLock, stockCurve[20].freq_kHz, 0, &fromProj) != 975u ||
        fromLock || !fromProj)
        return errBase + 12;
    if (gui_draft_desired_point_mhz(*resolved, 21, &fromLock, stockCurve[21].freq_kHz, 0, &fromProj) != 1000u ||
        !fromLock)
        return errBase + 13;
    if (gui_draft_desired_point_mhz(*resolved, 0, &fromLock, 0, 0, &fromProj) != 0u)
        return errBase + 14;

    const VfOffsetRange pascalVfRange = vf_offset_range_for_vf_points(
        vf_offset_range_from_probe(true, -200000, 1200000));
    auto tuiSvc = std::make_unique<ServiceResponse>();
    tuiSvc->snapshot.loaded = 1;
    tuiSvc->snapshot.numPopulated = 80;
    tuiSvc->snapshot.curveOffsetRangeKnown = 1;
    tuiSvc->snapshot.curveOffsetMinkHz = pascalVfRange.minKHz;
    tuiSvc->snapshot.curveOffsetMaxkHz = pascalVfRange.maxKHz;
    memcpy(tuiSvc->snapshot.curve, stockCurve, sizeof(stockCurve));
    TuiViewModel tuiVm{};
    tuiVm.service = tuiSvc.get();
    tuiVm.desired = resolved.get();
    const TuiPointValues tui0 = tui_point_values(tuiVm, 0);
    const TuiPointValues tui21 = tui_point_values(tuiVm, 21);
    const TuiPointValues tui79 = tui_point_values(tuiVm, 79);
    if (tui0.targetMHz != 253 || tui21.rule != TUI_POINT_FLATTEN_KNEE || tui21.targetMHz != 1000 ||
        tui79.rule != TUI_POINT_FLATTEN_TAIL || tui79.deltaMHz != -2500)
        return errBase + 15;

    gc_i64 stockFreqHz[VF_NUM_POINTS]{};
    gc_i64 stockOffsetsHz[VF_NUM_POINTS]{};
    for (int i = 0; i < 80; ++i) {
        stockFreqHz[i] = (gc_i64)stockCurve[i].freq_kHz * 1000LL;
        stockOffsetsHz[i] = (gc_i64)stockOffsets[i] * 1000LL;
    }
    VfFloorBinding floor{};
    vf_floor_binding_update(&floor, stockCurve, stockOffsets, VF_NUM_POINTS, 0);
    CurvePlanInput planIn{};
    planIn.curve = stockCurve;
    planIn.programmedOffsetsKHz = stockOffsets;
    planIn.frequencyHz = stockFreqHz;
    planIn.programmedOffsetsHz = stockOffsetsHz;
    planIn.floor = floor.floor;
    planIn.evidence = CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE;
    planIn.range.minKHz = pascalVfRange.minKHz;
    planIn.range.maxKHz = pascalVfRange.maxKHz;
    planIn.range.source = CURVE_PLAN_RANGE_NVML_GRAPHICS;
    planIn.tailStrategy = CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET;
    planIn.desired = resolved.get();
    planIn.lockMode = LOCK_MODE_FLATTEN;
    planIn.offsetGridHz = 500;
    const CurvePlanResult plan = curve_plan_evaluate(planIn);
    if (plan.disposition != CURVE_PLAN_REPRESENTABLE || plan.issueCount != 0)
        return errBase + 16;

    int targetsKHz[VF_NUM_POINTS]{};
    bool mask[VF_NUM_POINTS]{};
    const LinuxCurveTargetBuildResult built = linux_build_curve_targets(
        stockCurve, stockOffsets, resolved.get(),
        vf_offset_range_flatten_floor_khz(pascalVfRange), targetsKHz, mask);
    if (built.failed || !built.lockTail || targetsKHz[0] != 38000 || targetsKHz[21] != 13000 ||
        targetsKHz[79] != -2500000)
        return errBase + 17;
    if (!driver->apply_masked_offsets_khz(targetsKHz, mask)) return errBase + 18;
    if (driver->rawControlDelta[0] != 76000 || driver->rawControlDelta[21] != 26000 ||
        driver->rawControlDelta[79] != -5000000)
        return errBase + 19;
    VFCurvePoint afterCurve[VF_NUM_POINTS]{};
    int afterOffsets[VF_NUM_POINTS]{};
    driver->evaluate_live_curve(afterCurve, afterOffsets);
    if (afterCurve[0].freq_kHz != 253000u || afterCurve[20].freq_kHz != 975000u ||
        afterCurve[21].freq_kHz != 1000000u || afterCurve[79].freq_kHz != 1000000u)
        return errBase + 20;

    bool lockedTailMask[VF_NUM_POINTS]{};
    bool explicitMask[VF_NUM_POINTS]{};
    bool origPopulated[VF_NUM_POINTS]{};
    unsigned char hasCurvePt[VF_NUM_POINTS]{};
    unsigned int curvePtMHz[VF_NUM_POINTS]{};
    unsigned char fromGpuOff[VF_NUM_POINTS]{};
    unsigned int rbFreqKHz[VF_NUM_POINTS]{};
    int rbProgKHz[VF_NUM_POINTS]{};
    for (int i = 0; i < 80; ++i) {
        origPopulated[i] = true;
        rbFreqKHz[i] = afterCurve[i].freq_kHz;
        rbProgKHz[i] = afterOffsets[i];
        if (i >= 21) {
            lockedTailMask[i] = true;
        } else {
            hasCurvePt[i] = 1;
            fromGpuOff[i] = 1;
            curvePtMHz[i] = (stockCurve[i].freq_kHz + 38000u) / 1000u;
        }
    }
    ApplyCorrectionRequest corrReq{};
    corrReq.pointCount = 80;
    corrReq.hasLock = true;
    corrReq.lockCi = 21;
    corrReq.lockMHz = 1000;
    corrReq.lockedTailMask = lockedTailMask;
    corrReq.explicitCurveMask = explicitMask;
    corrReq.originalCurvePopulated = origPopulated;
    corrReq.hasCurvePoint = hasCurvePt;
    corrReq.curvePointMHz = curvePtMHz;
    corrReq.fromGpuOffset = fromGpuOff;
    corrReq.targetOffsetKHz = targetsKHz;
    ApplyCorrectionReadback corrRb{};
    corrRb.freqKHz = rbFreqKHz;
    corrRb.programmedOffsetKHz = rbProgKHz;
    int corrOut[VF_NUM_POINTS]{};
    bool corrMask[VF_NUM_POINTS]{};
    const ApplyCorrectionPlan corrPlan = apply_correction_plan_pass(
        corrReq, corrRb, pascalVfRange, corrOut, corrMask);
    if (corrPlan.clampedCount != 0 || corrOut[0] != 38000 || corrOut[79] != -2500000)
        return errBase + 21;

    if (!driver->set_scalar_gpu_offset_mhz(150) || driver->set_scalar_gpu_offset_mhz(-250))
        return errBase + 22;

    if (apply_clock_ceiling_transition_must_refuse(
            true, APPLY_CEILING_ARM_UNSUPPORTED, APPLY_CEILING_REASON_REQUESTED_FLATTEN) ||
        apply_clock_ceiling_transition_must_refuse(
            true, APPLY_CEILING_ARM_UNSUPPORTED, APPLY_CEILING_REASON_RESET_DROPS_CAP) ||
        !apply_clock_ceiling_transition_must_refuse(
            true, APPLY_CEILING_ARM_UNSUPPORTED, APPLY_CEILING_REASON_REQUESTED_LOCK) ||
        !apply_clock_ceiling_transition_must_refuse(
            true, APPLY_CEILING_ARM_REFUSED, APPLY_CEILING_REASON_REQUESTED_FLATTEN))
        return errBase + 23;
    return 0;
}

static int test_ampere_and_blackwell_simulations() {
    // 13045-13047: Ampere narrow scalar (-500..+1000 MHz) deep flatten + selective
    // GPU offset + NVML hard pin capture.
    {
        auto amp = std::make_unique<SimGpuDriver>();
        amp->spec = make_ampere_spec(PROFILE_SOURCE_OS_LINUX);
        VFCurvePoint stock[VF_NUM_POINTS]{};
        int offsets[VF_NUM_POINTS]{};
        amp->evaluate_live_curve(stock, offsets);

        const VfOffsetRange ampVf = vf_offset_range_for_vf_points(
            vf_offset_range_from_probe(true, -500000, 1000000));
        auto d = std::make_unique<DesiredSettings>();
        d->hasGpuOffset = 1;
        d->gpuOffsetMHz = 120;
        d->gpuOffsetExcludeLowCount = 20;
        d->hasLock = 1;
        d->lockMode = LOCK_MODE_FLATTEN;
        d->lockCi = 60;  // stock 1230 MHz; ci=59+120=1335 <= 1400; ci=127 needs -750 MHz (< -500 MHz scalar!)
        d->lockMHz = 1400;
        int targets[VF_NUM_POINTS]{};
        bool mask[VF_NUM_POINTS]{};
        const LinuxCurveTargetBuildResult built = linux_build_curve_targets(
            stock, offsets, d.get(), vf_offset_range_flatten_floor_khz(ampVf), targets, mask);
        if (built.failed || targets[61] != -2500000 || !amp->apply_masked_offsets_khz(targets, mask))
            return 13045;
        VFCurvePoint after[VF_NUM_POINTS]{};
        amp->evaluate_live_curve(after, offsets);
        if (after[60].freq_kHz != 1400000u || after[127].freq_kHz != 1400000u ||
            offsets[10] != 0 || offsets[30] != 120000)
            return 13046;

        amp->pinActive = true;
        amp->pinMinMHz = 1400;
        amp->pinMaxMHz = 1400;
        auto obs = std::make_unique<HardwareCaptureObservation>();
        auto props = std::make_unique<ProfileCaptureProposals>();
        char why[256]{};
        if (!sim_collect_capture(*amp, PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK, *obs) ||
            !profile_capture_prepare_proposals(*obs, {}, *props, why, sizeof(why)) ||
            !props->pin.selectable() || props->pin.frequencyHz != 1400000000LL)
            return 13047;
    }

    // 13051-13054: Blackwell deep external -2045 MHz flatten capture + positive-only refusal.
    {
        auto bw = std::make_unique<SimGpuDriver>();
        bw->spec = make_blackwell_spec(PROFILE_SOURCE_OS_WINDOWS);
        for (int i = 0; i < 80; ++i) bw->set_physical_point_offset_khz(i, 0);
        bw->set_physical_point_offset_khz(80, 52000);
        for (int i = 81; i < 128; ++i) bw->set_physical_point_offset_khz(i, -2045000);

        auto obs = std::make_unique<HardwareCaptureObservation>();
        auto props = std::make_unique<ProfileCaptureProposals>();
        char why[256]{};
        if (!sim_collect_capture(*bw, PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK, *obs))
            return 13051;
        if (obs->vfLimits.minimumHz != -2500000000LL || obs->vfLimits.maximumHz != 3000000000LL ||
            obs->points[120].offsetHz != -2045000000LL)
            return 13052;
        if (!profile_capture_prepare_proposals(*obs, {}, *props, why, sizeof(why)) ||
            props->exactControls.disposition == PROFILE_CONTROL_REFUSED ||
            !props->flatten.selectable() ||
            props->flatten.controls.disposition == PROFILE_CONTROL_REFUSED)
            return 13053;

        bw->spec.scalarGpuMinMHz = 0;
        bw->spec.scalarGpuMaxMHz = 300;
        bw->reset_to_stock();
        VFCurvePoint posCurve[VF_NUM_POINTS]{};
        int posOffsets[VF_NUM_POINTS]{};
        bw->evaluate_live_curve(posCurve, posOffsets);
        const VfOffsetRange posRange = vf_offset_range_for_vf_points(
            vf_offset_range_from_probe(true, 0, 300000));
        VfFloorBinding floor{};
        vf_floor_binding_update(&floor, posCurve, posOffsets, VF_NUM_POINTS, 0);
        auto flatReq = std::make_unique<DesiredSettings>();
        flatReq->hasLock = 1;
        flatReq->lockMode = LOCK_MODE_FLATTEN;
        flatReq->lockCi = 80;
        flatReq->lockMHz = 2400;
        CurvePlanInput in{};
        in.curve = posCurve;
        in.programmedOffsetsKHz = posOffsets;
        in.floor = floor.floor;
        in.evidence = CURVE_PLAN_EVIDENCE_UNSHAPED_LIVE;
        in.range.minKHz = posRange.minKHz;
        in.range.maxKHz = posRange.maxKHz;
        in.range.source = CURVE_PLAN_RANGE_NVML_GRAPHICS;
        in.tailStrategy = CURVE_PLAN_TAIL_UNIFORM_MIN_OFFSET;
        in.desired = flatReq.get();
        in.lockMode = LOCK_MODE_FLATTEN;
        in.offsetGridHz = 1000;
        const CurvePlanResult r = curve_plan_evaluate(in);
        if (r.disposition != CURVE_PLAN_REFUSED ||
            !plan_has_reason(r, CURVE_PLAN_REASON_FLATTEN_UNSUPPORTED))
            return 13054;
    }
    return 0;
}

}  // namespace

int run_gpu_generation_sim_tests() {
    if (int rc = test_vf_offset_range_policy_and_probe_path()) return rc;
    if (int rc = test_pascal_gp102_simulation(PROFILE_SOURCE_OS_WINDOWS, 13015)) return rc;
    if (int rc = test_pascal_gp102_simulation(PROFILE_SOURCE_OS_LINUX, 13060)) return rc;
    if (int rc = test_ampere_and_blackwell_simulations()) return rc;
    return 0;
}
