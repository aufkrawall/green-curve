// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Profile UI and Startup
// ============================================================================

#include "ui_control_projection.h"

#ifndef GREEN_CURVE_SERVICE_BINARY
// The per-operation UAC helper lands later in the include order
// (ui_main_window.cpp); handle_share_all_users_toggle() below is its only user
// outside that shard.
static bool run_elevated_command(const char* const* argv,
    const char* cancelledStatus, const char* failedPrefix);
#endif

static void resolve_profile_gpu_offset_state_for_save(const DesiredSettings* desired, int* gpuOffsetMHzOut, int* excludeLowCountOut) {
    resolve_effective_gpu_offset_state_for_config_save(desired, gpuOffsetMHzOut, excludeLowCountOut);
}


static void refresh_machine_logon_slot_cache() {
    int slot = 0;
    if (get_machine_logon_slot(&slot)) {
        g_app.machineLogonSlotCache = slot;
    }
}

// Refresh the cached shared-only policy state and whether this user is a machine
// admin (computed once — admin membership does not change at runtime).
static void refresh_restrict_policy_state() {
    bool restrict = false;
    get_machine_restrict_policy(&restrict);
    g_app.restrictPolicyActive = restrict;
    static int s_adminCache = -1;
    if (s_adminCache < 0) s_adminCache = current_user_is_local_admin() ? 1 : 0;
    g_app.currentUserIsLocalAdmin = (s_adminCache == 1);
}

// True when the current user is blocked from applying custom OC (admin policy on
// AND this user is not a machine admin) — they may only apply shared profiles.
static bool restricted_to_shared_profiles() {
    return g_app.restrictPolicyActive && !g_app.currentUserIsLocalAdmin;
}

