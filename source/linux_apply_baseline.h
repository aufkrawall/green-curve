// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_APPLY_BASELINE_H
#define GREEN_CURVE_LINUX_APPLY_BASELINE_H

// Baseline preparation affects graphics/VF and explicitly included auxiliary
// controls. Memory moves directly to its requested value in its own phase.
static bool linux_apply_reset_baseline(LinuxGpuState* g, const DesiredSettings* d) {
    if (!nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, 0) ||
        (!g->nvml.resetGpuLockedClocks &&
         !linux_clock_control_proven_absent(g)))
        return false;
    if (!linux_apply_reset_baseline_locked_clocks(g, d)) return false;
    if ((d->hasXbarOffsetKhz || d->hasXbarMsvddOffsetUv) &&
        !linux_xbar_write_owned(g, 0, 0, d->hasXbarOffsetKhz,
                               d->hasXbarMsvddOffsetUv)) return false;
    if (d->hasSysClkOffsetKhz &&
        !linux_xbar_write_entry(g, XBAR_PINNED_SYS_ENTRY_INDEX, 0))
        return false;
    if (d->hasVideoClkOffsetKhz &&
        !linux_xbar_write_entry(g, XBAR_PINNED_VIDEO_ENTRY_INDEX, 0))
        return false;
    lb_log("apply baseline: preserving memory offset (memory requested=%d)\n",
        d->hasMemOffset ? 1 : 0);
    return linux_apply_reset_baseline_vf_curve(g);
}

#endif
