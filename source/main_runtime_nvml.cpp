static bool parse_cli_options(LPWSTR cmdLine, CliOptions* opts) {
    if (!opts) return false;
    memset(opts, 0, sizeof(*opts));
    initialize_desired_settings_defaults(&opts->desired);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
    if (!argv) return false;

    for (int i = 1; i < argc; i++) {
        LPWSTR arg = argv[i];
        if (!arg) continue;
        if (wcscmp(arg, L"--help") == 0 || wcscmp(arg, L"-h") == 0) {
            opts->recognized = true;
            opts->showHelp = true;
        } else if (wcscmp(arg, L"--dump") == 0) {
            opts->recognized = true;
            opts->dump = true;
        } else if (wcscmp(arg, L"--json") == 0) {
            opts->recognized = true;
            opts->json = true;
        } else if (wcscmp(arg, L"--probe") == 0) {
            opts->recognized = true;
            opts->probe = true;
        } else if (wcscmp(arg, L"--reset") == 0) {
            opts->recognized = true;
            opts->reset = true;
        } else if (wcscmp(arg, L"--save-config") == 0) {
            opts->recognized = true;
            opts->saveConfig = true;
        } else if (wcscmp(arg, L"--apply-config") == 0) {
            opts->recognized = true;
            opts->applyConfig = true;
        } else if (wcscmp(arg, L"--service-install") == 0) {
            opts->recognized = true;
            opts->serviceInstall = true;
        } else if (wcscmp(arg, L"--service-remove") == 0) {
            opts->recognized = true;
            opts->serviceRemove = true;
        } else if (wcscmp(arg, L"--startup-task-enable") == 0) {
            opts->recognized = true;
            opts->startupTaskEnable = true;
        } else if (wcscmp(arg, L"--startup-task-disable") == 0) {
            opts->recognized = true;
            opts->startupTaskDisable = true;
        } else if (wcscmp(arg, L"--logon-start") == 0) {
            opts->recognized = true;
            opts->logonStart = true;
        } else if (wcscmp(arg, L"--config") == 0) {
            opts->recognized = true;
            if (i + 1 >= argc || !copy_wide_to_utf8(argv[++i], opts->configPath, MAX_PATH)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --config path");
                LocalFree(argv);
                return false;
            }
            opts->hasConfigPath = true;
        } else if (wcscmp(arg, L"--probe-output") == 0) {
            opts->recognized = true;
            if (i + 1 >= argc || !copy_wide_to_utf8(argv[++i], opts->probeOutputPath, MAX_PATH)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --probe-output path");
                LocalFree(argv);
                return false;
            }
            opts->hasProbeOutputPath = true;
        } else if (wcscmp(arg, L"--gpu-offset") == 0) {
            opts->recognized = true;
            int v = 0;
            if (i + 1 >= argc || !parse_wide_int_arg(argv[++i], &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --gpu-offset value");
                LocalFree(argv);
                return false;
            }
            opts->desired.hasGpuOffset = true;
            opts->desired.gpuOffsetMHz = v;
        } else if (wcscmp(arg, L"--mem-offset") == 0) {
            opts->recognized = true;
            int v = 0;
            if (i + 1 >= argc || !parse_wide_int_arg(argv[++i], &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --mem-offset value");
                LocalFree(argv);
                return false;
            }
            opts->desired.hasMemOffset = true;
            opts->desired.memOffsetMHz = v;
        } else if (wcscmp(arg, L"--power-limit") == 0) {
            opts->recognized = true;
            int v = 0;
            if (i + 1 >= argc || !parse_wide_int_arg(argv[++i], &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --power-limit value");
                LocalFree(argv);
                return false;
            }
            opts->desired.hasPowerLimit = true;
            if (v < 50 || v > 150) {
                set_message(opts->error, sizeof(opts->error), "power_limit_pct %d is outside the safe range 50..150", v);
                LocalFree(argv);
                return false;
            }
            opts->desired.powerLimitPct = v;
        } else if (wcscmp(arg, L"--fan") == 0) {
            opts->recognized = true;
            char buf[64] = {};
            if (i + 1 >= argc || !copy_wide_to_utf8(argv[++i], buf, sizeof(buf)) ||
                !parse_fan_value(buf, &opts->desired.fanAuto, &opts->desired.fanPercent)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan value, use auto or 0-100");
                LocalFree(argv);
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = opts->desired.fanAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
        } else if (wcsncmp(arg, L"--point", 7) == 0) {
            opts->recognized = true;
            int idx = -1;
            int v = 0;
            if (!parse_cli_point_arg_w(arg, &idx) || i + 1 >= argc || !parse_wide_int_arg(argv[++i], &v) || v <= 0) {
                set_message(opts->error, sizeof(opts->error), "Invalid --pointN value");
                LocalFree(argv);
                return false;
            }
            opts->desired.hasCurvePoint[idx] = true;
            opts->desired.curvePointMHz[idx] = (unsigned int)v;
        }
    }

    LocalFree(argv);
    return true;
}

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
    UINT systemLen = GetSystemDirectoryA(systemDir, ARRAY_COUNT(systemDir));
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

static bool nvml_get_offset_range(unsigned int domain, int* minMHz, int* maxMHz, int* currentMHz, char* detail, size_t detailSize) {
    if (!nvml_ensure_ready()) {
        set_message(detail, detailSize, "NVML not ready");
        return false;
    }

    bool ok = false;
    if (g_nvml_api.getClockOffsets && g_nvml_api.getPerformanceState) {
        unsigned int pstate = NVML_PSTATE_UNKNOWN;
        if (g_nvml_api.getPerformanceState(g_app.nvmlDevice, &pstate) == NVML_SUCCESS) {
            nvmlClockOffset_t info = {};
            info.version = nvmlClockOffset_v1;
            info.type = domain;
            info.pstate = pstate;
            nvmlReturn_t r = g_nvml_api.getClockOffsets(g_app.nvmlDevice, &info);
            if (r == NVML_SUCCESS) {
                if (minMHz) *minMHz = info.minClockOffsetMHz;
                if (maxMHz) *maxMHz = info.maxClockOffsetMHz;
                if (currentMHz) *currentMHz = info.clockOffsetMHz;
                g_app.offsetReadPstate = (int)pstate;
                ok = true;
            }
        }
    }

    if (!ok) {
        int mn = 0, mx = 0, cur = 0;
        nvmlReturn_t r1 = NVML_ERROR_NOT_SUPPORTED;
        nvmlReturn_t r2 = NVML_ERROR_NOT_SUPPORTED;
        if (domain == NVML_CLOCK_GRAPHICS) {
            if (g_nvml_api.getGpcClkMinMaxVfOffset) r1 = g_nvml_api.getGpcClkMinMaxVfOffset(g_app.nvmlDevice, &mn, &mx);
            if (g_nvml_api.getGpcClkVfOffset) r2 = g_nvml_api.getGpcClkVfOffset(g_app.nvmlDevice, &cur);
        } else if (domain == NVML_CLOCK_MEM) {
            if (g_nvml_api.getMemClkMinMaxVfOffset) r1 = g_nvml_api.getMemClkMinMaxVfOffset(g_app.nvmlDevice, &mn, &mx);
            if (g_nvml_api.getMemClkVfOffset) r2 = g_nvml_api.getMemClkVfOffset(g_app.nvmlDevice, &cur);
        }
        if (r1 == NVML_SUCCESS || r2 == NVML_SUCCESS) {
            if (minMHz) *minMHz = mn;
            if (maxMHz) *maxMHz = mx;
            if (currentMHz) *currentMHz = cur;
            ok = true;
        } else {
            set_message(detail, detailSize, "%s / %s", nvml_err_name(r1), nvml_err_name(r2));
        }
    }

    return ok;
}

static bool nvml_read_clock_offsets(char* detail, size_t detailSize) {
    int mn = 0, mx = 0, cur = 0;
    bool gpuOk = nvml_get_offset_range(NVML_CLOCK_GRAPHICS, &mn, &mx, &cur, detail, detailSize);
    if (gpuOk) {
        g_app.gpuClockOffsetMinMHz = mn;
        g_app.gpuClockOffsetMaxMHz = mx;
        g_app.gpuClockOffsetkHz = cur * 1000;
        g_app.gpuOffsetRangeKnown = true;
        if (!g_app.curveOffsetRangeKnown) {
            set_curve_offset_range_khz(mn * 1000, mx * 1000);
        }
    } else {
        g_app.gpuOffsetRangeKnown = false;
    }

    bool memOk = nvml_get_offset_range(NVML_CLOCK_MEM, &mn, &mx, &cur, detail, detailSize);
    if (memOk) {
        g_app.memClockOffsetMinMHz = mem_display_mhz_from_driver_mhz(mn);
        g_app.memClockOffsetMaxMHz = mem_display_mhz_from_driver_mhz(mx);
        g_app.memClockOffsetkHz = (cur * 1000) / 2;
        g_app.memOffsetRangeKnown = true;
    } else {
        g_app.memOffsetRangeKnown = false;
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
    if (g_nvml_api.setClockOffsets && g_nvml_api.getPerformanceState) {
        unsigned int statesToTry[2] = { NVML_PSTATE_UNKNOWN, NVML_PSTATE_0 };
        if (g_nvml_api.getPerformanceState(g_app.nvmlDevice, &statesToTry[0]) != NVML_SUCCESS) {
            statesToTry[0] = NVML_PSTATE_0;
        }
        for (int si = 0; si < 2 && r != NVML_SUCCESS; si++) {
            unsigned int pstate = statesToTry[si];
            if (pstate == NVML_PSTATE_UNKNOWN) continue;
            nvmlClockOffset_t info = {};
            info.version = nvmlClockOffset_v1;
            info.type = domain;
            info.pstate = pstate;
            info.clockOffsetMHz = offsetMHz;
            r = g_nvml_api.setClockOffsets(g_app.nvmlDevice, &info);
            if (r == NVML_SUCCESS) g_app.offsetReadPstate = (int)pstate;
        }
    }

    if (r != NVML_SUCCESS) {
        if (domain == NVML_CLOCK_GRAPHICS && g_nvml_api.setGpcClkVfOffset) {
            r = g_nvml_api.setGpcClkVfOffset(g_app.nvmlDevice, offsetMHz);
        } else if (domain == NVML_CLOCK_MEM && g_nvml_api.setMemClkVfOffset) {
            r = g_nvml_api.setMemClkVfOffset(g_app.nvmlDevice, offsetMHz);
        }
    }

    if (r != NVML_SUCCESS) {
        set_message(detail, detailSize, "%s", nvml_err_name(r));
        return false;
    }

    int mn = 0, mx = 0, cur = 0;
    bool readOk = false;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt > 0) Sleep(10);
        if (!nvml_get_offset_range(domain, &mn, &mx, &cur, detail, detailSize)) continue;
        readOk = true;
        if (cur == offsetMHz) break;
    }
    if (!readOk) {
        set_message(detail, detailSize, "write OK, readback failed");
        return true;
    }
    if (exactApplied) *exactApplied = (cur == offsetMHz);
    return true;
}

static bool nvml_read_fans(char* detail, size_t detailSize) {
    if (!nvml_ensure_ready()) {
        set_message(detail, detailSize, "NVML not ready");
        return false;
    }

    memset(g_app.fanPercent, 0, sizeof(g_app.fanPercent));
    memset(g_app.fanTargetPercent, 0, sizeof(g_app.fanTargetPercent));
    memset(g_app.fanRpm, 0, sizeof(g_app.fanRpm));
    memset(g_app.fanPolicy, 0, sizeof(g_app.fanPolicy));
    memset(g_app.fanControlSignal, 0, sizeof(g_app.fanControlSignal));
    memset(g_app.fanTargetMask, 0, sizeof(g_app.fanTargetMask));
    g_app.fanCount = 0;
    g_app.fanMinPct = 0;
    g_app.fanMaxPct = 100;
    g_app.fanRangeKnown = false;
    g_app.fanIsAuto = true;

    if (!g_nvml_api.getNumFans) {
        set_message(detail, detailSize, "nvmlDeviceGetNumFans missing");
        return false;
    }

    unsigned int count = 0;
    nvmlReturn_t r = g_nvml_api.getNumFans(g_app.nvmlDevice, &count);
    if (r != NVML_SUCCESS || count == 0) {
        set_message(detail, detailSize, "%s", nvml_err_name(r));
        return false;
    }

    g_app.fanSupported = true;
    g_app.fanCount = count > MAX_GPU_FANS ? MAX_GPU_FANS : count;

    if (g_nvml_api.getMinMaxFanSpeed) {
        unsigned int mn = 0, mx = 0;
        if (g_nvml_api.getMinMaxFanSpeed(g_app.nvmlDevice, &mn, &mx) == NVML_SUCCESS) {
            g_app.fanMinPct = mn;
            g_app.fanMaxPct = mx;
            g_app.fanRangeKnown = true;
        }
    }

    bool allAuto = true;
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        bool policyKnown = false;
        if (g_nvml_api.getFanControlPolicy) {
            unsigned int pol = 0;
            if (g_nvml_api.getFanControlPolicy(g_app.nvmlDevice, fan, &pol) == NVML_SUCCESS) {
                g_app.fanPolicy[fan] = pol;
                policyKnown = true;
                if (pol != NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) allAuto = false;
            }
        }
        bool isAutoForFan = true;
        if (policyKnown) {
            unsigned int pol = g_app.fanPolicy[fan];
            isAutoForFan = (pol == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW);
        }
        if (g_nvml_api.getFanSpeed) {
            unsigned int pct = 0;
            if (g_nvml_api.getFanSpeed(g_app.nvmlDevice, fan, &pct) == NVML_SUCCESS) {
                g_app.fanPercent[fan] = pct;
            }
        }
        bool shouldReadTarget = !policyKnown
            ? (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive || g_app.activeFanMode != FAN_MODE_AUTO)
            : !isAutoForFan;
        if (g_nvml_api.getTargetFanSpeed && shouldReadTarget) {
            unsigned int target = 0;
            if (g_nvml_api.getTargetFanSpeed(g_app.nvmlDevice, fan, &target) == NVML_SUCCESS) {
                g_app.fanTargetPercent[fan] = target;
                if (!policyKnown && target > 0) allAuto = false;
            }
        }
        if (g_nvml_api.getFanSpeedRpm) {
            nvmlFanSpeedInfo_t info = {};
            info.version = nvmlFanSpeedInfo_v1;
            info.fan = fan;
            if (g_nvml_api.getFanSpeedRpm(g_app.nvmlDevice, &info) == NVML_SUCCESS) {
                g_app.fanRpm[fan] = info.speed;
            }
        }
        if (g_nvml_api.getCoolerInfo) {
            nvmlCoolerInfo_t info = {};
            info.version = nvmlCoolerInfo_v1;
            info.index = fan;
            if (g_nvml_api.getCoolerInfo(g_app.nvmlDevice, &info) == NVML_SUCCESS) {
                g_app.fanControlSignal[fan] = info.signalType;
                g_app.fanTargetMask[fan] = info.target;
            }
        }
    }
    g_app.fanIsAuto = allAuto;
    return true;
}

static bool nvml_set_fan_auto(char* detail, size_t detailSize) {
    if (!nvml_ensure_ready() || !g_app.fanSupported || (!g_nvml_api.setDefaultFanSpeed && !g_nvml_api.setFanControlPolicy)) {
        set_message(detail, detailSize, "Fan auto unsupported");
        return false;
    }
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        bool changed = false;
        if (g_nvml_api.setDefaultFanSpeed) {
            char phase[128] = {};
            StringCchPrintfA(phase, ARRAY_COUNT(phase), "Fan auto default write: fan=%u", fan);
            set_last_apply_phase(phase);
            nvmlReturn_t r = g_nvml_api.setDefaultFanSpeed(g_app.nvmlDevice, fan);
            if (r != NVML_SUCCESS) {
                set_message(detail, detailSize, "fan %u: %s", fan, nvml_err_name(r));
                return false;
            }
            changed = true;
        }
        if (g_nvml_api.setFanControlPolicy) {
            char phase[128] = {};
            StringCchPrintfA(phase, ARRAY_COUNT(phase), "Fan auto policy write: fan=%u", fan);
            set_last_apply_phase(phase);
            nvmlReturn_t r = g_nvml_api.setFanControlPolicy(g_app.nvmlDevice, fan, NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW);
            if (r == NVML_SUCCESS) {
                changed = true;
            } else if (!g_nvml_api.setDefaultFanSpeed) {
                set_message(detail, detailSize, "fan %u: %s", fan, nvml_err_name(r));
                return false;
            }
        }
        if (!changed) {
            set_message(detail, detailSize, "Fan auto unsupported");
            return false;
        }
    }
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt > 0) Sleep(10);
        if (nvml_read_fans(detail, detailSize) && g_app.fanIsAuto) return true;
    }
    if (!nvml_read_fans(detail, detailSize)) return false;
    if (!g_app.fanIsAuto) {
        set_message(detail, detailSize, "Fan readback did not confirm driver auto mode");
        return false;
    }
    return true;
}

