// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// XBAR-specific INI record I/O shared by profile slots, the slot-1 legacy
// mirror, and global controls. Kept beside the already-oversized profile shard.
#ifndef GREEN_CURVE_CONFIG_PROFILE_XBAR_IO_H
#define GREEN_CURVE_CONFIG_PROFILE_XBAR_IO_H

static inline bool load_profile_xbar_settings(const char* path,
    const char* section, int slot, DesiredSettings* desired,
    char* err, size_t errSize) {
    char buf[64] = {};

    gc_GetPrivateProfileStringUtf8(section, "xbar_offset_khz", "", buf,
        ARRAY_COUNT(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = 0;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid xbar_offset_khz in profile %d", slot);
            return false;
        }
        if (value < -1000000 || value > 1000000) {
            set_message(err, errSize,
                "xbar_offset_khz %d is outside the safe range -1000000..1000000 in profile %d",
                value, slot);
            return false;
        }
        desired->hasXbarOffsetKhz = true;
        desired->xbarOffsetKhz = value;
    }

    gc_GetPrivateProfileStringUtf8(section, "xbar_msvdd_offset_uv", "", buf,
        ARRAY_COUNT(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = 0;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid xbar_msvdd_offset_uv in profile %d", slot);
            return false;
        }
        if (value < -100000 || value > 100000) {
            set_message(err, errSize,
                "xbar_msvdd_offset_uv %d is outside the safe range -100000..100000 in profile %d",
                value, slot);
            return false;
        }
        desired->hasXbarMsvddOffsetUv = true;
        desired->xbarMsvddOffsetUv = value;
    }

    gc_GetPrivateProfileStringUtf8(section, "sys_clk_offset_khz", "", buf,
        ARRAY_COUNT(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        int value = 0;
        if (!parse_int_strict(buf, &value)) {
            set_message(err, errSize, "Invalid sys_clk_offset_khz in profile %d", slot);
            return false;
        }
        if (value < -1000000 || value > 1000000) {
            set_message(err, errSize,
                "sys_clk_offset_khz %d is outside the safe range -1000000..1000000 in profile %d",
                value, slot);
            return false;
        }
        desired->hasSysClkOffsetKhz = true;
        desired->sysClkOffsetKhz = value;
    }
    return true;
}

static inline bool profile_xbar_keys_should_be_written() {
    return g_app.xbarProbeValid;
}

static inline void resolve_profile_xbar_save_values(
    const DesiredSettings* desired, const ControlState* control,
    bool haveControl, int* freqKhzOut, int* msvddUvOut) {
    *freqKhzOut = desired && desired->hasXbarOffsetKhz
        ? desired->xbarOffsetKhz
        : (haveControl && control && control->hasXbarOffset
            ? control->xbarOffsetKhz : g_app.xbarFreqOffsetKhz);
    *msvddUvOut = desired && desired->hasXbarMsvddOffsetUv
        ? desired->xbarMsvddOffsetUv
        : (haveControl && control && control->hasXbarMsvddOffset
            ? control->xbarMsvddOffsetUv : g_app.xbarMsvddOffsetUv);
}
#endif
