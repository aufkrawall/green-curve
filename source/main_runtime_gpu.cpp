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

static bool apply_curve_offsets_verified(const int* targetOffsets, const bool* pointMask, int maxBatchPasses) {
    if (!targetOffsets || !pointMask) return false;

    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend || !backend->writeSupported) return false;

    bool desiredMask[VF_NUM_POINTS] = {};
    int desiredOffsets[VF_NUM_POINTS] = {};
    bool pendingMask[VF_NUM_POINTS] = {};
    bool driverRefused[VF_NUM_POINTS] = {};
    int desiredCount = 0;

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!pointMask[i]) continue;
        if (g_app.curve[i].freq_kHz == 0) continue;
        unsigned char vfMaskByte = g_app.vfMask[i / 8];
        if (!(vfMaskByte & (1u << (i % 8)))) {
            debug_log("apply_curve_offsets_verified: point %d not in vfMask, skipping\n", i);
            continue;
        }
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
            bool converged = (g_app.freqOffsets[i] == desiredOffsets[i]);
            // Driver-refusal: a populated-but-placeholder VF entry (marked editable in
            // vfMask, but an idle/non-operating point — e.g. a high-index entry that
            // reports ~400 MHz, below the operating range) that the driver silently
            // pins at offset 0 no matter what we write. Retrying it costs a ~1s NVAPI
            // setControl per pass plus a per-point fallback, and it can never converge
            // (readback stays exactly 0). Detect the refusal (we asked for a non-zero
            // offset, the write returned success, but the readback is still exactly 0)
            // and accept it as non-offsettable instead of looping. Accepting it is a
            // hardware no-op — the driver was never going to move it.
            if (!converged && desiredOffsets[i] != 0 && g_app.freqOffsets[i] == 0) {
                if (!driverRefused[i]) {
                    driverRefused[i] = true;
                    debug_log("curve offset: driver refuses point %d (wrote %dkHz, readback pinned at 0, liveFreq=%ukHz); accepting as non-offsettable placeholder\n",
                        i, desiredOffsets[i], g_app.curve[i].freq_kHz);
                }
                pendingMask[i] = false;
                continue;
            }
            pendingMask[i] = !converged;
            if (pendingMask[i]) {
                anyPending = true;
                // Offset-convergence diagnostic: the driver accepted the write (ret=0)
                // but reports a DIFFERENT offset than we wrote, so this point keeps
                // being retried (each retry costs a ~1s NVAPI setControl). Log the
                // exact gap so we can see if the driver snaps/rounds/clamps the offset.
                debug_log("curve batch pass %d unconverged: ci=%d wroteOffset=%dkHz readbackOffset=%dkHz driverDelta=%dkHz liveFreq=%ukHz\n",
                    pass + 1, i, desiredOffsets[i], g_app.freqOffsets[i],
                    g_app.freqOffsets[i] - desiredOffsets[i], g_app.curve[i].freq_kHz);
            }
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
        if (driverRefused[i]) { pendingMask[i] = false; continue; }  // non-offsettable placeholder: don't fall back
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
            // Points the batch could not converge fall back to per-point writes.
            // Log the pre-fallback gap so the batch-vs-fallback behaviour is visible.
            bool pointOk = nvapi_set_point(i, desiredOffsets[i]);
            debug_log("curve fallback point %d target=%dkHz ok=%d (batch left readback=%dkHz, driverDelta=%dkHz)\n",
                i, desiredOffsets[i], pointOk ? 1 : 0,
                g_app.freqOffsets[i], g_app.freqOffsets[i] - desiredOffsets[i]);
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

    // Final offset-convergence summary: which points the driver still refuses to
    // honor exactly (these are what force the separate correction phase + extra ~1s
    // driver writes). Distinguishes a driver-rounded offset from a true write miss.
    int unconvergedCount = 0;
    int refusedCount = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desiredMask[i]) continue;
        if (driverRefused[i]) { refusedCount++; continue; }  // accepted: driver won't move a placeholder, not a real miss
        if (g_app.freqOffsets[i] != desiredOffsets[i]) {
            allOk = false;
            unconvergedCount++;
            debug_log("curve offset unconverged (final): ci=%d wroteOffset=%dkHz readbackOffset=%dkHz driverDelta=%dkHz liveFreq=%ukHz\n",
                i, desiredOffsets[i], g_app.freqOffsets[i],
                g_app.freqOffsets[i] - desiredOffsets[i], g_app.curve[i].freq_kHz);
        }
    }
    debug_log("apply_curve_offsets_verified: done desired=%d unconverged=%d driverRefused=%d allOk=%d (unconverged points force the correction phase's extra ~1s writes; refused placeholders are accepted)\n",
        desiredCount, unconvergedCount, refusedCount, allOk ? 1 : 0);

    return allOk;
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
    return driverMHz / 2; // NVML memory offset MHz is effective; UI mirrors actual MHz
}

static void invalidate_main_window() {
    if (!g_app.hMainWnd) return;
    if (g_app.trayWindowHiddenIntent ||
        !IsWindowVisible(g_app.hMainWnd)) {
        // Background profile/service changes update the resident control model,
        // but a tray-hidden owner has no frame to present.  Leave one deferred
        // invalidation for the next explicit show; an immediate WM_PAINT is
        // both wasted work and a needless opportunity for hidden-window side
        // effects in future paint code.
        RedrawWindow(g_app.hMainWnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ALLCHILDREN);
        return;
    }
    redraw_window_sync(g_app.hMainWnd);
}

static void redraw_window_sync(HWND hwnd) {
    if (!hwnd) return;
    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
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