static bool desired_settings_have_explicit_state(const DesiredSettings* desired, bool requireCurve, char* err, size_t errSize) {
    if (!desired) {
        set_message(err, errSize, "No desired settings");
        return false;
    }

    if (!desired->hasGpuOffset) {
        set_message(err, errSize, "Profile is missing gpu_offset_mhz");
        return false;
    }
    if (!desired->hasMemOffset) {
        set_message(err, errSize, "Profile is missing mem_offset_mhz");
        return false;
    }
    if (!desired->hasPowerLimit) {
        set_message(err, errSize, "Profile is missing power_limit_pct");
        return false;
    }
    if (!desired->hasFan) {
        set_message(err, errSize, "Profile is missing fan_mode/fan settings");
        return false;
    }

    if (requireCurve) {
        bool haveCurvePoint = false;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (desired->hasCurvePoint[i]) {
                haveCurvePoint = true;
                break;
            }
        }
        if (!haveCurvePoint) {
            set_message(err, errSize, "Profile is missing VF curve points");
            return false;
        }
    }

    if (err && errSize > 0) err[0] = 0;
    return true;
}

static bool nvml_set_fan_manual(int pct, bool* exactApplied, char* detail, size_t detailSize) {
    if (exactApplied) *exactApplied = false;
    if (!nvml_ensure_ready() || !g_app.fanSupported || !g_nvml_api.setFanSpeed) {
        set_message(detail, detailSize, "Fan manual unsupported");
        return false;
    }
    bool changedFan[MAX_GPU_FANS] = {};
    auto rollback_changed_fans = [&]() {
        char rollbackDetail[128] = {};
        for (unsigned int fan = 0; fan < g_app.fanCount && fan < MAX_GPU_FANS; fan++) {
            if (!changedFan[fan]) continue;
            if (g_nvml_api.setDefaultFanSpeed) {
                g_nvml_api.setDefaultFanSpeed(g_app.nvmlDevice, fan);
            }
            if (g_nvml_api.setFanControlPolicy) {
                g_nvml_api.setFanControlPolicy(g_app.nvmlDevice, fan, NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW);
            }
            g_app.fanPolicy[fan] = NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW;
        }
        nvml_read_fans(rollbackDetail, sizeof(rollbackDetail));
        debug_log("nvml_set_fan_manual: rolled back changed fans after partial failure%s%s\n",
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
                alreadyManual = true;
            } else {
                debug_log("nvml_set_fan_manual: setFanControlPolicy fan=%u failed: %s\n", fan, nvml_err_name(pr));
            }
        } else if (alreadyManual) {
            debug_log("nvml_set_fan_manual: fan=%u already manual, skipping policy write\n", fan);
        }
        char phase[128] = {};
        StringCchPrintfA(phase, ARRAY_COUNT(phase), "Fan manual speed write: fan=%u pct=%d", fan, pct);
        set_last_apply_phase(phase);
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
            if (!manual_fan_readback_matches_target(pct, got, g_app.fanTargetPercent[fan])) ok = false;
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
        if (!manual_fan_readback_matches_target(pct, got, g_app.fanTargetPercent[fan])) ok = false;
    }
    if (!ok) {
        set_message(detail, detailSize, "Fan readback did not confirm %d%%", pct);
    }
    if (matches) *matches = ok;
    return true;
}

