// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Profile UI and Startup
// ============================================================================

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
        SetWindowTextA(g_app.hShareAllUsersCheck, label);
        SendMessageA(g_app.hShareAllUsersCheck, BM_SETCHECK, (WPARAM)(shared ? BST_CHECKED : BST_UNCHECKED), 0);
        // Always enabled.  When the GUI is not elevated, toggling it requests UAC
        // for just this operation, so users never start the whole GUI elevated.
        EnableWindow(g_app.hShareAllUsersCheck, TRUE);
        ShowWindow(g_app.hShareAllUsersCheck, SW_SHOW);
        InvalidateRect(g_app.hShareAllUsersCheck, nullptr, TRUE);
        UpdateWindow(g_app.hShareAllUsersCheck);
    }

    // "Shared profiles" button is available to every user; it is only enabled
    // when the admin has published at least one profile to the shared bank.
    int sharedCount = 0;
    for (int s = 1; s <= CONFIG_NUM_SLOTS; s++) {
        if (is_machine_profile_slot_saved(s)) sharedCount++;
    }
    if (g_app.hSharedProfilesBtn) {
        char label[48] = {};
        if (sharedCount > 0) StringCchPrintfA(label, ARRAY_COUNT(label), "Shared profiles (%d)...", sharedCount);
        else StringCchCopyA(label, ARRAY_COUNT(label), "Shared profiles...");
        SetWindowTextA(g_app.hSharedProfilesBtn, label);
        EnableWindow(g_app.hSharedProfilesBtn, sharedCount > 0 ? TRUE : FALSE);
        ShowWindow(g_app.hSharedProfilesBtn, SW_SHOW);
        InvalidateRect(g_app.hSharedProfilesBtn, nullptr, TRUE);
        UpdateWindow(g_app.hSharedProfilesBtn);
    }
    debug_log_on_change("share-all-users controls: refreshed (selSlot=%d machineSlot=%d sharedCount=%d)\n",
        slot, g_app.machineLogonSlotCache, sharedCount);
}

static void refresh_profile_controls_from_config() {
    if (!g_app.hProfileCombo) return;
    refresh_machine_logon_slot_cache();
    int selectedSlot = get_config_int(g_app.configPath, "profiles", "selected_slot", CONFIG_DEFAULT_SLOT);
    int appLaunchSlot = get_config_int(g_app.configPath, "profiles", "app_launch_slot", 0);
    int logonSlot = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);
    // Per-account "apply admin shared profile N at my logon" (overrides logon_slot
    // and the all-users default for this account).  A stale value (slot no longer
    // published) is treated as unset for display; the apply path self-heals it.
    int logonSharedSlot = get_config_int(g_app.configPath, "profiles", "logon_shared_slot", 0);
    if (logonSharedSlot < 0 || logonSharedSlot > CONFIG_NUM_SLOTS) logonSharedSlot = 0;
    if (logonSharedSlot > 0 && !is_machine_profile_slot_saved(logonSharedSlot)) logonSharedSlot = 0;

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
            StringCchPrintfA(noneLabel, ARRAY_COUNT(noneLabel), "Admin default: Shared profile %d", machineDefault);
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
            StringCchPrintfA(label, ARRAY_COUNT(label), "Shared profile %d (admin)", s);
            SendMessageA(g_app.hLogonCombo, CB_ADDSTRING, 0, (LPARAM)label);
            SendMessageA(g_app.hLogonCombo, CB_SETITEMDATA, (WPARAM)comboIndex, (LPARAM)(LOGON_COMBO_SHARED_FLAG | s));
            if (logonSharedSlot == s) logonSelIndex = comboIndex;
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
        SendMessageA(g_app.hStartOnLogonCheck, BM_SETCHECK,
            (WPARAM)(is_start_on_logon_enabled(g_app.configPath) ? BST_CHECKED : BST_UNCHECKED), 0);
    }

    update_profile_state_label();
    update_profile_action_buttons();
    refresh_background_service_state();
    update_background_service_controls();
    update_share_all_users_check_state();
    update_tray_icon();
}

