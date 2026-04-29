// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_port_internal.h"

#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

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
            if (!argument_requires_value(argc, i) || !parse_int_strict(argv[++i], &opts->profileSlot) || opts->profileSlot < 1 || opts->profileSlot > CONFIG_NUM_SLOTS) {
                set_message(opts->error, sizeof(opts->error), "Invalid --profile value");
                return false;
            }
            opts->hasProfileSlot = true;
        } else if (strcmp(arg, "--gpu-offset") == 0) {
            opts->recognized = true;
            int value = 0;
            if (!argument_requires_value(argc, i) || !parse_int_strict(argv[++i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --gpu-offset value");
                return false;
            }
            opts->desired.hasGpuOffset = true;
            opts->desired.gpuOffsetMHz = value;
            if (value == 0) opts->desired.gpuOffsetExcludeLowCount = 0;
        } else if (strcmp(arg, "--gpu-offset-exclude-low-count") == 0) {
            opts->recognized = true;
            int countValue = 0;
            if (!argument_requires_value(argc, i) || !parse_int_strict(argv[++i], &countValue) || countValue < 0) {
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
            if (!argument_requires_value(argc, i) || !parse_int_strict(argv[++i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --mem-offset value");
                return false;
            }
            opts->desired.hasMemOffset = true;
            opts->desired.memOffsetMHz = value;
        } else if (strcmp(arg, "--power-limit") == 0) {
            opts->recognized = true;
            int value = 0;
            if (!argument_requires_value(argc, i) || !parse_int_strict(argv[++i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --power-limit value");
                return false;
            }
            opts->desired.hasPowerLimit = true;
            opts->desired.powerLimitPct = value;
        } else if (strcmp(arg, "--fan") == 0) {
            opts->recognized = true;
            bool isAuto = false;
            int fanPercent = 0;
            if (!argument_requires_value(argc, i) || !parse_fan_value(argv[++i], &isAuto, &fanPercent)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan value, use auto or 0-100");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanAuto = isAuto;
            opts->desired.fanMode = isAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
            opts->desired.fanPercent = fanPercent;
        } else if (strcmp(arg, "--fan-mode") == 0) {
            opts->recognized = true;
            int fanMode = FAN_MODE_AUTO;
            if (!argument_requires_value(argc, i) || !parse_fan_mode_config_value(argv[++i], &fanMode)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-mode value");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = fanMode;
            opts->desired.fanAuto = fanMode == FAN_MODE_AUTO;
        } else if (strcmp(arg, "--fan-fixed") == 0) {
            opts->recognized = true;
            int value = 0;
            if (!argument_requires_value(argc, i) || !parse_int_strict(argv[++i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-fixed value");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = FAN_MODE_FIXED;
            opts->desired.fanAuto = false;
            opts->desired.fanPercent = clamp_percent(value);
        } else if (strcmp(arg, "--fan-poll-ms") == 0) {
            opts->recognized = true;
            int value = 0;
            if (!argument_requires_value(argc, i) || !parse_int_strict(argv[++i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-poll-ms value");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = FAN_MODE_CURVE;
            opts->desired.fanCurve.pollIntervalMs = value;
        } else if (strcmp(arg, "--fan-hysteresis") == 0) {
            opts->recognized = true;
            int value = 0;
            if (!argument_requires_value(argc, i) || !parse_int_strict(argv[++i], &value)) {
                set_message(opts->error, sizeof(opts->error), "Invalid --fan-hysteresis value");
                return false;
            }
            opts->desired.hasFan = true;
            opts->desired.fanMode = FAN_MODE_CURVE;
            opts->desired.fanCurve.hysteresisC = value;
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
        } else {
            set_message(opts->error, sizeof(opts->error), "Unknown argument: %s", arg);
            return false;
        }
    }

    return true;
}

static bool command_available(const char* name) {
    const char* path = getenv("PATH");
    if (!name || !*name || !path) return false;
    std::string pathList(path);
    size_t start = 0;
    while (start <= pathList.size()) {
        size_t sep = pathList.find(':', start);
        std::string part = pathList.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        if (part.empty()) part = ".";
        std::string candidate = path_join(part, name);
        if (access(candidate.c_str(), X_OK) == 0) {
            struct stat st = {};
            if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return true;
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return false;
}

static bool read_small_file(const char* path, std::string* out, size_t maxBytes) {
    out->clear();
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    std::vector<char> buffer(maxBytes + 1u, 0);
    size_t readCount = fread(buffer.data(), 1, maxBytes, file);
    fclose(file);
    out->assign(buffer.data(), readCount);
    return true;
}

static std::string shell_quote_single(const char* text) {
    std::string out;
    out.push_back('\'');
    if (text) {
        for (const char* p = text; *p; ++p) {
            if (*p == '\'') out += "'\"'\"'";
            else out.push_back(*p);
        }
    }
    out.push_back('\'');
    return out;
}

static std::string capture_command_output(const char* command, size_t maxBytes) {
    std::string output;
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        appendf(&output, "command failed to start: %s\n", command);
        return output;
    }

    char buffer[512] = {};
    while (output.size() < maxBytes) {
        size_t toRead = sizeof(buffer);
        if (output.size() + toRead > maxBytes) toRead = maxBytes - output.size();
        size_t readCount = fread(buffer, 1, toRead, pipe);
        if (readCount > 0) output.append(buffer, readCount);
        if (readCount < toRead) break;
    }
    pclose(pipe);
    if (output.empty()) output = "<no output>\n";
    return output;
}

static void append_code_block(std::string* report, const char* title, const std::string& body) {
    appendf(report, "### %s\n\n```text\n", title);
    report->append(body);
    if (report->empty() || report->back() != '\n') report->push_back('\n');
    report->append("```\n\n");
}

static void append_path_state(std::string* report, const char* path) {
    struct stat st = {};
    if (stat(path, &st) == 0) {
        appendf(report, "- `%s`: present", path);
        if (S_ISDIR(st.st_mode)) report->append(" (directory)\n");
        else if (S_ISCHR(st.st_mode)) report->append(" (char device)\n");
        else if (S_ISREG(st.st_mode)) report->append(" (regular file)\n");
        else report->append("\n");
    } else {
        appendf(report, "- `%s`: missing\n", path);
    }
}

bool run_linux_probe(const char* outputPath, ProbeSummary* summary, char* err, size_t errSize) {
    if (summary) memset(summary, 0, sizeof(*summary));

    char resolvedOutput[LINUX_PATH_MAX] = {};
    if (outputPath && *outputPath) {
        snprintf(resolvedOutput, sizeof(resolvedOutput), "%s", outputPath);
    } else {
        default_probe_output_path(nullptr, resolvedOutput, sizeof(resolvedOutput));
    }

    std::string report;
    appendf(&report, "# Green Curve Linux Probe\n\n");
    appendf(&report, "Generated by %s %s on native Linux.\n\n", APP_NAME, APP_VERSION);

    const char* sessionType = getenv("XDG_SESSION_TYPE");
    const char* currentDesktop = getenv("XDG_CURRENT_DESKTOP");
    const char* waylandDisplay = getenv("WAYLAND_DISPLAY");
    const char* xDisplay = getenv("DISPLAY");
    bool isRoot = geteuid() == 0;
    bool hasWayland = (sessionType && streqi_ascii(sessionType, "wayland")) || (waylandDisplay && *waylandDisplay);

    if (summary) {
        summary->completed = true;
        summary->isRoot = isRoot;
        summary->hasWayland = hasWayland;
        summary->hasDisplay = xDisplay && *xDisplay;
        summary->hasNvidiaSmi = command_available("nvidia-smi");
        summary->hasSystemctl = command_available("systemctl");
        summary->hasSudo = command_available("sudo");
        summary->hasPkexec = command_available("pkexec");
        snprintf(summary->sessionType, sizeof(summary->sessionType), "%s", sessionType ? sessionType : "unknown");
        snprintf(summary->currentDesktop, sizeof(summary->currentDesktop), "%s", currentDesktop ? currentDesktop : "unknown");
        snprintf(summary->reportPath, sizeof(summary->reportPath), "%s", resolvedOutput);
        snprintf(summary->summary, sizeof(summary->summary), "root=%s, wayland=%s, nvidia-smi=%s, systemctl=%s",
            isRoot ? "yes" : "no",
            hasWayland ? "yes" : "no",
            summary->hasNvidiaSmi ? "yes" : "no",
            summary->hasSystemctl ? "yes" : "no");
    }

    appendf(&report, "## Session\n\n");
    appendf(&report, "- Effective UID: `%d`\n", (int)geteuid());
    appendf(&report, "- Root: `%s`\n", isRoot ? "yes" : "no");
    appendf(&report, "- XDG_SESSION_TYPE: `%s`\n", sessionType ? sessionType : "unset");
    appendf(&report, "- WAYLAND_DISPLAY: `%s`\n", waylandDisplay ? waylandDisplay : "unset");
    appendf(&report, "- DISPLAY: `%s`\n", xDisplay ? xDisplay : "unset");
    appendf(&report, "- XDG_CURRENT_DESKTOP: `%s`\n", currentDesktop ? currentDesktop : "unset");
    appendf(&report, "- TERM: `%s`\n\n", getenv("TERM") ? getenv("TERM") : "unset");

    appendf(&report, "## Tools\n\n");
    appendf(&report, "- `nvidia-smi`: `%s`\n", command_available("nvidia-smi") ? "yes" : "no");
    appendf(&report, "- `systemctl`: `%s`\n", command_available("systemctl") ? "yes" : "no");
    appendf(&report, "- `sudo`: `%s`\n", command_available("sudo") ? "yes" : "no");
    appendf(&report, "- `pkexec`: `%s`\n", command_available("pkexec") ? "yes" : "no");
    appendf(&report, "- `journalctl`: `%s`\n", command_available("journalctl") ? "yes" : "no");
    appendf(&report, "- `modinfo`: `%s`\n", command_available("modinfo") ? "yes" : "no");
    appendf(&report, "- `lspci`: `%s`\n\n", command_available("lspci") ? "yes" : "no");

    appendf(&report, "## Paths\n\n");
    append_path_state(&report, "/dev/nvidiactl");
    append_path_state(&report, "/dev/nvidia0");
    append_path_state(&report, "/dev/nvidia-uvm");
    append_path_state(&report, "/proc/driver/nvidia/version");
    append_path_state(&report, "/sys/module/nvidia/version");
    append_path_state(&report, "/sys/kernel/debug");
    append_path_state(&report, "/sys/class/drm");
    append_path_state(&report, "/sys/class/hwmon");
    report.push_back('\n');

    struct utsname systemName = {};
    if (uname(&systemName) == 0) {
        std::string unameText;
        appendf(&unameText, "%s %s %s %s %s\n", systemName.sysname, systemName.nodename, systemName.release, systemName.version, systemName.machine);
        append_code_block(&report, "uname -a", unameText);
    }

    append_code_block(&report, "id", capture_command_output("id", 4096));
    if (command_available("lsmod")) append_code_block(&report, "lsmod | grep nvidia", capture_command_output("lsmod | grep -i nvidia", 8192));
    if (command_available("lspci")) append_code_block(&report, "lspci | grep -i nvidia", capture_command_output("lspci -nn | grep -i nvidia", 8192));
    if (command_available("modinfo")) append_code_block(&report, "modinfo nvidia", capture_command_output("modinfo nvidia", 16384));
    if (command_available("nvidia-smi")) {
        append_code_block(&report, "nvidia-smi -L", capture_command_output("nvidia-smi -L", 8192));
        append_code_block(&report, "nvidia-smi -q -d POWER,CLOCK,FAN,PERFORMANCE", capture_command_output("nvidia-smi -q -d POWER,CLOCK,FAN,PERFORMANCE", 32768));
    }

    std::string procVersion;
    if (read_small_file("/proc/driver/nvidia/version", &procVersion, 4096)) {
        append_code_block(&report, "/proc/driver/nvidia/version", procVersion);
    }

    glob_t gpuInfo = {};
    if (glob("/proc/driver/nvidia/gpus/*/information", 0, nullptr, &gpuInfo) == 0) {
        for (size_t i = 0; i < gpuInfo.gl_pathc; i++) {
            std::string content;
            if (read_small_file(gpuInfo.gl_pathv[i], &content, 8192)) {
                append_code_block(&report, gpuInfo.gl_pathv[i], content);
            }
        }
    }
    globfree(&gpuInfo);

    appendf(&report, "## Library Candidates\n\n");
    const char* libraryPatterns[] = {
        "/usr/lib*/libnvidia-ml.so*",
        "/usr/lib*/nvidia*/libnvidia-ml.so*",
        "/usr/lib*/libXNVCtrl.so*",
        "/usr/lib*/nvidia*/libXNVCtrl.so*",
    };
    for (size_t patternIndex = 0; patternIndex < ARRAY_COUNT(libraryPatterns); patternIndex++) {
        glob_t libraries = {};
        if (glob(libraryPatterns[patternIndex], 0, nullptr, &libraries) == 0 && libraries.gl_pathc > 0) {
            appendf(&report, "- Pattern `%s`:\n", libraryPatterns[patternIndex]);
            for (size_t pathIndex = 0; pathIndex < libraries.gl_pathc && pathIndex < 8; pathIndex++) {
                appendf(&report, "  - `%s`\n", libraries.gl_pathv[pathIndex]);
            }
        }
        globfree(&libraries);
    }
    report.push_back('\n');

    appendf(&report, "## Next Questions\n\n");
    appendf(&report, "- Can NVML expose any VF-like clock/voltage control beyond the documented Linux surfaces?\n");
    appendf(&report, "- Do `nvidia-smi`, sysfs, debugfs, or another Linux-native control plane expose editable VF data?\n");
    appendf(&report, "- Which operations require root versus CAP_SYS_ADMIN versus a desktop auth helper?\n");
    appendf(&report, "- Does the target desktop launch this binary inside a visible terminal when `Terminal=true` is used?\n");

    if (!write_text_file_atomic(resolvedOutput, report, err, errSize)) return false;
    return true;
}

bool write_linux_assets(const char* outputDir, const char* execPath, const char* configPath, char* err, size_t errSize) {
    if (!outputDir || !*outputDir || !execPath || !*execPath || !configPath || !*configPath) {
        set_message(err, errSize, "Missing asset generation paths");
        return false;
    }
    if (!ensure_directory_recursive(outputDir, err, errSize)) return false;

    std::string execShell = shell_quote_single(execPath);
    std::string configShell = shell_quote_single(configPath);
    std::string desktopExec = "sh -lc \"exec " + execShell + " --tui --config " + configShell + "\"";
    std::string serviceExec = "/bin/sh -lc \"exec " + execShell + " --apply-config --config " + configShell + "\"";

    std::string desktop;
    appendf(&desktop,
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Version=1.0\n"
        "Name=%s\n"
        "Comment=Open the Linux terminal UI for %s\n"
        "Exec=%s\n"
        "Terminal=true\n"
        "Categories=Utility;System;\n"
        "StartupNotify=false\n",
        APP_NAME,
        APP_NAME,
        desktopExec.c_str());

    std::string autostart = desktop;
    autostart += "X-GNOME-Autostart-enabled=true\n";

    std::string service;
    appendf(&service,
        "[Unit]\n"
        "Description=%s Linux one-shot apply scaffold\n"
        "After=multi-user.target\n"
        "ConditionPathExists=%s\n\n"
        "[Service]\n"
        "Type=oneshot\n"
        "User=root\n"
        "ExecStart=%s\n"
        "StandardOutput=journal\n"
        "StandardError=journal\n\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n",
        APP_NAME,
        configPath,
        serviceExec.c_str());

    std::string readme;
    appendf(&readme,
        "# Linux Assets\n\n"
        "These files were generated by %s %s.\n\n"
        "- `greencurve.desktop`: visible launcher with `Terminal=true` for GNOME/KDE/Wayland sessions\n"
        "- `greencurve-autostart.desktop`: session autostart launcher that still opens a terminal window\n"
        "- `greencurve-apply.service`: root-owned systemd scaffold for future apply mode\n\n"
        "Install example:\n\n"
        "```bash\n"
        "install -Dm644 greencurve.desktop ~/.local/share/applications/greencurve.desktop\n"
        "install -Dm644 greencurve-autostart.desktop ~/.config/autostart/greencurve.desktop\n"
        "sudo install -Dm644 greencurve-apply.service /etc/systemd/system/greencurve-apply.service\n"
        "sudo systemctl daemon-reload\n"
        "sudo systemctl enable greencurve-apply.service\n"
        "```\n\n"
        "The systemd unit is scaffold-only until the Linux backend can perform verified VF-curve writes on real hardware.\n",
        APP_NAME,
        APP_VERSION);

    std::string desktopPath = path_join(outputDir, "greencurve.desktop");
    std::string autostartPath = path_join(outputDir, "greencurve-autostart.desktop");
    std::string servicePath = path_join(outputDir, "greencurve-apply.service");
    std::string readmePath = path_join(outputDir, "README.md");

    if (!write_text_file_atomic(desktopPath.c_str(), desktop, err, errSize)) return false;
    if (!write_text_file_atomic(autostartPath.c_str(), autostart, err, errSize)) return false;
    if (!write_text_file_atomic(servicePath.c_str(), service, err, errSize)) return false;
    if (!write_text_file_atomic(readmePath.c_str(), readme, err, errSize)) return false;
    return true;
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
