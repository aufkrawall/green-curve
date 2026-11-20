// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Linux slot loader branch for portable documents (imported profiles).
// Included by linux_port_profiles.cpp after the INI document helpers.
#include "profile_slot_runtime.h"
#include "profile_file_read.h"
#include "profile_config_encoding.h"

static bool section_holds_profile_document(const IniDocument* doc, const char* section) {
    for (const auto& s : doc->sections) if (s.name == section) for (const auto& e : s.entries)
        if (profile_slot::ascii_equal(std::string_view(e.key).substr(0, 9), "document.")) return true;
    return false;
}

// Imported profiles stay portable documents in the bank. Convert them into the
// record every apply path uses: position-based records need no GPU; a
// voltage-bound record maps onto `destination` when provided (TUI snapshot or
// live daemon state), and otherwise refuses with a reason the user can act on.
static bool load_document_section(const char* path, const char* section, const char* context,
    const ProfileDestinationTopology* destination, DesiredSettings* desired, char* err, size_t errSize,
    bool* needsLiveDestinationOut = nullptr) {
    if (needsLiveDestinationOut) *needsLiveDestinationOut = false;
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
    const bool needsDestination = profile_document_requires_destination(*document);
    if (needsLiveDestinationOut) *needsLiveDestinationOut = needsDestination;
    const bool repairedFlatTailOffset =
        !needsDestination &&
        profile_indexed_document_has_legacy_flat_tail_offset_artifact(*document);
    char why[192] = {};
    auto converted = std::make_unique<DesiredSettings>();
    if (!profile_slot_document_runtime(*document, destination, *converted, why, sizeof(why))) {
        set_message(err, errSize, "%s: %s", context, why);
        linux_debug_logf("profile load: %s document refused kind=%s live=%d points=%d reason=%s\n",
            context, needsDestination ? "voltage" : "indexed",
            destination ? destination->count : 0, document->topologyCount, why);
        return false;
    }
    if (repairedFlatTailOffset) {
        linux_debug_logf("profile load: %s repaired legacy flat-tail graphics offset artifact "
                         "(rawOffsetHz=%lld exclude=%d lockCi=%d lockMHz=%u -> 0 MHz)\n",
            context, document->graphicsOffset.offsetHz,
            document->graphicsOffset.exclusionPolicy == PROFILE_EXCLUDE_LEGACY_COUNT
                ? document->graphicsOffset.excludeLowCount : 0,
            converted->lockCi, converted->lockMHz);
    }
    *desired = *converted;
    linux_debug_logf("profile load: %s converted portable record kind=%s scope=0x%X graphicsRoute=%u points=%d live=%d\n",
        context, needsDestination ? "voltage" : "indexed",
        (unsigned)desired->applicationScope, (unsigned)desired->authoring.graphicsSurface, document->topologyCount,
        destination ? destination->count : 0);
    return true;
}

static bool load_desired_settings_from_sections(const IniDocument* doc, const char* controlsSection,
    const char* curveSection, const char* fanCurveSection, DesiredSettings* desired, const char* contextLabel,
    char* err, size_t errSize);

// Ordinary record or imported document, whichever the slot section holds.
static bool load_profile_sections_any(const char* path, const IniDocument* doc, const char* controls,
    const char* curve, const char* fan, const ProfileDestinationTopology* destination,
    DesiredSettings* desired, const char* context, char* err, size_t errSize,
    bool* needsLiveDestinationOut = nullptr) {
    if (needsLiveDestinationOut) *needsLiveDestinationOut = false;
    if (section_holds_profile_document(doc, controls))
        return load_document_section(path, controls, context, destination, desired, err, errSize, needsLiveDestinationOut);
    return load_desired_settings_from_sections(doc, controls, curve, fan, desired, context, err, errSize);
}

static inline void linux_repair_legacy_flat_tail_offset_artifact(DesiredSettings* desired, const char* context) {
    if (!profile_desired_has_legacy_flat_tail_offset_artifact(desired, nullptr, nullptr)) return;
    const int rawGpuOffsetMHz = desired->gpuOffsetMHz;
    const int rawExclude = desired->gpuOffsetExcludeLowCount;
    profile_desired_repair_legacy_flat_tail_offset(desired);
    linux_debug_logf("profile: cleared legacy flat-tail graphics offset artifact in %s "
                     "(rawGpuOffset=%d rawExclude=%d lockCi=%d lockMHz=%u -> 0 MHz)\n",
                     context ? context : "config", rawGpuOffsetMHz, rawExclude,
                     desired->lockCi, desired->lockMHz);
}
