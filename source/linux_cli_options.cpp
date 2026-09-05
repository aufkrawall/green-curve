// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_port_internal.h"

#include <stdio.h>
#include <string.h>

void print_linux_help() {
    puts("Green Curve for Linux (NVIDIA VF-curve / OC / UV / fan control)");
    puts("Usage:");
    puts("  greencurve                       Launch terminal UI");
    puts("  greencurve --tui                 Launch terminal UI");
    puts("  greencurve --dump | --json       Dump selected profile (text / JSON)");
    puts("  greencurve --dump-live | --json-live  Dump all live VF values from daemon");
    puts("  greencurve --probe [--probe-output path]   Probe driver + GPU control surfaces");
    puts("  greencurve --self-test           Read-only validation of the NvAPI/NVML apply path");
    puts("  greencurve --apply-config [--profile N]    Apply a profile via the daemon");
    puts("  greencurve --reset --apply-config          Reset OC/UV to driver defaults");
    puts("  greencurve --save-config [--profile N] [overrides]");
    puts("  greencurve --write-assets [--assets-dir path]");
    puts("  greencurve --config path --profile N");
    puts("  greencurve --gpu DDDD:BB:DD.F       Select and persist an exact PCI GPU");
    puts("Startup (what the daemon applies at boot):");
    puts("  greencurve --show-startup                 Show the current startup policy");
    puts("  greencurve --startup-profile N            Apply profile N at daemon start");
    puts("  greencurve --startup-profile last         Re-apply the last applied settings (default)");
    puts("  greencurve --startup-profile none         Apply nothing at daemon start");
    puts("Daemon (root):");
    puts("  sudo greencurve --service-install   Install + start the systemd daemon");
    puts("  sudo greencurve --service-remove    Stop + remove the daemon");
    puts("  greencurve --daemon                 Run the daemon in the foreground");
    puts("  greencurve --resume-restore         Re-apply the daemon's active settings after suspend");
    puts("                                      (run automatically by greencurve-resume.service)");
    puts("  Non-root clients need group access: sudo usermod -aG greencurve \"$USER\"");
    puts("  Then sign out/in, or run: newgrp greencurve");
    puts("Overrides:");
    puts("  --gpu-offset MHZ --mem-offset MHZ --power-limit PCT");
    puts("  --fan auto|PCT --fan-mode auto|fixed|curve --fan-fixed PCT");
    puts("  --fan-poll-ms MS --fan-hysteresis C (curve downshift, 0-10C)");
    puts("  --fan-zero-rpm 0|1");
    puts("  --fan-zero-rpm-hysteresis C (independent fan-off gap, 2-30C)");
    puts("  --fan-curve-enabledN 0|1 --fan-curve-tempN C --fan-curve-pctN PCT");
    puts("  --pointN MHZ for VF points 0-127");
    puts("");
    puts("Native NVIDIA control uses NvAPI (libnvidia-api.so.1) + NVML");
    puts("(libnvidia-ml.so.1); the root daemon applies settings, the client/TUI");
    puts("talk to it over /run/greencurve/greencurve.sock. Run --probe first.");
}