static void migrate_legacy_config_if_needed(const char* path) {
    if (!path) return;
    char test[8] = {};
    GetPrivateProfileStringA("meta", "format_version", "_X", test, sizeof(test), path);
    if (strcmp(test, "_X") != 0) return;

    GetPrivateProfileStringA("controls", "gpu_offset_mhz", "_X", test, sizeof(test), path);
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

        save_profile_to_config(path, 1, &desired, err, sizeof(err));
        if (wasStartupEnabled) {
            set_config_int(path, "profiles", "logon_slot", 1);
        }
        set_config_int(path, "profiles", "selected_slot", 1);
        set_config_int(path, "profiles", "app_launch_slot", 0);
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
    if (desired->hasGpuOffset || desired->hasMemOffset || desired->hasPowerLimit || desired->hasFan || desired->hasLock) return true;
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
static void populate_desired_into_gui(const DesiredSettings* desired) {
    if (!desired) return;
    bool preserveDirty = gui_state_dirty();
    unlock_all();
    if (g_app.loaded) populate_edits();
    begin_programmatic_edit_update();
    set_gui_state_dirty(false);
    g_app.guiHasUserModifiedValues = false;
    // Any populate clears the "loaded shared slot" marker; show_shared_profiles_menu
    // re-sets it AFTER calling this for a shared load.
    g_app.loadedSharedSlot = 0;

    // Curve points
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        g_app.guiCurvePointExplicit[ci] = desired->hasCurvePoint[ci];
        if (g_app.hEditsMhz[vi]) {
            unsigned int mhz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
            if (desired->hasCurvePoint[ci]) mhz = desired->curvePointMHz[ci];
            set_edit_value(g_app.hEditsMhz[vi], mhz);
        }
    }
    // GPU offset
    if (desired->hasGpuOffset) {
        g_app.guiGpuOffsetMHz = desired->gpuOffsetMHz;
        g_app.guiGpuOffsetExcludeLowCount = desired->gpuOffsetExcludeLowCount;
    }
    if (desired->hasGpuOffset && g_app.hGpuOffsetEdit) {
        set_edit_value(g_app.hGpuOffsetEdit, desired->gpuOffsetMHz);
        if (g_app.hGpuOffsetExcludeLowEdit) {
            char excludeBuf[16] = {};
            StringCchPrintfA(excludeBuf, ARRAY_COUNT(excludeBuf), "%d", desired->gpuOffsetExcludeLowCount);
            SetWindowTextA(g_app.hGpuOffsetExcludeLowEdit, excludeBuf);
        }
    }
    // Mem offset
    if (desired->hasMemOffset && g_app.hMemOffsetEdit) {
        set_edit_value(g_app.hMemOffsetEdit, desired->memOffsetMHz);
    }
    // Power limit
    if (desired->hasPowerLimit && g_app.hPowerLimitEdit) {
        set_edit_value(g_app.hPowerLimitEdit, desired->powerLimitPct);
    }
    // Fan
    if (desired->hasFan) {
        g_app.guiFanMode = desired->fanMode;
        if (desired->fanMode == FAN_MODE_FIXED) {
            g_app.guiFanFixedPercent = clamp_percent(desired->fanPercent);
        } else {
            g_app.guiFanFixedPercent = current_displayed_fan_percent();
        }
        copy_fan_curve(&g_app.guiFanCurve, &desired->fanCurve);
        ensure_valid_fan_curve_config(&g_app.guiFanCurve);
        if (g_app.hFanModeCombo) {
            SendMessageA(g_app.hFanModeCombo, CB_SETCURSEL, (WPARAM)g_app.guiFanMode, 0);
        }
        if (g_app.hFanEdit) {
            char fanText[16] = {};
            StringCchPrintfA(fanText, ARRAY_COUNT(fanText), "%d", g_app.guiFanFixedPercent);
            SetWindowTextA(g_app.hFanEdit, fanText);
        }
        refresh_fan_curve_button_text();
        update_fan_controls_enabled_state();
    }

    int lockCi = desired->hasLock ? desired->lockCi : -1;
    unsigned int lockMHz = desired->hasLock ? desired->lockMHz : 0;
    if (lockCi < 0 || lockMHz == 0) {
        infer_profile_lock_from_curve(desired, &lockCi, &lockMHz);
    }
    if (lockCi >= 0 && lockMHz > 0) {
        for (int vi = 0; vi < g_app.numVisible; vi++) {
            if (g_app.visibleMap[vi] != lockCi) continue;
            set_edit_value(g_app.hEditsMhz[vi], lockMHz);
            LockMode mode = desired->hasLock ? (desired->lockMode != LOCK_MODE_NONE ? desired->lockMode : LOCK_MODE_FLATTEN) : LOCK_MODE_FLATTEN;
            apply_lock(vi, mode);
            g_app.guiLockTracksAnchor = desired->hasLock ? desired->lockTracksAnchor : true;
            break;
        }
    } else if (g_app.lockedVi >= 0) {
        SendMessageA(g_app.hLocks[g_app.lockedVi], BM_SETCHECK, BST_UNCHECKED, 0);
        g_app.lockedVi = -1;
        g_app.lockedCi = -1;
        g_app.lockedFreq = 0;
        g_app.lockMode = LOCK_MODE_NONE;
    }
    end_programmatic_edit_update();
    set_gui_state_dirty(preserveDirty);
}

static void set_profile_status_text(const char* fmt, ...) {
    if (!g_app.hProfileStatusLabel || !fmt) return;
    char buf[256] = {};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfA(buf, ARRAY_COUNT(buf), fmt, ap);
    va_end(ap);
    SetWindowTextA(g_app.hProfileStatusLabel, buf);
}

