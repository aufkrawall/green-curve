// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_CLOCK_RESET_POLICY_H
#define GREEN_CURVE_CLOCK_RESET_POLICY_H

// On VF-global backends a scalar reset subtracts the most frequent delta from
// every point. A flatten floor must never be mistaken for a separate scalar:
// subtracting it raises the prefix. Reset that backend through its VF table.
template<class ResetScalar, class ResetCurve>
static ApplyRecoveryResult reset_core_clock_controls(
    bool vfGlobal, bool ownsScalar, ResetScalar resetScalar, ResetCurve resetCurve) {
    ApplyRecoveryResult result = {};
    result.gpuOffset.verified = vfGlobal || !ownsScalar;
    if (!result.gpuOffset.verified) {
        result.gpuOffset.attempted = true;
        result.gpuOffset.verified = resetScalar();
        // Do not remove the curve's protection while a separate offset is unknown.
        if (!result.gpuOffset.verified) return result;
    }
    result.curve.attempted = true;
    result.curve.verified = resetCurve();
    return result;
}
#endif
