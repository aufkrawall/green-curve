// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_port_internal.h"

#include <string>
#include <vector>

void merge_desired_settings(DesiredSettings* base, const DesiredSettings* incoming) {
    if (!base || !incoming) return;
    if (incoming->hasGpuOffset) {
        base->hasGpuOffset = true;
        base->gpuOffsetMHz = incoming->gpuOffsetMHz;
        base->gpuOffsetExcludeLowCount = incoming->gpuOffsetExcludeLowCount;
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

bool default_probe_output_path(const char* configPath, char* dst, size_t dstSize) {
    if (!dst || dstSize == 0) return false;
    std::string baseDir;
    if (configPath && *configPath) {
        baseDir = path_dirname(configPath);
    } else {
        char config[LINUX_PATH_MAX] = {};
        if (default_linux_config_path(config, sizeof(config))) baseDir = path_dirname(config);
    }
    if (baseDir.empty()) baseDir = ".";
    std::string output = path_join(baseDir, APP_LINUX_PROBE_FILE);
    snprintf(dst, dstSize, "%s", output.c_str());
    dst[dstSize - 1] = 0;
    return true;
}

bool default_assets_output_dir(const char* configPath, char* dst, size_t dstSize) {
    if (!dst || dstSize == 0) return false;
    std::string baseDir;
    if (configPath && *configPath) {
        baseDir = path_dirname(configPath);
    } else {
        char config[LINUX_PATH_MAX] = {};
        if (default_linux_config_path(config, sizeof(config))) baseDir = path_dirname(config);
    }
    if (baseDir.empty()) baseDir = ".";
    std::string output = path_join(baseDir, APP_LINUX_ASSETS_DIR);
    snprintf(dst, dstSize, "%s", output.c_str());
    dst[dstSize - 1] = 0;
    return true;
}

static void set_desired_fan_from_legacy_value(DesiredSettings* desired, bool fanAuto, int fanPercent) {
    if (!desired) return;
    desired->hasFan = true;
    desired->fanAuto = fanAuto;
    desired->fanMode = fanAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
    desired->fanPercent = fanPercent;
}

static int gpu_offset_component_mhz_for_point_linux(int pointIndex, int gpuOffsetMHz, int excludeLowCount) {
    if (gpuOffsetMHz == 0) return 0;
    if (excludeLowCount <= 0) return gpuOffsetMHz;
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return gpuOffsetMHz;
    return pointIndex < excludeLowCount ? 0 : gpuOffsetMHz;
}

static bool load_fan_curve_config_from_section(const IniDocument* doc, const char* section, FanCurveConfig* curve, char* err, size_t errSize) {
    if (!doc || !section || !curve) return false;
    if (!section_has_keys(doc, section)) return true;

    std::string value = get_section_value(doc, section, "poll_interval_ms");
    if (!value.empty() && !parse_int_strict(value.c_str(), &curve->pollIntervalMs)) {
        set_message(err, errSize, "Invalid fan curve poll interval in %s", section);
        return false;
    }

    value = get_section_value(doc, section, "hysteresis_c");
    if (!value.empty() && !parse_int_strict(value.c_str(), &curve->hysteresisC)) {
        set_message(err, errSize, "Invalid fan curve hysteresis in %s", section);
        return false;
    }

    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        char key[32] = {};
        snprintf(key, sizeof(key), "enabled%d", i);
        value = get_section_value(doc, section, key);
        if (!value.empty()) {
            int parsed = 0;
            if (!parse_int_strict(value.c_str(), &parsed)) {
                set_message(err, errSize, "Invalid fan curve enabled flag in %s", section);
                return false;
            }
            curve->points[i].enabled = parsed != 0;
        }

        snprintf(key, sizeof(key), "temp%d", i);
        value = get_section_value(doc, section, key);
        if (!value.empty() && !parse_int_strict(value.c_str(), &curve->points[i].temperatureC)) {
            set_message(err, errSize, "Invalid fan curve temperature in %s", section);
            return false;
        }

        snprintf(key, sizeof(key), "pct%d", i);
        value = get_section_value(doc, section, key);
        if (!value.empty() && !parse_int_strict(value.c_str(), &curve->points[i].fanPercent)) {
            set_message(err, errSize, "Invalid fan curve percentage in %s", section);
            return false;
        }
    }

    fan_curve_normalize(curve);
    return fan_curve_validate(curve, err, errSize);
}

static void save_fan_curve_section(IniDocument* doc, const char* sectionName, const FanCurveConfig* curve) {
    std::vector<IniEntry> entries;
    char key[32] = {};
    char value[32] = {};

    IniEntry pollEntry;
    pollEntry.key = "poll_interval_ms";
    snprintf(value, sizeof(value), "%d", curve->pollIntervalMs);
    pollEntry.value = value;
    entries.push_back(pollEntry);

    IniEntry hystEntry;
    hystEntry.key = "hysteresis_c";
    snprintf(value, sizeof(value), "%d", curve->hysteresisC);
    hystEntry.value = value;
    entries.push_back(hystEntry);

    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        IniEntry enabledEntry;
        snprintf(key, sizeof(key), "enabled%d", i);
        enabledEntry.key = key;
        snprintf(value, sizeof(value), "%d", curve->points[i].enabled ? 1 : 0);
        enabledEntry.value = value;
        entries.push_back(enabledEntry);

        IniEntry tempEntry;
        snprintf(key, sizeof(key), "temp%d", i);
        tempEntry.key = key;
        snprintf(value, sizeof(value), "%d", curve->points[i].temperatureC);
        tempEntry.value = value;
        entries.push_back(tempEntry);

        IniEntry pctEntry;
        snprintf(key, sizeof(key), "pct%d", i);
        pctEntry.key = key;
        snprintf(value, sizeof(value), "%d", curve->points[i].fanPercent);
        pctEntry.value = value;
        entries.push_back(pctEntry);
    }

    replace_section(doc, sectionName, entries);
}

