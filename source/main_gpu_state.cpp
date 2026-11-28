static bool show_best_guess_support_warning(HWND parent) {
    if (!should_show_best_guess_warning()) return true;

    char message[768] = {};
    StringCchPrintfA(message, ARRAY_COUNT(message),
        "Detected an unrecognized NVIDIA GPU family (%s, %s).\n\n"
        "Green Curve will allow best-effort support for a new NVIDIA GPU family using fallback clock and voltage controls. Writes stay enabled, but this exact architecture has not been tested yet.\n\n"
        "Check applied clocks and offsets carefully after changes.\n\n"
        "Continue with this GPU?\n- Yes: Continue this time\n- No: Continue and do not show this warning again\n- Cancel: Exit",
        gpu_family_name(g_app.gpuFamily),
        g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");

    bool handled = false;
    bool suppressWarning = false;
    HMODULE comctl = load_system_library_a("comctl32.dll");
    if (comctl) {
        typedef HRESULT (WINAPI *TaskDialogIndirect_t)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        auto taskDialogIndirect = (TaskDialogIndirect_t)GetProcAddress(comctl, "TaskDialogIndirect");
        if (taskDialogIndirect) {
            TASKDIALOGCONFIG config = {};
            config.cbSize = sizeof(config);
            config.hwndParent = parent;
            config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
            config.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
            config.pszWindowTitle = L"Green Curve - Unrecognized GPU Support";
            config.pszMainIcon = TD_WARNING_ICON;
            config.pszVerificationText = L"Do not show this warning again for unrecognized GPUs";

            WCHAR mainInstruction[128] = {};
            StringCchPrintfW(mainInstruction, ARRAY_COUNT(mainInstruction), L"Best-effort support for an unrecognized GPU");
            config.pszMainInstruction = mainInstruction;

            WCHAR content[2048] = {};
            StringCchPrintfW(content, ARRAY_COUNT(content),
                L"Detected an unrecognized NVIDIA GPU family (%hs, %hs).\n\n"
                L"Green Curve will allow best-effort support for a new NVIDIA GPU family using fallback clock and voltage controls. Writes stay enabled, but this exact architecture has not been tested yet.\n\n"
                L"Check applied clocks and offsets carefully after changes.",
                gpu_family_name(g_app.gpuFamily),
                g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");
            config.pszContent = content;

            int button = 0;
            BOOL verificationChecked = FALSE;
            HRESULT hr = taskDialogIndirect(&config, &button, nullptr, &verificationChecked);
            if (SUCCEEDED(hr)) {
                handled = true;
                if (button == IDCANCEL) {
                    remove_tray_icon();
                    release_single_instance_mutex();
                    return false;
                }
                suppressWarning = verificationChecked != FALSE;
            }
        }
        FreeLibrary(comctl);
    }

    if (!handled) {
        int result = gc_message_box(parent, message, "Green Curve - Unrecognized GPU Support", MB_YESNOCANCEL | MB_ICONWARNING);
        if (result == IDCANCEL || result == 0) {
            remove_tray_icon();
            release_single_instance_mutex();
            return false;
        }
        suppressWarning = result == IDNO;
    }

    if (suppressWarning) {
        if (set_config_int(g_app.configPath, "warnings", "hide_unrecognized_gpu_warning", 1)) {
            debug_log("unrecognized GPU warning disabled by user\n");
        } else {
            debug_log("unrecognized GPU warning: failed to persist suppression flag\n");
        }
    }

    g_bestGuessWarningShownThisSession = true;
    return true;
}

static bool vf_curve_global_gpu_offset_supported() {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->writeSupported) {
        debug_log_on_change("vf_curve_global_gpu_offset_supported: no backend or not writable\n");
        return false;
    }
    bool supported = backend->family == GPU_FAMILY_BLACKWELL || backend->family == GPU_FAMILY_UNKNOWN;
    debug_log_on_change("vf_curve_global_gpu_offset_supported: family=%d supported=%d\n", backend->family, supported ? 1 : 0);
    return supported;
}
static bool vf_curve_selective_gpu_offset_supported() {
    const VfBackendSpec* b = g_app.vfBackend;
    return vf_backend_supports_selective_gpu_offset(b != nullptr, b && b->readSupported, b && b->writeSupported);
}

static bool detect_live_selective_gpu_offset_state(int* gpuOffsetMHzOut, int* representativeOffsetkHzOut, int* detectedExcludeLowCountOut) {
    if (!vf_curve_selective_gpu_offset_supported()) {
        if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
        if (representativeOffsetkHzOut) *representativeOffsetkHzOut = 0;
        if (detectedExcludeLowCountOut) *detectedExcludeLowCountOut = 0;
        return false;
    }

    const int ownedLockCi = vf_live_offset_owned_lock_ci(
        g_app.lockedCi, (int)g_app.lockMode, g_app.lockedFreq);
    const VfSelectiveGpuOffsetDetection det = vf_detect_live_selective_gpu_offset(
        g_app.curve, g_app.freqOffsets, &g_app.vfFloorBinding.floor, ownedLockCi);
    const char* tailSource = det.tail.fromOwnedLock ? "owned"
        : (det.tail.fromObservedTail ? "observed" : "none");

    if (det.detected) {
        debug_log_on_change(
            "detect_live_selective: BEST exclude=%d MHz=%d matches=%d/%d violations=%d/%d tail=ci%d(%s,plateau=%u) candidates=%d\n",
            det.excludeLowCount, det.gpuOffsetMHz, det.matchCount, det.consideredCount,
            det.excludedViolations, det.excludedTotal, det.tail.tailStartCi, tailSource,
            det.tail.observedPlateauMHz, det.candidateCount);
        if (gpuOffsetMHzOut) *gpuOffsetMHzOut = det.gpuOffsetMHz;
        if (representativeOffsetkHzOut) *representativeOffsetkHzOut = det.representativeOffsetKHz;
        if (detectedExcludeLowCountOut) *detectedExcludeLowCountOut = det.excludeLowCount;
        return true;
    }

    if (det.candidateCount > 0) {
        char sampleBuf[96] = {};
        for (int i = 0; i < det.candidateCount && i < 5; i++) {
            char part[24] = {};
            StringCchPrintfA(part, ARRAY_COUNT(part), "%s%d", i > 0 ? "," : "", det.candidateSampleMHz[i]);
            StringCchCatA(sampleBuf, ARRAY_COUNT(sampleBuf), part);
        }
        debug_log_on_change(
            "detect_live_selective: none (candidates=%d [%s] MHz, populated=%d, tail=ci%d(%s,plateau=%u))\n",
            det.candidateCount, sampleBuf, det.populatedCount, det.tail.tailStartCi, tailSource,
            det.tail.observedPlateauMHz);
    } else {
        debug_log_on_change(
            "detect_live_selective: none (candidates=0, populated=%d, tail=ci%d(%s,plateau=%u))\n",
            det.populatedCount, det.tail.tailStartCi, tailSource, det.tail.observedPlateauMHz);
    }
    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
    if (representativeOffsetkHzOut) *representativeOffsetkHzOut = 0;
    if (detectedExcludeLowCountOut) *detectedExcludeLowCountOut = 0;
    return false;
}

static bool live_selective_gpu_offset_matches_requested_state_with_tolerance(int gpuOffsetMHz, int toleranceMHz) {
    if (!vf_curve_selective_gpu_offset_supported() || gpuOffsetMHz == 0) return false;
    if (toleranceMHz < 0) toleranceMHz = 0;

    int detectedSelectiveOffsetMHz = 0, representativeOffsetkHz = 0, detectedExclude = 0;
    if (!detect_live_selective_gpu_offset_state(&detectedSelectiveOffsetMHz, &representativeOffsetkHz, &detectedExclude)) return false;

    int tolerancekHz = toleranceMHz * 1000;
    int requestedOffsetkHz = gpuOffsetMHz * 1000;
    return abs(representativeOffsetkHz - requestedOffsetkHz) <= tolerancekHz;
}

static bool live_selective_gpu_offset_matches_requested_shape(int gpuOffsetMHz, int excludeLowCount, int toleranceMHz, int* representativeOffsetkHzOut) {
    if (representativeOffsetkHzOut) *representativeOffsetkHzOut = 0;
    if (!vf_curve_selective_gpu_offset_supported() || gpuOffsetMHz == 0 || excludeLowCount <= 0) return false;
    return vf_live_selective_gpu_offset_matches_shape(
        g_app.curve, g_app.freqOffsets, &g_app.vfFloorBinding.floor, g_app.lockedCi,
        gpuOffsetMHz, excludeLowCount, toleranceMHz, representativeOffsetkHzOut);
}

static bool load_runtime_selective_gpu_offset_request(int* gpuOffsetMHzOut, int* excludeLowCountOut) {
    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
    if (excludeLowCountOut) *excludeLowCountOut = 0;
    if (!g_app.configPath[0]) return false;

    char buf[32] = {};
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return false;
    gc_GetPrivateProfileStringUtf8("runtime", "selective_gpu_offset_mhz", "", buf, sizeof(buf), g_app.configPath);
    trim_ascii(buf);
    if (!buf[0]) {
        leave_config_storage_lock(configMutex);
        return false;
    }

    int gpuOffsetMHz = 0;
    if (!parse_int_strict(buf, &gpuOffsetMHz)) {
        leave_config_storage_lock(configMutex);
        return false;
    }
    // Reject values that would overflow when multiplied by 1000 for kHz conversion.
    // The IPC validation clamps to ±1000, but persisted config values bypass it.
    if (gpuOffsetMHz < -1000000 || gpuOffsetMHz > 1000000) {
        debug_log("load_runtime_selective: rejecting out-of-range gpuOffsetMHz=%d\n", gpuOffsetMHz);
        leave_config_storage_lock(configMutex);
        return false;
    }

    // Read both values under the same mutex to avoid TOCTOU race.
    // The exclude count may be stored as "selective_gpu_offset_exclude_low_count"
    // (new format) or "selective_gpu_offset_exclude_low_70" (legacy).
    char excludeBuf[32] = {};
    gc_GetPrivateProfileStringUtf8("runtime", "selective_gpu_offset_exclude_low_count", "", excludeBuf, sizeof(excludeBuf), g_app.configPath);
    bool hasExplicitLowCount = excludeBuf[0] != '\0';
    if (!hasExplicitLowCount) {
        gc_GetPrivateProfileStringUtf8("runtime", "selective_gpu_offset_exclude_low_70", "", excludeBuf, sizeof(excludeBuf), g_app.configPath);
    }
    leave_config_storage_lock(configMutex);

    int excludeLowCount = 0;
    trim_ascii(excludeBuf);
    if (excludeBuf[0]) {
        int value = 0;
        if (!parse_int_strict(excludeBuf, &value)) return false;
        if (hasExplicitLowCount) {
            excludeLowCount = value;
        } else {
            excludeLowCount = value ? 70 : 0;
        }
    }

    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = gpuOffsetMHz;
    if (excludeLowCountOut) *excludeLowCountOut = excludeLowCount;
    return true;
}

static bool desired_settings_has_explicit_curve(const DesiredSettings* desired) {
    if (!desired) return false;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i]) return true;
    }
    return false;
}

