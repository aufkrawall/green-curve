#include "linux_port.h"

#include <algorithm>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

struct IniEntry {
    std::string key;
    std::string value;
};

struct IniSection {
    std::string name;
    std::vector<IniEntry> entries;
};

struct IniDocument {
    std::vector<IniSection> sections;
};

static int clamp_int(int value, int minimum, int maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int clamp_percent(int value) {
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

static void appendf(std::string* text, const char* fmt, ...) {
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

static bool starts_with(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    size_t prefixLen = strlen(prefix);
    return strncmp(text, prefix, prefixLen) == 0;
}

static bool path_exists(const char* path) {
    struct stat st = {};
    return path && stat(path, &st) == 0;
}

static bool directory_exists(const char* path) {
    struct stat st = {};
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static std::string path_dirname(const std::string& path) {
    if (path.empty()) return std::string(".");
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return std::string(".");
    if (slash == 0) return std::string("/");
    return path.substr(0, slash);
}

static std::string path_join(const std::string& left, const std::string& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    if (left[left.size() - 1] == '/') return left + right;
    return left + "/" + right;
}

static bool ensure_directory_recursive(const char* path, char* err, size_t errSize) {
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
                if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
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

static bool write_text_file_atomic(const char* path, const std::string& data, char* err, size_t errSize) {
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

static bool load_ini_document(const char* path, IniDocument* doc, char* err, size_t errSize) {
    if (!doc) return false;
    doc->sections.clear();

    std::string text;
    if (!read_text_file(path, &text, err, errSize)) return false;
    if (text.empty()) return true;

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

static void set_section_value(IniDocument* doc, const char* sectionName, const char* key, const char* value) {
    IniSection* section = get_or_create_section(doc, sectionName);
    IniEntry* entry = find_entry(section, key);
    if (!entry) {
        section->entries.push_back(IniEntry());
        entry = &section->entries.back();
        entry->key = key ? key : "";
    }
    entry->value = value ? value : "";
}

static void set_section_int(IniDocument* doc, const char* sectionName, const char* key, int value) {
    char buffer[64] = {};
    snprintf(buffer, sizeof(buffer), "%d", value);
    set_section_value(doc, sectionName, key, buffer);
}

static void replace_section(IniDocument* doc, const char* sectionName, const std::vector<IniEntry>& entries) {
    IniSection* section = get_or_create_section(doc, sectionName);
    section->entries = entries;
}

static bool section_has_keys(const IniDocument* doc, const char* sectionName) {
    const IniSection* section = find_section(doc, sectionName);
    return section && !section->entries.empty();
}

static std::string get_section_value(const IniDocument* doc, const char* sectionName, const char* key) {
    const IniSection* section = find_section(doc, sectionName);
    const IniEntry* entry = find_entry(section, key);
    if (!entry) return std::string();
    return entry->value;
}

static int get_section_int(const IniDocument* doc, const char* sectionName, const char* key, int defaultValue) {
    std::string value = get_section_value(doc, sectionName, key);
    if (value.empty()) return defaultValue;
    int parsed = 0;
    return parse_int_strict(value.c_str(), &parsed) ? parsed : defaultValue;
}

static bool save_ini_document(const char* path, const IniDocument& doc, char* err, size_t errSize) {
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

static const char* fan_mode_to_config_value(int mode) {
    switch (mode) {
        case FAN_MODE_FIXED: return "fixed";
        case FAN_MODE_CURVE: return "curve";
        default: return "auto";
    }
}

static bool parse_fan_mode_config_value(const char* text, int* mode) {
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
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (points[j].temperatureC < points[i].temperatureC) {
                FanCurvePoint temp = points[i];
                points[i] = points[j];
                points[j] = temp;
            }
        }
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
    for (int i = enabledCount + disabledCount; i < FAN_CURVE_MAX_POINTS; i++) {
        config->points[i] = { false, 100, 100 };
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
    ssize_t readCount = readlink("/proc/self/exe", dst, dstSize - 1);
    if (readCount < 0) return false;
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

    std::string value = get_section_value(doc, controlsSection, "gpu_offset_mhz");
    if (!value.empty()) {
        if (!parse_int_strict(value.c_str(), &desired->gpuOffsetMHz)) {
            set_message(err, errSize, "Invalid gpu_offset_mhz in %s", messageContext);
            return false;
        }
        desired->hasGpuOffset = true;
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
        if (!desired->hasFan || desired->fanMode != FAN_MODE_CURVE) {
            set_desired_fan_from_legacy_value(desired, fanAuto, fanPercent);
        }
    }

    value = get_section_value(doc, controlsSection, "fan_fixed_pct");
    if (!value.empty()) {
        int parsed = 0;
        if (!parse_int_strict(value.c_str(), &parsed)) {
            set_message(err, errSize, "Invalid fan_fixed_pct in %s", messageContext);
            return false;
        }
        desired->hasFan = true;
        desired->fanMode = desired->fanMode == FAN_MODE_CURVE ? FAN_MODE_CURVE : FAN_MODE_FIXED;
        desired->fanAuto = false;
        desired->fanPercent = clamp_percent(parsed);
    }

    if (!load_fan_curve_config_from_section(doc, fanCurveSection, &desired->fanCurve, err, errSize)) return false;

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

    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desired->hasCurvePoint[i] || desired->curvePointMHz[i] == 0) continue;
        IniEntry entry;
        char key[32] = {};
        snprintf(key, sizeof(key), "point%d", i);
        entry.key = key;
        snprintf(value, sizeof(value), "%u", desired->curvePointMHz[i]);
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

static bool argument_requires_value(int argc, int index) {
    return index + 1 < argc;
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
        if (access(candidate.c_str(), X_OK) == 0) return true;
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
