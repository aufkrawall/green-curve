// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_backend.cpp — native Linux GPU control backend (see linux_backend.h).
// Faithfully ports the Windows NvAPI/NVML read+apply logic; each function names
// its Windows counterpart in a comment so the two stay in sync.

#include "linux_backend.h"
#include "linux_gpu_selection.h"
#include "vf_backends.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Logging — to stderr so systemd captures it in the journal.
// ---------------------------------------------------------------------------
static bool g_lbDebug = true;
static void lb_log(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
static void lb_log(const char* fmt, ...) {
    if (!g_lbDebug) return;
    va_list ap;
    va_start(ap, fmt);
    // flawfinder: ignore -- private logger; every call site supplies a constant format.
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// NvAPI status is 0 == OK on every OS; Linux error codes are NEGATIVE (-1
// generic, -9 INCOMPATIBLE_STRUCT_VERSION), so the only portable test is == 0.
static inline bool nvapi_ok(int status) { return status == 0; }

// Map the (Linux-negative) NvAPI status to a name for diagnostics — so a struct
// layout/ABI mismatch on a new driver or arch is obvious in the logs.
static const char* nvapi_status_name(int status) {
    switch (status) {
        case 0:  return "OK";
        case -1: return "ERROR";
        case -5: return "INVALID_ARGUMENT";
        case -6: return "HANDLE_INVALIDATED";
        case -9: return "INCOMPATIBLE_STRUCT_VERSION";
        default: return "ERROR(other)";
    }
}

typedef void* (*nvapi_qi_t)(unsigned int);
typedef int (*nvapi_init_t)();
typedef int (*nvapi_enum_t)(GPU_HANDLE*, int*);
typedef int (*nvapi_name_t)(GPU_HANDLE, char*);
typedef int (*nvapi_arch_t)(GPU_HANDLE, nvapiGpuArchInfo_t*);
typedef int (*nvapi_buf_t)(void*, void*);  // get/set status/info/control

template <typename Fn>
static Fn sym(PlLib lib, const char* name) {
    return reinterpret_cast<Fn>(pl_lib_sym(lib, name));
}

// ===========================================================================
// NvAPI VF curve  (ports: nvapi_get_vf_info_cached, nvapi_read_curve,
//                  nvapi_read_control_table, nvapi_read_offsets,
//                  nvapi_set_point, apply_curve_offsets_verified)
// ===========================================================================

static const unsigned int LB_CONTROL_BUF_SIZE = 0x4000;

// ports nvapi_get_vf_info_cached(): per-point editable mask + active clock count
static bool nvapi_get_vf_info(LinuxGpuState* g) {
    const VfBackendSpec* b = g->backend;
    if (!b) return false;
    if (g->vfInfoCached) return true;

    memset(g->vfMask, 0, sizeof(g->vfMask));
    memset(g->vfMask, 0xFF, 16);  // default: first 128 bits editable
    g->vfNumClocks = b->defaultNumClocks;

    auto getInfo = (nvapi_buf_t)g->nvapiQi(b->getInfoId);
    if (getInfo) {
        unsigned int infoSize = b->infoBufferSize ? b->infoBufferSize : 0x4000;
        if (infoSize > 0x4000) infoSize = 0x4000;
        unsigned char* ibuf = (unsigned char*)calloc(1, infoSize);
        if (ibuf && b->infoBufferSize <= infoSize) {
            unsigned int ver = (b->infoVersion << 16) | infoSize;
            memcpy(ibuf, &ver, sizeof(ver));
            if (b->infoMaskOffset + sizeof(g->vfMask) <= infoSize)
                memset(ibuf + b->infoMaskOffset, 0xFF, sizeof(g->vfMask));
            if (getInfo(g->gpuHandle, ibuf) == 0) {
                if (b->infoMaskOffset + sizeof(g->vfMask) <= infoSize)
                    memcpy(g->vfMask, ibuf + b->infoMaskOffset, sizeof(g->vfMask));
                if (b->infoNumClocksOffset + sizeof(g->vfNumClocks) <= infoSize)
                    memcpy(&g->vfNumClocks, ibuf + b->infoNumClocksOffset, sizeof(g->vfNumClocks));
                if (g->vfNumClocks == 0) g->vfNumClocks = b->defaultNumClocks;
                g->vfInfoCached = true;
            }
        }
        free(ibuf);
    }
    return true;
}

// ports nvapi_read_curve()
static bool nvapi_read_curve(LinuxGpuState* g) {
    const VfBackendSpec* b = g->backend;
    if (!b || !b->readSupported) return false;
    if (!nvapi_get_vf_info(g)) return false;
    auto getStatus = (nvapi_buf_t)g->nvapiQi(b->getStatusId);
    if (!getStatus || b->statusBufferSize == 0 || b->statusBufferSize > 0x4000) return false;
    unsigned char* buf = (unsigned char*)calloc(1, b->statusBufferSize);
    if (!buf) return false;
    unsigned int ver = (b->statusVersion << 16) | b->statusBufferSize;
    memcpy(buf, &ver, sizeof(ver));
    if (b->statusMaskOffset + sizeof(g->vfMask) <= b->statusBufferSize)
        memcpy(buf + b->statusMaskOffset, g->vfMask, sizeof(g->vfMask));
    if (b->statusNumClocksOffset + sizeof(g->vfNumClocks) <= b->statusBufferSize)
        memcpy(buf + b->statusNumClocksOffset, &g->vfNumClocks, sizeof(g->vfNumClocks));
    bool ok = false;
    if (getStatus(g->gpuHandle, buf) == 0) {
        g->numPopulated = 0;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            unsigned int freq = 0, volt = 0;
            unsigned int off = b->statusEntriesOffset + (unsigned int)i * b->statusEntryStride;
            if (off + 8 <= b->statusBufferSize) {
                memcpy(&freq, buf + off, 4);
                memcpy(&volt, buf + off + 4, 4);
            }
            g->curve[i].freq_kHz = freq;
            g->curve[i].volt_uV = volt;
            if (freq > 0) g->numPopulated++;
        }
        ok = true;
    }
    free(buf);
    return ok;
}

// ports nvapi_read_control_table()
static bool nvapi_read_control_table(LinuxGpuState* g, unsigned char* buf, unsigned int bufSize) {
    const VfBackendSpec* b = g->backend;
    if (!b || !buf || bufSize < b->controlBufferSize) return false;
    auto getFunc = (nvapi_buf_t)g->nvapiQi(b->getControlId);
    if (!getFunc) return false;
    if (!nvapi_get_vf_info(g)) return false;
    memset(buf, 0, b->controlBufferSize);
    unsigned int ver = (b->controlVersion << 16) | b->controlBufferSize;
    memcpy(buf, &ver, sizeof(ver));
    if (b->controlMaskOffset + sizeof(g->vfMask) > b->controlBufferSize) return false;
    memcpy(buf + b->controlMaskOffset, g->vfMask, sizeof(g->vfMask));
    return getFunc(g->gpuHandle, buf) == 0;
}

// ports nvapi_read_offsets()
static bool nvapi_read_offsets(LinuxGpuState* g) {
    const VfBackendSpec* b = g->backend;
    if (!b || !b->readSupported || b->controlBufferSize > 0x4000) return false;
    unsigned char* buf = (unsigned char*)calloc(1, b->controlBufferSize ? b->controlBufferSize : 0x4000);
    if (!buf) return false;
    bool ok = nvapi_read_control_table(g, buf, b->controlBufferSize);
    if (ok) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            int delta = 0;
            unsigned int off = b->controlEntryBaseOffset + (unsigned int)i * b->controlEntryStride + b->controlEntryDeltaOffset;
            if (off + sizeof(delta) <= b->controlBufferSize) memcpy(&delta, buf + off, sizeof(delta));
            else delta = 0;
            g->freqOffsets[i] = delta;
        }
    }
    free(buf);
    return ok;
}