static bool selective_gpu_offset_curve_shape_looks_safe(const DesiredSettings* desired, int gpuOffsetMHz, int excludeLowCount) {
    if (!desired) return false;
    if (gpuOffsetMHz == 0 || excludeLowCount <= 0) return true;
    if (!desired_settings_has_explicit_curve(desired)) return true;

    bool sawExcludedPoint = false;
    bool sawIncludedPoint = false;
    int firstIncludedCi = -1;
    unsigned int previousMHz = 0;
    bool havePreviousMHz = false;

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (!desired->hasCurvePoint[ci]) continue;
        unsigned int mhz = desired->curvePointMHz[ci];
        if (mhz == 0) continue;

        bool excluded = is_gpu_offset_excluded_low_point(ci, gpuOffsetMHz, excludeLowCount);
        if (excluded) {
            sawExcludedPoint = true;
            if (firstIncludedCi >= 0) return false;
        } else {
            sawIncludedPoint = true;
            if (firstIncludedCi < 0) firstIncludedCi = ci;
        }

        if (havePreviousMHz && mhz < previousMHz) return false;
        previousMHz = mhz;
        havePreviousMHz = true;
    }

    if (!sawIncludedPoint) return false;
    if (!sawExcludedPoint) return true;
    return firstIncludedCi >= excludeLowCount;
}

