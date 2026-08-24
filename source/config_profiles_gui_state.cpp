// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Projection of profile intent onto the GDI main window: populating the editor
// from a DesiredSettings, the profile status/state labels, the profile action
// buttons, the background-service controls, and the load-replaces-draft
// confirmation.  Split out of config_profiles_ui.cpp, which had reached the
// file-size guideline; that file keeps profile configuration and the logon /
// sharing plumbing.

static void populate_desired_into_gui(const DesiredSettings* desired) {
    if (!desired) return;
    // Editor population re-runs live curve detection through populate_edits(),
    // which can infer only FLATTEN from the VF shape.  The APPLIED lock state
    // belongs to the service (especially an NVML HARD pin), so save it across
    // the projection and restore it; otherwise a Save/Load repopulate makes
    // the applied curve briefly/until-refresh look green instead of pinned.
    const int savedAppliedLockVi = g_app.appliedLockVi;
    const int savedAppliedLockCi = g_app.appliedLockCi;
    const unsigned int savedAppliedLockFreq = g_app.appliedLockFreq;
    const LockMode savedAppliedLockMode = g_app.appliedLockMode;
    bool preserveDirty = gui_state_dirty();
    unlock_all();
    if (g_app.loaded) populate_edits();
    begin_programmatic_edit_update();
    set_gui_state_dirty(false);
    g_app.guiHasUserModifiedValues = false;
    // Any populate clears the "loaded shared slot" marker; show_shared_profiles_menu
    // re-sets it AFTER calling this for a shared load.
    g_app.loadedSharedSlot = 0;
    // This is the single sink for every profile-sourced editor population (user
    // slot Load, shared slot, machine/logon slot, the pre-READY pending draft,
    // startup, and the post-apply repopulate).  Mark both offsets as
    // profile-sourced so the high-overclock confirmation stays silent for the
    // user's own saved intent; the EN_CHANGE handlers clear the flag per field
    // as soon as that field is hand-edited.
    g_app.guiGpuOffsetFromProfileLoad = true;
    g_app.guiMemOffsetFromProfileLoad = true;

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
    if (desired->hasMemOffset) {
        g_app.guiMemOffsetMHz = desired->memOffsetMHz;
        if (g_app.hMemOffsetEdit)
            set_edit_value(g_app.hMemOffsetEdit, desired->memOffsetMHz);
    }
    // Power limit
    if (desired->hasPowerLimit) {
        g_app.guiPowerLimitPct = desired->powerLimitPct;
        if (g_app.hPowerLimitEdit)
            set_edit_value(g_app.hPowerLimitEdit, desired->powerLimitPct);
    }
    // XBAR (Blackwell).  A loaded profile is the editor's declaration: an
    // omitted field means zero, including records written before this domain
    // existed. Projecting the hardware value here would turn "no XBAR" into a
    // preserve-current request and defeat named-profile transition cleanup.
    int projectedXbarFreqKhz = desired->hasXbarOffsetKhz
        ? desired->xbarOffsetKhz : 0;
    int projectedXbarMsvddUv = desired->hasXbarMsvddOffsetUv
        ? desired->xbarMsvddOffsetUv : 0;
    g_app.guiXbarOffsetKhz = projectedXbarFreqKhz;
    g_app.guiXbarOffsetFromProfileLoad = true;
    g_app.guiXbarMsvddOffsetUv = projectedXbarMsvddUv;
    g_app.guiXbarMsvddOffsetFromProfileLoad = true;
    StringCchPrintfA(g_app.guiDraft.xbarOffsetText, 32, "%d",
                     projectedXbarFreqKhz / 1000);
    StringCchPrintfA(g_app.guiDraft.xbarMsvddOffsetText, 32, "%d",
                     projectedXbarMsvddUv / 1000);
    // SYS clock rides the same profile-load projection rules as XBAR.
    int projectedSysClkKhz = desired->hasSysClkOffsetKhz
        ? desired->sysClkOffsetKhz : 0;
    g_app.guiSysClkOffsetKhz = projectedSysClkKhz;
    g_app.guiSysClkOffsetFromProfileLoad = true;
    StringCchPrintfA(g_app.guiDraft.sysClkOffsetText, 32, "%d",
                     projectedSysClkKhz / 1000);
    int projectedVideoClkKhz = desired->hasVideoClkOffsetKhz
        ? desired->videoClkOffsetKhz : 0;
    g_app.guiVideoClkOffsetKhz = projectedVideoClkKhz;
    g_app.guiVideoClkOffsetFromProfileLoad = true;
    StringCchPrintfA(g_app.guiDraft.videoClkOffsetText, 32, "%d",
                     projectedVideoClkKhz / 1000);
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
            // apply_lock() infers a missing target from GuiDraft, which this
            // projection has not written yet -- set_edit_value() above only
            // touched the control -- so it would adopt the previous (stock)
            // baseline and report a lock this profile never asked for. The
            // profile's own lock MHz is the authority; it is stored absolute.
            g_app.lockedFreq = lockMHz;
            g_app.guiLockTracksAnchor = desired->hasLock ? desired->lockTracksAnchor : true;
            // apply_lock() ran its pending refresh while GuiDraft still held
            // the PREVIOUS profile's anchor value, so that refresh may have
            // written the stale value back into this field. Re-state the
            // profile's authoritative lock MHz in the field and in the draft;
            // a later refresh skips an absolute anchor field, so the stale
            // value would otherwise stay on screen.
            set_edit_value(g_app.hEditsMhz[vi], lockMHz);
            if (g_app.guiDraft.attached) {
                g_app.guiDraft.curveValueValid[lockCi] = true;
                g_app.guiDraft.curveMHz[lockCi] = lockMHz;
                StringCchPrintfA(g_app.guiDraft.curveText[lockCi],
                    ARRAY_COUNT(g_app.guiDraft.curveText[lockCi]), "%u",
                    lockMHz);
            }
            debug_log("profile projection: re-stated lock anchor ci=%d mhz=%u tracks=%d after apply_lock refresh\n",
                lockCi, lockMHz,
                desired->hasLock ? (desired->lockTracksAnchor ? 1 : 0) : 1);
            break;
        }
    } else if (g_app.lockedVi >= 0) {
        // Clear the lock the previous profile carried.  draw_lock_checkbox()
        // derives the tick from these fields, so the repaint has to come AFTER
        // they are cleared -- the BM_SETCHECK that used to stand here wrote
        // native state an owner-draw button does not keep, and the stale tick
        // survived until the next full-window redraw.
        HWND previousLock = g_app.hLocks[g_app.lockedVi];
        int previousVi = g_app.lockedVi;
        g_app.lockedVi = -1;
        g_app.lockedCi = -1;
        g_app.lockedFreq = 0;
        g_app.lockMode = LOCK_MODE_NONE;
        if (previousLock) InvalidateRect(previousLock, nullptr, FALSE);
        debug_log("profile projection: cleared lock checkbox for vi=%d (profile carries no lock)\n",
            previousVi);
    }
    g_app.appliedLockVi = savedAppliedLockVi;
    g_app.appliedLockCi = savedAppliedLockCi;
    g_app.appliedLockFreq = savedAppliedLockFreq;
    g_app.appliedLockMode = savedAppliedLockMode;
    end_programmatic_edit_update();
    set_gui_state_dirty(preserveDirty);
    if (preserveDirty) gui_draft_capture_desired(desired);
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

