// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "windows_curve_authoring_targets.h"
#include "desired_graphics_surface.h"
#include "windows_graphics_offset_route.h"
#include "vf_control_write_policy.h"
#include "scalar_curve_submission_policy.h"
#include "graphics_offset_control.h"
#include "windows_graphics_handoff_review.h"
#include "authoring_modeled_transition_review.h"

static bool apply_handoff_curve_snapshot(const ScalarCurveSubmissionReview& review) {
    if (!review.outgoingReleased || !review.writeAttempted) return true;
    bool offsetsOk = false;
    return read_live_curve_snapshot_settled(1, 0, &offsetsOk) && offsetsOk;
}

template <class Guard>
static bool apply_authoring_scalar_review_refusal(Guard& guard, char* why, size_t size,
    bool* writeAttempted) {
    const bool retained = guard.armed;
    if (retained) guard.retain("the reviewed scalar changed before submission; VF controls remain untouched");
    if (!guard.writeAttempted && writeAttempted) *writeAttempted = false;
    set_message(why, size, "Reviewed graphics control changed; capture and review again."
        " No graphics scalar or VF curve write was attempted.%s",
        retained ? " The installed transition ceiling remains until recovery." : "");
    return false;
}

static CurvePlanEvidence apply_authoring_curve_evidence(const VfFloorTopology& floor,
    bool resetBaseline, bool preWriteScalarKnownZero, bool gpuApplied,
    bool wroteIncomingScalar, const VFCurvePoint* curve, const int* offsetsKHz,
    const gc_i64* offsetsHz) {
    if (resetBaseline && !wroteIncomingScalar) return CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE;
    return curve_plan_classify_pre_write_evidence(curve, offsetsKHz,
        floor, preWriteScalarKnownZero && !gpuApplied, offsetsHz);
}

// Included after the Windows planner and exact writer. Frozen originals are
// used before memory writes; corrections use fresh paired samples but retain
// the transaction's original unowned controls.
static bool apply_build_authoring_curve_targets(const DesiredSettings& desired,
    LockMode mode, bool composeGraphics, bool preserveUnowned,
    const VFCurvePoint* curve, const int* offsetsKHz, const gc_i64* frequencyHz,
    const gc_i64* offsetsHz, CurvePlanEvidence evidence,
    const CurveAuthoringTargets* original, CurveAuthoringTargets& output,
    char* why, size_t whySize) {
    int componentKHz[VF_NUM_POINTS]{}; gc_i64 componentHz[VF_NUM_POINTS]{};
    auto in = apply_curve_plan_input(&desired, mode, curve, offsetsKHz, evidence,
        apply_lock_pretail_provenance_mode(composeGraphics, desired.resetOcBeforeApply != 0),
        desired.resetOcBeforeApply != 0, desired.gpuOffsetExcludeLowCount, componentKHz, componentHz);
    in.frequencyHz = frequencyHz; in.programmedOffsetsHz = offsetsHz;
    const auto plan = curve_plan_evaluate(in);
    curve_plan_log(plan, "windows-authoring-targets", apply_curve_plan_log_line);
    if (plan.disposition == CURVE_PLAN_REFUSED || plan.disposition == CURVE_PLAN_UNAVAILABLE) {
        curve_plan_format_message(plan, false, why, whySize); return false;
    }
    CurveAuthoringTargetIssue issue;
    CurveAuthoringTargets next;
    if (!windows_build_authoring_curve_targets(in, g_app.vfBackend, g_app.vfMask,
            composeGraphics, preserveUnowned, original, next, issue)) {
        set_message(why, whySize, "Exact curve target unavailable at point %d", issue.ci);
        debug_log("curve-authoring: target compile refused status=%d ci=%d requestedHz=%lld\n",
            (int)issue.status, issue.ci, issue.requestedHz);
        return false;
    }
    if (original && !windows_authoring_curve_shape_matches(*original, next)) {
        set_message(why, whySize, "The curve write set changed during correction");
        debug_log("curve-authoring: correction shape changed; refusing write\n"); return false;
    }
    output = next;
    return true;
}

