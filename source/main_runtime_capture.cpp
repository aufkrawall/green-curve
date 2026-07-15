static void close_startup_sync_thread_handle() {
    if (g_app.hStartupSyncThread) {
        CloseHandle(g_app.hStartupSyncThread);
        g_app.hStartupSyncThread = nullptr;
    }
}

static void populate_global_controls() {
    ControlState control = {};
    bool haveControlState = get_effective_control_state(&control);
    if (haveControlState && !gui_state_dirty()) {
        apply_control_state_to_gui(&control);
    }

    bool serviceReady = g_app.isServiceProcess
        ? g_app.loaded : gui_service_model_ready(&g_app.guiServiceModel);
#ifndef GREEN_CURVE_SERVICE_BINARY
    populate_gpu_selector();
#endif

    int liveGpuOffsetExcludeLowCount = haveControlState && control.hasGpuOffset
        ? control.gpuOffsetExcludeLowCount
        : g_app.appliedGpuOffsetExcludeLowCount;
    int liveGpuOffsetMHz = haveControlState && control.hasGpuOffset
        ? control.gpuOffsetMHz
        : g_app.appliedGpuOffsetMHz;
    g_app.appliedGpuOffsetExcludeLowCount = liveGpuOffsetExcludeLowCount;
    g_app.appliedGpuOffsetMHz = liveGpuOffsetMHz;
    bool preservePendingCurveEdits = gui_has_pending_curve_or_lock_edits();
    if (!gui_state_dirty()) {
        g_app.guiGpuOffsetExcludeLowCount = liveGpuOffsetExcludeLowCount;
        g_app.guiGpuOffsetMHz = liveGpuOffsetMHz;
        g_app.guiMemOffsetMHz = haveControlState && control.hasMemOffset
            ? control.memOffsetMHz
            : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
        g_app.guiPowerLimitPct = haveControlState && control.hasPowerLimit
            ? control.powerLimitPct : g_app.powerLimitPct;
    }
    debug_log_on_change("populate_global_controls: dirty=%d haveControl=%d liveGpu=%d liveExclude=%d guiGpu=%d guiExclude=%d appliedGpu=%d appliedExclude=%d\n",
        gui_state_dirty() ? 1 : 0,
        haveControlState ? 1 : 0,
        liveGpuOffsetMHz,
        liveGpuOffsetExcludeLowCount,
        g_app.guiGpuOffsetMHz,
        g_app.guiGpuOffsetExcludeLowCount,
        g_app.appliedGpuOffsetMHz,
        g_app.appliedGpuOffsetExcludeLowCount);
    begin_programmatic_edit_update();
    if (g_app.hGpuOffsetEdit) {
        char buf[32];
        int gpuOffsetToShow = gui_state_dirty() ? g_app.guiGpuOffsetMHz : g_app.appliedGpuOffsetMHz;
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", gpuOffsetToShow);
        SetWindowTextA(g_app.hGpuOffsetEdit, buf);
        EnableWindow(g_app.hGpuOffsetEdit, (serviceReady && g_app.gpuOffsetRangeKnown) ? TRUE : FALSE);
        debug_log_on_change("populate_global_controls: wrote gpu offset edit=%d enabled=%d\n",
            gpuOffsetToShow,
            (serviceReady && g_app.gpuOffsetRangeKnown) ? 1 : 0);
    }
    if (g_app.hGpuOffsetExcludeLowEdit) {
        int excludeToShow = gui_state_dirty() ? g_app.guiGpuOffsetExcludeLowCount : g_app.appliedGpuOffsetExcludeLowCount;
        char excludeBuf[16] = {};
        StringCchPrintfA(excludeBuf, ARRAY_COUNT(excludeBuf), "%d", excludeToShow);
        SetWindowTextA(g_app.hGpuOffsetExcludeLowEdit, excludeBuf);
        EnableWindow(g_app.hGpuOffsetExcludeLowEdit, (serviceReady && g_app.gpuOffsetRangeKnown) ? TRUE : FALSE);
        debug_log_on_change("populate_global_controls: wrote gpu exclude edit=%d enabled=%d\n",
            excludeToShow,
            (serviceReady && g_app.gpuOffsetRangeKnown) ? 1 : 0);
    }
    if (g_app.hMemOffsetEdit) {
        char buf[32];
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d",
            gui_state_dirty() ? g_app.guiMemOffsetMHz :
                (control.hasMemOffset ? control.memOffsetMHz :
                    mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz)));
        SetWindowTextA(g_app.hMemOffsetEdit, buf);
        EnableWindow(g_app.hMemOffsetEdit, (serviceReady && g_app.memOffsetRangeKnown) ? TRUE : FALSE);
    }
    if (g_app.hPowerLimitEdit) {
        char buf[32];
        StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d",
            gui_state_dirty() ? g_app.guiPowerLimitPct :
                (control.hasPowerLimit ? control.powerLimitPct :
                    g_app.powerLimitPct));
        SetWindowTextA(g_app.hPowerLimitEdit, buf);
        EnableWindow(g_app.hPowerLimitEdit, serviceReady ? TRUE : FALSE);
    }
    bool mutationReady = serviceReady && g_app.loaded &&
        g_app.guiDraft.attached && !g_app.guiDraft.detached;
    if (g_app.hApplyBtn) EnableWindow(g_app.hApplyBtn, mutationReady ? TRUE : FALSE);
    if (g_app.hRefreshBtn) EnableWindow(g_app.hRefreshBtn, TRUE);
    if (g_app.hResetBtn) EnableWindow(g_app.hResetBtn, mutationReady ? TRUE : FALSE);
    end_programmatic_edit_update();
    if (!preservePendingCurveEdits && !gui_has_pending_global_edits()) {
        detect_locked_tail_from_curve();
    }
    update_fan_controls_enabled_state();
    if (!serviceReady || !g_app.loaded) {
        g_app.serviceSnapshotAuthoritative = false;
    }
}