// Every slot, not just the selected one: the combo's CBN_SELCHANGE handler calls
// update_profile_action_buttons() without a config refresh, and the service-state
// projection calls it about twice a second while the model is not READY.
// is_profile_slot_saved() costs up to six INI section reads under the storage
// lock, so it must not run on that cadence.
static void ensure_profile_slot_cache() {
    if (g_app.profileSlotCacheValid) return;
    for (int slot = 1; slot <= CONFIG_NUM_SLOTS; ++slot)
        g_app.profileSlotSavedCache[slot - 1] =
            is_profile_slot_saved(g_app.configPath, slot);
    int logonSlot = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);
    int logonSharedSlot = get_config_int(g_app.configPath, "profiles",
        "logon_shared_slot", 0);
    int appLaunchSlot = get_config_int(g_app.configPath, "profiles",
        "app_launch_slot", 0);
    g_app.logonAssignmentPresentCache = logonSlot > 0 || logonSharedSlot > 0;
    g_app.appLaunchAssignmentPresentCache = appLaunchSlot > 0;
    g_app.startOnLogonPresentCache = is_start_on_logon_enabled(g_app.configPath);
    g_app.profileSlotCacheValid = true;
    debug_log_on_change("profile slot cache: refreshed logon=%d shared=%d appLaunch=%d tray=%d from %s\n",
        logonSlot, logonSharedSlot, appLaunchSlot,
        g_app.startOnLogonPresentCache ? 1 : 0,
        g_app.configPath[0] ? g_app.configPath : "(no config path)");
}

