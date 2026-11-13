// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the production memory setter, rollback and failure reporting.
#include "app_shared.h"
#include "apply_clock_ceiling_policy.h"
#include "clock_reset_policy.h"
#include "control_readback_policy.h"
#include "memory_offset_units.h"
#include "ownership_handback_policy.h"
#include <cstdio>
#include <cstring>

namespace memory_recovery_fixture {
static AppData g_app;
static NvmlApi g_nvml_api;
static void* g_serviceFanThread;
static int hardwareMHz, writes, releases;
static bool verifyReset, setterFails, coreFails;
static bool pstateReadSucceeds, pstateMemoryPresent, setterLeavesUnchanged;
static bool preciseReadback;
static bool setterClaimsMatch;
static gc_u32 preciseSurface;
static int p0HardwareMHz, p0Writes;
static bool p0SetFails, p0ReadFails, p0BadMetadata, p0ConservativeRange;
static bool p0ReadFailsAfterSet;
static bool vfGlobal = true;
static int graphicsLegacyWrites;
static gc_u32 durableGraphicsSurfaces;
static int durableGraphicsStores;
static bool durableGraphicsStoreFails;
static struct { void clear() {} } g_applyScalarShiftEvidence;
static void debug_log(const char*, ...) {}
static void set_last_apply_phase(const char*) {}
static void Sleep(unsigned long) {}
static bool nvml_ensure_ready() { return true; }
static const char* nvml_err_name(int) { return "fixture"; }
static bool vf_curve_global_gpu_offset_supported() { return vfGlobal; }
static bool nvapi_set_gpu_offset(int, bool) { ++graphicsLegacyWrites; return !coreFails; }
static bool nvapi_read_curve() { g_app.curve[0].freq_kHz = 2000000; return true; }
static bool apply_curve_offsets_verified(const int*, const bool*, int) { return !coreFails; }
static bool nvml_set_clock_offset_domain(unsigned int, int value, bool* exact, char*, size_t) {
    ++writes;
    if (!setterLeavesUnchanged) hardwareMHz = value;
    *exact = setterClaimsMatch || (verifyReset && value == 0);
    return !setterFails;
}
static bool nvml_read_clock_offsets(char*, size_t) {
    if (preciseReadback) {
        g_app.memOffsetSurface = preciseSurface;
        g_app.readback.memOffset = memory_offset_khz_from_effective_mhz(
            preciseSurface == CLOCK_OFFSET_SURFACE_P0 ? p0HardwareMHz : hardwareMHz,
            &g_app.memClockOffsetkHz);
        return g_app.readback.memOffset;
    }
    if (!verifyReset || hardwareMHz != 0) return false;
    g_app.memClockOffsetkHz = 0;
    g_app.readback.memOffset = true;
    return true;
}
static bool nvapi_read_pstates() {
    g_app.pstateMemMaxMHz = pstateReadSucceeds && pstateMemoryPresent
        ? (unsigned int)(10000 + hardwareMHz / 2) : 0;
    return pstateReadSucceeds;
}
static void detect_clock_offsets() {
    if (g_app.pstateMemMaxMHz > 0 && g_app.nvmlMemMaxMHz > 0) {
        g_app.memClockOffsetkHz =
            ((int)g_app.pstateMemMaxMHz - (int)g_app.nvmlMemMaxMHz) * 1000;
        g_app.readback.memOffset = true;
    }
}
#include "gpu_backend_memory_offset.h"
static bool service_ownership_marker_note_graphics_surface_before_write(gc_u32 surface) {
    return graphics_scalar_recovery_prepare(durableGraphicsSurfaces, surface, [](gc_u32) {
        ++durableGraphicsStores; return !durableGraphicsStoreFails;
    });
}
#define GREEN_CURVE_SERVICE_BINARY
#include "gpu_backend_graphics_offset.h"
#undef GREEN_CURVE_SERVICE_BINARY
static bool nvapi_set_power_limit(int, bool = false) { return true; }
static void stop_fan_curve_runtime() {}
static bool app_is_service_process() { return true; }
static void stop_service_fan_runtime_thread() {}
static bool nvml_set_fan_auto(char*, size_t) { return true; }
static void clear_runtime_selective_gpu_offset_request() {}
static int release(nvmlDevice_t) { ++releases; return NVML_SUCCESS; }
#include "main_gpu_rollback.h"
struct ApplyClockCeilingGuard {
    struct { bool required = false; } plan;
    bool armed = false;
    void arm() {}
    bool must_refuse_transition() const { return false; }
    void retain(const char*) {}
    void adopt(const char*) {}
};
static void StringCchCopyA(char* out, size_t size, const char* value) {
    std::snprintf(out, size, "%s", value);
}
static void refresh_global_state(char*, size_t) {}
#include "gpu_backend_apply_failure.h"
#include "gpu_selection_write_evidence.h"

static void initialize(bool canVerifyReset) {
    g_app = {};
    g_app.gpuFamily = GPU_FAMILY_BLACKWELL;
    g_app.powerLimitPct = 100;
    g_app.fanIsAuto = true;
    g_app.activeFanMode = FAN_MODE_AUTO;
    g_app.readback.memOffset = true;
    g_nvml_api = {};
    g_nvml_api.resetGpuLockedClocks = release;
    g_nvml_api.getMemClkVfOffset = [](nvmlDevice_t, int* out) -> nvmlReturn_t {
        *out = hardwareMHz;
        return preciseReadback ? NVML_SUCCESS : NVML_ERROR_NOT_SUPPORTED;
    };
    g_nvml_api.setMemClkVfOffset = [](nvmlDevice_t, int value) -> nvmlReturn_t {
        bool exact = false;
        return nvml_set_clock_offset_domain(NVML_CLOCK_MEM, value, &exact, nullptr, 0)
            ? NVML_SUCCESS : NVML_ERROR_NOT_SUPPORTED;
    };
    g_nvml_api.getClockOffsets = [](nvmlDevice_t, nvmlClockOffset_t* info) -> nvmlReturn_t {
        info->clockOffsetMHz = p0HardwareMHz;
        info->minClockOffsetMHz = -10000; info->maxClockOffsetMHz = 10000;
        if (p0ConservativeRange) info->minClockOffsetMHz = info->maxClockOffsetMHz = 0;
        if (p0BadMetadata) info->type = info->type == NVML_CLOCK_GRAPHICS ? NVML_CLOCK_MEM : NVML_CLOCK_GRAPHICS;
        return p0ReadFails || (p0ReadFailsAfterSet && p0Writes) ? NVML_ERROR_NOT_SUPPORTED : NVML_SUCCESS;
    };
    g_nvml_api.setClockOffsets = [](nvmlDevice_t, nvmlClockOffset_t* info) -> nvmlReturn_t {
        ++p0Writes;
        p0HardwareMHz = info->clockOffsetMHz;
        return p0SetFails ? NVML_ERROR_NOT_SUPPORTED : NVML_SUCCESS;
    };
    hardwareMHz = writes = releases = 0;
    verifyReset = canVerifyReset;
    setterFails = coreFails = false;
    pstateReadSucceeds = pstateMemoryPresent = setterLeavesUnchanged = false;
    preciseReadback = false;
    setterClaimsMatch = false;
    preciseSurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    p0HardwareMHz = p0Writes = 0;
    p0SetFails = p0ReadFails = p0BadMetadata = p0ConservativeRange = false;
    p0ReadFailsAfterSet = false;
    vfGlobal = true; graphicsLegacyWrites = 0;
    durableGraphicsSurfaces = 0; durableGraphicsStores = 0; durableGraphicsStoreFails = false;
}

static int run_graphics_recovery_marker_tests() {
    ServiceBootIdentity boot{1, 2}; GpuAdapterInfo target{}; target.valid = target.pciInfoValid = 1; target.pciBus = 3;
    ServiceOwnershipMarker marker{}, decoded{};
    service_ownership_marker_initialize(&marker, boot, &target);
    marker.graphicsControlSurfaces = GRAPHICS_SCALAR_RECOVERY_ALL;
    if (!service_ownership_marker_decode(&marker, sizeof(marker), &decoded) ||
        decoded.graphicsControlSurfaces != GRAPHICS_SCALAR_RECOVERY_ALL || decoded.targetGpu.pciBus != 3) return 9033;
    ServiceOwnershipMarkerV1 old{};
    old.magic = SERVICE_OWNERSHIP_MARKER_MAGIC; old.version = 1; old.size = 216; old.handbackAttempts = 1;
    old.bootIdentity = boot; old.targetGpu = target;
    if (!service_ownership_marker_decode(&old, sizeof(old), &decoded) || decoded.version != 2 ||
        decoded.size != 224 || decoded.handbackAttempts != 1 || decoded.graphicsControlSurfaces ||
        decoded.targetGpu.pciBus != 3 || !service_boot_identity_equal(decoded.bootIdentity, boot)) return 9034;
    const auto before = decoded;
    marker.graphicsControlSurfaces = 4;
    if (service_ownership_marker_decode(&marker, sizeof(marker), &decoded) ||
        memcmp(&before, &decoded, sizeof(before))) return 9035;
    marker.graphicsControlSurfaces = 1; marker.targetGpu.valid = 0;
    if (service_ownership_marker_valid(&marker)) return 9036;
    marker.targetGpu.valid = 1; marker.reserved = 1;
    if (service_ownership_marker_valid(&marker)) return 9037;
    old.version = 2;
    if (service_ownership_marker_decode(&old, sizeof(old), &decoded) ||
        service_ownership_marker_decode(&before, sizeof(before) - 1, &decoded)) return 9038;
    gc_u32 covered = 0; int stores = 0; bool failStore = true;
    const auto store = [&](gc_u32 mask) { ++stores; return !failStore && mask == (covered | 1u); };
    if (graphics_scalar_recovery_prepare(covered, CLOCK_OFFSET_SURFACE_P0, store) || covered || stores != 1) return 9039;
    failStore = false;
    if (!graphics_scalar_recovery_prepare(covered, CLOCK_OFFSET_SURFACE_P0, store) || covered != 1 || stores != 2 ||
        !graphics_scalar_recovery_prepare(covered, CLOCK_OFFSET_SURFACE_P0, store) || stores != 2) return 9040;
    if (graphics_scalar_recovery_prepare(covered, 99, store) || covered != 1 || stores != 2) return 9041;
    covered = 4;
    if (graphics_scalar_recovery_prepare(covered, CLOCK_OFFSET_SURFACE_P0, store) || covered != 4 || stores != 2) return 9042;
    old.version = 1;
    unsigned char legacy[216]{}; memcpy(legacy, &old, sizeof(old));
    memset(legacy + 212, 0xff, 4); // Frozen tail padding cannot become new control evidence.
    if (!service_ownership_marker_decode(legacy, sizeof(legacy), &decoded) || decoded.graphicsControlSurfaces) return 9049;
    covered = GRAPHICS_SCALAR_RECOVERY_P0;
    if (graphics_scalar_recovery_adopt(covered, GRAPHICS_SCALAR_RECOVERY_GLOBAL, false) ||
        covered != GRAPHICS_SCALAR_RECOVERY_P0) return 9050;
    if (!graphics_scalar_recovery_adopt(covered, GRAPHICS_SCALAR_RECOVERY_GLOBAL, true) ||
        covered != GRAPHICS_SCALAR_RECOVERY_ALL) return 9051;
    if (graphics_scalar_recovery_adopt(covered, 4, true) || covered != GRAPHICS_SCALAR_RECOVERY_ALL) return 9052;
    if (!graphics_scalar_recovery_adopt(covered, 0, false) || covered != GRAPHICS_SCALAR_RECOVERY_ALL) return 9053;
    return 0;
}

static int run_precise_graphics_tests() {
    if (const int failed = run_graphics_recovery_marker_tests()) return failed;
    initialize(false); preciseReadback = true;
    g_nvml_api.getGpcClkVfOffset = g_nvml_api.getMemClkVfOffset;
    g_nvml_api.setGpcClkVfOffset = g_nvml_api.setMemClkVfOffset;
    if (!nvapi_set_gpu_offset_precise(247000000, CLOCK_OFFSET_SURFACE_P0, false) || p0HardwareMHz != 247 ||
        p0Writes != 1 || writes || !g_app.readback.gpuOffset || g_app.gpuOffsetUncertainSurface ||
        g_app.gpuOffsetOwnedSurface != CLOCK_OFFSET_SURFACE_P0 || g_app.gpuClockOffsetkHz != 247000) return 8843;
    if (!nvapi_set_gpu_offset_precise(247000000, CLOCK_OFFSET_SURFACE_P0, false) || p0Writes != 1) return 8844;
    if (!nvapi_set_gpu_offset_precise(-247000000, CLOCK_OFFSET_SURFACE_GLOBAL, false) ||
        p0Writes != 2 || p0HardwareMHz || writes != 1 || hardwareMHz != -247 ||
        g_app.gpuOffsetOwnedSurface != CLOCK_OFFSET_SURFACE_GLOBAL) return 8845;
    if (nvapi_set_gpu_offset_precise(-247000500, CLOCK_OFFSET_SURFACE_GLOBAL, false) || writes != 1 || p0Writes != 2 ||
        g_app.gpuOffsetOwnedSurface != CLOCK_OFFSET_SURFACE_GLOBAL) return 8846;
    if (!nvapi_reset_graphics_scalar_controls() || hardwareMHz || g_app.gpuOffsetOwnedSurface ||
        g_app.gpuOffsetUncertainSurface || graphicsLegacyWrites || writes != 2) return 8847;
    initialize(false); p0SetFails = true;
    if (nvapi_set_gpu_offset_precise(247000000, CLOCK_OFFSET_SURFACE_P0, false) ||
        p0Writes != 1 || p0HardwareMHz != 247 || writes || g_app.readback.gpuOffset ||
        g_app.gpuOffsetUncertainSurface != CLOCK_OFFSET_SURFACE_P0) return 8848;
    DesiredSettings fan{}, repair{}; fan.hasFan = 1; repair.hasGpuOffset = 1;
    repair.authoring.graphicsPrecise = 1; repair.authoring.graphicsSurface = DESIRED_GRAPHICS_P0;
    char why[256]{};
    if (apply_write_uncertainty_precheck(&fan, why, sizeof(why)) || !std::strstr(why, "graphics-offset") ||
        !apply_write_uncertainty_precheck(&repair, why, sizeof(why))) return 8849;
    auto recovered = rollback_to_safe_defaults();
    if (recovered.gpuOffset.verified || recovered.curve.attempted || releases ||
        g_app.gpuOffsetUncertainSurface != CLOCK_OFFSET_SURFACE_P0 || graphicsLegacyWrites) return 8850;
    p0SetFails = false; recovered = rollback_to_safe_defaults();
    if (!recovered.gpuOffset.verified || !recovered.curve.verified || !recovered.restrictionReleased ||
        p0HardwareMHz || g_app.gpuOffsetUncertainSurface || graphicsLegacyWrites || releases != 1) return 8851;
    initialize(false); p0ConservativeRange = true;
    if (nvapi_set_gpu_offset_precise(247000000, CLOCK_OFFSET_SURFACE_P0, false) ||
        p0Writes || g_app.gpuOffsetUncertainSurface) return 8852;
    initialize(false); preciseReadback = true;
    g_nvml_api.getGpcClkVfOffset = g_nvml_api.getMemClkVfOffset;
    g_nvml_api.setGpcClkVfOffset = g_nvml_api.setMemClkVfOffset;
    g_app.gpuOffsetOwnedSurface = CLOCK_OFFSET_SURFACE_P0;
    g_nvml_api.getClockOffsets = nullptr;
    if (nvapi_set_gpu_offset_precise(247000000, CLOCK_OFFSET_SURFACE_GLOBAL, false) || p0Writes || writes ||
        g_app.gpuOffsetOwnedSurface != CLOCK_OFFSET_SURFACE_P0) return 8853;
    g_app.gpuOffsetUncertainSurface = CLOCK_OFFSET_SURFACE_P0;
    if (nvapi_set_gpu_offset_precise(247000000, CLOCK_OFFSET_SURFACE_GLOBAL, false) || p0Writes || writes) return 8854;
    g_app.gpuOffsetRecoverySurfaces = GRAPHICS_SCALAR_RECOVERY_ALL;
    reset_selected_gpu_write_evidence(&g_app);
    if (g_app.gpuOffsetOwnedSurface || g_app.gpuOffsetUncertainSurface || g_app.gpuOffsetRecoverySurfaces) return 8855;
    initialize(false); p0ReadFailsAfterSet = true;
    if (nvapi_set_gpu_offset_precise(247000000, CLOCK_OFFSET_SURFACE_P0, false) || p0Writes != 1 ||
        g_app.gpuOffsetUncertainSurface != CLOCK_OFFSET_SURFACE_P0 || g_app.readback.gpuOffset) return 8856;
    p0ReadFailsAfterSet = false;
    if (!nvapi_reset_graphics_scalar_controls() || p0HardwareMHz || p0Writes != 2 ||
        g_app.gpuOffsetUncertainSurface || graphicsLegacyWrites) return 8857;
    initialize(false); preciseReadback = setterLeavesUnchanged = true;
    g_nvml_api.getGpcClkVfOffset = g_nvml_api.getMemClkVfOffset;
    g_nvml_api.setGpcClkVfOffset = g_nvml_api.setMemClkVfOffset;
    if (nvapi_set_gpu_offset_precise(247000000, CLOCK_OFFSET_SURFACE_GLOBAL, false) || hardwareMHz ||
        writes != 1 || g_app.gpuOffsetUncertainSurface != CLOCK_OFFSET_SURFACE_GLOBAL) return 8858;
    if (!nvapi_reset_graphics_scalar_controls() || writes != 2 || g_app.gpuOffsetUncertainSurface) return 8859;
    initialize(false); p0BadMetadata = true;
    if (nvapi_set_gpu_offset_precise(247000000, CLOCK_OFFSET_SURFACE_P0, false) || p0Writes ||
        g_app.gpuOffsetUncertainSurface) return 8860;
    initialize(false); preciseReadback = true;
    ScalarCurveSubmissionReview review{}; review.beforeValid = true; review.beforeHz = -1000000;
    if (nvapi_set_gpu_offset_precise_checked(247000000, CLOCK_OFFSET_SURFACE_P0, false, &review) ||
        !review.refusedWithoutWrite || p0Writes || writes || g_app.gpuOffsetUncertainSurface) return 8974;
    review.beforeHz = 0;
    if (!nvapi_set_gpu_offset_precise_checked(247000000, CLOCK_OFFSET_SURFACE_P0, false, &review) ||
        review.refusedWithoutWrite || p0Writes != 1 || writes) return 8975;
    if (nvapi_set_gpu_offset_precise_checked(100000000, CLOCK_OFFSET_SURFACE_P0, false, &review) ||
        !review.refusedWithoutWrite || p0Writes != 1 || writes ||
        g_app.gpuOffsetOwnedSurface != CLOCK_OFFSET_SURFACE_P0 || g_app.gpuOffsetUncertainSurface) return 8976;
    initialize(false); preciseReadback = true; durableGraphicsStoreFails = true;
    review = {};
    if (nvapi_set_gpu_offset_precise_checked(100000000, CLOCK_OFFSET_SURFACE_P0, false, &review) ||
        !review.refusedWithoutWrite || p0Writes || writes || durableGraphicsStores != 1 ||
        durableGraphicsSurfaces || g_app.gpuOffsetRecoverySurfaces || g_app.gpuOffsetUncertainSurface) return 9043;
    durableGraphicsStoreFails = false;
    if (!nvapi_set_gpu_offset_precise(0, CLOCK_OFFSET_SURFACE_P0, false) || p0Writes ||
        g_app.gpuOffsetOwnedSurface != CLOCK_OFFSET_SURFACE_P0 ||
        g_app.gpuOffsetRecoverySurfaces != GRAPHICS_SCALAR_RECOVERY_P0 ||
        durableGraphicsSurfaces != GRAPHICS_SCALAR_RECOVERY_P0 || durableGraphicsStores != 2) return 9044;
    if (!nvapi_reset_graphics_scalar_controls() || p0Writes != 1 || g_app.gpuOffsetOwnedSurface ||
        g_app.gpuOffsetUncertainSurface || g_app.gpuOffsetRecoverySurfaces) return 9045;
    initialize(false); preciseReadback = true;
    // Simulate a fresh process carrying only the decoded durable coverage.
    g_nvml_api.getGpcClkVfOffset = g_nvml_api.getMemClkVfOffset;
    g_nvml_api.setGpcClkVfOffset = g_nvml_api.setMemClkVfOffset;
    p0HardwareMHz = 125; hardwareMHz = -25;
    durableGraphicsSurfaces = g_app.gpuOffsetRecoverySurfaces = GRAPHICS_SCALAR_RECOVERY_ALL;
    if (!nvapi_reset_graphics_scalar_controls() || p0Writes != 1 || writes != 1 ||
        p0HardwareMHz || hardwareMHz || g_app.gpuOffsetRecoverySurfaces || g_app.gpuOffsetOwnedSurface ||
        g_app.gpuOffsetUncertainSurface || durableGraphicsStores) return 9046;
    p0HardwareMHz = 125; hardwareMHz = -25; p0Writes = writes = 0; p0SetFails = true;
    g_app.gpuOffsetRecoverySurfaces = GRAPHICS_SCALAR_RECOVERY_ALL;
    if (nvapi_reset_graphics_scalar_controls() || p0Writes != 1 || writes ||
        g_app.gpuOffsetRecoverySurfaces != GRAPHICS_SCALAR_RECOVERY_ALL) return 9047;
    p0SetFails = false;
    if (!nvapi_reset_graphics_scalar_controls() || p0Writes != 2 || writes != 1 ||
        p0HardwareMHz || hardwareMHz || g_app.gpuOffsetRecoverySurfaces) return 9048;
    initialize(false);
    DesiredSettings unrelated{}, sameSurface{};
    unrelated.hasFan = 1; sameSurface.hasGpuOffset = 1;
    sameSurface.authoring.graphicsPrecise = 1; sameSurface.authoring.graphicsSurface = DESIRED_GRAPHICS_P0;
    g_app.gpuOffsetRecoverySurfaces = GRAPHICS_SCALAR_RECOVERY_P0;
    if (apply_write_uncertainty_precheck(&unrelated, why, sizeof(why)) || p0Writes || writes) return 9054;
    if (!apply_write_uncertainty_precheck(&sameSurface, why, sizeof(why))) return 9055;
    g_app.gpuOffsetRecoverySurfaces = GRAPHICS_SCALAR_RECOVERY_ALL;
    if (apply_write_uncertainty_precheck(&sameSurface, why, sizeof(why))) return 9056;
    g_app.gpuOffsetOwnedSurface = CLOCK_OFFSET_SURFACE_GLOBAL;
    if (!apply_write_uncertainty_precheck(&sameSurface, why, sizeof(why))) return 9057;
    g_app.gpuOffsetOwnedSurface = CLOCK_OFFSET_SURFACE_P0;
    if (apply_write_uncertainty_precheck(&sameSurface, why, sizeof(why))) return 9058;
    g_app.gpuOffsetRecoverySurfaces = 4;
    if (apply_write_uncertainty_precheck(&sameSurface, why, sizeof(why)) || p0Writes || writes) return 9059;
    return 0;
}

static int run_precise_memory_tests() {
    initialize(false);
    preciseReadback = true; preciseSurface = CLOCK_OFFSET_SURFACE_P0;
    hardwareMHz = 888;
    if (!nvapi_set_mem_offset_precise(500, false) || p0HardwareMHz != 1 ||
        p0Writes != 1 || writes || hardwareMHz != 888) return 8200;
    initialize(false);
    preciseReadback = true; preciseSurface = CLOCK_OFFSET_SURFACE_P0; p0SetFails = true;
    if (nvapi_set_mem_offset_precise(500, false) || p0Writes != 1 || writes ||
        !g_app.memOffsetWriteUncertain || g_app.readback.memOffset) return 8201;
    initialize(false);
    preciseReadback = true; preciseSurface = CLOCK_OFFSET_SURFACE_P0; p0ReadFails = true;
    if (nvapi_set_mem_offset_precise(500, false) || p0Writes || writes) return 8202;
    initialize(false);
    preciseReadback = true; preciseSurface = CLOCK_OFFSET_SURFACE_P0; p0BadMetadata = true;
    if (nvapi_set_mem_offset_precise(500, false) || p0Writes || writes) return 8203;
    initialize(false);
    preciseReadback = true; preciseSurface = CLOCK_OFFSET_SURFACE_P0;
    g_nvml_api.setClockOffsets = nullptr;
    if (nvapi_set_mem_offset_precise(500, false) || p0Writes || writes) return 8204;
    initialize(false);
    preciseReadback = true; preciseSurface = CLOCK_OFFSET_SURFACE_P0; p0ConservativeRange = true;
    if (!nvapi_set_mem_offset_precise(500, false) || p0Writes != 1 || writes) return 8205;
    initialize(false);
    preciseReadback = true;
    if (!nvapi_set_mem_offset_precise(500, false) || hardwareMHz != 1 ||
        g_app.memClockOffsetkHz != 500 || writes != 1) return 8080;
    if (!nvapi_set_mem_offset_precise(-500, false) || hardwareMHz != -1 ||
        g_app.memClockOffsetkHz != -500 || writes != 2) return 8081;
    if (nvapi_set_mem_offset_precise(501, false) || writes != 2) return 8082;
    initialize(false);
    if (nvapi_set_mem_offset_precise(500, false) || writes) return 8083;
    initialize(false);
    preciseReadback = setterLeavesUnchanged = setterClaimsMatch = true;
    if (nvapi_set_mem_offset_precise(500, false) || writes != 1 ||
        !g_app.memOffsetWriteUncertain) return 8084;
    initialize(false);
    preciseReadback = setterLeavesUnchanged = setterClaimsMatch = true;
    hardwareMHz = 2999;
    if (nvapi_set_mem_offset_precise(1500000, false) || writes != 1 ||
        hardwareMHz != 2999) return 8085;
    int value = 0;
    if (!memory_offset_effective_mhz_from_hz(123500000, &value) || value != 247 ||
        memory_offset_hz_from_effective_mhz(value) != 123500000 ||
        memory_offset_effective_mhz_from_hz(LLONG_MIN, &value) ||
        memory_offset_khz_from_effective_mhz(INT_MAX, &value)) return 8086;
    return 0;
}
static int run_pstate_evidence_tests() {
    // A graphics-only Pstates response cannot certify cached stock memory.
    initialize(false);
    pstateReadSucceeds = setterLeavesUnchanged = true;
    g_app.nvmlMemMaxMHz = 10000;
    hardwareMHz = 3000;
    if (nvapi_set_mem_offset(0, true) || !g_app.memOffsetWriteUncertain ||
        g_app.readback.memOffset || hardwareMHz != 3000 || writes != 1)
        return 7640;
    const auto recovery = rollback_to_safe_defaults();
    if (!apply_recovery_domain_is_uncertain(recovery.memory) ||
        !g_app.memOffsetWriteUncertain || hardwareMHz != 3000 || writes != 4)
        return 7641;

    // Real memory evidence still verifies without NVML's offset getters.
    initialize(false);
    pstateReadSucceeds = pstateMemoryPresent = true;
    g_app.nvmlMemMaxMHz = 10000;
    if (!nvapi_set_mem_offset(1500000, true) || g_app.memOffsetWriteUncertain ||
        !g_app.readback.memOffset || g_app.memClockOffsetkHz != 1500000)
        return 7642;
    if (!nvapi_set_mem_offset(0, true) || g_app.memOffsetWriteUncertain || hardwareMHz)
        return 7643;

    // A fresh contradictory memory clock and an absent stock denominator
    // must both fail verification rather than reuse cached equality.
    setterLeavesUnchanged = true;
    hardwareMHz = 3000;
    if (nvapi_set_mem_offset(0, true) || !g_app.memOffsetWriteUncertain ||
        g_app.readback.memOffset) return 7644;
    initialize(false);
    pstateReadSucceeds = pstateMemoryPresent = true;
    if (nvapi_set_mem_offset(0, true) || !g_app.memOffsetWriteUncertain ||
        g_app.readback.memOffset) return 7645;
    return 0;
}
// A GPU switch must not carry the previous GPU's evidence. An uncertain flag
// would force a write on the new GPU; a stale valid readback would let the
// setter skip a write it never verified there.
static int run_gpu_selection_evidence_tests() {
    initialize(false);
    nvapi_set_mem_offset(1500000, false);
    g_app.powerLimitWriteUncertain = true;
    g_app.powerLimitSurfaceProven = true;
    g_app.powerLimitRecoveryDefaultmW = 250000;
    g_app.readback.powerLimit = true;
    if (!g_app.memOffsetWriteUncertain) return 7660;
    reset_selected_gpu_write_evidence(&g_app);
    if (g_app.memOffsetWriteUncertain || g_app.powerLimitWriteUncertain ||
        g_app.powerLimitSurfaceProven || g_app.powerLimitRecoveryDefaultmW ||
        g_app.readback.memOffset || g_app.readback.powerLimit ||
        g_app.readback.gpuOffset || g_app.readback.pstate ||
        g_app.powerLimitPct != POWER_LIMIT_DEFAULT_PCT) return 7661;
    const auto recovery = rollback_to_safe_defaults();
    if (recovery.memory.attempted) return 7662;

    // The previous GPU read 1500 MHz with fresh evidence; the new GPU has not
    // been read, so the same target is written rather than skipped.
    initialize(true);
    g_app.memClockOffsetkHz = 1500000;
    g_app.readback.memOffset = true;
    reset_selected_gpu_write_evidence(&g_app);
    if (nvapi_set_mem_offset(1500000, false) || writes != 1) return 7663;

    // The Apply planner decides before the setter runs. After a switch whose
    // memory reads failed, a target equal to the previous GPU's cached offset
    // must still be written, whether or not the previous write was verified.
    for (int uncertain = 0; uncertain < 2; ++uncertain) {
        initialize(false);
        g_app.memClockOffsetkHz = 1500000;
        g_app.memOffsetWriteUncertain = uncertain != 0;
        reset_selected_gpu_write_evidence(&g_app);
        if (!apply_plan_mem_offset_write(g_app, 1500000)) return 7676;
    }
    // The production plan skips once this GPU's fresh reading matches.
    initialize(true);
    g_app.memClockOffsetkHz = 1500000;
    if (apply_plan_mem_offset_write(g_app, 1500000)) return 7678;
    // Fresh evidence of this GPU still lets an identical target skip, and a
    // different or uncertain target is written.
    if (mem_offset_write_required(false, true, 1500000, 1500000) ||
        !mem_offset_write_required(false, true, 1500000, 0) ||
        !mem_offset_write_required(true, true, 1500000, 1500000)) return 7677;
    return 0;
}

// An Apply that does not own an unverified domain cannot repair it, so it is
// refused before any write; one that owns every unverified domain proceeds.
static int run_uncertainty_precheck_tests() {
    char detail[256] = {};
    DesiredSettings fan = {};
    fan.hasFan = true;
    DesiredSettings memory = {};
    memory.hasMemOffset = true;
    DesiredSettings both = memory;
    both.hasPowerLimit = true;
    initialize(false);
    if (!apply_write_uncertainty_precheck(&fan, detail, sizeof(detail))) return 7664;
    nvapi_set_mem_offset(1500000, false);
    if (!g_app.memOffsetWriteUncertain ||
        apply_write_uncertainty_precheck(&fan, detail, sizeof(detail)) ||
        !std::strstr(detail, "memory-offset") || std::strstr(detail, "power"))
        return 7665;
    if (!apply_write_uncertainty_precheck(&memory, detail, sizeof(detail))) return 7666;
    g_app.powerLimitWriteUncertain = true;
    detail[0] = 0;
    if (apply_write_uncertainty_precheck(&memory, detail, sizeof(detail)) ||
        !std::strstr(detail, "power-limit") || std::strstr(detail, "memory"))
        return 7667;
    detail[0] = 0;
    if (apply_write_uncertainty_precheck(&fan, detail, sizeof(detail)) ||
        !std::strstr(detail, "memory-offset and power-limit")) return 7668;
    if (!apply_write_uncertainty_precheck(&both, detail, sizeof(detail))) return 7669;
    return 0;
}

static int run() {
    if (int r = run_gpu_selection_evidence_tests()) return r;
    if (int r = run_uncertainty_precheck_tests()) return r;
    initialize(true);
    if (nvapi_set_mem_offset(1500000, false) || !g_app.memOffsetWriteUncertain ||
        g_app.memClockOffsetkHz || hardwareMHz != 3000) return 7550;
    auto recovery = rollback_to_safe_defaults();
    if (writes != 2 || hardwareMHz || !recovery.memory.attempted ||
        !recovery.memory.verified || g_app.memOffsetWriteUncertain || releases != 1) return 7551;
    initialize(false);
    if (nvapi_set_mem_offset(1500000, false)) return 7552;
    recovery = rollback_to_safe_defaults();
    if (writes != 4 || !apply_recovery_domain_is_uncertain(recovery.memory) ||
        !g_app.memOffsetWriteUncertain || !recovery.restrictionReleased) return 7553;
    initialize(false);
    nvapi_set_mem_offset(1500000, false);
    ApplyClockCeilingGuard ceiling;
    char message[512] = {};
    apply_recover_clock_failure(ceiling, "fixture failure", message, sizeof(message));
    if (!std::strstr(message, "memory defaults") ||
        !std::strstr(message, "restored core clocks") ||
        std::strstr(message, GC_APPLY_RECOVERY_VERIFIED_TEXT)) return 7554;
    // Unverified memory must not hide that core protection is still holding.
    initialize(false);
    coreFails = true;
    nvapi_set_mem_offset(1500000, false);
    message[0] = 0;
    apply_recover_clock_failure(ceiling, "fixture failure", message, sizeof(message));
    if (!std::strstr(message, "memory defaults") ||
        !std::strstr(message, "clock protection retained") || releases ||
        std::strstr(message, GC_APPLY_RECOVERY_VERIFIED_TEXT)) return 7559;
    initialize(true);
    setterFails = true;
    if (nvapi_set_mem_offset(1500000, false) || !g_app.memOffsetWriteUncertain) return 7555;
    setterFails = false;
    recovery = rollback_to_safe_defaults();
    if (hardwareMHz || !recovery.memory.verified) return 7556;
    initialize(true);
    if (!nvapi_set_mem_offset(0, true) || writes != 1) return 7557;
    initialize(true);
    if (!nvapi_set_mem_offset(0, false) || writes) return 7558;
    return run_pstate_evidence_tests();
}
} // namespace memory_recovery_fixture

int run_memory_recovery_tests() {
    int result = memory_recovery_fixture::run_precise_graphics_tests();
    if (!result) result = memory_recovery_fixture::run_precise_memory_tests();
    if (!result) result = memory_recovery_fixture::run();
    if (result) std::fprintf(stderr, "regression assertion failed: %d\n", result);
    return result;
}
#ifdef GC_MEMORY_RECOVERY_FIXTURE_MAIN
int main() {
    const int result = run_memory_recovery_tests();
    if (!result) std::fprintf(stdout, "graphics recovery: marker migration, durable coverage and exact reset regression passed\n");
    return result;
}
#endif