// ports get_curve_offset_range_khz() / clamp_freq_delta_khz()
static void curve_offset_range_khz(LinuxGpuState* g, int* minKHz, int* maxKHz) {
    const int FALLBACK = 1000000;  // 1000 MHz (matches Windows)
    int mn = -FALLBACK, mx = FALLBACK;
    if (g->curveOffsetRangeKnown && g->curveOffsetMinKHz <= g->curveOffsetMaxKHz) {
        mn = g->curveOffsetMinKHz;
        mx = g->curveOffsetMaxKHz;
    } else if (g->gpuOffsetMinMHz <= g->gpuOffsetMaxMHz && (g->gpuOffsetMinMHz || g->gpuOffsetMaxMHz)) {
        mn = g->gpuOffsetMinMHz * 1000;
        mx = g->gpuOffsetMaxMHz * 1000;
    }
    if (minKHz) *minKHz = mn;
    if (maxKHz) *maxKHz = mx;
}

static int clamp_freq_delta_khz(LinuxGpuState* g, int freqDelta_kHz) {
    int mn = 0, mx = 0;
    curve_offset_range_khz(g, &mn, &mx);
    if (freqDelta_kHz > mx) { lb_log("clamp_freq_delta_khz: %d -> max %d\n", freqDelta_kHz, mx); return mx; }
    if (freqDelta_kHz < mn) { lb_log("clamp_freq_delta_khz: %d -> min %d\n", freqDelta_kHz, mn); return mn; }
    return freqDelta_kHz;
}

// ports apply_curve_offsets_verified(): batch per-point writes with readback
// verification.  The set-control mask carries multiple bits per pass on Windows;
// per the LACT/NVCurve finding, on a driver that rejects multi-bit masks the
// verify loop converges via repeated single-changed-point passes.
static bool apply_curve_offsets_verified(LinuxGpuState* g, const int* targetOffsets,
                                         const bool* pointMask, int maxBatchPasses) {
    const VfBackendSpec* b = g->backend;
    if (!b || !b->writeSupported || !targetOffsets || !pointMask) return false;

    bool desiredMask[VF_NUM_POINTS] = {};
    int desiredOffsets[VF_NUM_POINTS] = {};
    bool pendingMask[VF_NUM_POINTS] = {};
    int desiredCount = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (!pointMask[i] || g->curve[i].freq_kHz == 0) continue;
        if (!(g->vfMask[i / 8] & (1u << (i % 8)))) {
            lb_log("curve: point %d not in vfMask, skipping\n", i);
            continue;
        }
        desiredMask[i] = pendingMask[i] = true;
        desiredOffsets[i] = clamp_freq_delta_khz(g, targetOffsets[i]);
        desiredCount++;
    }
    if (desiredCount == 0) return true;
    if (maxBatchPasses < 1) maxBatchPasses = 1;

    auto setFunc = (nvapi_buf_t)g->nvapiQi(b->setControlId);
    if (!setFunc) return false;
    if (b->controlBufferSize > LB_CONTROL_BUF_SIZE) return false;

    unsigned char* baseControl = (unsigned char*)calloc(1, LB_CONTROL_BUF_SIZE);
    unsigned char* batchBuf = (unsigned char*)calloc(1, LB_CONTROL_BUF_SIZE);
    bool ok = false;
    if (baseControl && batchBuf && nvapi_read_control_table(g, baseControl, LB_CONTROL_BUF_SIZE)) {
        for (int pass = 0; pass < maxBatchPasses; pass++) {
            memcpy(batchBuf, baseControl, b->controlBufferSize);
            unsigned char writeMask[32] = {};
            bool anyPendingWrite = false;
            int pointsInPass = 0;
            for (int i = 0; i < VF_NUM_POINTS; i++) {
                if (!pendingMask[i]) continue;
                int cur = 0;
                unsigned int off = b->controlEntryBaseOffset + (unsigned int)i * b->controlEntryStride + b->controlEntryDeltaOffset;
                if (off + sizeof(cur) > b->controlBufferSize) { anyPendingWrite = false; break; }
                memcpy(&cur, batchBuf + off, sizeof(cur));
                if (cur == desiredOffsets[i]) { pendingMask[i] = false; continue; }
                memcpy(batchBuf + off, &desiredOffsets[i], sizeof(desiredOffsets[i]));
                writeMask[i / 8] |= (unsigned char)(1u << (i % 8));
                anyPendingWrite = true;
                pointsInPass++;
            }
            if (!anyPendingWrite) { ok = true; break; }
            memcpy(batchBuf + b->controlMaskOffset, writeMask, sizeof(writeMask));
            int setRet = setFunc(g->gpuHandle, batchBuf);
            lb_log("curve batch pass %d: points=%d ret=%d\n", pass + 1, pointsInPass, setRet);
            if (setRet != 0) break;

            bool readOk = false;
            for (int t = 0; t < 6; t++) {
                if (t > 0) pl_sleep_ms(10);
                if (nvapi_read_offsets(g)) { readOk = true; break; }
            }
            if (!readOk) break;

            bool anyPending = false;
            for (int i = 0; i < VF_NUM_POINTS; i++) {
                if (!desiredMask[i]) continue;
                pendingMask[i] = (g->freqOffsets[i] != desiredOffsets[i]);
                if (pendingMask[i]) anyPending = true;
                unsigned int off = b->controlEntryBaseOffset + (unsigned int)i * b->controlEntryStride + b->controlEntryDeltaOffset;
                if (off + sizeof(g->freqOffsets[i]) <= b->controlBufferSize)
                    memcpy(baseControl + off, &g->freqOffsets[i], sizeof(g->freqOffsets[i]));
            }
            if (!anyPending) { ok = true; break; }
        }
    }
    free(baseControl);
    free(batchBuf);
    return ok;
}

// ===========================================================================
// NVML  (ports: nvml_resolve, nvml clock-offset/power/locked/fan helpers)
// ===========================================================================

typedef nvmlReturn_t (*nvmlDeviceGetName_t)(nvmlDevice_t, char*, unsigned int);