static void update_profile_state_label() {
    if (!g_app.hProfileStateLabel || !g_app.hProfileCombo) return;
    refresh_machine_logon_slot_cache();
    int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
    if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
    slot += 1;

    bool saved = is_profile_slot_saved(g_app.configPath, slot);
    bool isAppLaunch = (get_config_int(g_app.configPath, "profiles", "app_launch_slot", 0) == slot);
    bool isLogon = (get_config_int(g_app.configPath, "profiles", "logon_slot", 0) == slot);
    bool isMachineDefault = (slot == g_app.machineLogonSlotCache && g_app.machineLogonSlotCache > 0);
    bool isMachineProfileBank = is_machine_profile_slot_saved(slot);

    char roles[96] = {};
    if (isAppLaunch && isLogon && isMachineDefault && isMachineProfileBank) StringCchCopyA(roles, ARRAY_COUNT(roles), " | app start + logon + all users + shared");
    else if (isAppLaunch && isLogon && isMachineDefault) StringCchCopyA(roles, ARRAY_COUNT(roles), " | app start + logon + all users");
    else if (isAppLaunch && isLogon && isMachineProfileBank) StringCchCopyA(roles, ARRAY_COUNT(roles), " | app start + logon + shared");
    else if (isAppLaunch && isMachineDefault && isMachineProfileBank) StringCchCopyA(roles, ARRAY_COUNT(roles), " | app start + all users + shared");
    else if (isLogon && isMachineDefault && isMachineProfileBank) StringCchCopyA(roles, ARRAY_COUNT(roles), " | logon + all users + shared");
    else if (isMachineDefault && isMachineProfileBank) StringCchCopyA(roles, ARRAY_COUNT(roles), " | all users + shared");
    else if (isAppLaunch && isLogon) StringCchCopyA(roles, ARRAY_COUNT(roles), " | app start + logon");
    else if (isAppLaunch && isMachineDefault) StringCchCopyA(roles, ARRAY_COUNT(roles), " | app start + all users");
    else if (isAppLaunch && isMachineProfileBank) StringCchCopyA(roles, ARRAY_COUNT(roles), " | app start + shared");
    else if (isLogon && isMachineDefault) StringCchCopyA(roles, ARRAY_COUNT(roles), " | logon + all users");
    else if (isLogon && isMachineProfileBank) StringCchCopyA(roles, ARRAY_COUNT(roles), " | logon + shared");
    else if (isMachineDefault) StringCchCopyA(roles, ARRAY_COUNT(roles), " | all users");
    else if (isMachineProfileBank) StringCchCopyA(roles, ARRAY_COUNT(roles), " | shared");
    else if (isAppLaunch) StringCchCopyA(roles, ARRAY_COUNT(roles), " | app start");
    else if (isLogon) StringCchCopyA(roles, ARRAY_COUNT(roles), " | logon");

    char text[128] = {};
    StringCchPrintfA(text, ARRAY_COUNT(text), "Slot %d is %s%s", slot,
        saved ? "saved" : "empty", roles);
    SetWindowTextA(g_app.hProfileStateLabel, text);
}

static void update_profile_action_buttons() {
    if (!g_app.hProfileCombo) return;
    int slot = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
    if (slot < 0) slot = CONFIG_DEFAULT_SLOT - 1;
    slot += 1;
    bool saved = is_profile_slot_saved(g_app.configPath, slot);
    if (g_app.hProfileLoadBtn) EnableWindow(g_app.hProfileLoadBtn, saved ? TRUE : FALSE);
    if (g_app.hProfileClearBtn) EnableWindow(g_app.hProfileClearBtn, saved ? TRUE : FALSE);
}

static void update_background_service_controls() {
    if (g_app.hServiceEnableCheck) {
        bool checked = g_app.backgroundServiceToggleInFlight
            ? g_app.backgroundServiceToggleTargetEnabled
            : g_app.backgroundServiceInstalled;
        SendMessageA(g_app.hServiceEnableCheck, BM_SETCHECK, (WPARAM)(checked ? BST_CHECKED : BST_UNCHECKED), 0);
        InvalidateRect(g_app.hServiceEnableCheck, nullptr, FALSE);
        UpdateWindow(g_app.hServiceEnableCheck);
        EnableWindow(g_app.hServiceEnableCheck, g_app.backgroundServiceToggleInFlight ? FALSE : TRUE);
    }
    if (g_app.hServiceEnableLabel) {
        SetWindowTextA(g_app.hServiceEnableLabel,
            g_app.backgroundServiceInstalled && g_app.backgroundServiceBroken
                ? "Background service installed (repair needed)"
                : "Background service installed");
        EnableWindow(g_app.hServiceEnableLabel, g_app.backgroundServiceToggleInFlight ? FALSE : TRUE);
    }
    if (g_app.hServiceStatusLabel) {
        char text[512] = {};
        if (g_app.backgroundServiceToggleInFlight) {
            StringCchPrintfA(text, ARRAY_COUNT(text), "%s background service...",
                g_app.backgroundServiceToggleTargetEnabled ? "Installing and starting" : "Stopping and removing");
        } else if (!g_app.backgroundServiceInstalled) {
            StringCchCopyA(text, ARRAY_COUNT(text), "Background service not installed. Click checkbox to install it.");
        } else if (g_app.backgroundServiceBroken) {
            if (g_app.backgroundServiceError[0]) {
                StringCchPrintfA(text, ARRAY_COUNT(text), "Background service needs repair: %s", g_app.backgroundServiceError);
            } else {
                StringCchCopyA(text, ARRAY_COUNT(text), "Background service is installed but not responding. Live controls are disabled.");
            }
        } else if (g_app.backgroundServiceAvailable) {
            StringCchCopyA(text, ARRAY_COUNT(text), "Background service installed. Click checkbox to uninstall it.");
        } else if (g_app.backgroundServiceRunning) {
            StringCchCopyA(text, ARRAY_COUNT(text), "Background service running, waiting for first successful GPU initialization.");
        } else {
            StringCchCopyA(text, ARRAY_COUNT(text), "Background service installed but stopped. Live controls are disabled.");
        }
        // Shared-only policy notice for restricted (non-admin) users (ASCII only
        // for the ANSI GUI path).
        if (restricted_to_shared_profiles()) {
            const char* note = " | Administrator restricts this PC to shared profiles; use 'Shared profiles...' to apply one.";
            if (strlen(text) + strlen(note) < ARRAY_COUNT(text)) {
                StringCchCatA(text, ARRAY_COUNT(text), note);
            }
        }
        SetWindowTextA(g_app.hServiceStatusLabel, text);
        // Surface a user-profile-install warning.  Two triggers cover the same
        // problem (a restricted/standard user cannot execute the GUI binary):
        //   1. service_install_dir_is_under_user_profile() — keys off the
        //      SCM-registered service dir (requires the service installed).
        //   2. running_exe_dir_is_under_user_profile() — keys off the running
        //      GUI binary's own dir, so the warning also fires pre-install /
        //      in portable use, before there is any SCM service dir to check.
        bool underUserProfile = !g_app.backgroundServiceToggleInFlight &&
            ((g_app.backgroundServiceInstalled && service_install_dir_is_under_user_profile()) ||
             running_exe_dir_is_under_user_profile());
        if (underUserProfile) {
            char warning[320] = {};
            StringCchPrintfA(warning, ARRAY_COUNT(warning),
                " Warning: Green Curve is running from a user account folder, so restricted/standard "
                "users on this PC cannot launch it. Reinstall under an all-users folder such as %%ProgramFiles%%\\greencurve to "
                "make it available to all users.");
            // Append to the existing status text if there is room.
            size_t currentLen = strlen(text);
            size_t warningLen = strlen(warning);
            if (currentLen + warningLen < ARRAY_COUNT(text)) {
                StringCchCatA(text, ARRAY_COUNT(text), warning);
                SetWindowTextA(g_app.hServiceStatusLabel, text);
            }
        }
    }

    update_share_all_users_check_state();
}

