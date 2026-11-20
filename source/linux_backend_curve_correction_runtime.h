// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Linux daemon runtime wiring for post-write VF curve correction.
// Included by linux_backend_mutation.cpp after linux_backend_apply_context.h.
#ifndef GREEN_CURVE_LINUX_BACKEND_CURVE_CORRECTION_RUNTIME_H
#define GREEN_CURVE_LINUX_BACKEND_CURVE_CORRECTION_RUNTIME_H

static bool linux_apply_correct_authoring_curve(LinuxApplyTransactionContext* context,
    const DesiredSettings* intended, VfControlWritePolicy policy,
    unsigned long long curveStartMs) {
    LinuxGpuState* g = context->gpu;
    const LinuxCurveTargetBuildResult original = context->plannedBuild;
    unsigned long long passStartMs = 0;
    unsigned long long previousPassMs = 0;
    bool compileFailed = false;
    bool unchanged = false;
    const int batchPasses = intended && intended->hasLock ? 3 : 2;
    const auto loop = apply_run_correction_loop(
        APPLY_CORRECTION_MAX_PASSES,
        [&](int pass) {
            passStartMs = pl_monotonic_ms();
            const unsigned long long elapsed = passStartMs >= curveStartMs ? (passStartMs - curveStartMs) : 0ULL;
            if (linux_curve_correction_pass_may_start(pass, elapsed, previousPassMs))
                return true;
            lb_log("curve-authoring correction: stopping before pass %d (elapsed=%llums prev=%llums budget=%lums)\n",
                pass + 1, elapsed, previousPassMs, linux_curve_correction_budget_ms());
            return false;
        },
        [&](int pass) {
            if (!g->vfSnapshotFresh || !g->vfFrequenciesHzValid || !g->vfOffsetsHzValid) {
                lb_log("curve-authoring correction: exact readback unavailable before pass %d\n", pass + 1);
                compileFailed = true;
                return false;
            }
            int nextOffsets[VF_NUM_POINTS] = {};
            bool nextMask[VF_NUM_POINTS] = {};
            int cleanup = 0;
            LinuxApplyTransactionContext probe = *context;
            LinuxCurveTargetBuildResult next = linux_apply_build_curve(&probe, &cleanup, true, false);
            for (int i = 0; i < VF_NUM_POINTS; ++i) {
                nextOffsets[i] = probe.curveTargets[i];
                nextMask[i] = probe.curveMask[i];
            }
            if (!linux_authoring_correction_shape_matches(original, next)) {
                lb_log("curve-authoring correction: pass %d shape changed or compile failed; refusing write\n", pass + 1);
                compileFailed = true;
                return false;
            }
            unchanged = true;
            for (int i = 0; i < VF_NUM_POINTS; ++i) {
                if (next.pointMask[i] && next.pointExactOffsetHz[i] != context->plannedBuild.pointExactOffsetHz[i]) {
                    unchanged = false;
                    break;
                }
            }
            if (unchanged) return false;
            gc_i64 targetsHz[VF_NUM_POINTS] = {};
            bool exactMask[VF_NUM_POINTS] = {};
            for (int i = 0; i < VF_NUM_POINTS; ++i) {
                exactMask[i] = next.pointExactOffset[i] || next.pointCapturedIntent[i];
                targetsHz[i] = next.pointExactOffset[i] ? next.pointExactOffsetHz[i] : next.pointCapturedOffsetHz[i];
            }
            const bool written = apply_curve_offsets_verified(
                g, nextOffsets, nextMask, batchPasses, targetsHz, exactMask, policy);
            if (!written) {
                lb_log("curve-authoring correction: pass %d offset write did not converge\n", pass + 1);
                compileFailed = true;
                return false;
            }
            memcpy(context->curveTargets, nextOffsets, sizeof(context->curveTargets));
            memcpy(context->curveMask, nextMask, sizeof(context->curveMask));
            context->plannedBuild = next;
            const unsigned long long now = pl_monotonic_ms();
            previousPassMs = now >= passStartMs ? (now - passStartMs) : 0ULL;
            lb_log("curve-authoring correction pass %d: points=%d rawVerified=%d elapsedMs=%llu\n",
                pass + 1, next.pointCount, written ? 1 : 0, previousPassMs);
            return true;
        },
        [](int) { return false; },
        [&](int pass) {
            if (!linux_apply_verify_curve_write(context, false)) return false;
            lb_log("curve-authoring correction pass %d: verified live VF targets\n", pass + 1);
            return true;
        });
    lb_log("curve-authoring correction: passes=%d verified=%d budgetExhausted=%d unchanged=%d compileFailed=%d\n",
        loop.passesRun, loop.verified ? 1 : 0, loop.budgetExhausted ? 1 : 0,
        unchanged ? 1 : 0, compileFailed ? 1 : 0);
    if (!compileFailed && loop.verified) return true;
    return !compileFailed && linux_apply_verify_curve_write(context, true);
}