static bool fan_setting_matches_current(int wantMode, int wantPct, const FanCurveConfig* wantCurve) {
    if (!g_app.fanSupported) return false;
    if (wantMode != g_app.activeFanMode) return false;
    if (wantMode == FAN_MODE_AUTO) return g_app.fanIsAuto;
    if (wantMode == FAN_MODE_CURVE) {
        return !g_app.fanIsAuto && wantCurve && fan_curve_equals(wantCurve, &g_app.activeFanCurve);
    }
    if (g_app.fanIsAuto || g_app.fanCount == 0) return false;
    for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
        int gotPct = (int)g_app.fanPercent[fan];
        if (!manual_fan_readback_matches_target(wantPct, gotPct, g_app.fanTargetPercent[fan])) return false;
    }
    return true;
}

static bool nvml_pci_device_matches_nvapi(unsigned int nvapiDeviceId, unsigned int nvmlDeviceId) {
    if (!nvapiDeviceId || !nvmlDeviceId) return false;
    if (nvapiDeviceId == nvmlDeviceId) return true;
    if ((nvmlDeviceId & 0xFFFFu) == (nvapiDeviceId & 0xFFFFu)) return true;
    if (((nvmlDeviceId >> 16) & 0xFFFFu) == (nvapiDeviceId & 0xFFFFu)) return true;
    return false;
}