static void update_share_all_users_check_state() {
    refresh_machine_logon_slot_cache();
    refresh_restrict_policy_state();
    bool sharedState = false;
    bool sharedPainted = ui_checkbox_state_get(&g_app.shareAllUsersPainted);
    bool sharedRepaint = false;

    // "Share with all users" checkbox is bound to the SELECTED profile slot.
    // Checked = that slot is published to the shared bank AND is the all-users
    // default logon profile (the coherent shared state).
    int slot = CONFIG_DEFAULT_SLOT;
    if (g_app.hShareAllUsersCheck) {
        int sel = g_app.hProfileCombo ? (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0) : -1;
        if (sel < 0 || sel > CONFIG_NUM_SLOTS - 1) sel = CONFIG_DEFAULT_SLOT - 1;
        slot = sel + 1;
        bool shared = is_machine_profile_slot_saved(slot) && g_app.machineLogonSlotCache == slot;
        char label[64] = {};
        StringCchPrintfA(label, ARRAY_COUNT(label), "Share slot %d with all users", slot);
        bool changed = gui_set_window_text_if_changed(
            g_app.hShareAllUsersCheck, label);
        // Keep the clickable rectangle on the caption after a slot change
        // (F-CHECKBOX-HIT); the layout pass does not run for a text change.
        if (changed) fit_themed_checkbox_to_label(g_app.hShareAllUsersCheck);
        // The tick is derived by themed_checkbox_checked_state(), so compare
        // against what was last PAINTED.  Asking the control (BM_GETCHECK on an
        // owner-draw button) always answered "unchecked" and therefore never
        // requested the repaint that clears a tick after unsharing.
        bool repaintTick = ui_checkbox_state_needs_repaint(
            &g_app.shareAllUsersPainted, shared);
        changed = repaintTick || changed;
        sharedState = shared;
        sharedRepaint = repaintTick;
        // Elevation is never the gate: when the GUI is not elevated, toggling
        // this requests UAC for just this operation, so users never start the
        // whole GUI elevated.  The service is the gate -- publishing a profile to
        // the shared bank exists so the service can apply it for other users, and
        // achieves nothing observable without one.  An ALREADY shared slot keeps
        // the checkbox live so it can be un-shared (F-ACTIONABLE rule 2).
        GuiServiceActionability shareActionable =
            gui_service_actionability_from_app();
        changed = gui_set_window_enabled_if_changed(
            g_app.hShareAllUsersCheck,
            gui_service_config_control_actionable(&shareActionable, shared))
            || changed;
        if (!IsWindowVisible(g_app.hShareAllUsersCheck)) {
            ShowWindow(g_app.hShareAllUsersCheck, SW_SHOW);
            changed = true;
        }
        if (changed)
            InvalidateRect(g_app.hShareAllUsersCheck, nullptr, FALSE);
    }

    // "Shared profiles" button is available to every user; it is only enabled
    // when the admin has published at least one profile to the shared bank.
    int sharedCount = 0;
    for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
        bool slotShared = is_machine_profile_slot_saved(s);
        g_app.machineProfileSlotSavedCache[s - 1] = slotShared;
        if (slotShared) sharedCount++;
    }
    // This function owns the count: it scans the MACHINE config, which the
    // user-config write hook cannot invalidate.  Cache it so the service-state
    // projection can re-assert the button without rescanning a second config
    // file on every tick.
    g_app.sharedProfileCountCache = sharedCount;
    if (g_app.hSharedProfilesBtn) {
        char label[48] = {};
        if (sharedCount > 0) StringCchPrintfA(label, ARRAY_COUNT(label), "Shared profiles (%d)...", sharedCount);
        else StringCchCopyA(label, ARRAY_COUNT(label), "Shared profiles...");
        bool changed = gui_set_window_text_if_changed(
            g_app.hSharedProfilesBtn, label);
        // Loading a shared profile writes the editor, so it needs a reachable
        // service for the same reason Load does.  This runs AFTER
        // update_profile_action_buttons() in refresh_profile_controls_from_config()
        // and standalone from the share toggle, so both writers must use this
        // exact expression or one silently undoes the other.
        GuiServiceActionability actionable = gui_service_actionability_from_app();
        changed = gui_set_window_enabled_if_changed(
            g_app.hSharedProfilesBtn,
            sharedCount > 0 && gui_service_capability_enabled(
                &actionable, GUI_SERVICE_CAP_PROFILE_EDIT)) || changed;
        if (!IsWindowVisible(g_app.hSharedProfilesBtn)) {
            ShowWindow(g_app.hSharedProfilesBtn, SW_SHOW);
            changed = true;
        }
        if (changed)
            InvalidateRect(g_app.hSharedProfilesBtn, nullptr, FALSE);
    }
    debug_log_on_change("share-all-users controls: refreshed (selSlot=%d machineSlot=%d sharedCount=%d shared=%d lastPainted=%d repaintRequested=%d)\n",
        slot, g_app.machineLogonSlotCache, sharedCount,
        sharedState ? 1 : 0, sharedPainted ? 1 : 0, sharedRepaint ? 1 : 0);
}

