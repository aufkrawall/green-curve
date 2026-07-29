// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_PORT_H
#define GREEN_CURVE_LINUX_PORT_H

#include <stddef.h>
#include <stdio.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define APP_NAME "Green Curve"
#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif
#define CONFIG_FILE_NAME "config.ini"
#define APP_LINUX_PROBE_FILE "greencurve_linux_probe.md"
#define APP_LINUX_ASSETS_DIR "linux-artifacts"

#define LINUX_PATH_MAX 4096

// Shared GPU/IPC data model: VF_NUM_POINTS, FAN_CURVE_MAX_*, CONFIG_NUM_SLOTS,
// CONFIG_DEFAULT_SLOT, FAN_MODE_*, FanCurvePoint, FanCurveConfig, LockMode and
// DesiredSettings all come from gpu_core.h so the Linux client and the daemon
// share one definition (and the wire protocol struct).
#include "gpu_core.h"
// initialize_desired_settings_defaults() / normalize_desired_settings_for_ui():
// pure, header-only, and covered by the regression harness.
#include "desired_settings_ui_policy.h"

// What `--startup-profile` / `--show-startup` asked the client to do with the
// daemon's boot-apply policy.  The policy modes themselves are the shared
// SERVICE_STARTUP_POLICY_* values, because the daemon owns the record.
enum LinuxStartupPolicyAction {
    LINUX_STARTUP_POLICY_ACTION_NONE = 0,  // not requested on this command line
    LINUX_STARTUP_POLICY_ACTION_SHOW = 1,
    LINUX_STARTUP_POLICY_ACTION_SET = 2,
};

struct LinuxCliOptions {
    bool recognized;
    bool showHelp;
    // Set by the terminal relaunch (linux_terminal_launch.cpp) and by the
    // generated .desktop entries.  Guards against relaunching forever and makes
    // an error hold the window open instead of closing it unread.
    bool fromDesktop;
    int startupPolicyAction;  // LinuxStartupPolicyAction
    int startupPolicyMode;    // ServiceStartupPolicyMode when action == SET
    int startupPolicySlot;    // 1..CONFIG_NUM_SLOTS when mode == PROFILE
    bool dump;
    bool json;
    bool dumpLive;
    bool jsonLive;
    bool probe;
    bool reset;
    bool saveConfig;
    bool applyConfig;
    bool writeAssets;
    bool tui;
    bool daemon;
    bool serviceInstall;
    bool serviceRemove;
    bool selfTest;
    bool hasConfigPath;
    bool hasProbeOutputPath;
    bool hasAssetsDir;
    bool hasProfileSlot;
    bool hasGpuTarget;
    int profileSlot;
    GpuAdapterInfo gpuTarget;
    char configPath[LINUX_PATH_MAX];
    char probeOutputPath[LINUX_PATH_MAX];
    char assetsDir[LINUX_PATH_MAX];
    char error[256];
    DesiredSettings desired;
};

struct ProbeSummary {
    bool completed;
    bool isRoot;
    bool hasWayland;
    bool hasDisplay;
    bool hasNvidiaSmi;
    bool hasSystemctl;
    bool hasSudo;
    bool hasPkexec;
    char sessionType[32];
    char currentDesktop[128];
    char reportPath[LINUX_PATH_MAX];
    char summary[256];
};

void trim_ascii(char* s);
bool streqi_ascii(const char* a, const char* b);
bool parse_int_strict(const char* s, int* out);
void set_message(char* dst, size_t dstSize, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
bool parse_fan_value(const char* text, bool* isAuto, int* pct);
const char* fan_mode_label(int mode);
// The fan-curve math is declared by fan_curve.h and defined once in
// fan_curve.cpp, which the Linux binary now links.  Redeclaring it here is what
// let a diverged private copy live in linux_port.cpp unnoticed.
#include "fan_curve.h"
bool desired_has_any_action(const DesiredSettings* desired);
bool get_executable_path(char* dst, size_t dstSize);
bool default_linux_config_path(char* dst, size_t dstSize);
bool default_probe_output_path(const char* configPath, char* dst, size_t dstSize);
bool default_assets_output_dir(const char* configPath, char* dst, size_t dstSize);
void merge_desired_settings(DesiredSettings* base, const DesiredSettings* incoming);
bool parse_linux_cli_options(int argc, char** argv, LinuxCliOptions* opts);
void print_linux_help();
bool load_profile_from_config_path(const char* path, int slot, DesiredSettings* desired, char* err, size_t errSize);
bool load_default_or_selected_profile(const char* path, int* slot, DesiredSettings* desired, char* err, size_t errSize);
bool save_profile_to_config_path(const char* path, int slot, const DesiredSettings* desired, char* err, size_t errSize);
bool clear_profile_from_config_path(const char* path, int slot,
                                    char* err, size_t errSize);
// `[debug] enabled` from config.ini; returns `defaultValue` when the file or
// the key is absent (the config is optional on Linux).
int load_linux_debug_enabled(const char* configPath, int defaultValue);
bool parse_linux_gpu_bdf(const char* text, GpuAdapterInfo* target);
void format_linux_gpu_bdf(const GpuAdapterInfo* target, char* text, size_t textSize);
bool load_linux_gpu_selection(const char* path, GpuAdapterInfo* target);
bool save_linux_gpu_selection(const char* path, const GpuAdapterInfo* target,
                              char* err, size_t errSize);
bool run_linux_probe(const char* outputPath, ProbeSummary* summary, char* err, size_t errSize);
bool write_linux_assets(const char* outputDir, const char* execPath, const char* configPath, char* err, size_t errSize);
void print_desired_settings_text(FILE* out, int slot, const DesiredSettings* desired);
void print_desired_settings_json(FILE* out, int slot, const DesiredSettings* desired);
void print_linux_live_state_text(FILE* out, const ServiceResponse* response);
void print_linux_live_state_json(FILE* out, const ServiceResponse* response);
int linux_run_tui(const char* configPath, int initialSlot, DesiredSettings* initialDesired,
                  const GpuAdapterInfo* initialTarget);

#endif