static bool nvml_pci_matches_selected_gpu(const nvmlPciInfo_t* pci) {
    if (!pci || !g_app.selectedGpu.valid || !g_app.selectedGpu.pciInfoValid) return false;
    if (!nvml_pci_device_matches_nvapi(g_app.selectedGpu.deviceId, pci->pciDeviceId)) return false;
    if (g_app.selectedGpu.subSystemId && pci->pciSubSystemId &&
        g_app.selectedGpu.subSystemId != pci->pciSubSystemId) return false;
    return true;
}

static bool nvml_select_device_for_selected_gpu(char* detail, size_t detailSize) {
    unsigned int count = 0;
    bool haveCount = g_nvml_api.getCount && g_nvml_api.getCount(&count) == NVML_SUCCESS && count > 0;
    if (!haveCount) count = g_app.adapterCount > 0 ? g_app.adapterCount : 1;
    if (g_nvml_api.getPciInfo && g_app.selectedGpu.valid && g_app.selectedGpu.pciInfoValid) {
        int matched = -1;
        for (unsigned int i = 0; i < count && i < 64; i++) {
            nvmlDevice_t candidate = nullptr;
            if (g_nvml_api.getHandleByIndex(i, &candidate) != NVML_SUCCESS || !candidate) continue;
            nvmlPciInfo_t pci = {};
            if (g_nvml_api.getPciInfo(candidate, &pci) != NVML_SUCCESS) continue;
            if (!nvml_pci_matches_selected_gpu(&pci)) continue;
            if (matched >= 0) {
                set_message(detail, detailSize, "Multiple NVML devices match selected GPU PCI identity");
                return false;
            }
            matched = (int)i;
            g_app.nvmlDevice = candidate;
            g_app.selectedNvmlIndex = i;
            g_app.selectedGpu.pciDomain = pci.domain;
            g_app.selectedGpu.pciBus = pci.bus;
            g_app.selectedGpu.pciDevice = pci.device;
        }
        if (matched >= 0) {
            g_app.selectedGpuOrdinalFallback = false;
            debug_log("nvml_select_device: matched NVAPI index %u to NVML index %u by PCI identity\n", g_app.selectedGpuIndex, g_app.selectedNvmlIndex);
            return true;
        }
        if (count > 1) {
            set_message(detail, detailSize, "No NVML device matched selected GPU PCI identity");
            return false;
        }
    }
    unsigned int fallback = g_app.selectedGpuIndex;
    if (fallback >= count) fallback = 0;
    nvmlReturn_t r = g_nvml_api.getHandleByIndex(fallback, &g_app.nvmlDevice);
    if (r != NVML_SUCCESS) {
        set_message(detail, detailSize, "NVML device index %u failed: %s", fallback, nvml_err_name(r));
        return false;
    }
    g_app.selectedNvmlIndex = fallback;
    g_app.selectedGpuOrdinalFallback = count > 1;
    if (g_app.selectedGpuOrdinalFallback) {
        debug_log("nvml_select_device: WARNING using ordinal fallback index %u on %u-device system\n", fallback, count);
    }
    return true;
}

