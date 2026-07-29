// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// F-PENDING: the GUI-side half of the "typed but not applied yet" presentation.
//
// gui_pending_changes_policy.h owns the pure per-domain decisions; this shard
// feeds them from AppData, caches the result, and turns a genuine change into
// the minimum presentation work: repaint exactly the controls whose state
// flipped, repaint the graph only when the curve or the lock moved, and
// re-evaluate the Apply enable.  Nothing here reads HWND text -- the pending
// state comes from GuiDraft, the same source the graph projects from.
//
// Every comparison is against drift-free APPLIED INTENT (appliedCurveMHz,
// appliedLock*, appliedGpuOffset*, the merged ControlState), never against live
// readback, because the NVIDIA VF curve legitimately shifts under boost and
// temperature.  Comparing against g_app.curve would light points orange on
// their own and repaint the window on every telemetry tick.

struct GuiPendingChanges {
    GuiPendingSummary summary;
    bool curvePoint[VF_NUM_POINTS];
    // Resolved global GPU-offset state. The graph projects the pending curve
    // through exactly these numbers, so the dashed line and the orange markers
    // can never disagree about where an offset-moved point lands.
    bool gpuOffsetValid;
    int appliedGpuOffsetMHz;
    int appliedGpuOffsetExcludeLowCount;
    int pendingGpuOffsetMHz;
    int pendingGpuOffsetExcludeLowCount;
};

static GuiPendingChanges g_guiPendingChanges;

// Draft text that does not parse always counts as pending.  Apply must stay
// reachable while a field is half-typed so the user gets the real validation
// error from capture_gui_apply_settings() instead of a greyed-out button.
static GuiPendingScalar gui_pending_scalar_from_draft(const char* draftText,
                                                      int appliedValue) {
    GuiPendingScalar scalar = {};
    int parsed = 0;
    scalar.draftValid = draftText && parse_int_strict(draftText, &parsed);
    scalar.draftValue = scalar.draftValid ? parsed : 0;
    scalar.appliedValue = appliedValue;
    return scalar;
}

static bool gui_pending_point_in_locked_tail(int vi) {
    return g_app.lockedVi >= 0 && vi >= g_app.lockedVi;
}

// Resolved once here so the diff, the graph's pending tail, and Apply itself all
// agree on the lock target.
static unsigned int gui_editor_lock_target_mhz() {
    int ci = g_app.lockedCi;
    bool validAtAnchor = ci >= 0 && ci < VF_NUM_POINTS &&
        g_app.guiDraft.curveValueValid[ci];
    return gui_pending_lock_target_mhz(g_app.guiDraft.attached, validAtAnchor,
        validAtAnchor ? g_app.guiDraft.curveMHz[ci] : 0u, g_app.lockedFreq);
}

static void gui_pending_evaluate_curve(GuiPendingChanges* out) {
    for (int vi = 0; vi < g_app.numVisible; ++vi) {
        int ci = g_app.visibleMap[vi];
        if (ci < 0 || ci >= VF_NUM_POINTS) continue;
        GuiPendingCurvePoint point = {};
        point.visible = true;
        point.ownedByEditor = g_app.guiCurvePointExplicit[ci];
        // Existence only: a point the driver never reported cannot be compared.
        // The live FREQUENCY is deliberately never turned into a comparable
        // value here -- it drifts under boost and temperature, so diffing
        // against it would make the editor look changed on its own.
        point.liveKnown = g_app.curve[ci].freq_kHz != 0;
        point.draftValid = g_app.guiDraft.curveValueValid[ci];
        point.draftMHz = g_app.guiDraft.curveMHz[ci];
        point.appliedMHz = g_app.appliedCurveMHz[ci];
        if (!gui_pending_curve_point_changed(point)) continue;
        out->curvePoint[ci] = true;
        out->summary.changedPointCount++;
        out->summary.domainMask |= GUI_PENDING_CURVE;
    }
}

