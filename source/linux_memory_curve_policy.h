// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_MEMORY_CURVE_POLICY_H
#define GREEN_CURVE_LINUX_MEMORY_CURVE_POLICY_H
#include "curve_preservation_precision.h"

static inline bool linux_memory_curve_precheck(const LinuxGpuState* g, const DesiredSettings* d,
    const LinuxHardwareSnapshot* snapshot, char* why, size_t size) {
    if (!d->hasMemOffset || d->resetOcBeforeApply || !snapshot->curveValid) return true;
    if (!snapshot->curveOffsetsExact || !g->backend || !g->backend->writeSupported) {
        gc_strlcpy(why, size, "exact VF controls cannot be preserved across the memory change"); return false;
    }
    bool mask[VF_NUM_POINTS]{};
    for (int i = 0; i < VF_NUM_POINTS; ++i) if (snapshot->curveMask[i]) {
        mask[i] = (g->vfMask[i / 8] & (1u << (i % 8))) != 0;
        if (!mask[i] && snapshot->curveOffsetsHz[i]) {
            gc_snprintf(why, size, "read-only VF point %d cannot be preserved across the memory change", i); return false;
        }
    }
    const VfOffsetRange range = vf_offset_range_from_probe(g->curveOffsetRangeKnown,
        g->curveOffsetMinKHz, g->curveOffsetMaxKHz);
    int bad = -1;
    if (!curve_exact_targets_representable(g->backend, range, mask, snapshot->curveOffsetsHz, &bad)) {
        gc_snprintf(why, size, "existing VF point %d is not representable for memory preservation", bad); return false;
    }
    return true;
}
#endif