#ifndef GREEN_CURVE_SERVICE_BINARY
// "Share slot N with all users" was clicked.  Sharing publishes the SELECTED
// slot's data into the machine-wide bank AND makes it the all-users default
// logon profile in one action; unsharing reverses both.  Elevation is requested
// per operation so the GUI itself never has to run elevated.
static void handle_share_all_users_toggle() {
    int sel = g_app.hProfileCombo
        ? (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0) : -1;
    if (sel < 0 || sel > CONFIG_NUM_SLOTS - 1) sel = CONFIG_DEFAULT_SLOT - 1;
    int slot = sel + 1;
    bool currentlyShared = is_machine_profile_slot_saved(slot) &&
        g_app.machineLogonSlotCache == slot;

    // Sharing requires the slot to actually hold a saved profile.
    if (!currentlyShared && !is_profile_slot_saved(g_app.configPath, slot)) {
        gc_message_box(g_app.hMainWnd,
            "The selected profile slot is empty. Save a profile into this slot before sharing it with all users.",
            "Green Curve", MB_OK | MB_ICONINFORMATION);
        update_share_all_users_check_state();
        return;
    }

    bool ok = false;
    bool elevated = is_elevated();
    debug_log("share-all-users click: slot=%d currentlyShared=%d elevated=%d action=%s\n",
        slot, currentlyShared ? 1 : 0, elevated ? 1 : 0,
        currentlyShared ? "unshare" : "share");
    if (!elevated) {
        char slotArg[16] = {};
        StringCchPrintfA(slotArg, ARRAY_COUNT(slotArg), "%d", slot);
        const char* argv[] = {
            currentlyShared ? "--unshare-slot" : "--share-slot",
            slotArg,
            "--config",
            g_app.configPath,
            nullptr
        };
        ok = run_elevated_command(argv,
            currentlyShared
                ? "Administrator consent was cancelled; profile is still shared."
                : "Administrator consent was cancelled; profile was not shared.",
            currentlyShared
                ? "Stop sharing profile with all users"
                : "Share profile with all users");
    } else {
        char err[256] = {};
        ok = currentlyShared
            ? unshare_profile_slot_for_all_users(slot, err, sizeof(err))
            : share_profile_slot_for_all_users(g_app.configPath, slot, err, sizeof(err));
        if (!ok) {
            write_error_report_log_for_user_failure(
                "Share-with-all-users update failed", err[0] ? err : "Unknown error");
            gc_message_box(g_app.hMainWnd,
                err[0] ? err : "Failed to update the shared profile.",
                "Green Curve", MB_OK | MB_ICONERROR);
        }
    }
    if (ok) {
        set_profile_status_text(currentlyShared
            ? "Slot %d is no longer shared with all users."
            : "Slot %d is now shared with all users and applied on logon for users without their own profile.",
            slot);
    }
    debug_log("share-all-users click: slot=%d action=%s ok=%d\n",
        slot, currentlyShared ? "unshare" : "share", ok ? 1 : 0);
    update_share_all_users_check_state();
    refresh_profile_controls_from_config();
    if (ok) {
        // The machine default is an effective logon profile for this account, so
        // create/remove its authenticated handoff task independently of resident
        // tray startup.
        schedule_logon_combo_sync();
    }
}
#endif  // GREEN_CURVE_SERVICE_BINARY

