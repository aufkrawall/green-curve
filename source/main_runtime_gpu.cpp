static bool nvapi_read_control_table(unsigned char* buf, size_t bufSize) {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend) return false;
    if (!buf || bufSize < backend->controlBufferSize) return false;

    auto getFunc = (NvApiFunc)nvapi_qi(backend->getControlId);
    if (!getFunc) return false;

    unsigned char mask[32] = {};
    if (!nvapi_get_vf_info_cached(mask, nullptr)) return false;

    memset(buf, 0, backend->controlBufferSize);
    const unsigned int version = (backend->controlVersion << 16) | backend->controlBufferSize;
    memcpy(&buf[0], &version, sizeof(version));
    if (backend->controlMaskOffset + sizeof(mask) > backend->controlBufferSize) return false;
    memcpy(&buf[backend->controlMaskOffset], mask, sizeof(mask));
    return getFunc(g_app.gpuHandle, buf) == 0;
}

struct HeapBuffer {
    void* ptr;
    HeapBuffer(size_t size) : ptr(calloc(1, size)) {}
    ~HeapBuffer() { free(ptr); }
    operator unsigned char*() const { return (unsigned char*)ptr; }
    operator bool() const { return ptr != nullptr; }
};

static bool apply_curve_offsets_verified(const int* targetOffsets, const bool* pointMask, int maxBatchPasses) {
    if (!targetOffsets || !pointMask) return false;

    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->writeSupported) return false;

    bool desiredMask[VF_NUM_POINTS] = {};
    int desiredOffsets[VF_NUM_POINTS] = {};
    bool pendingMask[VF_NUM_POINTS] = {};
    int desiredCount = 0;

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!pointMask[i]) continue;
        if (g_app.curve[i].freq_kHz == 0) continue;
        desiredMask[i] = true;
        pendingMask[i] = true;
        desiredOffsets[i] = clamp_freq_delta_khz(targetOffsets[i]);
        desiredCount++;
    }
    if (desiredCount == 0) return true;

    if (maxBatchPasses < 1) maxBatchPasses = 1;

    auto setFunc = (NvApiFunc)nvapi_qi(backend->setControlId);
    if (!setFunc) return false;

    const size_t CONTROL_BUF_SIZE = 0x4000;
    HeapBuffer baseControl(CONTROL_BUF_SIZE);
    if (!baseControl) return false;
    if (backend->controlBufferSize > CONTROL_BUF_SIZE) return false;
    if (!nvapi_read_control_table(baseControl, CONTROL_BUF_SIZE)) return false;

    bool anyWrite = false;
    int batchedPoints = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desiredMask[i]) batchedPoints++;
    }
    bool allowBatch = batchedPoints > 1;
    bool batchFailed = false;
    if (!allowBatch) maxBatchPasses = 0;
    HeapBuffer batchBuf(CONTROL_BUF_SIZE);
    if (!batchBuf) return false;
    for (int pass = 0; pass < maxBatchPasses; pass++) {
        unsigned char* buf = batchBuf;
        memcpy(buf, baseControl, backend->controlBufferSize);

        unsigned char writeMask[32] = {};
        bool anyPendingWrite = false;
        int pointsInPass = 0;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!pendingMask[i]) continue;
            int currentDelta = 0;
            unsigned int deltaOffset = backend->controlEntryBaseOffset + (unsigned int)i * backend->controlEntryStride + backend->controlEntryDeltaOffset;
            if (deltaOffset + sizeof(currentDelta) > backend->controlBufferSize) return false;
            memcpy(&currentDelta, &buf[deltaOffset], sizeof(currentDelta));
            if (currentDelta == desiredOffsets[i]) {
                pendingMask[i] = false;
                continue;
            }
            memcpy(&buf[deltaOffset], &desiredOffsets[i], sizeof(desiredOffsets[i]));
            writeMask[i / 8] |= (unsigned char)(1u << (i % 8));
            anyPendingWrite = true;
            pointsInPass++;
        }

        if (!anyPendingWrite) break;

        memcpy(&buf[backend->controlMaskOffset], writeMask, sizeof(writeMask));
        char phase[128] = {};
        StringCchPrintfA(phase, ARRAY_COUNT(phase), "VF curve batch pass: pass=%d points=%d", pass + 1, pointsInPass);
        set_last_apply_phase(phase);
        debug_log("curve batch pass %d begin: points=%d maskBytes=%02X%02X%02X%02X\n",
            pass + 1, pointsInPass,
            writeMask[0], writeMask[1], writeMask[2], writeMask[3]);
        int setRet = setFunc(g_app.gpuHandle, buf);
        debug_log("curve batch pass %d: points=%d ret=%d maskBytes=%02X%02X%02X%02X\n",
            pass + 1, pointsInPass, setRet,
            writeMask[0], writeMask[1], writeMask[2], writeMask[3]);
        if (setRet != 0) {
            batchFailed = true;
            break;
        }
        anyWrite = true;

        bool readOk = false;
        for (int verifyTry = 0; verifyTry < 6; verifyTry++) {
            if (verifyTry > 0) Sleep(10);
            if (nvapi_read_offsets()) {
                readOk = true;
                break;
            }
        }
        if (!readOk) {
            batchFailed = true;
            break;
        }

        bool anyPending = false;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!desiredMask[i]) continue;
            pendingMask[i] = (g_app.freqOffsets[i] != desiredOffsets[i]);
            if (pendingMask[i]) anyPending = true;
            unsigned int deltaOffset = backend->controlEntryBaseOffset + (unsigned int)i * backend->controlEntryStride + backend->controlEntryDeltaOffset;
            if (deltaOffset + sizeof(g_app.freqOffsets[i]) > backend->controlBufferSize) return false;
            memcpy(&baseControl[deltaOffset], &g_app.freqOffsets[i], sizeof(g_app.freqOffsets[i]));
        }
        if (!anyPending) break;
    }

    bool allOk = !batchFailed;
    bool hasPending = false;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desiredMask[i]) continue;
        if (g_app.freqOffsets[i] != desiredOffsets[i]) {
            pendingMask[i] = true;
            hasPending = true;
        } else {
            pendingMask[i] = false;
        }
    }

    if (hasPending) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!pendingMask[i]) continue;
            bool pointOk = nvapi_set_point(i, desiredOffsets[i]);
            debug_log("curve fallback point %d target=%d ok=%d\n", i, desiredOffsets[i], pointOk ? 1 : 0);
            if (!pointOk) {
                allOk = false;
            } else {
                anyWrite = true;
            }
        }

        bool readOk = false;
        for (int verifyTry = 0; verifyTry < 6; verifyTry++) {
            if (verifyTry > 0) Sleep(10);
            if (nvapi_read_offsets()) {
                readOk = true;
                break;
            }
        }
        if (!readOk) {
            allOk = false;
        }
    }

    if (anyWrite) {
        if (!nvapi_read_curve()) allOk = false;
        rebuild_visible_map();
    }

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desiredMask[i]) continue;
        if (g_app.freqOffsets[i] != desiredOffsets[i]) allOk = false;
    }

    return allOk;
}

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

    bool serviceReady = g_app.backgroundServiceAvailable;