static bool nvml_ensure_ready() {
    if (g_app.nvmlReady && g_app.nvmlDevice) return true;
    if (!g_nvml) {
        g_nvml = load_trusted_nvml_library();
    }
    if (!g_nvml) return false;

    if (!g_nvml_api.init) {
        nvml_resolve((void**)&g_nvml_api.init, "nvmlInit_v2");
        nvml_resolve((void**)&g_nvml_api.shutdown, "nvmlShutdown");
        nvml_resolve((void**)&g_nvml_api.getHandleByIndex, "nvmlDeviceGetHandleByIndex_v2");
        nvml_resolve((void**)&g_nvml_api.getCount, "nvmlDeviceGetCount_v2");
        nvml_resolve((void**)&g_nvml_api.getPciInfo, "nvmlDeviceGetPciInfo");
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
        nvml_resolve((void**)&g_nvml_api.getMaxClock, "nvmlDeviceGetMaxClock");
    }

    if (!g_nvml_api.init || !g_nvml_api.getHandleByIndex) return false;
    nvmlReturn_t r = g_nvml_api.init();
    if (r != NVML_SUCCESS && r != NVML_ERROR_ALREADY_INITIALIZED) return false;
    char selectDetail[256] = {};
    if (!nvml_select_device_for_selected_gpu(selectDetail, sizeof(selectDetail))) {
        debug_log("nvml_ensure_ready: selected GPU mapping failed: %s\n", selectDetail[0] ? selectDetail : "unknown error");
        return false;
    }
    g_app.nvmlReady = true;
    return true;
}

