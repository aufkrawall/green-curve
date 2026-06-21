// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_backend.cpp — native Linux GPU control backend (see linux_backend.h).
// Faithfully ports the Windows NvAPI/NVML read+apply logic; each function names
// its Windows counterpart in a comment so the two stay in sync.

#include "linux_backend.h"
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
        if (a->setClockOffsets(g->nvmlDevice, &info) == NVML_SUCCESS) return true;
    }
    if (domain == NVML_CLOCK_GRAPHICS && a->setGpcClkVfOffset)
        return a->setGpcClkVfOffset(g->nvmlDevice, offsetMHz) == NVML_SUCCESS;
    if (domain == NVML_CLOCK_MEM && a->setMemClkVfOffset)
        return a->setMemClkVfOffset(g->nvmlDevice, offsetMHz) == NVML_SUCCESS;
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
    return r == NVML_SUCCESS;
}

static bool nvml_set_fan(LinuxGpuState* g, int fanMode, bool fanAuto, int fanPercent) {
    NvmlApi* a = &g->nvml;
    unsigned int numFans = 0;
    if (a->getNumFans) a->getNumFans(g->nvmlDevice, &numFans);
    if (numFans == 0) numFans = 1;
    bool ok = true;
    if (fanAuto || fanMode == FAN_MODE_AUTO) {
        for (unsigned int f = 0; f < numFans; f++) {
            if (a->setDefaultFanSpeed) ok &= (a->setDefaultFanSpeed(g->nvmlDevice, f) == NVML_SUCCESS);
            else if (a->setFanControlPolicy) ok &= (a->setFanControlPolicy(g->nvmlDevice, f, NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW) == NVML_SUCCESS);
        }
        return ok;
    }
    // Fixed (and the initial set for curve mode; the daemon reasserts curve).
    if (fanPercent < 0) fanPercent = 0;
    if (fanPercent > 100) fanPercent = 100;
    if (!a->setFanSpeed) return false;
    for (unsigned int f = 0; f < numFans; f++)
        ok &= (a->setFanSpeed(g->nvmlDevice, f, (unsigned int)fanPercent) == NVML_SUCCESS);
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

bool linux_backend_init(LinuxGpuState* g, unsigned int index, char* err, size_t errSize) {
    memset(g, 0, sizeof(*g));
    g->nvapiIndex = index;
    g->nvmlIndex = index;

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
    if (index >= count) index = 0;
    if (!g->nvml.getHandleByIndex || g->nvml.getHandleByIndex(index, &g->nvmlDevice) != NVML_SUCCESS) {
        gc_strlcpy(err, errSize, "nvmlDeviceGetHandleByIndex failed"); return false;
    }
    g->nvmlReady = true;
    nvmlDeviceGetName_t getName = sym<nvmlDeviceGetName_t>(g->nvmlLib, "nvmlDeviceGetName");
    if (getName) getName(g->nvmlDevice, g->gpuName, sizeof(g->gpuName));
    nvml_query_ranges(g);

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
                    if (index >= (unsigned int)n) index = 0;
                    g->gpuHandle = handles[index];
                    if (getArch) {
                        nvapiGpuArchInfo_t ai{};
                        ai.version = NVAPI_GPU_ARCH_INFO_VER2;
                        if (nvapi_ok(getArch(g->gpuHandle, &ai))) g->architecture = ai.architecture;
                    }
                    g->backend = vf_backend_for_architecture(g->architecture, &g->family);
                    nvapi_read_curve(g);
                    nvapi_read_offsets(g);
                }
            }
        }
    }
    if (!g->backend) {
        lb_log("linux_backend_init: NvAPI VF surface unavailable; NVML-only mode\n");
    }
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