bool parse_linux_cli_options(int argc, char** argv, LinuxCliOptions* opts) {
    if (!opts) return false;
    memset(opts, 0, sizeof(*opts));
    initialize_desired_settings_defaults(&opts->desired);

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (!arg) continue;

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            opts->recognized = true;
            opts->showHelp = true;
        } else if (strcmp(arg, "--dump") == 0) {
            opts->recognized = true;
            opts->dump = true;
        } else if (strcmp(arg, "--json") == 0) {
            opts->recognized = true;
            opts->json = true;
        } else if (strcmp(arg, "--dump-live") == 0) {
            opts->recognized = true;
            opts->dumpLive = true;
        } else if (strcmp(arg, "--json-live") == 0) {
            opts->recognized = true;
            opts->jsonLive = true;
        } else if (strcmp(arg, "--probe") == 0) {
            opts->recognized = true;
            opts->probe = true;
        } else if (strcmp(arg, "--reset") == 0) {
            opts->recognized = true;
            opts->reset = true;
        } else if (strcmp(arg, "--save-config") == 0) {
            opts->recognized = true;
            opts->saveConfig = true;
        } else if (strcmp(arg, "--apply-config") == 0) {
            opts->recognized = true;
            opts->applyConfig = true;
        } else if (strcmp(arg, "--write-assets") == 0) {
            opts->recognized = true;
            opts->writeAssets = true;
        } else if (strcmp(arg, "--tui") == 0) {
            opts->recognized = true;
            opts->tui = true;
        } else if (strcmp(arg, "--daemon") == 0) {
            opts->recognized = true;
            opts->daemon = true;
        } else if (strcmp(arg, "--service-install") == 0) {
            opts->recognized = true;
            opts->serviceInstall = true;
        } else if (strcmp(arg, "--service-remove") == 0) {
            opts->recognized = true;
            opts->serviceRemove = true;
        } else if (strcmp(arg, "--resume-restore") == 0) {
            opts->recognized = true;
            opts->resumeRestore = true;
        } else if (strcmp(arg, "--self-test") == 0) {
            opts->recognized = true;
            opts->selfTest = true;
        } else if (strcmp(arg, "--from-desktop") == 0) {
            // Set by the terminal relaunch and by the generated .desktop
            // entries.  Deliberately does not set `recognized`: on its own it
            // still means "open the TUI".
            opts->fromDesktop = true;
        } else if (strcmp(arg, "--show-startup") == 0) {
            opts->recognized = true;
            opts->startupPolicyAction = LINUX_STARTUP_POLICY_ACTION_SHOW;
        } else if (strcmp(arg, "--startup-profile") == 0) {
            opts->recognized = true;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error),
                    "Missing --startup-profile value (none, last or 1-%d)",
                    CONFIG_NUM_SLOTS);
                return false;
            }
            const char* value = argv[++i];
            opts->startupPolicyAction = LINUX_STARTUP_POLICY_ACTION_SET;
            if (streqi_ascii(value, "none") || streqi_ascii(value, "off")) {
                opts->startupPolicyMode = SERVICE_STARTUP_POLICY_NONE;
            } else if (streqi_ascii(value, "last") ||
                       streqi_ascii(value, "restore-last")) {
                opts->startupPolicyMode = SERVICE_STARTUP_POLICY_RESTORE_LAST;
            } else if (parse_int_strict(value, &opts->startupPolicySlot) &&
                       opts->startupPolicySlot >= 1 &&
                       opts->startupPolicySlot <= CONFIG_NUM_SLOTS) {
                opts->startupPolicyMode = SERVICE_STARTUP_POLICY_PROFILE;
            } else {
                set_message(opts->error, sizeof(opts->error),
                    "Invalid --startup-profile value '%s'; use none, last or 1-%d",
                    value, CONFIG_NUM_SLOTS);
                return false;
            }
        } else if (strcmp(arg, "--config") == 0) {
            opts->recognized = true;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Missing --config path");
                return false;
            }
            snprintf(opts->configPath, sizeof(opts->configPath), "%s", argv[++i]);
            opts->hasConfigPath = true;
        } else if (strcmp(arg, "--probe-output") == 0) {
            opts->recognized = true;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Missing --probe-output path");
                return false;
            }
            snprintf(opts->probeOutputPath, sizeof(opts->probeOutputPath), "%s", argv[++i]);
            opts->hasProbeOutputPath = true;
        } else if (strcmp(arg, "--assets-dir") == 0) {
            opts->recognized = true;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Missing --assets-dir path");
                return false;
            }
            snprintf(opts->assetsDir, sizeof(opts->assetsDir), "%s", argv[++i]);
            opts->hasAssetsDir = true;
        } else if (strcmp(arg, "--profile") == 0) {
            opts->recognized = true;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --profile value");
                return false;
            }
            i++;
            if (!parse_int_strict(argv[i], &opts->profileSlot) || opts->profileSlot < 1 || opts->profileSlot > CONFIG_NUM_SLOTS) {
                set_message(opts->error, sizeof(opts->error), "Invalid --profile value");
                return false;
            }
            opts->hasProfileSlot = true;
        } else if (strcmp(arg, "--gpu") == 0) {
            opts->recognized = true;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error),
                            "Invalid --gpu value; expected DDDD:BB:DD.F PCI BDF");
                return false;
            }
            i++;
            if (!parse_linux_gpu_bdf(argv[i], &opts->gpuTarget)) {
                set_message(opts->error, sizeof(opts->error),
                            "Invalid --gpu value; expected DDDD:BB:DD.F PCI BDF");
                return false;
            }
            opts->hasGpuTarget = true;
        } else if (strcmp(arg, "--gpu-offset") == 0) {
            opts->recognized = true;
            int value = 0;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --gpu-offset value");
                return false;
            }
            i++;
            if (!parse_int_strict(argv[i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --gpu-offset value");
                return false;
            }
            opts->desired.hasGpuOffset = true;
            opts->desired.gpuOffsetMHz = value;
            if (value == 0) opts->desired.gpuOffsetExcludeLowCount = 0;
        } else if (strcmp(arg, "--gpu-offset-exclude-low-count") == 0) {
            opts->recognized = true;
            int countValue = 0;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --gpu-offset-exclude-low-count value");
                return false;
            }
            i++;
            if (!parse_int_strict(argv[i], &countValue) || countValue < 0) {
                set_message(opts->error, sizeof(opts->error), "Invalid --gpu-offset-exclude-low-count value");
                return false;
            }
            opts->desired.hasGpuOffset = true;
            opts->desired.gpuOffsetExcludeLowCount = countValue;
        } else if (strcmp(arg, "--gpu-offset-exclude-low-70") == 0) {
            opts->recognized = true;
            opts->desired.hasGpuOffset = true;
            opts->desired.gpuOffsetExcludeLowCount = 70;
        } else if (strcmp(arg, "--gpu-offset-include-low-70") == 0) {
            opts->recognized = true;
            opts->desired.hasGpuOffset = true;
            opts->desired.gpuOffsetExcludeLowCount = 0;
        } else if (strcmp(arg, "--mem-offset") == 0) {
            opts->recognized = true;
            int value = 0;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --mem-offset value");
                return false;
            }
            i++;
            if (!parse_int_strict(argv[i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --mem-offset value");
                return false;
            }
            opts->desired.hasMemOffset = true;
            opts->desired.memOffsetMHz = value;
        } else if (strcmp(arg, "--power-limit") == 0) {
            opts->recognized = true;
            int value = 0;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --power-limit value");
                return false;
            }
            i++;
            if (!parse_int_strict(argv[i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --power-limit value");
                return false;
            }
            // Reject rather than clamp: a silent clamp is how a requested 105%
            // used to become a stored 100%.
            if (value < POWER_LIMIT_MIN_PCT || value > POWER_LIMIT_MAX_PCT) {
                set_message(opts->error, sizeof(opts->error),
                    "--power-limit %d is outside the safe range %d..%d",
                    value, POWER_LIMIT_MIN_PCT, POWER_LIMIT_MAX_PCT);
                return false;
            }
            opts->desired.hasPowerLimit = true;
            opts->desired.powerLimitPct = value;
        } else if (strcmp(arg, "--fan") == 0) {
            opts->recognized = true;
            bool isAuto = false;
            int fanPercent = 0;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan value, use auto or 0-100");
                return false;
            }
            i++;
            if (!parse_fan_value(argv[i], &isAuto, &fanPercent)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan value, use auto or 0-100");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanAuto = isAuto;
            opts->desired.fanMode = isAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
            opts->desired.fanPercent = fanPercent;
            opts->fanOverrides.mode = true;
            opts->fanOverrides.fixedPercent = true;
        } else if (strcmp(arg, "--fan-mode") == 0) {
            opts->recognized = true;
            int fanMode = FAN_MODE_AUTO;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-mode value");
                return false;
            }
            i++;
            if (!parse_fan_mode_config_value(argv[i], &fanMode)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-mode value");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = fanMode;
            opts->desired.fanAuto = fanMode == FAN_MODE_AUTO;
            opts->fanOverrides.mode = true;
        } else if (strcmp(arg, "--fan-fixed") == 0) {
            opts->recognized = true;
            int value = 0;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-fixed value");
                return false;
            }
            i++;
            if (!parse_int_strict(argv[i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-fixed value");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = FAN_MODE_FIXED;
            opts->desired.fanAuto = false;
            opts->desired.fanPercent = clamp_percent(value);
            opts->fanOverrides.mode = true;
            opts->fanOverrides.fixedPercent = true;
        } else if (strcmp(arg, "--fan-poll-ms") == 0) {
            opts->recognized = true;
            int value = 0;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-poll-ms value");
                return false;
            }
            i++;
            if (!parse_int_strict(argv[i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-poll-ms value");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = FAN_MODE_CURVE;
            opts->desired.fanCurve.pollIntervalMs = value;
            opts->fanOverrides.mode = true;
            opts->fanOverrides.pollInterval = true;
        } else if (strcmp(arg, "--fan-hysteresis") == 0) {
            opts->recognized = true;
            int value = 0;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-hysteresis value");
                return false;
            }
            i++;
            if (!parse_int_strict(argv[i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-hysteresis value");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = FAN_MODE_CURVE;
            opts->desired.fanCurve.hysteresisC = value;
            opts->fanOverrides.mode = true;
            opts->fanOverrides.hysteresis = true;
        } else if (strcmp(arg, "--fan-zero-rpm-hysteresis") == 0) {
            opts->recognized = true;
            int value = 0;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error),
                    "Invalid --fan-zero-rpm-hysteresis value, use %d-%d",
                    FAN_ZERO_RPM_MIN_HYSTERESIS_C,
                    FAN_ZERO_RPM_MAX_HYSTERESIS_C);
                return false;
            }
            i++;
            if (!parse_int_strict(argv[i], &value) ||
                value < FAN_ZERO_RPM_MIN_HYSTERESIS_C ||
                value > FAN_ZERO_RPM_MAX_HYSTERESIS_C) {
                set_message(opts->error, sizeof(opts->error),
                    "Invalid --fan-zero-rpm-hysteresis value, use %d-%d",
                    FAN_ZERO_RPM_MIN_HYSTERESIS_C,
                    FAN_ZERO_RPM_MAX_HYSTERESIS_C);
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = FAN_MODE_CURVE;
            opts->desired.fanCurve.zeroRpmHysteresisC = (gc_u8)value;
            opts->fanOverrides.mode = true;
            opts->fanOverrides.zeroRpmHysteresis = true;
        } else if (strcmp(arg, "--fan-zero-rpm") == 0) {
            opts->recognized = true;
            int value = 0;
            if (!argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error),
                    "Invalid --fan-zero-rpm value, use 0 or 1");
                return false;
            }
            i++;
            if (!parse_int_strict(argv[i], &value) ||
                (value != 0 && value != 1)) {
                set_message(opts->error, sizeof(opts->error),
                    "Invalid --fan-zero-rpm value, use 0 or 1");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = FAN_MODE_CURVE;
            opts->desired.fanCurve.zeroRpmEnabled =
                gc_bool8_from_bool(value != 0);
            opts->fanOverrides.mode = true;
            opts->fanOverrides.zeroRpm = true;
        } else if (starts_with(arg, "--point")) {
            opts->recognized = true;
            int pointIndex = 0;
            if (!parse_int_strict(arg + 7, &pointIndex) || pointIndex < 0 || pointIndex >= VF_NUM_POINTS || !argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --pointN value");
                return false;
            }
            int value = 0;
            if (!parse_int_strict(argv[++i], &value) || value <= 0) {
                set_message(opts->error, sizeof(opts->error), "Invalid --pointN MHz value");
                return false;
            }
            opts->desired.hasCurvePoint[pointIndex] = true;
            opts->desired.curvePointMHz[pointIndex] = (unsigned int)value;
        } else if (starts_with(arg, "--fan-curve-temp")) {
            opts->recognized = true;
            int pointIndex = 0;
            if (!parse_int_strict(arg + strlen("--fan-curve-temp"), &pointIndex) || pointIndex < 0 || pointIndex >= FAN_CURVE_MAX_POINTS || !argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-curve-tempN value");
                return false;
            }
            int value = 0;
            if (!parse_int_strict(argv[++i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-curve-tempN value");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = FAN_MODE_CURVE;
            opts->desired.fanCurve.points[pointIndex].enabled = true;
            opts->desired.fanCurve.points[pointIndex].temperatureC = value;
            opts->fanOverrides.mode = true;
            opts->fanOverrides.pointEnabled[pointIndex] = true;
            opts->fanOverrides.pointTemperature[pointIndex] = true;
        } else if (starts_with(arg, "--fan-curve-pct")) {
            opts->recognized = true;
            int pointIndex = 0;
            if (!parse_int_strict(arg + strlen("--fan-curve-pct"), &pointIndex) || pointIndex < 0 || pointIndex >= FAN_CURVE_MAX_POINTS || !argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-curve-pctN value");
                return false;
            }
            int value = 0;
            if (!parse_int_strict(argv[++i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-curve-pctN value");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = FAN_MODE_CURVE;
            opts->desired.fanCurve.points[pointIndex].enabled = true;
            opts->desired.fanCurve.points[pointIndex].fanPercent = value;
            opts->fanOverrides.mode = true;
            opts->fanOverrides.pointEnabled[pointIndex] = true;
            opts->fanOverrides.pointPercent[pointIndex] = true;
        } else if (starts_with(arg, "--fan-curve-enabled")) {
            opts->recognized = true;
            int pointIndex = 0;
            if (!parse_int_strict(arg + strlen("--fan-curve-enabled"), &pointIndex) || pointIndex < 0 || pointIndex >= FAN_CURVE_MAX_POINTS || !argument_requires_value(argc, i)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-curve-enabledN value");
                return false;
            }
            int value = 0;
            if (!parse_int_strict(argv[++i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-curve-enabledN value");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = FAN_MODE_CURVE;
            opts->desired.fanCurve.points[pointIndex].enabled = value != 0;
            opts->fanOverrides.mode = true;
            opts->fanOverrides.pointEnabled[pointIndex] = true;
        } else {
            set_message(opts->error, sizeof(opts->error), "Unknown argument: %s", arg);
            return false;
        }
    }

    return true;
}

bool merge_linux_cli_desired_settings(DesiredSettings* base,
                                      const LinuxCliOptions* opts,
                                      char* err, size_t errSize) {
    if (!base || !opts) return false;
    DesiredSettings nonFanOverrides = opts->desired;
    nonFanOverrides.hasFan = false;
    merge_desired_settings(base, &nonFanOverrides);
    return apply_linux_cli_fan_overrides(base, &opts->desired,
                                         &opts->fanOverrides, err, errSize);
}