static int displayed_curve_khz(unsigned int rawFreq_kHz) {
    long long v = (long long)rawFreq_kHz;
    if (v > INT_MAX) v = INT_MAX;
    return (int)v;
}

// Record the drift-free VF curve intent that Green Curve just applied (or that the
// service reports as active) into g_app.appliedCurveMHz. This baseline is the ONLY
// source used to display and compare owned VF points, so expected boost/temperature
// drift in the live curve (g_app.curve) can never leak into the editor, the graph,
// fan-only apply detection, or a saved profile. Populated exclusively from intent
// (the DesiredSettings), never from live readback. Call only for real curve applies
// (skip fan-only requests, which carry no curve intent).
static void capture_applied_curve_baseline(const DesiredSettings* desired) {
    if (!desired) return;
    int owned = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i] && desired->curvePointMHz[i] > 0) {
            g_app.appliedCurveMHz[i] = desired->curvePointMHz[i];
            owned++;
        } else {
            g_app.appliedCurveMHz[i] = 0;
        }
    }
    debug_log("capture_applied_curve_baseline: owned=%d point74=%u point75=%u point76=%u lockCi=%d lockMHz=%u\n",
        owned, g_app.appliedCurveMHz[74], g_app.appliedCurveMHz[75], g_app.appliedCurveMHz[76],
        desired->hasLock ? desired->lockCi : -1, desired->hasLock ? desired->lockMHz : 0u);
}