static void refresh_profile_controls_from_config() {
    if (!g_app.hProfileCombo) return;
    refresh_machine_logon_slot_cache();
    int selectedSlot = get_config_int(g_app.configPath, "profiles", "selected_slot", CONFIG_DEFAULT_SLOT);
    int appLaunchSlot = get_config_int(g_app.configPath, "profiles", "app_launch_slot", 0);
    int logonSlot = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);
    // Per-account "apply admin shared profile N at my logon" (overrides logon_slot
    // and the all-users default for this account).  Preserve the selector even
    // while the shared bank is temporarily unavailable; background reads never
    // have authority to erase an explicit user choice.
    int logonSharedSlot = get_config_int(g_app.configPath, "profiles", "logon_shared_slot", 0);
    if (logonSharedSlot < 0 || logonSharedSlot > CONFIG_NUM_SLOTS) logonSharedSlot = 0;

    SendMessageA(g_app.hProfileCombo, WM_SETREDRAW, FALSE, 0);
    SendMessageA(g_app.hProfileCombo, CB_RESETCONTENT, 0, 0);
    for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
        char label[32] = {};
        StringCchPrintfA(label, ARRAY_COUNT(label), "Slot %d - %s", s,
            is_profile_slot_saved(g_app.configPath, s) ? "Saved" : "Empty");
        SendMessageA(g_app.hProfileCombo, CB_ADDSTRING, 0, (LPARAM)label);
    }
    SendMessageA(g_app.hProfileCombo, CB_SETDROPPEDWIDTH, (WPARAM)dp(170), 0);

    if (g_app.hAppLaunchCombo) {
        SendMessageA(g_app.hAppLaunchCombo, WM_SETREDRAW, FALSE, 0);
        SendMessageA(g_app.hAppLaunchCombo, CB_RESETCONTENT, 0, 0);
        SendMessageA(g_app.hAppLaunchCombo, CB_ADDSTRING, 0, (LPARAM)"Disabled");
        for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
            char label[32] = {};
            StringCchPrintfA(label, ARRAY_COUNT(label), "Slot %d - %s", s,
                is_profile_slot_saved(g_app.configPath, s) ? "Saved" : "Empty");
            SendMessageA(g_app.hAppLaunchCombo, CB_ADDSTRING, 0, (LPARAM)label);
        }
        SendMessageA(g_app.hAppLaunchCombo, CB_SETDROPPEDWIDTH, (WPARAM)dp(180), 0);
    }
    if (g_app.hLogonCombo) {
        // Unified per-account logon selector.  Index 0 is "no personal choice" and
        // surfaces the admin all-users default when one is published; then this
        // user's own slots (hidden for restricted users — the service ignores a
        // per-user logon_slot for them under the shared-only policy); then each
        // admin-published shared profile.  Every item carries CB_SETITEMDATA so the
        // handler maps a selection to its meaning regardless of ordering:
        //   0                         -> no personal choice (admin default applies)
        //   1..CONFIG_NUM_SLOTS       -> per-user logon_slot
        //   LOGON_COMBO_SHARED_FLAG|N -> admin shared bank slot N (logon_shared_slot)
        bool restrictedShared = restricted_to_shared_profiles();
        int machineDefault = g_app.machineLogonSlotCache;
        if (machineDefault < 0 || machineDefault > CONFIG_NUM_SLOTS) machineDefault = 0;
        bool haveMachineDefault = machineDefault > 0 && is_machine_profile_slot_saved(machineDefault);

        SendMessageA(g_app.hLogonCombo, WM_SETREDRAW, FALSE, 0);
        SendMessageA(g_app.hLogonCombo, CB_RESETCONTENT, 0, 0);

        int logonSelIndex = 0;   // default to "no personal choice"
        int comboIndex = 0;

        char noneLabel[64] = {};
        if (haveMachineDefault) {
            StringCchPrintfA(noneLabel, ARRAY_COUNT(noneLabel), "Use admin's default (Shared profile %d)", machineDefault);
        } else {
            StringCchCopyA(noneLabel, ARRAY_COUNT(noneLabel), "Disabled");
        }
        SendMessageA(g_app.hLogonCombo, CB_ADDSTRING, 0, (LPARAM)noneLabel);
        SendMessageA(g_app.hLogonCombo, CB_SETITEMDATA, (WPARAM)comboIndex, (LPARAM)0);
        comboIndex++;

        if (!restrictedShared) {
            for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
                char label[40] = {};
                StringCchPrintfA(label, ARRAY_COUNT(label), "Slot %d - %s", s,
                    is_profile_slot_saved(g_app.configPath, s) ? "Saved" : "Empty");
                SendMessageA(g_app.hLogonCombo, CB_ADDSTRING, 0, (LPARAM)label);
                SendMessageA(g_app.hLogonCombo, CB_SETITEMDATA, (WPARAM)comboIndex, (LPARAM)s);
                if (logonSharedSlot == 0 && logonSlot == s) logonSelIndex = comboIndex;
                comboIndex++;
            }
        }

        for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
            if (!is_machine_profile_slot_saved(s)) continue;
            char label[48] = {};
            StringCchPrintfA(label, ARRAY_COUNT(label), "Always use: Shared profile %d", s);
            SendMessageA(g_app.hLogonCombo, CB_ADDSTRING, 0, (LPARAM)label);
            SendMessageA(g_app.hLogonCombo, CB_SETITEMDATA, (WPARAM)comboIndex, (LPARAM)(LOGON_COMBO_SHARED_FLAG | s));
            if (logonSharedSlot == s) logonSelIndex = comboIndex;
            comboIndex++;
        }
        if (logonSharedSlot > 0 &&
            !is_machine_profile_slot_saved(logonSharedSlot)) {
            char label[72] = {};
            StringCchPrintfA(label, ARRAY_COUNT(label),
                "Shared profile %d (temporarily unavailable)",
                logonSharedSlot);
            SendMessageA(g_app.hLogonCombo, CB_ADDSTRING, 0,
                (LPARAM)label);
            SendMessageA(g_app.hLogonCombo, CB_SETITEMDATA,
                (WPARAM)comboIndex,
                (LPARAM)(LOGON_COMBO_SHARED_FLAG | logonSharedSlot));
            logonSelIndex = comboIndex;
            comboIndex++;
        }

        SendMessageA(g_app.hLogonCombo, CB_SETCURSEL, (WPARAM)logonSelIndex, 0);
        SendMessageA(g_app.hLogonCombo, CB_SETDROPPEDWIDTH, (WPARAM)dp(220), 0);
    }

    if (appLaunchSlot < 0 || appLaunchSlot > CONFIG_NUM_SLOTS) appLaunchSlot = 0;
    if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;
    if (selectedSlot < 1 || selectedSlot > CONFIG_NUM_SLOTS) selectedSlot = CONFIG_DEFAULT_SLOT;
    SendMessageA(g_app.hProfileCombo, CB_SETCURSEL, (WPARAM)(selectedSlot - 1), 0);

    if (appLaunchSlot >= 0 && appLaunchSlot <= CONFIG_NUM_SLOTS)
        SendMessageA(g_app.hAppLaunchCombo, CB_SETCURSEL, (WPARAM)appLaunchSlot, 0);
    // The Logon combo selection is set during its (item-data-tagged) population above.

    SendMessageA(g_app.hProfileCombo, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_app.hProfileCombo, nullptr, TRUE);
    if (g_app.hAppLaunchCombo) {
        SendMessageA(g_app.hAppLaunchCombo, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_app.hAppLaunchCombo, nullptr, TRUE);
    }
    if (g_app.hLogonCombo) {
        SendMessageA(g_app.hLogonCombo, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_app.hLogonCombo, nullptr, TRUE);
    }
    if (g_app.hStartOnLogonCheck) {
        // Owner-draw: the tick comes from is_start_on_logon_enabled() at paint
        // time, so the only thing to do here is request a repaint when the
        // config no longer matches what is on screen.
        if (ui_checkbox_state_needs_repaint(&g_app.startOnLogonPainted,
                is_start_on_logon_enabled(g_app.configPath))) {
            InvalidateRect(g_app.hStartOnLogonCheck, nullptr, FALSE);
        }
    }

    update_profile_state_label();
    update_profile_action_buttons();
    update_background_service_controls();
    update_share_all_users_check_state();
    update_tray_icon();
}