#ifndef GREEN_CURVE_SERVICE_BINARY
    populate_gpu_selector();
#endif

    int liveGpuOffsetExcludeLowCount = haveControlState && control_state_has_meaningful_gpu(&control)
        ? control.gpuOffsetExcludeLowCount
        : g_app.appliedGpuOffsetExcludeLowCount;
    int liveGpuOffsetMHz = haveControlState && control_state_has_meaningful_gpu(&control)
        ? control.gpuOffsetMHz
        : g_app.appliedGpuOffsetMHz;
    g_app.appliedGpuOffsetExcludeLowCount = liveGpuOffsetExcludeLowCount;
    g_app.appliedGpuOffsetMHz = liveGpuOffsetMHz;
    bool preservePendingCurveEdits = gui_has_pending_curve_or_lock_edits();
    if (!gui_state_dirty()) {
        g_app.guiGpuOffsetExcludeLowCount = liveGpuOffsetExcludeLowCount;
        g_app.guiGpuOffsetMHz = liveGpuOffsetMHz;
    }
    debug_log("populate_global_controls: dirty=%d haveControl=%d liveGpu=%d liveExclude=%d guiGpu=%d guiExclude=%d appliedGpu=%d appliedExclude=%d\n",
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
        debug_log("populate_global_controls: wrote gpu offset edit=%d enabled=%d\n",
            gpuOffsetToShow,
            (serviceReady && g_app.gpuOffsetRangeKnown) ? 1 : 0);
    }
    if (g_app.hGpuOffsetExcludeLowEdit) {
        int excludeToShow = gui_state_dirty() ? g_app.guiGpuOffsetExcludeLowCount : g_app.appliedGpuOffsetExcludeLowCount;
        char excludeBuf[16] = {};
        StringCchPrintfA(excludeBuf, ARRAY_COUNT(excludeBuf), "%d", excludeToShow);
        SetWindowTextA(g_app.hGpuOffsetExcludeLowEdit, excludeBuf);
        EnableWindow(g_app.hGpuOffsetExcludeLowEdit, (serviceReady && g_app.gpuOffsetRangeKnown) ? TRUE : FALSE);
        debug_log("populate_global_controls: wrote gpu exclude edit=%d enabled=%d\n",
            excludeToShow,
            (serviceReady && g_app.gpuOffsetRangeKnown) ? 1 : 0);
    }
    if (g_app.hMemOffsetEdit) {
        if (!gui_state_dirty()) {
            char buf[32];
            int memOffsetToShow = control_state_has_meaningful_mem(&control)
                ? control.memOffsetMHz
                : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
            StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", memOffsetToShow);
            SetWindowTextA(g_app.hMemOffsetEdit, buf);
        }
        EnableWindow(g_app.hMemOffsetEdit, (serviceReady && g_app.memOffsetRangeKnown) ? TRUE : FALSE);
    }
    if (g_app.hPowerLimitEdit) {
        if (!gui_state_dirty()) {
            char buf[32];
            int powerToShow = control_state_has_meaningful_power(&control) ? control.powerLimitPct : g_app.powerLimitPct;
            StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", powerToShow);
            SetWindowTextA(g_app.hPowerLimitEdit, buf);
        }
        EnableWindow(g_app.hPowerLimitEdit, serviceReady ? TRUE : FALSE);
    }
    if (g_app.hApplyBtn) EnableWindow(g_app.hApplyBtn, (serviceReady && g_app.loaded) ? TRUE : FALSE);
    if (g_app.hRefreshBtn) EnableWindow(g_app.hRefreshBtn, serviceReady ? TRUE : FALSE);
    if (g_app.hResetBtn) EnableWindow(g_app.hResetBtn, (serviceReady && g_app.loaded) ? TRUE : FALSE);
    end_programmatic_edit_update();
    if (!preservePendingCurveEdits && !gui_has_pending_global_edits()) {
        detect_locked_tail_from_curve();
    }
    update_fan_controls_enabled_state();
    g_app.serviceSnapshotAuthoritative = false;
}

