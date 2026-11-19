// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GUI_OBSERVED_LOCK_POLICY_H
#define GREEN_CURVE_GUI_OBSERVED_LOCK_POLICY_H

// The Windows editor's projection of a VF flatten another tool programmed.
//
// The editor shows the live curve; a flattened tail is shown the way a Green
// Curve flatten is: lock checkbox on the anchor, tail fields greyed, the
// plateau as the lock target.  The projection is editor state only:
//  - ownership stays with the service (gpu_lock_ownership_policy.h): the
//    service never adopts the tail, and the snapshot reports no lock for it;
//  - the projected lock is mirrored into the editor's applied-lock fields, so
//    it is not a pending change and a sparse power/fan Apply leaves the curve
//    alone;
//  - a full Apply sends the editor's lock like any other (it resets offsets
//    first, so the projection is what keeps the flatten the editor shows);
//  - Save labels it as a flatten another tool set and offers Import
//    (profile_save_policy.h).
// It never touches an editor with unsaved edits, and it is withdrawn when the
// flatten leaves the GPU or Green Curve's own lock takes over.

#include "vf_flat_tail_recognition.h"

struct GuiObservedLock {
    bool valid = false;
    int ci = -1;
    int vi = -1;
    unsigned int mhz = 0;
    int tailCount = 0;
    bool exact = false;
};

static inline bool gui_observed_lock_same(const GuiObservedLock& a, const GuiObservedLock& b) {
    return a.valid == b.valid && (!a.valid || (a.ci == b.ci && a.mhz == b.mhz));
}

// A flat tail counts as programmed only when some tail point carries a
// programmed offset other than the anchor's: a stock curve whose top bins are
// already level (zero offsets, or one uniform VF-table offset) is not a lock.
// The anchor must be listed in the editor with at least one listed point
// after it, or there is no tail to grey.
static inline GuiObservedLock gui_observed_lock_recognize(
    const VFCurvePoint* curve, const int* offsetsKHz, const VfFloorTopology& floor,
    const int* visibleMap, int numVisible) {
    GuiObservedLock out;
    if (!curve || !offsetsKHz || !visibleMap || numVisible < 2) return out;
    const VfFlatTailRecognition rec = vf_recognize_flat_tail(curve, floor, 1, 3);
    if (!rec.found || rec.anchorCi < 0 || rec.anchorCi >= VF_NUM_POINTS || rec.plateauMHz == 0)
        return out;
    int vi = -1;
    for (int i = 0; i < numVisible; ++i) {
        if (visibleMap[i] == rec.anchorCi) { vi = i; break; }
    }
    if (vi < 0 || vi >= numVisible - 1) return out;
    const int end = floor.graphicsEnd < VF_NUM_POINTS ? floor.graphicsEnd : VF_NUM_POINTS;
    bool programmed = false;
    for (int j = rec.anchorCi + 1; j < end && !programmed; ++j) {
        if (curve[j].freq_kHz == 0) continue;
        programmed = offsetsKHz[j] != offsetsKHz[rec.anchorCi];
    }
    if (!programmed) return out;
    out.valid = true;
    out.ci = rec.anchorCi;
    out.vi = vi;
    out.mhz = rec.plateauMHz;
    out.tailCount = rec.tailCount;
    out.exact = rec.exact;
    return out;
}

enum GuiObservedLockAction {
    GUI_OBSERVED_LOCK_NONE = 0,  // leave the editor and the remembered projection alone
    GUI_OBSERVED_LOCK_ADOPT,     // project `recognized` into the editor lock and applied mirror
    GUI_OBSERVED_LOCK_KEEP,      // the editor already shows `recognized`
    GUI_OBSERVED_LOCK_CLEAR,     // the flatten left the GPU: withdraw the projection
    GUI_OBSERVED_LOCK_FORGET,    // drop the remembered projection, editor untouched
};