static bool maybe_confirm_profile_load_replace(int slot) {
    DesiredSettings current = {};
    DesiredSettings target = {};
    char err[256] = {};
    if (!refresh_service_snapshot_and_active_desired(err, sizeof(err))) {
        debug_log("profile load confirm: refresh_service_snapshot_and_active_desired failed: %s\n", err);
        // Cannot build a comparison dialog, but the actual load in the handler
        // will still validate and report errors. Skip the confirmation.
        return true;
    }
    if (!capture_gui_config_settings(&current, err, sizeof(err))) {
        debug_log("profile load confirm: capture_gui_config_settings failed: %s\n", err);
        // Cannot compare current GUI state to profile; skip confirmation and
        // let the handler perform the actual load (which validates on its own).
        return true;
    }
    if (!load_profile_from_config(g_app.configPath, slot, &target, err, sizeof(err))) {
        debug_log("profile load confirm: load_profile_from_config failed: %s\n", err);
        // Cannot read profile for comparison; skip confirmation and let the
        // handler try the actual load (which reports its own errors).
        return true;
    }

    DesiredSettings targetFull = {};
    ControlState control = {};
    bool haveControlState = get_effective_control_state(&control);
    initialize_desired_settings_defaults(&targetFull);
    targetFull.hasGpuOffset = true;
    if (haveControlState && control_state_has_meaningful_gpu(&control)) {
        targetFull.gpuOffsetMHz = control.gpuOffsetMHz;
        targetFull.gpuOffsetExcludeLowCount = control.gpuOffsetExcludeLowCount;
    } else {
        resolve_displayed_live_gpu_offset_state_for_gui(&targetFull.gpuOffsetMHz, &targetFull.gpuOffsetExcludeLowCount);
    }
    targetFull.hasMemOffset = true;
    targetFull.memOffsetMHz = haveControlState && control_state_has_meaningful_mem(&control) ? control.memOffsetMHz : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    targetFull.hasPowerLimit = true;
    targetFull.powerLimitPct = haveControlState && control_state_has_meaningful_power(&control) ? control.powerLimitPct : g_app.powerLimitPct;
    targetFull.hasFan = true;
    targetFull.fanMode = haveControlState && control_state_has_meaningful_fan(&control) ? control.fanMode : g_app.activeFanMode;
    targetFull.fanAuto = targetFull.fanMode == FAN_MODE_AUTO;
    targetFull.fanPercent = haveControlState && control_state_has_meaningful_fan(&control) ? control.fanFixedPercent : g_app.activeFanFixedPercent;
    copy_fan_curve(&targetFull.fanCurve, haveControlState && control_state_has_meaningful_fan(&control) ? &control.fanCurve : &g_app.activeFanCurve);
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        int ci = g_app.visibleMap[vi];
        targetFull.hasCurvePoint[ci] = true;
        targetFull.curvePointMHz[ci] = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
    }
    merge_desired_settings(&targetFull, &target);

    bool same = true;
    if (current.gpuOffsetMHz != targetFull.gpuOffsetMHz) same = false;
    if (current.gpuOffsetExcludeLowCount != targetFull.gpuOffsetExcludeLowCount) same = false;
    if (current.memOffsetMHz != targetFull.memOffsetMHz) same = false;
    if (current.powerLimitPct != targetFull.powerLimitPct) same = false;
    if (current.fanMode != targetFull.fanMode || current.fanPercent != targetFull.fanPercent || !fan_curve_equals(&current.fanCurve, &targetFull.fanCurve)) same = false;
    for (int i = 0; same && i < VF_NUM_POINTS; i++) {
        if (current.hasCurvePoint[i] != targetFull.hasCurvePoint[i]) same = false;
        else if (current.hasCurvePoint[i] && current.curvePointMHz[i] != targetFull.curvePointMHz[i]) same = false;
    }
    if (same) return true;

    char msg[256] = {};
    StringCchPrintfA(msg, ARRAY_COUNT(msg),
        "Loading slot %d will replace the values currently typed into the GUI. Continue?", slot);
    return MessageBoxA(g_app.hMainWnd, msg, "Green Curve", MB_YESNO | MB_ICONQUESTION) == IDYES;
}

