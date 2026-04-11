// ============================================================================
// Entry Point
// ============================================================================

static void dump_curve_to_file(const char* path) {
    if (!g_debug_logging) return;
    char err[256] = {};
    if (!write_log_snapshot(path, err, sizeof(err))) {
        debug_log("Failed to write log snapshot: %s\n", err);
    }
}

// CLI mode: --dump, --json, or --probe
// Returns true if CLI handled (should exit), false if should run GUI
static bool handle_cli(LPWSTR wCmdLine) {
    CliOptions opts = {};
    if (!parse_cli_options(wCmdLine, &opts)) {
        char err[256] = {};
        const char* text = opts.error[0] ? opts.error : "Failed to parse CLI";
        write_text_file_atomic(APP_CLI_LOG_FILE, text, strlen(text), err, sizeof(err));
        return true;
    }
    if (opts.logonStart) {
        set_default_config_path();
        if (opts.hasConfigPath) StringCchCopyA(g_app.configPath, ARRAY_COUNT(g_app.configPath), opts.configPath);
        g_app.launchedFromLogon = true;
        g_app.startHiddenToTray = true;
        return false;
    }
    if (!opts.recognized) return false;
    set_default_config_path();
    if (opts.hasConfigPath) StringCchCopyA(g_app.configPath, ARRAY_COUNT(g_app.configPath), opts.configPath);

    // Elevate if needed (VF curve read requires admin)
    if (!is_elevated()) {
        WCHAR path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = path;
        sei.lpParameters = wCmdLine;  // pass through all args
        sei.nShow = SW_HIDE;  // no window flash
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, INFINITE);
            CloseHandle(sei.hProcess);
        }
        return true;
    }

    // CLI always writes to file since we're a GUI subsystem app
    const char* logPath = APP_CLI_LOG_FILE;
    FILE* logf = fopen(logPath, "w");
    if (!logf) return false;

    #define CLI_LOG(...) do { fprintf(logf, __VA_ARGS__); fflush(logf); } while(0)

    CLI_LOG("Green Curve CLI mode started\n");

    if (opts.showHelp) {
        CLI_LOG(APP_NAME " v" APP_VERSION " - NVIDIA VF Curve Editor\n");
        CLI_LOG("Usage:\n");
        CLI_LOG("  greencurve.exe              Launch GUI\n");
        CLI_LOG("  greencurve.exe --dump       Write VF curve to greencurve_cli_log.txt\n");
        CLI_LOG("  greencurve.exe --json       Write VF curve to greencurve_curve.json\n");
        CLI_LOG("  greencurve.exe --probe [--probe-output <path>]  Probe NvAPI/NVML/VF support and write a report\n");
        CLI_LOG("  greencurve.exe --gpu-offset <mhz> --mem-offset <mhz> --power-limit <pct>\n");
        CLI_LOG("  greencurve.exe --fan <auto|0-100> --point49 <mhz> ... --point126 <mhz>\n");
        CLI_LOG("  greencurve.exe --apply-config [--config <path>]  Apply logon profile slot\n");
        CLI_LOG("  greencurve.exe --save-config [--config <path>]  Save to selected profile slot\n");
        CLI_LOG("  greencurve.exe --reset      Reset curve/global controls to defaults\n");
        CLI_LOG("  greencurve.exe --help       This help\n");
        fclose(logf);
        return true;
    }

    CLI_LOG("Green Curve: Initializing NvAPI...\n");

    if (!nvapi_init()) {
        CLI_LOG("ERROR: Failed to initialize NvAPI.\n");
        fclose(logf);
        return true;
    }
    CLI_LOG("Green Curve: NvAPI initialized.\n");

    if (!nvapi_enum_gpu()) {
        CLI_LOG("ERROR: No NVIDIA GPU found.\n");
        fclose(logf);
        return true;
    }
    CLI_LOG("Green Curve: GPU enumerated.\n");

    nvapi_get_name();
    CLI_LOG("Green Curve: GPU name: %s\n", g_app.gpuName);
    nvapi_read_gpu_metadata();
    CLI_LOG("Green Curve: GPU family: %s\n", gpu_family_name(g_app.gpuFamily));
    if (g_app.vfBackend) {
        CLI_LOG("Green Curve: Selected VF backend: %s (%s)\n",
            g_app.vfBackend->name,
            !g_app.vfBackend->supported ? "probe-only" :
            vf_backend_is_best_guess(g_app.vfBackend) ? "best-guess" : "supported");
    } else {
        CLI_LOG("Green Curve: Selected VF backend: none\n");
    }

    bool curveOk = false;
    bool offsetsOk = false;

    if (g_app.vfBackend && g_app.vfBackend->readSupported) {
        CLI_LOG("Green Curve: Reading VF curve...\n");
        curveOk = nvapi_read_curve();
        if (!curveOk) {
            CLI_LOG("ERROR: Failed to read VF curve.\n");
            if (!opts.probe) {
                fclose(logf);
                return true;
            }
        } else {
            CLI_LOG("Green Curve: VF curve read OK - %d populated points.\n", g_app.numPopulated);
        }

        CLI_LOG("Green Curve: Reading VF offsets...\n");
        offsetsOk = nvapi_read_offsets();
        if (!offsetsOk) {
            CLI_LOG("WARNING: Failed to read VF offsets (non-fatal).\n");
        } else {
            CLI_LOG("Green Curve: VF offsets read OK.\n");
        }
    } else {
        CLI_LOG("Green Curve: Skipping live VF read because the selected backend is not yet supported.\n");
    }

    bool needsCurveWriteSupport =
        opts.reset ||
        opts.saveConfig ||
        opts.applyConfig ||
        desired_has_any_action(&opts.desired);
    if (needsCurveWriteSupport && (!g_app.vfBackend || !g_app.vfBackend->writeSupported || !curveOk)) {
        CLI_LOG("ERROR: Live VF write operations are not available on this GPU yet.\n");
        CLI_LOG("Run --probe [--probe-output <path>] and share the JSON report so support can be added.\n");
        if (!opts.probe) {
            fclose(logf);
            return true;
        }
    }

    // Read global OC/PL values
    {
        char detail[128] = {};
        refresh_global_state(detail, sizeof(detail));
    }

    if (opts.applyConfig) {
        DesiredSettings cfg = {};
        char err[256] = {};
        // Determine which profile slot to apply
        int logonSlot = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);
        if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;
        int loadSlot = (logonSlot > 0) ? logonSlot : CONFIG_DEFAULT_SLOT;
        if (is_profile_slot_saved(g_app.configPath, loadSlot)) {
            if (!load_profile_from_config(g_app.configPath, loadSlot, &cfg, err, sizeof(err))) {
                CLI_LOG("ERROR: %s\n", err);
                fclose(logf);
                return true;
            }
            CLI_LOG("Applying profile %d...\n", loadSlot);
        } else {
            // Fallback to legacy config
            if (!load_desired_settings_from_ini(g_app.configPath, &cfg, err, sizeof(err))) {
                CLI_LOG("ERROR: %s\n", err);
                fclose(logf);
                return true;
            }
        }
        merge_desired_settings(&cfg, &opts.desired);
        if (cfg.hasFan && cfg.fanMode != FAN_MODE_AUTO) {
            cfg.fanMode = FAN_MODE_AUTO;
            cfg.fanAuto = true;
            cfg.fanPercent = 0;
            fan_curve_set_default(&cfg.fanCurve);
            CLI_LOG("Note: silent one-shot logon apply leaves fan control on driver auto when the tray app is not running.\n");
        }
        char result[512] = {};
        bool ok = apply_desired_settings(&cfg, false, result, sizeof(result));
        CLI_LOG("%s\n", result);
        if (!ok) {
            fclose(logf);
            return true;
        }
    } else if (desired_has_any_action(&opts.desired)) {
        char result[512] = {};
        bool ok = apply_desired_settings(&opts.desired, false, result, sizeof(result));
        CLI_LOG("%s\n", result);
        if (!ok) {
            fclose(logf);
            return true;
        }
    }

    if (opts.reset) {
        int resetOffsets[VF_NUM_POINTS] = {};
        bool resetMask[VF_NUM_POINTS] = {};
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (g_app.curve[ci].freq_kHz == 0) continue;
            resetMask[ci] = true;
        }
        apply_curve_offsets_verified(resetOffsets, resetMask, 2);
        if (g_app.gpuClockOffsetkHz != 0) nvapi_set_gpu_offset(0);
        if (g_app.memClockOffsetkHz != 0) nvapi_set_mem_offset(0);
        if (g_app.powerLimitPct != 100) nvapi_set_power_limit(100);
        char detail[128] = {};
        if (!g_app.fanIsAuto) nvml_set_fan_auto(detail, sizeof(detail));
        refresh_global_state(detail, sizeof(detail));
        CLI_LOG("Reset applied.\n");
    }

    if (opts.saveConfig) {
        DesiredSettings saveDesired = {};
        bool useDesired = false;
        int targetSlot = get_config_int(g_app.configPath, "profiles", "selected_slot", CONFIG_DEFAULT_SLOT);
        if (targetSlot < 1 || targetSlot > CONFIG_NUM_SLOTS) targetSlot = CONFIG_DEFAULT_SLOT;
        if (opts.applyConfig) {
            if (!load_desired_settings_from_ini(g_app.configPath, &saveDesired, opts.error, sizeof(opts.error))) {
                CLI_LOG("ERROR: %s\n", opts.error);
                fclose(logf);
                return true;
            }
            merge_desired_settings(&saveDesired, &opts.desired);
            useDesired = true;
        } else if (desired_has_any_action(&opts.desired)) {
            saveDesired = opts.desired;
            useDesired = true;
        }
        char err[256] = {};
        if (!useDesired) {
            initialize_desired_settings_defaults(&saveDesired);
            saveDesired.hasGpuOffset = true;
            saveDesired.gpuOffsetMHz = current_applied_gpu_offset_mhz();
            saveDesired.gpuOffsetExcludeLow70 = current_applied_gpu_offset_excludes_low_points();
            saveDesired.hasMemOffset = true;
            saveDesired.memOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
            saveDesired.hasPowerLimit = true;
            saveDesired.powerLimitPct = g_app.powerLimitPct;
            saveDesired.hasFan = true;
            saveDesired.fanMode = g_app.activeFanMode;
            saveDesired.fanAuto = g_app.activeFanMode == FAN_MODE_AUTO;
            saveDesired.fanPercent = g_app.activeFanFixedPercent;
            copy_fan_curve(&saveDesired.fanCurve, &g_app.activeFanCurve);
            for (int i = 0; i < VF_NUM_POINTS; i++) {
                if (g_app.curve[i].freq_kHz > 0) {
                    saveDesired.hasCurvePoint[i] = true;
                    saveDesired.curvePointMHz[i] = displayed_curve_mhz(g_app.curve[i].freq_kHz);
                }
            }
        }
        if (!save_profile_to_config(g_app.configPath, targetSlot, &saveDesired, err, sizeof(err))) {
            CLI_LOG("ERROR: %s\n", err);
            fclose(logf);
            return true;
        }
        CLI_LOG("Profile %d written to %s\n", targetSlot, g_app.configPath);
    }

    if (opts.dump) {
        if (!curveOk) {
            CLI_LOG("ERROR: VF curve dump unavailable because live VF read did not succeed.\n");
        } else {
        CLI_LOG("\n--- VF Curve Table ---\n");
        CLI_LOG("GPU offset: %d MHz\n", g_app.gpuClockOffsetkHz / 1000);
        CLI_LOG("Mem offset: %d MHz\n", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
        CLI_LOG("Power limit: %d%%\n", g_app.powerLimitPct);
        if (g_app.fanSupported) {
            CLI_LOG("Fan mode: %s\n", fan_mode_label(g_app.activeFanMode));
            for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
                CLI_LOG("  Fan %u: %u%% / %u RPM / policy=%u\n", fan, g_app.fanPercent[fan], g_app.fanRpm[fan], g_app.fanPolicy[fan]);
            }
            if (g_app.activeFanMode == FAN_MODE_CURVE) {
                char summary[96] = {};
                fan_curve_format_summary(&g_app.activeFanCurve, summary, sizeof(summary));
                CLI_LOG("  Curve: %s\n", summary);
            }
        } else {
            CLI_LOG("Fan mode: unsupported\n");
        }
        CLI_LOG("\n");
        CLI_LOG("%-6s  %-10s  %-10s  %-12s\n", "Point", "Freq(MHz)", "Volt(mV)", "Offset(kHz)");
        CLI_LOG("------  ----------  ----------  ------------\n");
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (g_app.curve[i].freq_kHz > 0 || g_app.curve[i].volt_uV > 0) {
                CLI_LOG("%-6d  %-10u  %-10u  %-12d\n",
                    i,
                    displayed_curve_mhz(g_app.curve[i].freq_kHz),
                    g_app.curve[i].volt_uV / 1000,
                    g_app.freqOffsets[i]);
            }
        }
        }
    }

    if (opts.probe) {
        CLI_LOG("\n=== NvAPI Probe: OC/PL Functions ===\n\n");

        // Helper: try a function with given ID, size, and version
        auto probe_func = [&](unsigned int id, const char* name, unsigned int structSize, unsigned int version) {
            auto func = (NvApiFunc)nvapi_qi(id);
            if (!func) {
                CLI_LOG("[%08X] %-40s  NOT FOUND\n", id, name);
                return -99;
            }
            unsigned char buf[0x4000] = {};
            const unsigned int header = (version << 16) | structSize;
            memcpy(&buf[0], &header, sizeof(header));
            if (structSize > 0x44) memset(&buf[4], 0xFF, 32);
            int ret = func(g_app.gpuHandle, buf);
            if (ret == 0) {
                CLI_LOG("[%08X] %-40s  OK (ver=0x%04X size=0x%04X)\n", id, name, version, structSize);
                // Dump first 64 bytes
                CLI_LOG("  Data: ");
                for (int i = 0; i < 64; i++) CLI_LOG("%02X ", buf[i]);
                CLI_LOG("\n");
            } else {
                CLI_LOG("[%08X] %-40s  ERR %d (0x%08X) (ver=%u size=0x%X)\n",
                    id, name, ret, (unsigned int)ret, version, structSize);
            }
            return ret;
        };

        // --- Power Policies ---
        CLI_LOG("--- Power Limit ---\n");
        // GetPowerPoliciesInfo (0x34206D86)
        probe_func(0x34206D86u, "PowerPoliciesGetInfo", 0x28, 1);
        // GetPowerPoliciesStatus (0x355C8B8C)
        probe_func(0x355C8B8C, "PowerPoliciesGetStatus", 0x50, 1);
        // SetPowerPoliciesStatus (0xAD95F5ED) - just probe get, don't set
        auto setPL = (NvApiFunc)nvapi_qi(0xAD95F5ED);
        CLI_LOG("[%08X] %-40s  %s\n", 0xAD95F5EDu, "PowerPoliciesSetStatus",
            setPL ? "FOUND" : "NOT FOUND");

        // --- Pstates (Global Clock Offset) ---
        CLI_LOG("\n--- Pstates (Global Clock Offset) ---\n");
        // Try Pstates20 get with various sizes
        const unsigned int psSizes[] = {0x0008, 0x0018, 0x0048, 0x00B0, 0x01C8, 0x0410,
                                         0x0840, 0x1098, 0x1C94, 0x2420, 0x3000};
        for (unsigned int sz : psSizes) {
            int r = probe_func(0x6FF81213u, "GPU_GetPstates20", sz, 2);
            if (r == 0) break;
            r = probe_func(0x6FF81213u, "GPU_GetPstates20", sz, 3);
            if (r == 0) break;
        }

        // Dump Pstates20 offset fields at known and nearby offsets
        {
            auto psFunc = (NvApiFunc)nvapi_qi(0x6FF81213u);
            if (psFunc) {
                unsigned char buf[0x1CF8] = {};
                const unsigned int version = (2u << 16) | 0x1CF8;
                memcpy(&buf[0], &version, sizeof(version));
                if (psFunc(g_app.gpuHandle, buf) == 0) {
                    CLI_LOG("Pstates20 v2 offset field scan (size 0x1CF8):\n");
                    for (unsigned int off = 0x30; off <= 0x60; off += 4) {
                        int val = 0;
                        memcpy(&val, &buf[off], 4);
                        CLI_LOG("  [0x%03X] = %d kHz%s\n", off, val,
                            (off == 0x3C) ? " <-- known GPU offset" :
                            (off == 0x54) ? " <-- known Mem offset" : "");
                    }
                    // Also try v3
                    memset(buf, 0, sizeof(buf));
                    const unsigned int v3ver = (3u << 16) | 0x1CF8;
                    memcpy(&buf[0], &v3ver, sizeof(v3ver));
                    if (psFunc(g_app.gpuHandle, buf) == 0) {
                        CLI_LOG("Pstates20 v3 offset field scan (size 0x1CF8):\n");
                        for (unsigned int off = 0x30; off <= 0x60; off += 4) {
                            int val = 0;
                            memcpy(&val, &buf[off], 4);
                            CLI_LOG("  [0x%03X] = %d kHz\n", off, val);
                        }
                    }
                }
            }
        }

        // Try Pstates20 set existence
        auto setPS = (NvApiFunc)nvapi_qi(0x0F4DAE6B);
        CLI_LOG("[%08X] %-40s  %s\n", 0x0F4DAE6Bu, "GPU_SetPstates20",
            setPS ? "FOUND" : "NOT FOUND");

        // Try GetPstatesInfoEx (0x6048B02F)
        for (unsigned int sz : psSizes) {
            int r = probe_func(0x6048B02Fu, "GPU_GetPstatesInfoEx", sz, 1);
            if (r == 0) break;
        }

        // --- VF Probe ---
        CLI_LOG("\n--- VF Probe ---\n");
        const VfBackendSpec* backend = probe_backend_for_current_gpu();
        auto getStatus = (NvApiFunc)nvapi_qi(backend->getStatusId);
        if (getStatus) {
            unsigned char probeMask[32] = {};
            unsigned int probeNumClocks = backend->defaultNumClocks;
            bool haveSeed = nvapi_get_vf_info_cached(probeMask, &probeNumClocks);
            if (!haveSeed) memset(probeMask, 0xFF, sizeof(probeMask));

            CLI_LOG("VF seed mask: ");
            for (unsigned int i = 0; i < sizeof(probeMask); i++) {
                CLI_LOG("%02X%s", probeMask[i], (i + 1 < sizeof(probeMask)) ? " " : "");
            }
            CLI_LOG("\n");
            CLI_LOG("VF seed num clocks: %u\n", probeNumClocks);

            unsigned char buf[0x4000] = {};
            if (backend->statusBufferSize <= sizeof(buf)) {
                const unsigned int version = (backend->statusVersion << 16) | backend->statusBufferSize;
                memcpy(&buf[0], &version, sizeof(version));
                if (backend->statusMaskOffset + sizeof(probeMask) <= backend->statusBufferSize) {
                    memcpy(&buf[backend->statusMaskOffset], probeMask, sizeof(probeMask));
                }
                if (backend->statusNumClocksOffset + sizeof(probeNumClocks) <= backend->statusBufferSize) {
                    memcpy(&buf[backend->statusNumClocksOffset], &probeNumClocks, sizeof(probeNumClocks));
                }
                int ret = getStatus(g_app.gpuHandle, buf);
                if (ret == 0) {
                    int populated = 0;
                    unsigned int f0 = 0, v0 = 0;
                    unsigned int fMax = 0;
                    for (int i = 0; i < VF_NUM_POINTS; i++) {
                        unsigned int freq = 0, volt = 0;
                        unsigned int entryOffset = backend->statusEntriesOffset + (unsigned int)i * backend->statusEntryStride;
                        if (entryOffset + 8 > backend->statusBufferSize) break;
                        memcpy(&freq, &buf[entryOffset], sizeof(freq));
                        memcpy(&volt, &buf[entryOffset + 4], sizeof(volt));
                        if (freq > 0) {
                            populated++;
                            if (f0 == 0) { f0 = freq; v0 = volt; }
                            if (freq > fMax) fMax = freq;
                        }
                    }
                    CLI_LOG("VF GetStatus cached seed: OK, %d pts, first=%u kHz/%u uV, max=%u kHz\n",
                        populated, f0, v0, fMax);
                } else {
                    CLI_LOG("VF GetStatus cached seed: ERR %d (0x%08X)\n", ret, (unsigned int)ret);
                }
            } else {
                CLI_LOG("VF GetStatus cached seed: skipped, buffer too large (%u)\n", backend->statusBufferSize);
            }
        }

        // Try common VF info function
        probe_func(0x507B4B59u, "VF_GetInfo (already known)", 0x182C, 1);

        char detail[128] = {};
        CLI_LOG("\n--- NVML Global Controls ---\n");
        if (nvml_read_clock_offsets(detail, sizeof(detail))) {
            CLI_LOG("Graphics offset: %d MHz (range %d..%d, pstate %d)\n",
                g_app.gpuClockOffsetkHz / 1000, g_app.gpuClockOffsetMinMHz, g_app.gpuClockOffsetMaxMHz, g_app.offsetReadPstate);
            CLI_LOG("Memory offset: %d MHz (range %d..%d)\n",
                mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz), g_app.memClockOffsetMinMHz, g_app.memClockOffsetMaxMHz);
        } else {
            CLI_LOG("Clock offsets: %s\n", detail);
        }

        // nvmlDeviceGetClock diagnostics - try ALL clock IDs
        CLI_LOG("\n--- NVML Effective Clocks ---\n");
        if (g_nvml_api.getClock) {
            unsigned int val = 0;
            if (g_nvml_api.getClock(g_app.nvmlDevice, NVML_CLOCK_GRAPHICS, NVML_CLOCK_ID_CURRENT, &val) == NVML_SUCCESS)
                CLI_LOG("GPU current clock: %u MHz\n", val);
            if (g_nvml_api.getClock(g_app.nvmlDevice, NVML_CLOCK_MEM, NVML_CLOCK_ID_CURRENT, &val) == NVML_SUCCESS)
                CLI_LOG("Mem current clock: %u MHz\n", val);
        } else {
            CLI_LOG("nvmlDeviceGetClock: NOT AVAILABLE\n");
        }

        // Run offset detection and show results
        detect_clock_offsets();
        CLI_LOG("\n--- Offset Detection Results ---\n");
        CLI_LOG("GPU offset (after detection): %d MHz\n", g_app.gpuClockOffsetkHz / 1000);
        CLI_LOG("Mem offset (after detection): %d MHz (display)\n", mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz));
        if (g_app.loaded) {
            unsigned int vfMaxMHz = 0;
            for (int i = 0; i < VF_NUM_POINTS; i++) {
                unsigned int freqMHz = displayed_curve_mhz(g_app.curve[i].freq_kHz);
                if (freqMHz > vfMaxMHz) vfMaxMHz = freqMHz;
            }
            CLI_LOG("VF curve max: %u MHz\n", vfMaxMHz);
        }
        if (nvml_read_fans(detail, sizeof(detail))) {
            CLI_LOG("Fans: %u (range %u..%u), mode=%s\n", g_app.fanCount, g_app.fanMinPct, g_app.fanMaxPct, g_app.fanIsAuto ? "auto" : "manual");
            for (unsigned int fan = 0; fan < g_app.fanCount; fan++) {
                CLI_LOG("  Fan %u: pct=%u rpm=%u policy=%u signal=%u target=0x%X\n",
                    fan, g_app.fanPercent[fan], g_app.fanRpm[fan], g_app.fanPolicy[fan], g_app.fanControlSignal[fan], g_app.fanTargetMask[fan]);
            }
        } else {
            CLI_LOG("Fans: %s\n", detail);
        }
        CLI_LOG("\n=== Probe complete ===\n");

        char probePath[MAX_PATH] = {};
        if (opts.hasProbeOutputPath) {
            StringCchCopyA(probePath, ARRAY_COUNT(probePath), opts.probeOutputPath);
        } else {
            SYSTEMTIME now = {};
            GetLocalTime(&now);
            StringCchPrintfA(probePath, ARRAY_COUNT(probePath),
                "greencurve_probe_%04u%02u%02u_%02u%02u%02u.json",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        }
        char err[256] = {};
        if (write_probe_report(probePath, err, sizeof(err))) {
            CLI_LOG("Probe report written to %s\n", probePath);
        } else {
            CLI_LOG("ERROR: Failed to write probe report: %s\n", err);
        }
    }

    if (opts.json) {
        char err[256] = {};
        if (curveOk && write_json_snapshot(APP_JSON_FILE, err, sizeof(err))) {
            CLI_LOG("JSON written to %s\n", APP_JSON_FILE);
        } else if (!curveOk) {
            CLI_LOG("ERROR: JSON snapshot unavailable because live VF read did not succeed.\n");
        } else {
            CLI_LOG("ERROR: %s\n", err);
        }
    }

    CLI_LOG("\nGreen Curve CLI done.\n");
    fclose(logf);
    #undef CLI_LOG
    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrev*/, LPSTR /*lpCmdLine*/, int nCmdShow) {
    LPWSTR wCmdLine = GetCommandLineW();

    g_debug_logging = (GetEnvironmentVariableA(APP_DEBUG_ENV, nullptr, 0) > 0);
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

    // CLI mode - handle --dump, --json, --help
    if (handle_cli(wCmdLine)) {
        return 0;
    }

    // Initialize DPI awareness
    SetProcessDPIAware();
    init_dpi();

    // GUI mode: Check UAC elevation
    if (!is_elevated() && !is_elevated_flag(wCmdLine)) {
        request_elevation();
    }

    g_app.hInst = hInstance;
    set_default_config_path();

    if (!acquire_single_instance_mutex()) {
        return 0;
    }

    // Init NvAPI
    if (!nvapi_init()) {
        MessageBoxA(nullptr, "Failed to initialize NvAPI.\nIs an NVIDIA GPU and driver installed?",
                     "Green Curve - Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!nvapi_enum_gpu()) {
        MessageBoxA(nullptr, "No NVIDIA GPU found.", "Green Curve - Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    nvapi_get_name();
    nvapi_read_gpu_metadata();

    // Read initial curve
    bool curveOk = nvapi_read_curve();
    nvapi_read_offsets();

        if (!curveOk) {
            char message[512] = {};
        if (!g_app.vfBackend) {
            StringCchPrintfA(message, ARRAY_COUNT(message),
                "Detected an unrecognized NVIDIA GPU architecture (%s).\n\n"
                "Green Curve will not enable live VF access on unknown GPUs yet.\n"
                "Run:\n"
                "greencurve.exe --probe --probe-output <path>\n\n"
                "and share the resulting JSON report so support can be added safely.",
                g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");
        } else if (g_app.vfBackend && g_app.vfBackend->readSupported && !g_app.vfBackend->writeSupported) {
            StringCchPrintfA(message, ARRAY_COUNT(message),
                "Detected %s GPU (%s). Live VF curve reading is available, but VF editing is not implemented yet.\n\n"
                "Ask a user of this GPU family to run:\n"
                "greencurve.exe --probe --probe-output <path>\n\n"
                "and share the resulting JSON report.",
                gpu_family_name(g_app.gpuFamily),
                g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");
        } else if (g_app.vfBackend && !g_app.vfBackend->readSupported) {
            StringCchPrintfA(message, ARRAY_COUNT(message),
                "Detected %s GPU (%s), but live VF support is not implemented yet.\n\n"
                "Ask a user of this GPU family to run:\n"
                "greencurve.exe --probe --probe-output <path>\n\n"
                "and share the resulting JSON report.",
                gpu_family_name(g_app.gpuFamily),
                g_app.gpuName[0] ? g_app.gpuName : "NVIDIA GPU");
        } else {
            StringCchPrintfA(message, ARRAY_COUNT(message),
                "Failed to read VF curve from GPU.\n"
                "This may require administrator privileges or a supported GPU.");
        }
        MessageBoxA(nullptr, message, "Green Curve - Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Build visible map after initial read
    rebuild_visible_map();
    detect_locked_tail_from_curve();

    // Read global OC/PL/fan values
    {
        char detail[128] = {};
        refresh_global_state(detail, sizeof(detail));
    }

    // Migrate legacy config to profile slot format if needed
    migrate_legacy_config_if_needed(g_app.configPath);

    // Register window class
    auto load_app_icon = [hInstance](int cx, int cy) -> HICON {
        HICON icon = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(APP_ICON_ID), IMAGE_ICON, cx, cy, LR_SHARED);
        if (!icon) icon = LoadIcon(nullptr, IDI_APPLICATION);
        return icon;
    };

    g_app.hWindowClassBrush = CreateSolidBrush(COL_BG);

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = APP_CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_app.hWindowClassBrush;
    wc.hIcon = load_app_icon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = load_app_icon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    wc.style = 0;  // no CS_HREDRAW/CS_VREDRAW to reduce flicker

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(nullptr, "Failed to register window class.", "Green Curve", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Create main window
    SIZE initialSize = main_window_min_size();
    int winW = initialSize.cx;
    int winH = initialSize.cy;
    g_app.hMainWnd = CreateWindowExA(
        0, APP_CLASS_NAME,
        APP_TITLE,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        winW, winH,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_app.hMainWnd) {
        MessageBoxA(nullptr, "Failed to create window.", "Green Curve", MB_OK | MB_ICONERROR);
        return 1;
    }

    SendMessageA(g_app.hMainWnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    SendMessageA(g_app.hMainWnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);

    auto load_tray_icon = [hInstance](int resourceId) -> HICON {
        HICON icon = (HICON)LoadImageA(
            hInstance,
            MAKEINTRESOURCEA(resourceId),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            0);
        if (!icon) icon = CopyIcon(LoadIcon(nullptr, IDI_APPLICATION));
        return icon;
    };
    g_app.trayIcons[TRAY_ICON_STATE_DEFAULT] = load_tray_icon(TRAY_ICON_DEFAULT_ID);
    g_app.trayIcons[TRAY_ICON_STATE_OC] = load_tray_icon(TRAY_ICON_OC_ID);
    g_app.trayIcons[TRAY_ICON_STATE_FAN] = load_tray_icon(TRAY_ICON_FAN_ID);
    g_app.trayIcons[TRAY_ICON_STATE_OC_FAN] = load_tray_icon(TRAY_ICON_OC_FAN_ID);
    g_app.trayIconState = TRAY_ICON_STATE_DEFAULT;

    // Create buttons (positioned by create_edit_controls)
    g_app.hApplyBtn = CreateWindowExA(
        0, "BUTTON", "Apply Changes",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, dp(110), dp(30),
        g_app.hMainWnd, (HMENU)(INT_PTR)APPLY_BTN_ID, hInstance, nullptr
    );

    g_app.hRefreshBtn = CreateWindowExA(
        0, "BUTTON", "Refresh",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, dp(90), dp(30),
        g_app.hMainWnd, (HMENU)(INT_PTR)REFRESH_BTN_ID, hInstance, nullptr
    );

    g_app.hResetBtn = CreateWindowExA(
        0, "BUTTON", "Reset",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, dp(90), dp(30),
        g_app.hMainWnd, (HMENU)(INT_PTR)RESET_BTN_ID, hInstance, nullptr
    );

    g_app.hLicenseBtn = CreateWindowExA(
        0, "BUTTON", "License",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, dp(80), dp(30),
        g_app.hMainWnd, (HMENU)(INT_PTR)LICENSE_BTN_ID, hInstance, nullptr
    );

    g_app.hProfileCombo = CreateWindowExA(
        0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        0, 0, dp(156), dp(220),
        g_app.hMainWnd, (HMENU)(INT_PTR)PROFILE_COMBO_ID, hInstance, nullptr
    );
    SendMessageA(g_app.hProfileCombo, CB_SETCURSEL, (WPARAM)(CONFIG_DEFAULT_SLOT - 1), 0);

    g_app.hProfileLabel = CreateWindowExA(
        0, "STATIC", "Profile slot:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, dp(72), dp(18),
        g_app.hMainWnd, (HMENU)(INT_PTR)PROFILE_LABEL_ID, hInstance, nullptr
    );

    g_app.hProfileStateLabel = CreateWindowExA(
        0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, dp(220), dp(18),
        g_app.hMainWnd, (HMENU)(INT_PTR)PROFILE_STATE_ID, hInstance, nullptr
    );

    g_app.hProfileLoadBtn = CreateWindowExA(
        0, "BUTTON", "Load",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, dp(65), dp(22),
        g_app.hMainWnd, (HMENU)(INT_PTR)PROFILE_LOAD_ID, hInstance, nullptr
    );

    g_app.hProfileSaveBtn = CreateWindowExA(
        0, "BUTTON", "Save",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, dp(65), dp(22),
        g_app.hMainWnd, (HMENU)(INT_PTR)PROFILE_SAVE_ID, hInstance, nullptr
    );

    g_app.hProfileClearBtn = CreateWindowExA(
        0, "BUTTON", "Clear",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, dp(65), dp(22),
        g_app.hMainWnd, (HMENU)(INT_PTR)PROFILE_CLEAR_ID, hInstance, nullptr
    );

    g_app.hAppLaunchCombo = CreateWindowExA(
        0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        0, 0, dp(170), dp(220),
        g_app.hMainWnd, (HMENU)(INT_PTR)APP_LAUNCH_COMBO_ID, hInstance, nullptr
    );
    SendMessageA(g_app.hAppLaunchCombo, CB_SETCURSEL, 0, 0);

    g_app.hAppLaunchLabel = CreateWindowExA(
        0, "STATIC", "Apply profile on GUI start:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, dp(170), dp(18),
        g_app.hMainWnd, (HMENU)(INT_PTR)APP_LAUNCH_LABEL_ID, hInstance, nullptr
    );

    g_app.hLogonCombo = CreateWindowExA(
        0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        0, 0, dp(170), dp(220),
        g_app.hMainWnd, (HMENU)(INT_PTR)LOGON_COMBO_ID, hInstance, nullptr
    );
    SendMessageA(g_app.hLogonCombo, CB_SETCURSEL, 0, 0);

    g_app.hLogonLabel = CreateWindowExA(
        0, "STATIC", "Apply profile after user log in:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, dp(208), dp(18),
        g_app.hMainWnd, (HMENU)(INT_PTR)LOGON_LABEL_ID, hInstance, nullptr
    );

    g_app.hProfileStatusLabel = CreateWindowExA(
        0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, dp(420), dp(18),
        g_app.hMainWnd, (HMENU)(INT_PTR)PROFILE_STATUS_ID, hInstance, nullptr
    );

    g_app.hStartOnLogonCheck = CreateWindowExA(
        0, "BUTTON", "Start program to tray on log in",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, dp(320), dp(24),
        g_app.hMainWnd, (HMENU)(INT_PTR)START_ON_LOGON_CHECK_ID, hInstance, nullptr
    );

    g_app.hLogonHintLabel = CreateWindowExA(
        0, "STATIC",
        "Starts silently without tray icon or window when apply on log in is activated without above tray checkbox ticked.\r\nWithout tray runtime, custom fan modes fall back to driver auto.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, dp(900), dp(34),
        g_app.hMainWnd, (HMENU)(INT_PTR)LOGON_HINT_ID, hInstance, nullptr
    );

    layout_bottom_buttons(g_app.hMainWnd);

    // Create edit controls
    create_edit_controls(g_app.hMainWnd, hInstance);
    ensure_tray_icon();
    apply_logon_startup_behavior();
    if (!g_app.startHiddenToTray) {
        show_best_guess_support_warning(g_app.hMainWnd);
    }
    maybe_load_app_launch_profile_to_gui();
    invalidate_main_window();

    if (g_app.startHiddenToTray) {
        hide_main_window_to_tray();
    } else {
        show_window_with_primed_first_frame(g_app.hMainWnd, nCmdShow);
    }

    schedule_logon_combo_sync();

    // Message loop
    MSG msg = {};
    while (GetMessageA(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    release_single_instance_mutex();
    close_nvml();
    for (int i = 0; i < 4; i++) {
        if (g_app.trayIcons[i]) {
            DestroyIcon(g_app.trayIcons[i]);
            g_app.trayIcons[i] = nullptr;
        }
    }
    if (g_app.hWindowClassBrush) {
        DeleteObject(g_app.hWindowClassBrush);
        g_app.hWindowClassBrush = nullptr;
    }

    return (int)msg.wParam;
}
