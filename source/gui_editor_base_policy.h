// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GUI_EDITOR_BASE_POLICY_H
#define GREEN_CURVE_GUI_EDITOR_BASE_POLICY_H

// The record the Windows editor's Apply and Save build on.
//
// The editor shows whole-MHz fields and owns a point only when it holds an
// explicit MHz for it. A record with an explicit application scope (Import
// current GPU settings, imported profile files) can carry intent those fields
// cannot hold: captured per-point offsets, exact Hz for the lock, memory and
// points, a power ratio, a topology binding. Rebuilding the request from the
// fields alone dropped all of it: captured points went back to stock, the
// request fell back to legacy scope, and the slot never became the active
// profile. Tray, hotkey, logon and app start applied the record itself and
// were not affected.
//
// So the editor keeps such a record as its base. Apply and Save start from
// the base and overlay only what the user changed in the editor, domain by
// domain and point by point; everything else keeps the record's own authority.
// The base is
//  - the slot record the user loaded (Load, shared Load, Save), when it has an
//    explicit scope;
//  - otherwise the service's active intent when that has an explicit scope (a
//    record applied from the tray, or an earlier Apply of a loaded record);
//  - none for a loaded legacy record or a legacy active intent: the editor
//    represents those completely, and the legacy request path is unchanged.

#include <stdio.h>
#include "gpu_core.h"
#include "fan_curve_api.h"

enum GuiEditorBaseMode : int {
    GUI_EDITOR_BASE_ACTIVE = 0,     // the active intent, when it is explicit
    GUI_EDITOR_BASE_LOADED_RECORD,  // a loaded explicit-scope record
    GUI_EDITOR_BASE_LOADED_LEGACY,  // a loaded legacy record: no base
};

static inline GuiEditorBaseMode gui_editor_base_mode_for_record(const DesiredSettings& record) {
    return record.applicationScope != DESIRED_SCOPE_LEGACY ? GUI_EDITOR_BASE_LOADED_RECORD
                                                           : GUI_EDITOR_BASE_LOADED_LEGACY;
}

static inline const DesiredSettings* gui_editor_base_select(GuiEditorBaseMode mode,
    const DesiredSettings* loaded, const DesiredSettings* active) {
    if (mode == GUI_EDITOR_BASE_LOADED_RECORD) return loaded;
    if (mode == GUI_EDITOR_BASE_LOADED_LEGACY) return nullptr;
    return active && active->applicationScope != DESIRED_SCOPE_LEGACY ? active : nullptr;
}

// The applied values the editor compares a domain against when the base does
// not own that domain (the field then shows the current value).
struct GuiEditorCurrentScalars {
    int gpuOffsetMHz = 0, gpuOffsetExcludeLowCount = 0;
    int memOffsetMHz = 0, powerLimitPct = 0;
    int xbarOffsetKhz = 0, xbarMsvddOffsetUv = 0, sysClkOffsetKhz = 0, videoClkOffsetKhz = 0;
    bool fanChanged = false;  // the editor's fan differs from the running fan
};

struct GuiEditorEdits {
    bool gpuOffset = false, memOffset = false, power = false, fan = false;
    bool xbar = false, xbarMsvdd = false, sysClk = false, videoClk = false;
    bool lock = false;
    int lockRegionStart = VF_NUM_POINTS;  // first point the edited lock owns
    bool point[VF_NUM_POINTS] = {};
    int pointCount = 0;
};

static inline int gui_editor_exclude_count(const DesiredSettings& d) {
    return d.hasGpuOffset && d.gpuOffsetMHz != 0 && d.gpuOffsetExcludeLowCount > 0 ? d.gpuOffsetExcludeLowCount : 0;
}
static inline LockMode gui_editor_lock_mode(const DesiredSettings& d) {
    return d.lockMode == LOCK_MODE_NONE ? LOCK_MODE_FLATTEN : d.lockMode;
}
static inline bool gui_editor_lock_present(const DesiredSettings& d) {
    return d.hasLock && d.lockCi >= 0 && d.lockCi < VF_NUM_POINTS && d.lockMHz > 0;
}