// Compare the loaded profile against the current live hardware state to detect
// if the hardware has been externally modified (e.g., "Reset to Default",
// driver TDR, or external tool). Returns true if a mismatch is detected that
// warrants skipping the profile restore.
//
// This comparison is unconditional: it compares the profile's saved values
// directly against live hardware. This catches all cases where the GPU state
// has changed externally, regardless of whether the service's in-memory
// "active desired" state is still valid (it may be cleared on service restart).
static bool profile_mismatches_live_hardware(const DesiredSettings* profile) {
    if (!profile) return false;

    // Build a DesiredSettings from the current live hardware state.
    DesiredSettings live = {};
    build_full_live_desired_settings(&live);

    debug_log("profile_live_mismatch_check: comparing profile against live hardware\n"
        "  profile:   hasGpu=%d gpu=%d exclude=%d hasMem=%d mem=%d hasPower=%d power=%d hasFan=%d fanMode=%d\n"
        "  live:      hasGpu=%d gpu=%d exclude=%d hasMem=%d mem=%d hasPower=%d power=%d hasFan=%d fanMode=%d\n",
        profile->hasGpuOffset ? 1 : 0, profile->gpuOffsetMHz, profile->gpuOffsetExcludeLowCount,
        profile->hasMemOffset ? 1 : 0, profile->memOffsetMHz,
        profile->hasPowerLimit ? 1 : 0, profile->powerLimitPct,
        profile->hasFan ? 1 : 0, profile->fanMode,
        live.hasGpuOffset ? 1 : 0, live.gpuOffsetMHz, live.gpuOffsetExcludeLowCount,
        live.hasMemOffset ? 1 : 0, live.memOffsetMHz,
        live.hasPowerLimit ? 1 : 0, live.powerLimitPct,
        live.hasFan ? 1 : 0, live.fanMode);

    // Compare each control field that the profile explicitly sets against the live state.
    // Any difference indicates the hardware has diverged from what the profile expects.
    if (profile->hasGpuOffset && profile->gpuOffsetMHz != live.gpuOffsetMHz) {
        debug_log("profile_live_mismatch_check: MISMATCH gpu_offset_mhz profile=%d live=%d\n",
            profile->gpuOffsetMHz, live.gpuOffsetMHz);
        return true;
    }
    if (profile->hasGpuOffset && profile->gpuOffsetExcludeLowCount != live.gpuOffsetExcludeLowCount) {
        debug_log("profile_live_mismatch_check: MISMATCH gpu_exclude_low_count profile=%d live=%d\n",
            profile->gpuOffsetExcludeLowCount, live.gpuOffsetExcludeLowCount);
        return true;
    }
    if (profile->hasMemOffset && profile->memOffsetMHz != live.memOffsetMHz) {
        debug_log("profile_live_mismatch_check: MISMATCH mem_offset_mhz profile=%d live=%d\n",
            profile->memOffsetMHz, live.memOffsetMHz);
        return true;
    }
    if (profile->hasPowerLimit && profile->powerLimitPct != live.powerLimitPct) {
        debug_log("profile_live_mismatch_check: MISMATCH power_limit_pct profile=%d live=%d\n",
            profile->powerLimitPct, live.powerLimitPct);
        return true;
    }
    if (profile->hasFan && profile->fanMode != live.fanMode) {
        debug_log("profile_live_mismatch_check: MISMATCH fan_mode profile=%d live=%d\n",
            profile->fanMode, live.fanMode);
        return true;
    }
    if (profile->hasFan && profile->fanMode == FAN_MODE_FIXED && profile->fanPercent != live.fanPercent) {
        debug_log("profile_live_mismatch_check: MISMATCH fan_fixed_pct profile=%d live=%d\n",
            profile->fanPercent, live.fanPercent);
        return true;
    }

    // Compare curve points — check visible points that the profile explicitly sets
    // for any difference from the live hardware curve.
    int mismatchCurvePoints = 0;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (!profile->hasCurvePoint[ci]) continue;
        if (!is_curve_point_visible_in_gui(ci)) continue;
        unsigned int liveMhz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        if (profile->curvePointMHz[ci] != liveMhz) {
            mismatchCurvePoints++;
            if (mismatchCurvePoints <= 3) {
                debug_log("profile_live_mismatch_check: MISMATCH curve point %d profile=%u live=%u\n",
                    ci, profile->curvePointMHz[ci], liveMhz);
            }
        }
    }
    if (mismatchCurvePoints > 0) {
        debug_log("profile_live_mismatch_check: MISMATCH %d curve points differ from live\n",
            mismatchCurvePoints);
        return true;
    }

    debug_log("profile_live_mismatch_check: profile matches live hardware, restoring normally\n");
    return false;
}