static bool apply_verify_authoring_curve_targets(const CurveAuthoringTargets& targets,
    LockMode mode, int lockCi, char* why, size_t whySize) {
    return windows_verify_authoring_curve_targets(g_app.curve, g_app.curveFrequencyHz,
        g_app.freqOffsetsHz, g_app.vfFrequenciesHzValid, g_app.vfOffsetsHzValid,
        targets, mode, lockCi, why, whySize);
}

static bool apply_authoring_precheck_before_writes(const DesiredSettings* d, bool composeGraphics,
    char* why, size_t whySize, ScalarCurveSubmissionReview* review = nullptr,
    const AuthoringModeledPrecheckContext* modeled = nullptr,
    bool* preWriteScalarKnownZeroOut = nullptr) {
    if (preWriteScalarKnownZeroOut) *preWriteScalarKnownZeroOut = false;
    ScalarCurveSubmissionReview local{};
    if (!review) review = &local;
    *review = {};
    if (!apply_graphics_handoff_precheck(d, *review, why, whySize)) return false;
    if (!curve_authoring_needs_exact_targets(d)) return true;
    bool offsetsOk = false;
    if (!read_live_curve_snapshot_settled(1, 0, &offsetsOk) || !offsetsOk ||
        !g_app.vfFrequenciesHzValid || !g_app.vfOffsetsHzValid) {
        set_message(why, whySize, "Fresh exact curve samples are unavailable. Nothing was changed."); return false;
    }
    const gc_u32 incomingSurface = desired_named_graphics_surface(*d);
    const auto scalar = incomingSurface
        ? graphics_offset_control_plan(*d, g_nvml_api, g_app.nvmlDevice)
        : GraphicsOffsetControlPlan{};
    const HardwareCaptureComposition composition = modeled ? modeled->composition : CAPTURE_COMPOSITION_UNKNOWN;
    auto nvml_present_scalars_zero = [&]() {
        if (review->outgoingSurface && review->outgoingBeforeHz != 0) return false;
        if (incomingSurface && (!graphics_offset_control_ready(scalar) || scalar.before.value != 0))
            return false;
        if (g_nvml_api.getClockOffsets && incomingSurface != CLOCK_OFFSET_SURFACE_P0) {
            DesiredSettings q{}; q.hasGpuOffset = 1; q.authoring.graphicsPrecise = 1;
            q.authoring.graphicsSurface = DESIRED_GRAPHICS_P0;
            const auto p0 = graphics_offset_control_plan(q, g_nvml_api, g_app.nvmlDevice);
            if (!graphics_offset_control_ready(p0) || p0.before.value != 0) return false;
        }
        if (g_nvml_api.getGpcClkVfOffset && incomingSurface != CLOCK_OFFSET_SURFACE_GLOBAL) {
            DesiredSettings q{}; q.hasGpuOffset = 1; q.authoring.graphicsPrecise = 1;
            q.authoring.graphicsSurface = DESIRED_GRAPHICS_GLOBAL;
            const auto gl = graphics_offset_control_plan(q, g_nvml_api, g_app.nvmlDevice);
            if (!graphics_offset_control_ready(gl) || gl.before.value != 0) return false;
        }
        return true;
    };
    const bool scalarKnownZero = composition == CAPTURE_COMPOSITION_INDEPENDENT && nvml_present_scalars_zero();
    if (preWriteScalarKnownZeroOut) *preWriteScalarKnownZeroOut = scalarKnownZero;
    int componentKHz[VF_NUM_POINTS]{}; gc_i64 componentHz[VF_NUM_POINTS]{};
    auto in = apply_curve_plan_input(d, d->hasLock ? d->lockMode : LOCK_MODE_NONE,
        g_app.curve, g_app.freqOffsets, CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE,
        apply_lock_pretail_provenance_mode(composeGraphics, false), d->resetOcBeforeApply != 0,
        d->gpuOffsetExcludeLowCount, componentKHz, componentHz);
    in.frequencyHz = g_app.curveFrequencyHz; in.programmedOffsetsHz = g_app.freqOffsetsHz;
    in.evidence = curve_plan_classify_pre_write_evidence(in.curve, in.programmedOffsetsKHz,
        in.floor, scalarKnownZero, in.programmedOffsetsHz);
    const auto plan = curve_plan_evaluate(in);
    curve_plan_log(plan, "windows-authoring-pre-write", apply_curve_plan_log_line);
    if (plan.disposition == CURVE_PLAN_REFUSED || plan.disposition == CURVE_PLAN_UNAVAILABLE) {
        curve_plan_format_message(plan, true, why, whySize); return false;
    }
    CurveAuthoringTargets preview; CurveAuthoringTargetIssue issue;
    const bool preserve = d->hasMemOffset && !d->resetOcBeforeApply &&
        apply_plan_mem_offset_write(g_app, (int)(desired_memory_hz(*d) / 1000));
    if (!windows_build_authoring_curve_targets(in, g_app.vfBackend, g_app.vfMask, composeGraphics,
            preserve, nullptr, preview, issue, d->resetOcBeforeApply != 0)) {
        set_message(why, whySize, "Exact curve preflight could not resolve point %d. Review its voltage, writable state and exact target. Nothing was changed.", issue.ci);
        debug_log("curve-authoring: pre-write target refusal status=%d ci=%d targetHz=%lld\n",
            (int)issue.status, issue.ci, issue.requestedHz); return false;
    }
    const bool clampCap = modeled && modeled->clampAvailabilityExplicit
        ? modeled->clampAvailable
        : (g_nvml_api.setGpuLockedClocks && g_nvml_api.resetGpuLockedClocks);
    const bool pinAuthorized = modeled && modeled->pinAuthorityExplicit
        ? modeled->pinMutationAuthorized
        : authoring_pin_mutation_authorized(*d);
    const bool hasLock = d->hasLock && d->lockMode != LOCK_MODE_NONE;
    const gc_i64 beforeHz = incomingSurface ? (long long)scalar.before.value * 1000000 : 0;
    const gc_i64 afterHz = incomingSurface ? desired_graphics_hz(*d) : 0;
    const bool hasHandoff = review->outgoingSurface && review->outgoingBeforeHz != 0;
    const bool multiStep = modeled ? modeled->multiStepCurve : false;
    const bool resetsToStock = d->resetOcBeforeApply != 0;
    const bool scalarRaisesBeforeCurve = incomingSurface && preview.pointCount > 0 &&
        (resetsToStock ? afterHz > 0 : (!graphics_offset_control_ready(scalar) || afterHz > beforeHz));
    if (incomingSurface) {
        const auto transition = scalar_curve_submission_plan(*d, graphics_offset_control_ready(scalar),
            beforeHz, in.programmedOffsetsHz, true, clampCap, hasHandoff);
        debug_log("scalar-curve: preflight status=%d loweredCaptured=%d baselineDependent=%d\n",
            (int)transition.status, transition.loweredCaptured, transition.baselineDependent ? 1 : 0);
        const bool deferToModeled = pinAuthorized &&
            (transition.status == SCALAR_CURVE_PIN_EVIDENCE_REQUIRED ||
             (!hasLock && transition.status == SCALAR_CURVE_GUARD_UNAVAILABLE));
        if (!scalar_curve_submission_allowed(transition) && !deferToModeled) {
            set_message(why, whySize, "Curve transition: %s. Nothing was changed.", scalar_curve_submission_reason(transition.status));
            return false;
        }
        if (review->beforeValid && beforeHz != review->beforeHz) {
            set_message(why, whySize, "The incoming graphics control changed during review. Nothing was changed.");
            return false;
        }
        review->installedGuardRequired |= transition.status == SCALAR_CURVE_INSTALL_GUARD;
        review->beforeValid = graphics_offset_control_ready(scalar);
        review->beforeHz = beforeHz;
    }
    if (scalarRaisesBeforeCurve || multiStep) {
        AuthoringModeledTransitionInput mIn{};
        mIn.baseline = &in;
        mIn.targets = &preview;
        mIn.composition = composition;
        mIn.pinMutationAuthorized = pinAuthorized;
        mIn.clampAvailable = clampCap;
        mIn.scalarBeforeCurve = scalarRaisesBeforeCurve;
        mIn.multiStepCurve = multiStep;
        mIn.scalarBeforeHz = (!incomingSurface && !resetsToStock && hasHandoff) ? review->outgoingBeforeHz : beforeHz;
        mIn.scalarAfterHz = incomingSurface ? afterHz : 0;
        mIn.outgoingCeilingHz = modeled ? modeled->outgoingCeilingHz : 0;
        mIn.resetsToStock = resetsToStock;
        mIn.pureNonLoweringCapturedDelta = authoring_targets_pure_non_lowering_captured_delta(
            *d, preview, in.programmedOffsetsHz, hasLock, hasHandoff, multiStep,
            resetsToStock, mIn.scalarBeforeHz, mIn.scalarAfterHz);
        mIn.hasOutgoingSurfaceHandoff = hasHandoff;
        AuthoringModeledTransitionOutcome mOutcome{};
        char mWhy[256]{};
        if (!authoring_modeled_transition_preflight(mIn, *review, mOutcome, mWhy, sizeof(mWhy))) {
            set_message(why, whySize, "Curve transition: %s. Nothing was changed.", mWhy);
            return false;
        }
    }
    return true;
}

