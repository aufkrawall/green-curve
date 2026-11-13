// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_GPU_BACKEND_EXACT_CURVE_H
#define GREEN_CURVE_GPU_BACKEND_EXACT_CURVE_H
#include "vf_exact_control_write.h"
#include "curve_preservation_precision.h"
#include "apply_correction_budget_policy.h"

static bool apply_curve_preservation_precheck(const DesiredSettings* desired, char* why, size_t size) {
    if (!desired->hasMemOffset || desired->resetOcBeforeApply || !g_app.loaded) return true;
    const int target = (int)(desired_memory_hz(*desired) / 1000);
    if (!apply_plan_mem_offset_write(g_app, target)) return true;
    if (!nvapi_read_offsets() || !g_app.vfOffsetsHzValid) {
        set_message(why, size, "Fresh exact VF controls are unavailable; the memory change cannot preserve the existing curve. Nothing was changed.");
        debug_log("curve-preserve: refused before memory change; exact control read unavailable\n");
        return false;
    }
    bool mask[VF_NUM_POINTS]{};
    for (int i = 0; i < VF_NUM_POINTS; ++i) {
        mask[i] = g_app.curve[i].freq_kHz && (g_app.vfMask[i / 8] & (1u << (i % 8)));
        if (g_app.curve[i].freq_kHz && !mask[i] && g_app.freqOffsetsHz[i]) {
            set_message(why, size, "Existing read-only VF control at point %d cannot be preserved across a memory change. Nothing was changed.", i);
            debug_log("curve-preserve: refused before memory change; read-only point=%d\n", i);
            return false;
        }
    }
    int bad = -1;
    if (!curve_exact_targets_representable(g_app.vfBackend, vf_offset_range_current(), mask, g_app.freqOffsetsHz, &bad)) {
        set_message(why, size, "Existing VF control at point %d cannot be preserved exactly by this write surface. Nothing was changed.", bad);
        debug_log("curve-preserve: refused before memory change; point=%d targetHz=%lld\n", bad, bad >= 0 ? g_app.freqOffsetsHz[bad] : 0);
        return false;
    }
    return true;
}

static bool apply_curve_offsets_exact_hz(const gc_i64* targets, const bool* mask,
    int passes, ULONGLONG deadline, const unsigned int* expectedVoltageUv = nullptr) {
    const auto* b = g_app.vfBackend;
    if (!b) return false;
    if (expectedVoltageUv) {
        if (!nvapi_read_curve()) return false;
        for (int i = 0; i < VF_NUM_POINTS; ++i) if (expectedVoltageUv[i] != g_app.curve[i].volt_uV) {
            debug_log("curve-exact: topology changed before write; ci=%d expectedUv=%u currentUv=%u\n",
                i, expectedVoltageUv[i], g_app.curve[i].volt_uV);
            return false;
        }
    }
    struct IO {
        NvApiFunc get, set;
        ULONGLONG deadline;
        bool read(unsigned char* bytes, size_t) { return get(g_app.gpuHandle, bytes) == 0; }
        bool write(unsigned char* bytes, size_t) {
            set_last_apply_phase("VF exact control write"); return set(g_app.gpuHandle, bytes) == 0;
        }
        bool mayWrite() { return apply_fallback_write_may_start(GetTickCount64(), deadline); }
    } io{(NvApiFunc)nvapi_qi(b->getControlId), (NvApiFunc)nvapi_qi(b->setControlId), deadline};
    if (!io.get || !io.set) return false;
    const auto r = vf_exact_control_write(b, g_app.vfMask, mask, targets, passes, io);
    debug_log("curve-exact: status=%d points=%d writes=%d batch=%d single=%d firstCi=%d targetHz=%lld actualHz=%lld\n",
        (int)r.status, r.points, r.writes, r.batchWrites, r.pointWrites, r.firstCi, r.targetHz, r.actualHz);
    if (r.status != VF_EXACT_VERIFIED) return false;
    const bool refreshed = nvapi_read_offsets() && g_app.vfOffsetsHzValid && nvapi_read_curve();
    if (refreshed) rebuild_visible_map();
    return refreshed;
}
#endif