static bool maybe_load_selected_profile_to_gui_without_apply() {
    int selectedSlot = get_config_int(g_app.configPath, "profiles", "selected_slot", CONFIG_DEFAULT_SLOT);
    if (selectedSlot < 1 || selectedSlot > CONFIG_NUM_SLOTS) {
        debug_log("startup selected profile restore: selected slot %d is invalid\n", selectedSlot);
        return false;
    }
    if (!is_profile_slot_saved(g_app.configPath, selectedSlot)) {
        debug_log("startup selected profile restore: slot %d is empty\n", selectedSlot);
        return false;
    }

    if (g_app.usingBackgroundService && g_app.backgroundServiceAvailable) {
        char snapErr[256] = {};
        if (!refresh_service_snapshot_and_active_desired(snapErr, sizeof(snapErr))) {
            debug_log("startup selected profile restore: service snapshot refresh failed before GUI restore: %s\n",
                snapErr[0] ? snapErr : "unknown error");
        }
    }

    DesiredSettings desired = {};
    char err[256] = {};
    if (!load_profile_from_config(g_app.configPath, selectedSlot, &desired, err, sizeof(err))) {
        debug_log("startup selected profile restore: slot %d load failed: %s\n",
            selectedSlot,
            err[0] ? err : "unknown error");
        set_profile_status_text("Selected slot %d could not be loaded into the GUI: %s",
            selectedSlot,
            err[0] ? err : "unknown error");
        return false;
    }

    int curvePoints = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desired.hasCurvePoint[i]) curvePoints++;
    }
    debug_log("startup selected profile restore: loading slot %d into GUI without apply (curvePoints=%d gpu=%d mem=%d power=%d fanMode=%d)\n",
        selectedSlot,
        curvePoints,
        desired.hasGpuOffset ? desired.gpuOffsetMHz : 0,
        desired.hasMemOffset ? desired.memOffsetMHz : 0,
        desired.hasPowerLimit ? desired.powerLimitPct : 0,
        desired.hasFan ? desired.fanMode : -1);

    // If the loaded profile values don't match the current live hardware, the
    // hardware was externally modified. Skip the restore to avoid showing
    // stale values from a previously-applied profile.
    if (profile_mismatches_live_hardware(&desired)) {
        debug_log("startup selected profile restore: LIVE HARDWARE MISMATCH detected for slot %d, skipping profile restore\n",
            selectedSlot);
        set_profile_status_text("Selected slot %d was not restored because the GPU hardware has been externally modified since the profile was saved.", selectedSlot);
        return false;
    }

    populate_desired_into_gui(&desired);
    refresh_profile_controls_from_config();
    set_profile_status_text("Loaded selected slot %d into the GUI. GPU settings were not applied.", selectedSlot);
    return true;
}

static void maybe_load_app_launch_profile_to_gui() {
    if (g_app.launchedFromLogon) {
        set_profile_status_text("Ready. Skipped app-start auto-load for the logon launch.");
        return;
    }
    int appLaunchSlot = get_config_int(g_app.configPath, "profiles", "app_launch_slot", 0);
    if (appLaunchSlot < 1 || appLaunchSlot > CONFIG_NUM_SLOTS) {
        if (!maybe_load_selected_profile_to_gui_without_apply()) {
            set_profile_status_text("Ready. App start auto-load is disabled.");
        }
        return;
    }
    if (!is_profile_slot_saved(g_app.configPath, appLaunchSlot)) {
        set_config_int(g_app.configPath, "profiles", "app_launch_slot", 0);
        set_profile_status_text("App start slot %d was empty and has been disabled.", appLaunchSlot);
        refresh_profile_controls_from_config();
        layout_bottom_buttons(g_app.hMainWnd);
        return;
    }
    DesiredSettings desired = {};
    char err[256] = {};
    if (!load_profile_from_config(g_app.configPath, appLaunchSlot, &desired, err, sizeof(err))) {
        set_config_int(g_app.configPath, "profiles", "app_launch_slot", 0);
        refresh_profile_controls_from_config();
        write_error_report_log_for_user_failure("App-start profile load failed", err);
        set_profile_status_text("App start load failed: %s", err[0] ? err : "unknown error");
        return;
    }
    if (!desired_settings_have_explicit_state(&desired, true, err, sizeof(err))) {
        set_config_int(g_app.configPath, "profiles", "app_launch_slot", 0);
        refresh_profile_controls_from_config();
        write_error_report_log_for_user_failure("App-start profile rejected", err);
        set_profile_status_text("App start slot %d was rejected: %s", appLaunchSlot, err);
        return;
    }
    char result[512] = {};
    refresh_background_service_state();
    if (g_app.usingBackgroundService && g_app.backgroundServiceAvailable) {
        DesiredSettings activeDesired = {};
        char snapErr[256] = {};
        char matchDetail[256] = {};
        if (refresh_service_snapshot_and_active_desired(snapErr, sizeof(snapErr), &activeDesired)
            && desired_settings_match_active_service_intent(&desired, &activeDesired, matchDetail, sizeof(matchDetail))) {
            debug_log("maybe_load_app_launch_profile_to_gui: slot %d already active in background service; skipping reset-before-apply (%s)\n",
                appLaunchSlot,
                matchDetail[0] ? matchDetail : "match");
            populate_desired_into_gui(&desired);
            set_config_int(g_app.configPath, "profiles", "selected_slot", appLaunchSlot);
            refresh_profile_controls_from_config();
            set_profile_status_text("Loaded slot %d into the GUI. Background service already has matching active settings, so app-start apply was skipped.", appLaunchSlot);
            return;
        }
        debug_log("maybe_load_app_launch_profile_to_gui: slot %d needs reset-before-apply; service active intent check=%s\n",
            appLaunchSlot,
            matchDetail[0] ? matchDetail : (snapErr[0] ? snapErr : "unavailable"));
    } else {
        debug_log("maybe_load_app_launch_profile_to_gui: slot %d cannot use active-service skip (usingService=%d available=%d)\n",
            appLaunchSlot,
            g_app.usingBackgroundService ? 1 : 0,
            g_app.backgroundServiceAvailable ? 1 : 0);
    }
    debug_log("maybe_load_app_launch_profile_to_gui: applying slot %d with reset-before-apply\n", appLaunchSlot);
    desired.resetOcBeforeApply = true;
    bool ok = apply_desired_settings(&desired, false, result, sizeof(result));
    if (ok) {
        populate_desired_into_gui(&desired);
        set_config_int(g_app.configPath, "profiles", "selected_slot", appLaunchSlot);
        refresh_profile_controls_from_config();
        set_profile_status_text((g_app.backgroundServiceInstalled && g_app.backgroundServiceAvailable)
            ? "Loaded slot %d into the GUI and applied it through the background service on app start."
            : "Loaded slot %d into the GUI and applied it on app start.", appLaunchSlot);
    } else {
        char detail[128] = {};
        refresh_global_state(detail, sizeof(detail));
        populate_global_controls();
        if (g_app.loaded) populate_edits();
        invalidate_main_window();
        set_profile_status_text("App start apply for slot %d failed and the GUI was refreshed from live state: %s", appLaunchSlot, result);
    }
}

