// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_backend.cpp — native Linux GPU control backend (see linux_backend.h).
// Faithfully ports the Windows NvAPI/NVML read+apply logic; each function names
// its Windows counterpart in a comment so the two stay in sync.

#include "linux_backend.h"
#include "linux_gpu_selection.h"
#include "linux_architecture_policy.h"
#include "linux_vf_validation.h"
#include "vf_backends.h"
#include "fan_curve.h"
#include "fan_runtime_policy.h"
#include "linux_curve_targets.h"
// linux_platform_is_integrated_soc(), used by the capability-probe derivation
// in linux_backend_mutation.cpp so the Tegra/SoC detection exists exactly once.
#include "linux_gpu.h"

#include <stdarg.h>
#include <limits.h>
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
static const int LB_NVAPI_FUNCTION_MISSING = -100001;
static const int LB_NVAPI_INVALID_DATA = -100002;

struct LinuxVfReadCandidate {
    VFCurvePoint curve[VF_NUM_POINTS];
    int offsets[VF_NUM_POINTS];
    int populated;
};

// Read current per-point editable mask + active clock count. A cached result is
// never promoted to fresh: every published VF snapshot proves all three driver
// calls (info, status and control) in the same refresh transaction.
static bool nvapi_get_vf_info(LinuxGpuState* g) {
    const VfBackendSpec* b = g->backend;
    g->vfInfoFresh = false;
    if (!b || !g->gpuHandle || !g->nvapiQi) {
        g->vfInfoStatus = LB_NVAPI_FUNCTION_MISSING;
        return false;
    }
    auto getInfo = (nvapi_buf_t)g->nvapiQi(b->getInfoId);
    if (!getInfo) {
        g->vfInfoStatus = LB_NVAPI_FUNCTION_MISSING;
        return false;
    }
    unsigned int infoSize = b->infoBufferSize ? b->infoBufferSize : 0x4000;
    if (infoSize > 0x4000 || b->infoBufferSize > infoSize ||
        b->infoMaskOffset + sizeof(g->vfMask) > infoSize ||
        b->infoNumClocksOffset + sizeof(g->vfNumClocks) > infoSize) {
        g->vfInfoStatus = LB_NVAPI_INVALID_DATA;
        return false;
    }
    unsigned char* ibuf = (unsigned char*)calloc(1, infoSize);
    if (!ibuf) {
        g->vfInfoStatus = -1;
        return false;
    }
    unsigned int ver = (b->infoVersion << 16) | infoSize;
    memcpy(ibuf, &ver, sizeof(ver));
    memset(ibuf + b->infoMaskOffset, 0xFF, sizeof(g->vfMask));
    int status = getInfo(g->gpuHandle, ibuf);
    g->vfInfoStatus = status;
    if (!nvapi_ok(status)) {
        free(ibuf);
        return false;
    }
    unsigned int returnedVersion = 0;
    memcpy(&returnedVersion, ibuf, sizeof(returnedVersion));
    if (!linux_vf_returned_version_valid(returnedVersion, b->infoVersion,
                                         infoSize)) {
        g->vfInfoStatus = LB_NVAPI_INVALID_DATA;
        free(ibuf);
        return false;
    }
    unsigned char mask[sizeof(g->vfMask)] = {};
    unsigned int numClocks = 0;
    memcpy(mask, ibuf + b->infoMaskOffset, sizeof(mask));
    memcpy(&numClocks, ibuf + b->infoNumClocksOffset, sizeof(numClocks));
    free(ibuf);
    bool anyMask = false;
    for (unsigned char value : mask) anyMask |= value != 0;
    if (!anyMask || numClocks == 0 || numClocks > 64) {
        g->vfInfoStatus = LB_NVAPI_INVALID_DATA;
        return false;
    }
    memcpy(g->vfMask, mask, sizeof(mask));
    g->vfNumClocks = numClocks;
    g->vfInfoCached = true;
    g->vfInfoFresh = true;
    return true;
}

