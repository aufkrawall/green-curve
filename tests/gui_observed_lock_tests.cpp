// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The Windows editor's projection of a VF flatten another tool set
// (source/gui_observed_lock_policy.h) and the Save notice for it
// (source/profile_save_policy.h), assertion codes 12290-12299.  The RTX 5070
// table is the curve_plan fixture's: stock rises to a level 2400 MHz top from
// point 71; the reported case is a 2962 MHz flatten from point 76 (925 mV)
// whose tail carries -2045 MHz offsets the driver holds at the anchor.

#include <stdio.h>
#include <string.h>

#include "curve_plan_fixtures.h"
#include "gui_observed_lock_policy.h"
#include "profile_save_policy.h"

namespace {

struct Editor {
    VfFloorTopology floor{};
    int visibleMap[VF_NUM_POINTS]{};
    int numVisible = 0;
};

Editor editor_for(const Table& t) {
    Editor e;
    e.floor = vf_floor_topology_compute(t.curve, t.offsets, VF_NUM_POINTS);
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (vf_point_listed(e.floor, t.curve, i)) e.visibleMap[e.numVisible++] = i;
    return e;
}

GuiObservedLock recognize(const Table& t) {
    const Editor e = editor_for(t);
    return gui_observed_lock_recognize(t.curve, t.offsets, e.floor, e.visibleMap, e.numVisible);
}

int visible_index(const Table& t, int ci) {
    const Editor e = editor_for(t);
    for (int vi = 0; vi < e.numVisible; ++vi)
        if (e.visibleMap[vi] == ci) return vi;
    return -1;
}

// Another tool's flatten: prefix raised by up to 300 MHz, the 925 mV anchor
// raised to 2962 MHz, every tail point pushed down 2045 MHz and held at the
// anchor by the driver.
void rtx_external_flatten(Table* t) {
    rtx_stock(t, 45);
    for (int i = 60; i < 76; ++i) {
        t->offsets[i] = 300000;
        t->curve[i].freq_kHz += 300000u;
    }
    t->offsets[76] = 2962000 - (int)t->curve[76].freq_kHz;
    t->curve[76].freq_kHz = 2962000u;
    for (int i = 77; i < 127; ++i) {
        t->offsets[i] = -2045000;
        t->curve[i].freq_kHz = 2962000u;
    }
}

GuiObservedLock lock_at(int ci, unsigned int mhz) {
    GuiObservedLock l;
    l.valid = true;
    l.ci = ci;
    l.mhz = mhz;
    return l;
}

GuiObservedLockInput clean_input() {
    GuiObservedLockInput in;
    in.snapshotLoaded = true;
    return in;
}

void editor_lock(GuiObservedLockInput* in, int ci, unsigned int mhz, int mode = LOCK_MODE_FLATTEN) {
    in->editorLockCi = ci;
    in->editorLockMHz = mhz;
    in->editorLockMode = mode;
}

void applied_lock(GuiObservedLockInput* in, int ci, unsigned int mhz, int mode = LOCK_MODE_FLATTEN) {
    in->appliedLockCi = ci;
    in->appliedLockMHz = mhz;
    in->appliedLockMode = mode;
}

int run_recognition_cases() {
    // 12290: a stock curve with a level top is not a lock.  The recognizer
    // alone finds the 2400 MHz top (that is what the offset rule is for).
    Table stock;
    rtx_stock(&stock, 45);
    const Editor stockEditor = editor_for(stock);
    const VfFlatTailRecognition stockRec = vf_recognize_flat_tail(stock.curve, stockEditor.floor, 1, 3);
    if (!stockRec.found || stockRec.anchorCi != 71 || stockRec.plateauMHz != 2400) return 12290;
    if (recognize(stock).valid) return 12290;

    // 12291: one uniform VF-table offset over that level top is not a lock either.
    Table uniform;
    rtx_stock(&uniform, 45);
    for (int i = 0; i < 127; ++i) {
        uniform.offsets[i] = 50000;
        uniform.curve[i].freq_kHz += 50000u;
    }
    if (recognize(uniform).valid) return 12291;

    // 12292: the reported flatten is the editor lock at its anchor.
    Table ext;
    rtx_external_flatten(&ext);
    const GuiObservedLock rec = recognize(ext);
    if (!rec.valid || rec.ci != 76 || rec.mhz != 2962 || rec.tailCount != 50 || !rec.exact ||
        rec.vi != visible_index(ext, 76) || rec.vi < 0) return 12292;
    if (ext.curve[rec.ci].volt_uV != 925000u) return 12292;

    // 12293: an anchor the editor does not list with a point after it has no
    // tail to grey; missing inputs recognize nothing.
    {
        Editor e = editor_for(ext);
        const int anchorVi = visible_index(ext, 76);
        if (gui_observed_lock_recognize(ext.curve, ext.offsets, e.floor, e.visibleMap, anchorVi + 1).valid)
            return 12293;
        if (!gui_observed_lock_recognize(ext.curve, ext.offsets, e.floor, e.visibleMap, anchorVi + 2).valid)
            return 12293;
        if (gui_observed_lock_recognize(ext.curve, nullptr, e.floor, e.visibleMap, e.numVisible).valid ||
            gui_observed_lock_recognize(nullptr, ext.offsets, e.floor, e.visibleMap, e.numVisible).valid)
            return 12293;
    }

    // 12294: a sub-700 mV anchor with jittered tail (the Pascal fixture) is
    // recognized at its true start, with the tolerant plateau.
    Table pascal;
    pascal_external_flatten(&pascal);
    const GuiObservedLock pascalRec = recognize(pascal);
    if (!pascalRec.valid || pascalRec.ci != 21 || pascalRec.mhz != 1000 || pascalRec.exact) return 12294;
    return 0;
}

int run_decision_cases() {
    const GuiObservedLock seen = lock_at(76, 2962);

    // 12295: a clean editor with no lock adopts the flatten; once projected
    // it keeps it; a moved flatten replaces the projection.
    GuiObservedLockInput in = clean_input();
    in.recognized = seen;
    if (gui_observed_lock_decide(in) != GUI_OBSERVED_LOCK_ADOPT) return 12295;
    editor_lock(&in, 76, 2962);
    applied_lock(&in, 76, 2962);
    in.projected = seen;
    if (!gui_observed_lock_editor_is_projection(in)) return 12295;
    if (gui_observed_lock_decide(in) != GUI_OBSERVED_LOCK_KEEP) return 12295;
    in.recognized = lock_at(76, 2900);
    if (gui_observed_lock_decide(in) != GUI_OBSERVED_LOCK_ADOPT) return 12295;

    // 12296: drafts are never rewritten; Green Curve's own lock releases the
    // projection; nothing happens before the snapshot is loaded.
    in.recognized = seen;
    in.editorDirty = true;
    if (gui_observed_lock_decide(in) != GUI_OBSERVED_LOCK_NONE) return 12296;
    in.editorDirty = false;
    in.serviceReportsLock = true;
    if (gui_observed_lock_decide(in) != GUI_OBSERVED_LOCK_FORGET) return 12296;
    {
        GuiObservedLockInput owned = clean_input();
        owned.serviceReportsLock = true;
        owned.recognized = seen;
        if (gui_observed_lock_decide(owned) != GUI_OBSERVED_LOCK_NONE) return 12296;
        GuiObservedLockInput cold;
        cold.recognized = seen;
        if (gui_observed_lock_decide(cold) != GUI_OBSERVED_LOCK_NONE) return 12296;
    }
    in.serviceReportsLock = false;

    // 12297: the flatten leaving the GPU withdraws the projection, also after
    // an Apply that unchecked it (editor empty, applied mirror still set); a
    // typed or loaded lock is never replaced.
    in.recognized = GuiObservedLock{};
    if (gui_observed_lock_decide(in) != GUI_OBSERVED_LOCK_CLEAR) return 12297;
    {
        GuiObservedLockInput unchecked = clean_input();
        unchecked.projected = seen;
        applied_lock(&unchecked, 76, 2962);
        if (gui_observed_lock_decide(unchecked) != GUI_OBSERVED_LOCK_CLEAR) return 12297;
        GuiObservedLockInput typed = clean_input();
        typed.projected = seen;
        typed.recognized = seen;
        editor_lock(&typed, 76, 2900);
        applied_lock(&typed, 76, 2962);
        if (gui_observed_lock_editor_is_projection(typed)) return 12297;
        if (gui_observed_lock_decide(typed) != GUI_OBSERVED_LOCK_FORGET) return 12297;
        GuiObservedLockInput loaded = clean_input();
        loaded.recognized = seen;
        editor_lock(&loaded, 70, 2700, LOCK_MODE_HARD);
        if (gui_observed_lock_decide(loaded) != GUI_OBSERVED_LOCK_NONE) return 12297;
        GuiObservedLockInput pinned = clean_input();
        pinned.projected = seen;
        editor_lock(&pinned, 76, 2962, LOCK_MODE_HARD);
        applied_lock(&pinned, 76, 2962, LOCK_MODE_HARD);
        if (gui_observed_lock_editor_is_projection(pinned)) return 12297;
    }

    // 12298: a lock the editor adopted earlier (editor == applied, not a
    // projection) gives way to the flatten on the GPU; without one it is left
    // to the stale-adopted-lock clear.
    {
        GuiObservedLockInput adopted = clean_input();
        editor_lock(&adopted, 70, 2700);
        applied_lock(&adopted, 70, 2700);
        adopted.recognized = seen;
        if (gui_observed_lock_decide(adopted) != GUI_OBSERVED_LOCK_ADOPT) return 12298;
        adopted.recognized = GuiObservedLock{};
        if (gui_observed_lock_decide(adopted) != GUI_OBSERVED_LOCK_NONE) return 12298;
        if (gui_observed_lock_decide(clean_input()) != GUI_OBSERVED_LOCK_NONE) return 12298;
    }
    if (strcmp(gui_observed_lock_action_name(GUI_OBSERVED_LOCK_ADOPT), "adopt") != 0 ||
        strcmp(gui_observed_lock_action_name(GUI_OBSERVED_LOCK_CLEAR), "clear") != 0)
        return 12298;
    return 0;
}

int run_save_cases() {
    // 12299: Save of the projection names the flatten and counts the lower
    // points the other tool set that the slot leaves out.
    Table ext;
    rtx_external_flatten(&ext);
    DesiredSettings saved = flatten_request(76, 2962);
    int componentKHz[VF_NUM_POINTS] = {};
    int first = -1;
    if (profile_save_count_unsaved_pre_anchor_points(ext.curve, ext.offsets, componentKHz, &saved, &first) != 16 ||
        first != 60) return 12299;
    saved.hasCurvePoint[60] = true;
    saved.curvePointMHz[60] = 1777;
    saved.curvePointCaptured[61] = 1;
    if (profile_save_count_unsaved_pre_anchor_points(ext.curve, ext.offsets, componentKHz, &saved, &first) != 14 ||
        first != 62) return 12299;
    for (int i = 0; i < VF_NUM_POINTS; ++i) componentKHz[i] = 300000;
    if (profile_save_count_unsaved_pre_anchor_points(ext.curve, ext.offsets, componentKHz, &saved, nullptr) != 0)
        return 12299;
    DesiredSettings noLock = {};
    if (profile_save_count_unsaved_pre_anchor_points(ext.curve, ext.offsets, nullptr, &noLock, &first) != 0 ||
        first != -1) return 12299;
    // The projection is an editor lock too (`lockFromUserEdit`), so the
    // observed flag is decided first; Green Curve's own lock never is.
    if (profile_save_notice(true, 0, &saved, false, true, 0, true) != PROFILE_SAVE_NOTICE_OBSERVED_FLATTEN ||
        profile_save_notice(true, 0, &saved, true, true, 0, true) != PROFILE_SAVE_NOTICE_NONE ||
        profile_save_notice(true, 0, &saved, false, true, 0, false) != PROFILE_SAVE_NOTICE_NONE ||
        profile_save_notice(true, 0, &noLock, false, false, 0, true) != PROFILE_SAVE_NOTICE_NONE ||
        profile_save_notice(false, 3, &saved, false, true, 0, true) != PROFILE_SAVE_NOTICE_OMITTED_EXTERNAL_CURVE)
        return 12299;
    if (strcmp(profile_save_notice_name(PROFILE_SAVE_NOTICE_OBSERVED_FLATTEN), "observed-flatten") != 0)
        return 12299;
    return 0;
}

}  // namespace

int run_gui_observed_lock_tests() {
    if (int failure = run_recognition_cases()) return failure;
    if (int failure = run_decision_cases()) return failure;
    if (int failure = run_save_cases()) return failure;
    return 0;
}
