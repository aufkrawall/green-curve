// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_GUI_PENDING_CHANGES_POLICY_H
#define GREEN_CURVE_GUI_PENDING_CHANGES_POLICY_H

// F-PENDING: pure per-domain "typed but not applied yet" decision.
//
// The GDI main window paints every field whose pending value differs from the
// applied one in COL_PENDING and greys the Apply button when nothing differs.
// Both consumers read the SAME summary produced here, so the button state and
// the colours can never contradict each other.
//
// Two rules make this safe to drive a disabled Apply button:
//
//   1. Unparseable draft text ALWAYS counts as a pending change.  A half-typed
//      or garbage value must never grey out Apply -- the user has to be able to
//      press it and get the real parse error from capture_gui_apply_settings(),
//      which stays the authoritative backstop.
//   2. Curve points compare against drift-free applied INTENT (appliedCurveMHz,
//      0 == the applied state does not own that point), never against live
//      readback.  Expected boost/temperature drift in the live curve would
//      otherwise light points orange on its own and repaint on every telemetry
//      tick.
//
// Kept free of Win32 so the regression harness can pin every branch without
// creating a window.

enum {
    GUI_PENDING_CURVE       = 1u << 0,
    GUI_PENDING_LOCK        = 1u << 1,
    GUI_PENDING_GPU_OFFSET  = 1u << 2,
    GUI_PENDING_GPU_EXCLUDE = 1u << 3,
    GUI_PENDING_MEM_OFFSET  = 1u << 4,
    GUI_PENDING_POWER_LIMIT = 1u << 5,
    GUI_PENDING_FAN_MODE    = 1u << 6,
    GUI_PENDING_FAN_FIXED   = 1u << 7,
    GUI_PENDING_FAN_CURVE   = 1u << 8,
};

// Any field the editor holds as text: GPU offset, exclude-low count, memory
// offset, power limit, fixed fan percent.
struct GuiPendingScalar {
    bool draftValid;
    int draftValue;
    int appliedValue;
};

static inline bool gui_pending_scalar_changed(GuiPendingScalar in) {
    if (!in.draftValid) return true;
    return in.draftValue != in.appliedValue;
}

// ownedByEditor mirrors g_app.guiCurvePointExplicit[ci]: the editor asserts
// this point rather than merely displaying live readback.  liveKnown mirrors
// g_app.curve[ci].freq_kHz != 0, matching the same guard the apply diff uses.
struct GuiPendingCurvePoint {
    bool visible;
    bool ownedByEditor;
    bool liveKnown;
    bool draftValid;
    unsigned int draftMHz;
    unsigned int appliedMHz;
};

static inline bool gui_pending_curve_point_changed(GuiPendingCurvePoint in) {
    if (!in.visible || !in.ownedByEditor || !in.liveKnown) return false;
    if (!in.draftValid) return true;
    // No applied baseline means the editor owns a point the hardware does not,
    // which is exactly what a freshly typed or freshly loaded point looks like.
    if (in.appliedMHz == 0) return true;
    return in.draftMHz != in.appliedMHz;
}

// A change to the GLOBAL GPU offset moves every point it is not excluded from,
// even though no point was typed and none is owned by the editor.  Apply
// re-places such a point at stock + the new offset component
// (gpu_backend_apply.cpp), so it is pending exactly when the component moves.
//
// Two carve-outs, both matching the apply path:
//   - a point the editor owns explicitly is written as an ABSOLUTE target, so
//     the global offset does not stack onto it;
//   - the locked tail is pinned to the lock target regardless of any offset.
//
// Comparing the offset COMPONENTS rather than frequencies keeps this exact and
// drift-free.
struct GuiPendingOffsetShift {
    bool inLockedTail;
    bool ownedByEditor;
    int appliedOffsetComponentMHz;
    int pendingOffsetComponentMHz;
};

static inline bool gui_pending_offset_shift_changed(GuiPendingOffsetShift in) {
    if (in.inLockedTail) return false;
    if (in.ownedByEditor) return false;
    return in.appliedOffsetComponentMHz != in.pendingOffsetComponentMHz;
}

