// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_LINUX_MEMORY_CURVE_PRESERVATION_H
#define GREEN_CURVE_LINUX_MEMORY_CURVE_PRESERVATION_H
#include "vf_exact_control_write.h"
#include "linux_memory_curve_policy.h"
#include "linux_daemon_deadline_policy.h"

static bool linux_apply_memory_preserving_curve(LinuxGpuState* g, const DesiredSettings* d,
    const LinuxHardwareSnapshot* snapshot, unsigned long long deadline) {
    if (!d->hasMemOffset) return false;
    char why[192]{};
    if (!linux_memory_curve_precheck(g, d, snapshot, why, sizeof(why))) {
        lb_log("mem-curve: refused before memory write: %s\n", why); return false;
    }
    const bool preserve = snapshot->curveValid && !d->resetOcBeforeApply;
    gc_i64 heldHz[VF_NUM_POINTS]{};
    unsigned int heldVoltage[VF_NUM_POINTS]{};
    bool mask[VF_NUM_POINTS]{};
    unsigned char membership[32]{};
    const VfBackendSpec* backend = g->backend;
    void* handle = g->gpuHandle;
    const GpuAdapterInfo binding = g->selectedGpu;
    if (preserve) {
        if (!nvapi_read_vf_snapshot(g, why, sizeof(why)) || !g->vfOffsetsHzValid) {
            lb_log("mem-curve: fresh exact pre-write controls unavailable\n"); return false;
        }
        memcpy(membership, g->vfMask, sizeof(membership));
        auto fresh = std::make_unique<LinuxHardwareSnapshot>(*snapshot);
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            heldHz[i] = g->freqOffsetsHz[i]; heldVoltage[i] = g->curve[i].volt_uV;
            mask[i] = g->curve[i].freq_kHz && (membership[i / 8] & (1u << (i % 8)));
            fresh->curveMask[i] = g->curve[i].freq_kHz != 0; fresh->curveOffsetsHz[i] = heldHz[i];
        }
        if (!linux_memory_curve_precheck(g, d, fresh.get(), why, sizeof(why))) {
            lb_log("mem-curve: fresh controls refused before memory write: %s\n", why); return false;
        }
    }
    int effective = nvml_mem_effective_mhz_from_display_mhz(d->memOffsetMHz);
    if (d->memOffsetPrecise && !memory_offset_effective_mhz_from_hz(d->memOffsetHz, &effective)) return false;
    lb_log("offset: apply domain=%u display=%d effective=%d preserveVF=%d\n",
        (unsigned)NVML_CLOCK_MEM, d->memOffsetMHz, effective, preserve ? 1 : 0);
    if (!nvml_set_clock_offset(g, NVML_CLOCK_MEM, effective,
        d->memOffsetPrecise ? snapshot->memOffsetSurface : (gc_u32)CLOCK_OFFSET_SURFACE_UNSPECIFIED)) return false;
    if (!preserve) return true;
    if (g->gpuHandle != handle || g->backend != backend || !gpu_adapter_info_equal(&binding, &g->selectedGpu) ||
        !nvapi_read_vf_snapshot(g, why, sizeof(why)) || memcmp(membership, g->vfMask, sizeof(membership))) {
        lb_log("mem-curve: binding or membership changed before preservation\n"); return false;
    }
    for (int i = 0; i < VF_NUM_POINTS; ++i) if (heldVoltage[i] != g->curve[i].volt_uV) {
        lb_log("mem-curve: topology changed before preservation ci=%d\n", i); return false;
    }
    using Function = int (*)(void*, void*);
    struct IO {
        void* handle;
        Function get, set;
        unsigned long long deadline;
        bool read(unsigned char* bytes, size_t) { return get(handle, bytes) == 0; }
        bool write(unsigned char* bytes, size_t) { return set(handle, bytes) == 0; }
        bool mayWrite() { return !deadline || pl_monotonic_ms() < deadline; }
    } io{handle, (Function)g->nvapiQi(backend->getControlId), (Function)g->nvapiQi(backend->setControlId), deadline};
    if (!io.get || !io.set) return false;
    const auto result = vf_exact_control_write(backend, membership, mask, heldHz, 3, io);
    lb_log("mem-curve: status=%d points=%d writes=%d firstCi=%d targetHz=%lld actualHz=%lld\n",
        (int)result.status, result.points, result.writes, result.firstCi, result.targetHz, result.actualHz);
    if (result.status != VF_EXACT_VERIFIED || !nvapi_read_vf_snapshot(g, why, sizeof(why)) || !g->vfOffsetsHzValid) return false;
    for (int i = 0; i < VF_NUM_POINTS; ++i) if (mask[i] && g->freqOffsetsHz[i] != heldHz[i]) return false;
    return true;
}
#endif
