// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_DESIRED_TOPOLOGY_BINDING_H
#define GREEN_CURVE_DESIRED_TOPOLOGY_BINDING_H
#include "vf_writable_point_policy.h"
#include <stdio.h>

// Runtime indices are valid only under their complete voltage binding. After
// a topology change, map authority by unique exact voltage or refuse. Measured
// frequencies and imported source indices never disambiguate equal voltages.
static inline bool desired_rebind_topology(const DesiredSettings& source,
    const VFCurvePoint* curve, const VfFloorTopology& floor,
    DesiredSettings* output, char* why, size_t whySize, int* remapped = nullptr) {
    const auto fail = [&](const char* text, int ci = -1) {
        if (why && whySize) snprintf(why, whySize, "topology binding: %s (point=%d)", text, ci);
        return false;
    };
    if (!output || !desired_authoring_valid(&source) || !desired_curve_intent_valid(&source))
        return fail("invalid source intent");
    if (!source.authoring.topologyBound) {
        *output = source; if (remapped) *remapped = 0; return true;
    }
    if (!curve || !floor.valid || floor.graphicsEnd <= 0 || floor.graphicsEnd > VF_NUM_POINTS)
        return fail("fresh graphics topology is unavailable");
    gc_i64 voltages[VF_NUM_POINTS]{};
    gc_u32 count = 0;
    for (int i = 0; i < floor.graphicsEnd; ++i) if (curve[i].freq_kHz) {
        if (!curve[i].volt_uV) return fail("destination voltage is unavailable", i);
        voltages[i] = curve[i].volt_uV; ++count;
    }
    const bool completeMatch = count == source.authoring.topologyCount &&
        !memcmp(voltages, source.authoring.voltageUv, sizeof(voltages));
    DesiredSettings next = source;
    for (int i = 0; i < VF_NUM_POINTS; ++i) desired_curve_point_clear(&next, i);
    memset(next.authoring.graphicsExcluded, 0, sizeof(next.authoring.graphicsExcluded));
    bool assigned[VF_NUM_POINTS]{};
    int moved = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        const bool point = desired_curve_point_present(&source, i);
        const bool anchor = source.hasLock && source.lockMode != LOCK_MODE_NONE && source.lockCi == i;
        const bool exclusion = source.authoring.identityExclusions && source.authoring.graphicsExcluded[i];
        if (!point && !anchor && !exclusion) continue;
        int selected = completeMatch ? i : -1, matches = completeMatch ? 1 : 0;
        if (!completeMatch) for (int j = 0; j < floor.graphicsEnd; ++j)
            if (voltages[j] && voltages[j] == source.authoring.voltageUv[i]) { selected = j; ++matches; }
        if (matches != 1) return fail(matches ? "ambiguous destination voltage" : "missing destination voltage", i);
        if (assigned[selected]) return fail("multiple identities select one destination", i);
        assigned[selected] = true;
        if ((point || anchor) && vf_point_target_status(floor, curve, selected) != VF_POINT_TARGET_OK)
            return fail("destination point is not writable", selected);
        if (point) desired_curve_point_copy_to(&next, selected, &source, i);
        if (anchor) next.lockCi = selected;
        if (exclusion) next.authoring.graphicsExcluded[selected] = 1;
        if (selected != i) ++moved;
    }
    memcpy(next.authoring.voltageUv, voltages, sizeof(voltages));
    next.authoring.topologyCount = count;
    if (!desired_authoring_valid(&next) || !desired_curve_intent_valid(&next))
        return fail("mapped authority conflicts with the destination lock");
    *output = next; if (remapped) *remapped = moved;
    return true;
}
#endif