static bool nvapi_read_curve_candidate(LinuxGpuState* g,
                                       LinuxVfReadCandidate* candidate) {
    const VfBackendSpec* b = g->backend;
    g->vfStatusFresh = false;
    if (!b || !b->readSupported || !candidate || !g->gpuHandle) {
        g->vfStatusStatus = LB_NVAPI_FUNCTION_MISSING;
        return false;
    }
    if (!g->vfInfoFresh) {
        g->vfStatusStatus = LB_NVAPI_INVALID_DATA;
        return false;
    }
    auto getStatus = (nvapi_buf_t)g->nvapiQi(b->getStatusId);
    if (!getStatus || b->statusBufferSize == 0 || b->statusBufferSize > 0x4000 ||
        b->statusMaskOffset + sizeof(g->vfMask) > b->statusBufferSize ||
        b->statusNumClocksOffset + sizeof(g->vfNumClocks) >
            b->statusBufferSize ||
        b->statusEntriesOffset + (VF_NUM_POINTS - 1u) *
            b->statusEntryStride + 8u > b->statusBufferSize) {
        g->vfStatusStatus = LB_NVAPI_FUNCTION_MISSING;
        return false;
    }
    unsigned char* buf = (unsigned char*)calloc(1, b->statusBufferSize);
    if (!buf) {
        g->vfStatusStatus = -1;
        return false;
    }
    unsigned int ver = (b->statusVersion << 16) | b->statusBufferSize;
    memcpy(buf, &ver, sizeof(ver));
    memcpy(buf + b->statusMaskOffset, g->vfMask, sizeof(g->vfMask));
    memcpy(buf + b->statusNumClocksOffset, &g->vfNumClocks,
           sizeof(g->vfNumClocks));
    int status = getStatus(g->gpuHandle, buf);
    g->vfStatusStatus = status;
    bool ok = nvapi_ok(status);
    if (ok) {
        unsigned int returnedVersion = 0;
        unsigned int returnedNumClocks = 0;
        unsigned char returnedMask[sizeof(g->vfMask)] = {};
        memcpy(&returnedVersion, buf, sizeof(returnedVersion));
        if (!linux_vf_returned_version_valid(returnedVersion,
                b->statusVersion, b->statusBufferSize)) {
            g->vfStatusStatus = LB_NVAPI_INVALID_DATA;
            ok = false;
        }
        memcpy(returnedMask, buf + b->statusMaskOffset,
               sizeof(returnedMask));
        memcpy(&returnedNumClocks, buf + b->statusNumClocksOffset,
               sizeof(returnedNumClocks));
        if (ok && (memcmp(returnedMask, g->vfMask,
                          sizeof(returnedMask)) != 0 ||
                   returnedNumClocks != g->vfNumClocks)) {
            g->vfStatusStatus = LB_NVAPI_INVALID_DATA;
            ok = false;
        }
    }
    if (ok) {
        // The status table concatenates the VF points of EVERY clock domain
        // (`numClocks` of them), not just the graphics curve, and the domains
        // are not padded to a fixed stride. Observed on an RTX 5070 (driver
        // 610.43.03): the graphics domain is 127 points (180 MHz @ 450 mV ..
        // 3157 MHz @ 1240 mV), index 127 already belongs to another domain
        // (405 MHz @ 540 mV), index 128 to a third, and 129..131 are the
        // ~14 GHz GDDR7 memory domain.
        //
        // Reading a fixed VF_NUM_POINTS window therefore drags foreign points
        // into the curve, where they break the ordered-voltage invariant and
        // failed the ENTIRE snapshot closed — no VF read and no writes at all
        // on that GPU. Voltage ascends monotonically within a domain and drops
        // at a domain boundary, so the graphics curve is the leading
        // non-decreasing run. Everything past it is zeroed: unpopulated points
        // are skipped by validation and by apply_curve_offsets_verified(), so
        // a foreign domain can be neither published nor written.
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            unsigned int freq = 0, volt = 0;
            unsigned int off = b->statusEntriesOffset + (unsigned int)i * b->statusEntryStride;
            if (off + 8 <= b->statusBufferSize) {
                memcpy(&freq, buf + off, 4);
                memcpy(&volt, buf + off + 4, 4);
            }
            candidate->curve[i].freq_kHz = freq;
            candidate->curve[i].volt_uV = volt;
        }
        int domainEnd = linux_vf_graphics_domain_length(candidate->curve,
                                                        VF_NUM_POINTS);
        candidate->populated = 0;
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            if (i >= domainEnd) {
                candidate->curve[i].freq_kHz = 0;
                candidate->curve[i].volt_uV = 0;
            } else if (candidate->curve[i].freq_kHz > 0) {
                candidate->populated++;
            }
        }
        if (!g->graphicsDomainBoundaryLogged ||
            g->lastLoggedGraphicsDomainEnd != domainEnd) {
            if (domainEnd != VF_NUM_POINTS) {
                lb_log("curve: graphics domain ends at point %d "
                       "(%d MHz @ %u mV); dropped %d trailing entries "
                       "belonging to other clock domains\n",
                       domainEnd - 1,
                       domainEnd > 0
                           ? (int)(candidate->curve[domainEnd - 1].freq_kHz /
                                   1000u) : 0,
                       domainEnd > 0
                           ? candidate->curve[domainEnd - 1].volt_uV / 1000u
                           : 0u,
                       VF_NUM_POINTS - domainEnd);
            } else if (g->graphicsDomainBoundaryLogged) {
                lb_log("curve: graphics domain now spans the full %d-point "
                       "read window\n", VF_NUM_POINTS);
            }
            g->lastLoggedGraphicsDomainEnd = domainEnd;
            g->graphicsDomainBoundaryLogged = true;
        }
        g->vfStatusFresh = true;
    }
    free(buf);
    return ok;
}

