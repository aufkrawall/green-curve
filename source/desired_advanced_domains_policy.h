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

#endif  // GREEN_CURVE_DESIRED_ADVANCED_DOMAINS_POLICY_H