static bool capture_gui_apply_settings(DesiredSettings* desired, char* err, size_t errSize) {
    if (!desired) return false;

    DesiredSettings full = {};
    if (!capture_gui_desired_settings(&full, false, true, false, err, errSize)) return false;

    DesiredSettings fanOnly = {};
    initialize_desired_settings_defaults(&fanOnly);

    ControlState control = {};
    bool haveControlState = get_effective_control_state(&control);
    int currentGpuOffsetMHz = haveControlState && control_state_has_meaningful_gpu(&control) ? control.gpuOffsetMHz : current_applied_gpu_offset_mhz();
    int currentGpuOffsetExcludeLowCount = haveControlState && control_state_has_meaningful_gpu(&control) ? control.gpuOffsetExcludeLowCount : (current_applied_gpu_offset_excludes_low_points() ? g_app.appliedGpuOffsetExcludeLowCount : 0);
    int currentMemOffsetMHz = haveControlState && control_state_has_meaningful_mem(&control) ? control.memOffsetMHz : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    int currentPowerLimitPct = haveControlState && control_state_has_meaningful_power(&control) ? control.powerLimitPct : g_app.powerLimitPct;

    bool gpuUnchanged = !full.hasGpuOffset || (full.gpuOffsetMHz == currentGpuOffsetMHz && full.gpuOffsetExcludeLowCount == currentGpuOffsetExcludeLowCount);
    bool memUnchanged = !full.hasMemOffset || (full.memOffsetMHz == currentMemOffsetMHz);
    bool powerUnchanged = !full.hasPowerLimit || (full.powerLimitPct == currentPowerLimitPct);
    bool fanChanged = full.hasFan && !fan_setting_matches_current(full.fanMode, full.fanPercent, &full.fanCurve);

    // A fan-only apply must stay fan-only even when the live VF curve has drifted
    // under boost/temperature. Compare each captured point against the drift-free
    // applied-intent baseline (g_app.appliedCurveMHz), NEVER against live readback
    // (g_app.curve). Otherwise expected boost drift on a pre-tail point would make a
    // genuine fan-only change look like a curve edit, triggering a full
    // reset-and-reapply mid-game and reseeding the editor with the drifted value.
    // A point the editor owns that has no applied baseline (baseline == 0) is a real
    // change (e.g. a freshly loaded/typed point) and correctly forces a full apply.
    bool curveUnchanged = true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!full.hasCurvePoint[i]) continue;
        if (g_app.curve[i].freq_kHz == 0) continue;
        if (g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible) {
            bool inLockedTail = false;
            for (int vi = g_app.lockedVi; vi < g_app.numVisible; vi++) {
                if (g_app.visibleMap[vi] == i) {
                    inLockedTail = true;
                    break;
                }
            }
            if (inLockedTail) continue;  // lock/tail handled by lockChanged below
        }
        unsigned int baselineMHz = g_app.appliedCurveMHz[i];
        if (baselineMHz == 0 || full.curvePointMHz[i] != baselineMHz) {
            debug_log("capture_gui_apply_settings: curve change ci=%d editor=%u baseline=%u (drift-free)\n",
                i, full.curvePointMHz[i], baselineMHz);
            curveUnchanged = false;
            break;
        }
    }

    bool lockWasApplied = g_app.appliedLockCi >= 0 && g_app.appliedLockFreq > 0;
    bool lockNowActive = full.hasLock && full.lockCi >= 0 && full.lockMHz > 0;
    bool lockChanged = lockNowActive != lockWasApplied
        || (lockNowActive && (full.lockCi != g_app.appliedLockCi || full.lockMHz != g_app.appliedLockFreq || full.lockMode != g_app.appliedLockMode));
    debug_log("capture_gui_apply_settings: lockState applied=(ci=%d mhz=%u mode=%s) desired=(has=%d ci=%d mhz=%u mode=%s) changed=%d\n",
        g_app.appliedLockCi,
        g_app.appliedLockFreq,
        lock_mode_name(g_app.appliedLockMode),
        full.hasLock ? 1 : 0,
        full.hasLock ? full.lockCi : -1,
        full.hasLock ? full.lockMHz : 0u,
        lock_mode_name(full.lockMode),
        lockChanged ? 1 : 0);

    if (gpuUnchanged && memUnchanged && powerUnchanged && curveUnchanged && !lockChanged && fanChanged) {
        debug_log("capture_gui_apply_settings: fan-only apply shortcut taken\n");
        *desired = fanOnly;
        desired->hasFan = true;
        desired->fanMode = full.fanMode;
        desired->fanAuto = full.fanAuto;
        desired->fanPercent = full.fanPercent;
        copy_fan_curve(&desired->fanCurve, &full.fanCurve);
        return true;
    }

    if (gpuUnchanged && memUnchanged && powerUnchanged && curveUnchanged && !lockChanged && !fanChanged) {
        set_message(err, errSize, "No changes to apply");
        return false;
    }

    if (full.hasGpuOffset && full.gpuOffsetExcludeLowCount
        && !selective_gpu_offset_curve_shape_looks_safe(&full, full.gpuOffsetMHz, full.gpuOffsetExcludeLowCount)) {
        set_message(err, errSize,
            "Selective GPU offset with this curve shape is unsafe to apply. Reload or re-save the preset with this build, then verify the lock tail before applying.");
        return false;
    }

    bool fullHasExplicitCurve = false;
    for (int i = 0; i < VF_NUM_POINTS && !fullHasExplicitCurve; i++) {
        if (full.hasCurvePoint[i]) fullHasExplicitCurve = true;
    }

    DesiredSettings resetFull = {};
    if (!capture_gui_desired_settings(&resetFull, true, true, false, err, errSize)) return false;

    // Keep the VF curve request sparse. The reset-before-apply step zeros all
    // offsets and re-reads the stock curve; only user-explicit points and the
    // locked tail should be written back after that reset.
    if (!fullHasExplicitCurve) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            resetFull.hasCurvePoint[i] = false;
            resetFull.curvePointMHz[i] = 0;
        }
    }
    if (!fanChanged) {
        resetFull.hasFan = false;
        resetFull.fanAuto = false;
        resetFull.fanMode = FAN_MODE_AUTO;
        resetFull.fanPercent = 0;
    }
    resetFull.resetOcBeforeApply = true;
    debug_log("capture_gui_apply_settings: OC reset-before-apply target gpu=%d exclude=%d mem=%d power=%d fanChanged=%d\n",
        resetFull.gpuOffsetMHz,
        resetFull.gpuOffsetExcludeLowCount,
        resetFull.memOffsetMHz,
        resetFull.powerLimitPct,
        fanChanged ? 1 : 0);
    *desired = resetFull;
    return true;
}

