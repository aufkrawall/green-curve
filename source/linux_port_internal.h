// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_PORT_INTERNAL_H
#define GREEN_CURVE_LINUX_PORT_INTERNAL_H

#include "linux_port.h"

#include <string>
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

int clamp_percent(int value);
bool starts_with(const char* text, const char* prefix);
std::string path_dirname(const std::string& path);
std::string path_join(const std::string& left, const std::string& right);
bool argument_requires_value(int argc, int index);
void appendf(std::string* text, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
bool ensure_directory_recursive(const char* path, char* err, size_t errSize);
bool write_text_file_atomic(const char* path, const std::string& data, char* err, size_t errSize);
bool load_ini_document(const char* path, IniDocument* doc, char* err, size_t errSize);
bool save_ini_document(const char* path, const IniDocument& doc, char* err, size_t errSize);
void set_section_value(IniDocument* doc, const char* sectionName, const char* key, const char* value);
void set_section_int(IniDocument* doc, const char* sectionName, const char* key, int value);
void replace_section(IniDocument* doc, const char* sectionName, const std::vector<IniEntry>& entries);
bool section_has_keys(const IniDocument* doc, const char* sectionName);
std::string get_section_value(const IniDocument* doc, const char* sectionName, const char* key);
int get_section_int(const IniDocument* doc, const char* sectionName, const char* key, int defaultValue);
bool parse_fan_mode_config_value(const char* text, int* mode);
const char* fan_mode_to_config_value(int mode);

#endif
