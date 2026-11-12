// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// "Prepare compatible curve", operation A (curve_prefix_repair.h), on the
// manual Apply path.  Included from ui_main_apply.cpp only.
//
// When the planner proves that earlier points rise above a lock, the GUI
// offers to lower them in the editor draft instead of sending a request the
// service would refuse.  Nothing here writes hardware: an accepted repair is
// typed into the point fields through the ordinary user-edit path, so the
// draft owns plain absolute targets and Apply, Save and automatic restore
// replay that intent, never this algorithm.  The judgement uses the exact
// planner input the service's pre-write check builds
// (apply_curve_plan_pre_write_input), over the state mirrored from the
// service; the service still re-plans on a fresh read at Apply.

#include "curve_prefix_repair_message.h"

static void gui_curve_prepare_log_line(const char* line) {
    debug_log("%s", line ? line : "");
}

// Every changed point must have an editor row before anything is typed, so an
// accepted repair is all-or-nothing.
static bool gui_curve_prepare_rows(const CurvePrefixRepair& repair, int* rows) {
    for (int k = 0; k < repair.changedCount; ++k) {
        const int ci = repair.points[k].ci;
        rows[k] = -1;
        const int lastRow = g_app.lockedVi >= 0 ? g_app.lockedVi : g_app.numVisible;
        for (int vi = 0; vi < lastRow && vi < g_app.numVisible; ++vi) {
            if (g_app.visibleMap[vi] == ci) {
                rows[k] = vi;
                break;
            }
        }
        if (rows[k] < 0 || !g_app.hEditsMhz[rows[k]]) {
            debug_log("curve-prepare: accept aborted -- ci=%d has no editor row before the"
                      " lock (lockedVi=%d numVisible=%d); draft unchanged\n",
                ci, g_app.lockedVi, g_app.numVisible);
            return false;
        }
    }
    return true;
}

static bool gui_curve_prepare_accept(const CurvePrefixRepair& repair) {
    int rows[VF_NUM_POINTS] = {};
    if (!gui_curve_prepare_rows(repair, rows)) return false;
    for (int k = 0; k < repair.changedCount; ++k) {
        char text[16] = {};
        StringCchPrintfA(text, ARRAY_COUNT(text), "%u", repair.points[k].newMHz);
        // Not a programmatic update: EN_CHANGE takes the user-edit path and
        // records the point as a typed (absolute) target in the draft.
        SetWindowTextA(g_app.hEditsMhz[rows[k]], text);
    }
    return true;
}

// Returns true when this Apply must stop here: the user accepted (or
// declined) a draft repair, or the planner proved a prefix refusal the repair
// cannot fix and the user has been told why.  False lets the Apply continue
// to the service, which plans again on a fresh read.
static bool gui_curve_prepare_intercept_apply(const DesiredSettings* desired) {
    if (!desired || !desired->resetOcBeforeApply) return false;
    if (!desired->hasLock || desired->lockCi < 0 || desired->lockMHz == 0) return false;
    if (!g_app.vfFloorBinding.bound) return false;
    const ApplyCurvePlanPreWriteRoute route = apply_curve_plan_pre_write_route(desired);
    int componentKHz[VF_NUM_POINTS] = {};
    const CurvePlanInput input =
        apply_curve_plan_pre_write_input(desired, route, componentKHz);
    const CurvePlanResult plan = curve_plan_evaluate(input);
    curve_plan_log(plan, "gui-draft", gui_curve_prepare_log_line);
    if (plan.disposition != CURVE_PLAN_REFUSED) return false;
    bool provenPrefix = false;
    for (int k = 0; k < plan.issueCount; ++k) {
        if (plan.issues[k].reason == CURVE_PLAN_REASON_PREFIX_ABOVE_PLATEAU &&
            plan.issues[k].proven) provenPrefix = true;
    }
    // Anchor and tail refusals are reported by the service's own check.
    if (!provenPrefix) return false;

    const CurvePrefixRepair repair = curve_prefix_repair_prepare(input);
    curve_prefix_repair_log(repair, "gui-draft", gui_curve_prepare_log_line);
    if (repair.status == CURVE_PREFIX_REPAIR_NOT_NEEDED ||
        repair.status == CURVE_PREFIX_REPAIR_NO_LOCK) {
        return false;
    }
    char message[2048] = {};
    if (repair.status == CURVE_PREFIX_REPAIR_PREPARED) {
        curve_prefix_repair_format_preview(repair, plan, message, sizeof(message));
        const int answer = gc_message_box(g_app.hMainWnd, message,
            "Prepare Compatible Curve", MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) {
            record_ui_action("Prepare compatible curve declined (%d points)",
                repair.changedCount);
            set_profile_status_text(
                "Apply cancelled: earlier points rise above the lock. Nothing was changed.");
            return true;
        }
        if (!gui_curve_prepare_accept(repair)) {
            gc_message_box(g_app.hMainWnd,
                "The prepared points could not be placed in the editor, so the draft was"
                " not changed. Refresh and try again.",
                "Prepare Compatible Curve", MB_OK | MB_ICONWARNING);
            return true;
        }
        record_ui_action("Prepare compatible curve accepted (%d points, %u-%u mV)",
            repair.changedCount, repair.minVoltUv / 1000u, repair.maxVoltUv / 1000u);
        set_profile_status_text(
            "Prepared compatible curve: %d point%s lowered in the draft. Review, then press"
            " Apply.", repair.changedCount, repair.changedCount == 1 ? "" : "s");
        return true;
    }
    curve_plan_format_message(plan, true, message, sizeof(message));
    char why[768] = {};
    curve_prefix_repair_format_refusal(repair, why, sizeof(why));
    if (why[0]) {
        curve_plan_appendf(message, sizeof(message), "\n\n%s", why);
    }
    write_error_report_log_for_user_failure("GUI apply refused (curve prefix)", message);
    gc_message_box(g_app.hMainWnd, message, "Green Curve", MB_OK | MB_ICONWARNING);
    set_profile_status_text(
        "Apply refused: earlier points rise above the lock. Nothing was changed.");
    return true;
}
