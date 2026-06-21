// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// vf_backends.cpp — definitions of the per-family VF-curve backend tables.
// Extracted from main.cpp so both the Windows and Linux backends link against
// one copy.  Pascal, Turing, Ampere, Lovelace, and Blackwell are treated as
// tested known families.  Only the "future" fallback uses the same layout as a
// best-effort guess (bestGuessOnly = true).

#include "vf_backends.h"

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
