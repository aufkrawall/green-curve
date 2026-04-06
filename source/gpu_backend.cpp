static bool apply_desired_settings(const DesiredSettings* desired, bool interactive, char* result, size_t resultSize) {
    if (!desired) {
        set_message(result, resultSize, "No desired settings");
        return false;
    }

    DeleteFileA(APP_LOG_FILE);

    int successCount = 0;
    int failCount = 0;
    char failureDetails[1024] = {};
    auto append_failure = [&](const char* fmt, ...) {
        char part[256] = {};
        va_list ap;
        va_start(ap, fmt);
        StringCchVPrintfA(part, ARRAY_COUNT(part), fmt, ap);
        va_end(ap);
        if (!part[0]) return;
        if (failureDetails[0]) {
            StringCchCatA(failureDetails, ARRAY_COUNT(failureDetails), "; ");
        }
        StringCchCatA(failureDetails, ARRAY_COUNT(failureDetails), part);
        debug_log("apply failure: %s\n", part);
    };
    bool hasLock = interactive && g_app.lockedVi >= 0 && g_app.lockedVi < g_app.numVisible;
    bool hasCurveEdits = false;
    int lockCi = -1;
    int lockVi = -1;
    unsigned int lockMhz = 0;
    bool memOffsetValid = true;
    bool shouldApplyMemOffset = false;
    int targetMemkHz = 0;
    bool memApplied = false;
    bool powerChanged = false;
    bool fanChanged = false;
    int currentAppliedGpuOffsetMHz = current_applied_gpu_offset_mhz();
    bool currentActiveGpuOffsetExcludeLow70 = current_applied_gpu_offset_excludes_low_points();
    int targetGpuOffsetkHz = currentAppliedGpuOffsetMHz * 1000;
    bool gpuOffsetValid = true;
    bool shouldApplyGpuOffset = false;
    bool desiredActiveGpuOffsetExcludeLow70 = false;
    bool gpuPolicyViaCurveBatch = false;
    bool gpuPolicyChangeRequested = false;
    int originalCurveOffsets[VF_NUM_POINTS] = {};
    int originalCurveFreqkHz[VF_NUM_POINTS] = {};
    bool originalCurvePopulated[VF_NUM_POINTS] = {};
    int targetCurveOffsets[VF_NUM_POINTS] = {};
    bool targetCurveMask[VF_NUM_POINTS] = {};
    bool lockedTailMask[VF_NUM_POINTS] = {};
    bool haveNonZeroCurveOffsets = false;

    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        originalCurveOffsets[ci] = g_app.freqOffsets[ci];
        originalCurveFreqkHz[ci] = (int)g_app.curve[ci].freq_kHz;
        originalCurvePopulated[ci] = g_app.curve[ci].freq_kHz > 0;
        if (originalCurveOffsets[ci] != 0) haveNonZeroCurveOffsets = true;
        if (desired->hasCurvePoint[ci]) {
            hasCurveEdits = true;
        }
    }

    if (hasLock) {
        if (g_app.lockedCi >= 0 && g_app.lockedCi < VF_NUM_POINTS && g_app.lockedFreq > 0) {
            lockCi = g_app.lockedCi;
        } else {
            lockCi = g_app.visibleMap[g_app.lockedVi];
        }
        for (int vi = 0; vi < g_app.numVisible; vi++) {
            if (g_app.visibleMap[vi] == lockCi) {
                lockVi = vi;
                break;
            }
        }
        if (lockVi < 0) {
            hasLock = false;
        } else {
            lockMhz = desired->hasCurvePoint[lockCi] ? desired->curvePointMHz[lockCi] : get_edit_value(g_app.hEditsMhz[lockVi]);
            for (int vi = lockVi; vi < g_app.numVisible; vi++) {
                int ci = g_app.visibleMap[vi];
                if (ci >= 0 && ci < VF_NUM_POINTS) lockedTailMask[ci] = true;
            }
        }
    }

    if (desired->hasMemOffset) {
        if (!g_app.memOffsetRangeKnown ||
            (desired->memOffsetMHz >= g_app.memClockOffsetMinMHz && desired->memOffsetMHz <= g_app.memClockOffsetMaxMHz)) {
            targetMemkHz = mem_driver_khz_from_display_mhz(desired->memOffsetMHz);
            shouldApplyMemOffset = (g_app.memClockOffsetkHz != targetMemkHz);
        } else {
            memOffsetValid = false;
        }
    }

    if (desired->hasGpuOffset) {
        if (!g_app.gpuOffsetRangeKnown ||
            (desired->gpuOffsetMHz >= g_app.gpuClockOffsetMinMHz && desired->gpuOffsetMHz <= g_app.gpuClockOffsetMaxMHz)) {
            desiredActiveGpuOffsetExcludeLow70 = desired->gpuOffsetExcludeLow70 && desired->gpuOffsetMHz != 0;
            targetGpuOffsetkHz = desired->gpuOffsetMHz * 1000;
            gpuPolicyChangeRequested =
                (targetGpuOffsetkHz != currentAppliedGpuOffsetMHz * 1000) ||
                (desiredActiveGpuOffsetExcludeLow70 != currentActiveGpuOffsetExcludeLow70);
            shouldApplyGpuOffset = gpuPolicyChangeRequested && !currentActiveGpuOffsetExcludeLow70 && !desiredActiveGpuOffsetExcludeLow70;
            gpuPolicyViaCurveBatch = gpuPolicyChangeRequested && (currentActiveGpuOffsetExcludeLow70 || desiredActiveGpuOffsetExcludeLow70);
            debug_log("desired gpu offset mhz=%d current=%d shouldApply=%d viaCurve=%d desiredSelective=%d currentSelective=%d\n",
                desired->gpuOffsetMHz,
                currentAppliedGpuOffsetMHz,
                shouldApplyGpuOffset ? 1 : 0,
                gpuPolicyViaCurveBatch ? 1 : 0,
                desiredActiveGpuOffsetExcludeLow70 ? 1 : 0,
                currentActiveGpuOffsetExcludeLow70 ? 1 : 0);
        } else {
            gpuOffsetValid = false;
            debug_log("desired gpu offset mhz=%d rejected by range %d..%d\n",
                desired->gpuOffsetMHz, g_app.gpuClockOffsetMinMHz, g_app.gpuClockOffsetMaxMHz);
        }
    }

    if (!gpuOffsetValid) {
        failCount++;
        append_failure("GPU offset %d MHz is outside the supported range %d..%d MHz",
            desired->gpuOffsetMHz, g_app.gpuClockOffsetMinMHz, g_app.gpuClockOffsetMaxMHz);
    }
    if (!memOffsetValid) {
        failCount++;
        append_failure("Memory offset %d MHz is outside the supported range %d..%d MHz",
            desired->memOffsetMHz, g_app.memClockOffsetMinMHz, g_app.memClockOffsetMaxMHz);
    }

    bool gpuApplied = false;

    // Apply GPU offset first via dedicated path (handles uniform offset reliably).
    // When combined with lock/curve edits, applying the GPU offset separately avoids
    // sending highly non-uniform offsets for all curve points in a single batch,
    // which can fail. After this, the curve batch only needs to adjust the lock tail.
    if (gpuOffsetValid && shouldApplyGpuOffset) {
        if (nvapi_set_gpu_offset(targetGpuOffsetkHz)) {
            successCount++;
            gpuApplied = true;
            g_app.appliedGpuOffsetMHz = desired->gpuOffsetMHz;
            g_app.appliedGpuOffsetExcludeLow70 = false;
        } else {
            failCount++;
            append_failure("GPU offset %d MHz was not accepted by the driver", desired->gpuOffsetMHz);
        }
    }

    // Refresh cached originals after GPU offset so subsequent curve computations
    // use the post-offset state
    if (gpuApplied) {
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            originalCurveOffsets[ci] = g_app.freqOffsets[ci];
            originalCurveFreqkHz[ci] = (int)g_app.curve[ci].freq_kHz;
        }
    }

    bool preserveCurveAcrossMem = shouldApplyMemOffset && (haveNonZeroCurveOffsets || gpuApplied);
    bool userCurveRequest = hasCurveEdits || hasLock;
    bool curveRequest = userCurveRequest || gpuPolicyViaCurveBatch;

    if (curveRequest || preserveCurveAcrossMem) {
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (!originalCurvePopulated[ci]) continue;
            targetCurveOffsets[ci] = originalCurveOffsets[ci];
            if (preserveCurveAcrossMem) targetCurveMask[ci] = true;
        }

        if (gpuPolicyViaCurveBatch) {
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!originalCurvePopulated[ci]) continue;
                int currentPointGpuOffsetkHz = gpu_offset_component_mhz_for_point(ci, currentAppliedGpuOffsetMHz, currentActiveGpuOffsetExcludeLow70) * 1000;
                int desiredPointGpuOffsetkHz = gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLow70) * 1000;
                int targetOffset = clamp_freq_delta_khz(originalCurveOffsets[ci] - currentPointGpuOffsetkHz + desiredPointGpuOffsetkHz);
                targetCurveOffsets[ci] = targetOffset;
                if (targetOffset != originalCurveOffsets[ci]) {
                    targetCurveMask[ci] = true;
                }
            }
        }

        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (!desired->hasCurvePoint[ci]) continue;
            if (!originalCurvePopulated[ci]) continue;
            if (hasLock && ci >= lockCi) continue;
            long long base = (long long)originalCurveFreqkHz[ci] - (long long)originalCurveOffsets[ci];
            if (base < 0) base = 0;
            long long target = (long long)desired->curvePointMHz[ci] * 1000LL;
            targetCurveOffsets[ci] = clamp_freq_delta_khz((int)(target - base));
            targetCurveMask[ci] = true;
        }

        if (hasLock && lockMhz > 0) {
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!lockedTailMask[ci]) continue;
                if (!originalCurvePopulated[ci]) continue;
                long long base = (long long)originalCurveFreqkHz[ci] - (long long)originalCurveOffsets[ci];
                if (base < 0) base = 0;
                long long target = (long long)lockMhz * 1000LL;
                targetCurveOffsets[ci] = clamp_freq_delta_khz((int)(target - base));
                targetCurveMask[ci] = true;
            }
        }
    }

    if (desired->hasMemOffset) {
        if (memOffsetValid) {
            if (shouldApplyMemOffset) {
                if (nvapi_set_mem_offset(targetMemkHz)) {
                    successCount++;
                    memApplied = true;
                } else {
                    failCount++;
                    append_failure("Memory offset %d MHz was not accepted by the driver", desired->memOffsetMHz);
                }
            }
        }
    }

    bool curveBatchOk = true;
    bool curveBatchNeeded = false;
    bool curveTouched = gpuApplied;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (targetCurveMask[ci]) {
            curveBatchNeeded = true;
            break;
        }
    }
    if (curveBatchNeeded && (curveRequest || memApplied)) {
        curveTouched = true;
        curveBatchOk = apply_curve_offsets_verified(targetCurveOffsets, targetCurveMask, hasLock ? 3 : 2);
        char curveVerifyDetail[256] = {};
        bool curveRequestOk = true;
        DesiredSettings verifyDesired = *desired;

        if (gpuPolicyViaCurveBatch) {
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                if (!originalCurvePopulated[ci] || verifyDesired.hasCurvePoint[ci]) continue;

                long long targetFreqkHz = (long long)originalCurveFreqkHz[ci]
                    - (long long)gpu_offset_component_mhz_for_point(ci, currentAppliedGpuOffsetMHz, currentActiveGpuOffsetExcludeLow70) * 1000LL
                    + (long long)gpu_offset_component_mhz_for_point(ci, desired->gpuOffsetMHz, desiredActiveGpuOffsetExcludeLow70) * 1000LL;
                if (targetFreqkHz < 0) targetFreqkHz = 0;

                verifyDesired.hasCurvePoint[ci] = true;
                verifyDesired.curvePointMHz[ci] = displayed_curve_mhz((unsigned int)targetFreqkHz);
            }
        }

        auto verify_curve_request = [&](char* detailOut, size_t detailOutSize) -> bool {
            if (!curveRequest) return true;
            return curve_targets_match_request(&verifyDesired, hasLock ? lockedTailMask : nullptr, lockMhz, detailOut, detailOutSize);
        };

        if (curveRequest) {
            curveRequestOk = verify_curve_request(curveVerifyDetail, sizeof(curveVerifyDetail));
            if (!curveRequestOk) {
                for (int correctionPass = 0; correctionPass < 2; correctionPass++) {
                    int correctedCurveOffsets[VF_NUM_POINTS] = {};
                    bool correctedCurveMask[VF_NUM_POINTS] = {};
                    bool haveCorrections = false;

                    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                        unsigned int targetMHz = 0;
                        bool haveTarget = false;

                        if (hasLock && lockedTailMask[ci] && lockMhz > 0) {
                            targetMHz = lockMhz;
                            haveTarget = true;
                        } else if (verifyDesired.hasCurvePoint[ci]) {
                            targetMHz = verifyDesired.curvePointMHz[ci];
                            haveTarget = true;
                        }

                        if (!haveTarget || g_app.curve[ci].freq_kHz == 0) continue;

                        correctedCurveOffsets[ci] = curve_delta_khz_for_target_display_mhz(ci, targetMHz);
                        correctedCurveMask[ci] = true;
                        haveCorrections = true;
                    }

                    if (!haveCorrections) break;

                    bool correctionOk = apply_curve_offsets_verified(correctedCurveOffsets, correctedCurveMask, hasLock ? 3 : 2);
                    if (!correctionOk) {
                        debug_log("curve correction pass %d had an offset verification mismatch\n", correctionPass + 1);
                    }
                    if (verify_curve_request(curveVerifyDetail, sizeof(curveVerifyDetail))) {
                        curveRequestOk = true;
                        debug_log("curve correction pass %d converged to requested live MHz targets\n", correctionPass + 1);
                        break;
                    }
                }
            }
            if (curveRequestOk && !curveBatchOk) {
                debug_log("curve request matched live targets after offset verification mismatch\n");
            }
        }
        if (curveRequest) {
            if (curveRequestOk) {
                successCount++;
                if (gpuPolicyViaCurveBatch) {
                    g_app.appliedGpuOffsetMHz = desired->gpuOffsetMHz;
                    g_app.appliedGpuOffsetExcludeLow70 = desiredActiveGpuOffsetExcludeLow70;
                }
            } else {
                failCount++;
                if (curveVerifyDetail[0]) {
                    append_failure("%s", curveVerifyDetail);
                } else if (hasLock && lockMhz > 0) {
                    append_failure("Curve lock to %u MHz did not verify after apply", lockMhz);
                } else if (gpuPolicyViaCurveBatch) {
                    append_failure("GPU offset %d MHz did not verify after apply", desired->gpuOffsetMHz);
                } else {
                    append_failure("VF curve update did not verify after apply");
                }
            }
        }
        if (memApplied && preserveCurveAcrossMem && !curveRequestOk && !curveRequest) {
            failCount++;
            append_failure("Restoring the existing VF curve after the memory offset did not verify");
        }
    }

    if (hasLock) {
        g_app.lockedCi = lockCi;
        if (lockCi >= 0 && lockCi < VF_NUM_POINTS && g_app.curve[lockCi].freq_kHz > 0) {
            g_app.lockedFreq = displayed_curve_mhz(g_app.curve[lockCi].freq_kHz);
        } else {
            g_app.lockedFreq = lockMhz;
        }
    }
    if (desired->hasPowerLimit) {
        int currentPowerPct = g_app.powerLimitPct;
        if (desired->powerLimitPct != currentPowerPct) {
            powerChanged = true;
            if (nvapi_set_power_limit(desired->powerLimitPct)) successCount++; else {
                failCount++;
                append_failure("Power limit %d%% was not accepted by the driver", desired->powerLimitPct);
            }
        }
    }
    if (desired->hasFan) {
        int desiredFanMode = desired->fanMode;
        if (desiredFanMode < FAN_MODE_AUTO || desiredFanMode > FAN_MODE_CURVE) {
            desiredFanMode = desired->fanAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
        }
        FanCurveConfig desiredCurve = desired->fanCurve;
        fan_curve_normalize(&desiredCurve);
        if (desiredFanMode == FAN_MODE_CURVE) {
            char validationErr[256] = {};
            if (!fan_curve_validate(&desiredCurve, validationErr, sizeof(validationErr))) {
                set_message(result, resultSize, "%s", validationErr);
                return false;
            }
        }

        if (!fan_setting_matches_current(desiredFanMode, desired->fanPercent, &desiredCurve)) {
            fanChanged = true;
            bool exact = false;
            char detail[128] = {};
            bool ok = false;
            if (desiredFanMode == FAN_MODE_AUTO) {
                stop_fan_curve_runtime();
                ok = nvml_set_fan_auto(detail, sizeof(detail));
                if (ok) {
                    g_app.activeFanMode = FAN_MODE_AUTO;
                    g_app.activeFanFixedPercent = g_app.fanCount ? (int)g_app.fanPercent[0] : 0;
                }
            } else if (desiredFanMode == FAN_MODE_FIXED) {
                stop_fan_curve_runtime();
                if (!g_app.fanRangeKnown ||
                    (desired->fanPercent >= (int)g_app.fanMinPct && desired->fanPercent <= (int)g_app.fanMaxPct) ||
                    desired->fanPercent == 0) {
                    if (g_app.hMainWnd) {
                        g_app.activeFanFixedPercent = clamp_percent(desired->fanPercent);
                        start_fixed_fan_runtime();
                        ok = g_app.fanFixedRuntimeActive && g_app.fanRuntimeLastApplyTickMs != 0;
                        if (!ok) {
                            set_message(detail, sizeof(detail),
                                g_app.fanRuntimeConsecutiveFailures > 0
                                    ? "Failed to verify the initial fixed fan apply"
                                    : "Failed to start fixed fan maintenance");
                        }
                    } else {
                        ok = nvml_set_fan_manual(desired->fanPercent, &exact, detail, sizeof(detail));
                    }
                } else {
                    set_message(detail, sizeof(detail), "Requested %d%% is outside the supported range %u..%u%%",
                        desired->fanPercent, g_app.fanMinPct, g_app.fanMaxPct);
                }
                if (ok) {
                    g_app.activeFanMode = FAN_MODE_FIXED;
                    g_app.activeFanFixedPercent = clamp_percent(desired->fanPercent);
                }
            } else {
                copy_fan_curve(&g_app.activeFanCurve, &desiredCurve);
                if (g_app.hMainWnd) {
                    start_fan_curve_runtime();
                    ok = g_app.fanCurveRuntimeActive && g_app.fanRuntimeLastApplyTickMs != 0;
                    if (!ok) {
                        set_message(detail, sizeof(detail),
                            g_app.fanRuntimeConsecutiveFailures > 0
                                ? "Failed to verify the initial fan curve apply"
                                : "Failed to start fan curve maintenance");
                    }
                } else {
                    set_message(detail, sizeof(detail),
                        "Fan curve mode requires the resident Green Curve tray app. Start the program hidden to tray for long-running fan control.");
                }
            }
            if (ok) successCount++;
            else {
                failCount++;
                append_failure("Fan control change failed%s%s",
                    detail[0] ? ": " : "",
                    detail[0] ? detail : "");
            }
        }
    }

    char detail[128] = {};
    if (memApplied || powerChanged || fanChanged) {
        refresh_global_state(detail, sizeof(detail));
    } else if (!curveTouched) {
        detect_clock_offsets();
    }
    if (curveTouched) {
        detect_locked_tail_from_curve();
    }
    populate_global_controls();
    if (interactive) {
        populate_edits();
        invalidate_main_window();
    }

    if (successCount == 0 && failCount == 0) {
        set_message(result, resultSize, "No setting changes needed.");
    } else if (failCount == 0) {
        set_message(result, resultSize, "Applied %d setting changes successfully.", successCount);
    } else {
        char logErr[256] = {};
        bool logWritten = write_error_report_log("Setting apply reported one or more failures", failureDetails, logErr, sizeof(logErr));
        if (failureDetails[0]) {
            set_message(result, resultSize, "Applied %d OK, %d failed: %s%s%s",
                successCount,
                failCount,
                failureDetails,
                logWritten ? " See " : "",
                logWritten ? APP_LOG_FILE : "");
        } else {
            set_message(result, resultSize, "Applied %d OK, %d failed.%s%s",
                successCount,
                failCount,
                logWritten ? " See " : "",
                logWritten ? APP_LOG_FILE : "");
        }
        if (!logWritten && logErr[0]) {
            debug_log("failed to write error report: %s\n", logErr);
        }
    }
    return failCount == 0;
}

