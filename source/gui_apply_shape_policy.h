// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Which request a manual GUI Apply sends, given which editor domains differ
// from what the service already applied.
//
// A FULL apply resets the GPU to stock first (reset-before-apply), settles for
// a second, and rewrites the whole VF curve and lock -- the transition the
// F-APPLY-CEILING clamp exists to protect, and the slowest thing the service
// does.  It is the right request whenever a clock-affecting domain changed.
// It was also what a power-limit tweak sent, so nudging the power target
// mid-game put the GPU through a full stock round trip.
//
// A power target write changes no clock, VF point or lock, so a request that
// changes only power (and optionally fan) is sent sparse, exactly like the
// fan-only request: the service merges it into the intent it already owns
// (see service_apply_desired_settings) and the curve is never touched.
//
// Deliberately NOT sparse:
//   - memory offset: a memory write can drop the VF curve on some drivers
//     (why the backend has preserveCurveAcrossMem), and a sparse request would
//     run that window without the transition clamp the full path arms;
//   - XBAR / SYS / VIDEO: the reset-to-stock baseline owns those domains, and
//     nothing here has been measured to show a direct write is equivalent.

#ifndef GREEN_CURVE_GUI_APPLY_SHAPE_POLICY_H
#define GREEN_CURVE_GUI_APPLY_SHAPE_POLICY_H

enum GuiApplyShape {
    GUI_APPLY_SHAPE_NO_CHANGE = 0,
    GUI_APPLY_SHAPE_FAN_ONLY,
    GUI_APPLY_SHAPE_POWER_SPARSE,
    GUI_APPLY_SHAPE_FULL,
};

// Each flag is "this domain differs from what the service applied".
struct GuiApplyChangeSet {
    bool gpuOffset;
    bool memOffset;
    bool powerLimit;
    bool advancedClocks;  // XBAR clock/MSVDD, SYS clock, VIDEO clock
    bool curve;
    bool lock;
    bool fan;
};

static inline GuiApplyShape gui_apply_shape(const GuiApplyChangeSet& changed) {
    const bool clockDomainChanged = changed.gpuOffset || changed.memOffset ||
        changed.advancedClocks || changed.curve || changed.lock;
    if (clockDomainChanged) return GUI_APPLY_SHAPE_FULL;
    if (changed.powerLimit) return GUI_APPLY_SHAPE_POWER_SPARSE;
    return changed.fan ? GUI_APPLY_SHAPE_FAN_ONLY : GUI_APPLY_SHAPE_NO_CHANGE;
}

static inline const char* gui_apply_shape_name(GuiApplyShape shape) {
    switch (shape) {
        case GUI_APPLY_SHAPE_NO_CHANGE: return "no-change";
        case GUI_APPLY_SHAPE_FAN_ONLY: return "fan-only";
        case GUI_APPLY_SHAPE_POWER_SPARSE: return "power-sparse";
        case GUI_APPLY_SHAPE_FULL: return "full";
    }
    return "unknown";
}

#endif  // GREEN_CURVE_GUI_APPLY_SHAPE_POLICY_H
