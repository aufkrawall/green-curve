// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_CONFIGURED_GPU_SELECTION_H
#define GREEN_CURVE_CONFIGURED_GPU_SELECTION_H

// Split out of gpu_core.h, which sits on its size ratchet.  Included from
// gpu_core.h right after GpuAdapterInfo, so every consumer still gets these
// types by including gpu_core.h.  The resolution rules live in
// gpu_selection_policy.h; the [gpu] record format in config_utils.cpp.

// What the persisted PCI location contributed when the GPU was selected.
// Firmware assigns bus numbers at boot, so they shift whenever an upstream
// device appears or disappears (an on-board NIC disabled in the BIOS, an added
// NVMe drive). The location therefore tells identical adapters apart; it is not
// part of the identity of a GPU whose model was unique when it was chosen.
// A fixed type keeps any stored or cast value well defined for validators.
enum GpuLocationRole : int {
    // Written before the role was recorded: the location is binding unless
    // exactly one adapter is present (the legacy single-adapter tolerance).
    GPU_LOCATION_ROLE_UNRECORDED = 0,
    // The model identity alone named the GPU; the location is advisory.
    GPU_LOCATION_ROLE_ADVISORY,
    // An identical adapter was present; only the location names the GPU.
    GPU_LOCATION_ROLE_REQUIRED,
};

// Durable GPU selection. legacyIndex preserves compatibility with existing
// configs, while a present stable identity prevents adapter enumeration order
// changes from redirecting an automatic profile apply to another GPU.
struct ConfiguredGpuSelection {
    gc_bool8 stableIdentityPresent;
    unsigned int legacyIndex;
    GpuAdapterInfo identity;
    GpuLocationRole locationRole;
};

#endif // GREEN_CURVE_CONFIGURED_GPU_SELECTION_H