// ============================================================================
// NvAPI Interface
// ============================================================================

static void* nvapi_qi(unsigned int id) {
    typedef void* (*qi_func)(unsigned int);
    static qi_func qi = nullptr;
    if (!qi) {
        g_app.hNvApi = LoadLibraryA("nvapi64.dll");
        if (!g_app.hNvApi) {
            g_app.hNvApi = LoadLibraryA("nvapi.dll");
        }
        if (!g_app.hNvApi) return nullptr;
        qi = (qi_func)GetProcAddress(g_app.hNvApi, "nvapi_QueryInterface");
        if (!qi) return nullptr;
    }
    return qi(id);
}

static bool nvapi_init() {
    typedef int (*init_t)();
    auto init = (init_t)nvapi_qi(NVAPI_INIT_ID);
    if (!init) return false;
    return init() == 0;
}

static bool nvapi_enum_gpu() {
    typedef int (*enum_t)(GPU_HANDLE*, int*);
    auto enumGpu = (enum_t)nvapi_qi(NVAPI_ENUM_GPU_ID);
    if (!enumGpu) return false;
    int count = 0;
    GPU_HANDLE handles[64] = {};
    int ret = enumGpu(handles, &count);
    if (ret != 0 || count < 1) return false;
    g_app.gpuHandle = handles[0];
    return true;
}

