// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

GC_NOINLINE static void gui_profile_load_command() {
    do {
        int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
        if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
        slot += 1;
        if (!is_profile_slot_saved(g_app.configPath, slot)) {
            set_profile_status_text("Slot %d is empty. Save a profile first.", slot);
            break;
        }
        if (gui_profile_source_load(slot)) break;
        if (!maybe_confirm_profile_load_replace(slot)) break;
        auto desiredPtr = std::make_unique<DesiredSettings>();
        DesiredSettings& desired = *desiredPtr;
        char err[256] = {};
        if (!load_profile_from_config(g_app.configPath, slot, &desired, err, sizeof(err))) {
            write_error_report_log_for_user_failure("Profile load failed", err);
            gc_message_box(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
            break;
        }
        populate_desired_into_gui(&desired);
        set_gui_state_dirty(true);
        gui_draft_capture_desired(&desired);
        populate_global_controls();
        if (!set_config_int(g_app.configPath, "profiles", "selected_slot", slot)) {
            write_error_report_log_for_user_failure(
                "Selected profile persistence failed",
                "The profile loaded into the editor, but selected_slot failed readback");
            gc_message_box(g_app.hMainWnd,
                "The profile was loaded, but the selected slot could not be saved.",
                "Green Curve", MB_OK | MB_ICONWARNING);
        }
        refresh_profile_controls_from_config();
        set_profile_status_text("Loaded slot %d into the GUI. GPU settings were not applied.", slot);
        invalidate_main_window();
    } while (false);
}

GC_NOINLINE static void gui_profile_save_command() {
    do {
        int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
        if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
        slot += 1;
        if (gui_profile_source_save()) break;
        auto desiredPtr = std::make_unique<DesiredSettings>();
        DesiredSettings& desired = *desiredPtr;
        char err[256] = {};
        if (!gui_service_model_ready(&g_app.guiServiceModel)) {
            gui_service_io_queue_full_sync("profile save requested while reconnecting");
            gc_message_box(g_app.hMainWnd,
                "Live GPU state is reconnecting. Your draft is preserved; save it after synchronization completes.",
                "Green Curve", MB_OK | MB_ICONWARNING);
            break;
        }
        if (!validate_configured_gpu_selection_for_client(
                err, sizeof(err))) {
            gc_message_box(g_app.hMainWnd, err[0] ? err :
                "Select the intended GPU again before saving a profile.",
                "Green Curve", MB_OK | MB_ICONWARNING);
            break;
        }
        bool appliedCurveOrLock =
            (g_app.lockedCi >= 0 && g_app.lockedFreq > 0) ||
            (g_app.appliedLockMode != LOCK_MODE_NONE &&
             g_app.appliedLockFreq > 0);
        if (!appliedCurveOrLock) {
            for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
                if (g_app.appliedCurveMHz[ci]) {
                    appliedCurveOrLock = true;
                    break;
                }
            }
        }
        if (profile_save_uses_gui_capture(
                g_app.guiHasUserModifiedValues,
                gui_has_pending_curve_or_lock_edits(),
                appliedCurveOrLock)) {
            if (!capture_gui_config_settings(&desired, err, sizeof(err))) {
                write_error_report_log_for_user_failure("Profile save capture failed", err);
                gc_message_box(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
                break;
            }
            debug_log("PROFILE_SAVE: saving GUI curve intent (modified=%d curveOrLock=%d appliedCurveOrLock=%d)\n",
                g_app.guiHasUserModifiedValues ? 1 : 0,
                gui_has_pending_curve_or_lock_edits() ? 1 : 0,
                appliedCurveOrLock ? 1 : 0);
        } else {
            build_full_live_desired_settings(&desired);
            if (desired.hasFan && g_app.guiFanMode >= FAN_MODE_AUTO && g_app.guiFanMode <= FAN_MODE_CURVE) {
                int liveCapturedFanMode = desired.fanMode;
                desired.fanMode = g_app.guiFanMode;
                desired.fanAuto = desired.fanMode == FAN_MODE_AUTO;
                desired.fanPercent = desired.fanMode == FAN_MODE_FIXED ? clamp_percent(g_app.guiFanFixedPercent) : 0;
                copy_fan_curve(&desired.fanCurve, &g_app.guiFanCurve);
                ensure_valid_fan_curve_config(&desired.fanCurve);
                if (liveCapturedFanMode != desired.fanMode) {
                    debug_log("PROFILE_SAVE: preserved visible GUI fan intent %d over live captured fan mode %d\n",
                        desired.fanMode,
                        liveCapturedFanMode);
                }
            }
            debug_log("PROFILE_SAVE: no user edits, saving live state\n");
        }
        const bool usedGuiCapture = profile_save_uses_gui_capture(
            g_app.guiHasUserModifiedValues, gui_has_pending_curve_or_lock_edits(),
            appliedCurveOrLock);
        const bool lockFromUserEdit = gui_has_pending_curve_or_lock_edits();
        if (!save_profile_to_config(g_app.configPath, slot, &desired, err, sizeof(err))) {
            write_error_report_log_for_user_failure("Profile save failed", err);
            gc_message_box(g_app.hMainWnd, err, "Green Curve", MB_OK | MB_ICONERROR);
            break;
        }
        bool captureRequested = false;
        const ProfileSaveNotice saveNotice = report_profile_save_omissions(
            slot, &desired, usedGuiCapture, lockFromUserEdit, &captureRequested);
        populate_desired_into_gui(&desired);
        sync_applied_profile_from_service_metadata();
        refresh_profile_controls_from_config();
        if (saveNotice == PROFILE_SAVE_NOTICE_OMITTED_EXTERNAL_CURVE)
            set_profile_status_text("Saved slot %d without the GPU's current VF curve (not set by Green Curve).", slot);
        else if (saveNotice == PROFILE_SAVE_NOTICE_DETECTED_APPROXIMATION)
            set_profile_status_text("Saved slot %d with a flatten detected from the GPU's curve.", slot);
        else
            set_profile_status_text("Saved the current GUI values to slot %d.", slot);
        invalidate_main_window();
        if (captureRequested) {
            gui_profile_capture_begin();
        }
    } while (false);
}
