// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_PROFILE_DOMAIN_MASK_H
#define GREEN_CURVE_PROFILE_DOMAIN_MASK_H
// An absent domain leaves current controls and ownership alone under UPDATE.
enum ProfileDomainMask : int {
    PROFILE_DOMAIN_NONE = 0,
    PROFILE_DOMAIN_VF_CURVE = 1 << 0,
    PROFILE_DOMAIN_GRAPHICS_OFFSET = 1 << 1,
    PROFILE_DOMAIN_MEMORY = 1 << 2,
    PROFILE_DOMAIN_POWER = 1 << 3,
    PROFILE_DOMAIN_LOCK = 1 << 4,
    PROFILE_DOMAIN_FAN = 1 << 5,
    PROFILE_DOMAIN_XBAR = 1 << 6,
    PROFILE_DOMAIN_SYS_CLK = 1 << 7,
    PROFILE_DOMAIN_VIDEO_CLK = 1 << 8,
};
#endif
