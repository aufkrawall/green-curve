// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Shared stateful multi-generation hardware-free GPU simulator for regression
// tests. Models Pascal (2x VF units, scalar -200..+1200 MHz, no NVML hard pin),
// Turing (1x VF units, idle floor run, scalar -500..+1000 MHz), Ampere (1x VF
// units, scalar -500..+1000 MHz, NVML hard pin), and Blackwell (1x VF units,
// scalar -1000..+3000 MHz, NVML hard pin + client lock table) with monotonic VF
// enforcement, discrete Boost-bin quantization, and dynamic thermal Boost shifts.
#ifndef GREEN_CURVE_TESTS_GPU_GENERATION_SIM_HARNESS_H
#define GREEN_CURVE_TESTS_GPU_GENERATION_SIM_HARNESS_H

#include "gpu_core.h"
#include "platform.h"
#include "curve_feasibility_plan.h"
#include "hardware_capture_getters.h"
#include "hardware_capture_runtime.h"
#include "vf_backends.h"
#include "vf_curve_units.h"
#include "vf_offset_range_policy.h"

#include <string.h>

namespace gpu_gen_sim {

struct SimGpuSpec {
    const VfBackendSpec* backend;
    GpuFamily family;
    unsigned int osFamily;
    int pointCount;
    int scalarGpuMinMHz;
    int scalarGpuMaxMHz;
    int scalarMemMinMHz;
    int scalarMemMaxMHz;
    int powerDefaultMw;
    int powerCurrentMw;
    bool supportsHardPin;
    unsigned int baseVoltUv[VF_NUM_POINTS];
    unsigned int stockBaseKHz[VF_NUM_POINTS];
};

struct SimGpuDriver {
    SimGpuSpec spec{};
    int scalarGpuOffsetMHz = 0;
    int scalarMemOffsetMHz = 0;  // effective MHz as NVML reports
    int boostShiftKHz = 0;             // simulates GPU Boost thermal bin shift
    unsigned int boostShiftMinVoltUv = 0;  // 0 = all points; e.g. 740000 uV = Boost 3.0 upper curve only
    unsigned int boostBinStepKHz = 0;  // optional live frequency bin quantization
    int rawControlDelta[VF_NUM_POINTS]{};
    bool pinActive = false;
    unsigned int pinMinMHz = 0;
    unsigned int pinMaxMHz = 0;
    int setControlCalls = 0;
    int scalarSetCalls = 0;

    void reset_to_stock() {
        scalarGpuOffsetMHz = 0;
        scalarMemOffsetMHz = 0;
        pinActive = false;
        pinMinMHz = 0;
        pinMaxMHz = 0;
        memset(rawControlDelta, 0, sizeof(rawControlDelta));
    }

    // Evaluate live monotonic VF curve frequencies in physical kHz.
    void evaluate_live_curve(VFCurvePoint* outCurve, int* outOffsetsKHz,
                             gc_i64* outOffsetsHz = nullptr) const {
        memset(outCurve, 0, sizeof(VFCurvePoint) * VF_NUM_POINTS);
        memset(outOffsetsKHz, 0, sizeof(int) * VF_NUM_POINTS);
        if (outOffsetsHz) memset(outOffsetsHz, 0, sizeof(gc_i64) * VF_NUM_POINTS);
        unsigned int runningFloorKHz = 0;
        for (int i = 0; i < spec.pointCount && i < VF_NUM_POINTS; ++i) {
            const int offKHz = vf_graphics_offset_khz(spec.backend, rawControlDelta[i]);
            gc_i64 offHz = 0;
            (void)vf_graphics_offset_hz(spec.backend, rawControlDelta[i], &offHz);
            const int pointBoostKHz = spec.baseVoltUv[i] >= boostShiftMinVoltUv ? boostShiftKHz : 0;
            long long effKHz = (long long)spec.stockBaseKHz[i] + pointBoostKHz +
                (long long)scalarGpuOffsetMHz * 1000LL + offKHz;
            if (effKHz < 135000LL) effKHz = 135000LL;
            if (boostBinStepKHz > 0) {
                effKHz = ((effKHz + (long long)(boostBinStepKHz / 2)) /
                          (long long)boostBinStepKHz) * (long long)boostBinStepKHz;
            }
            unsigned int liveKHz = (unsigned int)effKHz;
            if (liveKHz < runningFloorKHz) liveKHz = runningFloorKHz;
            runningFloorKHz = liveKHz;
            outCurve[i].freq_kHz = liveKHz;
            outCurve[i].volt_uV = spec.baseVoltUv[i];
            outOffsetsKHz[i] = offKHz;
            if (outOffsetsHz) outOffsetsHz[i] = offHz;
        }
    }

