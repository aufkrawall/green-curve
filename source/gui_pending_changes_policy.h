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
    GUI_PENDING_XBAR        = 1u << 9,
    GUI_PENDING_SYS_CLK     = 1u << 10,
    GUI_PENDING_VIDEO_CLK   = 1u << 11,
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
    // appliedMHz also carries the APPLIED ownership half: 0 means the applied
    // state does not own this point. Callers fold an applied HARD NVML pin
    // into it, because a hard pin owns every visible point even when the
    // applied curve baseline has no explicit entry there.
    unsigned int appliedMHz;
};

static inline bool gui_pending_curve_point_changed(GuiPendingCurvePoint in) {
    if (!in.visible || !in.liveKnown) return false;
    // A point the APPLIED state owns but the editor does not is released back
    // to stock by Apply's reset-before-apply step, so the release is a change
    // even though the editor typed nothing. appliedMHz != 0 is the applied
    // ownership half.
    if (!in.ownedByEditor) return in.appliedMHz != 0;
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
// The carve-outs match gpu_backend_apply.cpp's two offset paths:
//   - a UNIFORM global offset (neither side excludes low points) is written
//     through the dedicated offset control, and a point the editor owns
//     explicitly is then written as an ABSOLUTE target, so the uniform offset
//     does not stack onto it;
//   - a SELECTIVE offset (either side has exclude-low active) is applied
//     through per-point VF deltas in the curve batch, which re-places every
//     populated point (gpuPolicyViaCurveBatch) regardless of
//     guiCurvePointExplicit -- so an applied profile whose decoded curve
//     points are all "owned" still has to preview the move;
//   - the locked tail is pinned to the lock target regardless of any offset.
//
// Comparing the offset COMPONENTS rather than frequencies keeps this exact and
// drift-free.
struct GuiPendingOffsetShift {
    bool inLockedTail;
    bool ownedByEditor;
    bool selectiveOffsetActive;
    int appliedOffsetComponentMHz;
    int pendingOffsetComponentMHz;
};

static inline bool gui_pending_offset_shift_changed(GuiPendingOffsetShift in) {
    if (in.inLockedTail) return false;
    if (in.ownedByEditor && !in.selectiveOffsetActive) return false;
    return in.appliedOffsetComponentMHz != in.pendingOffsetComponentMHz;
}

// Whether the GPU-offset apply path is SELECTIVE (per-point VF deltas through
// the curve batch) rather than the dedicated uniform offset control.  This is
// the pure mirror of gpu_backend_apply.cpp's gpuPolicyViaCurveBatch gate:
//   - the APPLIED side is the resolver's effective exclude count
//     (current_applied_gpu_offset_excludes_low_points()), which already gates
//     on a nonzero applied offset at every normal source.  Re-gating it here
//     would diverge in the remembered-request edge where the resolver still
//     reports an exclude count with a zero offset;
//   - the PENDING side mirrors the backend's desired-settings normalization
//     (desired->gpuOffsetExcludeLowCount counts only while
//     desired->gpuOffsetMHz != 0).
// The applied offset parameter is part of the contract for the caller's
// benefit; the applied exclude count is already the effective value.
static inline bool gui_pending_offset_mode_selective(
    int appliedGpuOffsetMHz, int appliedExcludeLowCount,
    int pendingGpuOffsetMHz, int pendingExcludeLowCount) {
    (void)appliedGpuOffsetMHz;
    int appliedExcludeLow = appliedExcludeLowCount;
    int pendingExcludeLow =
        pendingGpuOffsetMHz != 0 ? pendingExcludeLowCount : 0;
    return appliedExcludeLow > 0 || pendingExcludeLow > 0;
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
// only afterwards.  (The disabled tail boxes are synced to the resolved target
// by the pending refresh, so an anchor retype no longer leaves stale numbers
// on screen.)
//
// A lock that TRACKS its anchor is not an absolute target: capture also adds
// the anchor's global GPU-offset component delta (desired minus current) to
// the base, because Apply re-places a tracking anchor at stock + the offset
// component.  The preview used to skip that step, so a freshly ticked flatten
// lock previewed the tail at the stock anchor while Apply wrote anchor +
// offset.  tracksAnchor=false means an absolute lock (anchor retyped or a
// persisted absolute profile): the base is written as-is.  The clamp to 1
// mirrors capture's `if (effectiveLockTargetMHz <= 0) effectiveLockTargetMHz =
// 1;`.
static inline unsigned int gui_pending_lock_target_mhz(
    bool draftAttached, bool draftValidAtAnchor, unsigned int draftMHzAtAnchor,
    unsigned int lockedFreqMHz, bool tracksAnchor,
    int appliedOffsetComponentMHz, int pendingOffsetComponentMHz) {
    unsigned int base = draftAttached && draftValidAtAnchor
        ? draftMHzAtAnchor : lockedFreqMHz;
    if (!tracksAnchor) return base;
    if (appliedOffsetComponentMHz == pendingOffsetComponentMHz) return base;
    long long target = (long long)base + (long long)pendingOffsetComponentMHz
        - (long long)appliedOffsetComponentMHz;
    if (target <= 0) target = 1;
    return (unsigned int)target;
}

// What the graph plots for one point, reduced to the half the EDITOR
// determines.  Resolved in the same order as gpu_backend_apply.cpp:
//
//   locked tail   -> the lock target, which capture_gui_desired_settings()
//                    expands over the tail;
//   point whose global-offset component moved and that the offset path will
//                    re-place -> stock base plus the PENDING component.  The
//                    applied component is still baked into the live readback,
//                    so the finished frequency cannot be reused for such a
//                    point.  A uniform offset leaves editor-owned points
//                    alone; a selective offset re-places them too
//                    (offsetMovesOwnedPoints).
//   owned point   -> the editor's absolute value, for the uniform-offset case
//                    where the global offset does not stack onto an
//                    explicitly written point (a selective offset has already
//                    answered above);
//   anything else -> nothing the editor determines; live readback stands.
//
// The live half is deliberately NOT part of this record, and that is the whole
// point of it: the graph's repaint gate compares two of these to learn whether
// the editor would draw a different pending curve.  g_app.curve drifts under
// boost and temperature and the live snapshot path already repaints for it, so
// folding the finished frequency in here would repaint the graph on every
// telemetry tick.
//
// Gating that repaint on the pending MASK and the changed-point SET instead --
// what this replaces -- missed every edit that moves values without changing
// WHICH points are pending.  Retyping the global GPU offset from +100 to +150
// moves every unowned point but marks exactly the same set, so the preview
// appeared only once something else (a manual Refresh) repainted the window.
// Changing the exclude-low count does move the set, which is why that one field
// looked like it worked.
struct GuiGraphPreviewPoint {
    unsigned int absoluteMHz;    // the editor asserts this exact frequency
    bool offsetFromStockBase;    // else: stock base + offsetComponentMHz
    int offsetComponentMHz;
};

struct GuiGraphPreviewInput {
    bool hardPinned;             // hard NVML pin: every plotted point is flat
    bool releasedToStock;        // applied owns this point; the editor does not
    bool inLockedTail;
    unsigned int lockTargetMHz;
    bool ownedByEditor;
    bool offsetMovesOwnedPoints;
    bool offsetProjectionValid;
    int appliedOffsetComponentMHz;
    int pendingOffsetComponentMHz;
    bool draftValid;
    unsigned int draftMHz;
};

static inline GuiGraphPreviewPoint gui_graph_preview_point(
    GuiGraphPreviewInput in) {
    GuiGraphPreviewPoint out = {};
    // A hard NVML pin makes the whole curve run at the lock target regardless
    // of the VF entries below the anchor, so it outranks the locked tail, the
    // offset projection, and the draft.  A lock target of 0 is unresolved and
    // falls through exactly like the tail case below.
    if (in.hardPinned && in.lockTargetMHz > 0) {
        out.absoluteMHz = in.lockTargetMHz;
        return out;
    }
    // A lock target of 0 is an unresolved lock, not a request to plot 0 MHz:
    // fall through to the offset/draft resolution exactly as the graph did.
    if (in.inLockedTail && in.lockTargetMHz > 0) {
        out.absoluteMHz = in.lockTargetMHz;
        return out;
    }
    if (in.offsetProjectionValid &&
        (in.offsetMovesOwnedPoints || !in.ownedByEditor) &&
        in.appliedOffsetComponentMHz != in.pendingOffsetComponentMHz) {
        out.offsetFromStockBase = true;
        out.offsetComponentMHz = in.pendingOffsetComponentMHz;
        return out;
    }
    // A point the applied state owns but the editor no longer does is released
    // back to stock by reset-before-apply. The stale draft still holds the old
    // applied value, so this must outrank the draft; when an offset projection
    // is valid the stock base carries the PENDING component for the point.
    if (in.releasedToStock && !in.ownedByEditor) {
        out.offsetFromStockBase = true;
        out.offsetComponentMHz =
            in.offsetProjectionValid ? in.pendingOffsetComponentMHz : 0;
        return out;
    }
    if (in.draftValid) out.absoluteMHz = in.draftMHz;
    return out;
}

// Field-wise, never memcmp: the record has padding between its bool and its
// int, and a repaint gate that compared padding would fire at random.
static inline bool gui_graph_preview_point_equal(GuiGraphPreviewPoint a,
                                                 GuiGraphPreviewPoint b) {
    return a.absoluteMHz == b.absoluteMHz
        && a.offsetFromStockBase == b.offsetFromStockBase
        && a.offsetComponentMHz == b.offsetComponentMHz;
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
