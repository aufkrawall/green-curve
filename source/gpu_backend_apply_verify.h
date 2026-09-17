// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_VERIFY_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_VERIFY_H

// Re-evaluate the latest readback on every call, including correction passes.
// Matching a generic selective offset never excuses violating an explicit point.
// Derived MHz values are only a preview: the driver can change the base curve
// after a batch even with an unchanged (including zero) point offset.
static bool apply_verify_curve_targets(const DesiredSettings* desired,
    const bool* explicitMask, const int* targetOffsets,
    bool curveRequest, bool selective, bool hasLock,
    LockMode mode, const bool* tailMask, unsigned int lockMHz,
    char* detail, size_t detailSize) {
    if (!curveRequest) return true;
    // A point the request marks offset-derived carries offset intent, not
    // absolute intent, whatever routing this apply uses for the offset itself.
    // Its absolute MHz was projected over the stock base the driver reported at
    // some earlier moment -- profile save, or the last editor refresh -- and
    // under load the driver reports a different base for the same point (one
    // whole VF bin on Blackwell), so holding the readback to that number fails
    // an apply that is doing exactly what was asked.
    bool sawPoint = false;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        bool tail = hasLock && tailMask[ci];
        if (!desired->hasCurvePoint[ci] && !tail) continue;
        if (g_app.curve[ci].freq_kHz == 0) {
            set_message(detail, detailSize, "Requested VF point %d has no fresh readback", ci);
            return false;
        }
        sawPoint = true;
        // A HARD pin owns the tail frequency. The explicit pre-tail points still
        // own their own voltage/frequency targets and are checked below.
        if (tail && mode == LOCK_MODE_HARD) continue;
        const bool fromOffset = desired->curvePointFromGpuOffset[ci];
        if (!tail && !explicitMask[ci] && (selective || fromOffset)) {
            const int expected = targetOffsets[ci];
            const int actual = g_app.freqOffsets[ci];
            // Preserve delta intent, including excluded points and undervolts.
            // Lower driver-clamped offsets remain safe; a higher offset must
            // not be hidden by a temperature-dependent absolute MHz preview.
            if ((long long)actual > (long long)expected + 12000) {
                set_message(detail, detailSize,
                    "VF point %d offset verified at %d kHz above requested %d kHz",
                    ci, actual, expected);
                debug_log("curve offset verification failed: ci=%d actual=%d target=%d kHz reconstructed=%d selective=%d\n",
                    ci, actual, expected,
                    fromOffset ? 1 : 0, selective ? 1 : 0);
                return false;
            }
            if ((long long)actual < (long long)expected - 12000)
                debug_log("curve offset verification: ci=%d driver limited offset to %d kHz (requested %d kHz)\n",
                    ci, actual, expected);
            continue;
        }
        unsigned int target = tail ? lockMHz : desired->curvePointMHz[ci];
        unsigned int actual = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        unsigned int tolerance = curve_point_verify_tolerance_mhz(ci);
        unsigned int delta = actual > target ? actual - target : target - actual;
        if (delta > tolerance) {
            set_curve_target_mismatch_detail(ci, actual, target, tail, detail, detailSize);
            debug_log("curve verification failed: ci=%d actual=%u target=%u explicit=%d tail=%d reconstructed=%d selective=%d delta=%u tol=%u\n",
                ci, actual, target, explicitMask[ci] ? 1 : 0, tail ? 1 : 0,
                fromOffset ? 1 : 0, selective ? 1 : 0, delta, tolerance);
            return false;
        }
    }
    if (!sawPoint) set_message(detail, detailSize, "No requested VF points could be verified");
    return sawPoint;
}
#endif