struct GuiObservedLockInput {
    bool snapshotLoaded = false;
    bool serviceReportsLock = false;  // Green Curve's own lock (or its retained markers)
    bool editorDirty = false;
    int editorLockCi = -1;
    unsigned int editorLockMHz = 0;
    int editorLockMode = 0;
    int appliedLockCi = -1;
    unsigned int appliedLockMHz = 0;
    int appliedLockMode = 0;
    GuiObservedLock projected;   // what this editor last projected
    GuiObservedLock recognized;  // what the live curve shows now
};

static inline bool gui_observed_lock_editor_matches(int ci, unsigned int mhz, int mode,
                                                    const GuiObservedLock& lock) {
    return lock.valid && ci == lock.ci && mhz == lock.mhz && mode == LOCK_MODE_FLATTEN;
}

// The editor lock is the projection (not a user edit or a loaded profile).
static inline bool gui_observed_lock_editor_is_projection(const GuiObservedLockInput& in) {
    return gui_observed_lock_editor_matches(in.editorLockCi, in.editorLockMHz, in.editorLockMode, in.projected) &&
           gui_observed_lock_editor_matches(in.appliedLockCi, in.appliedLockMHz, in.appliedLockMode, in.projected);
}

static inline GuiObservedLockAction gui_observed_lock_decide(const GuiObservedLockInput& in) {
    if (!in.snapshotLoaded) return GUI_OBSERVED_LOCK_NONE;
    // Green Curve owns a lock: the service snapshot path adopts it.
    if (in.serviceReportsLock) return in.projected.valid ? GUI_OBSERVED_LOCK_FORGET : GUI_OBSERVED_LOCK_NONE;
    // Never rewrite a draft; the next clean snapshot decides again.
    if (in.editorDirty) return GUI_OBSERVED_LOCK_NONE;
    const bool editorLock = in.editorLockCi >= 0 && in.editorLockMHz > 0;
    const bool appliedLock = in.appliedLockCi >= 0 && in.appliedLockMHz > 0;
    const bool projection = gui_observed_lock_editor_is_projection(in);
    // A lock the editor shows that the applied fields do not hold is a loaded
    // or typed one; only a projection or an adopted (applied == editor) lock
    // may be replaced.
    const bool adoptedLock = editorLock && appliedLock && in.editorLockCi == in.appliedLockCi &&
                             in.editorLockMHz == in.appliedLockMHz && in.editorLockMode == in.appliedLockMode;
    const bool appliedIsStaleProjection = !editorLock &&
        gui_observed_lock_editor_matches(in.appliedLockCi, in.appliedLockMHz, in.appliedLockMode, in.projected);
    if (in.recognized.valid) {
        if (projection && gui_observed_lock_same(in.projected, in.recognized)) return GUI_OBSERVED_LOCK_KEEP;
        if (!editorLock || adoptedLock || projection) return GUI_OBSERVED_LOCK_ADOPT;
        return in.projected.valid ? GUI_OBSERVED_LOCK_FORGET : GUI_OBSERVED_LOCK_NONE;
    }
    if (projection || appliedIsStaleProjection) return GUI_OBSERVED_LOCK_CLEAR;
    return in.projected.valid ? GUI_OBSERVED_LOCK_FORGET : GUI_OBSERVED_LOCK_NONE;
}

static inline const char* gui_observed_lock_action_name(GuiObservedLockAction action) {
    switch (action) {
        case GUI_OBSERVED_LOCK_NONE: return "none";
        case GUI_OBSERVED_LOCK_ADOPT: return "adopt";
        case GUI_OBSERVED_LOCK_KEEP: return "keep";
        case GUI_OBSERVED_LOCK_CLEAR: return "clear";
        case GUI_OBSERVED_LOCK_FORGET: return "forget";
    }
    return "unknown";
}

#endif  // GREEN_CURVE_GUI_OBSERVED_LOCK_POLICY_H