// ports nvapi_read_control_table()
static bool nvapi_read_control_table(LinuxGpuState* g, unsigned char* buf, unsigned int bufSize) {
    const VfBackendSpec* b = g->backend;
    g->vfControlFresh = false;
    if (!b || !g->gpuHandle || !buf || bufSize < b->controlBufferSize ||
        b->controlBufferSize == 0 || b->controlBufferSize > 0x4000 ||
        b->controlMaskOffset + sizeof(g->vfMask) > b->controlBufferSize ||
        b->controlEntryBaseOffset + (VF_NUM_POINTS - 1u) *
            b->controlEntryStride + b->controlEntryDeltaOffset +
            sizeof(int) > b->controlBufferSize) {
        g->vfControlStatus = LB_NVAPI_FUNCTION_MISSING;
        return false;
    }
    auto getFunc = (nvapi_buf_t)g->nvapiQi(b->getControlId);
    if (!getFunc) {
        g->vfControlStatus = LB_NVAPI_FUNCTION_MISSING;
        return false;
    }
    if (!g->vfInfoFresh) {
        g->vfControlStatus = LB_NVAPI_INVALID_DATA;
        return false;
    }
    memset(buf, 0, b->controlBufferSize);
    unsigned int ver = (b->controlVersion << 16) | b->controlBufferSize;
    memcpy(buf, &ver, sizeof(ver));
    memcpy(buf + b->controlMaskOffset, g->vfMask, sizeof(g->vfMask));
    int status = getFunc(g->gpuHandle, buf);
    g->vfControlStatus = status;
    g->vfControlFresh = nvapi_ok(status);
    if (g->vfControlFresh) {
        unsigned int returnedVersion = 0;
        unsigned char returnedMask[sizeof(g->vfMask)] = {};
        memcpy(&returnedVersion, buf, sizeof(returnedVersion));
        if (!linux_vf_returned_version_valid(returnedVersion,
                b->controlVersion, b->controlBufferSize)) {
            g->vfControlStatus = LB_NVAPI_INVALID_DATA;
            g->vfControlFresh = false;
        }
        memcpy(returnedMask, buf + b->controlMaskOffset,
               sizeof(returnedMask));
        if (g->vfControlFresh &&
            memcmp(returnedMask, g->vfMask, sizeof(returnedMask)) != 0) {
            g->vfControlStatus = LB_NVAPI_INVALID_DATA;
            g->vfControlFresh = false;
        }
    }
    return g->vfControlFresh;
}