template <class Guard>
static bool apply_authoring_post_reset_modeled_review(const DesiredSettings* d, bool composeGraphics,
    ScalarCurveSubmissionReview& review, Guard& clockCeiling,
    const AuthoringModeledPrecheckContext* modeled, char* why, size_t whySize) {
    if (!d || !curve_authoring_needs_exact_targets(d)) return true;
    const gc_u32 incomingSurface = desired_named_graphics_surface(*d);
    const bool multiStep = modeled && modeled->multiStepCurve;
    if (!review.deferredModeledCheck && !incomingSurface && !multiStep) return true;
    if (!g_app.vfFrequenciesHzValid || !g_app.vfOffsetsHzValid) {
        set_message(why, whySize, "Post-reset exact curve samples are unavailable.");
        return false;
    }
    int componentKHz[VF_NUM_POINTS]{}; gc_i64 componentHz[VF_NUM_POINTS]{};
    auto in = apply_curve_plan_input(d, d->hasLock ? d->lockMode : LOCK_MODE_NONE,
        g_app.curve, g_app.freqOffsets, CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE,
        apply_lock_pretail_provenance_mode(composeGraphics, false), true,
        d->gpuOffsetExcludeLowCount, componentKHz, componentHz);
    in.frequencyHz = g_app.curveFrequencyHz; in.programmedOffsetsHz = g_app.freqOffsetsHz;
    in.evidence = CURVE_PLAN_EVIDENCE_TRANSACTION_BASELINE;
    const auto plan = curve_plan_evaluate(in);
    curve_plan_log(plan, "windows-authoring-post-reset", apply_curve_plan_log_line);
    if (plan.disposition != CURVE_PLAN_REPRESENTABLE) {
        curve_plan_format_message(plan, false, why, whySize);
        return false;
    }
    CurveAuthoringTargets postTargets; CurveAuthoringTargetIssue issue;
    if (!windows_build_authoring_curve_targets(in, g_app.vfBackend, g_app.vfMask, composeGraphics,
            false, nullptr, postTargets, issue, false)) {
        set_message(why, whySize, "Post-reset curve targets could not resolve point %d (status=%d).",
            issue.ci, (int)issue.status);
        return false;
    }
    gc_i64 scalarBeforeHz = 0;
    if (incomingSurface) {
        const auto scalar = graphics_offset_control_plan(*d, g_nvml_api, g_app.nvmlDevice);
        if (!graphics_offset_control_ready(scalar) || scalar.before.value != 0) {
            set_message(why, whySize, "Post-reset graphics scalar baseline is nonzero or unreadable.");
            return false;
        }
        scalarBeforeHz = (long long)scalar.before.value * 1000000;
        review.beforeValid = true;
        review.beforeHz = scalarBeforeHz;
    }
    const gc_i64 afterHz = incomingSurface ? desired_graphics_hz(*d) : 0;
    const bool scalarRaisesBeforeCurve = incomingSurface && postTargets.pointCount > 0 && afterHz > 0;
    const bool pinAuthorized = modeled && modeled->pinAuthorityExplicit
        ? modeled->pinMutationAuthorized
        : authoring_pin_mutation_authorized(*d);
    const bool clampCap = modeled && modeled->clampAvailabilityExplicit
        ? modeled->clampAvailable
        : (g_nvml_api.setGpuLockedClocks && g_nvml_api.resetGpuLockedClocks);
    AuthoringModeledTransitionInput mIn{};
    mIn.baseline = &in;
    mIn.targets = &postTargets;
    mIn.composition = modeled ? modeled->composition : CAPTURE_COMPOSITION_UNKNOWN;
    mIn.pinMutationAuthorized = pinAuthorized;
    mIn.clampAvailable = clampCap;
    mIn.scalarBeforeCurve = scalarRaisesBeforeCurve;
    mIn.multiStepCurve = multiStep;
    mIn.scalarBeforeHz = scalarBeforeHz;
    mIn.scalarAfterHz = afterHz;
    mIn.outgoingCeilingHz = (gc_i64)clockCeiling.outgoingCeilingMHz * 1000000LL;
    mIn.resetsToStock = true;
    AuthoringModeledTransitionOutcome outcome{};
    char mWhy[256]{};
    if (!authoring_modeled_transition_post_reset(mIn, review, outcome, mWhy, sizeof(mWhy))) {
        set_message(why, whySize, "Post-reset curve transition: %s.", mWhy);
        return false;
    }
    if (outcome.adopted && !clockCeiling.apply_modeled_plan(review.modeledCeiling, d, why, whySize))
        return false;
    review.deferredModeledCheck = false;
    return true;
}

