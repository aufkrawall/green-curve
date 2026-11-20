// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Bounded post-write VF curve frequency correction for Linux apply.
//
// Matches Windows' post-write correction contract (apply_correction_policy.h
// and gpu_backend_apply_authoring.h): on Blackwell/Ada, writing a raised
// flatten anchor alongside a -1000 MHz flat tail can quantize or shift the
// anchor by a clock bin (e.g. 2947 MHz vs requested 2957 MHz, outside the
// 8 MHz verification tolerance). Re-deriving absolute-intent targets from the
// fresh post-write readback and running a time-bounded correction loop lets
// the curve converge without relaxing verification or exceeding the daemon's
// hardware write budget.
#ifndef GREEN_CURVE_LINUX_BACKEND_CURVE_CORRECTION_H
#define GREEN_CURVE_LINUX_BACKEND_CURVE_CORRECTION_H

#include "apply_correction_policy.h"
#include "linux_daemon_deadline_policy.h"
#include "linux_curve_targets.h"

// Keep 2000 ms of GC_DAEMON_HARDWARE_WRITE_BUDGET_MS in reserve for the
// post-curve lock/fan/refresh phases (or rollback on failure).
#define LINUX_CURVE_CORRECTION_RESERVE_MS 2000u

static_assert(LINUX_CURVE_CORRECTION_RESERVE_MS < GC_DAEMON_HARDWARE_WRITE_BUDGET_MS,
              "Linux curve correction reserve must leave time for correction passes");

static constexpr unsigned long linux_curve_correction_budget_ms() {
    return (unsigned long)GC_DAEMON_HARDWARE_WRITE_BUDGET_MS -
           (unsigned long)LINUX_CURVE_CORRECTION_RESERVE_MS;
}

// Pass 0 is always permitted so a recoverable one-bin quantization miss is
// never refused purely because earlier phases were slow.
static inline bool linux_curve_correction_pass_may_start(
    int passIndex, unsigned long long elapsedMs, unsigned long long previousPassMs) {
    if (passIndex <= 0) return true;
    return elapsedMs + previousPassMs <= (unsigned long long)linux_curve_correction_budget_ms();
}

// During exact-authoring correction, only baseline-dependent targets (absolute
// points and the lock anchor) may adjust their offset; write membership,
// voltage bindings, target frequencies, and captured/offset-intent offsets must
// stay unchanged.
static inline bool linux_authoring_correction_shape_matches(
    const LinuxCurveTargetBuildResult& original,
    const LinuxCurveTargetBuildResult& next) {
    if (original.failed || next.failed || next.deferred ||
        original.pointCount != next.pointCount ||
        original.composedGpuOffset != next.composedGpuOffset ||
        original.lockTail != next.lockTail)
        return false;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (original.pointMask[i] != next.pointMask[i] ||
            original.pointVoltageUv[i] != next.pointVoltageUv[i] ||
            original.pointAbsoluteIntent[i] != next.pointAbsoluteIntent[i] ||
            original.pointFrequencyPrecise[i] != next.pointFrequencyPrecise[i] ||
            original.pointTargetHz[i] != next.pointTargetHz[i] ||
            original.pointCapturedIntent[i] != next.pointCapturedIntent[i] ||
            original.pointBaselineDependent[i] != next.pointBaselineDependent[i])
            return false;
        if (original.pointMask[i] && !original.pointBaselineDependent[i] &&
            original.pointExactOffsetHz[i] != next.pointExactOffsetHz[i])
            return false;
    }
    return true;
}

// Exact-authoring flatten monotonicity check (mirrors
// windows_verify_authoring_curve_targets() in windows_curve_authoring_targets.h).
static inline bool linux_verify_authoring_flatten_monotonicity(
    const VFCurvePoint* curve, const gc_i64* frequencyHz, const gc_i64* offsetsHz,
    bool frequenciesValid, bool offsetsValid, const LinuxCurveTargetBuildResult& build,
    LockMode lockMode, int lockCi, char* detail, size_t detailSize) {
    if (lockMode != LOCK_MODE_FLATTEN || lockCi < 0 || lockCi >= VF_NUM_POINTS)
        return true;
    if (!curve || !frequencyHz || !offsetsHz || !frequenciesValid || !offsetsValid)
        return false;
    const gc_i64 plateau = build.pointTargetHz[lockCi];
    if (plateau <= 0) return true;
    for (int i = 0; i < lockCi; ++i) {
        if (!curve[i].freq_kHz) continue;
        const gc_i64 base = frequencyHz[i] > offsetsHz[i] ? frequencyHz[i] - offsetsHz[i] : 0;
        const gc_i64 intended = build.pointAbsoluteIntent[i] ? build.pointTargetHz[i]
            : build.pointMask[i] ? base + build.pointExactOffsetHz[i] : frequencyHz[i];
        if (intended > plateau) {
            if (detail) gc_snprintf(detail, detailSize,
                "Preceding VF point %d (%lld Hz) would raise the requested flat tail (%lld Hz)",
                i, intended, plateau);
            return false;
        }
    }
    return true;
}