    bool set_scalar_gpu_offset_mhz(int mhz) {
        ++scalarSetCalls;
        if (mhz < spec.scalarGpuMinMHz || mhz > spec.scalarGpuMaxMHz) return false;
        scalarGpuOffsetMHz = mhz;
        return true;
    }

    bool set_physical_point_offset_khz(int ci, int offsetKHz) {
        if (ci < 0 || ci >= spec.pointCount) return false;
        int raw = 0;
        if (!vf_graphics_offset_driver_units(spec.backend, offsetKHz, &raw)) return false;
        rawControlDelta[ci] = raw;
        return true;
    }

    bool set_physical_point_offset_hz(int ci, gc_i64 offsetHz) {
        if (ci < 0 || ci >= spec.pointCount) return false;
        int raw = 0;
        if (!vf_graphics_offset_driver_units_hz(spec.backend, offsetHz, &raw)) return false;
        rawControlDelta[ci] = raw;
        return true;
    }

    bool apply_masked_offsets_khz(const int* offsetsKHz, const bool* mask) {
        if (!offsetsKHz || !mask) return false;
        ++setControlCalls;
        for (int i = 0; i < spec.pointCount && i < VF_NUM_POINTS; ++i) {
            if (!mask[i]) continue;
            if (!set_physical_point_offset_khz(i, offsetsKHz[i])) return false;
        }
        return true;
    }
};

inline SimGpuDriver*& active_driver() {
    static SimGpuDriver* active = nullptr;
    return active;
}

inline int sim_nvapi_table(void* handle, void* buffer, int kind) {
    auto& d = *(SimGpuDriver*)handle;
    const VfBackendSpec& b = *d.spec.backend;
    const unsigned int sizes[] = {b.infoBufferSize, b.statusBufferSize, b.controlBufferSize};
    const unsigned int masks[] = {b.infoMaskOffset, b.statusMaskOffset, b.controlMaskOffset};
    const unsigned int versions[] = {b.infoVersion, b.statusVersion, b.controlVersion};
    auto* bytes = (unsigned char*)buffer;
    unsigned int version = 0;
    memcpy(&version, bytes, 4);
    if (version != ((versions[kind] << 16) | sizes[kind])) return -99;
    memset(bytes, 0, sizes[kind]);
    memcpy(bytes, &version, 4);

    VFCurvePoint live[VF_NUM_POINTS]{};
    int liveOff[VF_NUM_POINTS]{};
    d.evaluate_live_curve(live, liveOff);

    for (int i = 0; i < d.spec.pointCount && i < VF_NUM_POINTS; ++i) {
        bytes[masks[kind] + (unsigned int)(i / 8)] |= (unsigned char)(1u << (i % 8));
        if (kind == 1) {
            const unsigned int at = b.statusEntriesOffset + (unsigned int)i * b.statusEntryStride;
            const unsigned int rawFreq = live[i].freq_kHz * b.graphicsFrequencyScale;
            const unsigned int volt = live[i].volt_uV;
            memcpy(bytes + at, &rawFreq, 4);
            memcpy(bytes + at + 4, &volt, 4);
        } else if (kind == 2) {
            const unsigned int at = b.controlEntryBaseOffset +
                (unsigned int)i * b.controlEntryStride + b.controlEntryDeltaOffset;
            const int rawDelta = d.rawControlDelta[i];
            memcpy(bytes + at, &rawDelta, 4);
        }
    }
    return 0;
}

inline int sim_nvapi_info(void* h, void* b) { return sim_nvapi_table(h, b, 0); }
inline int sim_nvapi_status(void* h, void* b) { return sim_nvapi_table(h, b, 1); }
inline int sim_nvapi_control(void* h, void* b) { return sim_nvapi_table(h, b, 2); }

inline void* sim_nvapi_query(unsigned int id) {
    SimGpuDriver* act = active_driver();
    if (!act || !act->spec.backend) return nullptr;
    const VfBackendSpec& b = *act->spec.backend;
    if (id == b.getInfoId) return (void*)sim_nvapi_info;
    if (id == b.getStatusId) return (void*)sim_nvapi_status;
    if (id == b.getControlId) return (void*)sim_nvapi_control;
    return nullptr;
}

inline nvmlReturn_t sim_nvml_clock_offsets(nvmlDevice_t, nvmlClockOffset_t* v) {
    SimGpuDriver* act = active_driver();
    if (!act || !v) return NVML_ERROR_INVALID_ARGUMENT;
    if (v->type == NVML_CLOCK_GRAPHICS) {
        v->clockOffsetMHz = act->scalarGpuOffsetMHz;
        v->minClockOffsetMHz = act->spec.scalarGpuMinMHz;
        v->maxClockOffsetMHz = act->spec.scalarGpuMaxMHz;
        return NVML_SUCCESS;
    }
    if (v->type == NVML_CLOCK_MEM) {
        v->clockOffsetMHz = act->scalarMemOffsetMHz;
        v->minClockOffsetMHz = act->spec.scalarMemMinMHz;
        v->maxClockOffsetMHz = act->spec.scalarMemMaxMHz;
        return NVML_SUCCESS;
    }
    return NVML_ERROR_NOT_SUPPORTED;
}

inline nvmlReturn_t sim_nvml_power(nvmlDevice_t, unsigned int* v) {
    SimGpuDriver* act = active_driver();
    if (!act || !v) return NVML_ERROR_INVALID_ARGUMENT;
    *v = (unsigned int)act->spec.powerCurrentMw;
    return NVML_SUCCESS;
}

inline nvmlReturn_t sim_nvml_default_power(nvmlDevice_t, unsigned int* v) {
    SimGpuDriver* act = active_driver();
    if (!act || !v) return NVML_ERROR_INVALID_ARGUMENT;
    *v = (unsigned int)act->spec.powerDefaultMw;
    return NVML_SUCCESS;
}

inline nvmlReturn_t sim_nvml_power_constraints(nvmlDevice_t, unsigned int* lo, unsigned int* hi) {
    SimGpuDriver* act = active_driver();
    if (!act || !lo || !hi) return NVML_ERROR_INVALID_ARGUMENT;
    *lo = (unsigned int)(act->spec.powerDefaultMw / 2);
    *hi = (unsigned int)(act->spec.powerDefaultMw + act->spec.powerDefaultMw / 5);
    return NVML_SUCCESS;
}

inline NvapiClientLocks sim_read_locks(void*) {
    SimGpuDriver* act = active_driver();
    NvapiClientLocks locks{};
    locks.known = true;
    locks.rejectedIndex = -1;
    if (act && act->pinActive) {
        locks.graphicsMinPresent = locks.graphicsMaxPresent = true;
        locks.graphicsMinHz = (gc_i64)act->pinMinMHz * 1000000LL;
        locks.graphicsMaxHz = (gc_i64)act->pinMaxMHz * 1000000LL;
    }
    return locks;
}

inline bool sim_collect_capture(SimGpuDriver& driver, gc_u32 domains,
                                HardwareCaptureObservation& out) {
    active_driver() = &driver;
    NvmlApi api{};
    api.getClockOffsets = sim_nvml_clock_offsets;
    api.getPowerLimit = sim_nvml_power;
    api.getPowerDefaultLimit = sim_nvml_default_power;
    api.getPowerConstraints = sim_nvml_power_constraints;

    HardwareCaptureContext ctx{};
    ctx.usable = true;
    ctx.nvmlUsable = true;
    ctx.gpuHandle = &driver;
    ctx.query = sim_nvapi_query;
    ctx.backend = driver.spec.backend;
    ctx.nvml = &api;
    ctx.nvmlDevice = (nvmlDevice_t)&driver;
    ctx.binding.valid = 1;
    ctx.binding.pciInfoValid = 1;
    ctx.binding.deviceId = (gc_u32)(0x1B00u + (unsigned int)driver.spec.family);
    ctx.gpuGeneration = 1;
    ctx.osFamily = driver.spec.osFamily;
    ctx.backendFamily = (gc_u32)driver.spec.family;
    ctx.lockReader = sim_read_locks;
    snprintf(ctx.driverVersion, sizeof(ctx.driverVersion), "%s", "sim-driver");

    char why[256]{};
    const auto read = [&](HardwareCaptureObservation& dst, char* reason, size_t size) {
        return hardware_capture_collect(ctx, domains,
            [](const HardwareCaptureContext&, HardwareCaptureObservation&) {},
            dst, reason, size);
    };
    return hardware_capture_consistent(read, out, why, sizeof(why));
}

inline bool plan_has_reason(const CurvePlanResult& r, CurvePlanReason reason) {
    for (int i = 0; i < r.issueCount; ++i)
        if (r.issues[i].reason == reason) return true;
    return false;
}

inline SimGpuSpec make_pascal_gp102_spec(unsigned int osFamily) {
    static const unsigned int kStockMHz[80] = {
        215, 291, 367, 443, 481, 481, 481, 481, 481, 481,
        481, 481, 532, 582, 633, 683, 734, 785, 835, 886,
        937, 987, 1038, 1088, 1139, 1190, 1240, 1291, 1342, 1367,
        1392, 1418, 1430, 1455, 1481, 1506, 1531, 1544, 1569, 1595,
        1607, 1633, 1645, 1671, 1683, 1709, 1721, 1734, 1759, 1772,
        1785, 1797, 1810, 1835, 1848, 1860, 1873, 1886, 1886, 1898,
        1911, 1924, 1936, 1936, 1949, 1962, 1962, 1974, 1974, 1974,
        1974, 1974, 1974, 1974, 1974, 1974, 1974, 1974, 1974, 1974,
    };
    SimGpuSpec s{};
    s.backend = &g_vfBackendPascal;
    s.family = GPU_FAMILY_PASCAL;
    s.osFamily = osFamily;
    s.pointCount = 80;
    s.scalarGpuMinMHz = -200;
    s.scalarGpuMaxMHz = 1200;
    s.scalarMemMinMHz = -2000;
    s.scalarMemMaxMHz = 2000;
    s.powerDefaultMw = 250000;
    s.powerCurrentMw = 125000;
    s.supportsHardPin = false;
    for (int i = 0; i < 80; ++i) {
        s.baseVoltUv[i] = 450000u + (unsigned int)i * 10000u;
        s.stockBaseKHz[i] = kStockMHz[i] * 1000u;
    }
    s.baseVoltUv[20] = 650000u;
    s.baseVoltUv[21] = 662500u;
    s.baseVoltUv[22] = 675000u;
    for (int i = 23; i < 80; ++i) s.baseVoltUv[i] = 675000u + (unsigned int)(i - 22) * 10000u;
    s.baseVoltUv[79] = 1243750u;
    return s;
}

inline SimGpuSpec make_turing_spec(unsigned int osFamily) {
    SimGpuSpec s{};
    s.backend = &g_vfBackendTuring;
    s.family = GPU_FAMILY_TURING;
    s.osFamily = osFamily;
    s.pointCount = 100;
    s.scalarGpuMinMHz = -500;
    s.scalarGpuMaxMHz = 1000;
    s.scalarMemMinMHz = -2000;
    s.scalarMemMaxMHz = 2000;
    s.powerDefaultMw = 260000;
    s.powerCurrentMw = 260000;
    s.supportsHardPin = true;
    for (int i = 0; i < 100; ++i) {
        s.baseVoltUv[i] = 700000u + (unsigned int)i * 4000u;
        // Flat idle floor run on ci=0..4 at 300 MHz, then ascending to 2040 MHz.
        const unsigned int mhz = i <= 4 ? 300u : (unsigned int)(420 + (i - 5) * 18);
        s.stockBaseKHz[i] = (mhz > 2040u ? 2040u : mhz) * 1000u;
    }
    return s;
}

inline SimGpuSpec make_ampere_spec(unsigned int osFamily) {
    SimGpuSpec s{};
    s.backend = &g_vfBackendAmpere;
    s.family = GPU_FAMILY_AMPERE;
    s.osFamily = osFamily;
    s.pointCount = 128;
    s.scalarGpuMinMHz = -500;
    s.scalarGpuMaxMHz = 1000;
    s.scalarMemMinMHz = -2000;
    s.scalarMemMaxMHz = 2000;
    s.powerDefaultMw = 320000;
    s.powerCurrentMw = 320000;
    s.supportsHardPin = true;
    for (int i = 0; i < 128; ++i) {
        s.baseVoltUv[i] = 700000u + (unsigned int)i * 3125u;
        const unsigned int mhz = i < 8 ? 210u : (unsigned int)(450 + (i - 8) * 15);
        s.stockBaseKHz[i] = (mhz > 2150u ? 2150u : mhz) * 1000u;
    }
    return s;
}

inline SimGpuSpec make_blackwell_spec(unsigned int osFamily) {
    SimGpuSpec s{};
    s.backend = &g_vfBackendBlackwell;
    s.family = GPU_FAMILY_BLACKWELL;
    s.osFamily = osFamily;
    s.pointCount = 128;
    s.scalarGpuMinMHz = -1000;
    s.scalarGpuMaxMHz = 3000;
    s.scalarMemMinMHz = -4000;
    s.scalarMemMaxMHz = 6000;
    s.powerDefaultMw = 250000;
    s.powerCurrentMw = 250000;
    s.supportsHardPin = true;
    for (int i = 0; i < 128; ++i) {
        s.baseVoltUv[i] = 750000u + (unsigned int)i * 3125u;
        const unsigned int mhz = i < 10 ? 300u : (unsigned int)(750 + (i - 10) * 24);
        s.stockBaseKHz[i] = (mhz > 3090u ? 3090u : mhz) * 1000u;
    }
    return s;
}

}  // namespace gpu_gen_sim

#endif  // GREEN_CURVE_TESTS_GPU_GENERATION_SIM_HARNESS_H