static int displayed_curve_khz(unsigned int rawFreq_kHz) {
    long long v = (long long)rawFreq_kHz;
    if (v > INT_MAX) v = INT_MAX;
    return (int)v;
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
            if (inLockedTail) continue;
        }
        unsigned int currentMHz = displayed_curve_mhz(g_app.curve[i].freq_kHz);
        if (full.curvePointMHz[i] != currentMHz) {
            curveUnchanged = false;
            break;
        }
    }

    if (gpuUnchanged && memUnchanged && powerUnchanged && curveUnchanged && fanChanged) {
        debug_log("capture_gui_apply_settings: fan-only apply shortcut taken\n");
        *desired = fanOnly;
        desired->hasFan = true;
        desired->fanMode = full.fanMode;
        desired->fanAuto = full.fanAuto;
        desired->fanPercent = full.fanPercent;
        copy_fan_curve(&desired->fanCurve, &full.fanCurve);
        return true;
    }

    if (gpuUnchanged && memUnchanged && powerUnchanged && curveUnchanged && !fanChanged) {
        set_message(err, errSize, "No changes to apply");
        return false;
    }

    if (full.hasGpuOffset && full.gpuOffsetExcludeLowCount
        && !selective_gpu_offset_curve_shape_looks_safe(&full, full.gpuOffsetMHz, full.gpuOffsetExcludeLowCount)) {
        set_message(err, errSize,
            "Selective GPU offset with this curve shape is unsafe to apply. Reload or re-save the preset with this build, then verify the lock tail before applying.");
        return false;
    }

    DesiredSettings resetFull = {};
    if (!capture_gui_desired_settings(&resetFull, true, true, true, err, errSize)) return false;
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
    if (!capture_gui_desired_settings(&guiDesired, true, true, true, err, errSize)) return false;

    DesiredSettings full = {};
    build_full_live_desired_settings(&full);

    merge_desired_settings(&full, &guiDesired);
    if (guiDesired.hasLock) {
        full.hasLock = true;
        full.lockCi = guiDesired.lockCi;
        full.lockMHz = guiDesired.lockMHz;
        full.lockTracksAnchor = guiDesired.lockTracksAnchor;
    } else if (g_app.lockedCi >= 0 && g_app.lockedFreq > 0) {
        full.hasLock = true;
        full.lockCi = g_app.lockedCi;
        full.lockMHz = g_app.lockedFreq;
        full.lockTracksAnchor = g_app.guiLockTracksAnchor;
    }
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

