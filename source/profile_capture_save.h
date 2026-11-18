// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CAPTURE_SAVE_H
#define GREEN_CURVE_PROFILE_CAPTURE_SAVE_H
// "Import current GPU settings": turn a reviewed passive snapshot into the
// ordinary slot record every apply path understands (tray, hotkeys,
// auto-switch, app start, logon). Shared by the Windows dialog and the TUI.
//
// The snapshot is its own destination: the captured curve is mapped onto the
// topology it was read from, so the saved record names exact points of this
// GPU. Nothing is applied; saving never changes GPU settings or ownership.
#include "profile_capture_proposals.h"
#include "profile_slot_runtime.h"
#include "profile_summary.h"

// Setting groups a capture reads. External fan behavior is never captured.
static constexpr gc_u32 PROFILE_CAPTURE_DEFAULT_DOMAINS =
    PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER |
    PROFILE_DOMAIN_LOCK | PROFILE_DOMAIN_XBAR | PROFILE_DOMAIN_SYS_CLK | PROFILE_DOMAIN_VIDEO_CLK;

// The groups the dialogs offer as include/leave-out rows.
enum ProfileCaptureGroup : int {
    PROFILE_CAPTURE_GROUP_CURVE, PROFILE_CAPTURE_GROUP_CORE, PROFILE_CAPTURE_GROUP_MEMORY,
    PROFILE_CAPTURE_GROUP_POWER, PROFILE_CAPTURE_GROUP_ADVANCED, PROFILE_CAPTURE_GROUP_COUNT
};
static inline int profile_capture_group_domains(int group) {
    switch (group) {
        case PROFILE_CAPTURE_GROUP_CURVE: return PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK;
        case PROFILE_CAPTURE_GROUP_CORE: return PROFILE_DOMAIN_GRAPHICS_OFFSET;
        case PROFILE_CAPTURE_GROUP_MEMORY: return PROFILE_DOMAIN_MEMORY;
        case PROFILE_CAPTURE_GROUP_POWER: return PROFILE_DOMAIN_POWER;
        case PROFILE_CAPTURE_GROUP_ADVANCED: return PROFILE_DOMAIN_XBAR | PROFILE_DOMAIN_SYS_CLK | PROFILE_DOMAIN_VIDEO_CLK;
        default: return 0;
    }
}
static inline const char* profile_capture_group_name(int group) {
    static const char* names[] = {"Curve", "Core clock", "Memory clock", "Power limit", "Advanced clocks"};
    return group >= 0 && group < PROFILE_CAPTURE_GROUP_COUNT ? names[group] : "";
}
static inline const ProfileCaptureSummaryRow& profile_capture_group_row(const ProfileCaptureSummary& s, int group) {
    switch (group) {
        case PROFILE_CAPTURE_GROUP_CURVE: return s.curve;
        case PROFILE_CAPTURE_GROUP_CORE: return s.core;
        case PROFILE_CAPTURE_GROUP_MEMORY: return s.memory;
        case PROFILE_CAPTURE_GROUP_POWER: return s.power;
        default: return s.advanced;
    }
}
// Advanced clocks are offered only when the GPU has them.
static inline bool profile_capture_group_shown(const ProfileCaptureSummary& s, int group) {
    return group != PROFILE_CAPTURE_GROUP_ADVANCED || s.advancedPresent;
}

// Build the record to save. `included` is a mask of 1u << ProfileCaptureGroup.
static inline bool profile_capture_record(const HardwareCaptureObservation& observation,
    const ProfileCaptureProposals& proposals, ProfileCaptureRepresentation representation, unsigned int included,
    DesiredSettings& output, char* why, size_t whySize) {
    auto document = std::make_unique<ProfileDocument>();
    if (!profile_capture_select_proposal(proposals, representation, *document, why, whySize)) return false;
    int dropped = 0;
    for (int group = 0; group < PROFILE_CAPTURE_GROUP_COUNT; ++group)
        if (!(included & (1u << group))) dropped |= profile_capture_group_domains(group);
    if (dropped && !profile_document_drop_domains(*document, dropped, why, whySize)) return false;
    if (!document->domains) return profile_slot_runtime_fail(why, whySize, "choose at least one setting to save");
    auto destination = std::make_unique<ProfileDestinationTopology>();
    char detail[160]{};
    const bool needsTopology = profile_document_requires_destination(*document);
    if (needsTopology && !profile_destination_from_capture(observation, *destination, detail, sizeof(detail)))
        return profile_slot_runtime_fail(why, whySize, "the GPU's curve could not be read completely; read again");
    if (!profile_slot_document_runtime(*document, needsTopology ? destination.get() : nullptr, output, why, whySize))
        return false;
    // A capture names exactly the settings it includes. An ordinary
    // (legacy-scoped) slot would be completed from live values on save, so a
    // left-out setting would silently come back.
    if (output.applicationScope == DESIRED_SCOPE_LEGACY) output.applicationScope = DESIRED_SCOPE_REPLACE_PROFILE;
    return true;
}

// First empty slot, else the preferred one.
template <class SavedFn>
static inline int profile_capture_default_slot(int preferred, SavedFn saved, int slots = 5) {
    for (int slot = 1; slot <= slots; ++slot) if (!saved(slot)) return slot;
    return preferred >= 1 && preferred <= slots ? preferred : 1;
}
#endif