static void nvml_resolve(LinuxGpuState* g) {
    PlLib h = g->nvmlLib;
    NvmlApi* a = &g->nvml;
    a->init = sym<nvmlInit_v2_t>(h, "nvmlInit_v2");
    a->shutdown = sym<nvmlShutdown_t>(h, "nvmlShutdown");
    a->getCount = sym<nvmlDeviceGetCount_v2_t>(h, "nvmlDeviceGetCount_v2");
    a->getHandleByIndex = sym<nvmlDeviceGetHandleByIndex_v2_t>(h, "nvmlDeviceGetHandleByIndex_v2");
    a->getPciInfo = sym<nvmlDeviceGetPciInfo_t>(h, "nvmlDeviceGetPciInfo_v3");
    if (!a->getPciInfo) a->getPciInfo = sym<nvmlDeviceGetPciInfo_t>(h, "nvmlDeviceGetPciInfo_v2");
    a->getPowerLimit = sym<nvmlDeviceGetPowerManagementLimit_t>(h, "nvmlDeviceGetPowerManagementLimit");
    a->getPowerDefaultLimit = sym<nvmlDeviceGetPowerManagementDefaultLimit_t>(h, "nvmlDeviceGetPowerManagementDefaultLimit");
    a->getPowerConstraints = sym<nvmlDeviceGetPowerManagementLimitConstraints_t>(h, "nvmlDeviceGetPowerManagementLimitConstraints");
    a->setPowerLimit = sym<nvmlDeviceSetPowerManagementLimit_t>(h, "nvmlDeviceSetPowerManagementLimit");
    a->getClockOffsets = sym<nvmlDeviceGetClockOffsets_t>(h, "nvmlDeviceGetClockOffsets");
    a->setClockOffsets = sym<nvmlDeviceSetClockOffsets_t>(h, "nvmlDeviceSetClockOffsets");
    a->getPerformanceState = sym<nvmlDeviceGetPerformanceState_t>(h, "nvmlDeviceGetPerformanceState");
    a->getGpcClkVfOffset = sym<nvmlDeviceGetGpcClkVfOffset_t>(h, "nvmlDeviceGetGpcClkVfOffset");
    a->getMemClkVfOffset = sym<nvmlDeviceGetMemClkVfOffset_t>(h, "nvmlDeviceGetMemClkVfOffset");
    a->getGpcClkMinMaxVfOffset = sym<nvmlDeviceGetGpcClkMinMaxVfOffset_t>(h, "nvmlDeviceGetGpcClkMinMaxVfOffset");
    a->getMemClkMinMaxVfOffset = sym<nvmlDeviceGetMemClkMinMaxVfOffset_t>(h, "nvmlDeviceGetMemClkMinMaxVfOffset");
    a->setGpcClkVfOffset = sym<nvmlDeviceSetGpcClkVfOffset_t>(h, "nvmlDeviceSetGpcClkVfOffset");
    a->setMemClkVfOffset = sym<nvmlDeviceSetMemClkVfOffset_t>(h, "nvmlDeviceSetMemClkVfOffset");
    a->getNumFans = sym<nvmlDeviceGetNumFans_t>(h, "nvmlDeviceGetNumFans");
    a->getMinMaxFanSpeed = sym<nvmlDeviceGetMinMaxFanSpeed_t>(h, "nvmlDeviceGetMinMaxFanSpeed");
    a->getFanControlPolicy = sym<nvmlDeviceGetFanControlPolicy_v2_t>(h, "nvmlDeviceGetFanControlPolicy_v2");
    a->setFanControlPolicy = sym<nvmlDeviceSetFanControlPolicy_t>(h, "nvmlDeviceSetFanControlPolicy");
    a->getFanSpeed = sym<nvmlDeviceGetFanSpeed_v2_t>(h, "nvmlDeviceGetFanSpeed_v2");
    a->setFanSpeed = sym<nvmlDeviceSetFanSpeed_v2_t>(h, "nvmlDeviceSetFanSpeed_v2");
    a->setDefaultFanSpeed = sym<nvmlDeviceSetDefaultFanSpeed_v2_t>(h, "nvmlDeviceSetDefaultFanSpeed_v2");
    a->getTemperature = sym<nvmlDeviceGetTemperature_t>(h, "nvmlDeviceGetTemperature");
    a->getClock = sym<nvmlDeviceGetClock_t>(h, "nvmlDeviceGetClock");
    a->setGpuLockedClocks = sym<nvmlDeviceSetGpuLockedClocks_t>(h, "nvmlDeviceSetGpuLockedClocks");
    a->resetGpuLockedClocks = sym<nvmlDeviceResetGpuLockedClocks_t>(h, "nvmlDeviceResetGpuLockedClocks");
    a->setMemoryLockedClocks = sym<nvmlDeviceSetMemoryLockedClocks_t>(h, "nvmlDeviceSetMemoryLockedClocks");
    a->resetMemoryLockedClocks = sym<nvmlDeviceResetMemoryLockedClocks_t>(h, "nvmlDeviceResetMemoryLockedClocks");
}

// ports nvml_set_clock_offset_domain(): set a clock-domain offset in MHz.
static bool nvml_set_clock_offset(LinuxGpuState* g, unsigned int domain, int offsetMHz) {
    NvmlApi* a = &g->nvml;
    if (a->setClockOffsets && a->getPerformanceState) {
        unsigned int pstate = NVML_PSTATE_0;
        a->getPerformanceState(g->nvmlDevice, &pstate);
        nvmlClockOffset_t info = {};
        info.version = nvmlClockOffset_v1;
        info.type = domain;
        info.pstate = pstate;
        info.clockOffsetMHz = offsetMHz;
        if (a->setClockOffsets(g->nvmlDevice, &info) == NVML_SUCCESS) {
            if (a->getClockOffsets) {
                nvmlClockOffset_t verify = info;
                verify.clockOffsetMHz = 0;
                return a->getClockOffsets(g->nvmlDevice, &verify) == NVML_SUCCESS &&
                       verify.clockOffsetMHz == offsetMHz;
            }
            return true;
        }
    }
    if (domain == NVML_CLOCK_GRAPHICS && a->setGpcClkVfOffset) {
        if (a->setGpcClkVfOffset(g->nvmlDevice, offsetMHz) != NVML_SUCCESS) return false;
        int verify = 0;
        return !a->getGpcClkVfOffset ||
               (a->getGpcClkVfOffset(g->nvmlDevice, &verify) == NVML_SUCCESS && verify == offsetMHz);
    }
    if (domain == NVML_CLOCK_MEM && a->setMemClkVfOffset) {
        if (a->setMemClkVfOffset(g->nvmlDevice, offsetMHz) != NVML_SUCCESS) return false;
        int verify = 0;
        return !a->getMemClkVfOffset ||
               (a->getMemClkVfOffset(g->nvmlDevice, &verify) == NVML_SUCCESS && verify == offsetMHz);
    }
    return false;
}

static bool nvml_set_power_limit_pct(LinuxGpuState* g, int pct) {
    NvmlApi* a = &g->nvml;
    if (!a->setPowerLimit) return false;
    int defmW = g->powerLimitDefaultmW;
    if (defmW <= 0 && a->getPowerDefaultLimit) {
        unsigned int d = 0;
        if (a->getPowerDefaultLimit(g->nvmlDevice, &d) == NVML_SUCCESS) defmW = (int)d;
    }
    if (defmW <= 0) return false;
    long target = (long)defmW * pct / 100;
    if (g->powerLimitMinmW > 0 && target < g->powerLimitMinmW) target = g->powerLimitMinmW;
    if (g->powerLimitMaxmW > 0 && target > g->powerLimitMaxmW) target = g->powerLimitMaxmW;
    nvmlReturn_t r = a->setPowerLimit(g->nvmlDevice, (unsigned int)target);
    lb_log("power: set %d%% -> %ld mW ret=%d\n", pct, target, (int)r);
    if (r != NVML_SUCCESS) return false;
    unsigned int verify = 0;
    return !a->getPowerLimit ||
           (a->getPowerLimit(g->nvmlDevice, &verify) == NVML_SUCCESS && verify == (unsigned int)target);
}

static bool nvml_set_fan(LinuxGpuState* g, int fanMode, bool fanAuto, int fanPercent) {
    NvmlApi* a = &g->nvml;
    unsigned int numFans = 0;
    if (!a->getNumFans || a->getNumFans(g->nvmlDevice, &numFans) != NVML_SUCCESS ||
        numFans == 0)
        return false;
    if (numFans > MAX_GPU_FANS) numFans = MAX_GPU_FANS;
    bool ok = true;
    if (fanAuto || fanMode == FAN_MODE_AUTO) {
        for (unsigned int f = 0; f < numFans; f++) {
            bool fanOk = false;
            if (a->setDefaultFanSpeed)
                fanOk = a->setDefaultFanSpeed(g->nvmlDevice, f) == NVML_SUCCESS;
            else if (a->setFanControlPolicy)
                fanOk = a->setFanControlPolicy(g->nvmlDevice, f,
                    NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) == NVML_SUCCESS;
            unsigned int policy = 0;
            fanOk = fanOk && a->getFanControlPolicy &&
                    a->getFanControlPolicy(g->nvmlDevice, f, &policy) == NVML_SUCCESS &&
                    policy == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW;
            if (!fanOk) lb_log("fan: auto restore failed for fan %u\n", f);
            ok &= fanOk;
        }
        return ok;
    }
    // Fixed (and the initial set for curve mode; the daemon reasserts curve).
    if (fanPercent < 0) fanPercent = 0;
    if (fanPercent > 100) fanPercent = 100;
    if (!a->setFanSpeed || !a->getFanSpeed) return false;
    for (unsigned int f = 0; f < numFans; f++) {
        bool fanOk = a->setFanSpeed(g->nvmlDevice, f, (unsigned int)fanPercent) == NVML_SUCCESS;
        if (fanOk && a->getFanSpeed) {
            unsigned int verify = 0;
            fanOk = a->getFanSpeed(g->nvmlDevice, f, &verify) == NVML_SUCCESS &&
                    verify == (unsigned int)fanPercent;
        }
        if (!fanOk) lb_log("fan: fixed write/readback failed for fan %u\n", f);
        ok &= fanOk;
    }
    lb_log("fan: set fixed %d%% across %u fan(s) ok=%d\n", fanPercent, numFans, ok ? 1 : 0);
    return ok;
}

