// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ---------------------------------------------------------------------------
// How a VF curve point is written to, and read back from, a Windows profile.
//
// Three call sites persist a curve section -- the slot save and the legacy
// slot-1 save in config_profiles.cpp, and the installer settings-transfer
// export in main_runtime_capture.cpp -- and all three had their own copy of the
// same decision.  When the decision gained per-point provenance, one copy would
// have been enough to lose it again, so the decision lives here once and the
// call sites only do their own formatted append.
//
// See profile_curve_semantics.h for what the stored numbers mean.
// ---------------------------------------------------------------------------

#include "profile_curve_semantics.h"

// Everything a single saved point needs, resolved in one place.
struct ProfileCurvePointRecord {
    bool present;
    unsigned int mhz;         // ABSOLUTE MHz, never a base
    unsigned int voltMv;
    int offsetKHz;
    bool visible;
    bool fromGpuOffset;       // projection of the GPU offset, not a typed value
};

// `useCurrentForUnset` is the installer export's rule: a point the request does
// not own is exported at its live frequency so the receiving install starts
// from the same curve.  A point filled in that way was never typed, but it was
// not projected from THIS request's offset either -- it is simply readback, and
// readback is an absolute.
static bool profile_curve_point_record_for_save(const DesiredSettings* desired,
                                                int pointIndex,
                                                bool useCurrentForUnset,
                                                ProfileCurvePointRecord* out) {
    if (!out) return false;
    ProfileCurvePointRecord record = {};
    if (pointIndex < 0 || pointIndex >= VF_NUM_POINTS) {
        *out = record;
        return false;
    }
    bool owned = desired && desired->hasCurvePoint[pointIndex];
    if (owned) {
        record.mhz = desired->curvePointMHz[pointIndex];
        record.fromGpuOffset = desired->curvePointFromGpuOffset[pointIndex] != 0;
    } else if (useCurrentForUnset && g_app.curve[pointIndex].freq_kHz > 0) {
        record.mhz = displayed_curve_mhz(g_app.curve[pointIndex].freq_kHz);
    }
    if (record.mhz == 0) {
        *out = record;
        return false;
    }
    record.present = true;
    record.voltMv = g_app.curve[pointIndex].volt_uV / 1000;
    record.offsetKHz = g_app.curve[pointIndex].freq_kHz > 0
        ? g_app.freqOffsets[pointIndex] : 0;
    record.visible = is_curve_point_visible_in_gui(pointIndex);
    // A point the GPU reports no voltage for cannot answer the visibility
    // question, so an owned point asserts its own visibility instead of being
    // written out as hidden and skipped on the next load.
    if (g_app.curve[pointIndex].volt_uV == 0) record.visible = owned;
    *out = record;
    return true;
}

// The reader half lives in profile_curve_origin_io.h, where the regression
// harness can run it against a real INI file: it needs no live GPU state, and
// the round trip that this whole change exists to preserve is exactly the thing
// a test has to be able to execute.