static unsigned int displayed_curve_mhz(unsigned int rawFreq_kHz) {
    return (unsigned int)((displayed_curve_khz(rawFreq_kHz) + 500) / 1000);
}

static unsigned int curve_point_verify_tolerance_mhz(int pointIndex) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS || g_app.curve[pointIndex].freq_kHz == 0) {
        return 1;
    }

    unsigned int actualMHz = displayed_curve_mhz(g_app.curve[pointIndex].freq_kHz);
    auto nearest_distinct_neighbor_distance_mhz = [&](int startIndex, int step) -> unsigned int {
        for (int ci = startIndex; ci >= 0 && ci < VF_NUM_POINTS; ci += step) {
            if (g_app.curve[ci].freq_kHz == 0) continue;
            unsigned int neighborMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
            if (neighborMHz == actualMHz) continue;
            return actualMHz > neighborMHz ? (actualMHz - neighborMHz) : (neighborMHz - actualMHz);
        }
        return 0;
    };

    unsigned int leftDistanceMHz = nearest_distinct_neighbor_distance_mhz(pointIndex - 1, -1);
    unsigned int rightDistanceMHz = nearest_distinct_neighbor_distance_mhz(pointIndex + 1, 1);
    unsigned int minDistanceMHz = 0;
    if (leftDistanceMHz && rightDistanceMHz) {
        minDistanceMHz = (unsigned int)nvmin((int)leftDistanceMHz, (int)rightDistanceMHz);
    } else {
        minDistanceMHz = leftDistanceMHz ? leftDistanceMHz : rightDistanceMHz;
    }

    if (minDistanceMHz == 0) return 8;

    unsigned int toleranceMHz = (minDistanceMHz + 1) / 2;
    if (toleranceMHz < 1) toleranceMHz = 1;
    if (toleranceMHz > 8) toleranceMHz = 8;
    return toleranceMHz;
}