#ifndef GREEN_CURVE_SERVICE_BINARY
static bool set_logon_profile_selection_atomic(const char* path,
    int perUserSlot, int sharedSlot, char* err, size_t errSize);
#endif

static void migrate_legacy_config_if_needed(const char* path) {
    if (!path) return;
    char test[8] = {};
    gc_GetPrivateProfileStringUtf8("meta", "format_version", "_X", test, sizeof(test), path);
    if (strcmp(test, "_X") != 0) return;

    gc_GetPrivateProfileStringUtf8("controls", "gpu_offset_mhz", "_X", test, sizeof(test), path);
    if (strcmp(test, "_X") == 0) return;

    DesiredSettings desired = {};
    char err[256] = {};
    if (load_desired_settings_from_ini(path, &desired, err, sizeof(err))) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!desired.hasCurvePoint[i] && g_app.curve[i].freq_kHz > 0) {
                desired.hasCurvePoint[i] = true;
                desired.curvePointMHz[i] = displayed_curve_mhz(g_app.curve[i].freq_kHz);
            }
        }
        if (!desired.hasGpuOffset) { desired.hasGpuOffset = true; desired.gpuOffsetMHz = g_app.gpuClockOffsetkHz / 1000; }
        if (!desired.hasMemOffset) { desired.hasMemOffset = true; desired.memOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz); }
        if (!desired.hasPowerLimit) { desired.hasPowerLimit = true; desired.powerLimitPct = g_app.powerLimitPct; }
        if (!desired.hasFan) {
            // If the legacy config had a [fan_curve] section, preserve it instead
            // of overwriting with the runtime active curve.
            if (config_section_has_keys(path, "fan_curve")) {
                desired.hasFan = true;
                desired.fanMode = FAN_MODE_CURVE;
                desired.fanAuto = false;
            } else {
                desired.hasFan = true;
                desired.fanMode = g_app.activeFanMode;
                desired.fanAuto = g_app.activeFanMode == FAN_MODE_AUTO;
                desired.fanPercent = g_app.activeFanFixedPercent;
                copy_fan_curve(&desired.fanCurve, &g_app.activeFanCurve);
            }
        }

        bool wasStartupEnabled = false;
        load_startup_enabled_from_config(path, &wasStartupEnabled);

        if (!save_profile_to_config(path, 1, &desired, err, sizeof(err))) {
            debug_log("legacy config migration: profile save failed, leaving legacy config retryable: %s\n",
                err[0] ? err : "unknown error");
            return;
        }
        if (wasStartupEnabled) {
        #ifndef GREEN_CURVE_SERVICE_BINARY
            if (!set_logon_profile_selection_atomic(path, 1, 0,
                    err, sizeof(err))) {
                debug_log("legacy config migration: atomic logon selection failed: %s\n",
                    err[0] ? err : "unknown error");
            }
        #endif
        }
    }
}