static bool load_desired_settings_from_sections(const IniDocument* doc,
    const char* controlsSection,
    const char* curveSection,
    const char* fanCurveSection,
    DesiredSettings* desired,
    const char* contextLabel,
    char* err,
    size_t errSize) {
    if (!doc || !controlsSection || !curveSection || !fanCurveSection || !desired) return false;

    initialize_desired_settings_defaults(desired);
    char fanBuffer[64] = {};
    char messageContext[128] = {};
    snprintf(messageContext, sizeof(messageContext), "%s", contextLabel ? contextLabel : "config");

    bool hasExplicitFanMode = false;

    std::string value = get_section_value(doc, controlsSection, "gpu_offset_mhz");
    if (!value.empty()) {
        if (!parse_int_strict(value.c_str(), &desired->gpuOffsetMHz)) {
            set_message(err, errSize, "Invalid gpu_offset_mhz in %s", messageContext);
            return false;
        }
        desired->hasGpuOffset = true;
    }

    value = get_section_value(doc, controlsSection, "gpu_offset_exclude_low_count");
    if (value.empty())
        value = get_section_value(doc, controlsSection, "gpu_offset_exclude_low_70");
    if (!value.empty()) {
        int parsed = 0;
        if (!parse_int_strict(value.c_str(), &parsed)) {
            set_message(err, errSize, "Invalid gpu_offset_exclude_low_count in %s", messageContext);
            return false;
        }
        if (!get_section_value(doc, controlsSection, "gpu_offset_exclude_low_count").empty())
            desired->gpuOffsetExcludeLowCount = parsed;
        else
            desired->gpuOffsetExcludeLowCount = parsed != 0 ? 70 : 0;
    }

    value = get_section_value(doc, controlsSection, "lock_ci");
    if (!value.empty()) {
        int parsed = -1;
        if (!parse_int_strict(value.c_str(), &parsed)) {
            set_message(err, errSize, "Invalid lock_ci in %s", messageContext);
            return false;
        }
        desired->hasLock = parsed >= 0;
        desired->lockCi = parsed;
    }

    value = get_section_value(doc, controlsSection, "lock_mhz");
    if (!value.empty()) {
        int parsed = 0;
        if (!parse_int_strict(value.c_str(), &parsed) || parsed < 0) {
            set_message(err, errSize, "Invalid lock_mhz in %s", messageContext);
            return false;
        }
        if (parsed > 0) {
            desired->hasLock = true;
            desired->lockMHz = (unsigned int)parsed;
        }
    }

    value = get_section_value(doc, controlsSection, "mem_offset_mhz");
    if (!value.empty()) {
        if (!parse_int_strict(value.c_str(), &desired->memOffsetMHz)) {
            set_message(err, errSize, "Invalid mem_offset_mhz in %s", messageContext);
            return false;
        }
        desired->hasMemOffset = true;
    }

    value = get_section_value(doc, controlsSection, "power_limit_pct");
    if (!value.empty()) {
        if (!parse_int_strict(value.c_str(), &desired->powerLimitPct)) {
            set_message(err, errSize, "Invalid power_limit_pct in %s", messageContext);
            return false;
        }
        desired->hasPowerLimit = true;
    }

    value = get_section_value(doc, controlsSection, "fan_mode");
    if (!value.empty()) {
        if (!parse_fan_mode_config_value(value.c_str(), &desired->fanMode)) {
            set_message(err, errSize, "Invalid fan_mode in %s", messageContext);
            return false;
        }
        desired->hasFan = true;
        desired->fanAuto = desired->fanMode == FAN_MODE_AUTO;
        hasExplicitFanMode = true;
    }

    value = get_section_value(doc, controlsSection, "fan");
    if (!value.empty()) {
        bool fanAuto = false;
        int fanPercent = 0;
        snprintf(fanBuffer, sizeof(fanBuffer), "%s", value.c_str());
        if (!parse_fan_value(fanBuffer, &fanAuto, &fanPercent)) {
            set_message(err, errSize, "Invalid fan setting in %s", messageContext);
            return false;
        }
        if (!hasExplicitFanMode) {
            set_desired_fan_from_legacy_value(desired, fanAuto, fanPercent);
        } else if (desired->fanMode == FAN_MODE_FIXED && !fanAuto) {
            desired->hasFan = true;
            desired->fanAuto = false;
            desired->fanPercent = clamp_percent(fanPercent);
        }
    }

    value = get_section_value(doc, controlsSection, "fan_fixed_pct");
    if (!value.empty()) {
        int parsed = 0;
        if (!parse_int_strict(value.c_str(), &parsed)) {
            set_message(err, errSize, "Invalid fan_fixed_pct in %s", messageContext);
            return false;
        }
        if (!hasExplicitFanMode || desired->fanMode == FAN_MODE_FIXED) {
            desired->hasFan = true;
            desired->fanMode = FAN_MODE_FIXED;
            desired->fanAuto = false;
            desired->fanPercent = clamp_percent(parsed);
        }
    }

    if (!load_fan_curve_config_from_section(doc, fanCurveSection, &desired->fanCurve, err, errSize)) return false;

    std::string curveSemantics = get_section_value(doc, curveSection, "curve_semantics");
    bool legacyCurveSemantics = curveSemantics.empty();

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        char key[32] = {};
        snprintf(key, sizeof(key), "point%d", i);
        value = get_section_value(doc, curveSection, key);
        if (value.empty()) continue;
        int parsed = 0;
        if (!parse_int_strict(value.c_str(), &parsed) || parsed <= 0) {
            set_message(err, errSize, "Invalid curve point %d in %s", i, messageContext);
            return false;
        }
        desired->hasCurvePoint[i] = true;
        desired->curvePointMHz[i] = (unsigned int)parsed;
    }

    bool basePlusGpuOffsetCurve = streqi_ascii(curveSemantics.c_str(), "base_plus_gpu_offset");
    if (basePlusGpuOffsetCurve && desired->hasGpuOffset && desired->gpuOffsetMHz != 0) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (!desired->hasCurvePoint[i]) continue;
            int offsetCompMHz = gpu_offset_component_mhz_for_point_linux(i, desired->gpuOffsetMHz, desired->gpuOffsetExcludeLowCount);
            int absoluteMHz = (int)desired->curvePointMHz[i] + offsetCompMHz;
            if (absoluteMHz <= 0) {
                desired->hasCurvePoint[i] = false;
                desired->curvePointMHz[i] = 0;
                continue;
            }
            desired->curvePointMHz[i] = (unsigned int)absoluteMHz;
        }
    } else if (legacyCurveSemantics && desired->hasGpuOffset && desired->gpuOffsetMHz != 0) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            desired->hasCurvePoint[i] = false;
            desired->curvePointMHz[i] = 0;
        }
    }

    for (int i = 1; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i] && desired->hasCurvePoint[i - 1] && desired->curvePointMHz[i] < desired->curvePointMHz[i - 1]) {
            desired->curvePointMHz[i] = desired->curvePointMHz[i - 1];
        }
    }

    return true;
}

