// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Decode the graphics VF control table's per-point deltas into the cached
// offset arrays.  One decoder serves the plain reader (nvapi_read_offsets) and
// the verified writer, which adopts the table it has just read at entry so its
// convergence judgement never consults a cache that another tool, or a driver
// reset, has invalidated since the last read (gpu_backend_curve_write.h).

#ifndef GREEN_CURVE_VF_CONTROL_OFFSETS_DECODE_H
#define GREEN_CURVE_VF_CONTROL_OFFSETS_DECODE_H

#include "vf_curve_units.h"
#include <string.h>

// Returns whether every entry decoded to an exact Hz value, i.e. what the
// caller publishes as `vfOffsetsHzValid`.  An entry outside the buffer decodes
// to 0 and makes the whole table inexact; an entry whose delta has no exact Hz
// value leaves that Hz slot untouched, exactly as the reader always did.
static inline bool vf_control_offsets_decode(const VfBackendSpec* b,
    const unsigned char* buf, size_t bufSize, int* offsetsKHz, gc_i64* offsetsHz) {
    if (!b || !buf || !offsetsKHz || !offsetsHz) return false;
    bool exact = true;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        const size_t deltaOffset = (size_t)b->controlEntryBaseOffset +
            (size_t)i * b->controlEntryStride + b->controlEntryDeltaOffset;
        int delta = 0;
        if (deltaOffset + sizeof(delta) > bufSize) {
            offsetsKHz[i] = 0;
            offsetsHz[i] = 0;
            exact = false;
            continue;
        }
        memcpy(&delta, buf + deltaOffset, sizeof(delta));
        offsetsKHz[i] = vf_graphics_offset_khz(b, delta);
        if (!vf_graphics_offset_hz(b, delta, &offsetsHz[i])) exact = false;
    }
    return exact;
}

// What adopting a fresh table changed relative to the cache it replaced,
// restricted to the points a write is about to target.  Pure, so the
// diagnostic and the "re-read the curve" decision are testable.
struct VfControlAdoptionDiff {
    int changed = 0;
    int firstCi = -1;
    int firstCachedKHz = 0;
    int firstLiveKHz = 0;
};

static inline VfControlAdoptionDiff vf_control_adoption_diff(
    const int* cachedKHz, const int* liveKHz, const bool* mask) {
    VfControlAdoptionDiff diff;
    if (!cachedKHz || !liveKHz || !mask) return diff;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!mask[i] || cachedKHz[i] == liveKHz[i]) continue;
        if (diff.changed++ == 0) {
            diff.firstCi = i;
            diff.firstCachedKHz = cachedKHz[i];
            diff.firstLiveKHz = liveKHz[i];
        }
    }
    return diff;
}

#endif  // GREEN_CURVE_VF_CONTROL_OFFSETS_DECODE_H