static bool curve_targets_match_request(const DesiredSettings* desired, const bool* lockedTailMask, unsigned int lockMhz, char* detail, size_t detailSize) {
    if (!desired) {
        set_message(detail, detailSize, "No requested curve state to verify");
        return false;
    }

    auto matches_target = [](int pointIndex, unsigned int actualMHz, unsigned int targetMHz) -> bool {
        unsigned int toleranceMHz = curve_point_verify_tolerance_mhz(pointIndex);
        int diff = (int)actualMHz - (int)targetMHz;
        return diff >= -(int)toleranceMHz && diff <= (int)toleranceMHz;
    };

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (!desired->hasCurvePoint[ci]) continue;
        if (lockedTailMask && lockedTailMask[ci]) continue;
        if (g_app.curve[ci].freq_kHz == 0) continue;

        unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
        unsigned int targetMHz = desired->curvePointMHz[ci];
        if (!matches_target(ci, actualMHz, targetMHz)) {
            set_curve_target_mismatch_detail(ci, actualMHz, targetMHz, false, detail, detailSize);
            return false;
        }
    }

    if (lockedTailMask && lockMhz > 0) {
        bool sawTailPoint = false;
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (!lockedTailMask[ci]) continue;
            if (g_app.curve[ci].freq_kHz == 0) continue;

            sawTailPoint = true;
            unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
            if (!matches_target(ci, actualMHz, lockMhz)) {
                set_curve_target_mismatch_detail(ci, actualMHz, lockMhz, true, detail, detailSize);
                return false;
            }
        }
        if (!sawTailPoint) {
            set_message(detail, detailSize, "No VF points were available to verify the curve lock");
            return false;
        }
    }

    if (detail && detailSize > 0) detail[0] = 0;
    return true;
}

static int raw_curve_khz_from_display_mhz(unsigned int displayMHz) {
    long long v = (long long)displayMHz * 1000LL;
    if (v < 0) v = 0;
    return (int)v;
}

static int curve_base_khz_for_point(int pointIndex) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return 0;
    long long base = (long long)g_app.curve[pointIndex].freq_kHz - (long long)g_app.freqOffsets[pointIndex];
    if (base < 0) base = 0;
    return (int)base;
}

static int curve_delta_khz_for_target_display_mhz_unclamped(int pointIndex, unsigned int displayMHz) {
    long long target = (long long)raw_curve_khz_from_display_mhz(displayMHz);
    long long base = (long long)curve_base_khz_for_point(pointIndex);
    long long delta = target - base;
    return (int)delta;
}

static int curve_delta_khz_for_target_display_mhz(int pointIndex, unsigned int displayMHz) {
    return clamp_freq_delta_khz(curve_delta_khz_for_target_display_mhz_unclamped(pointIndex, displayMHz));
}

static void set_curve_target_mismatch_detail(int pointIndex, unsigned int actualMHz, unsigned int targetMHz, bool lockTail, char* detail, size_t detailSize) {
    int requiredDeltaKHz = curve_delta_khz_for_target_display_mhz_unclamped(pointIndex, targetMHz);
    int minkHz = 0;
    int maxkHz = 0;
    bool rangeKnown = get_curve_offset_range_khz(&minkHz, &maxkHz);
    unsigned int voltMV = 0;
    if (pointIndex >= 0 && pointIndex < VF_NUM_POINTS) {
        voltMV = g_app.curve[pointIndex].volt_uV / 1000;
    }

    if (rangeKnown && requiredDeltaKHz < minkHz) {
        if (lockTail) {
            set_message(detail, detailSize,
                "Lock tail hit the minimum curve offset at %u mV: reaching %u MHz needs %d kHz, but the supported range is %d..%d kHz (actual %u MHz)",
                voltMV, targetMHz, requiredDeltaKHz, minkHz, maxkHz, actualMHz);
        } else {
            set_message(detail, detailSize,
                "VF point %d hit the minimum curve offset: reaching %u MHz needs %d kHz, but the supported range is %d..%d kHz (actual %u MHz)",
                pointIndex, targetMHz, requiredDeltaKHz, minkHz, maxkHz, actualMHz);
        }
        return;
    }

    if (rangeKnown && requiredDeltaKHz > maxkHz) {
        if (lockTail) {
            set_message(detail, detailSize,
                "Lock tail hit the maximum curve offset at %u mV: reaching %u MHz needs %d kHz, but the supported range is %d..%d kHz (actual %u MHz)",
                voltMV, targetMHz, requiredDeltaKHz, minkHz, maxkHz, actualMHz);
        } else {
            set_message(detail, detailSize,
                "VF point %d hit the maximum curve offset: reaching %u MHz needs %d kHz, but the supported range is %d..%d kHz (actual %u MHz)",
                pointIndex, targetMHz, requiredDeltaKHz, minkHz, maxkHz, actualMHz);
        }
        return;
    }

    if (lockTail) {
        set_message(detail, detailSize,
            "Lock tail verified at %u MHz @ %u mV instead of requested %u MHz",
            actualMHz, voltMV, targetMHz);
    } else {
        set_message(detail, detailSize,
            "VF point %d verified at %u MHz instead of requested %u MHz",
            pointIndex, actualMHz, targetMHz);
    }
}