static bool ensure_profile_slot_available_for_auto_action(int slot) {
    if (slot <= 0) return true;
    if (is_profile_slot_saved(g_app.configPath, slot)) return true;
    set_profile_status_text("Slot %d is empty, so that automatic action was disabled.", slot);
    return false;
}

// Wait for the GPU driver to be fully ready before applying an aggressive
// profile at Windows startup.  A too-early apply (while the driver is still
// initializing) can cause a TDR / driver crash.  Bounded retry that actually
// probes the service snapshot each attempt (no blind timing bandaid).
static bool logon_wait_for_gpu_driver_ready(int slotForLog) {
    debug_log("apply_logon_startup_behavior: waiting for GPU driver readiness before applying slot %d\n", slotForLog);
    for (int attempt = 0; attempt < 15; attempt++) {
        refresh_background_service_state();
        if (!g_app.backgroundServiceAvailable) {
            Sleep(500);
            continue;
        }
        ServiceSnapshot snapshot = {};
        char snapErr[256] = {};
        if (service_client_get_snapshot(&snapshot, snapErr, sizeof(snapErr))) {
            if (snapshot.loaded && snapshot.numPopulated > 0 && snapshot.initialized) {
                debug_log("apply_logon_startup_behavior: driver ready on attempt %d (populated=%d)\n",
                    attempt + 1, snapshot.numPopulated);
                return true;
            }
        }
        Sleep(400);
    }
    debug_log("apply_logon_startup_behavior: GPU driver did not become ready in time, skipping apply\n");
    return false;
}

// Apply a per-user "apply admin shared profile N at logon" choice
// (logon_shared_slot).  Loads the admin's authoritative bank copy and tags it as
// a shared-slot apply so the service honors it under the shared-only policy.
// Returns true if the choice was handled (applied or fatally skipped); false
// means "no/stale shared choice — fall through to the per-user logon_slot path".
static bool apply_logon_shared_slot_if_configured(bool startProgramAtLogon) {
    int logonSharedSlot = get_config_int(g_app.configPath, "profiles", "logon_shared_slot", 0);
    if (logonSharedSlot < 0 || logonSharedSlot > CONFIG_NUM_SLOTS) logonSharedSlot = 0;
    if (logonSharedSlot == 0) return false;

    char machinePath[MAX_PATH] = {};
    DesiredSettings desired = {};
    char err[256] = {};
    if (!resolve_machine_config_path(machinePath, sizeof(machinePath)) ||
        !is_profile_slot_saved(machinePath, logonSharedSlot) ||
        !load_profile_from_config(machinePath, logonSharedSlot, &desired, err, sizeof(err)) ||
        !desired_settings_have_explicit_state(&desired, true, err, sizeof(err))) {
        // The admin unpublished/changed the shared bank: drop the stale choice
        // and let the caller fall back to the per-user logon_slot path.
        debug_log("apply_logon_startup_behavior: shared logon slot %d unavailable (%s); clearing it\n",
            logonSharedSlot, err[0] ? err : "not published");
        set_config_int(g_app.configPath, "profiles", "logon_shared_slot", 0);
        return false;
    }

    if (!logon_wait_for_gpu_driver_ready(logonSharedSlot)) {
        set_profile_status_text("Logon apply skipped: GPU driver was not ready after waiting.");
        return true;
    }

    desired.resetOcBeforeApply = true;
    // Tag the editor as holding this admin shared slot so the IPC apply carries
    // SERVICE_REQUEST_FLAG_SHARED_SLOT and the service applies its own copy.
    g_app.loadedSharedSlot = logonSharedSlot;
    g_app.guiHasUserModifiedValues = false;
    char result[512] = {};
    debug_log("apply_logon_startup_behavior: applying shared bank slot %d at logon (authoritative)\n", logonSharedSlot);
    bool ok = apply_desired_settings(&desired, false, result, sizeof(result));
    debug_log("apply_logon_startup_behavior: shared logon apply ok=%d msg=%s\n", ok ? 1 : 0, result);
    if (ok) {
        populate_desired_into_gui(&desired);
        g_app.loadedSharedSlot = logonSharedSlot;  // populate_* clears it; editor keeps showing the shared profile
        set_profile_status_text(startProgramAtLogon
            ? "Started the tray client and applied shared profile %d through the background service at Windows logon."
            : "Applied shared profile %d through the background service at Windows logon.",
            logonSharedSlot);
    } else {
        char detail[128] = {};
        refresh_global_state(detail, sizeof(detail));
        populate_global_controls();
        if (g_app.loaded) populate_edits();
        invalidate_main_window();
        set_profile_status_text("Logon apply of shared profile %d failed and live state was reloaded: %s",
            logonSharedSlot, result);
    }
    return true;
}