static bool apply_correct_authoring_curve_targets(const DesiredSettings& desired,
    LockMode mode, bool composeGraphics, bool preserveUnowned, CurveAuthoringTargets& targets,
    ULONGLONG applyStart, ULONGLONG deadline, char* why, size_t whySize,
    VfControlWritePolicy policy = VF_WRITE_EXISTING_STRATEGY) {
    if (!vf_control_write_policy_valid(policy)) return false;
    if (policy == VF_WRITE_SINGLE_BATCH) {
        debug_log("curve-authoring: correction suppressed by single-batch policy\n");
        return apply_verify_authoring_curve_targets(targets, mode, desired.lockCi, why, whySize);
    }
    const CurveAuthoringTargets original = targets;
    ULONGLONG passStart = 0, previousPassMs = 0;
    bool compileFailed = false, unchanged = false;
    const auto loop = apply_run_correction_loop(APPLY_CORRECTION_MAX_PASSES,
        [&](int pass) {
            passStart = GetTickCount64();
            return apply_correction_pass_may_start(pass, passStart - applyStart, previousPassMs);
        },
        [&](int pass) {
            CurveAuthoringTargets next;
            if (!g_app.vfFrequenciesHzValid || !g_app.vfOffsetsHzValid) {
                set_message(why, whySize, "Fresh exact curve readback is unavailable before correction");
                compileFailed = true; return false;
            }
            if (!apply_build_authoring_curve_targets(desired, mode, composeGraphics, preserveUnowned,
                    g_app.curve, g_app.freqOffsets, g_app.curveFrequencyHz, g_app.freqOffsetsHz,
                    CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE, &original, next, why, whySize)) {
                compileFailed = true; return false;
            }
            unchanged = true;
            for (int i = 0; i < VF_NUM_POINTS; ++i)
                if (next.mask[i] && next.offsetHz[i] != targets.offsetHz[i]) unchanged = false;
            if (unchanged) return false;
            const bool written = apply_curve_offsets_exact_hz(next.offsetHz, next.mask,
                desired.hasLock ? 3 : 2, deadline, next.voltageUv, policy);
            targets = next;
            bool offsetsOk = false;
            if (!read_live_curve_snapshot_settled(6, 25, &offsetsOk) || !offsetsOk) {
                set_message(why, whySize, "Fresh exact curve readback is unavailable after correction");
                compileFailed = true; return false;
            }
            previousPassMs = GetTickCount64() - passStart;
            debug_log("curve-authoring: correction pass=%d points=%d rawVerified=%d elapsedMs=%llu\n",
                pass + 1, next.pointCount, written ? 1 : 0, (unsigned long long)previousPassMs);
            return true;
        },
        [](int) { return false; },
        [&](int) { return apply_verify_authoring_curve_targets(targets, mode, desired.lockCi, why, whySize); });
    debug_log("curve-authoring: correction passes=%d verified=%d budgetExhausted=%d unchanged=%d compileFailed=%d\n",
        loop.passesRun, loop.verified ? 1 : 0, loop.budgetExhausted ? 1 : 0, unchanged ? 1 : 0, compileFailed ? 1 : 0);
    return !compileFailed && (loop.verified || apply_verify_authoring_curve_targets(targets, mode, desired.lockCi, why, whySize));
}