static int get_selected_profile_slot(const IniDocument* doc) {
    int slot = get_section_int(doc, "profiles", "selected_slot", CONFIG_DEFAULT_SLOT);
    if (slot < 1 || slot > CONFIG_NUM_SLOTS) slot = CONFIG_DEFAULT_SLOT;
    return slot;
}

bool load_profile_from_config_path(const char* path, int slot, DesiredSettings* desired, char* err, size_t errSize) {
    if (!desired || slot < 1 || slot > CONFIG_NUM_SLOTS) {
        set_message(err, errSize, "Invalid profile load arguments");
        return false;
    }

    IniDocument doc;
    if (!load_ini_document(path, &doc, err, errSize)) return false;

    char controlsSection[32] = {};
    char curveSection[32] = {};
    char fanCurveSection[32] = {};
    snprintf(controlsSection, sizeof(controlsSection), "profile%d", slot);
    snprintf(curveSection, sizeof(curveSection), "profile%d_curve", slot);
    snprintf(fanCurveSection, sizeof(fanCurveSection), "profile%d_fan_curve", slot);

    bool hasSlotSections = section_has_keys(&doc, controlsSection) || section_has_keys(&doc, curveSection) || section_has_keys(&doc, fanCurveSection);
    if (!hasSlotSections && slot == 1) {
        if (section_has_keys(&doc, "controls") || section_has_keys(&doc, "curve") || section_has_keys(&doc, "fan_curve")) {
            snprintf(controlsSection, sizeof(controlsSection), "controls");
            snprintf(curveSection, sizeof(curveSection), "curve");
            snprintf(fanCurveSection, sizeof(fanCurveSection), "fan_curve");
        } else {
            set_message(err, errSize, "Profile %d is empty", slot);
            return false;
        }
    } else if (!hasSlotSections) {
        set_message(err, errSize, "Profile %d is empty", slot);
        return false;
    }

    char context[32] = {};
    snprintf(context, sizeof(context), "profile %d", slot);
    if (!load_desired_settings_from_sections(&doc, controlsSection, curveSection, fanCurveSection, desired, context, err, errSize)) return false;
    normalize_desired_settings_for_ui(desired);
    return true;
}

