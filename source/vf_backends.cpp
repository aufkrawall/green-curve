// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// vf_backends.cpp — definitions of the per-family VF-curve backend tables.
// Extracted from main.cpp so both the Windows and Linux backends link against
// one copy.  Pascal, Turing, Ampere, Lovelace, and Blackwell are treated as
// tested known families.  Only the "future" fallback uses the same layout as a
// best-effort guess (bestGuessOnly = true).

#include "vf_backends.h"
#include "gpu_capability_policy.h"

// Pin the capability policy's bit-position indexing against the protocol's
// domain bits.  This is the one translation unit where both headers are
// complete (gpu_capability_policy.h cannot assert this itself: it is included
// by way of gpu_core.h, which defines the protocol only at its tail).
// Renumbering a ServiceMutationDomain without updating
// gpu_capability_domain_name() would otherwise silently mislabel every
// capability log line and every warning the user reads.
static_assert(gpu_capability_mask_for_index(0) == SERVICE_MUTATION_DOMAIN_RESET_BASELINE,
              "capability index 0 must be reset-baseline");
static_assert(gpu_capability_mask_for_index(1) == SERVICE_MUTATION_DOMAIN_GPU_OFFSET,
              "capability index 1 must be gpu-offset");
static_assert(gpu_capability_mask_for_index(2) == SERVICE_MUTATION_DOMAIN_MEM_OFFSET,
              "capability index 2 must be memory-offset");
static_assert(gpu_capability_mask_for_index(3) == SERVICE_MUTATION_DOMAIN_POWER,
              "capability index 3 must be power-limit");
static_assert(gpu_capability_mask_for_index(4) == SERVICE_MUTATION_DOMAIN_VF_CURVE,
              "capability index 4 must be vf-curve");
static_assert(gpu_capability_mask_for_index(5) == SERVICE_MUTATION_DOMAIN_LOCK,
              "capability index 5 must be clock-lock");
static_assert(gpu_capability_mask_for_index(6) == SERVICE_MUTATION_DOMAIN_FAN,
              "capability index 6 must be fan");
static_assert(gpu_capability_mask_for_index(7) == SERVICE_MUTATION_DOMAIN_XBAR,
              "capability index 7 must be xbar");
static_assert(gpu_capability_mask_for_index(8) == SERVICE_MUTATION_DOMAIN_SYS_CLK,
              "capability index 8 must be sys-clk");
// The core invariant, enforced at compile time: an unprobed capability set
// subtracts no domain, so this layer cannot regress a working GPU.
static_assert(gpu_capability_available_domains(nullptr) == SERVICE_MUTATION_DOMAIN_ALL,
              "an unprobed capability set must subtract nothing");

// Fields, in order: name, family, supported, readSupported, writeSupported,
// bestGuessOnly, getStatusId, getInfoId, getControlId, setControlId,
// statusBufferSize, statusVersion, statusMaskOffset, statusNumClocksOffset,
// statusEntriesOffset, statusEntryStride, infoBufferSize, infoVersion,
// infoMaskOffset, infoNumClocksOffset, controlBufferSize, controlVersion,
// controlMaskOffset, controlEntryBaseOffset, controlEntryStride,
// controlEntryDeltaOffset, defaultNumClocks.

extern const VfBackendSpec g_vfBackendBlackwell = {
    "blackwell",
    GPU_FAMILY_BLACKWELL,
    true, true, true, false,
    0x21537AD4u, 0x507B4B59u, 0x23F1B133u, 0x0733E009u,
    0x1C28, 1, 0x04, 0x24, 0x48, 0x1C,
    0x182C, 1, 0x04, 0x14,
    0x2420, 1, 0x04, 0x44, 0x24, 0x14,
    15,
};

extern const VfBackendSpec g_vfBackendLovelace = {
    "lovelace",
    GPU_FAMILY_LOVELACE,
    true, true, true, false,
    0x21537AD4u, 0x507B4B59u, 0x23F1B133u, 0x0733E009u,
    0x1C28, 1, 0x04, 0x24, 0x48, 0x1C,
    0x182C, 1, 0x04, 0x14,
    0x2420, 1, 0x04, 0x44, 0x24, 0x14,
    15,
};

extern const VfBackendSpec g_vfBackendAmpere = {
    "ampere",
    GPU_FAMILY_AMPERE,
    true, true, true, false,
    0x21537AD4u, 0x507B4B59u, 0x23F1B133u, 0x0733E009u,
    0x1C28, 1, 0x04, 0x24, 0x48, 0x1C,
    0x182C, 1, 0x04, 0x14,
    0x2420, 1, 0x04, 0x44, 0x24, 0x14,
    15,
};

extern const VfBackendSpec g_vfBackendTuring = {
    "turing",
    GPU_FAMILY_TURING,
    true, true, true, false,
    0x21537AD4u, 0x507B4B59u, 0x23F1B133u, 0x0733E009u,
    0x1C28, 1, 0x04, 0x24, 0x48, 0x1C,
    0x182C, 1, 0x04, 0x14,
    0x2420, 1, 0x04, 0x44, 0x24, 0x14,
    15,
};