static void merge_desired_settings(DesiredSettings* base, const DesiredSettings* override) {
    if (!base || !override) return;
    if (override->hasGpuOffset) {
        base->hasGpuOffset = true;
        base->gpuOffsetMHz = override->gpuOffsetMHz;
        base->gpuOffsetExcludeLowCount = override->gpuOffsetExcludeLowCount;
    }
    // Lock state is a single unit: ci + target MHz + mode (none/flatten/hard) +
    // anchor tracking. Merging it field-by-field here keeps every caller from
    // having to remember the individual fields. Dropping lockMode was the root
    // cause of pinned (hard) locks being persisted as flatten on profile save.
    if (override->hasLock) {
        base->hasLock = true;
        base->lockCi = override->lockCi;
        base->lockMHz = override->lockMHz;
        base->lockMode = override->lockMode;
        base->lockTracksAnchor = override->lockTracksAnchor;
    }
    if (override->hasMemOffset) {
        base->hasMemOffset = true;
        base->memOffsetMHz = override->memOffsetMHz;
    }
    if (override->hasPowerLimit) {
        base->hasPowerLimit = true;
        base->powerLimitPct = override->powerLimitPct;
    }
    if (override->hasXbarOffsetKhz) {
        base->hasXbarOffsetKhz = true;
        base->xbarOffsetKhz = override->xbarOffsetKhz;
    }
    if (override->hasXbarMsvddOffsetUv) {
        base->hasXbarMsvddOffsetUv = true;
        base->xbarMsvddOffsetUv = override->xbarMsvddOffsetUv;
    }
    if (override->hasSysClkOffsetKhz) {
        base->hasSysClkOffsetKhz = true;
        base->sysClkOffsetKhz = override->sysClkOffsetKhz;
    }
    if (override->hasVideoClkOffsetKhz) {
        base->hasVideoClkOffsetKhz = true;
        base->videoClkOffsetKhz = override->videoClkOffsetKhz;
    }
    if (override->hasFan) {
        base->hasFan = true;
        base->fanMode = override->fanMode;
        base->fanAuto = override->fanAuto;
        base->fanPercent = override->fanPercent;
        copy_fan_curve(&base->fanCurve, &override->fanCurve);
    }
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (override->hasCurvePoint[i]) {
            base->hasCurvePoint[i] = true;
            base->curvePointMHz[i] = override->curvePointMHz[i];
        }
    }
}

static bool desired_has_any_action(const DesiredSettings* desired) {
    if (!desired) return false;
    // hasLock counts: a pin-only (hard lock) profile carries no curve points
    // or offsets but still demands an NVML locked-clocks apply.
    if (desired->hasGpuOffset || desired->hasMemOffset || desired->hasPowerLimit ||
        desired->hasFan || desired->hasLock || desired->hasXbarOffsetKhz ||
        desired->hasXbarMsvddOffsetUv) return true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i]) return true;
    }
    return false;
}

