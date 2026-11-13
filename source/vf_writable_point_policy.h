// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_VF_WRITABLE_POINT_POLICY_H
#define GREEN_CURVE_VF_WRITABLE_POINT_POLICY_H

// Which graphics VF points can carry a write, shared by Windows and Linux for
// lock-anchor validation, explicit-target validation, editor listing and
// draft/slot retention.
//
// Two rules combine:
//
//  1. Every populated graphics point at or above MIN_VISIBLE_VOLT_mV is listed
//     (vf_point_visible_in_editor(), voltage only).  This is the editor rule
//     every existing slot was saved under; keeping it as a floor of the listed
//     set means a point that was editable before stays editable, whatever the
//     stock frequencies do.
//  2. Below that voltage, a point is listed only when it is WRITABLE: outside
//     the driver's flat low-voltage floor.  The floor is a leading run of at
//     least VF_MIN_FLAT_FLOOR_RUN populated points that share one stock base
//     frequency at rising voltage (an RTX 5070 reports points 0..44 at
//     180 MHz).  Any target written there is clamped back to the floor, so a
//     floor point is never a lock anchor, an explicit target or a retained
//     draft point.  Rising points below 700 mV (Pascal's point 22 at ~662 mV)
//     are ordinary curve points: hiding them hid the very point that blocked
//     a flatten.
//
// The listed set is the union of two suffixes of the graphics domain, so it is
// itself a contiguous suffix: the editor's visible-index order stays the
// physical voltage order.
//
// The floor's END moves with temperature: the RTX 5070 ramp starts about two
// points later on a cold read.  The boundary is therefore decided once per
// topology binding (VfFloorBinding: membership and voltages, never
// frequencies) and kept until the topology changes, so a later warm read
// cannot add or remove a point.  On the RTX 5070 the floor ends above 700 mV
// either way, so its listed set is exactly the voltage rule's.

#include "gpu_core.h"
#include "vf_curve_units.h"
#include "vf_point_visibility_policy.h"

// Shortest leading run of equal stock base MHz that counts as a driver floor
// rather than an ordinary curve.  Real floors are dozens of points; this only
// has to be high enough that a normal rising curve is never trimmed.
#define VF_MIN_FLAT_FLOOR_RUN 4

struct VfFloorTopology {
    bool valid;          // a populated graphics domain was seen
    int graphicsEnd;     // exclusive end of the graphics domain
    int firstPopulated;  // first populated graphics index, -1 when none
    int floorEnd;        // first index outside the floor (firstPopulated when no floor)
    int floorRun;        // populated points inside the floor, 0 when there is none
    int floorMHz;        // the floor's stock base MHz, 0 when there is none
};

// Stock base of one point in display MHz: live frequency minus the offset the
// driver reports as programmed there, rounded half away from zero.
static inline int vf_point_base_mhz(unsigned int freqKHz, int offsetKHz) {
    const long long baseKHz = (long long)freqKHz - (long long)offsetKHz;
    return (int)((baseKHz >= 0 ? baseKHz + 500 : baseKHz - 500) / 1000);
}

// `offsetsKHz` may be null (every offset zero).  Pure; no hysteresis.
static inline VfFloorTopology vf_floor_topology_compute(
    const VFCurvePoint* curve, const int* offsetsKHz, int count) {
    VfFloorTopology t = {};
    t.firstPopulated = -1;
    if (!curve || count <= 0) return t;
    if (count > VF_NUM_POINTS) count = VF_NUM_POINTS;
    t.graphicsEnd = vf_graphics_domain_length(curve, count);
    int run = 0;
    int floorMHz = 0;
    int bound = -1;
    for (int i = 0; i < t.graphicsEnd; ++i) {
        if (curve[i].freq_kHz == 0) continue;
        const int base = vf_point_base_mhz(curve[i].freq_kHz,
                                           offsetsKHz ? offsetsKHz[i] : 0);
        if (t.firstPopulated < 0) {
            t.firstPopulated = i;
            floorMHz = base;
            run = 1;
            continue;
        }
        if (base != floorMHz) {
            bound = i;
            break;
        }
        ++run;
    }
    if (t.firstPopulated < 0) return t;
    t.valid = true;
    t.floorEnd = t.firstPopulated;
    // A curve with a single value throughout has no rising part to bound the
    // run, and a short run is an ordinary curve start: neither is a floor.
    if (bound < 0 || run < VF_MIN_FLAT_FLOOR_RUN) return t;
    t.floorEnd = bound;
    t.floorRun = run;
    t.floorMHz = floorMHz;
    return t;
}

static inline bool vf_point_in_graphics_domain(const VfFloorTopology& t,
                                               const VFCurvePoint* curve, int ci) {
    return t.valid && curve && ci >= 0 && ci < VF_NUM_POINTS &&
        ci < t.graphicsEnd && curve[ci].freq_kHz != 0;
}

static inline bool vf_point_in_flat_floor(const VfFloorTopology& t,
                                          const VFCurvePoint* curve, int ci) {
    return vf_point_in_graphics_domain(t, curve, ci) && t.floorRun > 0 &&
        ci < t.floorEnd;
}