static bool capture_gui_config_settings(DesiredSettings* desired, char* err, size_t errSize) {
    if (!desired) return false;

    DesiredSettings guiDesired = {};
    if (!capture_gui_desired_settings(&guiDesired, true, true, false, err, errSize)) return false;

    DesiredSettings full = {};
    build_full_live_desired_settings(&full);

    merge_desired_settings(&full, &guiDesired);
    if (guiDesired.hasLock) {
        full.hasLock = true;
        full.lockCi = guiDesired.lockCi;
        full.lockMHz = guiDesired.lockMHz;
        full.lockMode = guiDesired.lockMode;
        full.lockTracksAnchor = guiDesired.lockTracksAnchor;
    } else if (g_app.lockedCi >= 0 && g_app.lockedFreq > 0) {
        full.hasLock = true;
        full.lockCi = g_app.lockedCi;
        full.lockMHz = g_app.lockedFreq;
        full.lockMode = g_app.lockMode;
        full.lockTracksAnchor = g_app.guiLockTracksAnchor;
    }
    debug_log("capture_gui_config_settings: resolved lock has=%d ci=%d mhz=%u mode=%s tracksAnchor=%d (source=%s)\n",
        full.hasLock ? 1 : 0,
        full.hasLock ? full.lockCi : -1,
        full.hasLock ? full.lockMHz : 0u,
        lock_mode_name(full.lockMode),
        full.lockTracksAnchor ? 1 : 0,
        guiDesired.hasLock ? "gui" : ((g_app.lockedCi >= 0 && g_app.lockedFreq > 0) ? "live" : "none"));
    *desired = full;
    return true;
}