static bool nvapi_get_name() {
    typedef int (*name_t)(GPU_HANDLE, char*);
    auto getName = (name_t)nvapi_qi(NVAPI_GET_NAME_ID);
    if (!getName) return false;
    return getName(g_app.gpuHandle, g_app.gpuName) == 0;
}

static bool nvapi_read_curve() {
    auto getStatus = (NvApiFunc)nvapi_qi(VF_GET_STATUS_ID);
    if (!getStatus) return false;

    unsigned char mask[32] = {};
    unsigned int numClocks = 15;
    nvapi_get_vf_info_cached(mask, &numClocks);

    unsigned char buf[VF_BUFFER_SIZE] = {};
    {
        const unsigned int version = (1u << 16) | VF_BUFFER_SIZE;
        memcpy(&buf[0], &version, sizeof(version));
    }
    memcpy(&buf[4], mask, 32);
    memcpy(&buf[0x24], &numClocks, sizeof(numClocks));

    int ret = getStatus(g_app.gpuHandle, buf);
    if (ret != 0) return false;

    g_app.numPopulated = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        unsigned int freq = 0, volt = 0;
        memcpy(&freq, &buf[VF_ENTRIES_OFFSET + i * VF_ENTRY_STRIDE], sizeof(freq));
        memcpy(&volt, &buf[VF_ENTRIES_OFFSET + i * VF_ENTRY_STRIDE + 4], sizeof(volt));
        g_app.curve[i].freq_kHz = freq;
        g_app.curve[i].volt_uV = volt;
        if (freq > 0) g_app.numPopulated++;
    }
    g_app.loaded = true;
    return true;
}

