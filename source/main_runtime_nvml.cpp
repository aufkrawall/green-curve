// CLI parsing moved to its own shard; included here so definition order in
// the amalgamated translation unit is exactly what it was before the split.
#include "main_cli_options.cpp"
#include "xbar_telemetry.h"
#include "memory_offset_units.h"
#include "nvml_clock_observation.h"
#include "vf_point_mask_policy.h"

static bool nvml_resolve(void** out, const char* name) {
    if (!g_nvml) return false;
    *out = (void*)GetProcAddress(g_nvml, name);
    return *out != nullptr;
}

static bool file_is_regular_no_reparse_w(const WCHAR* path) {
    if (!path || !path[0]) return false;
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

static bool find_program_files_nvsmi_file_w(const WCHAR* fileName, WCHAR* out, size_t outCount) {
    if (!fileName || !out || outCount == 0) return false;
    out[0] = 0;

    PWSTR programFiles = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles)) && programFiles) {
        WCHAR candidate[MAX_PATH] = {};
        if (SUCCEEDED(StringCchPrintfW(candidate, ARRAY_COUNT(candidate),
                L"%ls\\NVIDIA Corporation\\NVSMI\\%ls", programFiles, fileName)) &&
            file_is_regular_no_reparse_w(candidate) &&
            SUCCEEDED(StringCchCopyW(out, outCount, candidate))) {
            CoTaskMemFree(programFiles);
            return true;
        }
        CoTaskMemFree(programFiles);
    }

    WCHAR fallback[MAX_PATH] = {};
    if (SUCCEEDED(StringCchPrintfW(fallback, ARRAY_COUNT(fallback),
            L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\%ls", fileName)) &&
        file_is_regular_no_reparse_w(fallback) &&
        SUCCEEDED(StringCchCopyW(out, outCount, fallback))) {
        return true;
    }
    return false;
}

static HMODULE load_system_library_a(const char* name) {
    if (!name || !name[0] || strchr(name, '\\') || strchr(name, '/')) return nullptr;
    char systemDir[MAX_PATH] = {};
    UINT systemLen = gc_GetSystemDirectoryUtf8(systemDir, ARRAY_COUNT(systemDir));
    if (systemLen == 0 || systemLen >= ARRAY_COUNT(systemDir)) return nullptr;
    char path[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(path, ARRAY_COUNT(path), "%s\\%s", systemDir, name))) return nullptr;
    return LoadLibraryA(path);
}

static bool find_trusted_nvidia_smi_path_w(WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    if (find_program_files_nvsmi_file_w(L"nvidia-smi.exe", out, outCount)) return true;

    WCHAR systemDir[MAX_PATH] = {};
    UINT systemLen = GetSystemDirectoryW(systemDir, ARRAY_COUNT(systemDir));
    if (systemLen == 0 || systemLen >= ARRAY_COUNT(systemDir)) return false;
    WCHAR candidate[MAX_PATH] = {};
    if (FAILED(StringCchPrintfW(candidate, ARRAY_COUNT(candidate), L"%ls\\nvidia-smi.exe", systemDir))) return false;
    if (!file_is_regular_no_reparse_w(candidate)) return false;
    return SUCCEEDED(StringCchCopyW(out, outCount, candidate));
}

static bool find_trusted_nvidia_smi_path_a(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    WCHAR pathW[MAX_PATH] = {};
    if (!find_trusted_nvidia_smi_path_w(pathW, ARRAY_COUNT(pathW))) return false;
    return copy_wide_to_ansi(pathW, out, (int)outSize);
}

static HMODULE load_trusted_nvml_library() {
    WCHAR nvmlPath[MAX_PATH] = {};
    if (find_program_files_nvsmi_file_w(L"nvml.dll", nvmlPath, ARRAY_COUNT(nvmlPath))) {
        HMODULE module = LoadLibraryW(nvmlPath);
        if (module) return module;
    }
    return load_system_library_a("nvml.dll");
}

static bool nvml_ensure_ready();
static bool nvml_read_power_limit();

