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

// trim_ascii, streqi_ascii, parse_int_strict, set_message and parse_fan_value
// used to be duplicated here, verbatim, so the shipping Linux binary ran a
// second copy that no regression test and no fuzz target ever reached.  They
// now come from config_text_utils.cpp, which is in LINUX_SOURCE_FILES and is
// compiled on both platforms.  Do not reintroduce local copies -- a source
// guard fails the build if this file defines any of them again.
//
// The fan-curve math (fan_curve_set_default / _normalize / _validate /
// _interpolate_percent / _format_summary, plus sort_enabled_points) was
// duplicated here too, and that copy had diverged in two ways that mattered:
// its fan_curve_normalize wrote past the end of the 8-point array whenever a
// config had fewer than two enabled points and more than six disabled ones
// (reachable from the daemon socket, linux_port_profiles.cpp and the TUI), and
// it rebuilt the degenerate case as a 2-point curve capped at 35% instead of
// the 5-point default that reaches 90%.  fan_curve.cpp is now linked instead;
// the same source guard covers these names.

void appendf(std::string* text, const char* fmt, ...) {
    if (!text || !fmt) return;
    char stackBuffer[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    // flawfinder: ignore -- declaration carries a printf-format compiler attribute.
    int written = vsnprintf(stackBuffer, sizeof(stackBuffer), fmt, ap);
    va_end(ap);
    if (written < 0) return;
    if ((size_t)written < sizeof(stackBuffer)) {
        text->append(stackBuffer, (size_t)written);
        return;
    }

    std::vector<char> heapBuffer((size_t)written + 1u, 0);
    va_start(ap, fmt);
    // flawfinder: ignore -- same compiler-checked format used for the sizing pass.
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

// initialize_desired_settings_defaults() and normalize_desired_settings_for_ui()
// moved to desired_settings_ui_policy.h so the pure regression harness can pin
// their clamps; linux_port.cpp is not part of that harness.

bool desired_has_any_action(const DesiredSettings* desired) {
    if (!desired) return false;
    // hasLock counts: a lock-only profile carries no curve points or offsets
    // but still demands a curve-tail lock apply.
    if (desired->hasGpuOffset || desired->hasMemOffset || desired->hasPowerLimit || desired->hasFan || desired->hasLock) return true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (desired->hasCurvePoint[i]) return true;
    }
    return false;
}

bool get_executable_path(char* dst, size_t dstSize) {
    if (!dst || dstSize == 0) return false;
    // flawfinder: ignore -- kernel-owned /proc/self/exe; bounds and NUL termination checked.
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

