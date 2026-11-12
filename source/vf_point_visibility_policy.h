// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_VF_POINT_VISIBILITY_POLICY_H
#define GREEN_CURVE_VF_POINT_VISIBILITY_POLICY_H

#include "gpu_core.h"

// The voltage half of the editor's listed set (vf_writable_point_policy.h adds
// the writable points below MIN_VISIBLE_VOLT_mV).  Every point this answers
// true for is listed; it is also exactly the rule every profile saved before
// sub-700 mV points were listed used for its `pointN_visible` flag, so legacy
// readers that need the writer's answer use it directly.  The answer must not
// depend on anything the driver moves at run time, because it decides the
// grid's layout (a point's column and row).
//
// Each point's voltage is fixed by the driver's VF table.  Its frequency is
// not: the stock curve's low ramp shifts by a couple of points with GPU
// temperature (observed on an RTX 5070: at 22 C points 49-50 sat ~200 MHz
// lower than at 30 C).  The previous rule also required a stock frequency of
// at least 500 MHz, so the visible set changed between a cold logon read and a
// later warm read, and the grid shifted when the window was next refreshed.
// This half therefore depends on the voltage alone.  Entries below the
// minimum voltage are left to the writable rule, which lists them outside the
// flat floor and excludes the out-of-order idle placeholder some tables carry
// at their last index (a low-voltage, ~400 MHz entry after the highest point)
// as a foreign domain.
static inline bool vf_point_visible_in_editor(unsigned int freq_kHz,
                                              unsigned int volt_uV) {
    if (freq_kHz == 0) return false;  // unpopulated entry
    return volt_uV / 1000u >= (unsigned int)MIN_VISIBLE_VOLT_mV;
}

#endif