static bool control_state_has_meaningful_gpu(const ControlState* state) {
    return state && state->valid && state->hasGpuOffset;
}

static bool control_state_has_meaningful_mem(const ControlState* state) {
    return state && state->valid && state->hasMemOffset;
}

static bool control_state_has_meaningful_power(const ControlState* state) {
    return state && state->valid && state->hasPowerLimit;
}

static bool control_state_has_meaningful_fan(const ControlState* state) {
    return state && state->valid && state->hasFan;
}

static bool control_state_has_any_meaningful_value(const ControlState* state) {
    return control_state_has_meaningful_gpu(state)
        || control_state_has_meaningful_mem(state)
        || control_state_has_meaningful_power(state)
        || control_state_has_meaningful_fan(state);
}

static int current_green_curve_fan_intent_mode() {
    if (app_is_service_process()) {
        // fan_intent_policy.h: the owned intent, else the service's own
        // runtime -- never the activeFanMode mirror, which a post-write refresh
        // re-derives from the OUTGOING intent while an Apply is running.
        const bool intentOwnsFan = g_serviceHasActiveDesired && g_serviceActiveDesired.hasFan;
        return service_reported_fan_intent_mode(intentOwnsFan,
            g_serviceActiveDesired.fanMode, g_serviceActiveDesired.fanAuto != 0,
            g_app.fanCurveRuntimeActive, g_app.fanFixedRuntimeActive);
    }
    if (g_app.fanCurveRuntimeActive) return FAN_MODE_CURVE;
    if (g_app.fanFixedRuntimeActive) return FAN_MODE_FIXED;
    if (g_app.activeFanMode == FAN_MODE_CURVE) return FAN_MODE_CURVE;
    if (g_app.activeFanMode == FAN_MODE_FIXED && g_app.fanFixedRuntimeActive) return FAN_MODE_FIXED;
    return FAN_MODE_AUTO;
}

static int current_green_curve_fan_intent_fixed_percent() {
    if (app_is_service_process() && g_serviceHasActiveDesired && g_serviceActiveDesired.hasFan) {
        return g_serviceActiveDesired.fanMode == FAN_MODE_FIXED
            ? clamp_percent(g_serviceActiveDesired.fanPercent)
            : 0;
    }
    if (current_green_curve_fan_intent_mode() == FAN_MODE_FIXED) {
        return clamp_percent(g_app.activeFanFixedPercent);
    }
    return 0;
}

