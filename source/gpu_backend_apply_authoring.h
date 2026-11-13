// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "windows_curve_authoring_targets.h"
#include "desired_graphics_surface.h"
#include "windows_graphics_offset_route.h"
#include "vf_control_write_policy.h"
#include "scalar_curve_submission_policy.h"
#include "graphics_offset_control.h"

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
    char* why, size_t whySize, ScalarCurveSubmissionReview* review = nullptr) {
    if (review) *review = {};
    if (!curve_authoring_needs_exact_targets(d)) return true;
    bool offsetsOk = false;
    if (!read_live_curve_snapshot_settled(1, 0, &offsetsOk) || !offsetsOk ||
        !g_app.vfFrequenciesHzValid || !g_app.vfOffsetsHzValid) {
        set_message(why, whySize, "Fresh exact curve samples are unavailable. Nothing was changed."); return false;
    }
    int componentKHz[VF_NUM_POINTS]{}; gc_i64 componentHz[VF_NUM_POINTS]{};
    auto in = apply_curve_plan_input(d, d->hasLock ? d->lockMode : LOCK_MODE_NONE,
        g_app.curve, g_app.freqOffsets, CURVE_PLAN_EVIDENCE_LIVE_SHAPED_ESTIMATE,
        apply_lock_pretail_provenance_mode(composeGraphics, false), d->resetOcBeforeApply != 0,
        d->gpuOffsetExcludeLowCount, componentKHz, componentHz);
    in.frequencyHz = g_app.curveFrequencyHz; in.programmedOffsetsHz = g_app.freqOffsetsHz;
    // This read does not establish the absence of a separate scalar. Keep
    // baseline-derived failures conditional until authorized preparation.
    in.evidence = curve_plan_classify_pre_write_evidence(in.curve, in.programmedOffsetsKHz,
        in.floor, false, in.programmedOffsetsHz);
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
    if (desired_named_graphics_surface(*d)) {
        const auto scalar = graphics_offset_control_plan(*d, g_nvml_api, g_app.nvmlDevice);
        const auto transition = scalar_curve_submission_plan(*d, graphics_offset_control_ready(scalar),
            (long long)scalar.before.value * 1000000, in.programmedOffsetsHz, true,
            g_nvml_api.setGpuLockedClocks && g_nvml_api.resetGpuLockedClocks,
            g_app.gpuOffsetOwnedSurface && g_app.gpuOffsetOwnedSurface != scalar.surface);
        debug_log("scalar-curve: preflight status=%d loweredCaptured=%d baselineDependent=%d\n",
            (int)transition.status, transition.loweredCaptured, transition.baselineDependent ? 1 : 0);
        if (!scalar_curve_submission_allowed(transition)) {
            set_message(why, whySize, "Curve transition: %s. Nothing was changed.", scalar_curve_submission_reason(transition.status));
            return false;
        }
        if (review) {
            review->installedGuardRequired = transition.status == SCALAR_CURVE_INSTALL_GUARD;
            review->beforeValid = graphics_offset_control_ready(scalar);
            review->beforeHz = (long long)scalar.before.value * 1000000;
        }
    }
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

static ApplyBoostTally apply_authoring_curve_boost_tally(const CurveAuthoringTargets& targets,
    LockMode mode, int lockCi) {
    ApplyBoostTally tally{}; tally.firstFailedCi = -1;
    auto legacyFrequency = [](int i) { return g_app.curve[i].freq_kHz; };
    auto exactFrequency = [](int i) { return g_app.curveFrequencyHz[i]; };
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (!targets.mask[i] || targets.preservedMask[i] ||
            (mode != LOCK_MODE_NONE && lockCi >= 0 && i >= lockCi)) continue;
        const bool matched = g_app.vfFrequenciesHzValid && g_app.vfOffsetsHzValid &&
            (targets.absoluteMask[i] ? curve_point_absolute_target_verified_hz_over(
                legacyFrequency, exactFrequency, i, VF_NUM_POINTS, targets.absoluteHz[i])
                : g_app.freqOffsetsHz[i] == targets.offsetHz[i]);
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