static bool profile_slot_is_saved_cached(int slot) {
    if (slot < 1 || slot > CONFIG_NUM_SLOTS) return false;
    ensure_profile_slot_cache();
    return g_app.profileSlotSavedCache[slot - 1];
}

static int selected_profile_slot_from_combo() {
    int selected = (int)SendMessageA(g_app.hProfileCombo, CB_GETCURSEL, 0, 0);
    if (selected < 0) selected = CONFIG_DEFAULT_SLOT - 1;
    return selected + 1;
}

// Profile-row enablement (F-ACTIONABLE).  Everything whose purpose needs the
// service is greyed while it is unreachable; the only controls left live are the
// escape hatches (Refresh, the service checkbox, License) and any assignment that
// is already set, so it can still be cleared.  See
// gui_service_actionability_policy.h for why that exemption is load-bearing.
static void update_profile_action_buttons() {
    if (!g_app.hProfileCombo) return;
    int slot = selected_profile_slot_from_combo();
    bool saved = profile_slot_is_saved_cached(slot);
    GuiServiceActionability actionable = gui_service_actionability_from_app();
    bool profileEdit = gui_service_capability_enabled(
        &actionable, GUI_SERVICE_CAP_PROFILE_EDIT);
    bool automation = gui_service_capability_enabled(
        &actionable, GUI_SERVICE_CAP_AUTOMATION);

    gui_set_window_enabled_if_changed(g_app.hProfileLoadBtn,
        saved && profileEdit);
    gui_set_window_enabled_if_changed(g_app.hProfileClearBtn,
        saved && profileEdit);
    gui_set_window_enabled_if_changed(g_app.hProfileSaveBtn, profileEdit);
    gui_set_window_enabled_if_changed(g_app.hProfileCombo, profileEdit);
    gui_set_window_enabled_if_changed(g_app.hAutoProfilesBtn, automation);
    // Re-asserted from the cached count with the SAME expression its owner
    // (update_share_all_users_check_state) uses.  That function runs after this
    // one inside refresh_profile_controls_from_config(), and standalone from the
    // share toggle, so a different expression here would be silently overwritten.
    gui_set_window_enabled_if_changed(g_app.hSharedProfilesBtn,
        g_app.sharedProfileCountCache > 0 && profileEdit);

    // Assignments that persist and keep having effects: greyed at their default,
    // enabled while set so they can always be cleared.
    gui_set_window_enabled_if_changed(g_app.hAppLaunchCombo,
        gui_service_config_control_actionable(
            &actionable, g_app.appLaunchAssignmentPresentCache));
    gui_set_window_enabled_if_changed(g_app.hLogonCombo,
        gui_service_config_control_actionable(
            &actionable, g_app.logonAssignmentPresentCache));
    bool trayAutostart = gui_service_config_control_actionable(
        &actionable, g_app.startOnLogonPresentCache);
    if (gui_set_window_enabled_if_changed(g_app.hStartOnLogonCheck,
            trayAutostart)) {
        // The caption is drawn by the owner-draw handler, so the greyed/live
        // colour only changes with a repaint (F-CHECKBOX-HIT).
        InvalidateRect(g_app.hStartOnLogonCheck, nullptr, FALSE);
    }
    // Re-asserted from the cached machine scan with the same expression its owner
    // (update_share_all_users_check_state) uses; that function is not on any
    // service-transition path.
    bool slotShared = slot >= 1 && slot <= CONFIG_NUM_SLOTS &&
        g_app.machineProfileSlotSavedCache[slot - 1] &&
        g_app.machineLogonSlotCache == slot;
    gui_set_window_enabled_if_changed(g_app.hShareAllUsersCheck,
        gui_service_config_control_actionable(&actionable, slotShared));
}

