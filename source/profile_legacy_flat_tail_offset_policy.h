// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_LEGACY_FLAT_TAIL_OFFSET_POLICY_H
#define GREEN_CURVE_PROFILE_LEGACY_FLAT_TAIL_OFFSET_POLICY_H
// Legacy compatibility repair for pre-2026-11-19 saved flatten profiles that
// carry a mis-detected flat-tail graphics offset artifact.
//
// Before live VF offset detection excluded unowned external programmed flat
// tails (vf_live_offset_policy.h), another tool's tail flatten (writing a large
// constant negative delta such as -1927 MHz to every tail point ci > lockCi and
// relying on the driver's monotonic clamp) was detected as a selective or
// uniform GPU offset (for example gpu_offset_mhz=-1927 with
// gpu_offset_exclude_low_count=76 alongside lock_ci=76, lock_mode=flatten,
// absolute flat tail points 76..126, and zero projected points).
//
// In an ordinary INI slot, validate_desired_settings_for_ipc() silently clamped
// that bogus -1927 MHz to -1000 MHz while keeping exclude_low_count=76 (and with
// lock_tracks_anchor=1 could corrupt the lock target). Once exported and
// re-imported as a portable indexed document, profile_desired_from_indexed_document()
// refused the slot outright because -1927 MHz is outside -1000..1000 MHz,
// preventing the user from even loading their own saved profile.
//
// This policy recognizes only that provably dead flat-tail artifact:
//   - strictly below -1000 MHz (never touches any valid in-range offset or any
//     positive out-of-range offset);
//   - explicit FLATTEN lock with at least one tail point above lockCi and every
//     present tail point at lockCi..127 holding the exact absolute lock target;
//   - zero projected or captured curve points;
//   - either selective exclusion covering the entire pre-tail
//     (excludeLowCount >= lockCi, so zero pre-tail points are affected, and any
//     recorded tail observed offsets corroborate the negative delta), OR
//     uniform (excludeLowCount == 0) with zero pre-tail points, at least four
//     observed tail offsets all matching the negative delta within 30 MHz, and
//     an observed anchor offset proving the anchor itself did not carry it.
//
// Any genuinely out-of-range offset fails this predicate and continues to be
// refused with exact MHz and bounds.
#include "profile_document.h"

static constexpr long long PROFILE_LEGACY_FLAT_TAIL_OFFSET_LIMIT_HZ = -1000000000LL;
static constexpr int PROFILE_LEGACY_FLAT_TAIL_OFFSET_LIMIT_MHZ = -1000;
static constexpr unsigned long long PROFILE_LEGACY_FLAT_TAIL_OFFSET_TOLERANCE_HZ = 30000000ULL;
static constexpr int PROFILE_LEGACY_FLAT_TAIL_UNIFORM_MIN_MATCHES = 4;

static inline unsigned long long profile_legacy_flat_tail_abs_diff_i64(long long a, long long b) {
    return a >= b
        ? (unsigned long long)a - (unsigned long long)b
        : (unsigned long long)b - (unsigned long long)a;
}

static inline bool profile_indexed_document_has_legacy_flat_tail_offset_artifact(
    const ProfileDocument& s) {
    if (!s.legacyIndicesUnresolved) return false;
    if (!s.graphicsOffset.present) return false;
    const auto& g = s.graphicsOffset;
    if (g.offsetHz >= PROFILE_LEGACY_FLAT_TAIL_OFFSET_LIMIT_HZ) return false;
    if (!s.lock.present || s.lock.mode != LOCK_MODE_FLATTEN) return false;
    if (s.lock.targetFrequencyHz <= 0 || s.lock.targetFrequencyHz > 5000000000LL) return false;
    if (s.topologyCount <= 0 || s.topologyCount > VF_NUM_POINTS) return false;

    const int anchorAt = profile_topology_index(s, s.lock.anchorTopologyId);
    if (anchorAt < 0 || anchorAt >= s.topologyCount) return false;
    const int lockCi = s.topology[anchorAt].sourceIndex;
    if (lockCi < 0 || lockCi >= VF_NUM_POINTS) return false;

    const bool selectiveCoversPreTail =
        g.exclusionPolicy == PROFILE_EXCLUDE_LEGACY_COUNT && g.excludeLowCount >= lockCi;
    const bool uniformOnly =
        g.exclusionPolicy == PROFILE_EXCLUDE_NONE ||
        (g.exclusionPolicy == PROFILE_EXCLUDE_LEGACY_COUNT && g.excludeLowCount == 0);
    if (!selectiveCoversPreTail && !uniformOnly) return false;

    bool seen[VF_NUM_POINTS]{};
    int preTailPointCount = 0;
    int tailPointCount = 0;
    int tailObservedCount = 0;
    int tailMatchingObservedCount = 0;
    bool anchorHasObservedOffset = false;
    long long anchorObservedOffsetHz = 0;

    for (int i = 0; i < s.topologyCount; ++i) {
        const int ci = s.topology[i].sourceIndex;
        if (ci < 0 || ci >= VF_NUM_POINTS || seen[ci]) return false;
        seen[ci] = true;
        const auto& p = s.points[i];
        if (p.kind == PROFILE_POINT_ABSENT) continue;
        if (p.kind != PROFILE_POINT_ABSOLUTE_FREQUENCY) return false;

        if (ci < lockCi) {
            ++preTailPointCount;
        } else if (ci == lockCi) {
            if (p.absoluteFrequencyHz != s.lock.targetFrequencyHz) return false;
            if (p.hasObservedOffset) {
                anchorHasObservedOffset = true;
                anchorObservedOffsetHz = p.observedOffsetHz;
            }
        } else {
            if (p.absoluteFrequencyHz != s.lock.targetFrequencyHz) return false;
            ++tailPointCount;
            if (p.hasObservedOffset) {
                ++tailObservedCount;
                if (profile_legacy_flat_tail_abs_diff_i64(p.observedOffsetHz, g.offsetHz) <=
                    PROFILE_LEGACY_FLAT_TAIL_OFFSET_TOLERANCE_HZ) {
                    ++tailMatchingObservedCount;
                }
            }
        }
    }

    if (tailPointCount < 1) return false;
    if (selectiveCoversPreTail) {
        if (tailObservedCount > 0 && tailMatchingObservedCount == 0) return false;
        return true;
    }

    // Uniform (excludeLowCount == 0) requires direct observed-offset proof that
    // the large negative delta lived strictly on the flattened tail ci > lockCi.
    if (preTailPointCount > 0) return false;
    if (tailObservedCount < PROFILE_LEGACY_FLAT_TAIL_UNIFORM_MIN_MATCHES) return false;
    if (tailMatchingObservedCount != tailObservedCount) return false;
    if (!anchorHasObservedOffset) return false;
    if (profile_legacy_flat_tail_abs_diff_i64(anchorObservedOffsetHz, g.offsetHz) <=
        PROFILE_LEGACY_FLAT_TAIL_OFFSET_TOLERANCE_HZ) {
        return false;
    }
    return true;
}

