// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Editor projection of a flatten another tool set (gui_observed_lock_policy.h).
// Included by main_state_sync.cpp ahead of apply_service_snapshot_to_app().

static GuiObservedLock gui_observed_lock_projected() {
    GuiObservedLock p;
    if (!g_app.observedLockValid) return p;
    p.valid = true;
    p.ci = g_app.observedLockCi;
    p.mhz = g_app.observedLockMHz;
    return p;
}

#ifndef GREEN_CURVE_SERVICE_BINARY
// The editor lock is the projection of a flatten another tool set.
static bool gui_editor_lock_is_observed() {
    GuiObservedLockInput in;
    in.editorLockCi = g_app.lockedCi;
    in.editorLockMHz = g_app.lockedFreq;
    in.editorLockMode = (int)g_app.lockMode;
    in.appliedLockCi = g_app.appliedLockCi;
    in.appliedLockMHz = g_app.appliedLockFreq;
    in.appliedLockMode = (int)g_app.appliedLockMode;
    in.projected = gui_observed_lock_projected();
    return gui_observed_lock_editor_is_projection(in);
}
#endif

// Show a flatten another tool programmed the way a Green Curve flatten is
// shown (gui_observed_lock_policy.h).  Called with g_appLock held, after the
// snapshot's curve and visible map were adopted.  Returns true when the editor
// lock is the projection afterwards, so the stale-adopted-lock clear below
// must leave it alone.
static bool project_observed_external_lock(const ServiceSnapshot* snapshot) {
    if (app_is_service_process() || !g_app.usingBackgroundService) return false;
    GuiObservedLockInput in;
    in.snapshotLoaded = snapshot->loaded;
    in.serviceReportsLock = snapshot->hasLock && snapshot->lockCi >= 0 && snapshot->lockMHz > 0;
    in.editorDirty = gui_state_dirty();
    in.editorLockCi = g_app.lockedCi;
    in.editorLockMHz = g_app.lockedFreq;
    in.editorLockMode = (int)g_app.lockMode;
    in.appliedLockCi = g_app.appliedLockCi;
    in.appliedLockMHz = g_app.appliedLockFreq;
    in.appliedLockMode = (int)g_app.appliedLockMode;
    in.projected = gui_observed_lock_projected();
    if (in.snapshotLoaded && !in.serviceReportsLock) {
        in.recognized = gui_observed_lock_recognize(g_app.curve, g_app.freqOffsets,
            g_app.vfFloorBinding.floor, g_app.visibleMap, g_app.numVisible);
    }
    const GuiObservedLockAction action = gui_observed_lock_decide(in);
    const GuiObservedLock& rec = in.recognized;
    debug_log_on_change("lock observe: action=%s recognized=%d anchor=ci%d mv=%u plateau=%u MHz tail=%d exact=%d"
        " projected=ci%d/%u editor=ci%d/%u applied=ci%d/%u dirty=%d serviceLock=%d\n",
        gui_observed_lock_action_name(action), rec.valid ? 1 : 0, rec.ci,
        rec.valid ? g_app.curve[rec.ci].volt_uV / 1000u : 0u, rec.mhz, rec.tailCount, rec.exact ? 1 : 0,
        in.projected.ci, in.projected.mhz, in.editorLockCi, in.editorLockMHz,
        in.appliedLockCi, in.appliedLockMHz, in.editorDirty ? 1 : 0, in.serviceReportsLock ? 1 : 0);
    switch (action) {
        case GUI_OBSERVED_LOCK_ADOPT:
            g_app.lockedVi = rec.vi;
            g_app.lockedCi = rec.ci;
            g_app.lockedFreq = rec.mhz;
            g_app.lockMode = LOCK_MODE_FLATTEN;
            g_app.guiLockTracksAnchor = true;
            // Mirrored, so the projection is not a pending change (sparse
            // power/fan Applies leave the curve alone; a full Apply sends it).
            g_app.appliedLockVi = rec.vi;
            g_app.appliedLockCi = rec.ci;
            g_app.appliedLockFreq = rec.mhz;
            g_app.appliedLockMode = LOCK_MODE_FLATTEN;
            g_app.observedLockValid = true;
            g_app.observedLockCi = rec.ci;
            g_app.observedLockMHz = rec.mhz;
            debug_log("lock observe: showing a flatten another tool set as the editor lock ci=%d vi=%d mv=%u mhz=%u tail=%d\n",
                rec.ci, rec.vi, g_app.curve[rec.ci].volt_uV / 1000u, rec.mhz, rec.tailCount);
            g_guiForceFullRefresh = true;
            return true;
        case GUI_OBSERVED_LOCK_KEEP:
            return true;
        case GUI_OBSERVED_LOCK_CLEAR: {
            const GuiObservedLock projected = in.projected;
            if (gui_observed_lock_editor_matches(g_app.lockedCi, g_app.lockedFreq, (int)g_app.lockMode, projected)) {
                g_app.lockedVi = -1;
                g_app.lockedCi = -1;
                g_app.lockedFreq = 0;
                g_app.lockMode = LOCK_MODE_NONE;
                g_app.guiLockTracksAnchor = true;
            }
            if (gui_observed_lock_editor_matches(g_app.appliedLockCi, g_app.appliedLockFreq, (int)g_app.appliedLockMode, projected)) {
                g_app.appliedLockVi = -1;
                g_app.appliedLockCi = -1;
                g_app.appliedLockFreq = 0;
                g_app.appliedLockMode = LOCK_MODE_NONE;
            }
            g_app.observedLockValid = false;
            debug_log("lock observe: the flatten another tool set left the GPU; editor lock ci=%d mhz=%u withdrawn\n",
                projected.ci, projected.mhz);
            g_guiForceFullRefresh = true;
            return false;
        }
        case GUI_OBSERVED_LOCK_FORGET:
            debug_log("lock observe: projection ci=%d mhz=%u released (serviceLock=%d editor=ci%d/%u)\n",
                in.projected.ci, in.projected.mhz, in.serviceReportsLock ? 1 : 0,
                in.editorLockCi, in.editorLockMHz);
            g_app.observedLockValid = false;
            return false;
        case GUI_OBSERVED_LOCK_NONE:
            return false;
    }
    return false;
}