static void read_nvidia_smi_max_clocks() {
    // Read nvidia-smi VBIOS default max clocks once, cache in AppData
    if (g_app.smiClocksRead) return;
    g_app.smiClocksRead = true;
    
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    
    PROCESS_INFORMATION pi = {};
    WCHAR cmd[] = L"nvidia-smi -q -d CLOCK";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWrite);
        char buf[4096] = {};
        DWORD totalRead = 0;
        while (totalRead < sizeof(buf) - 1) {
            DWORD n = 0;
            if (!ReadFile(hRead, buf + totalRead, sizeof(buf) - 1 - totalRead, &n, nullptr) || n == 0) break;
            totalRead += n;
        }
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        
        bool inMaxSection = false;
        char* line = buf;
        while (line && *line) {
            char* nextLine = strchr(line, '\n');
            if (nextLine) { *nextLine = 0; nextLine++; }
            char* cr = strchr(line, '\r');
            if (cr) *cr = 0;
            while (*line == ' ' || *line == '\t') line++;
            
            if (strstr(line, "Max Clocks")) { inMaxSection = true; line = nextLine; continue; }
            if (inMaxSection && line[0] == '[') inMaxSection = false;
            
            if (inMaxSection) {
                char* vp = nullptr;
                if ((vp = strstr(line, "Graphics")) && (vp = strchr(vp, ':')))
                    g_app.smiGpuMaxMHz = (unsigned int)atoi(vp + 1);
                if ((vp = strstr(line, "Memory")) && (vp = strchr(vp, ':')))
                    g_app.smiMemMaxMHz = (unsigned int)atoi(vp + 1);
            }
            line = nextLine;
        }
    } else {
        CloseHandle(hWrite);
    }
    CloseHandle(hRead);
}

