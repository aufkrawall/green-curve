// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Entry Point
// ============================================================================

// CLI mode: --dump, --json, or --probe
// Returns true if CLI handled (should exit), false if should run GUI
static int g_cliExitCode = 0;

static void set_main_window_title(HWND hwnd) {
    if (!hwnd) return;
    char title[128] = {};
    StringCchPrintfA(title, ARRAY_COUNT(title), "%s v%s \"%s\"", APP_NAME, APP_VERSION, APP_EDITION);
    SetWindowTextA(hwnd, title);
}

static bool handle_cli(LPWSTR wCmdLine) {
    CliOptions opts = {};
    if (!parse_cli_options(wCmdLine, &opts)) {
        char err[256] = {};
        const char* text = opts.error[0] ? opts.error : "Failed to parse CLI";
        resolve_data_paths(err, sizeof(err));
        write_text_file_atomic(cli_log_path(), text, strlen(text), err, sizeof(err));
        return true;
    }
    if (opts.logonStart) {
        set_default_config_path();
        if (opts.hasConfigPath) StringCchCopyA(g_app.configPath, ARRAY_COUNT(g_app.configPath), opts.configPath);
        g_app.launchedFromLogon = true;

        // Logon startup has two distinct behaviors:
        // 1. tray startup enabled: launch resident app hidden to tray
        // 2. tray startup disabled: do a silent one-shot profile apply and exit
        if (is_start_on_logon_enabled(g_app.configPath)) {
            g_app.startHiddenToTray = true;
            return false;
        }

        opts.recognized = true;
        opts.applyConfig = true;
        opts.logonStart = false;
    }
    if (!opts.recognized) return false;
    set_default_config_path();
    if (opts.hasConfigPath) StringCchCopyA(g_app.configPath, ARRAY_COUNT(g_app.configPath), opts.configPath);

    // CLI always writes to file since we're a GUI subsystem app
    char pathErr[256] = {};
    resolve_data_paths(pathErr, sizeof(pathErr));
    const char* logPath = cli_log_path();
    FILE* logf = fopen(logPath, "w");
    if (!logf) return true;

    #define CLI_LOG(...) do { char _gc_ts[64] = {}; format_log_timestamp_prefix(_gc_ts, sizeof(_gc_ts)); fprintf(logf, "%s", _gc_ts); fprintf(logf, __VA_ARGS__); fflush(logf); } while(0)

    CLI_LOG("Green Curve CLI mode started\n");

    if (opts.serviceInstall || opts.serviceRemove) {
        char err[256] = {};
        bool ok = service_install_or_remove(opts.serviceInstall, err, sizeof(err));
        if (ok) {
            CLI_LOG(opts.serviceInstall ? "Background service installed.\n" : "Background service removed.\n");
            g_cliExitCode = 0;
        } else {
            CLI_LOG("ERROR: %s\n", err[0] ? err : "Background service update failed");
            g_cliExitCode = 1;
        }
        fclose(logf);
        return true;
    }

    if (opts.startupTaskEnable || opts.startupTaskDisable) {
        char err[256] = {};
        bool ok = set_startup_task_enabled(opts.startupTaskEnable, err, sizeof(err));
        if (ok) {
            CLI_LOG(opts.startupTaskEnable ? "Startup task enabled.\n" : "Startup task disabled.\n");
            g_cliExitCode = 0;
        } else {
            CLI_LOG("ERROR: %s\n", err[0] ? err : "Startup task update failed");
            g_cliExitCode = 1;
        }
        fclose(logf);
        return true;
    }

    if (opts.showHelp) {
        CLI_LOG(APP_NAME " v" APP_VERSION " - NVIDIA VF Curve Editor\n");
        CLI_LOG("Usage:\n");
        CLI_LOG("  greencurve.exe              Launch GUI\n");
        CLI_LOG("  greencurve.exe --dump       Write VF curve to greencurve_cli_log.txt\n");
        CLI_LOG("  greencurve.exe --json       Write VF curve to greencurve_curve.json\n");
        CLI_LOG("  greencurve.exe --probe [--probe-output <path>]  Probe NvAPI/NVML/VF support and write a report\n");
        CLI_LOG("  greencurve.exe --gpu-offset <mhz> --mem-offset <mhz> --power-limit <pct>\n");
        CLI_LOG("  greencurve.exe --fan <auto|0-100> --point49 <mhz> ... --point127 <mhz>\n");
        CLI_LOG("  greencurve.exe --apply-config [--config <path>]  Apply logon profile slot\n");
        CLI_LOG("  greencurve.exe --service-install           Install and start background service\n");
        CLI_LOG("  greencurve.exe --service-remove            Stop and remove background service\n");
        CLI_LOG("  greencurve.exe --save-config [--config <path>]  Save to selected profile slot\n");
        CLI_LOG("  greencurve.exe --reset      Reset curve/global controls to defaults\n");
        CLI_LOG("  greencurve.exe --help       This help\n");
        fclose(logf);
        return true;
    }

    refresh_background_service_state();
    bool needsService = opts.applyConfig || desired_has_any_action(&opts.desired) || opts.reset || opts.saveConfig || opts.dump || opts.json || opts.probe;
    if (needsService && !g_app.backgroundServiceAvailable) {
        CLI_LOG("ERROR: Background service is required for hardware operations but is not available.\n");
        if (!g_app.backgroundServiceInstalled) {
            CLI_LOG("Install it with: greencurve.exe --service-install\n");
        } else if (!g_app.backgroundServiceRunning) {
            CLI_LOG("The service is installed but not running. Start or reinstall it.\n");
        } else {
            CLI_LOG("The service is installed but not responding. Restart or reinstall it.\n");
        }
        fclose(logf);
        g_cliExitCode = 1;
        return true;
    }

    if (g_app.backgroundServiceAvailable) {
        ServiceSnapshot snapshot = {};
        char err[256] = {};
        if (service_client_get_snapshot(&snapshot, err, sizeof(err))) {
            apply_service_snapshot_to_app(&snapshot);
            CLI_LOG("Green Curve: Background service available. Using service-backed hardware control path.\n");
        } else {
            CLI_LOG("ERROR: %s\n", err[0] ? err : "Failed reading background service snapshot");
            fclose(logf);
            g_cliExitCode = 1;
            return true;
        }
    }

    if (opts.applyConfig) {
        DesiredSettings cfg = {};
        char err[256] = {};
        // Determine which profile slot to apply
        int logonSlot = get_config_int(g_app.configPath, "profiles", "logon_slot", 0);
        if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;
        if (logonSlot < 1 || logonSlot > CONFIG_NUM_SLOTS) {
            CLI_LOG("ERROR: No valid logon profile slot is configured. Silent logon apply was skipped.\n");
            fclose(logf);
            g_cliExitCode = 1;
            return true;
        }
        if (!is_profile_slot_saved(g_app.configPath, logonSlot)) {
            CLI_LOG("ERROR: Logon profile slot %d is empty. Silent logon apply was skipped.\n", logonSlot);
            fclose(logf);
            g_cliExitCode = 1;
            return true;
        }
        if (!load_profile_from_config(g_app.configPath, logonSlot, &cfg, err, sizeof(err))) {
            write_error_report_log_for_user_failure("CLI profile load failed", err);
            CLI_LOG("ERROR: %s\n", err);
            fclose(logf);
            g_cliExitCode = 1;
            return true;
        }
        if (!desired_settings_have_explicit_state(&cfg, true, err, sizeof(err))) {
            write_error_report_log_for_user_failure("CLI logon profile rejected", err);
            CLI_LOG("ERROR: %s\n", err);
            fclose(logf);
            g_cliExitCode = 1;
            return true;
        }

        int profileCurvePoints = 0;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (cfg.hasCurvePoint[i]) profileCurvePoints++;
        }
        CLI_LOG("Green Curve: Profile summary gpu=%dMHz exclLow=%d lockCi=%d lockMHz=%u curvePoints=%d fanMode=%d\n",
            cfg.gpuOffsetMHz,
            cfg.gpuOffsetExcludeLowCount,
            cfg.hasLock ? cfg.lockCi : -1,
            cfg.hasLock ? cfg.lockMHz : 0u,
            profileCurvePoints,
            cfg.hasFan ? cfg.fanMode : -1);

        if (g_app.launchedFromLogon) {
            CLI_LOG("Waiting for GPU driver readiness before applying profile %d...\n", logonSlot);
            bool driverReady = false;
            for (int attempt = 0; attempt < 25; attempt++) {
                refresh_background_service_state();
                if (!g_app.backgroundServiceAvailable) {
                    Sleep(500);
                    continue;
                }
                ServiceSnapshot snapshot = {};
                char snapErr[256] = {};
                if (service_client_get_snapshot(&snapshot, snapErr, sizeof(snapErr))) {
                    if (snapshot.loaded && snapshot.numPopulated > 0 && snapshot.initialized) {
                        driverReady = true;
                        CLI_LOG("GPU driver ready on attempt %d (populated=%d)\n", attempt + 1, snapshot.numPopulated);
                        break;
                    }
                }
                Sleep(400);
            }
            if (!driverReady) {
                CLI_LOG("ERROR: GPU driver did not become ready in time. Skipping apply.\n");
                fclose(logf);
                g_cliExitCode = 1;
                return true;
            }
            Sleep(300);
        }

        CLI_LOG("Applying profile %d...\n", logonSlot);
        merge_desired_settings(&cfg, &opts.desired);
        char result[512] = {};
        set_pending_operation_source("CLI apply-config");
        bool ok = apply_desired_settings(&cfg, false, result, sizeof(result));
        CLI_LOG("%s\n", result);
        if (!ok) {
            fclose(logf);
            g_cliExitCode = 1;
            return true;
        }
        g_cliExitCode = 0;
    } else if (desired_has_any_action(&opts.desired)) {
        char result[512] = {};
        set_pending_operation_source("CLI direct apply");
        bool ok = apply_desired_settings(&opts.desired, false, result, sizeof(result));
        CLI_LOG("%s\n", result);
        if (!ok) {
            fclose(logf);
            g_cliExitCode = 1;
            return true;
        }
        g_cliExitCode = 0;
    }

    if (opts.reset) {
        char result[512] = {};
        bool ok = service_client_reset(result, sizeof(result), nullptr);
        CLI_LOG("%s\n", result);
        if (!ok) {
            fclose(logf);
            g_cliExitCode = 1;
            return true;
        }
    }

    if (opts.saveConfig) {
        DesiredSettings saveDesired = {};
        bool useDesired = false;
        int targetSlot = get_config_int(g_app.configPath, "profiles", "selected_slot", CONFIG_DEFAULT_SLOT);
        if (targetSlot < 1 || targetSlot > CONFIG_NUM_SLOTS) targetSlot = CONFIG_DEFAULT_SLOT;
        char err[256] = {};
        if (!refresh_service_snapshot_and_active_desired(err, sizeof(err))) {
            CLI_LOG("ERROR: %s\n", err[0] ? err : "Failed to refresh background service state before save");
            g_cliExitCode = 1;
            fclose(logf);
            return true;
        }
        if (opts.applyConfig) {
            if (!load_desired_settings_from_ini(g_app.configPath, &saveDesired, opts.error, sizeof(opts.error))) {
                CLI_LOG("ERROR: %s\n", opts.error);
                g_cliExitCode = 1;
                fclose(logf);
                return true;
            }
            merge_desired_settings(&saveDesired, &opts.desired);
            useDesired = true;
        } else if (desired_has_any_action(&opts.desired)) {
            saveDesired = opts.desired;
            useDesired = true;
        }
        if (!useDesired) {
            initialize_desired_settings_defaults(&saveDesired);
            ControlState control = {};
            bool haveControlState = get_effective_control_state(&control);
            saveDesired.hasGpuOffset = true;
            if (haveControlState && control_state_has_meaningful_gpu(&control)) {
                saveDesired.gpuOffsetMHz = control.gpuOffsetMHz;
                saveDesired.gpuOffsetExcludeLowCount = control.gpuOffsetExcludeLowCount;
            } else {
                resolve_displayed_live_gpu_offset_state_for_gui(&saveDesired.gpuOffsetMHz, &saveDesired.gpuOffsetExcludeLowCount);
            }
            saveDesired.hasMemOffset = true;
            saveDesired.memOffsetMHz = haveControlState && control_state_has_meaningful_mem(&control) ? control.memOffsetMHz : mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
            saveDesired.hasPowerLimit = true;
            saveDesired.powerLimitPct = haveControlState && control_state_has_meaningful_power(&control) ? control.powerLimitPct : g_app.powerLimitPct;
            saveDesired.hasFan = true;
            saveDesired.fanMode = haveControlState && control_state_has_meaningful_fan(&control) ? control.fanMode : g_app.activeFanMode;
            saveDesired.fanAuto = saveDesired.fanMode == FAN_MODE_AUTO;
            saveDesired.fanPercent = haveControlState && control_state_has_meaningful_fan(&control) ? control.fanFixedPercent : g_app.activeFanFixedPercent;
            copy_fan_curve(&saveDesired.fanCurve, haveControlState && control_state_has_meaningful_fan(&control) ? &control.fanCurve : &g_app.activeFanCurve);
            for (int i = 0; i < VF_NUM_POINTS; i++) {
                if (g_app.curve[i].freq_kHz > 0) {
                    saveDesired.hasCurvePoint[i] = true;
                    saveDesired.curvePointMHz[i] = displayed_curve_mhz(g_app.curve[i].freq_kHz);
                }
            }
        }
        if (!save_profile_to_config(g_app.configPath, targetSlot, &saveDesired, err, sizeof(err))) {
            CLI_LOG("ERROR: %s\n", err);
            g_cliExitCode = 1;
            fclose(logf);
            return true;
        }
        CLI_LOG("Profile %d written to %s\n", targetSlot, g_app.configPath);
    }

    if (opts.dump) {
        char result[512] = {};
        if (service_client_write_file_command(SERVICE_CMD_WRITE_LOG_SNAPSHOT, logPath, "client dump", result, sizeof(result))) {
            CLI_LOG("VF curve dump written to %s\n", logPath);
            CLI_LOG("%s\n", result);
        } else {
            CLI_LOG("ERROR: %s\n", result);
            fclose(logf);
            g_cliExitCode = 1;
            return true;
        }
    }

    if (opts.probe) {
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
        char result[512] = {};
        if (service_client_write_file_command(SERVICE_CMD_WRITE_PROBE_REPORT, probePath, "client probe", result, sizeof(result))) {
            CLI_LOG("Probe report written to %s\n", probePath);
            CLI_LOG("%s\n", result);
        } else {
            CLI_LOG("ERROR: %s\n", result);
            fclose(logf);
            g_cliExitCode = 1;
            return true;
        }
    }

    if (opts.json) {
        const char* jsonPath = json_snapshot_path();
        char result[512] = {};
        if (service_client_write_file_command(SERVICE_CMD_WRITE_JSON_SNAPSHOT, jsonPath, "client json", result, sizeof(result))) {
            CLI_LOG("JSON written to %s\n", jsonPath);
            CLI_LOG("%s\n", result);
        } else {
            CLI_LOG("ERROR: %s\n", result);
            fclose(logf);
            g_cliExitCode = 1;
            return true;
        }
    }

    CLI_LOG("\nGreen Curve CLI done.\n");
    fclose(logf);
    #undef CLI_LOG
    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrev*/, LPSTR /*lpCmdLine*/, int nCmdShow) {
    LPWSTR wCmdLine = GetCommandLineW();

    // Initialize the shared config lock before the service handoff path can read config.
    SetProcessDPIAware();
    init_dpi();
    InitializeCriticalSection(&g_debugLogLock);

    set_default_config_path();
    char pathErr[256] = {};
    resolve_data_paths(pathErr, sizeof(pathErr));

    // Initialize the remaining GUI-only process state before reading config/UI state.
    initialize_dark_mode_support();

    refresh_background_service_state();
    debug_log("startup state: usingService=%d installed=%d running=%d available=%d broken=%d launchedFromLogon=%d hiddenToTray=%d\n",
        g_app.usingBackgroundService ? 1 : 0,
        g_app.backgroundServiceInstalled ? 1 : 0,
        g_app.backgroundServiceRunning ? 1 : 0,
        g_app.backgroundServiceAvailable ? 1 : 0,
        g_app.backgroundServiceBroken ? 1 : 0,
        g_app.launchedFromLogon ? 1 : 0,
        g_app.startHiddenToTray ? 1 : 0);
    debug_log("startup config path: %s\n", g_app.configPath[0] ? g_app.configPath : "<unset>");

    char debugEnvBuf[16] = {};
    DWORD debugEnvLen = GetEnvironmentVariableA(APP_DEBUG_ENV, debugEnvBuf, ARRAY_COUNT(debugEnvBuf));
    bool debugEnvExplicitlyDisabled = (debugEnvLen > 0 && debugEnvBuf[0] == '0' && debugEnvBuf[1] == '\0');
    bool debugEnvEnabled = (debugEnvLen > 0 && !debugEnvExplicitlyDisabled);
    bool configExists = config_file_exists();
    int configDebugEnabled = get_config_int(g_app.configPath, "debug", "enabled", APP_DEBUG_DEFAULT_ENABLED);
    if (debugEnvExplicitlyDisabled) {
        g_debug_logging = false;
    } else {
        g_debug_logging = debugEnvEnabled || (configDebugEnabled != 0);
    }
    if (g_debug_logging) {
        g_debugSessionStartTickMs = GetTickCount64();
        debug_log("debug enabled: env=%d configExists=%d configDebug=%d path=%s\n",
            debugEnvEnabled ? 1 : 0,
            configExists ? 1 : 0,
            configDebugEnabled,
            g_app.configPath);
        debug_log_session_marker("BEGIN", "gui", wCmdLine ? "WinMain startup" : nullptr);
    }
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

    // CLI mode - handle --dump, --json, --help
    if (handle_cli(wCmdLine)) {
        DeleteCriticalSection(&g_debugLogLock);
        return g_cliExitCode;
    }

    g_app.hInst = hInstance;
    g_app.guiFanMode = GUI_FAN_MODE_UNSET;

    if (!acquire_single_instance_mutex()) {
        return 0;
    }

    bool offsetsOk = false;
    bool curveOk = false;
    if (g_app.backgroundServiceInstalled && g_app.backgroundServiceAvailable) {
        ServiceSnapshot snapshot = {};
        DesiredSettings activeDesired = {};
        char err[256] = {};
        if (!service_client_get_snapshot(&snapshot, err, sizeof(err))) {
            g_app.backgroundServiceAvailable = false;
            g_app.backgroundServiceBroken = true;
            clear_service_authoritative_state();
            curveOk = false;
            offsetsOk = false;
            debug_log("startup service snapshot failed: %s\n", err[0] ? err : "unknown");
            set_profile_status_text("Background service is installed but not responding. Start or reinstall the service to restore GPU control.");
        }
        if (g_app.backgroundServiceAvailable) {
            apply_service_snapshot_to_app(&snapshot);
            char desiredErr[256] = {};
            if (service_client_get_active_desired(&activeDesired, nullptr, desiredErr, sizeof(desiredErr))) {
                apply_service_desired_to_gui(&activeDesired);
            }
            curveOk = snapshot.loaded;
            offsetsOk = curveOk;
        }
    } else {
        clear_service_authoritative_state();
        curveOk = false;
        offsetsOk = false;
    }

    // A settled snapshot already rebuilt the visible map and inferred the lock tail.
    (void)offsetsOk;

    if (!curveOk && g_app.usingBackgroundService) {
        g_app.loaded = false;
        g_app.numPopulated = 0;
        g_app.fanSupported = false;
    }

    if (g_app.backgroundServiceAvailable) {
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

    g_taskbarCreatedMessage = RegisterWindowMessageA("TaskbarCreated");

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
        APP_NAME,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        winW, winH,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_app.hMainWnd) {
        MessageBoxA(nullptr, "Failed to create window.", "Green Curve", MB_OK | MB_ICONERROR);
        return 1;
    }

    allow_dark_mode_for_window(g_app.hMainWnd);
    set_main_window_title(g_app.hMainWnd);

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
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, dp(110), dp(30),
        g_app.hMainWnd, (HMENU)(INT_PTR)APPLY_BTN_ID, hInstance, nullptr
    );

    g_app.hRefreshBtn = CreateWindowExA(
        0, "BUTTON", "Refresh",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, dp(90), dp(30),
        g_app.hMainWnd, (HMENU)(INT_PTR)REFRESH_BTN_ID, hInstance, nullptr
    );

    g_app.hResetBtn = CreateWindowExA(
        0, "BUTTON", "Reset",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, dp(90), dp(30),
        g_app.hMainWnd, (HMENU)(INT_PTR)RESET_BTN_ID, hInstance, nullptr
    );

    g_app.hLicenseBtn = CreateWindowExA(
        0, "BUTTON", "License",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
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
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, dp(65), dp(22),
        g_app.hMainWnd, (HMENU)(INT_PTR)PROFILE_LOAD_ID, hInstance, nullptr
    );

    g_app.hProfileSaveBtn = CreateWindowExA(
        0, "BUTTON", "Save",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, dp(65), dp(22),
        g_app.hMainWnd, (HMENU)(INT_PTR)PROFILE_SAVE_ID, hInstance, nullptr
    );

    g_app.hProfileClearBtn = CreateWindowExA(
        0, "BUTTON", "Clear",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
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
        0, "BUTTON", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, dp(16), dp(16),
        g_app.hMainWnd, (HMENU)(INT_PTR)START_ON_LOGON_CHECK_ID, hInstance, nullptr
    );
    g_app.hStartOnLogonLabel = CreateWindowExA(
        0, "STATIC", "Start program to tray on log in",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
        0, 0, dp(300), dp(18),
        g_app.hMainWnd, (HMENU)(INT_PTR)START_ON_LOGON_LABEL_ID, hInstance, nullptr
    );

    g_app.hServiceEnableCheck = CreateWindowExA(
        0, "BUTTON", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, dp(16), dp(16),
        g_app.hMainWnd, (HMENU)(INT_PTR)SERVICE_ENABLE_CHECK_ID, hInstance, nullptr
    );
    g_app.hServiceEnableLabel = CreateWindowExA(
        0, "STATIC", "Background service installed",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
        0, 0, dp(340), dp(18),
        g_app.hMainWnd, (HMENU)(INT_PTR)SERVICE_ENABLE_LABEL_ID, hInstance, nullptr
    );
    g_app.hServiceStatusLabel = CreateWindowExA(
        0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, dp(460), dp(18),
        g_app.hMainWnd, (HMENU)(INT_PTR)SERVICE_STATUS_ID, hInstance, nullptr
    );

    g_app.hLogonHintLabel = CreateWindowExA(
        0, "STATIC",
        "OC, UV, power, and custom fan control require the background service.\r\nWhen the service is unavailable, live tuning controls stay disabled until it is running again.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, dp(900), dp(34),
        g_app.hMainWnd, (HMENU)(INT_PTR)LOGON_HINT_ID, hInstance, nullptr
    );

    apply_ui_font_to_children(g_app.hMainWnd);

    layout_bottom_buttons(g_app.hMainWnd);

    // Create edit controls
    create_edit_controls(g_app.hMainWnd, hInstance);
    ensure_tray_icon();
    refresh_background_service_state();
    update_background_service_controls();
    apply_logon_startup_behavior();
    if (g_app.usingBackgroundService && g_app.backgroundServiceBroken) {
        set_profile_status_text("Background service is installed but not responding. Start or reinstall the service to restore GPU control.");
    }
    if (!g_app.startHiddenToTray && g_app.backgroundServiceAvailable) {
        if (!show_best_guess_support_warning(g_app.hMainWnd)) {
            remove_tray_icon();
            release_single_instance_mutex();
            close_nvml();
            if (g_app.hNvApi) { FreeLibrary(g_app.hNvApi); g_app.hNvApi = nullptr; }
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
            if (s_hUiFont) {
                DeleteObject(s_hUiFont);
                s_hUiFont = nullptr;
            }
            close_debug_log_file();
            DeleteCriticalSection(&g_configLock);
            DeleteCriticalSection(&g_appLock);
            DeleteCriticalSection(&g_debugLogLock);
            return 0;
        }
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
    if (g_app.hNvApi) { FreeLibrary(g_app.hNvApi); g_app.hNvApi = nullptr; }
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
    if (s_hUiFont) {
        DeleteObject(s_hUiFont);
        s_hUiFont = nullptr;
    }
    close_debug_log_file();
    DeleteCriticalSection(&g_configLock);
    DeleteCriticalSection(&g_appLock);
    DeleteCriticalSection(&g_debugLogLock);

    return (int)msg.wParam;
}