static void infer_profile_lock_from_curve(const DesiredSettings* desired, int* lockCiOut, unsigned int* lockMHzOut) {
    if (lockCiOut) *lockCiOut = -1;
    if (lockMHzOut) *lockMHzOut = 0;
    if (!desired) return;

    int visiblePoints[VF_NUM_POINTS] = {};
    int visibleCount = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (!desired->hasCurvePoint[ci]) continue;
        if (!is_curve_point_visible_in_gui(ci)) continue;
        visiblePoints[visibleCount++] = ci;
    }
    if (visibleCount < 2) return;

    for (int visibleIndex = 0; visibleIndex < visibleCount - 1; visibleIndex++) {
        int ci = visiblePoints[visibleIndex];
        unsigned int lockMHz = desired->curvePointMHz[ci];
        if (lockMHz == 0) continue;

        bool hasTail = false;
        bool allSame = true;
        for (int tailIndex = visibleIndex + 1; tailIndex < visibleCount; tailIndex++) {
            int tailCi = visiblePoints[tailIndex];
            hasTail = true;
            if (desired->curvePointMHz[tailCi] != lockMHz) {
                allSame = false;
                break;
            }
        }

        if (hasTail && allSame) {
            if (lockCiOut) *lockCiOut = ci;
            if (lockMHzOut) *lockMHzOut = lockMHz;
            return;
        }
    }
}

#ifndef GREEN_CURVE_SERVICE_BINARY
static bool commit_logon_profiles_section(const char* path,
    const char* profilesSectionText, void*, char* err, size_t errSize) {
    const char* replacedSections[] = { "profiles" };
    return write_config_sections_atomic(path, profilesSectionText, replacedSections,
        ARRAY_COUNT(replacedSections), err, errSize);
}

// Save the mutually-exclusive account logon choices as one transaction.  The
// caller synchronizes Task Scheduler only after this succeeds; scheduler repair
// failure must never roll back an already-saved user choice.
static bool set_logon_profile_selection_atomic(const char* path, int perUserSlot,
    int sharedSlot, char* err, size_t errSize) {
    bool ok = update_logon_profile_selection_transaction(path, perUserSlot,
        sharedSlot, commit_logon_profiles_section, nullptr, err, errSize);
    if (ok) {
        invalidate_tray_profile_cache();
        debug_log("logon profile selection: committed atomically (perUserSlot=%d sharedSlot=%d)\n",
            perUserSlot, sharedSlot);
    }
    return ok;
}

static LRESULT logon_combo_item_data_from_slots(int perUserSlot, int sharedSlot) {
    return (LRESULT)logon_profile_selection_item_data(perUserSlot, sharedSlot);
}

static bool select_logon_combo_item_by_data(HWND combo, LRESULT itemData) {
    if (!combo) return false;
    LRESULT count = SendMessageA(combo, CB_GETCOUNT, 0, 0);
    if (count == CB_ERR || count < 0) return false;
    for (LRESULT index = 0; index < count; ++index) {
        LRESULT candidate = SendMessageA(combo, CB_GETITEMDATA, (WPARAM)index, 0);
        if (candidate == itemData) {
            return SendMessageA(combo, CB_SETCURSEL, (WPARAM)index, 0) != CB_ERR;
        }
    }
    return false;
}

