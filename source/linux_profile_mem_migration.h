// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_PROFILE_MEM_MIGRATION_H
#define GREEN_CURVE_LINUX_PROFILE_MEM_MIGRATION_H

// One-time stored-unit migration for the Linux VRAM offset.
//
// Builds before the effective/display parity fix stored
// DesiredSettings.memOffsetMHz in NVML EFFECTIVE MHz (the value was passed to
// NVML verbatim, and NVML memory offsets are effective MHz = 2x display MHz).
// After the parity fix the same field is DISPLAY MHz and every apply doubles
// it, so re-reading a pre-fix file without conversion would apply an overclock
// twice as strong as the user chose (+2500 stored -> +5000 effective).
//
// This header converts stored INI values exactly once. Absence of the
// [meta] linux_mem_migrated marker means "stored units are effective MHz";
// that is safe because the parity fix shipped unreleased after 0.25.0, so
// every config.ini written by an older Linux build carries effective units.
// Windows never reads the Linux config file, and the Windows profile code
// must not learn about this marker: a Windows-saved bank is already display
// MHz end to end. The marker lives in [meta] rather than a version bump of
// the shared INI format number, because that number is written by both
// platforms and a Linux-only bump would fork the Windows loader behavior.
//
// The conversion is a pure function of the file contents (halve, truncation
// toward zero), so a failed rewrite fails open: the marker is absent on the
// next load, the same stored values are converted again, and the result is
// identical. A marker that IS present makes the migration a strict no-op, so
// an already-display file can never be halved a second time.

#include "linux_port_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char LINUX_MEM_MIGRATION_MARKER_SECTION[] = "meta";
static const char LINUX_MEM_MIGRATION_MARKER_KEY[] = "linux_mem_migrated";

static inline const IniSection* linux_mem_migration_find_section(
    const IniDocument* doc, const char* name) {
    if (!doc || !name) return nullptr;
    for (const IniSection& section : doc->sections) {
        if (section.name == name) return &section;
    }
    return nullptr;
}

static inline IniSection* linux_mem_migration_find_section_mutable(
    IniDocument* doc, const char* name) {
    if (!doc || !name) return nullptr;
    for (IniSection& section : doc->sections) {
        if (section.name == name) return &section;
    }
    return nullptr;
}

static inline const IniEntry* linux_mem_migration_find_entry(
    const IniSection* section, const char* key) {
    if (!section || !key) return nullptr;
    for (const IniEntry& entry : section->entries) {
        if (entry.key == key) return &entry;
    }
    return nullptr;
}

static inline IniEntry* linux_mem_migration_find_entry_mutable(
    IniSection* section, const char* key) {
    if (!section || !key) return nullptr;
    for (IniEntry& entry : section->entries) {
        if (entry.key == key) return &entry;
    }
    return nullptr;
}

static inline bool linux_mem_migration_marker_set(const IniDocument* doc) {
    const IniEntry* entry = linux_mem_migration_find_entry(
        linux_mem_migration_find_section(
            doc, LINUX_MEM_MIGRATION_MARKER_SECTION),
        LINUX_MEM_MIGRATION_MARKER_KEY);
    // The writer emits one canonical boolean spelling. Treat anything else as
    // absent so a malformed/partially edited marker cannot suppress the
    // safety-critical effective->display conversion.
    return entry && entry->value == "1";
}

static inline void linux_mem_migration_stamp_marker(IniDocument* doc) {
    IniSection* section = linux_mem_migration_find_section_mutable(
        doc, LINUX_MEM_MIGRATION_MARKER_SECTION);
    if (!section) {
        IniSection created;
        created.name = LINUX_MEM_MIGRATION_MARKER_SECTION;
        doc->sections.push_back(created);
        section = &doc->sections.back();
    }
    IniEntry* entry = linux_mem_migration_find_entry_mutable(
        section, LINUX_MEM_MIGRATION_MARKER_KEY);
    if (!entry) {
        section->entries.push_back(IniEntry());
        entry = &section->entries.back();
        entry->key = LINUX_MEM_MIGRATION_MARKER_KEY;
    }
    entry->value = "1";
}

// Strict local parse for a stored signed int. The values are written by this
// program's own snprintf("%d"), but a hand-edited file must not trip overflow
// into a bogus halved value; anything unparseable is left for the load path's
// parse_int_strict() to report as the real error.
static inline bool linux_mem_migration_parse_int(const std::string& text,
                                                 int* out) {
    if (!out || text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long long parsed = strtoll(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end) return false;
    if (parsed < -2147483648LL || parsed > 2147483647LL) return false;
    *out = (int)parsed;
    return true;
}

static inline bool linux_mem_migration_is_control_section(
    const std::string& name) {
    if (name == "controls") return true;
    for (int slot = 1; slot <= CONFIG_NUM_SLOTS; slot++) {
        char expected[32] = {};
        snprintf(expected, sizeof(expected), "profile%d", slot);
        if (name == expected) return true;
    }
    return false;
}

// Convert every stored mem_offset_mhz under [profile1..N] and the legacy
// [controls] mirror from effective MHz to display MHz, then stamp the marker.
// Returns the number of values rewritten and sets *changedOut when the caller
// must persist the document (the marker was newly stamped or any value was
// rewritten). A doc without any recognized control section (e.g. a missing
// file parsed as empty) is left untouched so a load never fabricates a
// config file.
static inline int linux_ini_migrate_mem_offsets_effective_to_display(
    IniDocument* doc, bool* changedOut) {
    if (changedOut) *changedOut = false;
    if (!doc) return 0;
    if (linux_mem_migration_marker_set(doc)) return 0;

    int rewritten = 0;
    bool foundControlSection = false;
    for (IniSection& section : doc->sections) {
        if (!linux_mem_migration_is_control_section(section.name)) continue;
        foundControlSection = true;
        IniEntry* entry =
            linux_mem_migration_find_entry_mutable(&section, "mem_offset_mhz");
        if (!entry) continue;
        int stored = 0;
        if (!linux_mem_migration_parse_int(entry->value, &stored)) continue;
        int display = nvml_mem_display_mhz_from_effective_mhz(stored);
        char value[16] = {};
        snprintf(value, sizeof(value), "%d", display);
        if (entry->value == value) continue;
        entry->value = value;
        rewritten++;
    }

    // Stamp whenever the document is a real profile bank, values or not: a
    // bank without any mem_offset_mhz key has nothing to halve, and stamping
    // here keeps later loads from rescanning it forever.
    if (foundControlSection) {
        linux_mem_migration_stamp_marker(doc);
        if (changedOut) *changedOut = true;
    }
    return rewritten;
}

#endif
