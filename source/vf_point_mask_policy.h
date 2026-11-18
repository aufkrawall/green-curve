// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// vf_point_mask_policy.h — the point mask Green Curve sends with a VF STATUS
// read, derived from the 256-bit mask VF-info returns.  Pure; shared by the
// Windows reader, the Linux atomic reader and probe, and passive capture.
//
// VF-info advertises every point the driver's tables hold, across all clock
// domains: an RTX 5070 reports 132 (the graphics curve, two short domains and
// the memory domain at 129..131).  Green Curve's point model is VF_NUM_POINTS
// (128) wide on every platform, so no reader ever stores a status entry at or
// past index 128.
//
// A status request therefore names points 0..127 only.  Most drivers accept the
// full advertised mask, but one that cannot serve a requested point refuses the
// WHOLE call: an NVIDIA CMP 40HX on a modified driver advertises 131 points and
// returns no curve at all until the request stops at 127.  The left-out bits
// were never read, so bounding the request loses nothing on any board.
//
// VF CONTROL keeps the driver's full mask.  It accepts that mask on every board
// observed, the CMP 40HX included, and a write never names a point past 127.

#ifndef GREEN_CURVE_VF_POINT_MASK_POLICY_H
#define GREEN_CURVE_VF_POINT_MASK_POLICY_H

static inline bool vf_mask_point_set(const unsigned char* mask,
                                     unsigned int maskBytes,
                                     unsigned int point) {
    if (!mask || point / 8u >= maskBytes) return false;
    return (mask[point / 8u] & (unsigned char)(1u << (point % 8u))) != 0;
}

static inline unsigned int vf_mask_point_count(const unsigned char* mask,
                                               unsigned int maskBytes) {
    unsigned int count = 0;
    for (unsigned int point = 0; mask && point < maskBytes * 8u; ++point) {
        if (vf_mask_point_set(mask, maskBytes, point)) ++count;
    }
    return count;
}

// Advertised points at or past `tablePoints`: what a status request leaves out.
static inline unsigned int vf_mask_points_past_table(const unsigned char* mask,
                                                    unsigned int maskBytes,
                                                    unsigned int tablePoints) {
    unsigned int count = 0;
    for (unsigned int point = tablePoints; mask && point < maskBytes * 8u; ++point) {
        if (vf_mask_point_set(mask, maskBytes, point)) ++count;
    }
    return count;
}

// Highest advertised point index, or -1 for an empty mask.
static inline int vf_mask_highest_point(const unsigned char* mask,
                                        unsigned int maskBytes) {
    for (unsigned int point = maskBytes * 8u; mask && point-- > 0;) {
        if (vf_mask_point_set(mask, maskBytes, point)) return (int)point;
    }
    return -1;
}

// Copies `mask` into `requestOut` (both `maskBytes` long) without the points at
// or past `tablePoints`, and returns how many advertised points it left out.
static inline unsigned int vf_status_request_mask(const unsigned char* mask,
                                                  unsigned char* requestOut,
                                                  unsigned int maskBytes,
                                                  unsigned int tablePoints) {
    if (!requestOut) return 0;
    unsigned int leftOut = 0;
    for (unsigned int byte = 0; byte < maskBytes; ++byte) {
        unsigned char kept = 0;
        for (unsigned int bit = 0; bit < 8u; ++bit) {
            const unsigned int point = byte * 8u + bit;
            if (!vf_mask_point_set(mask, maskBytes, point)) continue;
            if (point < tablePoints) kept |= (unsigned char)(1u << bit);
            else ++leftOut;
        }
        requestOut[byte] = kept;
    }
    return leftOut;
}

#endif // GREEN_CURVE_VF_POINT_MASK_POLICY_H
