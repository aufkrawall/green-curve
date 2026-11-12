// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Profile Save's capture source.  The old rule saved live hardware defaults
// whenever the user had not typed anything, which was correct for a stock GPU
// but silently dropped an applied pinned/curved profile from the slot and
// cleared the tray tick.  Applied curve/lock intent is real intent, not drift,
// so it must use the GUI capture too.
//
// The no-edit branch (build_full_live_desired_settings()) stays a documented
// partial live save: it copies the scalar domains (GPU/memory offsets, power,
// fan, auxiliary clocks) from the effective control state and deliberately
// saves NO curve points and NO lock -- a flattened outgoing tail saved as
// points would be re-inferred as a lock on Load.  What it must not do is
// report plain success over a VF curve another writer programmed: the slot
// then silently lacks the curve the editor was showing.  The helpers below
// count what such a save omits, so the handler can say so.

#ifndef GREEN_CURVE_PROFILE_SAVE_POLICY_H
#define GREEN_CURVE_PROFILE_SAVE_POLICY_H

#include "gpu_core.h"

static inline bool profile_save_uses_gui_capture(
    bool userModified, bool pendingCurveOrLock, bool appliedCurveOrLock) {
    return userModified || pendingCurveOrLock || appliedCurveOrLock;
}

// Populated VF points whose programmed offset the saved request does not
// explain.  `savedComponentKHz[ci]` is the saved GPU offset's per-point
// component (zero where the save carries no GPU offset or excludes the point),
// so a Green Curve GPU offset written through the VF table is not counted.
static inline int profile_save_count_unsaved_programmed_points(
    const VFCurvePoint* curve, const int* offsetsKHz, const int* savedComponentKHz,
    int* firstCiOut) {
    if (firstCiOut) *firstCiOut = -1;
    if (!curve || !offsetsKHz) return 0;
    int count = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (curve[ci].freq_kHz == 0) continue;
        const int component = savedComponentKHz ? savedComponentKHz[ci] : 0;
        // Zero is stock (a GPU offset on the separate NVML route leaves the VF
        // table at zero); the saved component explains a VF-table GPU offset.
        if (offsetsKHz[ci] == 0 || offsetsKHz[ci] == component) continue;
        if (count == 0 && firstCiOut) *firstCiOut = ci;
        ++count;
    }
    return count;
}

// Pre-anchor points a saved lock carries as offsets kept from the curve on
// the GPU rather than typed or projected from the saved GPU offset (the GUI
// capture's pre-tail inference).
static inline int profile_save_count_inferred_pre_anchor_points(
    const DesiredSettings* saved, const int* savedComponentKHz) {
    if (!saved || !saved->hasLock || saved->lockCi < 0 || saved->lockCi >= VF_NUM_POINTS)
        return 0;
    int count = 0;
    for (int ci = 0; ci < saved->lockCi; ++ci) {
        if (!saved->hasCurvePoint[ci] || !saved->curvePointFromGpuOffset[ci]) continue;
        if (savedComponentKHz && savedComponentKHz[ci] != 0) continue;
        ++count;
    }
    return count;
}

enum ProfileSaveNotice {
    PROFILE_SAVE_NOTICE_NONE = 0,
    // No-edit save over a curve Green Curve did not set: the slot omits it.
    PROFILE_SAVE_NOTICE_OMITTED_EXTERNAL_CURVE = 1,
    // The saved lock came from live flat-tail detection over a curve Green
    // Curve does not own, with pre-anchor offsets inferred from the GPU: a
    // detected approximation, not a reviewed capture.
    PROFILE_SAVE_NOTICE_DETECTED_APPROXIMATION = 2,
};

// `lockOwnedByGreenCurve`: the service's active intent holds this lock.
// `lockFromUserEdit`: the lock is a pending editor change.
static inline ProfileSaveNotice profile_save_notice(bool usedGuiCapture,
    int unsavedProgrammedPoints, const DesiredSettings* saved,
    bool lockOwnedByGreenCurve, bool lockFromUserEdit, int inferredPreAnchorPoints) {
    if (!usedGuiCapture)
        return unsavedProgrammedPoints > 0 ? PROFILE_SAVE_NOTICE_OMITTED_EXTERNAL_CURVE
                                           : PROFILE_SAVE_NOTICE_NONE;
    if (saved && saved->hasLock && !lockOwnedByGreenCurve && !lockFromUserEdit &&
        inferredPreAnchorPoints > 0)
        return PROFILE_SAVE_NOTICE_DETECTED_APPROXIMATION;
    return PROFILE_SAVE_NOTICE_NONE;
}

static inline const char* profile_save_notice_name(ProfileSaveNotice notice) {
    switch (notice) {
        case PROFILE_SAVE_NOTICE_NONE: return "none";
        case PROFILE_SAVE_NOTICE_OMITTED_EXTERNAL_CURVE: return "omitted-external-curve";
        case PROFILE_SAVE_NOTICE_DETECTED_APPROXIMATION: return "detected-approximation";
        default: return "unknown";
    }
}

#endif  // GREEN_CURVE_PROFILE_SAVE_POLICY_H