static bool refresh_global_state(char* detail, size_t detailSize) {
    if (!g_app.isServiceProcess) {
        if (!g_app.backgroundServiceAvailable) {
            set_message(detail, detailSize,
                g_app.backgroundServiceInstalled
                    ? "Background service is not responding"
                    : "Background service is not installed");
            return false;
        }
        ServiceSnapshot snapshot = {};
        if (!service_client_get_snapshot(&snapshot, detail, detailSize)) return false;
        apply_service_snapshot_to_app(&snapshot);
        update_tray_icon();
        return snapshot.loaded;
    }
    bool ok1 = nvapi_read_pstates();
    bool ok2 = nvml_read_power_limit();
    bool ok3 = nvml_read_clock_offsets(detail, detailSize);
    bool ok4 = nvml_read_fans(detail, detailSize);
    if (!ok3 && !ok1) ok1 = nvapi_read_pstates();
    detect_clock_offsets();
    detect_locked_tail_from_curve();
    (void)current_applied_gpu_offset_excludes_low_points();
    (void)current_applied_gpu_offset_mhz();
    initialize_gui_fan_settings_from_live_state();
    if (ok1 || ok2 || ok3 || ok4) {
        mark_service_telemetry_cache_updated("global refresh");
    }
    update_tray_icon();
    return ok1 || ok2 || ok3 || ok4;
}