static int uniform_curve_offset_khz() {
    int values[VF_NUM_POINTS] = {};
    int counts[VF_NUM_POINTS] = {};
    int uniqueCount = 0;
    int populatedCount = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz == 0) continue;
        populatedCount++;
        int delta = g_app.freqOffsets[i];
        bool found = false;
        for (int j = 0; j < uniqueCount; j++) {
            if (values[j] == delta) {
                counts[j]++;
                found = true;
                break;
            }
        }
        if (!found && uniqueCount < VF_NUM_POINTS) {
            values[uniqueCount] = delta;
            counts[uniqueCount] = 1;
            uniqueCount++;
        }
    }
    if (populatedCount == 0) return 0;

    int bestValue = 0;
    int bestCount = 0;
    for (int i = 0; i < uniqueCount; i++) {
        if (counts[i] > bestCount || (counts[i] == bestCount && abs(values[i]) > abs(bestValue))) {
            bestValue = values[i];
            bestCount = counts[i];
        }
    }

    if (bestCount * 2 < populatedCount) return 0;
    return bestValue;
}

static void detect_clock_offsets() {
    // Detect global offsets from generic driver-visible sources.
    // GPU: use uniform VF control deltas, which reflect active global core offset.
    // Memory: prefer public Pstates20 memory clocks vs VBIOS max clocks.
    // This reflects the currently active offset and avoids stale NVML/Pstates delta fields
    // surviving after another tool resets memory to default.

    read_nvidia_smi_max_clocks();

    int gpuOffsetkHz = uniform_curve_offset_khz();
    if (gpuOffsetkHz != 0 || g_app.pstateGpuOffsetkHz != 0) {
        if (gpuOffsetkHz == 0) gpuOffsetkHz = g_app.pstateGpuOffsetkHz;
        g_app.gpuClockOffsetkHz = gpuOffsetkHz;
        if (!g_app.gpuOffsetRangeKnown) {
            int gpuOffsetMHz = gpuOffsetkHz / 1000;
            g_app.gpuClockOffsetMinMHz = gpuOffsetMHz;
            g_app.gpuClockOffsetMaxMHz = gpuOffsetMHz;
        }
    }

    if (g_app.pstateMemMaxMHz > 0 && g_app.smiMemMaxMHz > 0) {
        int memOffsetkHz = ((int)g_app.pstateMemMaxMHz - (int)g_app.smiMemMaxMHz) * 1000;
        g_app.memClockOffsetkHz = memOffsetkHz;
        if (!g_app.memOffsetRangeKnown && memOffsetkHz != 0) {
            int memOffsetMHz = mem_display_mhz_from_driver_khz(memOffsetkHz);
            g_app.memClockOffsetMinMHz = memOffsetMHz;
            g_app.memClockOffsetMaxMHz = memOffsetMHz;
        }
    }
}