static bool linux_apply_correct_curve_write(LinuxApplyTransactionContext* context,
    VfControlWritePolicy policy, unsigned long long curveStartMs) {
    LinuxGpuState* g = context->gpu;
    if (!g->vfSnapshotFresh || policy == VF_WRITE_SINGLE_BATCH) {
        lb_log("curve correction: suppressed (fresh=%d singleBatch=%d)\n",
            g->vfSnapshotFresh ? 1 : 0, policy == VF_WRITE_SINGLE_BATCH ? 1 : 0);
        return false;
    }
    const DesiredSettings* intended = context->transitionBuild ? context->committedIntent : context->desired;
    if (intended && curve_authoring_needs_exact_targets(intended))
        return linux_apply_correct_authoring_curve(context, intended, policy, curveStartMs);
    int rangeMinKHz = 0, rangeMaxKHz = 0;
    curve_offset_range_khz(g, &rangeMinKHz, &rangeMaxKHz);
    VfOffsetRange range = {};
    range.minKHz = rangeMinKHz;
    range.maxKHz = rangeMaxKHz;
    range.known = g->curveOffsetRangeKnown ||
        (g->gpuOffsetMinMHz <= g->gpuOffsetMaxMHz && (g->gpuOffsetMinMHz || g->gpuOffsetMaxMHz));
    const int batchPasses = intended && intended->hasLock ? 3 : 2;
    const ApplyCorrectionLoopOutcome outcome = linux_run_legacy_curve_correction(
        intended, context->plannedBuild, context->curveTargets, range, curveStartMs,
        [&](const int* offsets, const bool* mask, const gc_i64* exactHz, const bool* exactMask) {
            if (!apply_curve_offsets_verified(g, offsets, mask, batchPasses, exactHz, exactMask, policy)) {
                lb_log("curve correction: offset write did not converge\n");
                return false;
            }
            char why[160] = {};
            if (!nvapi_read_vf_snapshot(g, why, sizeof(why))) {
                lb_log("curve correction: curve re-read failed (%s)\n", why);
                return false;
            }
            return true;
        },
        [&](int ci) -> unsigned int { return g->curve[ci].freq_kHz; },
        [&](int ci) -> int { return g->freqOffsets[ci]; },
        [&](char* detail, size_t detailSize) -> bool {
            return linux_verify_curve_absolute_targets(g->curve, context->plannedBuild,
                detail, detailSize, g->curveFrequencyHz, g->vfFrequenciesHzValid) &&
                linux_verify_curve_captured_targets(g->freqOffsetsHz, context->plannedBuild,
                    detail, detailSize, g->vfOffsetsHzValid) &&
                linux_verify_projected_offsets(g->curve, g->freqOffsets, context->plannedBuild,
                    context->curveTargets, intended, detail, detailSize);
        },
        []() -> unsigned long long { return pl_monotonic_ms(); },
        [](const char* line) { lb_log("%s", line ? line : ""); });
    if (!outcome.verified) (void)linux_apply_verify_curve_write(context, true);
    return outcome.verified;
}

#endif  // GREEN_CURVE_LINUX_BACKEND_CURVE_CORRECTION_RUNTIME_H
