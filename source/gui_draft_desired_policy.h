// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GUI_DRAFT_DESIRED_POLICY_H
#define GREEN_CURVE_GUI_DRAFT_DESIRED_POLICY_H

// Which editor-draft value a profile-sourced DesiredSettings declares for one
// VF point. The draft (GuiDraft) is what Apply and Save read: the lock target
// sent by capture_gui_desired_settings() is the draft's value at the lock
// anchor, not g_app.lockedFreq. So the draft must hold exactly what
// populate_desired_into_gui() shows in the fields:
//  - every explicit curve point at its MHz;
//  - the lock anchor at the lock's own target, which wins over an explicit
//    anchor point because the anchor field always shows the lock MHz.
// A record may carry its lock without an explicit anchor point: a captured
// or imported flatten owns the anchor and the tail through the lock alone.
// Load rebuilds the draft from the applied/live curve (set_gui_state_dirty()
// -> gui_draft_capture_clean_projection()) before copying the record in, so a
// draft copy limited to explicit points kept the live anchor value and Apply
// sent the live clock (2482 MHz) instead of the slot's lock (2962 MHz).

#include "gpu_core.h"

// The MHz the record declares for point `ci`, or 0 when it declares nothing
// there (the draft then keeps its live/applied projection). `fromLock` says
// the value is the lock target rather than a curve point.
static inline unsigned int gui_draft_desired_point_mhz(const DesiredSettings& desired, int ci,
                                                       bool* fromLock = nullptr) {
    if (fromLock) *fromLock = false;
    if (ci < 0 || ci >= VF_NUM_POINTS) return 0;
    if (desired.hasLock && desired.lockCi == ci && desired.lockMHz > 0) {
        if (fromLock) *fromLock = true;
        return desired.lockMHz;
    }
    return desired.hasCurvePoint[ci] ? desired.curvePointMHz[ci] : 0;
}

#endif
