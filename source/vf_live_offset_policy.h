// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_VF_LIVE_OFFSET_POLICY_H
#define GREEN_CURVE_VF_LIVE_OFFSET_POLICY_H

// Pure live VF-curve GPU-offset detection (uniform and selective exclude-low).
//
// Why this exists: an external curve editor flattens a tail by writing a large
// negative delta (for example -1927 MHz or -2045 MHz) to every tail point and
// relying on the driver's monotonic clamp to hold those points at the anchor.
// Because Green Curve's service deliberately never adopts an unowned external
// flatten as its own lock (`ownedLockCi == -1`, gpu_lock_ownership_policy.h),
// skipping only `ci >= ownedLockCi` left the flattened tail visible to offset
// detection:
//   - selective offset detection saw `0..anchor-1` near 0 and `anchor+1..end`
//     all sharing that large negative tail delta, reporting a bogus selective
//     GPU offset (e.g. -1927 MHz, exclude first 76 VF points);
//   - uniform offset detection saw a >50% tail carrying one constant negative
//     delta when the flatten anchor sat below the midpoint, reporting that
//     negative tail delta as a global uniform GPU offset.
//
// Both detectors therefore share one tail-boundary rule
// (`vf_offset_detection_tail_start()`): exclude Green Curve's owned lock tail
// when present, AND exclude any live programmed flat tail recognized by
// `vf_recognize_programmed_flat_tail()`.  A stock level top (zero offsets, or
// one uniform/selective offset across the top) has no differing tail offsets
// and is never excluded.

#include <limits.h>

#include "vf_flat_tail_recognition.h"

struct VfOffsetTailBoundary {
    int tailStartCi;             // -1 when no tail is excluded
    bool fromOwnedLock;          // ownedLockCi supplied tailStartCi
    bool fromObservedTail;       // a live programmed flat tail supplied tailStartCi
    int observedAnchorCi;        // -1 when no programmed flat tail was recognized
    unsigned int observedPlateauMHz;
};

struct VfSelectiveGpuOffsetDetection {
    bool detected;
    int gpuOffsetMHz;
    int representativeOffsetKHz;
    int excludeLowCount;
    int matchCount;
    int consideredCount;
    int excludedViolations;
    int excludedTotal;
    int candidateCount;
    int candidateSampleMHz[5];
    int populatedCount;
    VfOffsetTailBoundary tail;
};

static inline long long vf_live_offset_abs64(long long v) {
    return v < 0 ? -v : v;
}

static inline VfOffsetTailBoundary vf_offset_detection_tail_start(
    const VFCurvePoint* curve, const int* freqOffsetsKHz,
    const VfFloorTopology* floorOpt, int ownedLockCi) {
    VfOffsetTailBoundary out = {};
    out.tailStartCi = -1;
    out.observedAnchorCi = -1;

    const int owned = (ownedLockCi >= 0 && ownedLockCi < VF_NUM_POINTS) ? ownedLockCi : -1;
    if (!curve || !freqOffsetsKHz) {
        out.tailStartCi = owned;
        out.fromOwnedLock = owned >= 0;
        return out;
    }

    const VfFloorTopology floor = (floorOpt && floorOpt->valid)
        ? *floorOpt
        : vf_floor_topology_compute(curve, freqOffsetsKHz, VF_NUM_POINTS);
    const VfFlatTailRecognition rec =
        vf_recognize_programmed_flat_tail(curve, freqOffsetsKHz, floor, 1, 3);
    if (rec.found) {
        out.observedAnchorCi = rec.anchorCi;
        out.observedPlateauMHz = rec.plateauMHz;
    }

    if (owned < 0) {
        if (rec.found) {
            out.tailStartCi = rec.anchorCi;
            out.fromObservedTail = true;
        }
        return out;
    }

    // Green Curve owns a lock at `owned`.  Only pull the boundary earlier if an
    // external flatten actually programmed differing offsets between the
    // observed anchor and `owned` — a stock level top leading into `owned` has
    // identical offsets across `rec.anchorCi .. owned-1` and stays included.
    if (rec.found && rec.anchorCi < owned) {
        bool earlierProgrammed = false;
        for (int j = rec.anchorCi + 1; j < owned; ++j) {
            if (curve[j].freq_kHz == 0) continue;
            if (freqOffsetsKHz[j] != freqOffsetsKHz[rec.anchorCi]) {
                earlierProgrammed = true;
                break;
            }
        }
        if (earlierProgrammed) {
            out.tailStartCi = rec.anchorCi;
            out.fromObservedTail = true;
            return out;
        }
    }

    out.tailStartCi = owned;
    out.fromOwnedLock = true;
    return out;
}

