// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Windows slot loader branch for portable documents (imported profiles).
// Included by config_profiles.cpp after the storage lock and section helpers.
#include "profile_slot_runtime.h"

static bool profile_live_destination_topology(ProfileDestinationTopology& output) {
    output = ProfileDestinationTopology{};
    if (!g_app.loaded || g_app.numPopulated <= 0) return false;
    VfFloorTopology floor = g_app.vfFloorBinding.floor;
    if (!floor.valid) floor = vf_floor_topology_compute(g_app.curve, nullptr, VF_NUM_POINTS);
    return profile_destination_from_vf_curve(g_app.curve, floor, output);
}

// Decode the section body and convert it into the record every apply path
// uses. Position-based records need no GPU; voltage-bound ones map onto the
// selected GPU's live curve. Refusals name what the user can do.
static bool load_profile_document_slot(const std::string& body, int slot, DesiredSettings* desired,
    ProfileReadMode mode, char* err, size_t errSize) {
    auto document = std::make_unique<ProfileDocument>();
    ProfileCodecError error{};
    bool found = false;
    if (!profile_slot_decode(body, *document, found, &error) || !found) {
        set_message(err, errSize, "Profile %d could not be read: %s", slot,
            error.message[0] ? error.message : "no profile data");
        debug_log("profile load: slot %d document decode failed: %s\n", slot, error.message);
        return false;
    }
    auto live = std::make_unique<ProfileDestinationTopology>();
    const bool needsGpu = profile_document_requires_destination(*document);
    const bool haveLive = needsGpu && profile_live_destination_topology(*live);
    char why[192] = {};
    auto converted = std::make_unique<DesiredSettings>();
    if (!profile_slot_document_runtime(*document, haveLive ? live.get() : nullptr, *converted, why, sizeof(why))) {
        set_message(err, errSize, "Profile %d: %s", slot, why);
        debug_log("profile load: slot %d document refused kind=%s live=%d points=%d reason=%s\n", slot,
            needsGpu ? "voltage" : "indexed", haveLive ? live->count : 0, document->topologyCount, why);
        return false;
    }
    *desired = *converted;
    if (desired->applicationScope == DESIRED_SCOPE_LEGACY && mode == PROFILE_READ_FOR_EDITOR &&
        g_app.loaded && g_app.numPopulated > 0) {
        const unsigned int dropped = vf_filter_unlisted_editor_curve_points(
            desired, g_app.vfFloorBinding.floor, g_app.curve);
        if (dropped) debug_log("profile load: editor projection dropped %u hidden VF points\n", dropped);
    }
    validate_desired_settings_for_ipc(desired);
    debug_log("profile load: slot %d converted portable record kind=%s scope=0x%X points=%d mode=%d\n", slot,
        needsGpu ? "voltage" : "indexed", (unsigned)desired->applicationScope, document->topologyCount, (int)mode);
    return true;
}
