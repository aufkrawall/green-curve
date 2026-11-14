// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include "gpu_core.h"

// An explicit edit replaces authority even when the displayed integer stayed
// the same. Preserve a named memory API, rather than silently changing routes.
template<class D> static inline bool desired_memory_edit_physical_mhz(D* d, int mhz) {
    const long long hz = (long long)mhz * 1000000;
    if (!d || mhz < -3000 || mhz > 3000 || d->memOffsetPrecise > 1) return false;
    if (d->hasMemOffset && d->memOffsetPrecise)
        return desired_memory_set_precise(d, hz, d->memOffsetSurface);
    d->hasMemOffset = 1; d->memOffsetMHz = mhz;
    desired_memory_precision_clear(d);
    return true;
}
template<class D> static inline bool desired_power_edit_percent(D* d, int percent) {
    if (!d || percent != clamp_power_limit_pct(percent) || d->powerLimitRatio > 1) return false;
    if (d->hasPowerLimit && d->powerLimitRatio)
        return desired_power_set_ratio(d, tuning_power_ratio_from_percent(percent));
    d->hasPowerLimit = 1; d->powerLimitPct = percent;
    desired_power_precision_clear(d);
    return true;
}