static bool nvapi_get_vf_info_cached(unsigned char* maskOut, unsigned int* numClocksOut) {
    const VfBackendSpec* backend = g_app.vfBackend;
    if (!backend) return false;

    if (!g_app.vfInfoCached) {
        memset(g_app.vfMask, 0, sizeof(g_app.vfMask));
        memset(g_app.vfMask, 0xFF, 16);
        g_app.vfNumClocks = backend->defaultNumClocks;

        auto getInfo = (NvApiFunc)nvapi_qi(backend->getInfoId);
        if (getInfo) {
            unsigned char ibuf[0x4000] = {};
            if (backend->infoBufferSize > sizeof(ibuf)) return false;
            const unsigned int version = (backend->infoVersion << 16) | backend->infoBufferSize;
            memcpy(&ibuf[0], &version, sizeof(version));
            if (backend->infoMaskOffset + sizeof(g_app.vfMask) <= backend->infoBufferSize) {
                memset(&ibuf[backend->infoMaskOffset], 0xFF, sizeof(g_app.vfMask));
            }
            if (getInfo(g_app.gpuHandle, ibuf) == 0) {
                if (backend->infoMaskOffset + sizeof(g_app.vfMask) <= backend->infoBufferSize) {
                    memcpy(g_app.vfMask, &ibuf[backend->infoMaskOffset], sizeof(g_app.vfMask));
                }
                if (backend->infoNumClocksOffset + sizeof(g_app.vfNumClocks) <= backend->infoBufferSize) {
                    memcpy(&g_app.vfNumClocks, &ibuf[backend->infoNumClocksOffset], sizeof(g_app.vfNumClocks));
                }
                if (g_app.vfNumClocks == 0) g_app.vfNumClocks = backend->defaultNumClocks;
            }
        }
        g_app.vfInfoCached = true;
    }

    if (maskOut) memcpy(maskOut, g_app.vfMask, sizeof(g_app.vfMask));
    if (numClocksOut) *numClocksOut = g_app.vfNumClocks ? g_app.vfNumClocks : backend->defaultNumClocks;
    return true;
}