static const FanCurveConfig* current_green_curve_fan_intent_curve() {
    if (app_is_service_process() && g_serviceHasActiveDesired && g_serviceActiveDesired.hasFan
        && g_serviceActiveDesired.fanMode == FAN_MODE_CURVE) {
        return &g_serviceActiveDesired.fanCurve;
    }
    return &g_app.activeFanCurve;
}

static void set_gui_state_dirty(bool dirty) {
    if (g_debug_logging && g_app.guiStateDirty != dirty) {
        debug_log("set_gui_state_dirty: %d -> %d (programmaticDepth=%d)\n",
            g_app.guiStateDirty ? 1 : 0,
            dirty ? 1 : 0,
            g_programmaticEditUpdateDepth);
    }
#ifndef GREEN_CURVE_SERVICE_BINARY
    if (dirty && !g_app.guiStateDirty) gui_draft_begin_user_edit();
#endif
    g_app.guiStateDirty = dirty;
}

#include "gpu_lock_ownership_policy.h"
#include "gui_observed_lock_policy.h"
#include "service_reset_lock_policy.h"

static bool gui_state_dirty() {
    return g_app.guiStateDirty;
}
static bool should_accept_service_curve_lock_detection() {
    if (!g_app.usingBackgroundService) return true;
    if (gui_state_dirty()) return false;
    if (g_app.lockedCi >= 0 && g_app.lockedCi < VF_NUM_POINTS && g_app.lockedFreq > 0) return false;
    return true;
}
static bool should_auto_detect_locked_tail_from_live_curve() {
    if (!gpu_live_lock_ownership_proven({
            app_is_service_process(), g_serviceHasActiveDesired,
            g_serviceHasActiveDesired && g_serviceActiveDesired.hasLock,
            g_serviceHasActiveDesired ? g_serviceActiveDesired.lockCi : -1,
            g_serviceHasActiveDesired ? g_serviceActiveDesired.lockMHz : 0u,
            g_app.usingBackgroundService, g_app.appliedLockCi,
            g_app.appliedLockFreq, (int)g_app.appliedLockMode}))
        return false;
    return g_app.lockedCi < 0 || g_app.lockedCi >= VF_NUM_POINTS || g_app.lockedFreq == 0;
}
static void persist_runtime_selective_gpu_offset_request(int gpuOffsetMHz, int excludeLowCount) {
    if (!g_app.configPath[0]) return;
    if (gpuOffsetMHz == 0 || excludeLowCount <= 0) {
        debug_log("persist_runtime_selective_gpu_offset_request: non-selective request clears runtime state (%d MHz excludeLowCount=%d)\n",
            gpuOffsetMHz,
            excludeLowCount);
        clear_runtime_selective_gpu_offset_request();
        return;
    }
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return;
    debug_log("persist_runtime_selective_gpu_offset_request: storing %d MHz excludeLowCount=%d\n",
        gpuOffsetMHz,
        excludeLowCount);
    char buf[32] = {};
    char countBuf[32] = {};
    StringCchPrintfA(buf, ARRAY_COUNT(buf), "%d", gpuOffsetMHz);
    StringCchPrintfA(countBuf, ARRAY_COUNT(countBuf), "%d", excludeLowCount);
    char section[128] = {};
    StringCchPrintfA(section, ARRAY_COUNT(section),
        "selective_gpu_offset_mhz=%s%cselective_gpu_offset_exclude_low_count=%s%c%c",
        buf,
        '\0',
        countBuf,
        '\0',
        '\0');
    gc_WritePrivateProfileSectionUtf8("runtime", section, g_app.configPath);
    leave_config_storage_lock(configMutex);
}

static void clear_runtime_selective_gpu_offset_request() {
    if (!g_app.configPath[0]) return;
    HANDLE configMutex = nullptr;
    if (!enter_config_storage_lock(&configMutex)) return;
    debug_log("clear_runtime_selective_gpu_offset_request: clearing runtime selective state\n");
    gc_WritePrivateProfileStringUtf8("runtime", nullptr, nullptr, g_app.configPath);
    leave_config_storage_lock(configMutex);
}

static bool load_matching_runtime_selective_gpu_offset_request(int* gpuOffsetMHzOut, int* excludeLowCountOut) {
    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
    if (excludeLowCountOut) *excludeLowCountOut = 0;

    int persistedOffsetMHz = 0;
    int persistedExcludeLowCount = 0;
    if (!load_runtime_selective_gpu_offset_request(&persistedOffsetMHz, &persistedExcludeLowCount)) return false;
    if (persistedExcludeLowCount <= 0 || persistedOffsetMHz == 0) {
        debug_log("runtime selective: ignoring persisted non-selective request value=%d exclude=%d\n",
            persistedOffsetMHz,
            persistedExcludeLowCount);
        return false;
    }

    if (!live_selective_gpu_offset_matches_requested_shape(persistedOffsetMHz, persistedExcludeLowCount, 12, nullptr)) {
        debug_log("runtime selective: ignoring persisted request value=%d exclude=%d because live readback does not match\n",
            persistedOffsetMHz,
            persistedExcludeLowCount);
        return false;
    }

    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = persistedOffsetMHz;
    if (excludeLowCountOut) *excludeLowCountOut = persistedExcludeLowCount;
    return true;
}

