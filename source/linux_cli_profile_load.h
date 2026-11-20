// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Live-daemon fallback for resolving voltage-bound portable profile slots on
// Linux CLI and initial startup-policy binding. Position-based slots and ordinary INI
// slots load offline without contacting the daemon; voltage-bound slots fetch
// the live VF topology from the daemon once and map by exact voltage.
#ifndef GREEN_CURVE_LINUX_CLI_PROFILE_LOAD_H
#define GREEN_CURVE_LINUX_CLI_PROFILE_LOAD_H

#include "linux_daemon.h"
#include "linux_debug_log.h"
#include "linux_port.h"
#include "profile_slot_runtime.h"

#include <memory>

static inline bool linux_profile_destination_from_snapshot(
    const ServiceSnapshot* snapshot, ProfileDestinationTopology* out) {
    if (!snapshot || !out) return false;
    const VfFloorTopology floor = vf_floor_topology_compute(
        snapshot->curve, snapshot->freqOffsets, VF_NUM_POINTS);
    return profile_destination_from_vf_curve(snapshot->curve, floor, *out);
}

static inline bool linux_load_profile_with_daemon_fallback(
    const char* configPath, int slot, const GpuAdapterInfo* readTarget,
    DesiredSettings* desired, char* err, size_t errSize) {
    bool needsLive = false;
    if (load_profile_from_config_path_for_destination(
            configPath, slot, nullptr, desired, err, errSize, &needsLive))
        return true;
    if (!needsLive) return false;
    auto live = std::make_unique<ServiceResponse>();
    char daemonErr[256] = {};
    if (!linux_daemon_get_state(readTarget, live.get(), daemonErr, sizeof(daemonErr))) {
        linux_debug_logf("profile load: slot %d needs live GPU topology, daemon unavailable (%s)",
            slot, daemonErr[0] ? daemonErr : "unreachable");
        return false;
    }
    auto topo = std::make_unique<ProfileDestinationTopology>();
    if (!linux_profile_destination_from_snapshot(&live->snapshot, topo.get())) {
        linux_debug_logf("profile load: slot %d needs live GPU topology, daemon snapshot has no VF curve",
            slot);
        return false;
    }
    return load_profile_from_config_path_for_destination(
        configPath, slot, topo.get(), desired, err, errSize, nullptr);
}

static inline bool linux_load_default_or_selected_profile_with_daemon_fallback(
    const char* configPath, int* slot, const GpuAdapterInfo* readTarget,
    DesiredSettings* desired, char* err, size_t errSize) {
    bool needsLive = false;
    if (load_default_or_selected_profile_for_destination(
            configPath, slot, nullptr, desired, err, errSize, &needsLive))
        return true;
    if (!needsLive) return false;
    auto live = std::make_unique<ServiceResponse>();
    char daemonErr[256] = {};
    if (!linux_daemon_get_state(readTarget, live.get(), daemonErr, sizeof(daemonErr))) {
        linux_debug_logf("profile load: selected slot needs live GPU topology, daemon unavailable (%s)",
            daemonErr[0] ? daemonErr : "unreachable");
        return false;
    }
    auto topo = std::make_unique<ProfileDestinationTopology>();
    if (!linux_profile_destination_from_snapshot(&live->snapshot, topo.get())) {
        linux_debug_logf("profile load: selected slot needs live GPU topology, daemon snapshot has no VF curve");
        return false;
    }
    return load_default_or_selected_profile_for_destination(
        configPath, slot, topo.get(), desired, err, errSize, nullptr);
}

#endif  // GREEN_CURVE_LINUX_CLI_PROFILE_LOAD_H
