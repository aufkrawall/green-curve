// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_SCALAR_SHIFT_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_SCALAR_SHIFT_H

#include <string.h>

struct ApplyScalarShiftSnapshot {
    int freqKHz[VF_NUM_POINTS];
    int offsetsKHz[VF_NUM_POINTS];
    unsigned int voltageUv[VF_NUM_POINTS];

    ApplyScalarShiftSnapshot(const int* freq, const int* offsets, const unsigned int* voltage) {
        memcpy(freqKHz, freq, sizeof(freqKHz));
        memcpy(offsetsKHz, offsets, sizeof(offsetsKHz));
        memcpy(voltageUv, voltage, sizeof(voltageUv));
    }
};

// Runtime-lock owned, in-memory evidence for the current GPU session. The
// measurement belongs to a scalar VALUE and a voltage topology, not to one
// Apply: a later no-reset request must not replace a compensated VF offset
// with its full component while the measured NVML contribution remains live.
// GPU enumeration/teardown/selection and every scalar write invalidate it.
struct ApplyScalarShiftEvidence {
    bool valid;
    int scalarKHz;
    unsigned int voltageUv[VF_NUM_POINTS];
    bool measured[VF_NUM_POINTS];
    int shiftKHz[VF_NUM_POINTS];

    void clear() { *this = {}; }

    void remember(int scalar, const int* preFreq, const unsigned int* preVoltage,
                  const int* postFreq, const unsigned int* postVoltage, const int* shift) {
        clear();
        valid = true;
        scalarKHz = scalar;
        for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
            voltageUv[ci] = postFreq[ci] > 0 ? postVoltage[ci] : 0;
            measured[ci] = preFreq[ci] > 0 && postFreq[ci] > 0 &&
                preVoltage[ci] != 0 && preVoltage[ci] == postVoltage[ci];
            if (measured[ci]) shiftKHz[ci] = shift[ci];
        }
    }

    bool copy(int scalar, const VFCurvePoint* curve, const bool* required,
              int* out, int* firstUnproven) const {
        if (firstUnproven) *firstUnproven = -1;
        if (!valid || scalar != scalarKHz) return false;
        for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
            const unsigned int voltage = curve[ci].freq_kHz > 0 ? curve[ci].volt_uV : 0;
            if (voltage != voltageUv[ci] || (required[ci] && !measured[ci])) {
                if (firstUnproven) *firstUnproven = ci;
                return false;
            }
        }
        if (out) {
            for (int ci = 0; ci < VF_NUM_POINTS; ++ci) out[ci] = shiftKHz[ci];
        }
        return true;
    }
};

static ApplyScalarShiftEvidence g_applyScalarShiftEvidence = {};

static bool apply_scalar_shift_required_points(const DesiredSettings* desired,
                                               const bool* tailMask, bool* required) {
    bool any = false;
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        const bool tail = tailMask ? tailMask[ci] :
            (desired->hasLock && desired->lockMHz > 0 && desired->lockCi >= 0 &&
             ci >= desired->lockCi);
        required[ci] = desired->hasCurvePoint[ci] &&
            desired->curvePointFromGpuOffset[ci] && !tail;
        any = any || required[ci];
    }
    return any;
}

// Only these requests need a fresh scalar read before the first write. Normal
// profile applies establish a new zero-scalar measurement after their reset.
static bool apply_scalar_shift_needs_precheck(const DesiredSettings* desired,
                                              bool globalVfOffset) {
    bool required[VF_NUM_POINTS] = {};
    if (globalVfOffset || !apply_scalar_shift_required_points(desired, nullptr, required))
        return false;
    if (desired->hasGpuOffset &&
        (desired->gpuOffsetMHz == 0 || desired->gpuOffsetExcludeLowCount > 0 ||
         desired->resetOcBeforeApply)) return false;
    return true;
}

// Refuse unmeasured composition before touching any hardware. A retained
// scalar must still read back exactly, and evidence cannot be rescaled for a
// different requested scalar without assuming a linear driver model.
static bool apply_provenance_scalar_shift_precheck(const DesiredSettings* desired,
    bool scalarKnown, int liveScalarKHz, const VFCurvePoint* curve,
    char* result, size_t resultSize) {
    const int target = desired->hasGpuOffset ? desired->gpuOffsetMHz * 1000 : liveScalarKHz;
    bool required[VF_NUM_POINTS] = {};
    apply_scalar_shift_required_points(desired, nullptr, required);
    int first = -1;
    if (scalarKnown && (target == 0 || (liveScalarKHz == target &&
        g_applyScalarShiftEvidence.copy(target, curve, required, nullptr, &first)))) return true;
    set_message(result, resultSize,
        "Cannot safely apply offset-derived curve points without a fresh GPU-offset measurement."
        " Use a normal Apply with reset-before-apply. Nothing was changed.");
    debug_log("nvml shift: REFUSED before any write; retained evidence unavailable"
              " scalarKnown=%d live=%d target=%d cached=%d cachedScalar=%d firstCi=%d\n",
        scalarKnown ? 1 : 0, liveScalarKHz, target, g_applyScalarShiftEvidence.valid ? 1 : 0,
        g_applyScalarShiftEvidence.scalarKHz, first);
    return false;
}

// Resolve the measurement used by target building. No fresh pair means reuse
// of proven same-session evidence, never an implicit full-component fallback
// over a nonzero scalar. Recheck after writes in case the topology changed.
static bool apply_resolve_provenance_scalar_shift(const DesiredSettings* desired,
    bool offsetInVfTable, bool scalarWritten, bool scalarBeforeKnownZero, int scalarKHz,
    const int* preFreq, const int* preOffsets, const unsigned int* preVoltage,
    const int* postFreq, const int* postOffsets, const unsigned int* postVoltage,
    const bool* tailMask, int* shiftOut, bool* compensatedOut) {
    *compensatedOut = false;
    const bool fresh = apply_measure_provenance_scalar_shift(desired, offsetInVfTable,
        scalarWritten, scalarBeforeKnownZero, scalarKHz,
        preFreq, preOffsets, postFreq, postOffsets, shiftOut);
    if (fresh) g_applyScalarShiftEvidence.remember(scalarKHz,
        preFreq, preVoltage, postFreq, postVoltage, shiftOut);
    bool required[VF_NUM_POINTS] = {};
    if (offsetInVfTable || scalarKHz == 0 ||
        !apply_scalar_shift_required_points(desired, tailMask, required)) return true;
    VFCurvePoint curve[VF_NUM_POINTS] = {};
    for (int ci = 0; ci < VF_NUM_POINTS; ++ci) {
        curve[ci].freq_kHz = postFreq[ci] > 0 ? (unsigned int)postFreq[ci] : 0;
        curve[ci].volt_uV = postVoltage[ci];
    }
    int first = -1;
    if (!g_applyScalarShiftEvidence.copy(scalarKHz, curve, required, shiftOut, &first)) {
        debug_log("nvml shift: evidence unavailable after writes; refusing curve targets"
                  " scalar=%d fresh=%d firstCi=%d\n", scalarKHz, fresh ? 1 : 0, first);
        return false;
    }
    *compensatedOut = true;
    if (!fresh) debug_log("nvml shift: retained same-session measurement scalar=%d kHz\n", scalarKHz);
    return true;
}

#endif