static bool nvml_get_offset_range(unsigned int domain, int* minMHz, int* maxMHz, int* currentMHz,
    char* detail, size_t detailSize, gc_u32* surface = nullptr, bool* rangeKnown = nullptr) {
    if (surface) *surface = CLOCK_OFFSET_SURFACE_UNSPECIFIED;
    if (rangeKnown) *rangeKnown = false;
    if (!nvml_ensure_ready()) {
        set_message(detail, detailSize, "NVML not ready");
        return false;
    }
    const NvmlClockObservation read = nvml_clock_observe(g_nvml_api,
        g_app.nvmlDevice, domain, rangeKnown == nullptr);
    if (!read.control.valid || (!rangeKnown && !read.control.rangeValid)) {
        set_message(detail, detailSize, "Offset control/range unavailable (control=%d range=%d)",
            (int)read.control.status, (int)read.control.rangeStatus);
        return false;
    }
    if (surface) *surface = read.surface;
    if (rangeKnown) *rangeKnown = read.control.rangeValid;
    if (currentMHz) *currentMHz = read.control.value;
    if (read.control.rangeValid) {
        if (minMHz) *minMHz = read.control.minimum;
        if (maxMHz) *maxMHz = read.control.maximum;
    }
    if (read.surface == CLOCK_OFFSET_SURFACE_P0)
        g_app.offsetReadPstate = (int)nvml_configured_clock_offset_pstate();
    return true;
}

static bool nvml_read_clock_offsets(char* detail, size_t detailSize) {
    int mn = 0, mx = 0, cur = 0;
    bool gpuRangeKnown = false;
    bool gpuOk = nvml_get_offset_range(NVML_CLOCK_GRAPHICS, &mn, &mx, &cur, detail, detailSize, nullptr, &gpuRangeKnown);
    const long long gpuKHz = (long long)cur * 1000;
    gpuOk = gpuOk && gpuKHz >= INT_MIN && gpuKHz <= INT_MAX;
    if (gpuOk) {
        if (gpuRangeKnown) {
            g_app.gpuClockOffsetMinMHz = mn;
            g_app.gpuClockOffsetMaxMHz = mx;
            if (!g_app.curveOffsetRangeKnown && (long long)mn * 1000 >= INT_MIN &&
                (long long)mx * 1000 <= INT_MAX)
                set_curve_offset_range_khz((int)((long long)mn * 1000), (int)((long long)mx * 1000));
        }
        g_app.gpuClockOffsetkHz = (int)gpuKHz;
        g_app.gpuOffsetRangeKnown = gpuRangeKnown;
        g_app.readback.gpuOffset = true;
    } else {
        g_app.gpuOffsetRangeKnown = g_app.readback.gpuOffset = false;
    }

    g_app.memOffsetSurface = CLOCK_OFFSET_SURFACE_UNSPECIFIED;
    bool memRangeKnown = false;
    bool memOk = nvml_get_offset_range(NVML_CLOCK_MEM, &mn, &mx, &cur, detail, detailSize,
        &g_app.memOffsetSurface, &memRangeKnown);
    int physicalMemKHz = 0;
    if (memOk && memory_offset_khz_from_effective_mhz(cur, &physicalMemKHz)) {
        if (memRangeKnown) {
            g_app.memClockOffsetMinMHz = mem_display_mhz_from_driver_mhz(mn);
            g_app.memClockOffsetMaxMHz = mem_display_mhz_from_driver_mhz(mx);
        }
        g_app.memClockOffsetkHz = physicalMemKHz;
        g_app.memOffsetRangeKnown = memRangeKnown;
        g_app.readback.memOffset = true;
    } else {
        g_app.memOffsetSurface = CLOCK_OFFSET_SURFACE_UNSPECIFIED;
        g_app.memOffsetRangeKnown = g_app.readback.memOffset = false;
        if (memOk) set_message(detail, detailSize, "Memory offset exceeds the physical cache range");
        memOk = false;
    }

    return gpuOk || memOk;
}

