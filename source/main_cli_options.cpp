// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Windows command line parsing, split out of main_runtime_nvml.cpp.
//
// The parser had grown to a fifth of that file while having nothing to do with
// NVML, and it is the one place a new CLI switch is added, so it now lives on
// its own next to the Linux port's equivalent (linux_cli_options.cpp).
// Included from main_runtime_nvml.cpp so the translation-unit order that the
// rest of the amalgamated build depends on is unchanged.

static LPWSTR gc_cli_take_next_arg(LPWSTR* argv, int argc, int* index) {
    if (!argv || !index || *index + 1 >= argc) return nullptr;
    ++*index;
    return argv[*index];
}

static void gc_cli_free_argv(LPWSTR* argv) {
    if (argv) LocalFree((HLOCAL)argv);
}

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
        } else if (wcscmp(arg, L"--self-test") == 0) {
            opts->recognized = true;
            opts->selfTest = true;
        } else if (wcscmp(arg, L"--clk-domain-probe") == 0) {
            opts->recognized = true;
            opts->clkDomainProbe = true;
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
        } else if (wcscmp(arg, L"--set-machine-logon-slot") == 0) {
            opts->recognized = true;
            int v = 0;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !parse_wide_int_arg(value, &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --set-machine-logon-slot value");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->setMachineLogonSlot = true;
            opts->machineLogonSlotValue = v;
        } else if (wcscmp(arg, L"--clear-machine-logon-slot") == 0) {
            opts->recognized = true;
            opts->clearMachineLogonSlot = true;
        } else if (wcscmp(arg, L"--publish-slot-to-machine") == 0) {
            opts->recognized = true;
            int v = 0;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !parse_wide_int_arg(value, &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --publish-slot-to-machine value");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->publishSlotToMachine = true;
            opts->machineSlotValue = v;
        } else if (wcscmp(arg, L"--clear-machine-slot") == 0) {
            opts->recognized = true;
            int v = 0;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !parse_wide_int_arg(value, &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --clear-machine-slot value");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->clearMachineSlot = true;
            opts->machineSlotValue = v;
        } else if (wcscmp(arg, L"--share-slot") == 0) {
            opts->recognized = true;
            int v = 0;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !parse_wide_int_arg(value, &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --share-slot value");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->shareSlot = true;
            opts->shareSlotValue = v;
        } else if (wcscmp(arg, L"--unshare-slot") == 0) {
            opts->recognized = true;
            int v = 0;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !parse_wide_int_arg(value, &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --unshare-slot value");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->unshareSlot = true;
            opts->shareSlotValue = v;
        } else if (wcscmp(arg, L"--set-restrict-shared") == 0) {
            opts->recognized = true;
            int v = 0;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !parse_wide_int_arg(value, &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --set-restrict-shared value (use 0 or 1)");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->setRestrictPolicy = true;
            opts->restrictPolicyValue = v;
        } else if (wcscmp(arg, L"--logon-start") == 0) {
            opts->recognized = true;
            opts->logonStart = true;
        } else if (wcscmp(arg, L"--tray-start") == 0) {
            // Internal per-user HKCU Run entry.  This is intentionally distinct
            // from the bounded Task Scheduler --logon-start handoff.
            opts->recognized = true;
            opts->trayStart = true;
        } else if (wcscmp(arg, L"--config") == 0) {
            opts->recognized = true;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !copy_wide_to_utf8(value, opts->configPath, MAX_PATH)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --config path");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->hasConfigPath = true;
        } else if (wcscmp(arg, L"--for-user") == 0) {
            // Elevated helper only: forces the per-user logon task to be scoped
            // to this requesting user instead of the approving admin.
            opts->recognized = true;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value) {
                set_message(opts->error, sizeof(opts->error), "Invalid --for-user value");
                gc_cli_free_argv(argv);
                return false;
            }
            set_forced_startup_user_sam(value);
        } else if (wcscmp(arg, L"--export-active-settings") == 0) {
            opts->recognized = true;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !copy_wide_to_utf8(value, opts->settingsFilePath, MAX_PATH)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --export-active-settings path");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->exportActiveSettings = true;
        } else if (wcscmp(arg, L"--apply-settings-file") == 0) {
            opts->recognized = true;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !copy_wide_to_utf8(value, opts->settingsFilePath, MAX_PATH)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --apply-settings-file path");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->applySettingsFile = true;
        } else if (wcscmp(arg, L"--probe-output") == 0) {
            opts->recognized = true;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !copy_wide_to_utf8(value, opts->probeOutputPath, MAX_PATH)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --probe-output path");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->hasProbeOutputPath = true;
        } else if (wcscmp(arg, L"--gpu-offset") == 0) {
            opts->recognized = true;
            int v = 0;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !parse_wide_int_arg(value, &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --gpu-offset value");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->desired.hasGpuOffset = true;
            opts->desired.gpuOffsetMHz = v;
        } else if (wcscmp(arg, L"--mem-offset") == 0) {
            opts->recognized = true;
            int v = 0;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !parse_wide_int_arg(value, &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --mem-offset value");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->desired.hasMemOffset = true;
            opts->desired.memOffsetMHz = v;
        } else if (wcscmp(arg, L"--power-limit") == 0) {
            opts->recognized = true;
            int v = 0;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !parse_wide_int_arg(value, &v)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --power-limit value");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->desired.hasPowerLimit = true;
            if (v < 50 || v > 150) {
                set_message(opts->error, sizeof(opts->error), "power_limit_pct %d is outside the safe range 50..150", v);
                gc_cli_free_argv(argv);
                return false;
            }
            opts->desired.powerLimitPct = v;
        } else if (wcscmp(arg, L"--fan") == 0) {
            opts->recognized = true;
            char buf[64] = {};
            bool fanAuto = false;
            LPWSTR value = gc_cli_take_next_arg(argv, argc, &i);
            if (!value || !copy_wide_to_utf8(value, buf, sizeof(buf)) ||
                !parse_fan_value(buf, &fanAuto, &opts->desired.fanPercent)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan value, use auto or 0-100");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanAuto = fanAuto;
            opts->desired.fanMode = fanAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
        } else if (wcsncmp(arg, L"--point", 7) == 0) {
            opts->recognized = true;
            int idx = -1;
            int v = 0;
            bool pointArgValid = parse_cli_point_arg_w(arg, &idx);
            LPWSTR value = pointArgValid
                ? gc_cli_take_next_arg(argv, argc, &i) : nullptr;
            if (!pointArgValid || !value ||
                !parse_wide_int_arg(value, &v) || v <= 0) {
                set_message(opts->error, sizeof(opts->error), "Invalid --pointN value");
                gc_cli_free_argv(argv);
                return false;
            }
            opts->desired.hasCurvePoint[idx] = true;
            opts->desired.curvePointMHz[idx] = (unsigned int)v;
        }
    }

    gc_cli_free_argv(argv);
    return true;
}
