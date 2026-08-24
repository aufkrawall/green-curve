// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_FAN_ZERO_RPM_GUI_POLICY_H
#define GREEN_CURVE_FAN_ZERO_RPM_GUI_POLICY_H

#include "platform.h"

// Explicit line breaks keep Windows STATIC text measurement independent of the
// current UI font.  The previous prose wrapped to four lines in some scaling/
// font combinations but had room for only about two and a half.
static const char FAN_ZERO_RPM_GUI_DESCRIPTION[] =
    "Fan ON: first point\r\n"
    "OFF: ON - hysteresis\r\n"
    "Minimum gap: 2" GC_DEGREE "C";

enum {
    FAN_ZERO_RPM_GUI_CHECK_Y = 386,
    FAN_ZERO_RPM_GUI_CHECK_HEIGHT = 22,
    FAN_ZERO_RPM_GUI_DESCRIPTION_Y = 414,
    FAN_ZERO_RPM_GUI_DESCRIPTION_HEIGHT = 60,
    FAN_ZERO_RPM_GUI_BUTTON_Y = 482,
    FAN_ZERO_RPM_GUI_BUTTON_HEIGHT = 28,
    FAN_ZERO_RPM_GUI_MIN_CLIENT_HEIGHT = 520,
};

static inline bool fan_zero_rpm_gui_layout_is_nonoverlapping() {
    return FAN_ZERO_RPM_GUI_CHECK_Y + FAN_ZERO_RPM_GUI_CHECK_HEIGHT <=
               FAN_ZERO_RPM_GUI_DESCRIPTION_Y &&
        FAN_ZERO_RPM_GUI_DESCRIPTION_Y +
                FAN_ZERO_RPM_GUI_DESCRIPTION_HEIGHT <=
            FAN_ZERO_RPM_GUI_BUTTON_Y &&
        FAN_ZERO_RPM_GUI_BUTTON_Y + FAN_ZERO_RPM_GUI_BUTTON_HEIGHT <=
            FAN_ZERO_RPM_GUI_MIN_CLIENT_HEIGHT;
}

#endif // GREEN_CURVE_FAN_ZERO_RPM_GUI_POLICY_H