static bool nvml_set_clock_offset_domain(unsigned int domain, int offsetMHz, bool* exactApplied, char* detail, size_t detailSize) {
    if (exactApplied) *exactApplied = false;
    if (!nvml_ensure_ready()) {
        set_message(detail, detailSize, "NVML not ready");
        return false;
    }

    int saneLimitMHz = (domain == NVML_CLOCK_MEM) ? 10000 : 5000;
    if (offsetMHz < -saneLimitMHz || offsetMHz > saneLimitMHz) {
        set_message(detail, detailSize, "Offset %d MHz exceeds the safe maximum of %d MHz", offsetMHz, saneLimitMHz);
        return false;
    }

    nvmlReturn_t r = NVML_ERROR_NOT_SUPPORTED;
    if (g_nvml_api.setClockOffsets) {
        nvmlClockOffset_t info = {};
        info.version = nvmlClockOffset_v1;
        info.type = domain;
        info.pstate = nvml_configured_clock_offset_pstate();
        info.clockOffsetMHz = offsetMHz;
        r = g_nvml_api.setClockOffsets(g_app.nvmlDevice, &info);
        debug_log("NVML offset write: domain=%u pstate=P%u requested=%d "
                  "result=%d\n", domain, info.pstate, offsetMHz, (int)r);
        if (r == NVML_SUCCESS) g_app.offsetReadPstate = (int)info.pstate;
    }

    if (r != NVML_SUCCESS) {
        if (domain == NVML_CLOCK_GRAPHICS && g_nvml_api.setGpcClkVfOffset) {
            r = g_nvml_api.setGpcClkVfOffset(g_app.nvmlDevice, offsetMHz);
        } else if (domain == NVML_CLOCK_MEM && g_nvml_api.setMemClkVfOffset) {
            r = g_nvml_api.setMemClkVfOffset(g_app.nvmlDevice, offsetMHz);
        }
        debug_log("NVML offset legacy write: domain=%u pstate=global "
                  "requested=%d result=%d\n", domain, offsetMHz, (int)r);
    }

    if (r != NVML_SUCCESS) {
        set_message(detail, detailSize, "%s", nvml_err_name(r));
        return false;
    }

    int mn = 0, mx = 0, cur = 0;
    bool readOk = nvml_get_offset_range(
        domain, &mn, &mx, &cur, detail, detailSize);
    if (!readOk) {
        set_message(detail, detailSize, "write OK, readback failed");
        return true;
    }
    bool matched = nvml_clock_offset_readback_matches(
        offsetMHz, cur, nvml_clock_offset_grid_step(domain));
    debug_log("NVML offset verify: domain=%u pstate=P%u requested=%d "
              "readback=%d matched=%d\n", domain,
              nvml_configured_clock_offset_pstate(), offsetMHz, cur,
              matched ? 1 : 0);
    if (exactApplied) *exactApplied = matched;
    return true;
}

// `notSupportedOut`, when supplied, separates "this GPU has no locked-clock
// control" from "the clamp was declined this time".  F-APPLY-CEILING needs the
// distinction: a declined clamp on hardware that can hold one refuses the
// transition, while hardware that has never had the control (the API is
// Volta-and-newer; Pascal answers NOT_SUPPORTED to every form) must keep
// working the way it always has.  See apply_clock_ceiling_policy.h.
static bool nvml_set_gpu_locked_clocks(unsigned int minMHz, unsigned int maxMHz, char* detail, size_t detailSize,
                                       bool* notSupportedOut = nullptr) {
    if (notSupportedOut) *notSupportedOut = false;
    if (!nvml_ensure_ready() || !g_nvml_api.setGpuLockedClocks) {
        set_message(detail, detailSize, "NVML not ready or setGpuLockedClocks unavailable");
        // A missing entry point is a fact about the driver; NVML merely not
        // coming up is an environment failure that a later attempt can fix, so
        // only the former may relax a required clamp.
        if (notSupportedOut && !g_nvml_api.setGpuLockedClocks)
            *notSupportedOut = true;
        return false;
    }
    nvmlReturn_t r = g_nvml_api.setGpuLockedClocks(g_app.nvmlDevice, minMHz, maxMHz);
    if (r != NVML_SUCCESS) {
        set_message(detail, detailSize, "nvmlDeviceSetGpuLockedClocks(%u, %u): %s", minMHz, maxMHz, nvml_err_name(r));
        if (notSupportedOut) *notSupportedOut = (r == NVML_ERROR_NOT_SUPPORTED);
        return false;
    }
    debug_log("nvml_set_gpu_locked_clocks: min=%u max=%u ok\n", minMHz, maxMHz);
    return true;
}

static bool nvml_reset_gpu_locked_clocks(char* detail, size_t detailSize) {
    if (!nvml_ensure_ready() || !g_nvml_api.resetGpuLockedClocks) {
        set_message(detail, detailSize, "NVML not ready or resetGpuLockedClocks unavailable");
        return false;
    }
    nvmlReturn_t r = g_nvml_api.resetGpuLockedClocks(g_app.nvmlDevice);
    if (r != NVML_SUCCESS) {
        set_message(detail, detailSize, "nvmlDeviceResetGpuLockedClocks: %s", nvml_err_name(r));
        return false;
    }
    g_app.transitionClockCapActive = false;
    debug_log("nvml_reset_gpu_locked_clocks: ok\n");
    return true;
}