static inline int vf_uniform_curve_offset_khz(
    const VFCurvePoint* curve, const int* freqOffsetsKHz,
    const VfFloorTopology* floorOpt, int ownedLockCi,
    VfOffsetTailBoundary* tailOut = nullptr) {
    VfOffsetTailBoundary tail =
        vf_offset_detection_tail_start(curve, freqOffsetsKHz, floorOpt, ownedLockCi);
    if (tailOut) *tailOut = tail;
    if (!curve || !freqOffsetsKHz) return 0;

    int values[VF_NUM_POINTS] = {};
    int counts[VF_NUM_POINTS] = {};
    int uniqueCount = 0;
    int consideredCount = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (curve[i].freq_kHz == 0) continue;
        if (tail.tailStartCi >= 0 && i >= tail.tailStartCi) continue;
        ++consideredCount;
        const int delta = freqOffsetsKHz[i];
        bool found = false;
        for (int j = 0; j < uniqueCount; ++j) {
            if (values[j] == delta) {
                ++counts[j];
                found = true;
                break;
            }
        }
        if (!found && uniqueCount < VF_NUM_POINTS) {
            values[uniqueCount] = delta;
            counts[uniqueCount] = 1;
            ++uniqueCount;
        }
    }
    if (consideredCount == 0) return 0;

    int bestValue = 0;
    int bestCount = 0;
    for (int i = 0; i < uniqueCount; ++i) {
        if (counts[i] > bestCount ||
            (counts[i] == bestCount &&
             vf_live_offset_abs64((long long)values[i]) >
             vf_live_offset_abs64((long long)bestValue))) {
            bestValue = values[i];
            bestCount = counts[i];
        }
    }
    if (bestCount * 2 < consideredCount) return 0;
    return bestValue;
}

