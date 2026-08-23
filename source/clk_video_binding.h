// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Experimental VIDEO clock entry binding.
//
// No ClkDomains entry has been proven to move the video clock domain on any
// tested board/driver (RTX 5070, 610.88: entries 0/1/3 are GPC/XBAR/SYS; the
// rest showed no measurable response under render or NVDEC load).  This
// binding lets the user point the experimental knob at a candidate entry and
// judge the result themselves.  Default is entry 2; owned entries (GPC=0,
// XBAR=1, SYS=3) are refused.  -1 disables the surface entirely.
#ifndef GREEN_CURVE_CLK_VIDEO_BINDING_H
#define GREEN_CURVE_CLK_VIDEO_BINDING_H

static inline int clk_video_entry_index(const char* configPath) {
    int entry = get_config_int(configPath, "debug", "video_clk_entry", 2);
    if (entry < 0 || entry >= (int)g_xbarSchemas[0].domainCount) return -1;
    if (entry == 0 || entry == XBAR_PINNED_XBAR_ENTRY_INDEX ||
        entry == XBAR_PINNED_SYS_ENTRY_INDEX) {
        return -1;
    }
    return entry;
}

#endif