static void apply_logon_startup_behavior() {
    if (!g_app.launchedFromLogon) return;

    refresh_background_service_state();

    if (!g_app.backgroundServiceAvailable) {
        set_profile_status_text(g_app.backgroundServiceInstalled
            ? "Background service is unavailable at logon. Live GPU apply was skipped."
            : "Background service is not installed. Live GPU apply was skipped.");
        return;
    }

    bool startProgramAtLogon = is_start_on_logon_enabled(g_app.configPath);
    g_app.startHiddenToTray = startProgramAtLogon;

    // A per-user "apply admin shared profile N at logon" choice takes precedence
    // over the per-user logon_slot and is the ONLY logon apply that passes the
    // shared-only policy for a restricted user.
    if (apply_logon_shared_slot_if_configured(startProgramAtLogon)) return;

    // Restricted (shared-only) users cannot auto-apply a custom per-user
    // logon_slot — the service rejects it.  Skip the doomed apply and guide them
    // to choose a shared profile for logon instead of showing a scary error.
    if (restricted_to_shared_profiles() &&
        get_config_int(g_app.configPath, "profiles", "logon_slot", 0) > 0) {
        debug_log("apply_logon_startup_behavior: restricted user, per-user logon_slot set but no shared logon choice; skipping (policy)\n");
        set_profile_status_text("Your administrator restricts this PC to shared profiles. Use \"Shared profiles...\" to pick one to apply at logon.");
        return;
    }

    int logonSlot = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);
    if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;
    if (logonSlot == 0) {
        set_profile_status_text(startProgramAtLogon
            ? "Started in the tray at Windows logon."
            : "Applied Windows logon startup rules without opening the tray app.");
        return;
    }

    if (!ensure_profile_slot_available_for_auto_action(logonSlot)) {
        set_config_int(g_app.configPath, "profiles", "logon_slot", 0);
        refresh_profile_controls_from_config();
        return;
    }

    DesiredSettings desired = {};
    char err[256] = {};
    if (!load_profile_from_config(g_app.configPath, logonSlot, &desired, err, sizeof(err))) {
        set_config_int(g_app.configPath, "profiles", "logon_slot", 0);
        refresh_profile_controls_from_config();
        write_error_report_log_for_user_failure("Logon profile load failed", err);
        set_profile_status_text("Logon apply failed for slot %d: %s", logonSlot, err[0] ? err : "unknown error");
        return;
    }
    if (!desired_settings_have_explicit_state(&desired, true, err, sizeof(err))) {
        set_config_int(g_app.configPath, "profiles", "logon_slot", 0);
        refresh_profile_controls_from_config();
        write_error_report_log_for_user_failure("Logon profile rejected", err);
        set_profile_status_text("Logon slot %d was rejected: %s", logonSlot, err);
        return;
    }

    if (!logon_wait_for_gpu_driver_ready(logonSlot)) {
        set_profile_status_text("Logon apply skipped: GPU driver was not ready after waiting.");
        return;
    }

    char result[512] = {};
    debug_log("apply_logon_startup_behavior: applying slot %d with reset-before-apply (gpu=%d exclude=%d mem=%d power=%d fanMode=%d)\n",
        logonSlot,
        desired.hasGpuOffset ? desired.gpuOffsetMHz : 0,
        desired.hasGpuOffset ? (desired.gpuOffsetExcludeLowCount ? 1 : 0) : 0,
        desired.hasMemOffset ? desired.memOffsetMHz : 0,
        desired.hasPowerLimit ? desired.powerLimitPct : 0,
        desired.hasFan ? desired.fanMode : -1);
    desired.resetOcBeforeApply = true;
    bool ok = apply_desired_settings(&desired, false, result, sizeof(result));
    debug_log("apply_logon_startup_behavior: apply result ok=%d msg=%s\n", ok ? 1 : 0, result);
    if (ok) {
        populate_desired_into_gui(&desired);
        set_config_int(g_app.configPath, "profiles", "selected_slot", logonSlot);
        refresh_profile_controls_from_config();
        set_profile_status_text(startProgramAtLogon
            ? "Started the tray client and applied slot %d through the background service at Windows logon."
            : "Applied slot %d through the background service at Windows logon without requiring the tray runtime.",
            logonSlot);
    } else {
        char detail[128] = {};
        refresh_global_state(detail, sizeof(detail));
        populate_global_controls();
        if (g_app.loaded) populate_edits();
        invalidate_main_window();
        set_profile_status_text(startProgramAtLogon
            ? "Started in the tray, but slot %d apply failed and live state was reloaded: %s"
            : "Silent Windows logon apply for slot %d failed and live state was reloaded: %s",
            logonSlot, result);
    }
}
#endif