static inline VfSelectiveGpuOffsetDetection vf_detect_live_selective_gpu_offset(
    const VFCurvePoint* curve, const int* freqOffsetsKHz,
    const VfFloorTopology* floorOpt, int ownedLockCi) {
    VfSelectiveGpuOffsetDetection out = {};
    out.tail = vf_offset_detection_tail_start(curve, freqOffsetsKHz, floorOpt, ownedLockCi);
    if (!curve || !freqOffsetsKHz) return out;

    static const int TOLERANCE_MHZ = 30;
    const long long toleranceKHz = (long long)TOLERANCE_MHZ * 1000LL;

    int candidateOffsets[VF_NUM_POINTS] = {};
    int candidateCount = 0;
    int numPopulated = 0;
    int nonTailPopulated = 0;

    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (curve[ci].freq_kHz == 0) continue;
        ++numPopulated;
        if (out.tail.tailStartCi >= 0 && ci >= out.tail.tailStartCi) continue;
        ++nonTailPopulated;
        const int offsetMHz = freqOffsetsKHz[ci] / 1000;
        if (offsetMHz == 0) continue;

        bool seen = false;
        for (int i = 0; i < candidateCount; ++i) {
            if (vf_live_offset_abs64((long long)candidateOffsets[i] - (long long)offsetMHz) <= TOLERANCE_MHZ) {
                seen = true;
                break;
            }
        }
        if (!seen && candidateCount < VF_NUM_POINTS) {
            candidateOffsets[candidateCount++] = offsetMHz;
        }
    }

    out.populatedCount = numPopulated;
    out.candidateCount = candidateCount;
    for (int i = 0; i < candidateCount && i < 5; ++i) {
        out.candidateSampleMHz[i] = candidateOffsets[i];
    }
    if (candidateCount == 0 || nonTailPopulated < 2) return out;

    int bestDetectedMHz = 0;
    int bestMatchedAverageKHz = 0;
    int bestExcludeLowCount = 0;
    int bestMatchCount = 0;
    int bestConsideredCount = 0;
    int bestViolations = INT_MAX;
    int bestExcludedTotal = 0;

    for (int excludeTry = 1; excludeTry < nonTailPopulated; ++excludeTry) {
        for (int candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
            const int candidateMHz = candidateOffsets[candidateIndex];
            const long long candidateTargetKHz = (long long)candidateMHz * 1000LL;
            bool sawExcludedPoint = false;
            bool sawIncludedPoint = false;
            bool skippedLockedTail = false;
            long long includedMatchSumKHz = 0;
            int includedMatchCount = 0;
            int includedConsideredCount = 0;
            int excludedViolations = 0;
            int excludedTotal = 0;
            int includedZeroGaps = 0;
            int firstIncludedMatchCi = -1;
            int lastIncludedMatchCi = -1;
            int populatedOrdinal = 0;

            for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
                if (curve[ci].freq_kHz == 0) continue;
                const int ordinal = populatedOrdinal++;

                if (out.tail.tailStartCi >= 0 && ci >= out.tail.tailStartCi) {
                    skippedLockedTail = true;
                    continue;
                }

                const bool excluded = ordinal < excludeTry;
                const long long actualOffsetKHz = (long long)freqOffsetsKHz[ci];
                if (excluded) {
                    sawExcludedPoint = true;
                    ++excludedTotal;
                    if (vf_live_offset_abs64(actualOffsetKHz) > toleranceKHz) {
                        ++excludedViolations;
                    }
                } else {
                    sawIncludedPoint = true;
                    ++includedConsideredCount;
                    if (vf_live_offset_abs64(actualOffsetKHz) <= toleranceKHz &&
                        vf_live_offset_abs64(candidateTargetKHz) > toleranceKHz) {
                        ++includedZeroGaps;
                    } else if (vf_live_offset_abs64(actualOffsetKHz - candidateTargetKHz) <= toleranceKHz) {
                        includedMatchSumKHz += actualOffsetKHz;
                        ++includedMatchCount;
                        if (firstIncludedMatchCi < 0) firstIncludedMatchCi = ci;
                        lastIncludedMatchCi = ci;
                    }
                }
            }

            if (!sawIncludedPoint || !sawExcludedPoint) continue;
            if (includedMatchCount == 0) continue;

            const int minimumMatches = skippedLockedTail ? 4 : 3;
            if (includedMatchCount < minimumMatches) continue;
            if (skippedLockedTail && firstIncludedMatchCi >= 0 &&
                lastIncludedMatchCi - firstIncludedMatchCi < 4) {
                continue;
            }
            if (includedMatchCount * 2 < includedConsideredCount) continue;
            if (includedZeroGaps > 0) continue;
            if (excludedTotal > 0 && excludedViolations * 3 > excludedTotal) continue;

            const long long matchedAverageKHz = includedMatchSumKHz / includedMatchCount;
            const long long detectedMHz =
                (matchedAverageKHz >= 0 ? (matchedAverageKHz + 500LL) : (matchedAverageKHz - 500LL)) / 1000LL;
            if (detectedMHz == 0) continue;
            // Reject residual noise at or below the tolerance threshold (30 MHz).
            if (vf_live_offset_abs64(detectedMHz) <= TOLERANCE_MHZ) continue;

            const bool isBetter = (includedMatchCount > bestMatchCount) ||
                (includedMatchCount == bestMatchCount && excludedViolations < bestViolations);
            if (isBetter) {
                bestDetectedMHz = (int)detectedMHz;
                bestMatchedAverageKHz = (int)matchedAverageKHz;
                bestExcludeLowCount = excludeTry;
                bestMatchCount = includedMatchCount;
                bestConsideredCount = includedConsideredCount;
                bestViolations = excludedViolations;
                bestExcludedTotal = excludedTotal;
            }
        }
    }

    if (bestExcludeLowCount > 0) {
        out.detected = true;
        out.gpuOffsetMHz = bestDetectedMHz;
        out.representativeOffsetKHz = bestMatchedAverageKHz;
        out.excludeLowCount = bestExcludeLowCount;
        out.matchCount = bestMatchCount;
        out.consideredCount = bestConsideredCount;
        out.excludedViolations = bestViolations;
        out.excludedTotal = bestExcludedTotal;
    }
    return out;
}