static bool live_curve_has_any_nonzero_offsets() {
    if (!g_app.loaded) return false;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        if (g_app.freqOffsets[ci] != 0) return true;
    }
    return false;
}

static bool service_active_desired_gpu_offset_fallback(int* gpuOffsetMHzOut, int* excludeLowCountOut) {
    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = 0;
    if (excludeLowCountOut) *excludeLowCountOut = 0;
    if (!app_is_service_process() || !g_serviceHasActiveDesired || !g_serviceActiveDesired.hasGpuOffset) return false;

    int gpuOffsetMHz = g_serviceActiveDesired.gpuOffsetMHz;
    int excludeLowCount = (g_serviceActiveDesired.gpuOffsetExcludeLowCount > 0 && gpuOffsetMHz != 0) ? g_serviceActiveDesired.gpuOffsetExcludeLowCount : 0;
    if (gpuOffsetMHz == 0 || excludeLowCount <= 0) return false;
    if (!live_curve_has_any_nonzero_offsets()) return false;

    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = gpuOffsetMHz;
    if (excludeLowCountOut) *excludeLowCountOut = excludeLowCount;
    return true;
}

static bool refresh_service_snapshot_and_active_desired(char* err, size_t errSize, DesiredSettings* activeDesiredOut) {
    if (activeDesiredOut) initialize_desired_settings_defaults(activeDesiredOut);
    if (app_is_service_process()) return true;
    if (!g_app.usingBackgroundService) {
        if (err && errSize > 0) err[0] = 0;
        return true;
    }

    bool previousUsingBackgroundService = g_app.usingBackgroundService;
    bool serviceAvailable = refresh_background_service_state();
    if (!serviceAvailable || !g_app.usingBackgroundService) {
        g_app.usingBackgroundService = previousUsingBackgroundService;
        set_message(err, errSize, "Background service is not available");
        return false;
    }

    ServiceResponse stateResponse = {};
    // CLI-only: no window to freeze, so it waits out the service's contract.
    if (!service_client_get_ready_state_contracted(&stateResponse, "refresh snapshot and active intent", err, errSize)) {
        return false;
    }
    // The atomic envelope's desired payload is retained for profile comparisons
    // (and synced with its identity) without projecting it over live snapshot
    // lock detection here.
    apply_service_snapshot_to_app(&stateResponse.snapshot,
        stateResponse.state.activeDesiredValid, &stateResponse.desired);
    if ((stateResponse.state.validSections &
            SERVICE_STATE_SECTION_APPLIED_CONTROLS) != 0)
        apply_control_state_to_gui(&stateResponse.controlState);
    if (activeDesiredOut && stateResponse.state.activeDesiredValid)
        *activeDesiredOut = stateResponse.desired;
    else if (activeDesiredOut)
        *activeDesiredOut = {};

    if (err && errSize > 0) err[0] = 0;
    return true;
}