// toleranceMHz is the VF grid step at the lock point
// (curve_point_verify_tolerance_mhz()).  The GPU can only represent frequencies
// on its own grid, so a requested lock target and the value the hardware then
// reports for it routinely differ by one step -- a profile asking for 2957 MHz
// lands on 2962 and stays there.  The apply path already treats that as on
// target and keeps the requested value; comparing exactly here instead left the
// editor permanently dirty, because re-applying the profile can never close a
// gap the hardware cannot represent.
struct GuiPendingLock {
    bool draftActive;
    int draftCi;
    unsigned int draftMHz;
    int draftMode;
    bool appliedActive;
    int appliedCi;
    unsigned int appliedMHz;
    int appliedMode;
    unsigned int toleranceMHz;
};

static inline bool gui_pending_lock_changed(GuiPendingLock in) {
    if (in.draftActive != in.appliedActive) return true;
    if (!in.draftActive) return false;
    if (in.draftCi != in.appliedCi) return true;
    if (in.draftMode != in.appliedMode) return true;
    unsigned int lo = in.draftMHz < in.appliedMHz ? in.draftMHz : in.appliedMHz;
    unsigned int hi = in.draftMHz < in.appliedMHz ? in.appliedMHz : in.draftMHz;
    return (hi - lo) > in.toleranceMHz;
}

// The lock target the editor actually holds.
//
// capture_gui_desired_settings() prefers the draft value at the anchor over
// g_app.lockedFreq whenever a draft is attached, so that is what Apply will
// write and what the preview and the diff must both use. lockedFreq can lag:
// apply_lock() infers it from the draft, which a profile projection populates
// only afterwards, and the tail edit controls deliberately keep their previous
// readback when the anchor is retyped.
static inline unsigned int gui_pending_lock_target_mhz(
    bool draftAttached, bool draftValidAtAnchor, unsigned int draftMHzAtAnchor,
    unsigned int lockedFreqMHz) {
    if (draftAttached && draftValidAtAnchor) return draftMHzAtAnchor;
    return lockedFreqMHz;
}

struct GuiPendingSummary {
    unsigned int domainMask;
    int changedPointCount;
};

static inline bool gui_pending_any(const GuiPendingSummary* summary) {
    return summary && summary->domainMask != 0u;
}

static inline bool gui_pending_domain_set(const GuiPendingSummary* summary,
                                          unsigned int mask) {
    return summary && (summary->domainMask & mask) != 0u;
}

// mutationReady carries the existing service/draft gate (coherent READY model,
// attached and non-detached draft).  Pending changes only ever REMOVE the
// button, never add it back on top of a blocked state.
static inline bool gui_pending_apply_button_enabled(
    bool mutationReady, const GuiPendingSummary* summary) {
    return mutationReady && gui_pending_any(summary);
}

static inline bool gui_pending_summary_equal(const GuiPendingSummary* a,
                                             const GuiPendingSummary* b) {
    if (!a || !b) return a == b;
    return a->domainMask == b->domainMask
        && a->changedPointCount == b->changedPointCount;
}

// Graph presentation: the pending curve is drawn only across the stretches that
// actually differ from the applied one.  Overlaying the complete pending curve
// hides the applied curve everywhere and makes an edit to two points look like a
// change to all of them.
//
// A run is expanded by one point on each side so the dashed line visibly departs
// from and rejoins the applied curve: the pending and applied values are equal
// at those neighbours, so the two curves meet exactly there.
struct GuiPendingRun {
    bool valid;
    int drawFirst;   // inclusive, already expanded by one neighbour
    int drawLast;    // inclusive, already expanded by one neighbour
    int nextScan;    // where the caller resumes scanning
};

static inline GuiPendingRun gui_pending_next_changed_run(const bool* changed,
                                                         int count, int from) {
    GuiPendingRun run = {};
    run.nextScan = count > 0 ? count : 0;
    if (!changed || count <= 0 || from < 0 || from >= count) return run;
    int i = from;
    while (i < count && !changed[i]) i++;
    if (i >= count) return run;
    int start = i;
    while (i < count && changed[i]) i++;
    int end = i - 1;
    run.valid = true;
    run.drawFirst = start > 0 ? start - 1 : start;
    run.drawLast = end + 1 < count ? end + 1 : end;
    run.nextScan = i;
    return run;
}

#endif // GREEN_CURVE_GUI_PENDING_CHANGES_POLICY_H
