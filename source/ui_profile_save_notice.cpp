// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Profile Save's honesty notice (profile_save_policy.h).  Included by
// ui_main.cpp ahead of the window procedure that saves.
//
// A no-edit Save stores the scalar domains from live state and no curve.  When
// the GPU carries a VF curve Green Curve did not set, the slot lacks it, and
// saying "saved the current GUI values" over the curve the editor was showing
// is what made the issue-17 round trip look like it had worked.  A saved lock
// that live flat-tail detection supplied, with pre-anchor offsets read from
// the GPU, is likewise labelled a detected approximation.  A saved lock that is
// the editor's projection of a flatten another tool set names the lower points
// that tool set, which the slot leaves out, and offers Import
// (gui_observed_lock_policy.h).

#include "profile_save_policy.h"

static ProfileSaveNotice report_profile_save_omissions(int slot,
    const DesiredSettings* saved, bool usedGuiCapture, bool lockFromUserEdit,
    bool* captureRequestedOut = nullptr) {
    if (captureRequestedOut) *captureRequestedOut = false;
    int componentKHz[VF_NUM_POINTS] = {};
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        componentKHz[ci] = saved->hasGpuOffset
            ? gpu_offset_component_mhz_for_point(ci, saved->gpuOffsetMHz,
                  saved->gpuOffsetExcludeLowCount) * 1000
            : 0;
    }
    int firstOmittedCi = -1;
    const int omitted = usedGuiCapture ? 0
        : profile_save_count_unsaved_programmed_points(g_app.curve, g_app.freqOffsets,
              componentKHz, &firstOmittedCi);
    const int inferred = profile_save_count_inferred_pre_anchor_points(saved, componentKHz);
    // Standalone (no service): this process is the writer, nothing to compare.
    const bool lockOwned = !g_app.usingBackgroundService ||
        (g_app.serviceActiveDesiredValid && g_app.serviceActiveDesired.hasLock &&
         g_app.serviceActiveDesired.lockCi == saved->lockCi);
    // The saved lock is the editor's projection of a flatten another tool set.
    const bool lockObserved = usedGuiCapture && saved->hasLock && gui_editor_lock_is_observed() &&
        saved->lockCi == g_app.observedLockCi;
    int firstBelowCi = -1;
    const int unsavedBelow = lockObserved
        ? profile_save_count_unsaved_pre_anchor_points(g_app.curve, g_app.freqOffsets,
              componentKHz, saved, &firstBelowCi)
        : 0;
    const ProfileSaveNotice notice = profile_save_notice(usedGuiCapture, omitted, saved,
        lockOwned, lockFromUserEdit, inferred, lockObserved);
    debug_log("profile-save: slot=%d source=%s notice=%s omittedProgrammedPoints=%d"
              " firstOmitted=ci%d inferredPreAnchor=%d lock=ci%d/%u MHz owned=%d userLock=%d"
              " observedLock=%d unsavedBelowLock=%d firstBelow=ci%d\n",
        slot, usedGuiCapture ? "gui-capture" : "live-scalars",
        profile_save_notice_name(notice), omitted, firstOmittedCi, inferred,
        saved->hasLock ? saved->lockCi : -1, saved->hasLock ? saved->lockMHz : 0u,
        lockOwned ? 1 : 0, lockFromUserEdit ? 1 : 0, lockObserved ? 1 : 0,
        unsavedBelow, firstBelowCi);
    char text[640] = {};
    if (notice == PROFILE_SAVE_NOTICE_OMITTED_EXTERNAL_CURVE) {
        const unsigned int mv = firstOmittedCi >= 0
            ? g_app.curve[firstOmittedCi].volt_uV / 1000u : 0u;
        StringCchPrintfA(text, ARRAY_COUNT(text),
            "Saved slot %d, but without the GPU's current VF curve.\n\n"
            "That curve was not set by Green Curve (%d programmed point%s, the first"
            " at %u mV), so the slot holds the GPU and memory offsets, power limit"
            " and fan settings only. Loading it later will not restore that curve.\n\n"
            "Open Import current GPU settings now?",
            slot, omitted, omitted == 1 ? "" : "s", mv);
        if (gc_message_box(g_app.hMainWnd, text, "Green Curve", MB_YESNO | MB_ICONWARNING) == IDYES) {
            if (captureRequestedOut) *captureRequestedOut = true;
        }
    } else if (notice == PROFILE_SAVE_NOTICE_OBSERVED_FLATTEN) {
        const unsigned int mv = g_app.curve[saved->lockCi].volt_uV / 1000u;
        char below[256] = {};
        if (unsavedBelow > 0) {
            StringCchPrintfA(below, ARRAY_COUNT(below),
                "The %d curve point%s below it (the first at %u mV) were set by that"
                " tool too and are not in the slot, so loading it applies this flatten"
                " to the stock curve below %u mV.\n\n",
                unsavedBelow, unsavedBelow == 1 ? "" : "s",
                g_app.curve[firstBelowCi].volt_uV / 1000u, mv);
        }
        StringCchPrintfA(text, ARRAY_COUNT(text),
            "Saved slot %d with the %u MHz flatten at %u mV that another tool set.\n\n"
            "%s"
            "Open Import current GPU settings for a reviewed copy of the whole curve?",
            slot, saved->lockMHz, mv, below);
        if (gc_message_box(g_app.hMainWnd, text, "Green Curve", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            if (captureRequestedOut) *captureRequestedOut = true;
        }
    } else if (notice == PROFILE_SAVE_NOTICE_DETECTED_APPROXIMATION) {
        const unsigned int mv = (saved->lockCi >= 0 && saved->lockCi < VF_NUM_POINTS)
            ? g_app.curve[saved->lockCi].volt_uV / 1000u : 0u;
        StringCchPrintfA(text, ARRAY_COUNT(text),
            "Saved slot %d with a %u MHz flatten at %u mV that was detected from the"
            " GPU's current curve, not set in Green Curve.\n\n"
            "%d lower point%s keep offsets read from the GPU. The slot is a detected"
            " approximation of that curve, not a reviewed capture; check it after"
            " loading.",
            slot, saved->lockMHz, mv, inferred, inferred == 1 ? "" : "s");
        gc_message_box(g_app.hMainWnd, text, "Green Curve", MB_OK | MB_ICONINFORMATION);
    }
    return notice;
}