static bool save_desired_to_config_with_startup(const char* path, const DesiredSettings* desired, bool useCurrentForUnset, int startupState, char* err, size_t errSize) {
    if (!path || !*path) {
        set_message(err, errSize, "No config path");
        return false;
    }

    char buf[64];
    ControlState control = {};
    bool haveControlState = get_effective_control_state(&control);

    int gpuOffset = 0;
    int gpuOffsetExcludeLowCount = 0;
    if (desired && desired->hasGpuOffset) {
        gpuOffset = desired->gpuOffsetMHz;
        gpuOffsetExcludeLowCount = (desired->gpuOffsetExcludeLowCount > 0 && desired->gpuOffsetMHz != 0) ? desired->gpuOffsetExcludeLowCount : 0;
    } else if (haveControlState && control_state_has_meaningful_gpu(&control)) {
        gpuOffset = control.gpuOffsetMHz;
        gpuOffsetExcludeLowCount = (control.gpuOffsetExcludeLowCount > 0 && control.gpuOffsetMHz != 0) ? control.gpuOffsetExcludeLowCount : 0;
    } else {
        resolve_effective_gpu_offset_state_for_config_save(desired, &gpuOffset, &gpuOffsetExcludeLowCount);
    }
    int memOffset = desired && desired->hasMemOffset ? desired->memOffsetMHz : (haveControlState && control_state_has_meaningful_mem(&control) ? control.memOffsetMHz : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
    int powerPct = desired && desired->hasPowerLimit ? desired->powerLimitPct : (haveControlState && control_state_has_meaningful_power(&control) ? control.powerLimitPct : g_app.powerLimitPct);
    int fanMode = desired && desired->hasFan ? desired->fanMode : (haveControlState && control_state_has_meaningful_fan(&control) ? control.fanMode : g_app.activeFanMode);
    int fanPct = desired && desired->hasFan ? clamp_percent(desired->fanPercent) : (haveControlState && control_state_has_meaningful_fan(&control) ? clamp_percent(control.fanFixedPercent) : g_app.activeFanFixedPercent);
    const FanCurveConfig* fanCurve = desired && desired->hasFan ? &desired->fanCurve : (haveControlState && control_state_has_meaningful_fan(&control) ? &control.fanCurve : &g_app.activeFanCurve);
    debug_log("save_desired_to_config_with_startup: path=%s startupState=%d desired=%d controlState=%d gpu=%d exclude=%d mem=%d power=%d fanMode=%d fanPct=%d\n",
        path,
        startupState,
        desired ? 1 : 0,
        haveControlState ? 1 : 0,
        gpuOffset,
        gpuOffsetExcludeLowCount,
        memOffset,
        powerPct,
        fanMode,
        fanPct);

    size_t cap = 65536;
    size_t used = 0;
    char* out = (char*)malloc(cap);
    if (!out) {
        set_message(err, errSize, "Out of memory building config");
        return false;
    }

    auto appendf = [&](const char* fmt, ...) -> bool {
        if (used + 256 > cap) {
            size_t newCap = cap * 2;
            char* tmp = (char*)realloc(out, newCap);
            if (!tmp) return false;
            out = tmp;
            cap = newCap;
        }
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(out + used, cap - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n < 0) return false;
        used += (size_t)n;
        return true;
    };

    bool buildOk = true;
    if (startupState != CONFIG_STARTUP_PRESERVE) {
        buildOk = buildOk && appendf("[startup]\r\napply_on_launch=%s\r\n", startupState == CONFIG_STARTUP_ENABLE ? "1" : "0");
    }
    buildOk = buildOk && appendf("[debug]\r\nenabled=%s\r\n", g_debug_logging ? "1" : "0");

    buildOk = buildOk && appendf("[controls]\r\n");
    buildOk = buildOk && appendf("gpu_offset_mhz=%d\r\n", gpuOffset);
    buildOk = buildOk && appendf("gpu_offset_exclude_low_count=%d\r\n", gpuOffsetExcludeLowCount);
    buildOk = buildOk && appendf("lock_ci=%d\r\n", desired && desired->hasLock ? desired->lockCi : (g_app.lockedCi >= 0 ? g_app.lockedCi : -1));
    buildOk = buildOk && appendf("lock_mhz=%u\r\n", desired && desired->hasLock ? desired->lockMHz : g_app.lockedFreq);
    buildOk = buildOk && appendf("lock_tracks_anchor=%d\r\n", desired && desired->hasLock ? (desired->lockTracksAnchor ? 1 : 0) : (g_app.guiLockTracksAnchor ? 1 : 0));
    buildOk = buildOk && appendf("mem_offset_mhz=%d\r\n", memOffset);
    buildOk = buildOk && appendf("power_limit_pct=%d\r\n", powerPct);
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", fanPct);
    buildOk = buildOk && appendf("fan_mode=%s\r\n", fan_mode_to_config_value(fanMode));
    buildOk = buildOk && appendf("fan_fixed_pct=%d\r\n", fanPct);
    buildOk = buildOk && appendf("fan=%s\r\n", fanMode == FAN_MODE_AUTO ? "auto" : buf);

    buildOk = buildOk && appendf("[curve]\r\n");
    buildOk = buildOk && appendf("format=explicit_vf_points_v1\r\n");
    buildOk = buildOk && appendf("gpu_offset_mhz=%d\r\n", gpuOffset);
    buildOk = buildOk && appendf("gpu_offset_exclude_low_count=%d\r\n", gpuOffsetExcludeLowCount);
    bool saveCurveAsBasePlusGpuOffset = gpuOffset != 0 && can_save_curve_as_base_plus_gpu_offset(desired, gpuOffset, gpuOffsetExcludeLowCount);
    if (saveCurveAsBasePlusGpuOffset) {
        buildOk = buildOk && appendf("curve_semantics=base_plus_gpu_offset\r\n");
    }
    for (int i = 0; i < VF_NUM_POINTS && buildOk; i++) {
        bool have = desired && desired->hasCurvePoint[i];
        unsigned int mhz = 0;
        if (have) {
            mhz = desired->curvePointMHz[i];
            if (saveCurveAsBasePlusGpuOffset) {
                int baseMHz = (int)mhz - gpu_offset_component_mhz_for_point(i, gpuOffset, gpuOffsetExcludeLowCount);
                if (baseMHz <= 0) continue;
                mhz = (unsigned int)baseMHz;
            }
        } else if (useCurrentForUnset && g_app.curve[i].freq_kHz > 0) {
            mhz = displayed_curve_mhz(g_app.curve[i].freq_kHz);
            if (saveCurveAsBasePlusGpuOffset) {
                int baseMHz = (int)mhz - gpu_offset_component_mhz_for_point(i, gpuOffset, gpuOffsetExcludeLowCount);
                if (baseMHz <= 0) continue;
                mhz = (unsigned int)baseMHz;
            }
        }
        if (mhz == 0) continue;
        buildOk = buildOk && appendf("point%d_mhz=%u\r\n", i, mhz);
        buildOk = buildOk && appendf("point%d_mv=%u\r\n", i, g_app.curve[i].volt_uV / 1000);
        buildOk = buildOk && appendf("point%d_offset_khz=%d\r\n", i, g_app.curve[i].freq_kHz > 0 ? g_app.freqOffsets[i] : 0);
        buildOk = buildOk && appendf("point%d_visible=%s\r\n", i, is_curve_point_visible_in_gui(i) ? "1" : "0");
    }

    buildOk = buildOk && appendf("[fan_curve]\r\n");
    buildOk = buildOk && appendf("poll_interval_ms=%d\r\n", fanCurve->pollIntervalMs);
    buildOk = buildOk && appendf("hysteresis_c=%d\r\n", fanCurve->hysteresisC);
    for (int i = 0; i < FAN_CURVE_MAX_POINTS && buildOk; i++) {
        buildOk = buildOk && appendf("enabled%d=%s\r\n", i, fanCurve->points[i].enabled ? "1" : "0");
        buildOk = buildOk && appendf("temp%d=%d\r\n", i, fanCurve->points[i].temperatureC);
        buildOk = buildOk && appendf("pct%d=%d\r\n", i, fanCurve->points[i].fanPercent);
    }

    if (!buildOk) {
        free(out);
        set_message(err, errSize, "Failed to build config buffer");
        return false;
    }

    const char* replaceSections[] = { "debug", "controls", "curve", "fan_curve" };
    const char* replaceSectionsWithStartup[] = { "startup", "debug", "controls", "curve", "fan_curve" };
    const char* const* sectionsToReplace = (startupState != CONFIG_STARTUP_PRESERVE) ? replaceSectionsWithStartup : replaceSections;
    int sectionCount = (startupState != CONFIG_STARTUP_PRESERVE) ? ARRAY_COUNT(replaceSectionsWithStartup) : ARRAY_COUNT(replaceSections);
    bool ok = write_config_sections_atomic(path, out, sectionsToReplace, sectionCount, err, errSize);
    free(out);
    if (ok) invalidate_tray_profile_cache();
    return ok;
}
