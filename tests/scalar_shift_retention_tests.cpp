// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the production measurement, retained-evidence preflight/resolver,
// and target builder without loading a GPU driver.
#include "gpu_core.h"
#include "vf_offset_range_policy.h"
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace scalar_shift_fixture {
static void debug_log(const char*, ...) {}
static void set_message(char* out, size_t size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(out, size, fmt, args);
    va_end(args);
}
static VfOffsetRange vf_offset_range_current() { return vf_offset_range_fallback(); }
static int gpu_offset_component_mhz_for_point(int ci, int offset, int exclude) {
    return ci < exclude ? 0 : offset;
}
#include "gpu_backend_apply_targets.h"
#include "gpu_backend_apply_scalar_shift.h"

struct Fixture {
    DesiredSettings desired = {};
    VFCurvePoint curve[VF_NUM_POINTS] = {};
    int preFreq[VF_NUM_POINTS] = {}, postFreq[VF_NUM_POINTS] = {};
    int offsets[VF_NUM_POINTS] = {}, shift[VF_NUM_POINTS] = {}, target[VF_NUM_POINTS] = {};
    unsigned int voltage[VF_NUM_POINTS] = {};
    bool populated[VF_NUM_POINTS] = {}, tail[VF_NUM_POINTS] = {}, mask[VF_NUM_POINTS] = {};
    bool compensated = false;
    char error[256] = {};

    Fixture(int scalarMHz = 150, int measuredShift = 150000) {
        g_applyScalarShiftEvidence.clear();
        desired.hasGpuOffset = true;
        desired.gpuOffsetMHz = scalarMHz;
        desired.resetOcBeforeApply = true;
        desired.hasCurvePoint[2] = true;
        desired.curvePointFromGpuOffset[2] = true;
        desired.curvePointMHz[2] = 2100 + scalarMHz;
        for (int ci = 0; ci < 8; ++ci) {
            preFreq[ci] = 2000000 + 50000 * ci;
            postFreq[ci] = preFreq[ci] + measuredShift;
            voltage[ci] = 700000u + 10000u * (unsigned)ci;
            populated[ci] = true;
            curve[ci].freq_kHz = (unsigned)postFreq[ci];
            curve[ci].volt_uV = voltage[ci];
        }
    }

    bool resolve(bool fresh, bool vfTable = false) {
        return apply_resolve_provenance_scalar_shift(&desired, vfTable, fresh, fresh,
            desired.hasGpuOffset ? desired.gpuOffsetMHz * 1000 : 150000,
            preFreq, offsets, voltage, postFreq, offsets, voltage,
            tail, shift, &compensated);
    }

    bool precheck(bool known = true, int liveScalar = 150000) {
        return apply_provenance_scalar_shift_precheck(&desired, known, liveScalar,
            curve, error, sizeof(error));
    }

    bool build() {
        return apply_build_curve_targets(&desired, true, false, false, LOCK_MODE_NONE,
            -1, 0, false, 0, desired.gpuOffsetMHz, 0,
            populated, offsets, postFreq, tail, target, mask, 0,
            compensated ? shift : nullptr);
    }
};

static int run() {
    // Same request, same scalar, same GPU session: its VF target cannot gain
    // the NVML contribution on the second apply. Includes no/partial shift
    // and negative offsets; no linear-composition model is assumed.
    for (const int measured : {150000, 0, 90000, -150000}) {
        const int scalarMHz = measured < 0 ? -150 : 150;
        Fixture f(scalarMHz, measured);
        if (!f.resolve(true) || !f.compensated || !f.build()) return 6900;
        const int original = f.target[2];
        if (original != scalarMHz * 1000 - measured) return 6901;
        f.desired.resetOcBeforeApply = false;
        if (!apply_scalar_shift_needs_precheck(&f.desired, false) ||
            !f.precheck(true, scalarMHz * 1000)) return 6902;
        if (!f.resolve(false) || !f.compensated || !f.build() ||
            f.target[2] != original) return 6903;
        // A typed neighbour retains its absolute target despite compensation.
        f.desired.hasCurvePoint[3] = true;
        f.desired.curvePointMHz[3] = 2300;
        if (!f.resolve(false) || !f.build() ||
            f.target[3] != 2300000 - f.postFreq[3]) return 6904;
    }
    {
        Fixture f;
        f.desired.resetOcBeforeApply = false;
        if (f.precheck() || !strstr(f.error, "Nothing was changed")) return 6905;
        if (f.resolve(false)) return 6906;
        if (!f.resolve(true)) return 6907;
        if (f.precheck(false)) return 6908;
        f.desired.gpuOffsetMHz = 100;
        if (f.precheck(true, 100000) || f.precheck(true, 150000)) return 6909;
        f.desired.gpuOffsetMHz = 150;
        g_applyScalarShiftEvidence.clear();
        if (f.precheck() || f.resolve(false)) return 6910;
    }
    {
        Fixture f;
        if (!f.resolve(true)) return 6911;
        // Temperature can move frequencies; it cannot rename a VF point.
        f.curve[2].freq_kHz += 15000;
        if (!f.precheck()) return 6912;
        f.curve[6].volt_uV += 1000;
        if (f.precheck()) return 6913;
        f.voltage[6] += 1000;
        if (f.resolve(false)) return 6914;
    }
    {
        Fixture f;
        f.preFreq[2] = 0; // a newly populated requested point was never measured
        if (f.resolve(true)) return 6915;
    }
    {
        Fixture f;
        if (!f.resolve(true)) return 6916;
        // A sparse stock-point request cancels the retained scalar at this
        // point; the scalar domain itself remains owned by the previous apply.
        f.desired.hasGpuOffset = false;
        f.desired.gpuOffsetMHz = 0;
        f.desired.resetOcBeforeApply = false;
        if (!f.precheck() || !f.resolve(false) || !f.build() ||
            f.target[2] != -150000) return 6917;
    }
    {
        Fixture f;
        if (apply_scalar_shift_needs_precheck(&f.desired, false) ||
            apply_scalar_shift_needs_precheck(&f.desired, true)) return 6918;
        f.desired.resetOcBeforeApply = false;
        f.desired.gpuOffsetExcludeLowCount = 2;
        if (apply_scalar_shift_needs_precheck(&f.desired, false)) return 6919;
        f.desired.gpuOffsetExcludeLowCount = 0;
        f.desired.gpuOffsetMHz = 0;
        if (apply_scalar_shift_needs_precheck(&f.desired, false)) return 6920;
        f.desired.gpuOffsetMHz = 150;
        f.desired.hasLock = true;
        f.desired.lockCi = 2;
        f.desired.lockMHz = 2300;
        if (apply_scalar_shift_needs_precheck(&f.desired, false)) return 6921;
        f.tail[2] = true;
        if (!f.resolve(false) || f.compensated) return 6922;
        f.desired.hasLock = false;
        f.desired.curvePointFromGpuOffset[2] = false;
        if (apply_scalar_shift_needs_precheck(&f.desired, false)) return 6923;
    }
    return 0;
}
} // namespace scalar_shift_fixture

int run_scalar_shift_retention_tests() { return scalar_shift_fixture::run(); }
