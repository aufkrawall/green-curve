// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#include "linux_port_internal.h"

#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

static int clamp_int(int value, int minimum, int maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

int clamp_percent(int value) {
    return clamp_int(value, 0, 100);
}

static std::string trim_copy(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && (unsigned char)value[start] <= ' ') start++;
    size_t end = value.size();
    while (end > start && (unsigned char)value[end - 1] <= ' ') end--;
    return value.substr(start, end - start);
}

void trim_ascii(char* s) {
    if (!s) return;
    int len = (int)strlen(s);
    int start = 0;
    while (start < len && (unsigned char)s[start] <= ' ') start++;
    int end = len;
    while (end > start && (unsigned char)s[end - 1] <= ' ') end--;
    if (start > 0 && end > start) {
        memmove(s, s + start, (size_t)(end - start));
    }
    if (end <= start) {
        s[0] = 0;
    } else {
        s[end - start] = 0;
    }
}

bool streqi_ascii(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

bool parse_int_strict(const char* s, int* out) {
    if (!s || !*s || !out) return false;
    char* end = nullptr;
    long value = strtol(s, &end, 10);
    if (!end || *end != 0) return false;
    if (value < -2147483647L - 1L || value > 2147483647L) return false;
    *out = (int)value;
    return true;
}

void set_message(char* dst, size_t dstSize, const char* fmt, ...) {
    if (!dst || dstSize == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, dstSize, fmt, ap);
    va_end(ap);
    dst[dstSize - 1] = 0;
}

void appendf(std::string* text, const char* fmt, ...) {
    if (!text || !fmt) return;
    char stackBuffer[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(stackBuffer, sizeof(stackBuffer), fmt, ap);
    va_end(ap);
    if (written < 0) return;
    if ((size_t)written < sizeof(stackBuffer)) {
        text->append(stackBuffer, (size_t)written);
        return;
    }

    std::vector<char> heapBuffer((size_t)written + 1u, 0);
    va_start(ap, fmt);
    vsnprintf(heapBuffer.data(), heapBuffer.size(), fmt, ap);
    va_end(ap);
    text->append(heapBuffer.data(), (size_t)written);
}

bool starts_with(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    size_t prefixLen = strlen(prefix);
    return strncmp(text, prefix, prefixLen) == 0;
}

bool argument_requires_value(int argc, int index) {
    return index >= 0 && index + 1 < argc;
}

static bool path_exists(const char* path) {
    struct stat st = {};
    return path && stat(path, &st) == 0;
}

static bool directory_exists(const char* path) {
    struct stat st = {};
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

std::string path_dirname(const std::string& path) {
    if (path.empty()) return std::string(".");
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return std::string(".");
    if (slash == 0) return std::string("/");
    return path.substr(0, slash);
}

std::string path_join(const std::string& left, const std::string& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    if (left[left.size() - 1] == '/') return left + right;
    return left + "/" + right;
}

bool ensure_directory_recursive(const char* path, char* err, size_t errSize) {
    if (!path || !*path) return false;
    std::string current;
    std::string normalized(path);
    if (normalized[0] == '/') current = "/";

    size_t start = normalized[0] == '/' ? 1u : 0u;
    while (start <= normalized.size()) {
        size_t slash = normalized.find('/', start);
        std::string part = normalized.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            current = path_join(current, part);
            if (!directory_exists(current.c_str())) {
                if (mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) {
                    set_message(err, errSize, "Failed to create %s (%s)", current.c_str(), strerror(errno));
                    return false;
                }
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

static bool read_text_file(const char* path, std::string* text, char* err, size_t errSize) {
    if (text) text->clear();
    if (!path || !text) return false;

    FILE* file = fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT) return true;
        set_message(err, errSize, "Failed to open %s (%s)", path, strerror(errno));
        return false;
    }

    char buffer[4096] = {};
    for (;;) {
        size_t readCount = fread(buffer, 1, sizeof(buffer), file);
        if (readCount > 0) text->append(buffer, readCount);
        if (readCount < sizeof(buffer)) {
            if (ferror(file)) {
                set_message(err, errSize, "Failed to read %s (%s)", path, strerror(errno));
                fclose(file);
                return false;
            }
            break;
        }
    }

    fclose(file);
    return true;
}

bool write_text_file_atomic(const char* path, const std::string& data, char* err, size_t errSize) {
    if (!path || !*path) {
        set_message(err, errSize, "Invalid output path");
        return false;
    }

    std::string parent = path_dirname(path);
    if (!ensure_directory_recursive(parent.c_str(), err, errSize)) return false;

    std::string tempPath(path);
    tempPath += ".tmp";
    FILE* file = fopen(tempPath.c_str(), "wb");
    if (!file) {
        set_message(err, errSize, "Failed to create %s (%s)", tempPath.c_str(), strerror(errno));
        return false;
    }

    size_t totalWritten = fwrite(data.data(), 1, data.size(), file);
    if (totalWritten != data.size()) {
        set_message(err, errSize, "Failed to write %s (%s)", tempPath.c_str(), strerror(errno));
        fclose(file);
        unlink(tempPath.c_str());
        return false;
    }
    if (fflush(file) != 0) {
        set_message(err, errSize, "Failed to flush %s (%s)", tempPath.c_str(), strerror(errno));
        fclose(file);
        unlink(tempPath.c_str());
        return false;
    }
    int fd = fileno(file);
    if (fd >= 0 && fsync(fd) != 0) {
        set_message(err, errSize, "Failed to sync %s (%s)", tempPath.c_str(), strerror(errno));
        fclose(file);
        unlink(tempPath.c_str());
        return false;
    }
    fclose(file);

    if (rename(tempPath.c_str(), path) != 0) {
        set_message(err, errSize, "Failed to finalize %s (%s)", path, strerror(errno));
        unlink(tempPath.c_str());
        return false;
    }
    return true;
}

static IniSection* find_section(IniDocument* doc, const char* name) {
    if (!doc || !name) return nullptr;
    for (IniSection& section : doc->sections) {
        if (section.name == name) return &section;
    }
    return nullptr;
}

static const IniSection* find_section(const IniDocument* doc, const char* name) {
    if (!doc || !name) return nullptr;
    for (const IniSection& section : doc->sections) {
        if (section.name == name) return &section;
    }
    return nullptr;
}

static IniSection* get_or_create_section(IniDocument* doc, const char* name) {
    IniSection* existing = find_section(doc, name);
    if (existing) return existing;
    doc->sections.push_back(IniSection());
    doc->sections.back().name = name ? name : "";
    return &doc->sections.back();
}

static IniEntry* find_entry(IniSection* section, const char* key) {
    if (!section || !key) return nullptr;
    for (IniEntry& entry : section->entries) {
        if (entry.key == key) return &entry;
    }
    return nullptr;
}

static const IniEntry* find_entry(const IniSection* section, const char* key) {
    if (!section || !key) return nullptr;
    for (const IniEntry& entry : section->entries) {
        if (entry.key == key) return &entry;
    }
    return nullptr;
}

bool load_ini_document(const char* path, IniDocument* doc, char* err, size_t errSize) {
    if (!doc) return false;
    doc->sections.clear();

    std::string text;
    if (!read_text_file(path, &text, err, errSize)) return false;
    if (text.empty()) return true;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text = text.substr(3);
    }

    IniSection* current = nullptr;
    size_t offset = 0;
    while (offset <= text.size()) {
        size_t lineEnd = text.find('\n', offset);
        std::string line = lineEnd == std::string::npos ? text.substr(offset) : text.substr(offset, lineEnd - offset);
        if (!line.empty() && line[line.size() - 1] == '\r') line.resize(line.size() - 1);
        line = trim_copy(line);
        if (!line.empty() && line[0] != ';' && line[0] != '#') {
            if (line.size() >= 2 && line[0] == '[' && line[line.size() - 1] == ']') {
                std::string name = trim_copy(line.substr(1, line.size() - 2));
                current = get_or_create_section(doc, name.c_str());
            } else {
                size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    if (!current) current = get_or_create_section(doc, "");
                    IniEntry entry;
                    entry.key = trim_copy(line.substr(0, eq));
                    entry.value = trim_copy(line.substr(eq + 1));
                    current->entries.push_back(entry);
                }
            }
        }
        if (lineEnd == std::string::npos) break;
        offset = lineEnd + 1;
    }

    return true;
}

void set_section_value(IniDocument* doc, const char* sectionName, const char* key, const char* value) {
    IniSection* section = get_or_create_section(doc, sectionName);
    IniEntry* entry = find_entry(section, key);
    if (!entry) {
        section->entries.push_back(IniEntry());
        entry = &section->entries.back();
        entry->key = key ? key : "";
    }
    entry->value = value ? value : "";
}

void set_section_int(IniDocument* doc, const char* sectionName, const char* key, int value) {
    char buffer[64] = {};
    snprintf(buffer, sizeof(buffer), "%d", value);
    set_section_value(doc, sectionName, key, buffer);
}

void replace_section(IniDocument* doc, const char* sectionName, const std::vector<IniEntry>& entries) {
    IniSection* section = get_or_create_section(doc, sectionName);
    section->entries = entries;
}

bool section_has_keys(const IniDocument* doc, const char* sectionName) {
    const IniSection* section = find_section(doc, sectionName);
    return section && !section->entries.empty();
}

std::string get_section_value(const IniDocument* doc, const char* sectionName, const char* key) {
    const IniSection* section = find_section(doc, sectionName);
    const IniEntry* entry = find_entry(section, key);
    if (!entry) return std::string();
    return entry->value;
}

int get_section_int(const IniDocument* doc, const char* sectionName, const char* key, int defaultValue) {
    std::string value = get_section_value(doc, sectionName, key);
    if (value.empty()) return defaultValue;
    int parsed = 0;
    return parse_int_strict(value.c_str(), &parsed) ? parsed : defaultValue;
}

bool save_ini_document(const char* path, const IniDocument& doc, char* err, size_t errSize) {
    std::string out;
    for (size_t i = 0; i < doc.sections.size(); i++) {
        const IniSection& section = doc.sections[i];
        if (!section.name.empty()) appendf(&out, "[%s]\n", section.name.c_str());
        for (const IniEntry& entry : section.entries) {
            appendf(&out, "%s=%s\n", entry.key.c_str(), entry.value.c_str());
        }
        if (i + 1 < doc.sections.size()) out += "\n";
    }
    return write_text_file_atomic(path, out, err, errSize);
}

bool parse_fan_value(const char* text, bool* isAuto, int* pct) {
    if (!isAuto || !pct) return false;
    char buffer[64] = {};
    if (text) snprintf(buffer, sizeof(buffer), "%s", text);
    trim_ascii(buffer);
    if (buffer[0] == 0 || streqi_ascii(buffer, "auto")) {
        *isAuto = true;
        *pct = 0;
        return true;
    }
    int value = 0;
    if (!parse_int_strict(buffer, &value)) return false;
    if (value < 0 || value > 100) return false;
    *isAuto = false;
    *pct = value;
    return true;
}

const char* fan_mode_label(int mode) {
    switch (mode) {
        case FAN_MODE_FIXED: return "Fixed";
        case FAN_MODE_CURVE: return "Curve";
        default: return "Auto";
    }
}

const char* fan_mode_to_config_value(int mode) {
    switch (mode) {
        case FAN_MODE_FIXED: return "fixed";
        case FAN_MODE_CURVE: return "curve";
        default: return "auto";
    }
}

bool parse_fan_mode_config_value(const char* text, int* mode) {
    if (!text || !*text || !mode) return false;
    if (streqi_ascii(text, "auto") || streqi_ascii(text, "default")) {
        *mode = FAN_MODE_AUTO;
        return true;
    }
    if (streqi_ascii(text, "fixed") || streqi_ascii(text, "manual")) {
        *mode = FAN_MODE_FIXED;
        return true;
    }
    if (streqi_ascii(text, "curve")) {
        *mode = FAN_MODE_CURVE;
        return true;
    }
    return false;
}

static void sort_enabled_points(FanCurvePoint* points, int count) {
    for (int i = 1; i < count; i++) {
        FanCurvePoint key = points[i];
        int j = i - 1;
        while (j >= 0 && points[j].temperatureC > key.temperatureC) {
            points[j + 1] = points[j];
            j--;
        }
        points[j + 1] = key;
    }
}

void fan_curve_set_default(FanCurveConfig* config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->pollIntervalMs = 1000;
    config->hysteresisC = 2;
    config->points[0] = { true, 30, 20 };
    config->points[1] = { true, 45, 35 };
    config->points[2] = { true, 60, 55 };
    config->points[3] = { true, 72, 72 };
    config->points[4] = { true, 84, 90 };
    config->points[5] = { false, 90, 95 };
    config->points[6] = { false, 95, 100 };
    config->points[7] = { false, 100, 100 };
}

void fan_curve_normalize(FanCurveConfig* config) {
    if (!config) return;

    config->pollIntervalMs = clamp_int(config->pollIntervalMs, 250, 5000);
    config->pollIntervalMs = ((config->pollIntervalMs + 125) / 250) * 250;
    config->hysteresisC = clamp_int(config->hysteresisC, 0, FAN_CURVE_MAX_HYSTERESIS_C);

    FanCurvePoint enabled[FAN_CURVE_MAX_POINTS] = {};
    FanCurvePoint disabled[FAN_CURVE_MAX_POINTS] = {};
    int enabledCount = 0;
    int disabledCount = 0;

    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        config->points[i].temperatureC = clamp_int(config->points[i].temperatureC, 0, 100);
        config->points[i].fanPercent = clamp_percent(config->points[i].fanPercent);
        if (config->points[i].enabled) enabled[enabledCount++] = config->points[i];
        else disabled[disabledCount++] = config->points[i];
    }

    if (enabledCount < 2) {
        FanCurveConfig defaults = {};
        fan_curve_set_default(&defaults);
        enabled[0] = defaults.points[0];
        enabled[1] = defaults.points[1];
        enabledCount = 2;
    }

    sort_enabled_points(enabled, enabledCount);
    for (int i = 0; i < enabledCount; i++) {
        config->points[i] = enabled[i];
        config->points[i].enabled = true;
    }
    for (int i = 0; i < disabledCount; i++) {
        config->points[enabledCount + i] = disabled[i];
        config->points[enabledCount + i].enabled = false;
    }
}

bool fan_curve_validate(const FanCurveConfig* config, char* err, size_t errSize) {
    if (!config) {
        set_message(err, errSize, "No fan curve config");
        return false;
    }
    if (config->pollIntervalMs < 250 || config->pollIntervalMs > 5000 || (config->pollIntervalMs % 250) != 0) {
        set_message(err, errSize, "Fan curve poll interval must be 250-5000 ms in 250 ms steps");
        return false;
    }
    if (config->hysteresisC < 0 || config->hysteresisC > FAN_CURVE_MAX_HYSTERESIS_C) {
        set_message(err, errSize, "Fan curve hysteresis must be 0-10 \xC2\xB0""C");
        return false;
    }

    FanCurvePoint active[FAN_CURVE_MAX_POINTS] = {};
    int activeCount = 0;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        const FanCurvePoint* point = &config->points[i];
        if (point->temperatureC < 0 || point->temperatureC > 100) {
            set_message(err, errSize, "Fan curve temperatures must be 0-100 \xC2\xB0""C");
            return false;
        }
        if (point->fanPercent < 0 || point->fanPercent > 100) {
            set_message(err, errSize, "Fan curve percentages must be 0-100");
            return false;
        }
        if (point->enabled) active[activeCount++] = *point;
    }
    if (activeCount < 2) {
        set_message(err, errSize, "Enable at least two fan curve points");
        return false;
    }
    sort_enabled_points(active, activeCount);
    for (int i = 1; i < activeCount; i++) {
        if (active[i].temperatureC <= active[i - 1].temperatureC) {
            set_message(err, errSize, "Enabled fan curve temperatures must be strictly increasing");
            return false;
        }
        if (active[i].fanPercent < active[i - 1].fanPercent) {
            set_message(err, errSize, "Enabled fan curve percentages must be nondecreasing");
            return false;
        }
    }
    return true;
}