// The per-fan read every fan path and the stall check rely on.
#include "main_runtime_nvml_fans.cpp"
#include "profile_explicit_state_policy.h"

static bool desired_settings_have_explicit_state(const DesiredSettings* desired, bool requireCurve, char* err, size_t errSize) {
    return profile_desired_has_explicit_state(desired, requireCurve, err, errSize);
}

static bool nvml_set_fan_manual(int pct, bool* exactApplied, char* detail, size_t detailSize, bool runtimeTick) {
    if (exactApplied) *exactApplied = false;
    if (!nvml_ensure_ready() || !g_app.fanSupported || !g_nvml_api.setFanSpeed) {
        set_message(detail, detailSize, "Fan manual unsupported");
        return false;
    }

    if (!runtimeTick || g_app.fanCount == 0) {
        unsigned int count = 0;
        if (!nvml_read_controllable_fan_count(&count, detail, detailSize)) return false;
        g_app.fanCount = count;
    }

    // Snapshot pre-write fan state for rollback.
    unsigned int previousPolicy[MAX_GPU_FANS] = {};
    for (unsigned int fan = 0; fan < g_app.fanCount && fan < MAX_GPU_FANS; fan++) {
        previousPolicy[fan] = g_app.fanPolicy[fan];
    }

    bool changedFan[MAX_GPU_FANS] = {};
    unsigned int rollbackFailures = 0;
    auto rollback_changed_fans = [&]() {
        char rollbackDetail[128] = {};
        for (unsigned int fan = 0; fan < g_app.fanCount && fan < MAX_GPU_FANS; fan++) {
            if (!changedFan[fan]) continue;
            if (g_nvml_api.setDefaultFanSpeed) {
                if (g_nvml_api.setDefaultFanSpeed(g_app.nvmlDevice, fan) != NVML_SUCCESS) {
                    rollbackFailures++;
                    debug_log("nvml_set_fan_manual rollback: setDefaultFanSpeed fan=%u failed\n", fan);
                }
            }
            if (g_nvml_api.setFanControlPolicy) {
                nvmlReturn_t pr = g_nvml_api.setFanControlPolicy(g_app.nvmlDevice, fan, NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW);
                if (pr != NVML_SUCCESS) {
                    rollbackFailures++;
                    debug_log("nvml_set_fan_manual rollback: setFanControlPolicy fan=%u failed: %s\n", fan, nvml_err_name(pr));
                }
            }
            g_app.fanPolicy[fan] = NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW;
        }
        nvml_read_fans(rollbackDetail, sizeof(rollbackDetail));
        debug_log("nvml_set_fan_manual: rolled back changed fans after partial failure (rollbackFailures=%u)%s%s\n",
            rollbackFailures,
            rollbackDetail[0] ? ": " : "",
            rollbackDetail[0] ? rollbackDetail : "");
    };
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        bool alreadyManual = (g_app.fanPolicy[fan] == NVML_FAN_POLICY_MANUAL);
        if (!alreadyManual && g_nvml_api.setFanControlPolicy) {
            char phase[128] = {};
            StringCchPrintfA(phase, ARRAY_COUNT(phase), "Fan manual policy write: fan=%u", fan);
            set_last_apply_phase(phase);
            nvmlReturn_t pr = g_nvml_api.setFanControlPolicy(g_app.nvmlDevice, fan, NVML_FAN_POLICY_MANUAL);
            if (pr == NVML_SUCCESS) {
                g_app.fanPolicy[fan] = NVML_FAN_POLICY_MANUAL;
            } else {
                debug_log("nvml_set_fan_manual: setFanControlPolicy fan=%u failed: %s\n", fan, nvml_err_name(pr));
            }
        } else if (alreadyManual && g_debug_log_verbose) {
            // A no-op confirmation on every curve step; the speed write below logs.
            debug_log("nvml_set_fan_manual: fan=%u already manual, skipping policy write\n", fan);
        }
        char phase[128] = {};
        StringCchPrintfA(phase, ARRAY_COUNT(phase), "Fan manual speed write: fan=%u pct=%d", fan, pct);
        // Runtime duty steps are summarized by log_fan_runtime_write().
        set_last_apply_phase(phase, !runtimeTick);
        nvmlReturn_t r = g_nvml_api.setFanSpeed(g_app.nvmlDevice, fan, (unsigned int)pct);
        if (r != NVML_SUCCESS) {
            set_message(detail, detailSize, "fan %u: %s", fan, nvml_err_name(r));
            rollback_changed_fans();
            return false;
        }
        if (fan < MAX_GPU_FANS) changedFan[fan] = true;
    }
    bool ok = false;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt > 0) Sleep(10);
        if (!nvml_read_fans(detail, detailSize)) continue;
        ok = (g_app.fanCount > 0);
        for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
            int got = (int)g_app.fanPercent[fan];
            if (!manual_fan_readback_matches_target(pct, got,
                    g_app.fanTargetPercent[fan], g_app.readback.fan.target[fan])) ok = false;
        }
        if (ok) break;
    }
    if (exactApplied) *exactApplied = ok;
    if (!ok && detail && detailSize > 0 && !detail[0]) {
        set_message(detail, detailSize, "Fan readback did not confirm %d%%", pct);
    }
    if (!ok) {
        rollback_changed_fans();
        return false;
    }
    return true;
}