static int clamp_freq_delta_khz(int freqDelta_kHz) {
    int minkHz = 0;
    int maxkHz = 0;
    get_curve_offset_range_khz(&minkHz, &maxkHz);
    if (freqDelta_kHz > maxkHz) return maxkHz;
    if (freqDelta_kHz < minkHz) return minkHz;
    return freqDelta_kHz;
}

static void set_curve_offset_range_khz(int minkHz, int maxkHz) {
    if (minkHz > maxkHz) return;
    g_app.curveOffsetMinkHz = minkHz;
    g_app.curveOffsetMaxkHz = maxkHz;
    g_app.curveOffsetRangeKnown = true;
}

static bool get_curve_offset_range_khz(int* minkHz, int* maxkHz) {
    const int FALLBACK_VF_OFFSET_LIMIT_KHZ = 500000; // 500 MHz
    int minValue = -FALLBACK_VF_OFFSET_LIMIT_KHZ;
    int maxValue = FALLBACK_VF_OFFSET_LIMIT_KHZ;
    bool known = false;

    if (g_app.curveOffsetRangeKnown && g_app.curveOffsetMinkHz <= g_app.curveOffsetMaxkHz) {
        minValue = g_app.curveOffsetMinkHz;
        maxValue = g_app.curveOffsetMaxkHz;
        known = true;
    } else if (g_app.gpuOffsetRangeKnown && g_app.gpuClockOffsetMinMHz <= g_app.gpuClockOffsetMaxMHz) {
        minValue = g_app.gpuClockOffsetMinMHz * 1000;
        maxValue = g_app.gpuClockOffsetMaxMHz * 1000;
        known = true;
    }

    if (minkHz) *minkHz = minValue;
    if (maxkHz) *maxkHz = maxValue;
    return known;
}