bool linux_backend_apply(LinuxGpuState* g, const DesiredSettings* d,
                         char* result, size_t resultSize) {
    bool allOk = true;
    char msg[512] = {};
    int phases = 0, failed = 0;

    // Phase 0: optional reset of the OC baseline before applying.
    if (d->resetOcBeforeApply) {
        nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, 0);
        nvml_set_clock_offset(g, NVML_CLOCK_MEM, 0);
        if (g->nvml.resetGpuLockedClocks) g->nvml.resetGpuLockedClocks(g->nvmlDevice);
    }

    // Phase 1: GPU clock offset (HARD lock uses NVML locked clocks instead).
    bool hardLock = d->hasLock && d->lockMode == LOCK_MODE_HARD && d->lockMHz > 0;
    if (d->hasGpuOffset && !hardLock) {
        phases++;
        if (!nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, d->gpuOffsetMHz)) { failed++; allOk = false; }
    }

    // Phase 2: memory clock offset.
    if (d->hasMemOffset) {
        phases++;
        if (!nvml_set_clock_offset(g, NVML_CLOCK_MEM, d->memOffsetMHz)) { failed++; allOk = false; }
    }

    // Phase 3: power limit.
    if (d->hasPowerLimit) {
        phases++;
        if (!nvml_set_power_limit_pct(g, d->powerLimitPct)) { failed++; allOk = false; }
    }

    // Phase 4: VF curve (skipped in HARD mode — the clock is pinned by NVML).
    // SAFETY GATE: refuse VF writes if the curve READ looks implausible — a
    // struct-layout/ABI mismatch (e.g. on an unverified driver/arch) would
    // otherwise let us compute and write garbage offsets to the GPU.  NVML
    // offset/power/fan still apply.  This is the key arm64 fail-safe.
    if (g->backend && g->backend->writeSupported && !hardLock) {
        char why[160] = {};
        if (!linux_backend_curve_plausible(g, why, sizeof(why))) {
            lb_log("apply: REFUSING VF curve write — curve read implausible (%s); "
                   "NVML-only this apply to avoid writing garbage to the GPU\n", why);
        } else {
            int targets[VF_NUM_POINTS];
            bool mask[VF_NUM_POINTS];
            int n = build_curve_targets(g, d, targets, mask);
            if (n > 0) {
                phases++;
                if (!apply_curve_offsets_verified(g, targets, mask, 25)) { failed++; allOk = false; }
            }
        }
    }

    // Phase 5: locked clocks (HARD lock) or release.
    if (hardLock && g->nvml.setGpuLockedClocks) {
        phases++;
        if (g->nvml.setGpuLockedClocks(g->nvmlDevice, d->lockMHz, d->lockMHz) != NVML_SUCCESS) { failed++; allOk = false; }
    } else if (!hardLock && g->nvml.resetGpuLockedClocks) {
        g->nvml.resetGpuLockedClocks(g->nvmlDevice);  // idempotent
    }

    // Phase 6: fan.
    if (d->hasFan) {
        phases++;
        if (!nvml_set_fan(g, d->fanMode, d->fanAuto, d->fanPercent)) { failed++; allOk = false; }
    }

    gc_snprintf(msg, sizeof(msg), "%s: %d phase(s), %d failed%s",
                allOk ? "Applied" : "Applied with errors", phases, failed,
                hardLock ? " (hard clock pin)" : "");
    if (result) gc_strlcpy(result, resultSize, msg);
    lb_log("apply: %s\n", msg);
    linux_backend_refresh(g);
    return allOk;
}

bool linux_backend_reset(LinuxGpuState* g, char* result, size_t resultSize) {
    bool ok = true;
    if (g->nvml.resetGpuLockedClocks) g->nvml.resetGpuLockedClocks(g->nvmlDevice);
    ok &= nvml_set_clock_offset(g, NVML_CLOCK_GRAPHICS, 0);
    ok &= nvml_set_clock_offset(g, NVML_CLOCK_MEM, 0);
    if (g->powerLimitDefaultmW > 0 && g->nvml.setPowerLimit)
        g->nvml.setPowerLimit(g->nvmlDevice, (unsigned int)g->powerLimitDefaultmW);
    // Zero the VF curve offsets.
    if (g->backend && g->backend->writeSupported) {
        int targets[VF_NUM_POINTS] = {};
        bool mask[VF_NUM_POINTS];
        for (int i = 0; i < VF_NUM_POINTS; i++) mask[i] = (g->curve[i].freq_kHz != 0);
        apply_curve_offsets_verified(g, targets, mask, 25);
    }
    nvml_set_fan(g, FAN_MODE_AUTO, true, 0);
    if (result) gc_strlcpy(result, resultSize, ok ? "Reset to defaults" : "Reset completed with errors");
    linux_backend_refresh(g);
    return ok;
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