static bool load_legacy_config_path(const char* path, DesiredSettings* desired, char* err, size_t errSize) {
    IniDocument doc;
    if (!load_ini_document(path, &doc, err, errSize)) return false;
    if (!load_desired_settings_from_sections(&doc, "controls", "curve", "fan_curve", desired, path, err, errSize)) return false;
    normalize_desired_settings_for_ui(desired);
    return true;
}

bool load_default_or_selected_profile(const char* path, int* slot, DesiredSettings* desired, char* err, size_t errSize) {
    if (!desired) return false;

    IniDocument doc;
    if (!load_ini_document(path, &doc, err, errSize)) return false;

    int selectedSlot = slot && *slot >= 1 && *slot <= CONFIG_NUM_SLOTS ? *slot : get_selected_profile_slot(&doc);

    char controlsSection[32] = {};
    char curveSection[32] = {};
    char fanCurveSection[32] = {};
    snprintf(controlsSection, sizeof(controlsSection), "profile%d", selectedSlot);
    snprintf(curveSection, sizeof(curveSection), "profile%d_curve", selectedSlot);
    snprintf(fanCurveSection, sizeof(fanCurveSection), "profile%d_fan_curve", selectedSlot);

    bool hasSelectedSections = section_has_keys(&doc, controlsSection) || section_has_keys(&doc, curveSection) || section_has_keys(&doc, fanCurveSection);
    if (hasSelectedSections) {
        char context[32] = {};
        snprintf(context, sizeof(context), "profile %d", selectedSlot);
        if (!load_desired_settings_from_sections(&doc, controlsSection, curveSection, fanCurveSection, desired, context, err, errSize)) return false;
        normalize_desired_settings_for_ui(desired);
        if (slot) *slot = selectedSlot;
        return true;
    }

    bool hasLegacySections = section_has_keys(&doc, "controls") || section_has_keys(&doc, "curve") || section_has_keys(&doc, "fan_curve");
    if (hasLegacySections) {
        if (!load_desired_settings_from_sections(&doc, "controls", "curve", "fan_curve", desired, path, err, errSize)) return false;
        normalize_desired_settings_for_ui(desired);
        if (slot) *slot = CONFIG_DEFAULT_SLOT;
        return true;
    }

    initialize_desired_settings_defaults(desired);
    normalize_desired_settings_for_ui(desired);
    if (slot) *slot = CONFIG_DEFAULT_SLOT;
    return true;
}