static void build_full_live_desired_settings(DesiredSettings* desired) {
    if (!desired) return;
    initialize_desired_settings_defaults(desired);

    ControlState control = {};
    bool haveControlState = get_effective_control_state(&control);
    desired->hasGpuOffset = true;
    if (haveControlState && control_state_has_meaningful_gpu(&control)) {
        desired->gpuOffsetMHz = control.gpuOffsetMHz;
        desired->gpuOffsetExcludeLowCount = control.gpuOffsetExcludeLowCount;
    } else {
        resolve_displayed_live_gpu_offset_state_for_gui(&desired->gpuOffsetMHz, &desired->gpuOffsetExcludeLowCount);
    }
    desired->hasMemOffset = true;
    desired->memOffsetMHz = haveControlState && control_state_has_meaningful_mem(&control)
        ? control.memOffsetMHz
        : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    desired->hasPowerLimit = true;
    desired->powerLimitPct = haveControlState && control_state_has_meaningful_power(&control)
        ? control.powerLimitPct
        : g_app.powerLimitPct;
    desired->hasXbarOffsetKhz = g_app.xbarProbeValid &&
        haveControlState && control.hasXbarOffset;
    desired->xbarOffsetKhz = desired->hasXbarOffsetKhz
        ? control.xbarOffsetKhz : g_app.xbarFreqOffsetKhz;
    desired->hasXbarMsvddOffsetUv = g_app.xbarProbeValid &&
        haveControlState && control.hasXbarMsvddOffset;
    desired->xbarMsvddOffsetUv = desired->hasXbarMsvddOffsetUv
        ? control.xbarMsvddOffsetUv : g_app.xbarMsvddOffsetUv;
    desired->hasSysClkOffsetKhz = g_app.sysClkProbeValid &&
        haveControlState && control.hasSysClkOffset;
    desired->sysClkOffsetKhz = desired->hasSysClkOffsetKhz
        ? control.sysClkOffsetKhz : g_app.sysClkFreqOffsetKhz;
    desired->hasVideoClkOffsetKhz = g_app.videoClkProbeValid &&
        haveControlState && control.hasVideoClkOffset;
    desired->videoClkOffsetKhz = desired->hasVideoClkOffsetKhz
        ? control.videoClkOffsetKhz : g_app.videoClkFreqOffsetKhz;
    desired->hasFan = true;
    int fanIntentMode = haveControlState && control_state_has_meaningful_fan(&control)
        ? control.fanMode
        : current_green_curve_fan_intent_mode();
    if (fanIntentMode == FAN_MODE_AUTO && !g_app.fanIsAuto) {
        debug_log("build_full_live_desired_settings: live fan policy is manual but Green Curve intent is Auto; saving Auto\n");
    }
    desired->fanMode = fanIntentMode;
    desired->fanAuto = desired->fanMode == FAN_MODE_AUTO;
    desired->fanPercent = desired->fanMode == FAN_MODE_FIXED
        ? (haveControlState && control_state_has_meaningful_fan(&control)
            ? control.fanFixedPercent
            : current_green_curve_fan_intent_fixed_percent())
        : 0;
    copy_fan_curve(&desired->fanCurve,
        haveControlState && control_state_has_meaningful_fan(&control)
            ? &control.fanCurve
            : current_green_curve_fan_intent_curve());
    // Do not save the applied lock or curve points when saving from live state.
    // The VF curve may have been flattened by a previous profile's lock, and saving
    // those frequencies as explicit curve points would cause infer_profile_lock_from_curve
    // to detect the flat tail and re-apply the lock when the profile is loaded.
    // Without curve points, the apply path's reset-before-apply restores the GPU
    // to its factory base frequencies (via offset zeroing), which is the correct
    // "no custom curve" behavior for a default/safety profile.
}

static bool load_curve_points_explicit_from_section(const char* path, const char* section, DesiredSettings* desired, char* err, size_t errSize) {
    if (!path || !section || !desired) return false;
    const ProfileCurveDecode sectionDecode = profile_curve_section_decode(path, section);
    bool foundAny = false;
    int lastMHz = 0;
    int lastCi = -1;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        char key[32] = {};
        char buf[64] = {};
        StringCchPrintfA(key, ARRAY_COUNT(key), "point%d_mhz", i);
        gc_GetPrivateProfileStringUtf8(section, key, "", buf, sizeof(buf), path);
        trim_ascii(buf);
        if (!buf[0]) continue;

        int mhz = 0;
        if (!parse_int_strict(buf, &mhz) || mhz < 0 ||
            (mhz == 0 && !profile_curve_point_zero_mhz_in_section_allowed(
                path, section, sectionDecode, i))) {
            set_message(err, errSize, "Invalid %s in section [%s]", key, section);
            return false;
        }
        if (mhz > 5000) {
            debug_log("load_curve_points_explicit: warning %s=%d MHz exceeds sanity limit, clamping to 5000\n", key, mhz);
            mhz = 5000;
        }

        char visibleKey[32] = {};
        char visibleBuf[64] = {};
        StringCchPrintfA(visibleKey, ARRAY_COUNT(visibleKey), "point%d_visible", i);
        gc_GetPrivateProfileStringUtf8(section, visibleKey, "", visibleBuf, sizeof(visibleBuf), path);
        trim_ascii(visibleBuf);
        if (visibleBuf[0]) {
            int visibleValue = 0;
            if (!parse_int_strict(visibleBuf, &visibleValue)) {
                set_message(err, errSize, "Invalid %s in section [%s]", visibleKey, section);
                return false;
            }
            if (visibleValue == 0) {
                debug_log("load_curve_points_explicit: skipping hidden %s=%d in section [%s]\n", key, mhz, section);
                continue;
            }
        } else {
            char mvKey[32] = {};
            char mvBuf[64] = {};
            StringCchPrintfA(mvKey, ARRAY_COUNT(mvKey), "point%d_mv", i);
            gc_GetPrivateProfileStringUtf8(section, mvKey, "", mvBuf, sizeof(mvBuf), path);
            trim_ascii(mvBuf);
            int mv = 0;
            if (mvBuf[0] && parse_int_strict(mvBuf, &mv) && mv > 0 && mv < MIN_VISIBLE_VOLT_mV) {
                debug_log("load_curve_points_explicit: skipping legacy hidden %s=%d mV in section [%s]\n", mvKey, mv, section);
                continue;
            }
        }

        // Non-monotonic points can confuse the driver. Log a warning but still
        // load the point so the user can see it in the GUI.
        if (mhz > 0 && lastCi >= 0 && mhz < lastMHz) {
            char semBuf[64] = {};
            gc_GetPrivateProfileStringUtf8(section, "curve_semantics", "", semBuf, sizeof(semBuf), path);
            trim_ascii(semBuf);
            bool isBasePlusOffset = (_stricmp(semBuf, "base_plus_gpu_offset") == 0);
            if (isBasePlusOffset) {
                debug_log("load_curve_points_explicit: note %s=%d MHz is below point %d (%d MHz) "
                    "(base values before GPU offset applied; will be rechecked after offset restore)\n",
                    key, mhz, lastCi, lastMHz);
            } else {
                debug_log("load_curve_points_explicit: warning %s=%d MHz is below point %d (%d MHz)\n",
                    key, mhz, lastCi, lastMHz);
            }
        }

        desired->hasCurvePoint[i] = true;
        desired->curvePointMHz[i] = (unsigned int)mhz; desired->curvePointFromGpuOffset[i] = gc_bool8_from_bool(mhz == 0);
        foundAny = true;
        if (mhz > 0) { lastCi = i; lastMHz = mhz; }
    }
    return foundAny;
}

