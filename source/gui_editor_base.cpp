// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The editor's base record (gui_editor_base_policy.h): which record Apply and
// Save build on, its lifecycle, and the overlay of the user's edits onto it.
// Included after desired_settings_helpers.cpp (the active-intent match) and
// before main_shell.cpp, whose capture paths use it.

#include "gui_editor_base_policy.h"

static void gui_editor_base_clear_typed() {
    memset(g_app.guiCurvePointTyped, 0, sizeof(g_app.guiCurvePointTyped));
}

static const char* gui_editor_base_mode_name(int mode) {
    return mode == GUI_EDITOR_BASE_LOADED_RECORD ? "loaded-record" :
        mode == GUI_EDITOR_BASE_LOADED_LEGACY ? "loaded-legacy" : "active";
}

// A slot record now fills the editor (Load, shared Load, Save). An explicit
// record becomes the base; a legacy one means the editor holds all of it.
static void gui_editor_base_attach(const DesiredSettings* record, int slot, const char* why) {
    if (!record) return;
    const GuiEditorBaseMode mode = gui_editor_base_mode_for_record(*record);
    g_app.guiEditorBaseMode = (int)mode;
    g_app.guiEditorBaseSlot = slot;
    if (mode == GUI_EDITOR_BASE_LOADED_RECORD) g_app.guiEditorBaseRecord = *record;
    gui_editor_base_clear_typed();
    int captured = 0, explicitPoints = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        if (record->curvePointCaptured[ci]) ++captured;
        if (record->hasCurvePoint[ci]) ++explicitPoints;
    }
    debug_log("editor base: %s slot=%d mode=%s scope=0x%X captured=%d explicit=%d lock=%d/%u precise(lock=%d mem=%d power=%d) bound=%d\n",
        why ? why : "attach", slot, gui_editor_base_mode_name((int)mode), (unsigned)record->applicationScope,
        captured, explicitPoints, record->hasLock ? record->lockCi : -1, record->hasLock ? record->lockMHz : 0u,
        record->authoring.lockPrecise ? 1 : 0, record->memOffsetPrecise ? 1 : 0, record->powerLimitRatio ? 1 : 0,
        record->authoring.topologyBound ? 1 : 0);
}

// The editor no longer shows a loaded record: it was applied, discarded, or
// replaced by the active intent.
static void gui_editor_base_reset(const char* why) {
    if (g_app.guiEditorBaseMode != GUI_EDITOR_BASE_ACTIVE)
        debug_log("editor base: reset from %s slot=%d (%s)\n", gui_editor_base_mode_name(g_app.guiEditorBaseMode),
            g_app.guiEditorBaseSlot, why ? why : "reset");
    g_app.guiEditorBaseMode = GUI_EDITOR_BASE_ACTIVE;
    g_app.guiEditorBaseSlot = 0;
    gui_editor_base_clear_typed();
}

static const DesiredSettings* gui_editor_base_current() {
    return gui_editor_base_select((GuiEditorBaseMode)g_app.guiEditorBaseMode, &g_app.guiEditorBaseRecord,
        g_app.serviceActiveDesiredValid ? &g_app.serviceActiveDesired : nullptr);
}

static bool gui_editor_point_is_captured_from_base(int ci) {
    return gui_editor_base_owns_captured_point(gui_editor_base_current(), g_app.guiCurvePointTyped, ci);
}

static bool gui_editor_point_is_owned(int ci) {
    if (ci < 0 || ci >= VF_NUM_POINTS) return false;
    return g_app.guiCurvePointExplicit[ci] || gui_editor_point_is_captured_from_base(ci);
}

// A loaded explicit record that is not what the service runs: Apply has work
// even when no field differs from the applied state (captured points, exact
// values and scope are not fields).
static bool gui_editor_loaded_record_pending() {
    if (g_app.guiEditorBaseMode != GUI_EDITOR_BASE_LOADED_RECORD) return false;
    if (!g_app.serviceActiveDesiredValid) return true;
    char detail[192] = {};
    return !desired_settings_match_active_service_intent(&g_app.guiEditorBaseRecord,
        &g_app.serviceActiveDesired, detail, sizeof(detail));
}

// The request on the editor's base, or *used = false when there is none (the
// legacy editor request stays in charge). `editor` is the full field request.
static bool gui_editor_base_overlay_request(const DesiredSettings& editor, const GuiEditorCurrentScalars& current,
    DesiredSettings* out, bool* used, const char* purpose, char* err, size_t errSize) {
    if (used) *used = false;
    const DesiredSettings* base = gui_editor_base_current();
    if (!base || !out) return true;
    const GuiEditorEdits edits = gui_editor_edits(*base, editor, current, g_app.guiCurvePointTyped);
    char summary[160] = {};
    gui_editor_edits_describe(edits, summary, sizeof(summary));
    auto merged = std::make_unique<DesiredSettings>();
    char why[160] = {};
    if (!gui_editor_overlay(*base, editor, edits, merged.get(), why, sizeof(why))) {
        debug_log("editor base: %s overlay refused mode=%s slot=%d edits={%s}: %s\n", purpose,
            gui_editor_base_mode_name(g_app.guiEditorBaseMode), g_app.guiEditorBaseSlot, summary, why);
        set_message(err, errSize, "Your edits could not be combined with the loaded profile: %s.", why);
        return false;
    }
    debug_log("editor base: %s on %s base slot=%d scope=0x%X edits={%s}\n", purpose,
        gui_editor_base_mode_name(g_app.guiEditorBaseMode), g_app.guiEditorBaseSlot,
        (unsigned)merged->applicationScope, summary);
    *out = *merged;
    if (used) *used = true;
    return true;
}
