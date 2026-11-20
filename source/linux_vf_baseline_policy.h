// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure policy for resetting a shaped live VF curve to stock baseline before
// rebuilding replacement curve targets on Linux, and for preparing TUI Apply
// requests cleanly.
//
// Why this exists: when a flattened profile is active on Blackwell/Ada
// (tail programmed to -1000 MHz and monotonically clamped to the plateau, and
// the neighbor below the raised anchor pulled up by driver cross-talk),
// `live - offset` over the shaped live curve produces distorted bases
// (e.g. ~3962 MHz on tail points, +120 MHz on the pre-anchor neighbor).
// Explicit-scope replacements (including legacy profiles upgraded after an
// explicit-scope profile) forbid global `resetOcBeforeApply`, so without a
// VF-only baseline reset under the armed transition ceiling, switching from a
// flattened profile to another curve/lock profile computes out-of-range or
// cross-talked targets.
#ifndef GREEN_CURVE_LINUX_VF_BASELINE_POLICY_H
#define GREEN_CURVE_LINUX_VF_BASELINE_POLICY_H

#include "gpu_core.h"
#include "linux_vf_rollback_mask_policy.h"

static inline int linux_vf_baseline_reset_mask(const VFCurvePoint* curve,
    const bool* writeMask, bool* resetMask,
    const unsigned char* editableMask = nullptr) {
    int count = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        const bool editable = !editableMask ||
            (editableMask[ci / 8] & (unsigned char)(1u << (ci % 8))) != 0;
        resetMask[ci] = curve[ci].freq_kHz != 0 && editable &&
            (!writeMask || writeMask[ci]);
        count += resetMask[ci] ? 1 : 0;
    }
    return count;
}

// True when any populated graphics VF point currently carries a nonzero
// programmed offset.
static inline bool linux_curve_has_nonzero_offsets(
    const VFCurvePoint* curve, const int* offsetsKHz,
    const gc_i64* offsetsHz, bool offsetsHzValid) {
    if (!curve) return false;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (curve[i].freq_kHz == 0) continue;
        if (offsetsHzValid && offsetsHz) {
            if (offsetsHz[i] != 0) return true;
        } else if (offsetsKHz && offsetsKHz[i] != 0) {
            return true;
        }
    }
    return false;
}

// True when a Linux Apply transaction should reset the VF curve to 0 under the
// armed transition clock ceiling before rebuilding targets in LINUX_MUTATION_CURVE.
//
// Never fires when:
// - `resetOcBeforeApply` is already set (LINUX_MUTATION_RESET_BASELINE handles it);
// - the request has no curve work;
// - the live curve is already at stock (all VF offsets == 0);
// - the request is a sparse update (UPDATE_INCLUDED without REPLACE_VF_POINTS)
//   or an explicit release, which must preserve unowned live VF points.
static inline bool linux_apply_needs_vf_baseline_reset(
    const DesiredSettings* d, bool hasCurveWork, bool liveCurveHasNonZeroOffsets) {
    if (!d || d->resetOcBeforeApply || !hasCurveWork || !liveCurveHasNonZeroOffsets)
        return false;
    if (desired_application_scope_kind(d->applicationScope) == DESIRED_SCOPE_REPLACE_PROFILE)
        return true;
    return (d->applicationScope & DESIRED_SCOPE_REPLACE_VF_POINTS) != 0;
}

// True when the TUI draft changed any graphics/VF/lock field compared to the
// last accepted live intent (pure fan/power/memory edits stay untouched).
static inline bool linux_tui_curve_or_lock_changed(
    const DesiredSettings* accepted, const DesiredSettings* draft) {
    if (!accepted || !draft) return false;
    if (draft->hasGpuOffset != accepted->hasGpuOffset) return true;
    if (draft->hasGpuOffset &&
        (draft->gpuOffsetMHz != accepted->gpuOffsetMHz ||
         draft->gpuOffsetExcludeLowCount != accepted->gpuOffsetExcludeLowCount))
        return true;
    if (draft->hasLock != accepted->hasLock) return true;
    if (draft->hasLock &&
        (draft->lockCi != accepted->lockCi ||
         draft->lockMHz != accepted->lockMHz ||
         draft->lockMode != accepted->lockMode))
        return true;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        const bool aPresent = desired_curve_point_present(accepted, ci);
        const bool dPresent = desired_curve_point_present(draft, ci);
        if (aPresent != dPresent) return true;
        if (!dPresent) continue;
        if (draft->curvePointMHz[ci] != accepted->curvePointMHz[ci] ||
            draft->curvePointFromGpuOffset[ci] != accepted->curvePointFromGpuOffset[ci] ||
            draft->curvePointCaptured[ci] != accepted->curvePointCaptured[ci] ||
            draft->curvePointCapturedOffsetHz[ci] != accepted->curvePointCapturedOffsetHz[ci])
            return true;
    }
    return false;
}

// Prepare a TUI Apply request for submission, matching Windows GUI parity:
// - Applying a loaded profile runs desired_prepare_replacement_request() so
//   legacy profiles reset their OC baseline and explicit UPDATE_INCLUDED slots
//   replace prior profile domains instead of leaking them.
// - Manually editing a legacy curve/lock/GPU-offset sets resetOcBeforeApply so
//   new curve targets are computed from a clean stock baseline, while pure
//   fan/power/memory edits leave OC state untouched.
// Returns true if the request was upgraded to a replacement/baseline reset.
static inline bool linux_tui_prepare_apply_request(
    bool loadedProfile, const DesiredSettings* accepted, DesiredSettings* request) {
    if (!request) return false;
    if (loadedProfile) {
        desired_prepare_replacement_request(request);
        return true;
    }
    if (request->applicationScope == DESIRED_SCOPE_LEGACY &&
        linux_tui_curve_or_lock_changed(accepted, request)) {
        request->resetOcBeforeApply = true;
        return true;
    }
    return false;
}

#endif  // GREEN_CURVE_LINUX_VF_BASELINE_POLICY_H
