// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_PORT_H
#define GREEN_CURVE_LINUX_PORT_H

#include <stddef.h>
#include <stdio.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define APP_NAME "Green Curve"
#ifndef APP_VERSION
#define APP_VERSION "0.12"
#endif
#define CONFIG_FILE_NAME "config.ini"
#define APP_LINUX_PROBE_FILE "greencurve_linux_probe.md"
#define APP_LINUX_ASSETS_DIR "linux-artifacts"

#define VF_NUM_POINTS 128
#define FAN_CURVE_MAX_POINTS 8
#define FAN_CURVE_MAX_HYSTERESIS_C 10
#define CONFIG_NUM_SLOTS 5
#define CONFIG_DEFAULT_SLOT 1
#define LINUX_PATH_MAX 4096

enum {
    FAN_MODE_AUTO = 0,
    FAN_MODE_FIXED = 1,
    FAN_MODE_CURVE = 2,
};

struct FanCurvePoint {
    bool enabled;
    int temperatureC;
    int fanPercent;
};

struct FanCurveConfig {
    int pollIntervalMs;
    int hysteresisC;
    FanCurvePoint points[FAN_CURVE_MAX_POINTS];
};

struct DesiredSettings {
    bool hasCurvePoint[VF_NUM_POINTS];
    unsigned int curvePointMHz[VF_NUM_POINTS];
    bool hasLock;
    int lockCi;
    unsigned int lockMHz;
    bool lockTracksAnchor;
    bool hasGpuOffset;
    int gpuOffsetMHz;
    int gpuOffsetExcludeLowCount;
    bool hasMemOffset;
    int memOffsetMHz;
    bool hasPowerLimit;
    int powerLimitPct;
    bool hasFan;
    bool fanAuto;
    int fanMode;
    int fanPercent;
    FanCurveConfig fanCurve;
};

struct LinuxCliOptions {
    bool recognized;
    bool showHelp;
    bool dump;
    bool json;
    bool probe;
    bool reset;
    bool saveConfig;
    bool applyConfig;
    bool writeAssets;
    bool tui;
    bool hasConfigPath;
    bool hasProbeOutputPath;
    bool hasAssetsDir;
    bool hasProfileSlot;
    int profileSlot;
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
void fan_curve_set_default(FanCurveConfig* config);
void fan_curve_normalize(FanCurveConfig* config);
bool fan_curve_validate(const FanCurveConfig* config, char* err, size_t errSize);
void fan_curve_format_summary(const FanCurveConfig* config, char* buffer, size_t bufferSize);
void initialize_desired_settings_defaults(DesiredSettings* desired);
void normalize_desired_settings_for_ui(DesiredSettings* desired);
bool desired_has_any_action(const DesiredSettings* desired);
bool get_executable_path(char* dst, size_t dstSize);
bool default_linux_config_path(char* dst, size_t dstSize);
bool default_probe_output_path(const char* configPath, char* dst, size_t dstSize);
bool default_assets_output_dir(const char* configPath, char* dst, size_t dstSize);
void merge_desired_settings(DesiredSettings* base, const DesiredSettings* incoming);
bool parse_linux_cli_options(int argc, char** argv, LinuxCliOptions* opts);
bool load_profile_from_config_path(const char* path, int slot, DesiredSettings* desired, char* err, size_t errSize);
bool load_default_or_selected_profile(const char* path, int* slot, DesiredSettings* desired, char* err, size_t errSize);
bool save_profile_to_config_path(const char* path, int slot, const DesiredSettings* desired, char* err, size_t errSize);
bool run_linux_probe(const char* outputPath, ProbeSummary* summary, char* err, size_t errSize);
bool write_linux_assets(const char* outputDir, const char* execPath, const char* configPath, char* err, size_t errSize);
void print_desired_settings_text(FILE* out, int slot, const DesiredSettings* desired);
void print_desired_settings_json(FILE* out, int slot, const DesiredSettings* desired);
int linux_run_tui(const char* configPath, int initialSlot, DesiredSettings* initialDesired);

#endif