// What the editor changed relative to the base. `editor` is the request the
// fields build (capture_gui_desired_settings(), every global included);
// `typed[ci]` says the user typed point ci since the editor took this base.
// A point the editor merely projects (live readback, retained applied shape)
// is never an edit: only typing makes it one.
static inline GuiEditorEdits gui_editor_edits(const DesiredSettings& base, const DesiredSettings& editor,
    const GuiEditorCurrentScalars& current, const bool* typed) {
    GuiEditorEdits e;
    if (editor.hasGpuOffset) {
        e.gpuOffset = base.hasGpuOffset
            ? editor.gpuOffsetMHz != base.gpuOffsetMHz || gui_editor_exclude_count(editor) != gui_editor_exclude_count(base)
            : editor.gpuOffsetMHz != current.gpuOffsetMHz ||
              gui_editor_exclude_count(editor) != (current.gpuOffsetMHz ? current.gpuOffsetExcludeLowCount : 0);
    }
    if (editor.hasMemOffset)
        e.memOffset = editor.memOffsetMHz != (base.hasMemOffset ? base.memOffsetMHz : current.memOffsetMHz);
    if (editor.hasPowerLimit)
        e.power = editor.powerLimitPct != (base.hasPowerLimit ? base.powerLimitPct : current.powerLimitPct);
    if (editor.hasFan) {
        e.fan = base.hasFan
            ? editor.fanMode != base.fanMode ||
              (editor.fanMode == FAN_MODE_FIXED && editor.fanPercent != base.fanPercent) ||
              (editor.fanMode == FAN_MODE_CURVE && !fan_curve_equals(&editor.fanCurve, &base.fanCurve))
            : current.fanChanged;
    }
    if (editor.hasXbarOffsetKhz)
        e.xbar = editor.xbarOffsetKhz != (base.hasXbarOffsetKhz ? base.xbarOffsetKhz : current.xbarOffsetKhz);
    if (editor.hasXbarMsvddOffsetUv)
        e.xbarMsvdd = editor.xbarMsvddOffsetUv !=
            (base.hasXbarMsvddOffsetUv ? base.xbarMsvddOffsetUv : current.xbarMsvddOffsetUv);
    if (editor.hasSysClkOffsetKhz)
        e.sysClk = editor.sysClkOffsetKhz != (base.hasSysClkOffsetKhz ? base.sysClkOffsetKhz : current.sysClkOffsetKhz);
    if (editor.hasVideoClkOffsetKhz)
        e.videoClk = editor.videoClkOffsetKhz !=
            (base.hasVideoClkOffsetKhz ? base.videoClkOffsetKhz : current.videoClkOffsetKhz);

    const bool baseLock = gui_editor_lock_present(base), editorLock = gui_editor_lock_present(editor);
    e.lock = baseLock != editorLock ||
        (editorLock && (editor.lockCi != base.lockCi || editor.lockMHz != base.lockMHz ||
                        gui_editor_lock_mode(editor) != gui_editor_lock_mode(base) ||
                        !!editor.lockTracksAnchor != !!base.lockTracksAnchor));
    if (e.lock) {
        if (baseLock) e.lockRegionStart = base.lockCi;
        if (editorLock && editor.lockCi < e.lockRegionStart) e.lockRegionStart = editor.lockCi;
    }
    // The tail an unchanged lock owns is the base's; the editor only shows it.
    const int ownedTail = baseLock && !e.lock ? base.lockCi : VF_NUM_POINTS;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (ci >= e.lockRegionStart || ci >= ownedTail || !typed || !typed[ci]) continue;
        if (base.hasCurvePoint[ci] && editor.hasCurvePoint[ci] && editor.curvePointMHz[ci] == base.curvePointMHz[ci] &&
            !!editor.curvePointFromGpuOffset[ci] == !!base.curvePointFromGpuOffset[ci])
            continue;
        e.point[ci] = true;
        ++e.pointCount;
    }
    return e;
}

static inline bool gui_editor_edits_any(const GuiEditorEdits& e) {
    return e.gpuOffset || e.memOffset || e.power || e.fan || e.xbar || e.xbarMsvdd || e.sysClk ||
        e.videoClk || e.lock || e.pointCount > 0;
}