static void write_profile_sections(IniDocument* doc, const char* controlsSection, const char* curveSection, const char* fanCurveSection, const DesiredSettings* desired) {
    std::vector<IniEntry> controlsEntries;
    std::vector<IniEntry> curveEntries;

    auto addControl = [&](const char* key, const char* value) {
        IniEntry entry;
        entry.key = key;
        entry.value = value;
        controlsEntries.push_back(entry);
    };

    char value[64] = {};
    snprintf(value, sizeof(value), "%d", desired->gpuOffsetMHz);
    addControl("gpu_offset_mhz", value);
    snprintf(value, sizeof(value), "%d", desired->gpuOffsetExcludeLowCount);
    addControl("gpu_offset_exclude_low_count", value);
    snprintf(value, sizeof(value), "%d", desired->hasLock ? desired->lockCi : -1);
    addControl("lock_ci", value);
    snprintf(value, sizeof(value), "%u", desired->hasLock ? desired->lockMHz : 0u);
    addControl("lock_mhz", value);
    snprintf(value, sizeof(value), "%d", desired->memOffsetMHz);
    addControl("mem_offset_mhz", value);
    snprintf(value, sizeof(value), "%d", desired->powerLimitPct);
    addControl("power_limit_pct", value);
    addControl("fan_mode", fan_mode_to_config_value(desired->fanMode));
    if (desired->fanMode == FAN_MODE_AUTO) addControl("fan", "auto");
    else {
        snprintf(value, sizeof(value), "%d", clamp_percent(desired->fanPercent));
        addControl("fan", value);
    }
    snprintf(value, sizeof(value), "%d", clamp_percent(desired->fanPercent));
    addControl("fan_fixed_pct", value);

    IniEntry semanticsEntry;
    semanticsEntry.key = "curve_semantics";
    semanticsEntry.value = "base_plus_gpu_offset";
    curveEntries.push_back(semanticsEntry);

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desired->hasCurvePoint[i] || desired->curvePointMHz[i] == 0) continue;
        IniEntry entry;
        char key[32] = {};
        snprintf(key, sizeof(key), "point%d", i);
        entry.key = key;
        int baseMHz = (int)desired->curvePointMHz[i] - gpu_offset_component_mhz_for_point_linux(i, desired->gpuOffsetMHz, desired->gpuOffsetExcludeLowCount);
        if (baseMHz <= 0) continue;
        snprintf(value, sizeof(value), "%d", baseMHz);
        entry.value = value;
        curveEntries.push_back(entry);
    }

    replace_section(doc, controlsSection, controlsEntries);
    replace_section(doc, curveSection, curveEntries);
    save_fan_curve_section(doc, fanCurveSection, &desired->fanCurve);
}

bool save_profile_to_config_path(const char* path, int slot, const DesiredSettings* desired, char* err, size_t errSize) {
    if (!path || !desired || slot < 1 || slot > CONFIG_NUM_SLOTS) {
        set_message(err, errSize, "Invalid profile save arguments");
        return false;
    }

    IniDocument doc;
    if (!load_ini_document(path, &doc, err, errSize)) return false;

    DesiredSettings normalized = *desired;
    normalize_desired_settings_for_ui(&normalized);

    set_section_int(&doc, "meta", "format_version", 2);

    int appLaunchSlot = get_section_int(&doc, "profiles", "app_launch_slot", 0);
    int logonSlot = get_section_int(&doc, "profiles", "logon_slot", 0);
    int startOnLogon = get_section_int(&doc, "startup", "start_program_on_logon", 0);
    if (appLaunchSlot < 0 || appLaunchSlot > CONFIG_NUM_SLOTS) appLaunchSlot = 0;
    if (logonSlot < 0 || logonSlot > CONFIG_NUM_SLOTS) logonSlot = 0;

    set_section_int(&doc, "profiles", "selected_slot", slot);
    set_section_int(&doc, "profiles", "app_launch_slot", appLaunchSlot);
    set_section_int(&doc, "profiles", "logon_slot", logonSlot);

    char controlsSection[32] = {};
    char curveSection[32] = {};
    char fanCurveSection[32] = {};
    snprintf(controlsSection, sizeof(controlsSection), "profile%d", slot);
    snprintf(curveSection, sizeof(curveSection), "profile%d_curve", slot);
    snprintf(fanCurveSection, sizeof(fanCurveSection), "profile%d_fan_curve", slot);
    write_profile_sections(&doc, controlsSection, curveSection, fanCurveSection, &normalized);

    if (slot == 1) {
        write_profile_sections(&doc, "controls", "curve", "fan_curve", &normalized);
    }

    set_section_int(&doc, "startup", "apply_on_launch", logonSlot > 0 ? 1 : 0);
    set_section_int(&doc, "startup", "start_program_on_logon", startOnLogon ? 1 : 0);

    return save_ini_document(path, doc, err, errSize);
}

