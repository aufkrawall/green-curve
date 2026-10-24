// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_VF_POINT_VISIBILITY_POLICY_H
#define GREEN_CURVE_VF_POINT_VISIBILITY_POLICY_H

#include "gpu_core.h"

// Which VF points the editor grid, the graph and saved profiles treat as
// visible.  The answer must not depend on anything the driver moves at run
// time, because it decides the grid's layout (a point's column and row) and
// the `pointN_visible` flag a profile stores.
//
// Each point's voltage is fixed by the driver's VF table.  Its frequency is
// not: the stock curve's low ramp shifts by a couple of points with GPU
// temperature (observed on an RTX 5070: at 22 C points 49-50 sat ~200 MHz
// lower than at 30 C).  The previous rule also required a stock frequency of
// at least 500 MHz, so the visible set changed between a cold logon read and a
// later warm read, and the grid shifted when the window was next refreshed.
// Visibility therefore depends on the voltage alone.  Entries below the
// minimum voltage stay hidden; that also hides the out-of-order idle
// placeholder some tables carry at their last index (a low-voltage, ~400 MHz
// entry after the highest point).
static inline bool vf_point_visible_in_editor(unsigned int freq_kHz,
                                              unsigned int volt_uV) {
    if (freq_kHz == 0) return false;  // unpopulated entry
    return volt_uV / 1000u >= (unsigned int)MIN_VISIBLE_VOLT_mV;
}

#endif