// The service explicitly owns profile identity.  Never infer it by comparing
// absolute VF MHz against temperature/boost-sensitive live telemetry.  Shared
// and machine-bank slots intentionally do not masquerade as the same-numbered
// personal slot in the tray's personal-profile checkmarks.
//
// The stored profile is read with PROFILE_READ_FOR_OWNERSHIP, which is the
// whole point of that mode: the ordinary editor read projects the record onto
// the LIVE curve, so the same file decoded differently as the GPU boosted and
// this comparison flipped on its own.  Because none of that live state is part
// of the cache key below, the flip was invisible until the next event that did
// invalidate the cache -- most often an ordinary profile Load, which writes
// selected_slot -- at which point the stale verdict was written out as
// applied_slot=0 and the tray tick vanished until the next Apply.  See
// applied_profile_indicator_policy.h.
static void sync_applied_profile_from_service_metadata() {
    AppliedProfileSyncCache inputs = current_applied_profile_sync_inputs();
    if (applied_profile_sync_inputs_unchanged(inputs)) return;

    // The tray tooltip names the active profile from applied_slot plus the
    // service's own view of it. A config write invalidates the tray cache on its
    // own, but the service view can move without one -- switching between two
    // shared slots leaves applied_slot at 0 -- so drop the cached label here,
    // where a genuine identity change has just been established.
    invalidate_tray_profile_cache();

    AppliedProfileIndicatorInputs decision = {};
    decision.serviceAuthoritative = g_app.serviceSnapshotAuthoritative;
    decision.activeDesiredValid = g_app.serviceActiveDesiredValid;
    decision.profileSource = (unsigned int)g_app.serviceActiveProfileSource;
    decision.profileSlot = g_app.serviceActiveProfileSlot;
    decision.maxSlots = CONFIG_NUM_SLOTS;

    char matchDetail[256] = {};
    char loadErr[256] = {};
    int candidateSlot = applied_user_slot_from_service_profile(
        g_app.serviceActiveProfileSource, g_app.serviceActiveProfileSlot);
    if (decision.serviceAuthoritative && decision.activeDesiredValid &&
        candidateSlot > 0) {
        DesiredSettings savedProfile = {};
        decision.profileReadable = load_profile_from_config(g_app.configPath,
            candidateSlot, &savedProfile, loadErr, sizeof(loadErr),
            PROFILE_READ_FOR_OWNERSHIP);
        if (decision.profileReadable) {
            decision.intentMatchesProfile =
                desired_settings_match_active_service_intent(
                    &savedProfile, &g_app.serviceActiveDesired,
                    matchDetail, sizeof(matchDetail), true);
        }
    }

    AppliedProfileIndicatorReason reason = APPLIED_PROFILE_REASON_NO_AUTHORITY;
    int appliedSlot = applied_profile_indicator_slot(decision, &reason);

    // Every evaluation is logged, not only the transitions: "why is my tick
    // gone" has to be answerable from one run's log, and a decision that keeps
    // the current value is exactly as interesting as one that changes it.
    // debug_log_on_change() keeps the 1 Hz telemetry tick from burying it.
    debug_log_on_change("applied profile metadata sync: verdict slot=%d reason=%s "
        "(authoritative=%d activeIntent=%d source=%u serviceSlot=%u candidate=%d "
        "readable=%d matches=%d detail=%s)\n",
        appliedSlot, applied_profile_indicator_reason_name(reason),
        decision.serviceAuthoritative ? 1 : 0,
        decision.activeDesiredValid ? 1 : 0,
        decision.profileSource, decision.profileSlot, candidateSlot,
        decision.profileReadable ? 1 : 0,
        decision.intentMatchesProfile ? 1 : 0,
        matchDetail[0] ? matchDetail : (loadErr[0] ? loadErr : "-"));

    int persisted = get_config_int(g_app.configPath, "profiles", "applied_slot", 0);
    if (persisted < 0 || persisted > CONFIG_NUM_SLOTS) persisted = 0;
    if (persisted == appliedSlot) {
        g_appliedProfileSyncCache = inputs;
        return;
    }
    if (!set_config_int(g_app.configPath, "profiles", "applied_slot", appliedSlot)) {
        debug_log("applied profile metadata sync: failed to persist applied_slot=%d (reason=%s source=%u slot=%u authoritative=%d)\n",
            appliedSlot, applied_profile_indicator_reason_name(reason),
            decision.profileSource, decision.profileSlot,
            decision.serviceAuthoritative ? 1 : 0);
        return;
    }
    debug_log("applied profile metadata sync: applied_slot %d -> %d reason=%s source=%u slot=%u authoritative=%d detail=%s (saved selection unchanged)\n",
        persisted, appliedSlot, applied_profile_indicator_reason_name(reason),
        decision.profileSource, decision.profileSlot,
        decision.serviceAuthoritative ? 1 : 0,
        matchDetail[0] ? matchDetail : (loadErr[0] ? loadErr : "-"));
    update_tray_icon();
    // set_config_int() atomically changed the file metadata. Capture the
    // resulting stamp so the next telemetry snapshot remains a cache hit.
    g_appliedProfileSyncCache = current_applied_profile_sync_inputs();
}

#include "config_profiles_gui_state.cpp"

// GUI startup/logon orchestration is isolated in main_startup_profiles.cpp so
// profile-control rendering and configuration code stay independently readable.
#endif