static std::string json_escape(const char* text) {
    std::string out;
    if (!text) return out;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        switch (*p) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (*p < 32) appendf(&out, "\\u%04x", *p);
                else out.push_back((char)*p);
                break;
        }
    }
    return out;
}

void print_desired_settings_text(FILE* out, int slot, const DesiredSettings* desired) {
    if (!out || !desired) return;
    fprintf(out, "Green Curve Linux config snapshot\n");
    fprintf(out, "Profile slot: %d\n", slot);
    fprintf(out, "GPU offset: %d MHz\n", desired->gpuOffsetMHz);
    fprintf(out, "GPU offset exclude first N: %d\n", desired->gpuOffsetExcludeLowCount);
    if (desired->hasLock) fprintf(out, "Lock: point %d @ %u MHz\n", desired->lockCi, desired->lockMHz);
    fprintf(out, "Memory offset: %d MHz\n", desired->memOffsetMHz);
    fprintf(out, "Power limit: %d%%\n", desired->powerLimitPct);
    fprintf(out, "Fan mode: %s\n", fan_mode_label(desired->fanMode));
    fprintf(out, "Fan fixed: %d%%\n", desired->fanPercent);
    char fanSummary[96] = {};
    fan_curve_format_summary(&desired->fanCurve, fanSummary, sizeof(fanSummary));
    fprintf(out, "Fan curve: %s\n", fanSummary);
    fprintf(out, "\nFan curve points\n");
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        fprintf(out, "  %d: %s temp=%dC pct=%d\n", i, desired->fanCurve.points[i].enabled ? "on " : "off", desired->fanCurve.points[i].temperatureC, desired->fanCurve.points[i].fanPercent);
    }
    fprintf(out, "\nVF points\n");
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desired->hasCurvePoint[i]) continue;
        fprintf(out, "  point%d=%u\n", i, desired->curvePointMHz[i]);
    }
}

void print_desired_settings_json(FILE* out, int slot, const DesiredSettings* desired) {
    if (!out || !desired) return;
    fprintf(out, "{\n");
    fprintf(out, "  \"profile_slot\": %d,\n", slot);
    fprintf(out, "  \"gpu_offset_mhz\": %d,\n", desired->gpuOffsetMHz);
    fprintf(out, "  \"gpu_offset_exclude_low_count\": %d,\n", desired->gpuOffsetExcludeLowCount);
    fprintf(out, "  \"lock_ci\": %d,\n", desired->hasLock ? desired->lockCi : -1);
    fprintf(out, "  \"lock_mhz\": %u,\n", desired->hasLock ? desired->lockMHz : 0u);
    fprintf(out, "  \"mem_offset_mhz\": %d,\n", desired->memOffsetMHz);
    fprintf(out, "  \"power_limit_pct\": %d,\n", desired->powerLimitPct);
    fprintf(out, "  \"fan_mode\": \"%s\",\n", json_escape(fan_mode_to_config_value(desired->fanMode)).c_str());
    fprintf(out, "  \"fan_fixed_pct\": %d,\n", desired->fanPercent);
    fprintf(out, "  \"fan_curve\": {\n");
    fprintf(out, "    \"poll_interval_ms\": %d,\n", desired->fanCurve.pollIntervalMs);
    fprintf(out, "    \"hysteresis_c\": %d,\n", desired->fanCurve.hysteresisC);
    fprintf(out, "    \"points\": [\n");
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        fprintf(out,
            "      {\"index\": %d, \"enabled\": %s, \"temp_c\": %d, \"pct\": %d}%s\n",
            i,
            desired->fanCurve.points[i].enabled ? "true" : "false",
            desired->fanCurve.points[i].temperatureC,
            desired->fanCurve.points[i].fanPercent,
            (i + 1 < FAN_CURVE_MAX_POINTS) ? "," : "");
    }
    fprintf(out, "    ]\n  },\n  \"vf_curve\": [\n");
    bool first = true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desired->hasCurvePoint[i]) continue;
        fprintf(out, "%s    {\"index\": %d, \"mhz\": %u}", first ? "" : ",\n", i, desired->curvePointMHz[i]);
        first = false;
    }
    if (!first) fprintf(out, "\n");
    fprintf(out, "  ]\n}\n");
}

