// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Linux slot loader branch for portable documents (imported profiles).
// Included by linux_port_profiles.cpp after the INI document helpers.
#include "profile_slot_runtime.h"
#include "profile_file_read.h"

static bool section_holds_profile_document(const IniDocument* doc, const char* section) {
    for (const auto& s : doc->sections) if (s.name == section) for (const auto& e : s.entries)
        if (profile_slot::ascii_equal(std::string_view(e.key).substr(0, 9), "document.")) return true;
    return false;
}

// Imported profiles stay portable documents in the bank. Convert them into the
// record every apply path uses: position-based records need no GPU; a
// voltage-bound record needs the live curve, which this file-level loader does
// not have, so it refuses with a reason the user can act on.
static bool load_document_section(const char* path, const char* section, const char* context,
    DesiredSettings* desired, char* err, size_t errSize) {
    ProfileCodecError error{};
    std::string bytes, text, body;
    bool present = false, found = false;
    auto document = std::make_unique<ProfileDocument>();
    if (!profile_file_read(path, bytes, profile_slot::MAX_CONFIG_BYTES, false, error) ||
        !profile_config::decode(bytes, text, &error) ||
        !profile_slot::extract_named(text, section, body, present, &error) || !present ||
        !profile_slot_decode(body, *document, found, &error) || !found) {
        set_message(err, errSize, "%s could not be read: %s", context, error.message[0] ? error.message : "no profile data");
        return false;
    }
    char why[192] = {};
    auto converted = std::make_unique<DesiredSettings>();
    if (!profile_slot_document_runtime(*document, nullptr, *converted, why, sizeof(why))) {
        set_message(err, errSize, "%s: %s", context, why);
        linux_debug_logf("profile load: %s document refused points=%d reason=%s\n", context, document->topologyCount, why);
        return false;
    }
    *desired = *converted;
    linux_debug_logf("profile load: %s converted portable record scope=0x%X points=%d\n", context,
        (unsigned)desired->applicationScope, document->topologyCount);
    return true;
}

static bool load_desired_settings_from_sections(const IniDocument* doc, const char* controlsSection,
    const char* curveSection, const char* fanCurveSection, DesiredSettings* desired, const char* contextLabel,
    char* err, size_t errSize);

// Ordinary record or imported document, whichever the slot section holds.
static bool load_profile_sections_any(const char* path, const IniDocument* doc, const char* controls,
    const char* curve, const char* fan, DesiredSettings* desired, const char* context, char* err, size_t errSize) {
    if (section_holds_profile_document(doc, controls)) return load_document_section(path, controls, context, desired, err, errSize);
    return load_desired_settings_from_sections(doc, controls, curve, fan, desired, context, err, errSize);
}