static void update_background_service_controls() {
    if (g_app.hServiceEnableCheck) {
        bool checked = g_app.backgroundServiceToggleInFlight
            ? g_app.backgroundServiceToggleTargetEnabled
            : g_app.backgroundServiceInstalled;
        // The caption lives inside the BUTTON (F-CHECKBOX-HIT), so a longer
        // "(repair needed)" text has to re-fit the clickable rectangle too --
        // otherwise the control would clip its own label with an ellipsis.
        bool textChanged = gui_set_window_text_if_changed(
            g_app.hServiceEnableCheck,
            g_app.backgroundServiceInstalled && g_app.backgroundServiceBroken
                ? "Background service installed (repair needed)"
                : "Background service installed");
        if (textChanged) fit_themed_checkbox_to_label(g_app.hServiceEnableCheck);
        // Owner-draw tick: compare against the last painted value, never against
        // native check state the control does not keep (see ui_checkbox_state.h).
        bool changed = ui_checkbox_state_needs_repaint(
            &g_app.serviceEnablePainted, checked) || textChanged;
        changed = gui_set_window_enabled_if_changed(
            g_app.hServiceEnableCheck,
            !g_app.backgroundServiceToggleInFlight) || changed;
        if (changed)
            InvalidateRect(g_app.hServiceEnableCheck, nullptr, FALSE);
    }
    if (g_app.hServiceStatusLabel) {
        char text[512] = {};
        if (g_app.applyInFlight) {
            // Outranks every steady-state description below for the same reason
            // the tray icon greys out: none of them is true while the write is
            // still running.  See gui_apply_in_flight_policy.h.
            gui_apply_in_flight_status_text(text, ARRAY_COUNT(text));
        } else if (g_app.backgroundServiceToggleInFlight) {
            StringCchPrintfA(text, ARRAY_COUNT(text), "%s background service...",
                g_app.backgroundServiceToggleTargetEnabled ? "Installing and starting" : "Stopping and removing");
        } else if (!g_app.backgroundServiceInstalled) {
            StringCchCopyA(text, ARRAY_COUNT(text), "Background service not installed. Click checkbox to install it.");
        } else if (g_app.guiServiceModel.phase == GUI_SERVICE_SYNCING) {
            StringCchCopyA(text, ARRAY_COUNT(text),
                "Synchronizing a coherent GPU state snapshot...");
        } else if (g_app.guiServiceModel.phase ==
                GUI_SERVICE_DEVICE_MISSING) {
            StringCchCopyA(text, ARRAY_COUNT(text),
                "Selected GPU disconnected. Waiting for the device to return; unsaved draft preserved.");
        } else if (g_app.guiServiceModel.phase ==
                GUI_SERVICE_RECOVERING) {
            StringCchCopyA(text, ARRAY_COUNT(text),
                "Selected GPU is reconnecting and being refreshed; unsaved draft preserved.");
        } else if (g_app.guiServiceModel.phase == GUI_SERVICE_DEGRADED) {
            StringCchCopyA(text, ARRAY_COUNT(text),
                "Background service is connected, but coherent live GPU state is unavailable.");
        } else if (gui_service_model_ready(&g_app.guiServiceModel) &&
                g_app.guiDraft.detached) {
            StringCchCopyA(text, ARRAY_COUNT(text),
                "Live GPU state is ready, but the preserved draft belongs to another GPU/topology. Reselect it or click Refresh to discard the draft.");
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
            }
        }
        gui_set_window_text_if_changed(g_app.hServiceStatusLabel, text);
    }
}