extern const VfBackendSpec g_vfBackendPascal = {
    "pascal",
    GPU_FAMILY_PASCAL,
    true, true, true, false,
    0x21537AD4u, 0x507B4B59u, 0x23F1B133u, 0x0733E009u,
    0x1C28, 1, 0x04, 0x24, 0x48, 0x1C,
    0x182C, 1, 0x04, 0x14,
    0x2420, 1, 0x04, 0x44, 0x24, 0x14,
    15,
};

extern const VfBackendSpec g_vfBackendFuture = {
    "future",
    GPU_FAMILY_UNKNOWN,
    true, true, true, true,
    0x21537AD4u, 0x507B4B59u, 0x23F1B133u, 0x0733E009u,
    0x1C28, 1, 0x04, 0x24, 0x48, 0x1C,
    0x182C, 1, 0x04, 0x14,
    0x2420, 1, 0x04, 0x44, 0x24, 0x14,
    15,
};

static_assert(0x48u + (VF_NUM_POINTS - 1u) * 0x1Cu + 4u <= 0x1C28u, "VF status buffer overflow for shared backend layout");
static_assert(0x04u + 32u <= 0x182Cu, "VF info buffer overflow for shared backend layout");
static_assert(0x44u + (VF_NUM_POINTS - 1u) * 0x24u + 4u <= 0x2420u, "VF control buffer overflow for shared backend layout");

const VfBackendSpec* vf_backend_for_architecture(unsigned int architecture,
                                                 GpuFamily* familyOut) {
    GpuFamily fam = GPU_FAMILY_UNKNOWN;
    const VfBackendSpec* spec = &g_vfBackendFuture;
    switch (architecture) {
        case NV_GPU_ARCHITECTURE_GP100: fam = GPU_FAMILY_PASCAL;    spec = &g_vfBackendPascal;    break;
        case NV_GPU_ARCHITECTURE_TU100: fam = GPU_FAMILY_TURING;    spec = &g_vfBackendTuring;    break;
        case NV_GPU_ARCHITECTURE_GA100: fam = GPU_FAMILY_AMPERE;    spec = &g_vfBackendAmpere;    break;
        case NV_GPU_ARCHITECTURE_AD100: fam = GPU_FAMILY_LOVELACE;  spec = &g_vfBackendLovelace;  break;
        case NV_GPU_ARCHITECTURE_GB200: fam = GPU_FAMILY_BLACKWELL; spec = &g_vfBackendBlackwell; break;
        default:                        fam = GPU_FAMILY_UNKNOWN;   spec = &g_vfBackendFuture;    break;
    }
    if (familyOut) *familyOut = fam;
    return spec;
}

const VfBackendSpec* vf_backend_for_family(GpuFamily family) {
    switch (family) {
        case GPU_FAMILY_PASCAL: return &g_vfBackendPascal;
        case GPU_FAMILY_TURING: return &g_vfBackendTuring;
        case GPU_FAMILY_AMPERE: return &g_vfBackendAmpere;
        case GPU_FAMILY_LOVELACE: return &g_vfBackendLovelace;
        case GPU_FAMILY_BLACKWELL: return &g_vfBackendBlackwell;
        default: return &g_vfBackendFuture;
    }
}

// Lives with the backend tables rather than in the Win32 config shard: it is
// a property of VF backend selection, is needed by both platforms, and its
// old home (config_utils.cpp) is Windows-only.
bool gpu_family_uses_best_guess_backend(GpuFamily family) {
    return family == GPU_FAMILY_UNKNOWN;
}

// Second, independent warning tier.  A family we recognize and have validated
// on discrete boards can still turn up on an integrated part (GB10-class Grace
// Blackwell reports Blackwell): the VF struct layout is correct, so backend
// selection is right and must not change, but the board-level domains a
// discrete card exposes are missing.  The unrecognized-family warning does not
// cover that case and its config key must not silence this one.
//
// Two independent triggers, both positive-evidence-only:
//
//  1. A domain the driver actually refused (the probe is incomplete).
//  2. A recognized DISCRETE family reporting a UNIFIED memory pool — a
//     contradiction that can only mean an integrated part wearing that family's
//     architecture id.  Without this, a GB10-class board that reports Blackwell
//     AND happens to answer every domain would be treated as a validated
//     discrete card and warn about nothing, which is the one way untested
//     silicon could reach a user silently.
//
// Returns false whenever the surface is complete AND the topology is DEDICATED
// or UNKNOWN — i.e. every validated discrete GPU, and every build where the
// probe found nothing. So this still cannot fire on hardware Green Curve
// already supports.
bool gpu_requires_limited_control_warning(GpuFamily family,
                                          const GpuCapabilityProbe* probe) {
    if (gpu_family_uses_best_guess_backend(family)) return false;
    if (gpu_capability_topology_contradicts_discrete(probe)) return true;
    return !gpu_capability_is_complete(probe);
}