static void nvml_query_ranges(LinuxGpuState* g) {
    NvmlApi* a = &g->nvml;
    int mn = 0, mx = 0;
    if (a->getGpcClkMinMaxVfOffset && a->getGpcClkMinMaxVfOffset(g->nvmlDevice, &mn, &mx) == NVML_SUCCESS) {
        g->gpuOffsetMinMHz = mn; g->gpuOffsetMaxMHz = mx;
        if (!g->curveOffsetRangeKnown) {
            g->curveOffsetMinKHz = mn * 1000; g->curveOffsetMaxKHz = mx * 1000;
            g->curveOffsetRangeKnown = true;
        }
    }
    if (a->getMemClkMinMaxVfOffset && a->getMemClkMinMaxVfOffset(g->nvmlDevice, &mn, &mx) == NVML_SUCCESS) {
        g->memOffsetMinMHz = mn; g->memOffsetMaxMHz = mx;
    }
    if (a->getPowerConstraints) {
        unsigned int pmin = 0, pmax = 0;
        if (a->getPowerConstraints(g->nvmlDevice, &pmin, &pmax) == NVML_SUCCESS) {
            g->powerLimitMinmW = (int)pmin; g->powerLimitMaxmW = (int)pmax;
        }
    }
    if (a->getPowerDefaultLimit) {
        unsigned int d = 0;
        if (a->getPowerDefaultLimit(g->nvmlDevice, &d) == NVML_SUCCESS) g->powerLimitDefaultmW = (int)d;
    }
}

// ===========================================================================
// Init / shutdown / refresh
// ===========================================================================

bool linux_backend_init(LinuxGpuState* g, const GpuAdapterInfo* target,
                        char* err, size_t errSize) {
    memset(g, 0, sizeof(*g));
    if (err && errSize) err[0] = 0;

    // NVML
    g->nvmlLib = pl_open_driver_library(PL_DRIVER_NVML);
    if (!g->nvmlLib) { gc_strlcpy(err, errSize, "libnvidia-ml.so.1 not found"); return false; }
    nvml_resolve(g);
    if (!g->nvml.init || g->nvml.init() != NVML_SUCCESS) {
        gc_strlcpy(err, errSize, "nvmlInit_v2 failed"); return false;
    }
    unsigned int count = 0;
    if (!g->nvml.getCount || g->nvml.getCount(&count) != NVML_SUCCESS || count == 0) {
        gc_strlcpy(err, errSize, "no NVML devices"); return false;
    }
    if (!g->nvml.getHandleByIndex) {
        gc_strlcpy(err, errSize, "nvmlDeviceGetHandleByIndex_v2 unavailable"); return false;
    }
    nvmlDeviceGetName_t getName = sym<nvmlDeviceGetName_t>(g->nvmlLib, "nvmlDeviceGetName");

    nvmlDevice_t nvmlDevices[MAX_GPU_ADAPTERS] = {};
    unsigned int enumeratedCount = count > MAX_GPU_ADAPTERS ? MAX_GPU_ADAPTERS : count;
    unsigned int adapterCount = 0;
    for (unsigned int i = 0; i < enumeratedCount; ++i) {
        if (g->nvml.getHandleByIndex(i, &nvmlDevices[i]) != NVML_SUCCESS) continue;
        GpuAdapterInfo* adapter = &g->adapters[adapterCount++];
        adapter->valid = true;
        adapter->nvmlIndex = i;
        adapter->nvapiIndex = MAX_GPU_ADAPTERS;
        if (getName) getName(nvmlDevices[i], adapter->name, sizeof(adapter->name));
        if (!adapter->name[0]) gc_strlcpy(adapter->name, sizeof(adapter->name), "NVIDIA GPU");
        if (g->nvml.getPciInfo) {
            nvmlPciInfo_t pci = {};
            if (g->nvml.getPciInfo(nvmlDevices[i], &pci) == NVML_SUCCESS) {
                adapter->valid = true;
                adapter->pciInfoValid = pci.pciDeviceId != 0;
                adapter->deviceId = pci.pciDeviceId;
                adapter->subSystemId = pci.pciSubSystemId;
                adapter->pciDomain = pci.domain;
                adapter->pciBus = pci.bus;
                adapter->pciDevice = pci.device;
                unsigned int domain = 0, bus = 0, device = 0, function = 0;
                if (sscanf(pci.busId, "%x:%x:%x.%x", &domain, &bus, &device, &function) == 4) {
                    adapter->pciDomain = domain;
                    adapter->pciBus = bus;
                    adapter->pciDevice = device;
                    adapter->pciFunction = function;
                }
            }
        }
    }
    g->adapterCount = adapterCount;

    int requestedIndex = -1;
    if (target && target->valid)
        requestedIndex = linux_resolve_gpu_identity(target, g->adapters, adapterCount);
    else if (adapterCount == 1)
        requestedIndex = 0;
    if (requestedIndex >= 0) {
        g->selectedAdapterIndex = (unsigned int)requestedIndex;
        g->selectedGpu = g->adapters[requestedIndex];
        g->nvmlIndex = g->selectedGpu.nvmlIndex;
        g->nvmlDevice = nvmlDevices[g->nvmlIndex];
        g->writeIdentityResolved = adapterCount == 1 ||
            (target && linux_gpu_identity_matches(target, &g->selectedGpu));
    } else if (requestedIndex == -2) {
        gc_strlcpy(err, errSize, "selected GPU PCI identity is ambiguous");
    } else if (adapterCount > 1) {
        gc_strlcpy(err, errSize, "multiple GPUs detected; select one by PCI BDF with --gpu");
    }

    // NvAPI
    g->nvapiLib = pl_open_driver_library(PL_DRIVER_NVAPI);
    if (g->nvapiLib) {
        g->nvapiQi = (nvapi_qi_t)pl_lib_sym(g->nvapiLib, "nvapi_QueryInterface");
        if (g->nvapiQi) {
            auto init = (nvapi_init_t)g->nvapiQi(NVAPI_INIT_ID);
            if (init && nvapi_ok(init())) {
                auto enumGpus = (nvapi_enum_t)g->nvapiQi(NVAPI_ENUM_GPU_ID);
                auto getArch = (nvapi_arch_t)g->nvapiQi(NVAPI_GPU_GET_ARCH_INFO_ID);
                GPU_HANDLE handles[64] = {};
                int n = 0;
                if (enumGpus && nvapi_ok(enumGpus(handles, &n)) && n > 0) {
                    typedef int (*get_pci_t)(GPU_HANDLE, unsigned int*, unsigned int*, unsigned int*, unsigned int*);
                    typedef int (*bus_id_t)(GPU_HANDLE, unsigned int*);
                    auto getPci = (get_pci_t)g->nvapiQi(NVAPI_GPU_GET_PCI_IDENTIFIERS_ID);
                    auto getBus = (bus_id_t)g->nvapiQi(NVAPI_GPU_GET_BUS_ID_ID);
                    auto getSlot = (bus_id_t)g->nvapiQi(NVAPI_GPU_GET_BUS_SLOT_ID_ID);
                    bool nvapiAssigned[MAX_GPU_ADAPTERS] = {};
                    for (int ni = 0; ni < n && ni < (int)MAX_GPU_ADAPTERS; ++ni) {
                        unsigned int bus = 0, slot = 0;
                        int match = -1;
                        if (getBus && getSlot && nvapi_ok(getBus(handles[ni], &bus)) &&
                            nvapi_ok(getSlot(handles[ni], &slot))) {
                            for (unsigned int ai = 0; ai < adapterCount; ++ai) {
                                if (g->adapters[ai].pciBus == bus && g->adapters[ai].pciDevice == slot) {
                                    if (match >= 0) { match = -2; break; }
                                    match = (int)ai;
                                }
                            }
                        } else if (adapterCount == 1 && n == 1) {
                            match = 0;
                        }
                        if (match < 0) continue;
                        GpuAdapterInfo* adapter = &g->adapters[match];
                        bool pciVerified = false;
                        unsigned int device = 0, subsystem = 0, revision = 0, extended = 0;
                        if (getPci && nvapi_ok(getPci(handles[ni], &device, &subsystem, &revision, &extended))) {
                            if (adapter->pciInfoValid &&
                                (!linux_gpu_device_id_matches(device, adapter->deviceId) ||
                                 (subsystem && adapter->subSystemId && subsystem != adapter->subSystemId))) {
                                adapter->vfReadSupported = false;
                                adapter->vfWriteSupported = false;
                                continue;
                            }
                            pciVerified = true;
                            adapter->pciInfoValid = true;
                            adapter->deviceId = device;
                            adapter->subSystemId = subsystem;
                            adapter->pciRevisionId = revision;
                            adapter->extDeviceId = extended;
                        }
                        if (adapterCount > 1 && !pciVerified) continue;
                        if (nvapiAssigned[match]) {
                            adapter->nvapiIndex = MAX_GPU_ADAPTERS;
                            adapter->vfReadSupported = false;
                            adapter->vfWriteSupported = false;
                            continue;
                        }
                        nvapiAssigned[match] = true;
                        adapter->nvapiIndex = (unsigned int)ni;
                        unsigned int architecture = 0;
                        if (getArch) {
                            nvapiGpuArchInfo_t ai{};
                            ai.version = NVAPI_GPU_ARCH_INFO_VER2;
                            if (nvapi_ok(getArch(handles[ni], &ai))) architecture = ai.architecture;
                        }
                        GpuFamily family = GPU_FAMILY_UNKNOWN;
                        const VfBackendSpec* backend = vf_backend_for_architecture(architecture, &family);
                        adapter->family = family;
                        adapter->vfReadSupported = backend && backend->readSupported;
                        adapter->vfWriteSupported = backend && backend->writeSupported;
                        adapter->vfBestGuess = backend && backend->bestGuessOnly;
                    }

                    int selected = -1;
                    if (target && target->valid)
                        selected = linux_resolve_gpu_identity(target, g->adapters, adapterCount);
                    else if (adapterCount == 1)
                        selected = 0;
                    if (selected >= 0) {
                        GpuAdapterInfo* adapter = &g->adapters[selected];
                        g->selectedAdapterIndex = (unsigned int)selected;
                        g->selectedGpu = *adapter;
                        g->nvmlIndex = adapter->nvmlIndex;
                        g->nvapiIndex = adapter->nvapiIndex;
                        g->nvmlDevice = nvmlDevices[g->nvmlIndex];
                        if (g->nvapiIndex < (unsigned int)n) g->gpuHandle = handles[g->nvapiIndex];
                        bool stableTarget = adapterCount == 1 ||
                            (target && linux_gpu_identity_matches(target, adapter));
                        g->writeIdentityResolved = stableTarget &&
                            (adapterCount == 1 || g->gpuHandle != nullptr);
                        if (stableTarget && adapterCount > 1 && !g->gpuHandle)
                            gc_strlcpy(err, errSize, "selected GPU does not match uniquely across NVML and NvAPI");
                        if (getArch && g->gpuHandle) {
                            nvapiGpuArchInfo_t ai{};
                            ai.version = NVAPI_GPU_ARCH_INFO_VER2;
                            if (nvapi_ok(getArch(g->gpuHandle, &ai))) g->architecture = ai.architecture;
                        }
                        g->backend = vf_backend_for_architecture(g->architecture, &g->family);
                    } else if (selected == -2) {
                        gc_strlcpy(err, errSize, "selected GPU PCI identity is ambiguous");
                    } else if (adapterCount > 1) {
                        gc_strlcpy(err, errSize, "multiple GPUs detected; select one by PCI BDF with --gpu");
                    }
                }
            }
        }
    }

    if (!g->nvmlDevice && adapterCount > 0) {
        g->selectedAdapterIndex = 0;
        g->selectedGpu = g->adapters[0];
        g->nvmlIndex = g->adapters[0].nvmlIndex;
        g->nvmlDevice = nvmlDevices[g->nvmlIndex];
        g->writeIdentityResolved = adapterCount == 1;
    }
    if (adapterCount > 1 && !g->gpuHandle) {
        g->writeIdentityResolved = false;
        if (err && !err[0])
            gc_strlcpy(err, errSize, "multi-GPU write target is not proven across NVML and NvAPI");
    }
    g->nvmlReady = g->nvmlDevice != nullptr;
    if (g->nvmlReady) {
        gc_strlcpy(g->gpuName, sizeof(g->gpuName), g->selectedGpu.name);
        nvml_query_ranges(g);
    }
    if (g->gpuHandle && g->backend) {
        nvapi_read_curve(g);
        nvapi_read_offsets(g);
    }
    if (!g->backend) {
        lb_log("linux_backend_init: NvAPI VF surface unavailable; NVML-only mode\n");
    }
    return g->nvmlReady;
}