static bool apply_authoring_point_matched(const CurveAuthoringTargets& targets, int ci) {
    if (!g_app.vfFrequenciesHzValid || !g_app.vfOffsetsHzValid) return false;
    auto legacyFrequency = [](int i) { return g_app.curve[i].freq_kHz; };
    auto exactFrequency = [](int i) { return g_app.curveFrequencyHz[i]; };
    return targets.absoluteMask[ci]
        ? curve_point_absolute_target_verified_hz_over(
              legacyFrequency, exactFrequency, ci, VF_NUM_POINTS, targets.absoluteHz[ci])
        : g_app.freqOffsetsHz[ci] == targets.offsetHz[ci];
}

static void apply_authoring_flatten_tally(const CurveAuthoringTargets& targets,
    LockMode mode, int lockCi, int& appliedOut, int& failedOut) {
    appliedOut = 0;
    failedOut = 0;
    if (mode == LOCK_MODE_NONE || lockCi < 0) return;
    for (int i = lockCi; i < VF_NUM_POINTS; ++i) {
        if (!targets.mask[i] || targets.preservedMask[i] || g_app.curve[i].freq_kHz == 0) continue;
        if (apply_authoring_point_matched(targets, i)) ++appliedOut;
        else ++failedOut;
    }
}

static ApplyBoostTally apply_authoring_curve_boost_tally(const CurveAuthoringTargets& targets,
    LockMode mode, int lockCi) {
    ApplyBoostTally tally{}; tally.firstFailedCi = -1;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (!targets.mask[i] || targets.preservedMask[i] ||
            (mode != LOCK_MODE_NONE && lockCi >= 0 && i >= lockCi)) continue;
        const bool matched = apply_authoring_point_matched(targets, i);
        const bool user = g_app.curveFrequencyHz[i] >= 500000000 &&
            (targets.absoluteMask[i] || targets.offsetHz[i] != 0);
        if (matched) { ++tally.applied; if (user) ++tally.userApplied; }
        else {
            ++tally.failed; if (user) ++tally.userFailed;
            if (tally.firstFailedCi < 0) {
                tally.firstFailedCi = i; tally.firstFailedActualMHz = curve_plan_display_hz(g_app.curveFrequencyHz[i]);
                tally.firstFailedProgrammedKHz = g_app.freqOffsets[i];
                tally.firstFailedTargetOffsetKHz = (int)(targets.offsetHz[i] / 1000);
                tally.firstFailedTargetMHz = curve_plan_display_hz(targets.absoluteHz[i]);
                tally.firstFailedOffsetIntent = !targets.absoluteMask[i];
            }
        }
    }
    return tally;
}