static bool is_curve_point_visible_in_gui(int pointIndex) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return false;
    return vf_point_listed(g_app.vfFloorBinding.floor, g_app.curve, pointIndex);
}

static void resolve_displayed_live_gpu_offset_state_for_gui(int* gpuOffsetMHzOut, int* excludeLowCountOut) {
    int gpuOffsetMHz = current_applied_gpu_offset_mhz();
    current_applied_gpu_offset_excludes_low_points();
    int excludeLowCount = g_app.appliedGpuOffsetExcludeLowCount;

    int persistedOffsetMHz = 0;
    int persistedExcludeLowCount = 0;
    if (load_matching_runtime_selective_gpu_offset_request(&persistedOffsetMHz, &persistedExcludeLowCount)) {
        gpuOffsetMHz = persistedOffsetMHz;
        excludeLowCount = persistedExcludeLowCount;
    }

    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = gpuOffsetMHz;
    if (excludeLowCountOut) *excludeLowCountOut = excludeLowCount;
}

static bool live_selective_gpu_offset_matches_requested_state(int gpuOffsetMHz) {
    if (!vf_curve_selective_gpu_offset_supported() || gpuOffsetMHz == 0) return false;
    int detectedMHz = 0, detectedExclude = 0;
    if (!detect_live_selective_gpu_offset_state(&detectedMHz, nullptr, &detectedExclude)) return false;
    return abs(detectedMHz - gpuOffsetMHz) <= 12;
}

// `fromHardware` reports whether the answer is a driver reading or remembered
// intent.  Two branches deliberately answer with intent so the editor stays
// populated when detection is ambiguous; protocol v14 must not publish those.
static int current_applied_gpu_offset_mhz(bool* fromHardware) {
    if (fromHardware) *fromHardware = g_app.readback.gpuOffset;
    if (!vf_curve_selective_gpu_offset_supported()) {
        int offsetMHz = g_app.gpuClockOffsetkHz / 1000;
        debug_log_on_change("current_applied_gpu_offset_mhz: not Blackwell, returning NVML offset=%d kHz -> %d MHz\n", g_app.gpuClockOffsetkHz, offsetMHz);
        g_app.appliedGpuOffsetMHz = offsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = 0;
        return g_app.appliedGpuOffsetMHz;
    }
    if (g_app.appliedGpuOffsetExcludeLowCount > 0 && g_app.appliedGpuOffsetMHz != 0
        && live_selective_gpu_offset_matches_requested_shape(g_app.appliedGpuOffsetMHz, g_app.appliedGpuOffsetExcludeLowCount, 12, nullptr)) {
        if (fromHardware) *fromHardware = true;
        debug_log_on_change("current_applied_gpu_offset_mhz: preserving session selective value=%d MHz\n", g_app.appliedGpuOffsetMHz);
        return g_app.appliedGpuOffsetMHz;
    }
    if (!g_app.lastApplyUsedGpuOffset) {
        debug_log_on_change("current_applied_gpu_offset_mhz: last apply did not use GPU offset, skipping detection\n");
        g_app.appliedGpuOffsetMHz = vf_curve_global_gpu_offset_supported() ? 0 : (g_app.gpuClockOffsetkHz / 1000);
        g_app.appliedGpuOffsetExcludeLowCount = 0;
        return g_app.appliedGpuOffsetMHz;
    }
    int persistedOffsetMHz = 0, persistedExcludeLowCount = 0;
    if (load_matching_runtime_selective_gpu_offset_request(&persistedOffsetMHz, &persistedExcludeLowCount)) {
        debug_log_on_change("current_applied_gpu_offset_mhz: preserving persisted request value=%d MHz exclude=%d\n", persistedOffsetMHz, persistedExcludeLowCount);
        g_app.appliedGpuOffsetMHz = persistedOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = persistedExcludeLowCount;
        if (fromHardware) *fromHardware = false;  // remembered request
        return persistedOffsetMHz;
    }
    int detectedSelectiveOffsetMHz = 0, detectedExcludeLowCount = 0;
    if (detect_live_selective_gpu_offset_state(&detectedSelectiveOffsetMHz, nullptr, &detectedExcludeLowCount)) {
        debug_log_on_change("current_applied_gpu_offset_mhz: detected selective offset=%d MHz exclude=%d\n", detectedSelectiveOffsetMHz, detectedExcludeLowCount);
        g_app.appliedGpuOffsetMHz = detectedSelectiveOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = detectedExcludeLowCount;
        if (fromHardware) *fromHardware = true;
        return detectedSelectiveOffsetMHz;
    }
    int desiredServiceOffsetMHz = 0, desiredServiceExcludeLowCount = 0;
    if (service_active_desired_gpu_offset_fallback(&desiredServiceOffsetMHz, &desiredServiceExcludeLowCount)) {
        debug_log_on_change("current_applied_gpu_offset_mhz: preserving service desired value=%d MHz exclude=%d\n", desiredServiceOffsetMHz, desiredServiceExcludeLowCount);
        g_app.appliedGpuOffsetMHz = desiredServiceOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = desiredServiceExcludeLowCount;
        if (fromHardware) *fromHardware = false;  // active desired intent
        return desiredServiceOffsetMHz;
    }
    int offsetMHz = g_app.gpuClockOffsetkHz / 1000;
    if (!vf_curve_global_gpu_offset_supported()) {
        debug_log_on_change("current_applied_gpu_offset_mhz: not Blackwell, returning NVML offset=%d kHz -> %d MHz\n", g_app.gpuClockOffsetkHz, offsetMHz);
    } else {
        debug_log_on_change("current_applied_gpu_offset_mhz: no selective detected, uniform offset=%d kHz -> %d MHz\n", g_app.gpuClockOffsetkHz, offsetMHz);
    }
    g_app.appliedGpuOffsetMHz = offsetMHz;
    g_app.appliedGpuOffsetExcludeLowCount = 0;
    return offsetMHz;
}