bool linux_backend_select_target(LinuxGpuState* g, const GpuAdapterInfo* target,
                                 char* err, size_t errSize) {
    if (!g || !target || !target->valid) {
        gc_strlcpy(err, errSize, "a stable GPU target is required");
        return false;
    }
    if (linux_gpu_identity_matches(target, &g->selectedGpu) && g->writeIdentityResolved)
        return true;
    LinuxGpuState replacement = {};
    if (!linux_backend_init(&replacement, target, err, errSize) ||
        !replacement.writeIdentityResolved) {
        linux_backend_shutdown(&replacement);
        if (err && !err[0]) gc_strlcpy(err, errSize, "selected GPU could not be resolved uniquely");
        return false;
    }
    linux_backend_shutdown(g);
    *g = replacement;
    return true;
}

void linux_backend_shutdown(LinuxGpuState* g) {
    if (g->nvml.shutdown) g->nvml.shutdown();
    if (g->nvmlLib) pl_lib_close(g->nvmlLib);
    if (g->nvapiLib) pl_lib_close(g->nvapiLib);
    memset(g, 0, sizeof(*g));
}

bool linux_backend_refresh(LinuxGpuState* g) {
    nvml_query_ranges(g);
    if (g->backend) { nvapi_read_curve(g); nvapi_read_offsets(g); }
    return true;
}

// ===========================================================================
// Apply / reset  (ports apply_desired_settings_service ordering)
// ===========================================================================

static bool desired_has_curve_write(const DesiredSettings* d) {
    if (!d) return false;
    for (int i = 0; i < VF_NUM_POINTS; ++i)
        if (d->hasCurvePoint[i]) return true;
    return d->hasLock && d->lockMode == LOCK_MODE_FLATTEN;
}

