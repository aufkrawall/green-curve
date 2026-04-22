#include "linux_port.h"

#include <stdio.h>
#include <string.h>

static void print_help() {
    puts("Green Curve Linux scaffold");
    puts("Usage:");
    puts("  greencurve-x86_64-linux-musl            Launch terminal UI");
    puts("  greencurve-x86_64-linux-musl --tui      Launch terminal UI");
    puts("  greencurve-x86_64-linux-musl --dump     Dump selected profile as text");
    puts("  greencurve-x86_64-linux-musl --json     Dump selected profile as JSON");
    puts("  greencurve-x86_64-linux-musl --probe [--probe-output path]");
    puts("  greencurve-x86_64-linux-musl --write-assets [--assets-dir path]");
    puts("  greencurve-x86_64-linux-musl --save-config [--profile N] [overrides]");
    puts("  greencurve-x86_64-linux-musl --apply-config [--profile N]");
    puts("  greencurve-x86_64-linux-musl --config path --profile N");
    puts("Overrides:");
    puts("  --gpu-offset MHZ --mem-offset MHZ --power-limit PCT");
    puts("  --fan auto|PCT --fan-mode auto|fixed|curve --fan-fixed PCT");
    puts("  --fan-poll-ms MS --fan-hysteresis C");
    puts("  --fan-curve-enabledN 0|1 --fan-curve-tempN C --fan-curve-pctN PCT");
    puts("  --pointN MHZ for VF points 0-127");
    puts("");
    puts("Current Linux target status:");
    puts("  - TUI, config editing, probe collection, and autostart/systemd asset generation are implemented.");
    puts("  - Verified Linux VF-curve read/write parity is still pending native Linux hardware investigation.");
}

static void merge_desired_settings(DesiredSettings* base, const DesiredSettings* incoming) {
    if (!base || !incoming) return;
    if (incoming->hasGpuOffset) {
        base->hasGpuOffset = true;
        base->gpuOffsetMHz = incoming->gpuOffsetMHz;
        base->gpuOffsetExcludeLow70 = incoming->gpuOffsetExcludeLow70;
    }
    if (incoming->hasLock) {
        base->hasLock = true;
        base->lockCi = incoming->lockCi;
        base->lockMHz = incoming->lockMHz;
    }
    if (incoming->hasMemOffset) {
        base->hasMemOffset = true;
        base->memOffsetMHz = incoming->memOffsetMHz;
    }
    if (incoming->hasPowerLimit) {
        base->hasPowerLimit = true;
        base->powerLimitPct = incoming->powerLimitPct;
    }
    if (incoming->hasFan) {
        base->hasFan = true;
        base->fanAuto = incoming->fanAuto;
        base->fanMode = incoming->fanMode;
        base->fanPercent = incoming->fanPercent;
        base->fanCurve = incoming->fanCurve;
    }
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (incoming->hasCurvePoint[i]) {
            base->hasCurvePoint[i] = true;
            base->curvePointMHz[i] = incoming->curvePointMHz[i];
        }
    }
}

int main(int argc, char** argv) {
    LinuxCliOptions opts = {};
    if (!parse_linux_cli_options(argc, argv, &opts)) {
        fprintf(stderr, "%s\n", opts.error[0] ? opts.error : "Failed to parse CLI");
        return 1;
    }

    if (opts.showHelp) {
        print_help();
        return 0;
    }

    char configPath[LINUX_PATH_MAX] = {};
    if (opts.hasConfigPath) snprintf(configPath, sizeof(configPath), "%s", opts.configPath);
    else if (!default_linux_config_path(configPath, sizeof(configPath))) snprintf(configPath, sizeof(configPath), "%s", CONFIG_FILE_NAME);

    int slot = opts.hasProfileSlot ? opts.profileSlot : CONFIG_DEFAULT_SLOT;
    DesiredSettings desired = {};
    char err[256] = {};
    if (!load_default_or_selected_profile(configPath, &slot, &desired, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err[0] ? err : "Failed to load config");
        return 1;
    }

    if (opts.reset) {
        initialize_desired_settings_defaults(&desired);
    }
    merge_desired_settings(&desired, &opts.desired);
    normalize_desired_settings_for_ui(&desired);

    if (opts.probe) {
        char outputPath[LINUX_PATH_MAX] = {};
        if (opts.hasProbeOutputPath) snprintf(outputPath, sizeof(outputPath), "%s", opts.probeOutputPath);
        else default_probe_output_path(configPath, outputPath, sizeof(outputPath));
        ProbeSummary summary = {};
        if (!run_linux_probe(outputPath, &summary, err, sizeof(err))) {
            fprintf(stderr, "%s\n", err[0] ? err : "Probe failed");
            return 1;
        }
        printf("Probe written to %s\n", outputPath);
        printf("%s\n", summary.summary);
    }

    if (opts.writeAssets) {
        char outputDir[LINUX_PATH_MAX] = {};
        char exePath[LINUX_PATH_MAX] = {};
        if (opts.hasAssetsDir) snprintf(outputDir, sizeof(outputDir), "%s", opts.assetsDir);
        else default_assets_output_dir(configPath, outputDir, sizeof(outputDir));
        if (!get_executable_path(exePath, sizeof(exePath))) {
            fprintf(stderr, "Failed to resolve /proc/self/exe\n");
            return 1;
        }
        if (!write_linux_assets(outputDir, exePath, configPath, err, sizeof(err))) {
            fprintf(stderr, "%s\n", err[0] ? err : "Asset generation failed");
            return 1;
        }
        printf("Linux assets written to %s\n", outputDir);
    }

    if (opts.saveConfig) {
        int targetSlot = opts.hasProfileSlot ? opts.profileSlot : slot;
        if (!save_profile_to_config_path(configPath, targetSlot, &desired, err, sizeof(err))) {
            fprintf(stderr, "%s\n", err[0] ? err : "Profile save failed");
            return 1;
        }
        printf("Profile %d written to %s\n", targetSlot, configPath);
    }

    if (opts.dump) {
        print_desired_settings_text(stdout, slot, &desired);
    }

    if (opts.json) {
        print_desired_settings_json(stdout, slot, &desired);
    }

    if (opts.applyConfig) {
        fprintf(stderr,
            "Linux apply-config is scaffolded but intentionally blocked until native Linux VF-curve parity is proven.\n"
            "Run --probe on native Linux, inspect %s, and continue backend work there.\n",
            APP_LINUX_PROBE_FILE);
        return 3;
    }

    if (opts.showHelp || opts.dump || opts.json || opts.probe || opts.writeAssets || opts.saveConfig || opts.applyConfig) {
        return 0;
    }

    return linux_run_tui(configPath, slot, &desired);
}