static bool nvapi_read_offsets() {
    const unsigned int CTRL_SIZE = 0x2420;
    unsigned char buf[0x2420] = {};
    if (!nvapi_read_control_table(buf, CTRL_SIZE)) return false;

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        int delta = 0;
        memcpy(&delta, &buf[0x44 + i * 0x24 + 0x14], sizeof(delta));
        g_app.freqOffsets[i] = delta;
    }
    return true;
}

static bool nvapi_set_point(int pointIndex, int freqDelta_kHz) {
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return false;
    freqDelta_kHz = clamp_freq_delta_khz(freqDelta_kHz);

    auto func = (NvApiFunc)nvapi_qi(VF_SET_CONTROL_ID);
    if (!func) return false;

    const unsigned int CTRL_SIZE = 0x2420;
    unsigned char buf[0x2420] = {};
    if (!nvapi_read_control_table(buf, CTRL_SIZE)) return false;

    memset(&buf[4], 0, 32);
    buf[4 + pointIndex / 8] = (unsigned char)(1 << (pointIndex % 8));
    memcpy(&buf[0x44 + pointIndex * 0x24 + 0x14], &freqDelta_kHz, sizeof(freqDelta_kHz));

    int ret = func(g_app.gpuHandle, buf);
    debug_log("set_point idx=%d delta=%d ret=%d\n", pointIndex, freqDelta_kHz, ret);
    return ret == 0;
}

// Pstates20 struct size and version for Blackwell
// NVML-based OC/PL functions
static bool nvml_read_power_limit() {
    if (!nvml_ensure_ready()) return false;
    if (!g_nvml_api.getPowerLimit || !g_nvml_api.getPowerDefaultLimit) return false;

    unsigned int cur = 0, def = 0;
    if (g_nvml_api.getPowerLimit(g_app.nvmlDevice, &cur) != NVML_SUCCESS) return false;
    if (g_nvml_api.getPowerDefaultLimit(g_app.nvmlDevice, &def) != NVML_SUCCESS) def = cur;

    g_app.powerLimitCurrentmW = (int)cur;
    g_app.powerLimitDefaultmW = def > 0 ? (int)def : (int)cur;
    g_app.powerLimitMinmW = 0;
    g_app.powerLimitMaxmW = 0;

    if (g_nvml_api.getPowerConstraints) {
        unsigned int mn = 0, mx = 0;
        if (g_nvml_api.getPowerConstraints(g_app.nvmlDevice, &mn, &mx) == NVML_SUCCESS) {
            g_app.powerLimitMinmW = (int)mn;
            g_app.powerLimitMaxmW = (int)mx;
        }
    }

    if (g_app.powerLimitDefaultmW > 0)
        g_app.powerLimitPct = (g_app.powerLimitCurrentmW * 100 + g_app.powerLimitDefaultmW / 2) / g_app.powerLimitDefaultmW;
    else
        g_app.powerLimitPct = 100;

    if (g_app.powerLimitPct < 0) g_app.powerLimitPct = 0;

    return true;
}