static bool nvml_manual_fan_matches_target(int pct, bool* matches, char* detail, size_t detailSize) {
    if (matches) *matches = false;
    if (!nvml_read_fans(detail, detailSize)) return false;
    if (g_app.fanCount == 0) {
        set_message(detail, detailSize, "No fans detected");
        return false;
    }
    if (g_app.fanIsAuto) {
        set_message(detail, detailSize, "Driver fan policy reverted to auto");
        return true;
    }

    bool ok = true;
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        int got = (int)g_app.fanPercent[fan];
        if (!manual_fan_readback_matches_target(pct, got,
                g_app.fanTargetPercent[fan], g_app.readback.fan.target[fan])) ok = false;
    }
    if (!ok) {
        set_message(detail, detailSize, "Fan readback did not confirm %d%%", pct);
    }
    if (matches) *matches = ok;
    return true;
}

#include "main_runtime_nvml_selection.h"

static bool nvml_ensure_ready() {
    // If device was removed, don't try to use NVML — the internal handles
    // are stale and calling into nvml.dll will crash.  Wait for device
    // arrival to re-initialize.
    if (g_app.deviceRemoved) return false;

    // A healthy initialized handle remains usable. The VEH invalidates these
    // fields before publishing its crash cue.
    if (g_app.nvmlReady && g_app.nvmlDevice) return true;

    // After a VEH-handled crash, the old process only prepares a controlled
    // restart. Never load or probe NVML again in that process.
    if (g_nvmlVhCrashed || nvml_crash_recovery_active()) {
        g_app.nvmlReady = false;
        g_app.nvmlDevice = nullptr;
        return false;
    }
    if (!g_nvml) {
        g_nvml = load_trusted_nvml_library();
    }
    if (!g_nvml) return false;

    if (!g_nvml_api.init) {
        nvml_resolve((void**)&g_nvml_api.init, "nvmlInit_v2");
        nvml_resolve((void**)&g_nvml_api.shutdown, "nvmlShutdown");
        nvml_resolve((void**)&g_nvml_api.getHandleByIndex, "nvmlDeviceGetHandleByIndex_v2");
        nvml_resolve((void**)&g_nvml_api.getCount, "nvmlDeviceGetCount_v2");
        nvml_resolve_pci_info(nvml_resolve, (void**)&g_nvml_api.getPciInfo);
        nvml_resolve((void**)&g_nvml_api.getPowerLimit, "nvmlDeviceGetPowerManagementLimit");
        nvml_resolve((void**)&g_nvml_api.getPowerDefaultLimit, "nvmlDeviceGetPowerManagementDefaultLimit");
        nvml_resolve((void**)&g_nvml_api.getPowerConstraints, "nvmlDeviceGetPowerManagementLimitConstraints");
        nvml_resolve((void**)&g_nvml_api.setPowerLimit, "nvmlDeviceSetPowerManagementLimit");
        nvml_resolve((void**)&g_nvml_api.getClockOffsets, "nvmlDeviceGetClockOffsets");
        nvml_resolve((void**)&g_nvml_api.setClockOffsets, "nvmlDeviceSetClockOffsets");
        nvml_resolve((void**)&g_nvml_api.getPerformanceState, "nvmlDeviceGetPerformanceState");
        nvml_resolve((void**)&g_nvml_api.getGpcClkVfOffset, "nvmlDeviceGetGpcClkVfOffset");
        nvml_resolve((void**)&g_nvml_api.getMemClkVfOffset, "nvmlDeviceGetMemClkVfOffset");
        nvml_resolve((void**)&g_nvml_api.getGpcClkMinMaxVfOffset, "nvmlDeviceGetGpcClkMinMaxVfOffset");
        nvml_resolve((void**)&g_nvml_api.getMemClkMinMaxVfOffset, "nvmlDeviceGetMemClkMinMaxVfOffset");
        nvml_resolve((void**)&g_nvml_api.setGpcClkVfOffset, "nvmlDeviceSetGpcClkVfOffset");
        nvml_resolve((void**)&g_nvml_api.setMemClkVfOffset, "nvmlDeviceSetMemClkVfOffset");
        nvml_resolve((void**)&g_nvml_api.getNumFans, "nvmlDeviceGetNumFans");
        nvml_resolve((void**)&g_nvml_api.getMinMaxFanSpeed, "nvmlDeviceGetMinMaxFanSpeed");
        nvml_resolve((void**)&g_nvml_api.getFanControlPolicy, "nvmlDeviceGetFanControlPolicy_v2");
        nvml_resolve((void**)&g_nvml_api.setFanControlPolicy, "nvmlDeviceSetFanControlPolicy");
        nvml_resolve((void**)&g_nvml_api.getFanSpeed, "nvmlDeviceGetFanSpeed_v2");
        nvml_resolve((void**)&g_nvml_api.getTargetFanSpeed, "nvmlDeviceGetTargetFanSpeed");
        nvml_resolve((void**)&g_nvml_api.getFanSpeedRpm, "nvmlDeviceGetFanSpeedRPM");
        nvml_resolve((void**)&g_nvml_api.setFanSpeed, "nvmlDeviceSetFanSpeed_v2");
        nvml_resolve((void**)&g_nvml_api.setDefaultFanSpeed, "nvmlDeviceSetDefaultFanSpeed_v2");
        nvml_resolve((void**)&g_nvml_api.getCoolerInfo, "nvmlDeviceGetCoolerInfo");
        nvml_resolve((void**)&g_nvml_api.getTemperature, "nvmlDeviceGetTemperature");
        nvml_resolve((void**)&g_nvml_api.getClock, "nvmlDeviceGetClock");
        nvml_resolve((void**)&g_nvml_api.getMaxClock, "nvmlDeviceGetMaxClockInfo");
        // Read-only load witnesses for the apply-transition trace; both are
        // optional and a board without them simply logs the load as unknown.
        nvml_resolve((void**)&g_nvml_api.getUtilization, "nvmlDeviceGetUtilizationRates");
        nvml_resolve((void**)&g_nvml_api.getPowerUsage, "nvmlDeviceGetPowerUsage");
        nvml_resolve((void**)&g_nvml_api.setGpuLockedClocks, "nvmlDeviceSetGpuLockedClocks");
        nvml_resolve((void**)&g_nvml_api.resetGpuLockedClocks, "nvmlDeviceResetGpuLockedClocks");
        nvml_resolve((void**)&g_nvml_api.setMemoryLockedClocks, "nvmlDeviceSetMemoryLockedClocks");
        nvml_resolve((void**)&g_nvml_api.resetMemoryLockedClocks, "nvmlDeviceResetMemoryLockedClocks");
    }

    if (!g_nvml_api.init || !g_nvml_api.getHandleByIndex) {
        debug_log("nvml_ensure_ready: FAILED — missing core function pointers (init=%d getHandleByIndex=%d)\n",
            g_nvml_api.init ? 1 : 0,
            g_nvml_api.getHandleByIndex ? 1 : 0);
        return false;
    }

    // Log which NVML component function groups resolved.  This helps diagnose
    // driver-upgrade / device-reconnect issues: if fan or clock-offset
    // functions are null after a reload, the new nvml.dll may not support
    // the required APIs for the connected GPU.
    {
        int fanOk = 0;
        if (g_nvml_api.getNumFans) fanOk++;
        if (g_nvml_api.getFanSpeed) fanOk++;
        if (g_nvml_api.setFanSpeed) fanOk++;
        if (g_nvml_api.setDefaultFanSpeed) fanOk++;
        if (g_nvml_api.setFanControlPolicy) fanOk++;
        if (g_nvml_api.getFanControlPolicy) fanOk++;
        if (g_nvml_api.getTargetFanSpeed) fanOk++;
        int clockOk = 0;
        if (g_nvml_api.getClockOffsets) clockOk++;
        if (g_nvml_api.setClockOffsets) clockOk++;
        if (g_nvml_api.getGpcClkVfOffset) clockOk++;
        if (g_nvml_api.setGpcClkVfOffset) clockOk++;
        if (g_nvml_api.getMemClkVfOffset) clockOk++;
        if (g_nvml_api.setMemClkVfOffset) clockOk++;
        if (g_nvml_api.getPerformanceState) clockOk++;
        if (g_nvml_api.getClock) clockOk++;
        int powerOk = 0;
        if (g_nvml_api.getPowerLimit) powerOk++;
        if (g_nvml_api.setPowerLimit) powerOk++;
        if (g_nvml_api.getPowerConstraints) powerOk++;
        bool haveTemp = g_nvml_api.getTemperature != nullptr;
        debug_log("nvml: components resolved — fan=%d/8 clock=%d/8 power=%d/3 temperature=%d\n",
            fanOk, clockOk, powerOk, haveTemp ? 1 : 0);
    }

    // RC7: log the exact nvmlInit result so we can distinguish fresh first-
    // reference (NVML_SUCCESS==0) from second-reference (NVML_ERROR_ALREADY_
    // INITIALIZED==12).  Only the former allows full write paths (mem offset,
    // fan control) through the kernel driver.
    nvmlReturn_t r = g_nvml_api.init();
        debug_log("nvml_ensure_ready: nvmlInit returned %d", (int)r);
    if (r == NVML_SUCCESS) {
        debug_log(" (NVML_SUCCESS — fresh first-reference client)\n");
    } else if (r == NVML_ERROR_ALREADY_INITIALIZED) {
        debug_log(" (NVML_ERROR_ALREADY_INITIALIZED — second-reference, writes may be rejected)\n");
    } else {
        debug_log(" (unknown — treating as failure)\n");
        return false;
    }
    char selectDetail[256] = {};
    if (!nvml_select_device_for_selected_gpu(selectDetail, sizeof(selectDetail))) {
        debug_log("nvml_ensure_ready: selected GPU mapping failed: %s\n", selectDetail[0] ? selectDetail : "unknown error");
        return false;
    }
    g_app.nvmlReady = true;
    return true;
}