static inline bool vf_live_selective_gpu_offset_matches_shape(
    const VFCurvePoint* curve, const int* freqOffsetsKHz,
    const VfFloorTopology* floorOpt, int ownedLockCi,
    int gpuOffsetMHz, int excludeLowCount, int toleranceMHz,
    int* representativeOffsetKHzOut = nullptr,
    VfOffsetTailBoundary* tailOut = nullptr) {
    if (representativeOffsetKHzOut) *representativeOffsetKHzOut = 0;
    VfOffsetTailBoundary tail =
        vf_offset_detection_tail_start(curve, freqOffsetsKHz, floorOpt, ownedLockCi);
    if (tailOut) *tailOut = tail;
    if (!curve || !freqOffsetsKHz || gpuOffsetMHz == 0 || excludeLowCount <= 0) return false;
    if (toleranceMHz < 0) toleranceMHz = 0;

    const long long toleranceKHz = (long long)toleranceMHz * 1000LL;
    const long long targetOffsetKHz = (long long)gpuOffsetMHz * 1000LL;
    bool sawExcludedPoint = false;
    bool sawIncludedPoint = false;
    bool skippedLockedTail = false;
    long long includedMatchSumKHz = 0;
    int includedMatchCount = 0;
    int includedConsideredCount = 0;
    int excludedViolations = 0;
    int excludedTotal = 0;
    int includedZeroGaps = 0;
    int populatedOrdinal = 0;

    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (curve[ci].freq_kHz == 0) continue;
        const int ordinal = populatedOrdinal++;

        if (tail.tailStartCi >= 0 && ci >= tail.tailStartCi) {
            skippedLockedTail = true;
            continue;
        }

        const bool excluded = ordinal < excludeLowCount;
        const long long actualOffsetKHz = (long long)freqOffsetsKHz[ci];
        if (excluded) {
            sawExcludedPoint = true;
            ++excludedTotal;
            if (vf_live_offset_abs64(actualOffsetKHz) > toleranceKHz) ++excludedViolations;
        } else {
            sawIncludedPoint = true;
            ++includedConsideredCount;
            if (vf_live_offset_abs64(actualOffsetKHz) <= toleranceKHz &&
                vf_live_offset_abs64(targetOffsetKHz) > toleranceKHz) {
                ++includedZeroGaps;
            } else if (vf_live_offset_abs64(actualOffsetKHz - targetOffsetKHz) <= toleranceKHz) {
                includedMatchSumKHz += actualOffsetKHz;
                ++includedMatchCount;
            }
        }
    }

    if (!sawIncludedPoint || !sawExcludedPoint) return false;
    const int minimumMatches = skippedLockedTail ? 2 : 3;
    if (includedMatchCount < minimumMatches) return false;
    if (includedMatchCount * 2 < includedConsideredCount) return false;
    if (includedZeroGaps > 0) return false;
    if (excludedTotal > 0 && excludedViolations * 3 > excludedTotal) return false;

    if (representativeOffsetKHzOut && includedMatchCount > 0) {
        *representativeOffsetKHzOut = (int)(includedMatchSumKHz / includedMatchCount);
    }
    return true;
}

#endif  // GREEN_CURVE_VF_LIVE_OFFSET_POLICY_H