static bool nvapi_read_pstates() {
    // Read clock data from public NvAPI Pstates20.
    auto func = (NvApiFunc)nvapi_qi(0x6FF81213u);
    g_app.pstateGpuOffsetkHz = 0;
    g_app.pstateMemOffsetkHz = 0;
    g_app.pstateGpuMaxMHz = 0;
    g_app.pstateMemMaxMHz = 0;
    if (!func) return false;

    nvapiPerfPstates20Info_t info = {};
    info.version = NVAPI_PERF_PSTATES20_INFO_VER3;
    int ret = func(g_app.gpuHandle, &info);
    if (ret != 0) {
        info = {};
        info.version = NVAPI_PERF_PSTATES20_INFO_VER2;
        ret = func(g_app.gpuHandle, &info);
    }
    if (ret != 0) return false;

    unsigned int numPstates = info.numPstates;
    if (numPstates > NVAPI_MAX_GPU_PSTATE20_PSTATES) numPstates = NVAPI_MAX_GPU_PSTATE20_PSTATES;
    unsigned int numClocks = info.numClocks;
    if (numClocks > NVAPI_MAX_GPU_PSTATE20_CLOCKS) numClocks = NVAPI_MAX_GPU_PSTATE20_CLOCKS;
    bool curveRangeAnyFound = false;
    bool curveRangeP0Found = false;
    int curveRangeAnyMinkHz = 0;
    int curveRangeAnyMaxkHz = 0;
    int curveRangeP0MinkHz = 0;
    int curveRangeP0MaxkHz = 0;

    auto update_curve_range = [](bool* found, int* minOut, int* maxOut, int minValue, int maxValue) {
        if (!found || !minOut || !maxOut) return;
        if (!*found) {
            *minOut = minValue;
            *maxOut = maxValue;
            *found = true;
            return;
        }
        if (minValue < *minOut) *minOut = minValue;
        if (maxValue > *maxOut) *maxOut = maxValue;
    };

    for (unsigned int pi = 0; pi < numPstates; pi++) {
        const nvapiPstate20Entry_t* pstate = &info.pstates[pi];
        for (unsigned int ci = 0; ci < numClocks; ci++) {
            const nvapiPstate20ClockEntry_t* clock = &pstate->clocks[ci];
            unsigned int maxFreq_kHz = 0;
            if (clock->typeId == NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_SINGLE) {
                maxFreq_kHz = clock->data.single.freq_kHz;
            } else if (clock->typeId == NVAPI_GPU_PERF_PSTATE20_CLOCK_TYPE_RANGE) {
                maxFreq_kHz = clock->data.range.maxFreq_kHz;
            }

            if (clock->domainId == NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS) {
                if (abs(clock->freqDelta_kHz.value) > abs(g_app.pstateGpuOffsetkHz)) {
                    g_app.pstateGpuOffsetkHz = clock->freqDelta_kHz.value;
                }
                unsigned int mhz = maxFreq_kHz / 1000;
                if (mhz > g_app.pstateGpuMaxMHz) g_app.pstateGpuMaxMHz = mhz;
                if (clock->bIsEditable) {
                    if (pstate->pstateId == NVML_PSTATE_0) {
                        update_curve_range(&curveRangeP0Found, &curveRangeP0MinkHz, &curveRangeP0MaxkHz,
                            clock->freqDelta_kHz.valueRange.min, clock->freqDelta_kHz.valueRange.max);
                    } else {
                        update_curve_range(&curveRangeAnyFound, &curveRangeAnyMinkHz, &curveRangeAnyMaxkHz,
                            clock->freqDelta_kHz.valueRange.min, clock->freqDelta_kHz.valueRange.max);
                    }
                }
            } else if (clock->domainId == NVAPI_GPU_PUBLIC_CLOCK_MEMORY) {
                int memOffsetkHz = clock->freqDelta_kHz.value;
                if (abs(memOffsetkHz) > abs(g_app.pstateMemOffsetkHz)) {
                    g_app.pstateMemOffsetkHz = memOffsetkHz;
                }
                unsigned int mhz = maxFreq_kHz / 1000;
                if (mhz > g_app.pstateMemMaxMHz) g_app.pstateMemMaxMHz = mhz;
            }
        }
    }

    if (curveRangeP0Found) {
        set_curve_offset_range_khz(curveRangeP0MinkHz, curveRangeP0MaxkHz);
    } else if (curveRangeAnyFound) {
        set_curve_offset_range_khz(curveRangeAnyMinkHz, curveRangeAnyMaxkHz);
    }

    return true;
}

static bool nvapi_set_gpu_offset(int offsetkHz) {
    int currentGlobalkHz = uniform_curve_offset_khz();
    if (currentGlobalkHz == offsetkHz) return true;

    debug_log("set_gpu_offset current=%d target=%d\n", currentGlobalkHz, offsetkHz);

    int targetOffsets[VF_NUM_POINTS] = {};
    bool pointMask[VF_NUM_POINTS] = {};
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g_app.curve[i].freq_kHz == 0) continue;
        targetOffsets[i] = clamp_freq_delta_khz(g_app.freqOffsets[i] - currentGlobalkHz + offsetkHz);
        pointMask[i] = true;
    }
    bool exactOk = apply_curve_offsets_verified(targetOffsets, pointMask, 2);
    int uniformkHz = uniform_curve_offset_khz();
    detect_clock_offsets();
    bool functionalOk = (uniformkHz == offsetkHz) || (g_app.gpuClockOffsetkHz == offsetkHz);
    debug_log("set_gpu_offset result exact=%d uniform=%d detected=%d\n",
        exactOk ? 1 : 0, uniformkHz, g_app.gpuClockOffsetkHz);
    return exactOk || functionalOk;
}