static bool refresh_global_state(char* detail, size_t detailSize) {
    if (!app_is_service_process()) {
        if (!g_app.backgroundServiceAvailable) {
            set_message(detail, detailSize,
                g_app.backgroundServiceInstalled
                    ? "Background service is not responding"
                    : "Background service is not installed");
            return false;
        }
        ServiceResponse stateResponse = {};
        if (!service_client_get_ready_state(&stateResponse, 2000, "global state refresh", detail, detailSize)) return false;
        apply_ready_service_envelope_to_app(&stateResponse);
        update_tray_icon();
        return stateResponse.snapshot.loaded;
    }
    bool ok1 = nvapi_read_pstates();
    bool ok2 = nvml_read_power_limit();
    bool ok3 = nvml_read_clock_offsets(detail, detailSize);
    bool ok4 = nvml_read_fans(detail, detailSize);
    bool okXbar = xbar_refresh_live_state();
    if (!ok3 && !ok1) ok1 = nvapi_read_pstates();
    detect_clock_offsets(g_app.readback.memOffset);
    detect_locked_tail_from_curve();
    (void)current_applied_gpu_offset_excludes_low_points();
    (void)current_applied_gpu_offset_mhz();
    initialize_gui_fan_settings_from_live_state();
    if (ok1 || ok2 || ok3 || ok4) {
        mark_service_telemetry_cache_updated("global refresh");
    }
    update_tray_icon();
    return ok1 || ok2 || ok3 || ok4 || okXbar;
}

#include "main_vf_info_cache.cpp"

// THE VF offset range for this GPU, plus the clamps derived from it.
#include "vf_offset_range_runtime.h"