static bool nvapi_read_offsets_candidate(LinuxGpuState* g,
                                         LinuxVfReadCandidate* candidate) {
    const VfBackendSpec* b = g->backend;
    if (!b || !candidate || !b->readSupported ||
        b->controlBufferSize == 0 || b->controlBufferSize > 0x4000) {
        g->vfControlStatus = LB_NVAPI_FUNCTION_MISSING;
        g->vfControlFresh = false;
        return false;
    }
    unsigned char* buf = (unsigned char*)calloc(1, b->controlBufferSize ? b->controlBufferSize : 0x4000);
    if (!buf) return false;
    bool ok = nvapi_read_control_table(g, buf, b->controlBufferSize);
    if (ok) {
        for (int i = 0; i < VF_NUM_POINTS; i++) {
            int delta = 0;
            unsigned int off = b->controlEntryBaseOffset + (unsigned int)i * b->controlEntryStride + b->controlEntryDeltaOffset;
            if (off + sizeof(delta) <= b->controlBufferSize) memcpy(&delta, buf + off, sizeof(delta));
            else delta = 0;
            candidate->offsets[i] = delta;
        }
    }
    free(buf);
    return ok;
}

static bool linux_vf_candidate_structurally_valid(
    const LinuxGpuState* g, const LinuxVfReadCandidate* candidate,
    char* why, size_t whySize) {
    return g && candidate && linux_vf_snapshot_structurally_valid(
        g->vfInfoFresh, g->vfStatusFresh, g->vfControlFresh,
        g->vfMask, sizeof(g->vfMask), g->vfNumClocks,
        candidate->curve, candidate->offsets, candidate->populated,
        why, whySize);
}

static bool nvapi_read_vf_snapshot(LinuxGpuState* g, char* why,
                                   size_t whySize) {
    if (why && whySize) why[0] = 0;
    g->vfInfoFresh = false;
    g->vfStatusFresh = false;
    g->vfControlFresh = false;
    g->vfSnapshotFresh = false;
    g->vfStructureValid = false;
    g->vfInfoStatus = LB_NVAPI_FUNCTION_MISSING;
    g->vfStatusStatus = LB_NVAPI_FUNCTION_MISSING;
    g->vfControlStatus = LB_NVAPI_FUNCTION_MISSING;
    LinuxVfReadCandidate candidate = {};
    if (!nvapi_get_vf_info(g)) {
        if (why) gc_snprintf(why, whySize, "getInfo status=%d (%s)",
            g->vfInfoStatus, nvapi_status_name(g->vfInfoStatus));
        return false;
    }
    if (!nvapi_read_curve_candidate(g, &candidate)) {
        if (why) gc_snprintf(why, whySize, "getStatus status=%d (%s)",
            g->vfStatusStatus, nvapi_status_name(g->vfStatusStatus));
        return false;
    }
    if (!nvapi_read_offsets_candidate(g, &candidate)) {
        if (why) gc_snprintf(why, whySize, "getControl status=%d (%s)",
            g->vfControlStatus, nvapi_status_name(g->vfControlStatus));
        return false;
    }
    if (!linux_vf_candidate_structurally_valid(g, &candidate, why, whySize))
        return false;
    memcpy(g->curve, candidate.curve, sizeof(g->curve));
    memcpy(g->freqOffsets, candidate.offsets, sizeof(g->freqOffsets));
    g->numPopulated = candidate.populated;
    g->vfStructureValid = true;
    g->vfSnapshotFresh = true;
    return true;
}

// Write verification needs a fresh CONTROL table before the full status read
// performed at transaction completion.
static bool nvapi_read_offsets(LinuxGpuState* g) {
    LinuxVfReadCandidate candidate = {};
    if (!nvapi_read_offsets_candidate(g, &candidate)) return false;
    memcpy(g->freqOffsets, candidate.offsets, sizeof(g->freqOffsets));
    return true;
}

