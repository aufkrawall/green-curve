// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// vf_backends.h — per-GPU-family VF-curve backend dispatch tables, shared by
// the Windows and Linux backends so the private-NVAPI struct layouts and
// entry-point IDs are defined exactly once.  The tables describe, per family,
// the NVAPI function IDs and buffer offsets used to read/write the
// voltage-frequency curve; they are identical driver-ABI constants on every OS.

#ifndef GREEN_CURVE_VF_BACKENDS_H
#define GREEN_CURVE_VF_BACKENDS_H

#include "gpu_core.h"
#include "gpu_capability_policy.h"

extern const VfBackendSpec g_vfBackendBlackwell;
extern const VfBackendSpec g_vfBackendLovelace;
extern const VfBackendSpec g_vfBackendAmpere;
extern const VfBackendSpec g_vfBackendTuring;
extern const VfBackendSpec g_vfBackendPascal;
extern const VfBackendSpec g_vfBackendFuture;

// Pure architecture -> backend-spec selection (no global state).  Mirrors the
// Windows select_vf_backend_for_current_gpu() mapping.  `familyOut` is optional.
// Defined in vf_backends.cpp (shared by both platforms). app_shared.h repeats
// this declaration for the Windows translation units that do not include this
// header; keep the two in sync.
bool gpu_family_uses_best_guess_backend(GpuFamily family);

// True only for a recognized family whose probed control surface is incomplete
// (the integrated-SoC case).  Never true for an unprobed or fully-available
// GPU, so it cannot fire on validated discrete hardware.
bool gpu_requires_limited_control_warning(GpuFamily family,
                                          const GpuCapabilityProbe* probe);

const VfBackendSpec* vf_backend_for_architecture(unsigned int architecture,
                                                  GpuFamily* familyOut);
const VfBackendSpec* vf_backend_for_family(GpuFamily family);

#endif // GREEN_CURVE_VF_BACKENDS_H
