// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CURVE_ORIGIN_IO_H
#define GREEN_CURVE_PROFILE_CURVE_ORIGIN_IO_H

// Reading per-point values out of a Windows profile's curve section.
//
// Header-only and free of g_app on purpose: this is the half of the saved-curve
// format the regression harness can actually execute against a real INI file.
// The half that needs live GPU state (what a point's voltage and offset were at
// save time) stays in config_profile_curve_format.cpp.

#include "profile_curve_semantics.h"

static bool read_profile_point_int(const char* path, const char* section, int pointIndex, const char* suffix, int* valueOut) {
    if (valueOut) *valueOut = 0;
    if (!path || !section || !suffix || pointIndex < 0 || pointIndex >= VF_NUM_POINTS) return false;

    char key[64] = {};
    char buf[64] = {};
    StringCchPrintfA(key, ARRAY_COUNT(key), "point%d_%s", pointIndex, suffix);
    gc_GetPrivateProfileStringUtf8(section, key, "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (!buf[0]) return false;

    int value = 0;
    if (!parse_int_strict(buf, &value)) return false;
    if (valueOut) *valueOut = value;
    return true;
}

static ProfileCurveDecode profile_curve_section_decode(const char* path,
                                                       const char* section) {
    if (!path || !section) return PROFILE_CURVE_DECODE_UNMARKED;
    char semanticsBuf[64] = {};
    gc_GetPrivateProfileStringUtf8(section, "curve_semantics", "", semanticsBuf,
                                   sizeof(semanticsBuf), path);
    trim_ascii(semanticsBuf);
    return profile_curve_decode_from_marker(semanticsBuf);
}

// Read the per-point provenance an `absolute_with_origin` section carries.
// Returns false when the section is not in that format, so the caller falls
// through to the legacy whole-section base+offset reconstruction.
//
// Every point defaults to NOT projected: absence of the key is the typed case,
// and that default is the whole reason a hand-edited point keeps its authority
// across a save.  A point the loader did not populate is skipped rather than
// flagged, so a stale key for a point this GPU has no row for cannot resurrect
// provenance for a value that is not there.
static bool restore_curve_point_origins_from_section(const char* path,
                                                     const char* section,
                                                     DesiredSettings* desired) {
    if (!path || !section || !desired) return false;
    if (profile_curve_section_decode(path, section) !=
        PROFILE_CURVE_DECODE_ABSOLUTE_WITH_ORIGIN)
        return false;

    int projected = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        desired->curvePointFromGpuOffset[i] = 0;
        if (!desired->hasCurvePoint[i]) continue;
        int value = 0;
        if (!read_profile_point_int(path, section, i,
                                    PROFILE_CURVE_POINT_ORIGIN_SUFFIX, &value))
            continue;
        if (value == 0) continue;
        desired->curvePointFromGpuOffset[i] = gc_bool8_from_bool(true);
        projected++;
    }
    debug_log("profile curve format: section [%s] is %s; %d projected point(s) "
              "kept as offset intent, the rest hold their absolute MHz\n",
              section, PROFILE_CURVE_SEMANTICS_ABSOLUTE_WITH_ORIGIN, projected);
    return true;
}

#endif