bool linux_backend_capture_snapshot(LinuxGpuState* g, LinuxHardwareSnapshot* snapshot,
                                    char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!g || !snapshot || !g->nvmlReady) {
        gc_strlcpy(err, errSize, "GPU backend is not ready for snapshot");
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (g->nvml.getGpcClkVfOffset &&
        g->nvml.getGpcClkVfOffset(g->nvmlDevice, &snapshot->gpuOffsetMHz) == NVML_SUCCESS)
        snapshot->gpuOffsetValid = true;
    if (g->nvml.getMemClkVfOffset &&
        g->nvml.getMemClkVfOffset(g->nvmlDevice, &snapshot->memOffsetMHz) == NVML_SUCCESS)
        snapshot->memOffsetValid = true;
    if (g->nvml.getPowerLimit &&
        g->nvml.getPowerLimit(g->nvmlDevice, &snapshot->powerLimitmW) == NVML_SUCCESS)
        snapshot->powerValid = true;
    if (g->backend && g->backend->writeSupported && g->numPopulated > 0) {
        snapshot->curveValid = true;
        for (int i = 0; i < VF_NUM_POINTS; ++i) {
            snapshot->curveOffsets[i] = g->freqOffsets[i];
            snapshot->curveMask[i] = g->curve[i].freq_kHz != 0;
        }
    }
    unsigned int fans = 0;
    if (g->nvml.getNumFans && g->nvml.getNumFans(g->nvmlDevice, &fans) == NVML_SUCCESS) {
        if (fans > MAX_GPU_FANS) fans = MAX_GPU_FANS;
        snapshot->fanCount = fans;
        snapshot->fanValid = fans > 0;
        for (unsigned int i = 0; i < fans; ++i) {
            if (!g->nvml.getFanSpeed ||
                g->nvml.getFanSpeed(g->nvmlDevice, i, &snapshot->fanPercent[i]) != NVML_SUCCESS)
                snapshot->fanValid = false;
            if (!g->nvml.getFanControlPolicy ||
                g->nvml.getFanControlPolicy(g->nvmlDevice, i, &snapshot->fanPolicy[i]) != NVML_SUCCESS)
                snapshot->fanValid = false;
        }
    }
    snapshot->valid = snapshot->gpuOffsetValid || snapshot->memOffsetValid ||
                      snapshot->powerValid || snapshot->curveValid || snapshot->fanValid;
    if (!snapshot->valid) gc_strlcpy(err, errSize, "no rollback-capable GPU state could be captured");
    return snapshot->valid;
}

bool linux_backend_restore_snapshot(LinuxGpuState* g, const LinuxHardwareSnapshot* snapshot,
                                    unsigned int phaseMask, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!g || !snapshot || !snapshot->valid) {
        gc_strlcpy(err, errSize, "rollback snapshot is invalid");
        return false;
    }
    bool ok = true;
    bool baseline = (phaseMask & LINUX_MUTATION_RESET_BASELINE) != 0;
    if ((baseline || (phaseMask & LINUX_MUTATION_GPU_OFFSET)) && snapshot->gpuOffsetValid)
        ok &= nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, snapshot->gpuOffsetMHz);
    if ((baseline || (phaseMask & LINUX_MUTATION_MEM_OFFSET)) && snapshot->memOffsetValid)
        ok &= nvml_set_clock_offset(g, NVML_CLOCK_MEM, snapshot->memOffsetMHz);
    if ((phaseMask & LINUX_MUTATION_POWER) && snapshot->powerValid && g->nvml.setPowerLimit) {
        bool powerOk = g->nvml.setPowerLimit(g->nvmlDevice, snapshot->powerLimitmW) == NVML_SUCCESS;
        if (powerOk && g->nvml.getPowerLimit) {
            unsigned int verify = 0;
            powerOk = g->nvml.getPowerLimit(g->nvmlDevice, &verify) == NVML_SUCCESS &&
                      verify == snapshot->powerLimitmW;
        }
        ok &= powerOk;
    }
    if ((phaseMask & LINUX_MUTATION_CURVE) && snapshot->curveValid)
        ok &= apply_curve_offsets_verified(g, snapshot->curveOffsets, snapshot->curveMask, 25);
    if ((phaseMask & LINUX_MUTATION_FAN) && snapshot->fanValid) {
        for (unsigned int i = 0; i < snapshot->fanCount; ++i) {
            bool fanOk = false;
            if (snapshot->fanPolicy[i] == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) {
                if (g->nvml.setDefaultFanSpeed)
                    fanOk = g->nvml.setDefaultFanSpeed(g->nvmlDevice, i) == NVML_SUCCESS;
                else if (g->nvml.setFanControlPolicy)
                    fanOk = g->nvml.setFanControlPolicy(g->nvmlDevice, i,
                        NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) == NVML_SUCCESS;
            } else if (g->nvml.setFanSpeed) {
                fanOk = g->nvml.setFanSpeed(g->nvmlDevice, i, snapshot->fanPercent[i]) == NVML_SUCCESS;
            }
            if (fanOk && snapshot->fanPolicy[i] == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) {
                unsigned int verifyPolicy = 0;
                fanOk = g->nvml.getFanControlPolicy &&
                        g->nvml.getFanControlPolicy(g->nvmlDevice, i, &verifyPolicy) == NVML_SUCCESS &&
                        verifyPolicy == NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW;
            } else if (fanOk) {
                unsigned int verifyPercent = 0;
                fanOk = g->nvml.getFanSpeed &&
                        g->nvml.getFanSpeed(g->nvmlDevice, i, &verifyPercent) == NVML_SUCCESS &&
                        verifyPercent == snapshot->fanPercent[i];
            }
            if (!fanOk) lb_log("fan: rollback verification failed for fan %u\n", i);
            ok &= fanOk;
        }
    }
    if (baseline || (phaseMask & LINUX_MUTATION_LOCK)) {
        // NVML exposes no getter for the configured locked-clock range.  Release
        // a lock written by this transaction, but never claim that the unknown
        // pre-transaction lock policy was restored exactly.
        if (g->nvml.resetGpuLockedClocks)
            g->nvml.resetGpuLockedClocks(g->nvmlDevice);
        ok = false;
    }
    if (!ok) gc_strlcpy(err, errSize, "one or more GPU rollback phases failed");
    linux_backend_refresh(g);
    return ok;
}

static bool linux_backend_preflight(LinuxGpuState* g, const DesiredSettings* d,
                                    const LinuxHardwareSnapshot* snapshot,
                                    char* err, size_t errSize) {
    if (!g || !d || !g->nvmlReady || !g->writeIdentityResolved) {
        gc_strlcpy(err, errSize, "GPU write target is unavailable or not uniquely resolved");
        return false;
    }
    bool hardLock = d->hasLock && d->lockMode == LOCK_MODE_HARD && d->lockMHz > 0;
    if (d->resetOcBeforeApply && !g->nvml.resetGpuLockedClocks) {
        gc_strlcpy(err, errSize, "OC baseline reset cannot release locked clocks safely"); return false;
    }
    if ((d->hasGpuOffset || d->resetOcBeforeApply) &&
        (!snapshot->gpuOffsetValid || (!g->nvml.setGpcClkVfOffset && !g->nvml.setClockOffsets))) {
        gc_strlcpy(err, errSize, "GPU offset cannot be snapshotted and written safely"); return false;
    }
    if ((d->hasMemOffset || d->resetOcBeforeApply) &&
        (!snapshot->memOffsetValid || (!g->nvml.setMemClkVfOffset && !g->nvml.setClockOffsets))) {
        gc_strlcpy(err, errSize, "memory offset cannot be snapshotted and written safely"); return false;
    }
    if (d->hasPowerLimit && (!snapshot->powerValid || !g->nvml.setPowerLimit)) {
        gc_strlcpy(err, errSize, "power limit cannot be snapshotted and written safely"); return false;
    }
    if (desired_has_curve_write(d) && !hardLock) {
        char why[160] = {};
        if (!snapshot->curveValid || !g->backend || !g->backend->writeSupported ||
            !linux_backend_curve_plausible(g, why, sizeof(why))) {
            gc_snprintf(err, errSize, "VF curve write preflight failed: %s", why[0] ? why : "unsupported");
            return false;
        }
    }
    if (hardLock && !g->nvml.setGpuLockedClocks) {
        gc_strlcpy(err, errSize, "hard clock locking is unavailable"); return false;
    }
    if (d->hasFan && !snapshot->fanValid) {
        gc_strlcpy(err, errSize, "fan state cannot be snapshotted safely"); return false;
    }
    return true;
}

