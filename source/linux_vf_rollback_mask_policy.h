// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Pure VF rollback mask builder: intersects captured snapshot masks with the
// live editable vfMask and preserves benign zero-readback tolerance for
// sub-450 MHz idle placeholders during rollback.
#ifndef GREEN_CURVE_LINUX_VF_ROLLBACK_MASK_POLICY_H
#define GREEN_CURVE_LINUX_VF_ROLLBACK_MASK_POLICY_H

template <typename CurvePoint>
static inline int linux_vf_rollback_masks(const CurvePoint* curve,
    const unsigned char* editableMask, const bool* capturedMask,
    const long long* capturedHz, bool offsetsExact,
    bool* restoreMask, bool* exactMask, int* skippedUnavailableOut = nullptr,
    int numPoints = 128, unsigned int placeholderMaxKHz = 450000u) {
    int count = 0;
    int skipped = 0;
    for (int ci = 0; ci < numPoints; ++ci) {
        const bool want = capturedMask && capturedMask[ci];
        const bool editable = !editableMask ||
            (editableMask[ci / 8] & (unsigned char)(1u << (ci % 8))) != 0;
        const bool usable = want && curve && curve[ci].freq_kHz != 0 && editable;
        if (want && !usable) ++skipped;
        if (restoreMask) restoreMask[ci] = usable;
        if (exactMask) {
            const bool placeholderNonZeroWholeKHz = usable && offsetsExact &&
                curve[ci].freq_kHz <= placeholderMaxKHz &&
                capturedHz && capturedHz[ci] != 0 && (capturedHz[ci] % 1000LL) == 0;
            exactMask[ci] = usable && offsetsExact && !placeholderNonZeroWholeKHz;
        }
        if (usable) ++count;
    }
    if (skippedUnavailableOut) *skippedUnavailableOut = skipped;
    return count;
}

#endif  // GREEN_CURVE_LINUX_VF_ROLLBACK_MASK_POLICY_H