static void apply_authoring_prepare_verify_desired(const CurveAuthoringTargets& targets,
    const bool* lockedTailMask, bool hasLock, LockMode lockMode, int lockCi, unsigned int lockMhz,
    DesiredSettings& verifyDesired, bool* diagExplicitMask,
    int& flattenApplied, int& flattenFailed) {
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (!targets.mask[ci]) continue;
        if (hasLock && lockedTailMask[ci]) {
            verifyDesired.curvePointMHz[ci] = lockMhz;
            continue;
        }
        verifyDesired.hasCurvePoint[ci] = true;
        if (targets.absoluteMask[ci]) {
            verifyDesired.curvePointMHz[ci] = curve_plan_display_hz(targets.absoluteHz[ci]);
            verifyDesired.curvePointCaptured[ci] = 0;
            verifyDesired.curvePointFromGpuOffset[ci] = 0;
            diagExplicitMask[ci] = true;
        } else {
            verifyDesired.curvePointCaptured[ci] = 1;
            verifyDesired.curvePointMHz[ci] = 0;
            diagExplicitMask[ci] = false;
        }
    }
    if (hasLock) {
        apply_authoring_flatten_tally(targets, lockMode, lockCi, flattenApplied, flattenFailed);
        debug_log("flatten undervolt: target=%u MHz applied=%d failed=%d\n",
            lockMhz, flattenApplied, flattenFailed);
    }
}