// The editor list, the draft and saved-slot retention, and the set a lock
// anchor or an explicit target must belong to.
static inline bool vf_point_listed(const VfFloorTopology& t,
                                   const VFCurvePoint* curve, int ci) {
    if (!vf_point_in_graphics_domain(t, curve, ci)) return false;
    if (vf_point_visible_in_editor(curve[ci].freq_kHz, curve[ci].volt_uV))
        return true;
    return !vf_point_in_flat_floor(t, curve, ci);
}

enum VfPointTargetStatus {
    VF_POINT_TARGET_OK = 0,
    VF_POINT_TARGET_INDEX_INVALID = 1,
    VF_POINT_TARGET_UNPOPULATED = 2,
    VF_POINT_TARGET_FOREIGN_DOMAIN = 3,
    VF_POINT_TARGET_FLOOR = 4,
};

static inline const char* vf_point_target_status_name(VfPointTargetStatus status) {
    switch (status) {
        case VF_POINT_TARGET_OK: return "ok";
        case VF_POINT_TARGET_INDEX_INVALID: return "index-invalid";
        case VF_POINT_TARGET_UNPOPULATED: return "unpopulated";
        case VF_POINT_TARGET_FOREIGN_DOMAIN: return "foreign-domain";
        case VF_POINT_TARGET_FLOOR: return "flat-floor";
        default: return "unknown";
    }
}

// May `ci` be a lock anchor or carry an explicit target?
static inline VfPointTargetStatus vf_point_target_status(
    const VfFloorTopology& t, const VFCurvePoint* curve, int ci) {
    if (ci < 0 || ci >= VF_NUM_POINTS) return VF_POINT_TARGET_INDEX_INVALID;
    if (!curve || !t.valid) return VF_POINT_TARGET_UNPOPULATED;
    if (ci >= t.graphicsEnd)
        return curve[ci].freq_kHz != 0 ? VF_POINT_TARGET_FOREIGN_DOMAIN
                                       : VF_POINT_TARGET_UNPOPULATED;
    if (curve[ci].freq_kHz == 0) return VF_POINT_TARGET_UNPOPULATED;
    // The historic visibility rule keeps some floor points listed above
    // 700 mV. Listing never grants write or lock-anchor authority.
    if (vf_point_in_flat_floor(t, curve, ci)) return VF_POINT_TARGET_FLOOR;
    if (!vf_point_listed(t, curve, ci)) return VF_POINT_TARGET_FLOOR;
    return VF_POINT_TARGET_OK;
}

// Topology identity for the floor binding: membership and voltages of the
// graphics domain, never frequencies (those drift with temperature).
static inline unsigned int vf_floor_topology_key(const VFCurvePoint* curve,
                                                 int count, unsigned int generation) {
    unsigned int hash = 2166136261u;
    auto mix = [&hash](unsigned int value) {
        for (int b = 0; b < 4; ++b) {
            hash ^= (value >> (8 * b)) & 0xffu;
            hash *= 16777619u;
        }
    };
    mix(generation);
    if (!curve || count <= 0) return hash;
    if (count > VF_NUM_POINTS) count = VF_NUM_POINTS;
    const int end = vf_graphics_domain_length(curve, count);
    mix((unsigned int)end);
    for (int i = 0; i < end; ++i) {
        if (curve[i].freq_kHz == 0) continue;
        mix((unsigned int)i);
        mix(curve[i].volt_uV);
    }
    return hash;
}

struct VfFloorBinding {
    bool bound;
    unsigned int topologyKey;
    VfFloorTopology floor;
};

// Decide the floor once per topology binding.  Returns true when the binding
// was (re)computed, so the caller can log the one line that explains a later
// listing.  An unchanged topology keeps the first answer even when a later
// read would place the floor's end elsewhere.
static inline bool vf_floor_binding_update(VfFloorBinding* binding,
                                           const VFCurvePoint* curve,
                                           const int* offsetsKHz, int count,
                                           unsigned int generation) {
    if (!binding) return false;
    const unsigned int key = vf_floor_topology_key(curve, count, generation);
    if (binding->bound && binding->topologyKey == key) return false;
    binding->floor = vf_floor_topology_compute(curve, offsetsKHz, count);
    binding->topologyKey = key;
    binding->bound = binding->floor.valid;
    return true;
}

// Projection of a saved/loaded request onto the editor: discard points the
// current GPU cannot list.  Ownership reads bypass this projection.
static inline unsigned int vf_filter_unlisted_editor_curve_points(
    DesiredSettings* desired, const VfFloorTopology& t, const VFCurvePoint* curve) {
    if (!desired || !curve) return 0;
    unsigned int dropped = 0;
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        if (!desired->hasCurvePoint[i] || vf_point_listed(t, curve, i)) continue;
        desired->hasCurvePoint[i] = false;
        desired->curvePointMHz[i] = 0;
        desired->curvePointFromGpuOffset[i] = false;
        ++dropped;
    }
    return dropped;
}

#endif  // GREEN_CURVE_VF_WRITABLE_POINT_POLICY_H
