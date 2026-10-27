// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Which advanced ClkDomains fields (XBAR clock, XBAR MSVDD voltage, SYS clock,
// VIDEO clock) a DesiredSettings claims.
//
// The domains arrived one protocol version at a time, and every "does this
// request own anything?" predicate was written as its own open-coded list.
// Two of those lists stopped at XBAR: a request that changed only the SYS or
// VIDEO clock was refused by the service as "No Green Curve-owned settings
// were requested", and a fan request that also carried one of them was
// classified as fan-only.  One predicate, so the next domain is added once.

#ifndef GREEN_CURVE_DESIRED_ADVANCED_DOMAINS_POLICY_H
#define GREEN_CURVE_DESIRED_ADVANCED_DOMAINS_POLICY_H

static inline bool desired_claims_advanced_clock_domain(
    const DesiredSettings* desired) {
    return desired &&
        (desired->hasXbarOffsetKhz || desired->hasXbarMsvddOffsetUv ||
         desired->hasSysClkOffsetKhz || desired->hasVideoClkOffsetKhz);
}

enum : unsigned int {
    DESIRED_ADVANCED_FIELD_XBAR_CLOCK = 1u << 0,
    DESIRED_ADVANCED_FIELD_XBAR_MSVDD = 1u << 1,
    DESIRED_ADVANCED_FIELD_SYS_CLOCK = 1u << 2,
    DESIRED_ADVANCED_FIELD_VIDEO_CLOCK = 1u << 3,
};

// A profile saved where a surface answered carries its fields to a GPU or
// driver where it does not.  Those fields are dropped for this apply, the same
// rule for every advanced domain, so one missing surface never refuses the
// curve, memory, power and fan settings beside it.  Returns the dropped fields.
static inline unsigned int desired_strip_unavailable_advanced_fields(
    DesiredSettings* desired, bool xbarAvailable, bool sysAvailable,
    bool videoAvailable) {
    if (!desired) return 0;
    unsigned int stripped = 0;
    if (!xbarAvailable && desired->hasXbarOffsetKhz) {
        desired->hasXbarOffsetKhz = false;
        desired->xbarOffsetKhz = 0;
        stripped |= DESIRED_ADVANCED_FIELD_XBAR_CLOCK;
    }
    if (!xbarAvailable && desired->hasXbarMsvddOffsetUv) {
        desired->hasXbarMsvddOffsetUv = false;
        desired->xbarMsvddOffsetUv = 0;
        stripped |= DESIRED_ADVANCED_FIELD_XBAR_MSVDD;
    }
    if (!sysAvailable && desired->hasSysClkOffsetKhz) {
        desired->hasSysClkOffsetKhz = false;
        desired->sysClkOffsetKhz = 0;
        stripped |= DESIRED_ADVANCED_FIELD_SYS_CLOCK;
    }
    if (!videoAvailable && desired->hasVideoClkOffsetKhz) {
        desired->hasVideoClkOffsetKhz = false;
        desired->videoClkOffsetKhz = 0;
        stripped |= DESIRED_ADVANCED_FIELD_VIDEO_CLOCK;
    }
    return stripped;
}

#endif  // GREEN_CURVE_DESIRED_ADVANCED_DOMAINS_POLICY_H