static bool maybe_confirm_profile_load_replace(int slot) {
    DesiredSettings current = {};
    DesiredSettings target = {};
    char err[256] = {};
    if (!gui_service_model_ready(&g_app.guiServiceModel)) {
        if (!gui_state_dirty()) return true;
        char reconnectMsg[256] = {};
        StringCchPrintfA(reconnectMsg, ARRAY_COUNT(reconnectMsg),
            "Loading slot %d will replace the unsaved draft preserved while the GPU is reconnecting. Continue?",
            slot);
        return gc_message_box(g_app.hMainWnd, reconnectMsg, "Green Curve",
            MB_YESNO | MB_ICONQUESTION) == IDYES;
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
    if (haveControlState && control.hasGpuOffset) {
        targetFull.gpuOffsetMHz = control.gpuOffsetMHz;
        targetFull.gpuOffsetExcludeLowCount = control.gpuOffsetExcludeLowCount;
    } else {
        resolve_displayed_live_gpu_offset_state_for_gui(&targetFull.gpuOffsetMHz, &targetFull.gpuOffsetExcludeLowCount);
    }
    targetFull.hasMemOffset = true;
    targetFull.memOffsetMHz = haveControlState && control.hasMemOffset ? control.memOffsetMHz : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    targetFull.hasPowerLimit = true;
    targetFull.powerLimitPct = haveControlState && control.hasPowerLimit ? control.powerLimitPct : g_app.powerLimitPct;
    targetFull.hasFan = true;
    targetFull.fanMode = haveControlState && control.hasFan ? control.fanMode : g_app.activeFanMode;
    targetFull.fanAuto = targetFull.fanMode == FAN_MODE_AUTO;
    targetFull.fanPercent = haveControlState && control.hasFan ? control.fanFixedPercent : g_app.activeFanFixedPercent;
    copy_fan_curve(&targetFull.fanCurve, haveControlState && control.hasFan ? &control.fanCurve : &g_app.activeFanCurve);
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
    if (current.hasXbarOffsetKhz != targetFull.hasXbarOffsetKhz ||
        (current.hasXbarOffsetKhz &&
         current.xbarOffsetKhz != targetFull.xbarOffsetKhz)) same = false;
    if (current.hasXbarMsvddOffsetUv != targetFull.hasXbarMsvddOffsetUv ||
        (current.hasXbarMsvddOffsetUv &&
         current.xbarMsvddOffsetUv != targetFull.xbarMsvddOffsetUv)) same = false;
    if (current.hasSysClkOffsetKhz != targetFull.hasSysClkOffsetKhz ||
        (current.hasSysClkOffsetKhz &&
         current.sysClkOffsetKhz != targetFull.sysClkOffsetKhz)) same = false;
    if (current.hasVideoClkOffsetKhz != targetFull.hasVideoClkOffsetKhz ||
        (current.hasVideoClkOffsetKhz &&
         current.videoClkOffsetKhz != targetFull.videoClkOffsetKhz)) same = false;
    if (current.hasLock != targetFull.hasLock ||
        (current.hasLock && (current.lockCi != targetFull.lockCi ||
                             current.lockMHz != targetFull.lockMHz ||
                             current.lockMode != targetFull.lockMode ||
                             current.lockTracksAnchor != targetFull.lockTracksAnchor))) same = false;
    if (current.fanMode != targetFull.fanMode || current.fanPercent != targetFull.fanPercent || !fan_curve_equals(&current.fanCurve, &targetFull.fanCurve)) same = false;
    for (int i = 0; same && i < VF_NUM_POINTS; i++) {
        if (current.hasCurvePoint[i] != targetFull.hasCurvePoint[i] ||
            (current.hasCurvePoint[i] &&
             current.curvePointMHz[i] != targetFull.curvePointMHz[i]))
            same = false;
    }
    if (same) return true;

    char msg[256] = {};
    StringCchPrintfA(msg, ARRAY_COUNT(msg),
        "Loading slot %d will replace the values currently typed into the GUI. Continue?", slot);
    return gc_message_box(g_app.hMainWnd, msg, "Green Curve", MB_YESNO | MB_ICONQUESTION) == IDYES;
}