// Pure legacy correction pass harness, reusable by both the Linux daemon
// transaction and unit tests with fake driver callbacks.
//
// `writeAndReadback(offsetsKHz, mask, exactHz, exactMask)` writes the pass and
// updates `g->curve` / `g->freqOffsets` (or fake readback arrays); returns false
// only on fatal readback failure.
// `verifyReadback(detail, detailSize)` checks post-write verification.
// `nowMs()` supplies monotonic milliseconds for budget accounting.
template <typename WriteAndReadback, typename ReadFreqKHz, typename ReadOffsetKHz,
          typename VerifyReadback, typename NowMs, typename LogFn>
static inline ApplyCorrectionLoopOutcome linux_run_legacy_curve_correction(
    const DesiredSettings* intended, const LinuxCurveTargetBuildResult& build,
    const int* initialTargetOffsets, const VfOffsetRange& range,
    unsigned long long startMs, WriteAndReadback writeAndReadback,
    ReadFreqKHz readFreqKHz, ReadOffsetKHz readOffsetKHz,
    VerifyReadback verifyReadback, NowMs nowMs, LogFn logFn,
    bool* monotonicViolationOut = nullptr, int* violationCiOut = nullptr,
    unsigned int* violationTargetOut = nullptr) {
    if (monotonicViolationOut) *monotonicViolationOut = false;
    if (violationCiOut) *violationCiOut = -1;
    if (violationTargetOut) *violationTargetOut = 0;

    bool lockedTailMask[VF_NUM_POINTS] = {};
    bool explicitCurveMask[VF_NUM_POINTS] = {};
    bool originalPopulated[VF_NUM_POINTS] = {};
    gc_bool8 hasCurvePoint[VF_NUM_POINTS] = {};
    unsigned int curvePointMHz[VF_NUM_POINTS] = {};
    gc_bool8 fromGpuOffset[VF_NUM_POINTS] = {};
    int targetOffsetKHz[VF_NUM_POINTS] = {};

    const bool hasLock = intended && intended->hasLock &&
        intended->lockMode != LOCK_MODE_NONE && intended->lockMHz > 0 &&
        intended->lockCi >= 0 && intended->lockCi < VF_NUM_POINTS;
    const int lockCi = hasLock ? intended->lockCi : -1;
    const unsigned int lockMHz = hasLock ? intended->lockMHz : 0u;

    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        const unsigned int f = readFreqKHz(ci);
        originalPopulated[ci] = f != 0;
        lockedTailMask[ci] = hasLock && ci >= lockCi && f != 0;
        explicitCurveMask[ci] = !lockedTailMask[ci] && build.pointAbsoluteIntent[ci];
        hasCurvePoint[ci] = build.pointMask[ci] ? 1 : 0;
        if (build.pointAbsoluteIntent[ci])
            curvePointMHz[ci] = build.pointTargetMHz[ci];
        else if (f != 0)
            curvePointMHz[ci] = curve_display_mhz_from_khz(f);
        // Non-tail points without absolute intent (offset-derived projections,
        // selective/global offsets, and captured/cleanup points) hold their
        // requested offset rather than chasing a projected MHz.
        fromGpuOffset[ci] = (!lockedTailMask[ci] && !build.pointAbsoluteIntent[ci]) ? 1 : 0;
        targetOffsetKHz[ci] = initialTargetOffsets ? initialTargetOffsets[ci] : 0;
    }

    ApplyCorrectionRequest req = {};
    req.pointCount = VF_NUM_POINTS;
    req.hasLock = hasLock;
    req.lockMHz = lockMHz;
    req.lockCi = lockCi;
    req.gpuPolicyViaCurveBatch = build.composedGpuOffset;
    req.offsetInVfTable = build.composedGpuOffset;
    req.lockedTailMask = lockedTailMask;
    req.explicitCurveMask = explicitCurveMask;
    req.originalCurvePopulated = originalPopulated;
    req.hasCurvePoint = hasCurvePoint;
    req.curvePointMHz = curvePointMHz;
    req.fromGpuOffset = fromGpuOffset;
    req.targetOffsetKHz = targetOffsetKHz;

    unsigned int rbFreqKHz[VF_NUM_POINTS] = {};
    int rbOffsetKHz[VF_NUM_POINTS] = {};
    const ApplyCorrectionReadback readback = {rbFreqKHz, rbOffsetKHz};
    auto snapshotReadback = [&]() {
        for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
            rbFreqKHz[ci] = readFreqKHz(ci);
            rbOffsetKHz[ci] = readOffsetKHz(ci);
        }
    };

    int prevErrorKHz[VF_NUM_POINTS] = {};
    apply_correction_reset_history(VF_NUM_POINTS, prevErrorKHz);
    unsigned long long passStartMs = 0;
    unsigned long long previousPassMs = 0;
    bool readbackFailed = false;

    ApplyCorrectionLoopOutcome outcome = apply_run_correction_loop(
        APPLY_CORRECTION_MAX_PASSES,
        [&](int pass) {
            passStartMs = nowMs();
            const unsigned long long elapsed = passStartMs >= startMs ? (passStartMs - startMs) : 0ULL;
            if (linux_curve_correction_pass_may_start(pass, elapsed, previousPassMs))
                return true;
            char line[256] = {};
            gc_snprintf(line, sizeof(line),
                "curve correction: stopping before pass %d (elapsed=%llums prev=%llums budget=%lums)\n",
                pass + 1, elapsed, previousPassMs, linux_curve_correction_budget_ms());
            logFn(line);
            return false;
        },
        [&](int pass) {
            int correctedOffsets[VF_NUM_POINTS] = {};
            bool correctedMask[VF_NUM_POINTS] = {};
            gc_i64 exactHz[VF_NUM_POINTS] = {};
            bool exactMask[VF_NUM_POINTS] = {};
            snapshotReadback();
            ApplyCorrectionPlan plan = apply_correction_plan_pass(
                req, readback, range, correctedOffsets, correctedMask);
            if (!plan.haveCorrections) return false;
            // Preserve exact Hz offsets for any captured/cleanup points.
            for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
                if (!build.pointCapturedIntent[ci]) continue;
                exactMask[ci] = true;
                exactHz[ci] = build.pointCapturedOffsetHz[ci];
                correctedOffsets[ci] = (int)(exactHz[ci] / 1000);
                correctedMask[ci] = true;
            }
            if (!writeAndReadback(correctedOffsets, correctedMask, exactHz, exactMask)) {
                readbackFailed = true;
                return false;
            }
            const unsigned long long now = nowMs();
            previousPassMs = now >= passStartMs ? (now - passStartMs) : 0ULL;
            char line[256] = {};
            gc_snprintf(line, sizeof(line),
                "curve correction pass %d: absolute=%d offsetIntent=%d tailFloor=%d clamped=%d elapsedMs=%llu\n",
                pass + 1, plan.absoluteCount, plan.offsetIntentCount,
                plan.tailFloorCount, plan.clampedCount, previousPassMs);
            logFn(line);
            return true;
        },
        [&](int pass) {
            snapshotReadback();
            ApplyCorrectionPassStats st = apply_correction_classify_pass(
                req, readback, range, pass, prevErrorKHz, nullptr);
            if (st.fixedPoint) {
                char line[192] = {};
                gc_snprintf(line, sizeof(line),
                    "curve correction pass %d: reached fixed point (unconverged=%d improved=%d)\n",
                    pass + 1, st.unconverged, st.improved);
                logFn(line);
            }
            return st.fixedPoint;
        },
        [&](int pass) {
            char detail[160] = {};
            if (!verifyReadback(detail, sizeof(detail))) return false;
            char line[192] = {};
            gc_snprintf(line, sizeof(line),
                "curve correction pass %d: verified live VF targets\n", pass + 1);
            logFn(line);
            return true;
        });

    if (readbackFailed) outcome.verified = false;
    if (outcome.verified && hasLock && intended->lockMode == LOCK_MODE_FLATTEN) {
        snapshotReadback();
        unsigned int lastTargetMHz = 0;
        const int violationCi = apply_correction_tail_monotonic_violation(
            req, readback, &lastTargetMHz);
        if (violationCi >= 0) {
            outcome.verified = false;
            if (monotonicViolationOut) *monotonicViolationOut = true;
            if (violationCiOut) *violationCiOut = violationCi;
            if (violationTargetOut) *violationTargetOut = lastTargetMHz;
            char line[256] = {};
            gc_snprintf(line, sizeof(line),
                "curve correction: strict flatten monotonicity violation at ci=%d (lock=%u MHz predecessor=%u MHz)\n",
                violationCi, lockMHz, lastTargetMHz);
            logFn(line);
        }
    }
    char summary[192] = {};
    gc_snprintf(summary, sizeof(summary),
        "curve correction: passes=%d verified=%d fixedPoint=%d budgetExhausted=%d nothingToCorrect=%d\n",
        outcome.passesRun, outcome.verified ? 1 : 0, outcome.fixedPoint ? 1 : 0,
        outcome.budgetExhausted ? 1 : 0, outcome.nothingToCorrect ? 1 : 0);
    logFn(summary);
    return outcome;
}

#endif  // GREEN_CURVE_LINUX_BACKEND_CURVE_CORRECTION_H