// ports nvapi_read_curve(); publishes only a complete atomic VF snapshot.
static bool nvapi_read_curve(LinuxGpuState* g) {
    char why[160] = {};
    return nvapi_read_vf_snapshot(g, why, sizeof(why));
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
    a->getArchitecture = sym<nvmlDeviceGetArchitecture_t>(h, "nvmlDeviceGetArchitecture");
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
    // The *intended* duty, and the only real readback of a manual fan write.
    // getFanSpeed is measured telemetry; see fan_manual_write_confirmed().
    a->getTargetFanSpeed = sym<nvmlDeviceGetTargetFanSpeed_t>(h, "nvmlDeviceGetTargetFanSpeed");
    a->setFanSpeed = sym<nvmlDeviceSetFanSpeed_v2_t>(h, "nvmlDeviceSetFanSpeed_v2");
    a->setDefaultFanSpeed = sym<nvmlDeviceSetDefaultFanSpeed_v2_t>(h, "nvmlDeviceSetDefaultFanSpeed_v2");
    a->getTemperature = sym<nvmlDeviceGetTemperature_t>(h, "nvmlDeviceGetTemperature");
    a->getClock = sym<nvmlDeviceGetClock_t>(h, "nvmlDeviceGetClock");
    a->setGpuLockedClocks = sym<nvmlDeviceSetGpuLockedClocks_t>(h, "nvmlDeviceSetGpuLockedClocks");
    a->resetGpuLockedClocks = sym<nvmlDeviceResetGpuLockedClocks_t>(h, "nvmlDeviceResetGpuLockedClocks");
    a->setMemoryLockedClocks = sym<nvmlDeviceSetMemoryLockedClocks_t>(h, "nvmlDeviceSetMemoryLockedClocks");
    a->resetMemoryLockedClocks = sym<nvmlDeviceResetMemoryLockedClocks_t>(h, "nvmlDeviceResetMemoryLockedClocks");
}

#include "linux_backend_nvml_write.cpp"

#include "linux_backend_discovery.cpp"

#include "linux_backend_mutation.cpp"
// ===========================================================================
// Self-validation (arm64 / new-driver pre-flight; read-only)
// ===========================================================================

bool linux_backend_curve_plausible(const LinuxGpuState* g, char* why, size_t whySize) {
    if (why && whySize) why[0] = 0;
    if (!g || !g->gpuHandle || !g->backend || !g->backend->readSupported) {
        if (why) gc_strlcpy(why, whySize, "no matched NvAPI VF backend");
        return false;
    }
    if (!g->vfSnapshotFresh || !g->vfStructureValid) {
        if (why) gc_strlcpy(why, whySize,
            "VF info/status/control snapshot is not fresh");
        return false;
    }
    LinuxVfReadCandidate candidate = {};
    memcpy(candidate.curve, g->curve, sizeof(candidate.curve));
    memcpy(candidate.offsets, g->freqOffsets, sizeof(candidate.offsets));
    candidate.populated = g->numPopulated;
    return linux_vf_candidate_structurally_valid(g, &candidate, why, whySize);
}