// A moved/added/removed lock rewrites the whole tail on apply (expandLockedTail
// in capture_gui_desired_settings), but the tail edit controls deliberately keep
// showing their previous readback.  Mark them pending so the stale numbers are
// not mistaken for what the GPU will run.
static void gui_pending_mark_locked_tail(GuiPendingChanges* out) {
    for (int vi = 0; vi < g_app.numVisible; ++vi) {
        if (!gui_pending_point_in_locked_tail(vi)) continue;
        int ci = g_app.visibleMap[vi];
        if (ci < 0 || ci >= VF_NUM_POINTS) continue;
        if (out->curvePoint[ci]) continue;
        out->curvePoint[ci] = true;
        out->summary.changedPointCount++;
    }
}

// Points the editor never typed still move when the global GPU offset changes:
// Apply re-places them at stock + the new offset component. Without this, going
// from a profile with a selective +475 MHz offset to one with none looked like a
// no-op on the curve even though six points were about to drop by 475 MHz.
static void gui_pending_mark_gpu_offset_shift(GuiPendingChanges* out) {
    for (int vi = 0; vi < g_app.numVisible; ++vi) {
        int ci = g_app.visibleMap[vi];
        if (ci < 0 || ci >= VF_NUM_POINTS) continue;
        if (out->curvePoint[ci]) continue;
        GuiPendingOffsetShift shift = {};
        shift.inLockedTail = gui_pending_point_in_locked_tail(vi);
        shift.ownedByEditor = g_app.guiCurvePointExplicit[ci];
        shift.appliedOffsetComponentMHz = gpu_offset_component_mhz_for_point(
            ci, out->appliedGpuOffsetMHz, out->appliedGpuOffsetExcludeLowCount);
        shift.pendingOffsetComponentMHz = gpu_offset_component_mhz_for_point(
            ci, out->pendingGpuOffsetMHz, out->pendingGpuOffsetExcludeLowCount);
        if (!gui_pending_offset_shift_changed(shift)) continue;
        out->curvePoint[ci] = true;
        out->summary.changedPointCount++;
        out->summary.domainMask |= GUI_PENDING_CURVE;
    }
}

static void gui_pending_evaluate_fan(GuiPendingChanges* out) {
    // Green Curve asserts no fan intent on a GPU that reports no fans, and the
    // fan controls are disabled there, so there is nothing to advertise.
    if (!g_app.fanSupported) return;

    int fanPercent = g_app.guiFanFixedPercent;
    bool fixedTextValid = true;
    if (g_app.guiFanMode == FAN_MODE_FIXED) {
        int parsed = 0;
        fixedTextValid = parse_int_strict(g_app.guiDraft.fanFixedText, &parsed);
        if (fixedTextValid) fanPercent = clamp_percent(parsed);
    } else if (g_app.guiFanMode == FAN_MODE_AUTO) {
        fanPercent = 0;
    }

    // Same predicate capture_gui_apply_settings() uses for its fanChanged
    // verdict, so the button and the apply path cannot disagree.
    bool fanChanged = !fixedTextValid ||
        !fan_setting_matches_current(g_app.guiFanMode, fanPercent,
                                     &g_app.guiFanCurve);
    if (!fanChanged) return;

    unsigned int fanBits = 0;
    if (g_app.guiFanMode != g_app.activeFanMode)
        fanBits |= GUI_PENDING_FAN_MODE;
    if (g_app.guiFanMode == FAN_MODE_FIXED &&
        (!fixedTextValid || fanPercent != g_app.activeFanFixedPercent))
        fanBits |= GUI_PENDING_FAN_FIXED;
    if (g_app.guiFanMode == FAN_MODE_CURVE &&
        !fan_curve_equals(&g_app.guiFanCurve, &g_app.activeFanCurve))
        fanBits |= GUI_PENDING_FAN_CURVE;
    // A mismatch the sub-domains cannot attribute (fan readback still settling
    // toward the target) is still a real apply, so keep Apply reachable and
    // point at the control that owns the intent.
    if (!fanBits) fanBits = GUI_PENDING_FAN_MODE;
    out->summary.domainMask |= fanBits;
}

