// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// gpu_support_warning_policy.h — whether the GUI startup warning tiers apply,
// judged from the selected adapter as the service snapshot reports it.  Pure.
//
// The GUI shows these warnings only while the background service is available,
// and in that state it never calls NvAPI itself: g_app.vfBackend stays null in
// the GUI process (only the service, --self-test and --probe resolve a VF
// backend).  A tier keyed on g_app.vfBackend therefore can never fire there.
// What the GUI does hold is the snapshot's selected adapter (identity, family,
// best-guess flag) and the decoded capability probe; the tiers use only those.

#ifndef GREEN_CURVE_GPU_SUPPORT_WARNING_POLICY_H
#define GREEN_CURVE_GPU_SUPPORT_WARNING_POLICY_H

#include "gpu_core.h"
#include "vf_backends.h"

// Unrecognized family: the service selected the best-effort fallback layout.
static inline bool gpu_warning_unrecognized_family_applies(const GpuAdapterInfo* selected) {
    return selected && selected->valid && selected->vfBestGuess;
}

// Reduced control surface: a recognized family whose board-level domains are
// incomplete, or which reports unified memory (gpu_requires_limited_control_warning).
static inline bool gpu_warning_limited_control_applies(const GpuAdapterInfo* selected,
                                                       const GpuCapabilityProbe* probe) {
    if (!selected || !selected->valid || selected->vfBestGuess) return false;
    return gpu_requires_limited_control_warning(selected->family, probe);
}

#endif // GREEN_CURVE_GPU_SUPPORT_WARNING_POLICY_H
