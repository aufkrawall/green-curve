// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_CORRECTION_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_CORRECTION_H

// The correction loop consumes a verification view, never a saved/wire record.
// Map captured presence onto its offset predicate without assigning sampled
// MHz or treating the captured offset as a graphics-scalar projection.
static void apply_bind_correction_curve_intent(ApplyCorrectionRequest* correction,
    DesiredSettings* desired, gc_bool8* present, gc_bool8* offset) {
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        present[ci] = gc_bool8_from_bool(desired_curve_point_present(desired, ci));
        offset[ci] = gc_bool8_from_bool(desired->curvePointCaptured[ci] ||
            desired->curvePointFromGpuOffset[ci]);
    }
    correction->hasCurvePoint = present;
    correction->curvePointMHz = desired->curvePointMHz;
    correction->fromGpuOffset = offset;
}

#endif