static int mem_display_mhz_from_driver_khz(int driver_kHz) {
    return driver_kHz / 1000; // actual clock kHz to actual MHz
}

static int mem_driver_khz_from_display_mhz(int displayMHz) {
    return displayMHz * 1000; // actual clock kHz
}

static int mem_display_mhz_from_driver_mhz(int driverMHz) {
    return driverMHz / 2; // NVML memory offset MHz is effective; UI mirrors actual MHz like Afterburner
}

static void invalidate_main_window() {
    if (!g_app.hMainWnd) return;
    redraw_window_sync(g_app.hMainWnd);
}

static void redraw_window_sync(HWND hwnd) {
    if (!hwnd) return;
    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME);
}

static void flush_desktop_composition() {
    typedef HRESULT (WINAPI *dwm_flush_t)();
    static dwm_flush_t dwmFlush = nullptr;
    static bool resolved = false;
    if (!resolved) {
        HMODULE dwm = load_system_library_a("dwmapi.dll");
        if (dwm) dwmFlush = (dwm_flush_t)GetProcAddress(dwm, "DwmFlush");
        resolved = true;
    }
    if (dwmFlush) dwmFlush();
}

static void show_window_with_primed_first_frame(HWND hwnd, int nCmdShow) {
    if (!hwnd) return;

    RECT wr = {};
    GetWindowRect(hwnd, &wr);
    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;

    SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0,
        SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    redraw_window_sync(hwnd);
    flush_desktop_composition();

    SetWindowPos(hwnd, nullptr, wr.left, wr.top, winW, winH,
        SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, nCmdShow);
    redraw_window_sync(hwnd);
    update_fan_telemetry_timer();
}

static bool is_system_dark_theme_active() {
    DWORD value = 1;
    DWORD type = 0;
    DWORD size = sizeof(value);
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    bool dark = false;
    if (RegQueryValueExA(hKey, "AppsUseLightTheme", nullptr, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
        dark = value == 0;
    }
    RegCloseKey(hKey);
    return dark;
}

static void initialize_dark_mode_support() {
    if (s_darkModeResolved) return;
    s_darkModeResolved = true;

    HMODULE ux = load_system_library_a("uxtheme.dll");
    if (!ux) return;

    s_fnAllowDarkModeForWindow = (AllowDarkModeForWindowFn)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    s_fnSetPreferredAppMode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    s_fnFlushMenuThemes = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));

    if (s_fnSetPreferredAppMode) {
        s_fnSetPreferredAppMode(is_system_dark_theme_active() ? APP_MODE_ALLOW_DARK : APP_MODE_DEFAULT);
    }
    if (s_fnFlushMenuThemes) {
        s_fnFlushMenuThemes();
    }
}

static void refresh_menu_theme_cache() {
    initialize_dark_mode_support();
    if (s_fnSetPreferredAppMode) {
        s_fnSetPreferredAppMode(is_system_dark_theme_active() ? APP_MODE_ALLOW_DARK : APP_MODE_DEFAULT);
    }
    if (s_fnFlushMenuThemes) {
        s_fnFlushMenuThemes();
    }
}

static void allow_dark_mode_for_window(HWND hwnd) {
    if (!hwnd) return;
    initialize_dark_mode_support();
    if (s_fnAllowDarkModeForWindow) {
        s_fnAllowDarkModeForWindow(hwnd, is_system_dark_theme_active() ? TRUE : FALSE);
    }
}

