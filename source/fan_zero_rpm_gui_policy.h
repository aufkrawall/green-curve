// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_FAN_ZERO_RPM_GUI_POLICY_H
#define GREEN_CURVE_FAN_ZERO_RPM_GUI_POLICY_H

#include "platform.h"

#include <stddef.h>
#include <stdio.h>

enum {
    FAN_ZERO_RPM_GUI_CHECK_Y = 386,
    FAN_ZERO_RPM_GUI_CHECK_HEIGHT = 22,
    FAN_ZERO_RPM_GUI_GAP_LABEL_Y = 414,
    FAN_ZERO_RPM_GUI_GAP_LABEL_HEIGHT = 18,
    FAN_ZERO_RPM_GUI_GAP_COMBO_Y = 434,
    FAN_ZERO_RPM_GUI_GAP_COMBO_CLOSED_HEIGHT = 24,
    FAN_ZERO_RPM_GUI_DESCRIPTION_Y = 472,
    FAN_ZERO_RPM_GUI_DESCRIPTION_HEIGHT = 48,
    FAN_ZERO_RPM_GUI_BUTTON_Y = 532,
    FAN_ZERO_RPM_GUI_BUTTON_HEIGHT = 28,
    FAN_ZERO_RPM_GUI_MIN_CLIENT_HEIGHT = 580,
};

// Keep the policy explanation to two explicit lines. It therefore remains
// predictable under DPI/font changes, while displaying the actual thresholds
// produced by the independent zero-RPM gap.
static inline void fan_zero_rpm_gui_format_description(
    char* out, size_t outSize, int startC, int gapC) {
    if (!out || outSize == 0) return;
    int stopC = startC - gapC;
    if (stopC < 0) stopC = 0;
    snprintf(out, outSize,
        "Fan ON: %d" GC_DEGREE "C (first point)\r\n"
        "Fan OFF: %d" GC_DEGREE "C (%d" GC_DEGREE "C gap)",
        startC, stopC, gapC);
    out[outSize - 1] = 0;
}

static inline bool fan_zero_rpm_gui_layout_is_nonoverlapping() {
    return FAN_ZERO_RPM_GUI_CHECK_Y + FAN_ZERO_RPM_GUI_CHECK_HEIGHT <=
               FAN_ZERO_RPM_GUI_GAP_LABEL_Y &&
        FAN_ZERO_RPM_GUI_GAP_LABEL_Y + FAN_ZERO_RPM_GUI_GAP_LABEL_HEIGHT <=
            FAN_ZERO_RPM_GUI_GAP_COMBO_Y &&
        FAN_ZERO_RPM_GUI_GAP_COMBO_Y +
                FAN_ZERO_RPM_GUI_GAP_COMBO_CLOSED_HEIGHT <=
            FAN_ZERO_RPM_GUI_DESCRIPTION_Y &&
        FAN_ZERO_RPM_GUI_DESCRIPTION_Y +
                FAN_ZERO_RPM_GUI_DESCRIPTION_HEIGHT <=
            FAN_ZERO_RPM_GUI_BUTTON_Y &&
        FAN_ZERO_RPM_GUI_BUTTON_Y + FAN_ZERO_RPM_GUI_BUTTON_HEIGHT <=
            FAN_ZERO_RPM_GUI_MIN_CLIENT_HEIGHT;
}

#endif // GREEN_CURVE_FAN_ZERO_RPM_GUI_POLICY_H