// Build per-point absolute-target curve offsets from a DesiredSettings request.
// offset_kHz = targetMHz*1000 - stockFreq_kHz  (mirrors the Windows correction
// loop's `targetMHz*1000 - liveFreq`).  Handles FLATTEN lock by applying a
// uniform floor offset (range minimum) to tail points beyond the lock index
// (the Build-109 uniform-tail-floor approach).
static int build_curve_targets(LinuxGpuState* g, const DesiredSettings* d,
                               int* targetOffsets, bool* pointMask) {
    int mn = 0, mx = 0;
    curve_offset_range_khz(g, &mn, &mx);
    int n = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        targetOffsets[i] = 0;
        pointMask[i] = false;
        if (g->curve[i].freq_kHz == 0) continue;
        if (d->hasCurvePoint[i] && d->curvePointMHz[i] > 0) {
            targetOffsets[i] = (int)(d->curvePointMHz[i] * 1000u) - (int)g->curve[i].freq_kHz;
            pointMask[i] = true;
            n++;
        }
    }
    // FLATTEN lock: hold the tail beyond lockCi flat by flooring it hard.
    if (d->hasLock && d->lockMode == LOCK_MODE_FLATTEN && d->lockCi >= 0 && d->lockCi < VF_NUM_POINTS) {
        for (int i = d->lockCi + 1; i < VF_NUM_POINTS; i++) {
            if (g->curve[i].freq_kHz == 0) continue;
            targetOffsets[i] = mn;  // uniform floor (range minimum)
            pointMask[i] = true;
            n++;
        }
        // Lock point itself: hold at requested lock frequency.
        if (d->lockMHz > 0 && g->curve[d->lockCi].freq_kHz > 0) {
            targetOffsets[d->lockCi] = (int)(d->lockMHz * 1000u) - (int)g->curve[d->lockCi].freq_kHz;
            pointMask[d->lockCi] = true;
        }
    }
    return n;
}

struct LinuxApplyTransactionContext {
    LinuxGpuState* gpu;
    const DesiredSettings* desired;
    const LinuxHardwareSnapshot* snapshot;
    int curveTargets[VF_NUM_POINTS];
    bool curveMask[VF_NUM_POINTS];
};

static bool linux_apply_transaction_step(void* opaque, unsigned int phase) {
    LinuxApplyTransactionContext* context = (LinuxApplyTransactionContext*)opaque;
    LinuxGpuState* g = context->gpu;
    const DesiredSettings* d = context->desired;
    switch (phase) {
        case LINUX_MUTATION_RESET_BASELINE:
            return nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, 0) &&
                   nvml_set_clock_offset(g, NVML_CLOCK_MEM, 0) &&
                   g->nvml.resetGpuLockedClocks &&
                   g->nvml.resetGpuLockedClocks(g->nvmlDevice) == NVML_SUCCESS;
        case LINUX_MUTATION_GPU_OFFSET:
            return nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, d->gpuOffsetMHz);
        case LINUX_MUTATION_MEM_OFFSET:
            return nvml_set_clock_offset(g, NVML_CLOCK_MEM, d->memOffsetMHz);
        case LINUX_MUTATION_POWER:
            return nvml_set_power_limit_pct(g, d->powerLimitPct);
        case LINUX_MUTATION_CURVE:
            return apply_curve_offsets_verified(g, context->curveTargets,
                                                context->curveMask, 25);
        case LINUX_MUTATION_LOCK:
            if (d->lockMode == LOCK_MODE_HARD && d->lockMHz > 0)
                return g->nvml.setGpuLockedClocks &&
                       g->nvml.setGpuLockedClocks(g->nvmlDevice, d->lockMHz,
                                                  d->lockMHz) == NVML_SUCCESS;
            return g->nvml.resetGpuLockedClocks &&
                   g->nvml.resetGpuLockedClocks(g->nvmlDevice) == NVML_SUCCESS;
        case LINUX_MUTATION_FAN:
            return nvml_set_fan(g, d->fanMode, d->fanAuto, d->fanPercent);
        default:
            return false;
    }
}

static bool linux_apply_transaction_rollback(void* opaque, unsigned int phases) {
    LinuxApplyTransactionContext* context = (LinuxApplyTransactionContext*)opaque;
    char rollback[256] = {};
    return linux_backend_restore_snapshot(context->gpu, context->snapshot, phases,
                                          rollback, sizeof(rollback));
}

static int phase_count(unsigned int phases) {
    int count = 0;
    while (phases) { count += (int)(phases & 1u); phases >>= 1; }
    return count;
}

LinuxMutationResult linux_backend_apply(LinuxGpuState* g, const DesiredSettings* d,
                                        char* result, size_t resultSize) {
    LinuxHardwareSnapshot snapshot = {};
    char preflight[256] = {};
    if (!linux_backend_capture_snapshot(g, &snapshot, preflight, sizeof(preflight)) ||
        !linux_backend_preflight(g, d, &snapshot, preflight, sizeof(preflight))) {
        LinuxMutationResult mutation = {};
        if (result) gc_strlcpy(result, resultSize, preflight[0] ? preflight : "Apply preflight failed");
        return mutation;
    }
    bool hardLock = d->hasLock && d->lockMode == LOCK_MODE_HARD && d->lockMHz > 0;
    LinuxApplyTransactionContext context = {g, d, &snapshot, {}, {}};
    unsigned int requested = 0;
    if (d->resetOcBeforeApply) requested |= LINUX_MUTATION_RESET_BASELINE;
    if (d->hasGpuOffset && !hardLock) requested |= LINUX_MUTATION_GPU_OFFSET;
    if (d->hasMemOffset) requested |= LINUX_MUTATION_MEM_OFFSET;
    if (d->hasPowerLimit) requested |= LINUX_MUTATION_POWER;
    if (desired_has_curve_write(d) && !hardLock &&
        build_curve_targets(g, d, context.curveTargets, context.curveMask) > 0)
        requested |= LINUX_MUTATION_CURVE;
    if (d->hasLock) requested |= LINUX_MUTATION_LOCK;
    if (d->hasFan) requested |= LINUX_MUTATION_FAN;
    LinuxMutationResult mutation = linux_execute_transaction(
        requested, linux_apply_transaction_step, linux_apply_transaction_rollback, &context);
    char msg[512] = {};
    gc_snprintf(msg, sizeof(msg), "%s: %d phase(s), %d failed%s%s",
                mutation.success ? "Applied" : "Apply failed",
                phase_count(mutation.attemptedPhases), phase_count(mutation.failedPhases),
                (!mutation.success && mutation.rollbackSucceeded) ? " (rolled back)" :
                (!mutation.success && mutation.rollbackAttempted) ? " (rollback uncertain)" : "",
                hardLock ? " (hard clock pin)" : "");
    if (result) gc_strlcpy(result, resultSize, msg);
    lb_log("apply: %s\n", msg);
    linux_backend_refresh(g);
    return mutation;
}

struct LinuxResetTransactionContext {
    LinuxGpuState* gpu;
    const LinuxHardwareSnapshot* snapshot;
};

static bool linux_reset_transaction_step(void* opaque, unsigned int phase) {
    LinuxResetTransactionContext* context = (LinuxResetTransactionContext*)opaque;
    LinuxGpuState* g = context->gpu;
    switch (phase) {
        case LINUX_MUTATION_LOCK:
            return g->nvml.resetGpuLockedClocks(g->nvmlDevice) == NVML_SUCCESS;
        case LINUX_MUTATION_GPU_OFFSET:
            return nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, 0);
        case LINUX_MUTATION_MEM_OFFSET:
            return nvml_set_clock_offset(g, NVML_CLOCK_MEM, 0);
        case LINUX_MUTATION_POWER: {
            bool ok = g->nvml.setPowerLimit(g->nvmlDevice,
                (unsigned int)g->powerLimitDefaultmW) == NVML_SUCCESS;
            unsigned int verify = 0;
            return ok && g->nvml.getPowerLimit &&
                   g->nvml.getPowerLimit(g->nvmlDevice, &verify) == NVML_SUCCESS &&
                   verify == (unsigned int)g->powerLimitDefaultmW;
        }
        case LINUX_MUTATION_CURVE: {
            int targets[VF_NUM_POINTS] = {};
            bool mask[VF_NUM_POINTS];
            for (int i = 0; i < VF_NUM_POINTS; ++i)
                mask[i] = g->curve[i].freq_kHz != 0;
            return apply_curve_offsets_verified(g, targets, mask, 25);
        }
        case LINUX_MUTATION_FAN:
            return nvml_set_fan(g, FAN_MODE_AUTO, true, 0);
        default:
            return false;
    }
}