// The base with the edited domains and points taken from the editor. An edited
// domain carries the editor's whole-MHz value, so the base's exact Hz,
// captured offset or ratio for it is dropped; an unedited one keeps all of it.
// Fails, naming the reason, when the result is not a valid request.
static inline bool gui_editor_overlay(const DesiredSettings& base, const DesiredSettings& editor,
    const GuiEditorEdits& e, DesiredSettings* out, char* why, size_t whySize) {
    auto fail = [&](const char* text) {
        if (why && whySize) snprintf(why, whySize, "%s", text);
        return false;
    };
    if (!out) return fail("no output");
    *out = base;
    if (e.gpuOffset) {
        out->hasGpuOffset = editor.hasGpuOffset;
        out->gpuOffsetMHz = editor.gpuOffsetMHz;
        out->gpuOffsetExcludeLowCount = editor.gpuOffsetExcludeLowCount;
        desired_graphics_precision_clear(out);
    }
    if (e.memOffset) desired_memory_intent_copy(out, &editor);
    if (e.power) desired_power_intent_copy(out, &editor);
    if (e.fan) {
        out->hasFan = editor.hasFan;
        out->fanAuto = editor.fanAuto;
        out->fanMode = editor.fanMode;
        out->fanPercent = editor.fanPercent;
        out->fanCurve = editor.fanCurve;
    }
    if (e.xbar) { out->hasXbarOffsetKhz = editor.hasXbarOffsetKhz; out->xbarOffsetKhz = editor.xbarOffsetKhz; }
    if (e.xbarMsvdd) {
        out->hasXbarMsvddOffsetUv = editor.hasXbarMsvddOffsetUv;
        out->xbarMsvddOffsetUv = editor.xbarMsvddOffsetUv;
    }
    if (e.sysClk) { out->hasSysClkOffsetKhz = editor.hasSysClkOffsetKhz; out->sysClkOffsetKhz = editor.sysClkOffsetKhz; }
    if (e.videoClk) {
        out->hasVideoClkOffsetKhz = editor.hasVideoClkOffsetKhz;
        out->videoClkOffsetKhz = editor.videoClkOffsetKhz;
    }
    if (e.lock) {
        out->hasLock = editor.hasLock;
        out->lockCi = editor.hasLock ? editor.lockCi : -1;
        out->lockMHz = editor.hasLock ? editor.lockMHz : 0;
        out->lockMode = editor.hasLock ? editor.lockMode : LOCK_MODE_NONE;
        out->lockTracksAnchor = editor.hasLock ? editor.lockTracksAnchor : 0;
        desired_lock_precision_clear(out);
        for (int ci = e.lockRegionStart; ci < VF_NUM_POINTS; ++ci) desired_curve_point_copy(out, &editor, ci);
    }
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci)
        if (e.point[ci]) desired_curve_point_copy(out, &editor, ci);
    out->resetOcBeforeApply = 0;
    if (!desired_curve_intent_valid(out)) return fail("the edited curve conflicts with the profile's lock");
    if (!desired_scalar_intent_valid(out)) return fail("the edited memory or power value conflicts with the profile");
    if (!desired_authoring_valid(out)) return fail("an edited point lies outside the profile's curve binding");
    return true;
}

// One line for the diagnostics log: which domains were taken from the editor.
static inline void gui_editor_edits_describe(const GuiEditorEdits& e, char* text, size_t size) {
    if (!text || !size) return;
    snprintf(text, size, "gpu=%d mem=%d power=%d fan=%d xbar=%d/%d sys=%d video=%d lock=%d lockFrom=%d points=%d",
        e.gpuOffset ? 1 : 0, e.memOffset ? 1 : 0, e.power ? 1 : 0, e.fan ? 1 : 0, e.xbar ? 1 : 0,
        e.xbarMsvdd ? 1 : 0, e.sysClk ? 1 : 0, e.videoClk ? 1 : 0, e.lock ? 1 : 0,
        e.lock ? e.lockRegionStart : -1, e.pointCount);
}

#endif