bool linux_backend_self_test(LinuxGpuState* g, FILE* out) {
    if (!out) out = stdout;
    fprintf(out, "=== Green Curve driver/arch self-test (read-only, no GPU changes) ===\n");
    fprintf(out, "GPU: %s  family=%d  pci=%04x:%02x:%02x.%u\n",
            g->gpuName[0] ? g->gpuName : "?", (int)g->family,
            g->selectedGpu.pciDomain, g->selectedGpu.pciBus,
            g->selectedGpu.pciDevice, g->selectedGpu.pciFunction);

    bool nvmlOk = g->nvmlReady;
    fprintf(out, "NVML ready             : %s\n", nvmlOk ? "yes" : "NO");
    if (g->curveOffsetRangeKnown)
        fprintf(out, "NVML GPC offset range  : %d .. %d MHz\n", g->gpuOffsetMinMHz, g->gpuOffsetMaxMHz);

    if (!g->gpuHandle || !g->backend) {
        fprintf(out, "NvAPI VF binding       : NOT available\n");
        fprintf(out, "Architecture source    : %s\n",
                linux_gpu_architecture_source_name(g->architectureSource));
        fprintf(out, "Health                  : %s%s%s\n",
                service_gpu_health_reason_name(g->health.reason),
                g->health.detail[0] ? " — " : "", g->health.detail);
        fprintf(out, "Available domains       : 0x%08x\n",
                (unsigned)g->health.availableMutationDomains);
        fprintf(out, "\nVerdict: DEGRADED — VF editing is unavailable; advertised independent "
                     "NVML domains remain usable.\n");
        return false;
    }
    fprintf(out, "NvAPI VF binding       : matched handle=%p\n", g->gpuHandle);
    fprintf(out, "Binding match method   : %s\n",
            linux_gpu_match_method_name(g->nvapiMatchMethod));
    fprintf(out, "NvAPI VF backend       : %s%s\n", g->backend->name,
            g->backend->bestGuessOnly ? " (best-effort unrecognized family)" : "");
    fprintf(out, "Architecture source    : %s\n",
            linux_gpu_architecture_source_name(g->architectureSource));
    fprintf(out, "expected struct sizes  : pstates20=%u arch=%u (compile-time pinned)\n",
            (unsigned)sizeof(nvapiPerfPstates20Info_t), (unsigned)sizeof(nvapiGpuArchInfo_t));

    LinuxBackendRefreshResult refresh = linux_backend_refresh(g);
    bool infoOk = g->vfInfoFresh;
    bool curveOk = g->vfStatusFresh;
    bool ctrlOk = g->vfControlFresh;
    char why[160] = {};
    bool plausible = linux_backend_curve_plausible(g, why, sizeof(why));
    LinuxHardwareSnapshot hardware = {};
    char hardwareWhy[160] = {};
    bool hardwareSnapshotOk = linux_backend_capture_snapshot(
        g, &hardware, hardwareWhy, sizeof(hardwareWhy));
    // Capture is the single producer for both availableMutationDomains and the
    // derived capability probe.  Keep the refresh envelope printed below in
    // sync with the facts the self-test just captured.
    refresh.health = g->health;
    fprintf(out, "NvAPI getInfo          : %s (status=%d numClocks=%u)\n",
            infoOk ? "ok" : "FAILED", g->vfInfoStatus, g->vfNumClocks);
    fprintf(out, "NvAPI getStatus (curve): %s (status=%d points=%d)\n",
            curveOk ? "ok" : "FAILED", g->vfStatusStatus, g->numPopulated);
    fprintf(out, "NvAPI getControl       : %s (status=%d; write ABI probe)\n",
            ctrlOk ? "ok" : "FAILED", g->vfControlStatus);
    fprintf(out, "Structural validation  : %s%s%s\n", plausible ? "ok" : "FAILED",
            (!plausible && why[0]) ? " — " : "", (!plausible && why[0]) ? why : "");
    fprintf(out, "Snapshot freshness     : %s (recovery attempted=%s succeeded=%s)\n",
            refresh.vfFresh ? "fresh" : "STALE",
            refresh.health.recoveryAttempted ? "yes" : "no",
            refresh.health.recoverySucceeded ? "yes" : "no");
    fprintf(out, "Rollback snapshot      : %s%s%s\n",
            hardwareSnapshotOk ? "available" : "UNAVAILABLE",
            (!hardwareSnapshotOk && hardwareWhy[0]) ? " — " : "",
            (!hardwareSnapshotOk && hardwareWhy[0]) ? hardwareWhy : "");
    fprintf(out, "Health                  : %s%s%s\n",
            service_gpu_health_reason_name(refresh.health.reason),
            refresh.health.detail[0] ? " — " : "", refresh.health.detail);
    fprintf(out, "Available domains       : 0x%08x\n",
            (unsigned)refresh.health.availableMutationDomains);

    // Per-domain control surface, in the same shape the Windows --self-test
    // prints, so a report from either platform reads identically.
    fprintf(out, "Memory topology        : %s\n",
            gpu_memory_topology_name(g->capability.memoryTopology));
    fprintf(out, "Control surface        : %s\n",
            gpu_control_surface_class_name(gpu_capability_surface_class(&g->capability)));
    for (int i = 0; i < GPU_CAP_DOMAIN_COUNT; ++i) {
        gc_u32 mask = gpu_capability_mask_for_index(i);
        fprintf(out, "  %-18s   : %s\n", gpu_capability_domain_name(i),
                gpu_capability_name(gpu_capability_get(&g->capability, mask)));
    }
    if (g->capability.memoryTopology == GPU_MEMORY_TOPOLOGY_UNIFIED) {
        fprintf(out, "NOTE: memory is unified with the CPU. A memory clock offset here\n"
                     "targets system RAM, not a separate VRAM pool.\n");
    }

    bool pass = nvmlOk && infoOk && curveOk && plausible && ctrlOk &&
                hardwareSnapshotOk;
    fprintf(out, "\nVerdict: %s\n", pass
            ? "PASS — read + write-struct paths validated; VF apply should work on this driver/arch."
            : "DEGRADED — see exact failure above. VF writes remain gated until a fresh complete "
              "snapshot validates; advertised independent NVML domains still work.");
    return pass;
}