static bool current_applied_gpu_offset_excludes_low_points() {
    if (!vf_curve_selective_gpu_offset_supported()) {
        g_app.appliedGpuOffsetExcludeLowCount = 0;
        return false;
    }
    if (g_app.appliedGpuOffsetExcludeLowCount > 0 && g_app.appliedGpuOffsetMHz != 0
        && live_selective_gpu_offset_matches_requested_shape(g_app.appliedGpuOffsetMHz, g_app.appliedGpuOffsetExcludeLowCount, 12, nullptr)) {
        return true;
    }
    int persistedOffsetMHz = 0, persistedExcludeLowCount = 0;
    if (load_matching_runtime_selective_gpu_offset_request(&persistedOffsetMHz, &persistedExcludeLowCount)) {
        g_app.appliedGpuOffsetMHz = persistedOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = persistedExcludeLowCount;
        return persistedExcludeLowCount > 0;
    }
    if (!g_app.lastApplyUsedGpuOffset) {
        debug_log_on_change("current_applied_gpu_offset_excludes_low_points: last apply did not use GPU offset, skipping detection\n");
        g_app.appliedGpuOffsetMHz = vf_curve_global_gpu_offset_supported() ? 0 : (g_app.gpuClockOffsetkHz / 1000);
        g_app.appliedGpuOffsetExcludeLowCount = 0;
        return false;
    }
    int detectedSelectiveOffsetMHz = 0, detectedExcludeLowCount = 0;
    if (detect_live_selective_gpu_offset_state(&detectedSelectiveOffsetMHz, nullptr, &detectedExcludeLowCount)) {
        g_app.appliedGpuOffsetMHz = detectedSelectiveOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = detectedExcludeLowCount;
        return detectedExcludeLowCount > 0;
    }
    int desiredServiceOffsetMHz = 0, desiredServiceExcludeLowCount = 0;
    if (service_active_desired_gpu_offset_fallback(&desiredServiceOffsetMHz, &desiredServiceExcludeLowCount)) {
        g_app.appliedGpuOffsetMHz = desiredServiceOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = desiredServiceExcludeLowCount;
        return desiredServiceExcludeLowCount > 0;
    }
    g_app.appliedGpuOffsetMHz = g_app.gpuClockOffsetkHz / 1000;
    g_app.appliedGpuOffsetExcludeLowCount = 0;
    return false;
}

static void resolve_effective_gpu_offset_state_for_config_save(const DesiredSettings* desired, int* gpuOffsetMHzOut, int* excludeLowCountOut) {
    int gpuOffsetMHz = 0;
    int excludeLowCount = 0;

    if (desired && desired->hasGpuOffset) {
        gpuOffsetMHz = desired->gpuOffsetMHz;
        excludeLowCount = desired->gpuOffsetExcludeLowCount;
    } else {
        resolve_displayed_live_gpu_offset_state_for_gui(&gpuOffsetMHz, &excludeLowCount);
    }

    if (gpuOffsetMHz == 0) excludeLowCount = 0;
    if (gpuOffsetMHzOut) *gpuOffsetMHzOut = gpuOffsetMHz;
    if (excludeLowCountOut) *excludeLowCountOut = excludeLowCount;
}

