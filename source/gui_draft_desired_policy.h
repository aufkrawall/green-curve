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
// there (the draft then keeps its live/applied projection).
//  - `fromLock` says the value came from the lock target (anchor or a
//    zero-MHz tail point carried by a portable document).
//  - A captured offset point (`curvePointCaptured[ci] != 0`) outside a lock
//    tail resolves from `stockBaseKHz + curvePointCapturedOffsetHz[ci] / 1000`
//    (`fromProjection=true`), matching the Linux TUI preview so loading a
//    captured profile shows its lower-voltage offsets over stock.
//  - A pre-tail projected point saved without an observed frequency
//    (`curvePointFromGpuOffset[ci] != 0`, `curvePointMHz[ci] == 0`) resolves
//    from `stockBaseKHz + gpuOffsetComponentMHz * 1000` (`fromProjection=true`)
//    so the editor draft never records "0" / invalid (which made Apply, Save,
//    and reload comparison fail with "Invalid MHz value for point N").
static inline unsigned int gui_draft_desired_point_mhz(
    const DesiredSettings& desired, int ci,
    bool* fromLock = nullptr, long long stockBaseKHz = 0,
    int gpuOffsetComponentMHz = 0, bool* fromProjection = nullptr) {
    if (fromLock) *fromLock = false;
    if (fromProjection) *fromProjection = false;
    if (ci < 0 || ci >= VF_NUM_POINTS) return 0;
    if (desired.hasLock && desired.lockCi == ci && desired.lockMHz > 0) {
        if (fromLock) *fromLock = true;
        return desired.lockMHz;
    }
    if (desired.hasCurvePoint[ci] && desired.curvePointMHz[ci] > 0) {
        return desired.curvePointMHz[ci];
    }
    const bool inLockTail = desired.hasLock && desired.lockCi >= 0 &&
        ci > desired.lockCi && desired.lockMHz > 0;
    if (inLockTail && desired.hasCurvePoint[ci]) {
        if (fromLock) *fromLock = true;
        return desired.lockMHz;
    }
    if (!inLockTail && desired.curvePointCaptured[ci] && stockBaseKHz > 0) {
        const long long offHz = desired.curvePointCapturedOffsetHz[ci];
        const long long offKHz = (offHz >= 0 ? offHz + 500LL : offHz - 500LL) / 1000LL;
        long long targetKHz = stockBaseKHz + offKHz;
        if (targetKHz < 1000LL) targetKHz = 1000LL;
        if (targetKHz > (long long)UINT_MAX) targetKHz = (long long)UINT_MAX;
        if (fromProjection) *fromProjection = true;
        return (unsigned int)((targetKHz + 500LL) / 1000LL);
    }
    if (desired.hasCurvePoint[ci] && desired.curvePointFromGpuOffset[ci] &&
        desired.hasGpuOffset && stockBaseKHz > 0) {
        long long targetKHz = stockBaseKHz + (long long)gpuOffsetComponentMHz * 1000LL;
        if (targetKHz < 1000LL) targetKHz = 1000LL;
        if (targetKHz > (long long)UINT_MAX) targetKHz = (long long)UINT_MAX;
        if (fromProjection) *fromProjection = true;
        return (unsigned int)((targetKHz + 500LL) / 1000LL);
    }
    return 0;
}

#endif