static bool linux_reset_transaction_rollback(void* opaque, unsigned int phases) {
    LinuxResetTransactionContext* context = (LinuxResetTransactionContext*)opaque;
    char rollback[256] = {};
    return linux_backend_restore_snapshot(context->gpu, context->snapshot, phases,
                                          rollback, sizeof(rollback));
}

LinuxMutationResult linux_backend_reset(LinuxGpuState* g, char* result, size_t resultSize) {
    LinuxMutationResult mutation = {};
    LinuxHardwareSnapshot snapshot = {};
    char detail[256] = {};
    if (!linux_backend_capture_snapshot(g, &snapshot, detail, sizeof(detail)) ||
        !g->writeIdentityResolved) {
        if (result) gc_strlcpy(result, resultSize, detail[0] ? detail : "Reset preflight failed");
        return mutation;
    }
    if (!snapshot.gpuOffsetValid || !snapshot.memOffsetValid || !snapshot.powerValid ||
        !snapshot.curveValid || !snapshot.fanValid || !g->nvml.resetGpuLockedClocks ||
        (!g->nvml.setGpcClkVfOffset && !g->nvml.setClockOffsets) ||
        (!g->nvml.setMemClkVfOffset && !g->nvml.setClockOffsets) ||
        !g->nvml.setPowerLimit || g->powerLimitDefaultmW <= 0 ||
        !g->backend || !g->backend->writeSupported) {
        gc_strlcpy(detail, sizeof(detail),
                   "Reset preflight failed: every mutable domain must support snapshot and restore");
        if (result) gc_strlcpy(result, resultSize, detail);
        return mutation;
    }
    LinuxResetTransactionContext context = {g, &snapshot};
    const unsigned int requested = LINUX_MUTATION_LOCK | LINUX_MUTATION_GPU_OFFSET |
        LINUX_MUTATION_MEM_OFFSET | LINUX_MUTATION_POWER | LINUX_MUTATION_CURVE |
        LINUX_MUTATION_FAN;
    mutation = linux_execute_transaction(requested, linux_reset_transaction_step,
                                         linux_reset_transaction_rollback, &context);
    if (result) gc_strlcpy(result, resultSize, mutation.success ? "Reset to defaults" :
        mutation.rollbackSucceeded ? "Reset failed and was rolled back" : "Reset failed; rollback uncertain");
    linux_backend_refresh(g);
    return mutation;
}

bool linux_backend_set_curve_fan_percent(LinuxGpuState* g, unsigned int percent) {
    if (!g || percent > 100) return false;
    return nvml_set_fan(g, FAN_MODE_FIXED, false, (int)percent);
}

bool linux_backend_set_fan_auto(LinuxGpuState* g) {
    return g && nvml_set_fan(g, FAN_MODE_AUTO, true, 0);
}

// ===========================================================================
// Self-validation (arm64 / new-driver pre-flight; read-only)
// ===========================================================================

bool linux_backend_curve_plausible(const LinuxGpuState* g, char* why, size_t whySize) {
    if (why && whySize) why[0] = 0;
    if (!g->backend || !g->backend->readSupported) {
        if (why) gc_strlcpy(why, whySize, "no NvAPI VF backend");
        return false;
    }
    // Sane VF-curve envelope: a real GeForce curve sits well inside these.
    const int MIN_F = 100, MAX_F = 6000;   // MHz
    const int MIN_V = 400, MAX_V = 1600;   // mV
    int populated = 0;
    int prevFreqMHz = 0;
    for (int i = 0; i < VF_NUM_POINTS; i++) {
        if (g->curve[i].freq_kHz == 0) continue;
        populated++;
        int fMHz = (int)(g->curve[i].freq_kHz / 1000u);
        int vMV = (int)(g->curve[i].volt_uV / 1000u);
        if (fMHz < MIN_F || fMHz > MAX_F || vMV < MIN_V || vMV > MAX_V) {
            if (why) gc_snprintf(why, whySize, "point %d out of range (%d MHz @ %d mV)", i, fMHz, vMV);
            return false;
        }
        // VF points are ordered by voltage; frequency should be non-decreasing
        // (allow small noise).  A scrambled order implies a layout mismatch.
        if (fMHz + 50 < prevFreqMHz) {
            if (why) gc_strlcpy(why, whySize, "curve frequency not monotonic (struct layout mismatch?)");
            return false;
        }
        prevFreqMHz = fMHz;
    }
    if (populated < 8) {
        if (why) gc_snprintf(why, whySize, "only %d populated points", populated);
        return false;
    }
    return true;
}

bool linux_backend_self_test(LinuxGpuState* g, FILE* out) {
    if (!out) out = stdout;
    fprintf(out, "=== Green Curve driver/arch self-test (read-only, no GPU changes) ===\n");
    fprintf(out, "GPU: %s  family=%d\n", g->gpuName[0] ? g->gpuName : "?", (int)g->family);

    bool nvmlOk = g->nvmlReady;
    fprintf(out, "NVML ready             : %s\n", nvmlOk ? "yes" : "NO");
    if (g->curveOffsetRangeKnown)
        fprintf(out, "NVML GPC offset range  : %d .. %d MHz\n", g->gpuOffsetMinMHz, g->gpuOffsetMaxMHz);

    if (!g->backend) {
        fprintf(out, "NvAPI VF backend       : NOT available (NVML-only mode)\n");
        fprintf(out, "\nVerdict: NVML-ONLY — clock offsets / power / fan / locked clocks only; "
                     "VF-curve editing needs libnvidia-api.so.1 on this driver.\n");
        return false;
    }
    fprintf(out, "NvAPI VF backend       : %s%s\n", g->backend->name,
            g->backend->bestGuessOnly ? " (best-effort unrecognized family)" : "");
    fprintf(out, "expected struct sizes  : pstates20=%u arch=%u (compile-time pinned)\n",
            (unsigned)sizeof(nvapiPerfPstates20Info_t), (unsigned)sizeof(nvapiGpuArchInfo_t));

    bool infoOk = nvapi_get_vf_info(g);
    fprintf(out, "NvAPI getInfo          : %s (numClocks=%u)\n", infoOk ? "ok" : "FAILED", g->vfNumClocks);

    bool curveOk = nvapi_read_curve(g);
    char why[160] = {};
    bool plausible = linux_backend_curve_plausible(g, why, sizeof(why));
    fprintf(out, "NvAPI getStatus (curve): %s — %d points; plausible=%s%s\n",
            curveOk ? "ok" : "FAILED", g->numPopulated,
            plausible ? "yes" : "NO", plausible ? "" : (why[0] ? why : ""));

    // Read the CONTROL table — the SAME struct version writes use — without
    // writing anything.  Success here means the write struct version is accepted
    // by this driver, so the apply (write) path should work.
    bool ctrlOk = false;
    if (g->backend->controlBufferSize <= LB_CONTROL_BUF_SIZE) {
        unsigned char* cbuf = (unsigned char*)calloc(1, LB_CONTROL_BUF_SIZE);
        if (cbuf) {
            ctrlOk = nvapi_read_control_table(g, cbuf, LB_CONTROL_BUF_SIZE);
            free(cbuf);
        }
    }
    fprintf(out, "NvAPI getControl       : %s\n",
            ctrlOk ? "ok — write struct version accepted by the driver"
                   : "FAILED — write struct version rejected (INCOMPATIBLE_STRUCT_VERSION?)");

    bool pass = nvmlOk && infoOk && curveOk && plausible && ctrlOk;
    fprintf(out, "\nVerdict: %s\n", pass
            ? "PASS — read + write-struct paths validated; VF apply should work on this driver/arch."
            : "INCOMPLETE — see failures above. VF writes are auto-gated off until the curve reads plausibly; "
              "NVML features still work.");
    return pass;
}
