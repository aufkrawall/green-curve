// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_CURVE_SEMANTICS_H
#define GREEN_CURVE_PROFILE_CURVE_SEMANTICS_H

// What `[<profile>_curve] curve_semantics=` means, for both platforms.
//
// A saved VF point carries two things: a number, and what the number IS.  Only
// the second one was ever missing.
//
// `base_plus_gpu_offset` (the format through 0.25.2) stored every visible point
// as `absolute - offsetComponent(point)` and reconstructed the absolute on
// load.  That is a WHOLE-SECTION statement, so it cannot express the profile
// the 0.26.0 model is built around: load an offset profile, hand-edit one
// point, and that point is a real absolute target while its neighbours are
// still projections of the GPU offset over a stock base.  Saving flattened the
// difference -- and the loader then marked every restored point offset-derived,
// so the hand-typed number quietly went back to tracking a base that moves with
// load.  A value the user typed must keep its authority across a save.
//
// `absolute_with_origin` stores the ABSOLUTE MHz for every point plus a
// per-point `pointN_from_gpu_offset=1` for the projected ones.  Nothing is lost
// in either direction:
//
//   - A projected point's absolute is only ever a preview anyway.  The apply
//     path derives its offset from the request's own gpuOffsetMHz and never
//     from the stored number (curve_point_offset_policy.h), so re-projection
//     over a different base still happens, and still happens per point.
//   - A typed point keeps its absolute AND its authority, because the flag that
//     would have surrendered them is simply absent.
//
// It is also the safer file to hand to an older build: an unrecognized
// `curve_semantics` value makes every loader here fall through to "the stored
// numbers are absolute", which is exactly right for the typed points and
// exactly what 0.25.2 did with every point anyway.  Writing base MHz under a
// marker an old build recognizes is what would have been dangerous -- it would
// re-add an offset the new file no longer subtracted.
#define PROFILE_CURVE_SEMANTICS_ABSOLUTE_WITH_ORIGIN "absolute_with_origin"
#define PROFILE_CURVE_SEMANTICS_BASE_PLUS_GPU_OFFSET "base_plus_gpu_offset"

// Per-point provenance key suffix, written only for points that ARE projected:
// absence means "typed absolute", which keeps a file that has no projections at
// all byte-identical to what the previous format produced for it.
#define PROFILE_CURVE_POINT_ORIGIN_SUFFIX "from_gpu_offset"

// How a section's stored point values must be read.  One answer for both
// platforms, because the two loaders used to spell the same test differently
// and a disagreement here is a curve written at the wrong frequency.
enum ProfileCurveDecode : int {
    // No marker at all.  Pre-`curve_semantics` file; the caller applies its own
    // compatibility heuristic (Windows) or its own clearing rule (Linux),
    // because what an unmarked file meant differed per platform.
    PROFILE_CURVE_DECODE_UNMARKED = 0,
    // Stored MHz are absolute and carry no provenance.  Also the answer for a
    // marker this build does not know, which is what makes a profile written by
    // a NEWER build safe to read: absolute is the conservative reading, and
    // adding an offset to numbers that never had one subtracted is not.
    PROFILE_CURVE_DECODE_ABSOLUTE = 1,
    // Stored MHz are absolute, plus `pointN_from_gpu_offset` for projections.
    PROFILE_CURVE_DECODE_ABSOLUTE_WITH_ORIGIN = 2,
    // Legacy whole-section format: stored MHz are BASE, so the section's GPU
    // offset component has to be added back, and every restored point is
    // necessarily offset-derived because the file cannot say otherwise.
    PROFILE_CURVE_DECODE_BASE_PLUS_GPU_OFFSET = 3,
};

static inline ProfileCurveDecode profile_curve_decode_from_marker(
    const char* semantics) {
    if (!semantics || !semantics[0]) return PROFILE_CURVE_DECODE_UNMARKED;
    if (streqi_ascii(semantics, PROFILE_CURVE_SEMANTICS_ABSOLUTE_WITH_ORIGIN))
        return PROFILE_CURVE_DECODE_ABSOLUTE_WITH_ORIGIN;
    if (streqi_ascii(semantics, PROFILE_CURVE_SEMANTICS_BASE_PLUS_GPU_OFFSET))
        return PROFILE_CURVE_DECODE_BASE_PLUS_GPU_OFFSET;
    return PROFILE_CURVE_DECODE_ABSOLUTE;
}

#endif
