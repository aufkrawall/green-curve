// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_PROFILE_CURVE_CODEC_H
#define GREEN_CURVE_LINUX_PROFILE_CURVE_CODEC_H

// Reading and writing the per-point half of a Linux profile's `[*_curve]`
// section.  Split out of linux_port_profiles.cpp, which is on its size ratchet.
// The Windows counterpart is config_profile_curve_format.cpp; both obey
// profile_curve_semantics.h, which is the one place the format is described.

#include "profile_curve_semantics.h"

// Read the per-point provenance of an `absolute_with_origin` section.  Returns
// false when the section is not in that format, so the caller falls through to
// the legacy whole-section base+offset reconstruction.
//
// Absence of a point's key means "typed absolute", which is why nothing here
// has to clear the flags: the caller's point loader already did, and a file
// that predates the key has no projections to describe.
static inline bool linux_profile_read_curve_point_origins(
    const IniDocument* doc, const char* curveSection,
    const std::string& curveSemantics, DesiredSettings* desired) {
    if (!doc || !curveSection || !desired) return false;
    if (profile_curve_decode_from_marker(curveSemantics.c_str()) !=
        PROFILE_CURVE_DECODE_ABSOLUTE_WITH_ORIGIN)
        return false;
    int projected = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desired->hasCurvePoint[i]) continue;
        char originKey[48] = {};
        snprintf(originKey, sizeof(originKey), "point%d_%s", i,
                 PROFILE_CURVE_POINT_ORIGIN_SUFFIX);
        std::string originValue = get_section_value(doc, curveSection, originKey);
        int parsedOrigin = 0;
        if (originValue.empty() ||
            !parse_int_strict(originValue.c_str(), &parsedOrigin) ||
            parsedOrigin == 0)
            continue;
        desired->curvePointFromGpuOffset[i] = gc_bool8_from_bool(true);
        projected++;
    }
    linux_debug_logf("profile: [%s] is %s; %d projected point(s) kept as offset "
                     "intent, the rest hold their absolute MHz\n",
                     curveSection, PROFILE_CURVE_SEMANTICS_ABSOLUTE_WITH_ORIGIN,
                     projected);
    return true;
}

// Append the curve points of an `absolute_with_origin` section: the ABSOLUTE
// MHz for every owned point, plus `pointN_from_gpu_offset=1` for the projected
// ones.  The previous format subtracted a whole-section GPU-offset component
// and declared every point offset-derived on the way back in -- which is how a
// hand-typed point lost its authority across a single save.
static inline void linux_profile_write_curve_points(
    const char* curveSection, const DesiredSettings* desired,
    std::vector<IniEntry>* curveEntries) {
    if (!curveSection || !desired || !curveEntries) return;
    IniEntry semanticsEntry;
    semanticsEntry.key = "curve_semantics";
    semanticsEntry.value = PROFILE_CURVE_SEMANTICS_ABSOLUTE_WITH_ORIGIN;
    curveEntries->push_back(semanticsEntry);

    char value[32] = {};
    int savedProjectedPoints = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!desired->hasCurvePoint[i] || desired->curvePointMHz[i] == 0) continue;
        IniEntry entry;
        char key[32] = {};
        snprintf(key, sizeof(key), "point%d", i);
        entry.key = key;
        snprintf(value, sizeof(value), "%u", desired->curvePointMHz[i]);
        entry.value = value;
        curveEntries->push_back(entry);
        if (!desired->curvePointFromGpuOffset[i]) continue;
        IniEntry originEntry;
        char originKey[48] = {};
        snprintf(originKey, sizeof(originKey), "point%d_%s", i,
                 PROFILE_CURVE_POINT_ORIGIN_SUFFIX);
        originEntry.key = originKey;
        originEntry.value = "1";
        curveEntries->push_back(originEntry);
        savedProjectedPoints++;
    }
    linux_debug_logf("profile: [%s] written as %s (gpuOffset=%d excludeLow=%d "
                     "projectedPoints=%d)\n",
                     curveSection, PROFILE_CURVE_SEMANTICS_ABSOLUTE_WITH_ORIGIN,
                     desired->gpuOffsetMHz, desired->gpuOffsetExcludeLowCount,
                     savedProjectedPoints);
}

#endif