void fan_curve_format_summary(const FanCurveConfig* config, char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0) return;
    if (!config) {
        buffer[0] = 0;
        return;
    }
    int activeCount = 0;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        if (config->points[i].enabled) activeCount++;
    }
    snprintf(buffer, bufferSize, "%d pts | %.2fs | %d\xC2\xB0""C hyst", activeCount, (double)config->pollIntervalMs / 1000.0, config->hysteresisC);
    buffer[bufferSize - 1] = 0;
}

void initialize_desired_settings_defaults(DesiredSettings* desired) {
    if (!desired) return;
    memset(desired, 0, sizeof(*desired));
    desired->lockTracksAnchor = true;
    desired->fanAuto = true;
    desired->fanMode = FAN_MODE_AUTO;
    desired->powerLimitPct = 100;
    fan_curve_set_default(&desired->fanCurve);
}

void normalize_desired_settings_for_ui(DesiredSettings* desired) {
    if (!desired) return;
    desired->hasGpuOffset = true;
    desired->hasMemOffset = true;
    desired->hasPowerLimit = true;
    desired->hasFan = true;
    if (desired->gpuOffsetMHz == 0) desired->gpuOffsetExcludeLowCount = 0;
    desired->powerLimitPct = clamp_percent(desired->powerLimitPct == 0 ? 100 : desired->powerLimitPct);
    desired->fanPercent = clamp_percent(desired->fanPercent <= 0 ? 50 : desired->fanPercent);
    if (desired->fanMode < FAN_MODE_AUTO || desired->fanMode > FAN_MODE_CURVE) desired->fanMode = FAN_MODE_AUTO;
    desired->fanAuto = desired->fanMode == FAN_MODE_AUTO;
    fan_curve_normalize(&desired->fanCurve);
    char err[128] = {};
    if (!fan_curve_validate(&desired->fanCurve, err, sizeof(err))) {
        fan_curve_set_default(&desired->fanCurve);
    }
}

bool desired_has_any_action(const DesiredSettings* desired) {
    if (!desired) return false;
    if (desired->hasGpuOffset || desired->hasMemOffset || desired->hasPowerLimit || desired->hasFan) return true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i]) return true;
    }
    return false;
}

bool get_executable_path(char* dst, size_t dstSize) {
    if (!dst || dstSize == 0) return false;
    ssize_t readCount = readlink("/proc/self/exe", dst, dstSize);
    if (readCount < 0) return false;
    if (readCount >= (ssize_t)dstSize) return false;
    dst[readCount] = 0;
    return true;
}

bool default_linux_config_path(char* dst, size_t dstSize) {
    if (!dst || dstSize == 0) return false;
    char exePath[LINUX_PATH_MAX] = {};
    if (!get_executable_path(exePath, sizeof(exePath))) return false;
    std::string configPath = path_join(path_dirname(exePath), CONFIG_FILE_NAME);
    snprintf(dst, dstSize, "%s", configPath.c_str());
    dst[dstSize - 1] = 0;
    return true;
}

