// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_DOCUMENT_DOMAINS_H
#define GREEN_CURVE_PROFILE_DOCUMENT_DOMAINS_H
// Whole-domain removal from a portable profile document. Used when the user
// leaves a setting group out of a captured profile, and when a capture curve
// representation is judged: exact, flatten and pin carry every non-curve
// setting identically, so only the curve, core offset and lock can decide
// whether a representation is usable.
#include "profile_document.h"

static constexpr int PROFILE_DOCUMENT_CURVE_DOMAINS =
    PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_GRAPHICS_OFFSET | PROFILE_DOMAIN_LOCK;
static constexpr int PROFILE_DOCUMENT_NON_CURVE_DOMAINS =
    PROFILE_DOMAIN_MEMORY | PROFILE_DOMAIN_POWER | PROFILE_DOMAIN_FAN | PROFILE_DOMAIN_XBAR |
    PROFILE_DOMAIN_SYS_CLK | PROFILE_DOMAIN_VIDEO_CLK;
static_assert((PROFILE_DOCUMENT_CURVE_DOMAINS | PROFILE_DOCUMENT_NON_CURVE_DOMAINS) == 511 &&
                  !(PROFILE_DOCUMENT_CURVE_DOMAINS & PROFILE_DOCUMENT_NON_CURVE_DOMAINS),
              "every profile domain is either curve-shaping or a separate setting row");

// Remove whole setting groups. Refuses (leaving `why`) when the remaining
// document is not structurally valid, e.g. a curve that follows a removed
// core offset.
static inline bool profile_document_drop_domains(ProfileDocument& d, int mask, char* why, size_t whySize) {
    if (mask & (PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK)) {
        for (int i = 0; i < d.topologyCount && i < VF_NUM_POINTS; ++i) {
            auto& p = d.points[i];
            p.kind = PROFILE_POINT_ABSENT; p.absoluteFrequencyHz = 0; p.capturedOffsetHz = 0;
        }
        d.lock = ProfileLockIntent{};
        d.lock.anchorTopologyId = -1;
        mask |= PROFILE_DOMAIN_VF_CURVE | PROFILE_DOMAIN_LOCK;
    }
    if (mask & PROFILE_DOMAIN_GRAPHICS_OFFSET) d.graphicsOffset = ProfileGraphicsOffset{};
    if (mask & PROFILE_DOMAIN_MEMORY) { d.hasMemoryOffset = false; d.memoryOffsetHz = 0; d.memorySurface = 0; }
    if (mask & PROFILE_DOMAIN_POWER) d.power = ProfilePowerIntent{};
    if (mask & PROFILE_DOMAIN_FAN) d.fan = ProfileFanIntent{};
    if (mask & PROFILE_DOMAIN_XBAR) { d.hasXbarOffsetHz = d.hasXbarMsvddOffsetUv = false; d.xbarOffsetHz = d.xbarMsvddOffsetUv = 0; }
    if (mask & PROFILE_DOMAIN_SYS_CLK) { d.hasSysClkOffsetHz = false; d.sysClkOffsetHz = 0; }
    if (mask & PROFILE_DOMAIN_VIDEO_CLK) { d.hasVideoClkOffsetHz = false; d.videoClkOffsetHz = 0; }
    d.domains &= ~mask;
    if (!profile_document_structurally_valid(d, why, whySize)) {
        if (why && whySize && (mask & PROFILE_DOMAIN_GRAPHICS_OFFSET))
            snprintf(why, whySize, "the curve follows the core clock offset; include both or neither");
        return false;
    }
    return true;
}
#endif