static void gui_pending_changes_evaluate(GuiPendingChanges* out) {
    if (!out) return;
    GuiPendingChanges empty = {};
    *out = empty;

    // A clean editor is by definition rebased from the applied state, so it has
    // nothing pending.  A detached or unattached draft belongs to another
    // GPU/topology and must not advertise per-field intent for this one; Apply
    // is already blocked there by the mutation-ready gate.
    if (!gui_state_dirty()) return;
    if (!gui_service_model_ready(&g_app.guiServiceModel)) return;
    if (!g_app.guiDraft.attached || g_app.guiDraft.detached) return;

    gui_pending_evaluate_curve(out);

    unsigned int lockTargetMHz = gui_editor_lock_target_mhz();
    GuiPendingLock lock = {};
    lock.draftActive = g_app.lockedCi >= 0 && lockTargetMHz > 0;
    lock.draftCi = g_app.lockedCi;
    lock.draftMHz = lockTargetMHz;
    lock.draftMode = (int)g_app.lockMode;
    lock.appliedActive = g_app.appliedLockCi >= 0 && g_app.appliedLockFreq > 0;
    lock.appliedCi = g_app.appliedLockCi;
    lock.appliedMHz = g_app.appliedLockFreq;
    lock.appliedMode = (int)g_app.appliedLockMode;
    // The applied baseline can hold the value the hardware settled on rather
    // than the one that was requested, so allow one VF grid step.
    lock.toleranceMHz = curve_point_verify_tolerance_mhz(g_app.lockedCi);
    if (gui_pending_lock_changed(lock)) {
        out->summary.domainMask |= GUI_PENDING_LOCK;
        gui_pending_mark_locked_tail(out);
    }

    ControlState control = {};
    bool haveControl = get_effective_control_state(&control);
    bool meaningfulGpu = haveControl && control_state_has_meaningful_gpu(&control);

    GuiPendingScalar gpuOffset = gui_pending_scalar_from_draft(
        g_app.guiDraft.gpuOffsetText,
        meaningfulGpu ? control.gpuOffsetMHz : current_applied_gpu_offset_mhz());
    if (gui_pending_scalar_changed(gpuOffset))
        out->summary.domainMask |= GUI_PENDING_GPU_OFFSET;

    GuiPendingScalar excludeLow = gui_pending_scalar_from_draft(
        g_app.guiDraft.gpuOffsetExcludeLowText,
        meaningfulGpu ? control.gpuOffsetExcludeLowCount
                      : (current_applied_gpu_offset_excludes_low_points()
                             ? g_app.appliedGpuOffsetExcludeLowCount : 0));
    if (gui_pending_scalar_changed(excludeLow))
        out->summary.domainMask |= GUI_PENDING_GPU_EXCLUDE;

    // Unparseable text leaves the applied value in place: an offset that cannot
    // be read must not be projected onto the curve as if it were zero.
    out->gpuOffsetValid = true;
    out->appliedGpuOffsetMHz = gpuOffset.appliedValue;
    out->appliedGpuOffsetExcludeLowCount = excludeLow.appliedValue;
    out->pendingGpuOffsetMHz =
        gpuOffset.draftValid ? gpuOffset.draftValue : gpuOffset.appliedValue;
    out->pendingGpuOffsetExcludeLowCount =
        excludeLow.draftValid ? excludeLow.draftValue : excludeLow.appliedValue;
    gui_pending_mark_gpu_offset_shift(out);

    GuiPendingScalar memOffset = gui_pending_scalar_from_draft(
        g_app.guiDraft.memOffsetText,
        haveControl && control_state_has_meaningful_mem(&control)
            ? control.memOffsetMHz
            : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
    if (gui_pending_scalar_changed(memOffset))
        out->summary.domainMask |= GUI_PENDING_MEM_OFFSET;

    GuiPendingScalar powerLimit = gui_pending_scalar_from_draft(
        g_app.guiDraft.powerLimitText,
        haveControl && control_state_has_meaningful_power(&control)
            ? control.powerLimitPct : g_app.powerLimitPct);
    if (gui_pending_scalar_changed(powerLimit))
        out->summary.domainMask |= GUI_PENDING_POWER_LIMIT;

    gui_pending_evaluate_fan(out);
}

static bool gui_pending_changes_equal(const GuiPendingChanges* a,
                                      const GuiPendingChanges* b) {
    if (!a || !b) return a == b;
    if (!gui_pending_summary_equal(&a->summary, &b->summary)) return false;
    // The projection numbers move the dashed curve even when the mask and the
    // changed-point set happen to match, so they are part of the repaint gate.
    if (a->gpuOffsetValid != b->gpuOffsetValid ||
        a->appliedGpuOffsetMHz != b->appliedGpuOffsetMHz ||
        a->appliedGpuOffsetExcludeLowCount != b->appliedGpuOffsetExcludeLowCount ||
        a->pendingGpuOffsetMHz != b->pendingGpuOffsetMHz ||
        a->pendingGpuOffsetExcludeLowCount != b->pendingGpuOffsetExcludeLowCount)
        return false;
    return memcmp(a->curvePoint, b->curvePoint, sizeof(a->curvePoint)) == 0;
}

// Lets the graph project an offset-moved point through the same numbers the
// diff used above.
static bool gui_pending_gpu_offset_projection(int* appliedMHz,
                                              int* appliedExcludeLowCount,
                                              int* pendingMHz,
                                              int* pendingExcludeLowCount) {
    if (!g_guiPendingChanges.gpuOffsetValid) return false;
    if (appliedMHz) *appliedMHz = g_guiPendingChanges.appliedGpuOffsetMHz;
    if (appliedExcludeLowCount)
        *appliedExcludeLowCount = g_guiPendingChanges.appliedGpuOffsetExcludeLowCount;
    if (pendingMHz) *pendingMHz = g_guiPendingChanges.pendingGpuOffsetMHz;
    if (pendingExcludeLowCount)
        *pendingExcludeLowCount = g_guiPendingChanges.pendingGpuOffsetExcludeLowCount;
    return true;
}

static bool gui_pending_domain_changed(unsigned int mask) {
    return gui_pending_domain_set(&g_guiPendingChanges.summary, mask);
}

static bool gui_pending_curve_point_is_changed(int ci) {
    if (ci < 0 || ci >= VF_NUM_POINTS) return false;
    return g_guiPendingChanges.curvePoint[ci];
}

static const GuiPendingSummary* gui_pending_summary() {
    return &g_guiPendingChanges.summary;
}

static bool gui_pending_edit_is_changed(HWND control) {
    if (!control) return false;
    if (control == g_app.hGpuOffsetEdit)
        return gui_pending_domain_changed(GUI_PENDING_GPU_OFFSET);
    if (control == g_app.hGpuOffsetExcludeLowEdit)
        return gui_pending_domain_changed(GUI_PENDING_GPU_EXCLUDE);
    if (control == g_app.hMemOffsetEdit)
        return gui_pending_domain_changed(GUI_PENDING_MEM_OFFSET);
    if (control == g_app.hPowerLimitEdit)
        return gui_pending_domain_changed(GUI_PENDING_POWER_LIMIT);
    if (control == g_app.hFanEdit)
        return gui_pending_domain_changed(GUI_PENDING_FAN_FIXED);
    if (control == g_app.hFanModeCombo)
        return gui_pending_domain_changed(GUI_PENDING_FAN_MODE);
    for (int vi = 0; vi < g_app.numVisible; ++vi) {
        if (control != g_app.hEditsMhz[vi]) continue;
        return gui_pending_curve_point_is_changed(g_app.visibleMap[vi]);
    }
    return false;
}

static void gui_pending_invalidate_if_flipped(HWND control, bool before,
                                              bool after) {
    if (!control || before == after) return;
    InvalidateRect(control, nullptr, FALSE);
}

static void gui_pending_invalidate_domain(HWND control, unsigned int mask,
                                          unsigned int beforeMask,
                                          unsigned int afterMask) {
    gui_pending_invalidate_if_flipped(control, (beforeMask & mask) != 0u,
                                      (afterMask & mask) != 0u);
}

// The graph occupies the top band of the content canvas, offset by the current
// scroll origin.  Invalidating without erase keeps the double-buffered paint
// path in charge of the pixels.
static void gui_pending_invalidate_graph() {
    if (!g_app.hMainWnd || !IsWindow(g_app.hMainWnd)) return;
    RECT graph = {
        -main_layout_scroll_x(),
        -main_layout_scroll_y(),
        main_layout_content_width() - main_layout_scroll_x(),
        main_layout_graph_height() - main_layout_scroll_y(),
    };
    InvalidateRect(g_app.hMainWnd, &graph, FALSE);
}

static void gui_pending_changes_refresh() {
    GuiPendingChanges next = {};
    gui_pending_changes_evaluate(&next);
    GuiPendingChanges previous = g_guiPendingChanges;
    bool identical = gui_pending_changes_equal(&previous, &next);
    if (!identical) {
        g_guiPendingChanges = next;
        debug_log("GUI pending changes: mask=0x%03X points=%d (was mask=0x%03X points=%d) dirty=%d attached=%d\n",
            next.summary.domainMask, next.summary.changedPointCount,
            previous.summary.domainMask, previous.summary.changedPointCount,
            gui_state_dirty() ? 1 : 0,
            g_app.guiDraft.attached ? 1 : 0);

        unsigned int beforeMask = previous.summary.domainMask;
        unsigned int afterMask = next.summary.domainMask;
        for (int vi = 0; vi < g_app.numVisible; ++vi) {
            int ci = g_app.visibleMap[vi];
            if (ci < 0 || ci >= VF_NUM_POINTS) continue;
            gui_pending_invalidate_if_flipped(g_app.hEditsMhz[vi],
                previous.curvePoint[ci], next.curvePoint[ci]);
            gui_pending_invalidate_domain(g_app.hLocks[vi], GUI_PENDING_LOCK,
                beforeMask, afterMask);
        }
        gui_pending_invalidate_domain(g_app.hGpuOffsetEdit,
            GUI_PENDING_GPU_OFFSET, beforeMask, afterMask);
        gui_pending_invalidate_domain(g_app.hGpuOffsetExcludeLowEdit,
            GUI_PENDING_GPU_EXCLUDE, beforeMask, afterMask);
        gui_pending_invalidate_domain(g_app.hMemOffsetEdit,
            GUI_PENDING_MEM_OFFSET, beforeMask, afterMask);
        gui_pending_invalidate_domain(g_app.hPowerLimitEdit,
            GUI_PENDING_POWER_LIMIT, beforeMask, afterMask);
        gui_pending_invalidate_domain(g_app.hFanModeCombo,
            GUI_PENDING_FAN_MODE, beforeMask, afterMask);
        gui_pending_invalidate_domain(g_app.hFanEdit,
            GUI_PENDING_FAN_FIXED, beforeMask, afterMask);
        gui_pending_invalidate_domain(g_app.hFanCurveBtn,
            GUI_PENDING_FAN_CURVE, beforeMask, afterMask);

        bool curveOrLockFlipped =
            ((beforeMask ^ afterMask) &
             (GUI_PENDING_CURVE | GUI_PENDING_LOCK)) != 0u ||
            memcmp(previous.curvePoint, next.curvePoint,
                   sizeof(next.curvePoint)) != 0;
        if (curveOrLockFlipped) gui_pending_invalidate_graph();
    }

    // The enable gate is re-asserted even when the mask did not move, because
    // the mutation-ready half of it changes with service/draft state.
    if (!g_app.hApplyBtn) return;
    GuiServiceActionability actionable = gui_service_actionability_from_app();
    bool mutationReady = gui_service_capability_enabled(
        &actionable, GUI_SERVICE_CAP_HARDWARE_MUTATION);
    gui_set_window_enabled_if_changed(g_app.hApplyBtn,
        gui_pending_apply_button_enabled(mutationReady,
                                         &g_guiPendingChanges.summary));
}