static bool nvapi_set_mem_offset(int offsetkHz) {
    if (g_app.memClockOffsetkHz == offsetkHz) return true;
    bool exact = false;
    char detail[128] = {};
    bool ok = nvml_set_clock_offset_domain(NVML_CLOCK_MEM, (offsetkHz / 1000) * 2, &exact, detail, sizeof(detail));
    if (!ok) return false;

    nvml_read_clock_offsets(detail, sizeof(detail));
    nvapi_read_pstates();
    detect_clock_offsets();
    return g_app.memClockOffsetkHz == offsetkHz;
}

static bool nvapi_set_power_limit(int pct) {
    if (pct < 50 || pct > 150) return false;
    if (g_app.powerLimitDefaultmW <= 0) return false;

    int watts = (g_app.powerLimitDefaultmW * pct + 50000) / 100000;
    if (watts < 1) return false;
    unsigned int targetmW = (unsigned int)watts * 1000u;

    if (g_app.powerLimitMinmW > 0 && targetmW < (unsigned int)g_app.powerLimitMinmW) return false;
    if (g_app.powerLimitMaxmW > 0 && targetmW > (unsigned int)g_app.powerLimitMaxmW) return false;

    if (nvml_ensure_ready() && g_nvml_api.setPowerLimit) {
        nvmlReturn_t r = g_nvml_api.setPowerLimit(g_app.nvmlDevice, targetmW);
        if (r == NVML_SUCCESS) {
            nvml_read_power_limit();
            return true;
        }
        debug_log("Power limit via NVML failed: %s\n", nvml_err_name(r));
    }

    char exePath[MAX_PATH] = {};
    DWORD pathLen = SearchPathA(nullptr, "nvidia-smi.exe", nullptr, ARRAY_COUNT(exePath), exePath, nullptr);
    if (pathLen == 0 || pathLen >= ARRAY_COUNT(exePath)) {
        StringCchCopyA(exePath, ARRAY_COUNT(exePath), "nvidia-smi.exe");
    }

    char cmdLine[256] = {};
    StringCchPrintfA(cmdLine, ARRAY_COUNT(cmdLine), "\"%s\" -pl %d", exePath, watts);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(nullptr, cmdLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        debug_log("Power limit via nvidia-smi failed to launch (error %lu)\n", GetLastError());
        return false;
    }

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode == 0) {
        nvml_read_power_limit();
    }
    else debug_log("Power limit via nvidia-smi failed with exit code %lu\n", exitCode);
    return exitCode == 0;
}

static void rebuild_visible_map() {
    g_app.numVisible = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        unsigned int freq_mhz = displayed_curve_mhz(g_app.curve[i].freq_kHz);
        unsigned int volt_mv = g_app.curve[i].volt_uV / 1000;
        if (volt_mv >= MIN_VISIBLE_VOLT_mV && freq_mhz >= MIN_VISIBLE_FREQ_MHz) {
            g_app.visibleMap[g_app.numVisible++] = i;
        }
    }
}

static bool restore_locked_tail_from_curve_index(int preferredCi) {
    if (preferredCi < 0 || preferredCi >= VF_NUM_POINTS) return false;
    if (g_app.numVisible < 2) return false;
    if (g_app.curve[preferredCi].freq_kHz == 0) return false;

    int preferredVi = -1;
    for (int vi = 0; vi < g_app.numVisible; vi++) {
        if (g_app.visibleMap[vi] == preferredCi) {
            preferredVi = vi;
            break;
        }
    }
    if (preferredVi < 0 || preferredVi >= g_app.numVisible - 1) return false;

    unsigned int lockFreqkHz = g_app.curve[preferredCi].freq_kHz;
    bool hasTail = false;
    for (int j = preferredVi + 1; j < g_app.numVisible; j++) {
        int cj = g_app.visibleMap[j];
        if (g_app.curve[cj].freq_kHz == 0) return false;
        hasTail = true;
        if (g_app.curve[cj].freq_kHz != lockFreqkHz) return false;
    }
    if (!hasTail) return false;

    g_app.lockedVi = preferredVi;
    g_app.lockedCi = preferredCi;
    g_app.lockedFreq = displayed_curve_mhz(lockFreqkHz);
    return true;
}

static void detect_locked_tail_from_curve() {
    int preferredCi = (g_app.lockedFreq > 0 && g_app.lockedCi >= 0 && g_app.lockedCi < VF_NUM_POINTS)
        ? g_app.lockedCi
        : -1;

    g_app.lockedVi = -1;
    g_app.lockedCi = -1;
    g_app.lockedFreq = 0;

    if (g_app.numVisible < 2) return;
    if (preferredCi >= 0 && restore_locked_tail_from_curve_index(preferredCi)) return;

    for (int vi = 0; vi < g_app.numVisible - 1; vi++) {
        int ci = g_app.visibleMap[vi];
        unsigned int lockFreqkHz = g_app.curve[ci].freq_kHz;
        if (lockFreqkHz == 0) continue;

        bool hasTail = false;
        bool allSame = true;
        for (int j = vi + 1; j < g_app.numVisible; j++) {
            int cj = g_app.visibleMap[j];
            unsigned int freqkHz = g_app.curve[cj].freq_kHz;
            if (freqkHz == 0) {
                allSame = false;
                break;
            }
            hasTail = true;
            if (freqkHz != lockFreqkHz) {
                allSame = false;
                break;
            }
        }

        if (hasTail && allSame) {
            g_app.lockedVi = vi;
            g_app.lockedCi = ci;
            g_app.lockedFreq = displayed_curve_mhz(lockFreqkHz);
            return;
        }
    }
}