static inline bool profile_desired_has_legacy_flat_tail_offset_artifact(
    const DesiredSettings* desired,
    const bool* hasObservedOffset,
    const int* observedOffsetKHz) {
    if (!desired) return false;
    if (desired->applicationScope != DESIRED_SCOPE_LEGACY) return false;
    if (!desired->hasGpuOffset || desired->gpuOffsetMHz >= PROFILE_LEGACY_FLAT_TAIL_OFFSET_LIMIT_MHZ)
        return false;
    if (!desired->hasLock || desired->lockMode != LOCK_MODE_FLATTEN) return false;
    if (desired->lockCi < 0 || desired->lockCi >= VF_NUM_POINTS || desired->lockMHz == 0)
        return false;

    const int lockCi = desired->lockCi;
    const bool selectiveCoversPreTail = desired->gpuOffsetExcludeLowCount >= lockCi;
    const bool uniformOnly = desired->gpuOffsetExcludeLowCount <= 0;
    if (!selectiveCoversPreTail && !uniformOnly) return false;

    const long long targetOffsetHz = (long long)desired->gpuOffsetMHz * 1000000LL;
    int preTailPointCount = 0;
    int tailPointCount = 0;
    int tailObservedCount = 0;
    int tailMatchingObservedCount = 0;
    bool anchorHasObservedOffset = false;
    long long anchorObservedOffsetHz = 0;

    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (desired->curvePointCaptured[ci]) return false;
        if (!desired->hasCurvePoint[ci]) continue;
        if (desired->curvePointFromGpuOffset[ci]) return false;

        if (ci < lockCi) {
            ++preTailPointCount;
        } else if (ci == lockCi) {
            if (desired->curvePointMHz[ci] != desired->lockMHz) return false;
            if (hasObservedOffset && observedOffsetKHz && hasObservedOffset[ci]) {
                anchorHasObservedOffset = true;
                anchorObservedOffsetHz = (long long)observedOffsetKHz[ci] * 1000LL;
            }
        } else {
            if (desired->curvePointMHz[ci] != desired->lockMHz) return false;
            ++tailPointCount;
            if (hasObservedOffset && observedOffsetKHz && hasObservedOffset[ci]) {
                ++tailObservedCount;
                const long long obsHz = (long long)observedOffsetKHz[ci] * 1000LL;
                if (profile_legacy_flat_tail_abs_diff_i64(obsHz, targetOffsetHz) <=
                    PROFILE_LEGACY_FLAT_TAIL_OFFSET_TOLERANCE_HZ) {
                    ++tailMatchingObservedCount;
                }
            }
        }
    }

    if (tailPointCount < 1) return false;
    if (selectiveCoversPreTail) {
        if (tailObservedCount > 0 && tailMatchingObservedCount == 0) return false;
        return true;
    }

    if (preTailPointCount > 0) return false;
    if (tailObservedCount < PROFILE_LEGACY_FLAT_TAIL_UNIFORM_MIN_MATCHES) return false;
    if (tailMatchingObservedCount != tailObservedCount) return false;
    if (!anchorHasObservedOffset) return false;
    if (profile_legacy_flat_tail_abs_diff_i64(anchorObservedOffsetHz, targetOffsetHz) <=
        PROFILE_LEGACY_FLAT_TAIL_OFFSET_TOLERANCE_HZ) {
        return false;
    }
    return true;
}

static inline void profile_desired_repair_legacy_flat_tail_offset(DesiredSettings* desired) {
    if (!desired) return;
    desired->hasGpuOffset = 1;
    desired->gpuOffsetMHz = 0;
    desired->gpuOffsetExcludeLowCount = 0;
    desired->lockTracksAnchor = 0;
}

#endif  // GREEN_CURVE_PROFILE_LEGACY_FLAT_TAIL_OFFSET_POLICY_H
