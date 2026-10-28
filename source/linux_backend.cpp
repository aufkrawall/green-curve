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
// service_request_replaces_lock_domain() plus the F-APPLY-CEILING decision the
// apply transaction shares with Windows.
#include "service_lifecycle_policy.h"
#include "apply_clock_ceiling_policy.h"
// linux_platform_is_integrated_soc(), used by the capability-probe derivation
// in linux_backend_mutation.cpp so the Tegra/SoC detection exists exactly once.
#include "linux_gpu.h"
#include "log_path_redaction.h"

#include <stdarg.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// The shared rule for a VF entry the driver pins at zero offset.
#include "vf_offset_range_policy.h"

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
    char message[2048] = {};
    char redacted[sizeof(message)] = {};
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    gc_log_redact_paths(message, redacted, sizeof(redacted));
    fputs(redacted, stderr);
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
        case -100001: return "FUNCTION_MISSING";
        case -100002: return "INVALID_VF_DATA";
        case -100003: return "NOT_ATTEMPTED";
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
#include "linux_backend_vf_read.h"

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
// on a driver that rejects multi-bit masks the verify loop converges via
// repeated single-changed-point passes.
static bool apply_curve_offsets_verified(LinuxGpuState* g, const int* targetOffsets,
                                         const bool* pointMask, int maxBatchPasses) {
    const VfBackendSpec* b = g->backend;
    if (!b || !b->writeSupported || !targetOffsets || !pointMask) return false;

    bool desiredMask[VF_NUM_POINTS] = {};
    int desiredOffsets[VF_NUM_POINTS] = {};
    bool pendingMask[VF_NUM_POINTS] = {};
    bool driverRefused[VF_NUM_POINTS] = {};
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
                // Same rule as the Windows writer (main_runtime_gpu.cpp): an
                // idle placeholder entry below the operating range that the
                // driver pins at offset 0 can never converge.  Without this the
                // loop spent every remaining pass (~1 s each) rewriting it and
                // then failed -- and rolled back -- a curve that had landed.
                if (pendingMask[i] &&
                    vf_offset_zero_readback_is_benign(desiredOffsets[i],
                        g->freqOffsets[i], g->curve[i].freq_kHz,
                        VF_PLACEHOLDER_MAX_FREQ_MHz * 1000u)) {
                    if (!driverRefused[i]) {
                        driverRefused[i] = true;
                        lb_log("curve offset: driver refuses point %d (wrote %dkHz, readback pinned at 0, liveFreq=%ukHz); accepting as non-offsettable placeholder\n",
                               i, desiredOffsets[i], g->curve[i].freq_kHz);
                    }
                    pendingMask[i] = false;
                }
                if (pendingMask[i]) {
                    anyPending = true;
                    lb_log("curve batch pass %d unconverged: ci=%d wroteOffset=%dkHz readbackOffset=%dkHz\n",
                           pass + 1, i, desiredOffsets[i], g->freqOffsets[i]);
                }
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
    // Read-only load witnesses for the apply-transition trace (F-APPLY-CEILING);
    // both optional, and a board without them logs the load as unknown.
    a->getUtilization = sym<nvmlDeviceGetUtilizationRates_t>(h, "nvmlDeviceGetUtilizationRates");
    a->getPowerUsage = sym<nvmlDeviceGetPowerUsage_t>(h, "nvmlDeviceGetPowerUsage");
    a->setGpuLockedClocks = sym<nvmlDeviceSetGpuLockedClocks_t>(h, "nvmlDeviceSetGpuLockedClocks");
    a->resetGpuLockedClocks = sym<nvmlDeviceResetGpuLockedClocks_t>(h, "nvmlDeviceResetGpuLockedClocks");
    a->setMemoryLockedClocks = sym<nvmlDeviceSetMemoryLockedClocks_t>(h, "nvmlDeviceSetMemoryLockedClocks");
    a->resetMemoryLockedClocks = sym<nvmlDeviceResetMemoryLockedClocks_t>(h, "nvmlDeviceResetMemoryLockedClocks");
}

#include "linux_backend_nvml_write.cpp"

#include "linux_backend_xbar.cpp"

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
    fprintf(out, "NvAPI getInfo          : %s (status=%d; active point mask)\n",
            infoOk ? "ok" : "FAILED", g->vfInfoStatus);
    fprintf(out, "NvAPI getStatus (curve): %s (status=%d points=%d)\n",
            curveOk ? "ok" : "FAILED", g->vfStatusStatus, g->numPopulated);
    fprintf(out, "NvAPI getControl       : %s (status=%d; write ABI probe)\n",
            ctrlOk ? "ok" : "FAILED", g->vfControlStatus);
    fprintf(out, "Structural validation  : %s%s%s\n", plausible ? "ok" : "FAILED",
            (!plausible && why[0]) ? " — " : "", (!plausible && why[0]) ? why : "");
    fprintf(out, "VF layout              : %s\n",
            linux_vf_layout_text(&g->vfLayout).text);
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
