// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the production memory setter, rollback and failure reporting.
#include "app_shared.h"
#include "apply_clock_ceiling_policy.h"
#include "clock_reset_policy.h"
#include "control_readback_policy.h"
#include <cstdio>
#include <cstring>

namespace memory_recovery_fixture {
static AppData g_app;
static NvmlApi g_nvml_api;
static void* g_serviceFanThread;
static int hardwareMHz, writes, releases;
static bool verifyReset, setterFails, coreFails;
static bool pstateReadSucceeds, pstateMemoryPresent, setterLeavesUnchanged;
static void debug_log(const char*, ...) {}
static void set_last_apply_phase(const char*) {}
static void Sleep(unsigned long) {}
static bool nvml_ensure_ready() { return true; }
static const char* nvml_err_name(int) { return "fixture"; }
static bool vf_curve_global_gpu_offset_supported() { return true; }
static bool nvapi_set_gpu_offset(int, bool) { return !coreFails; }
static bool nvapi_read_curve() { g_app.curve[0].freq_kHz = 2000000; return true; }
static bool apply_curve_offsets_verified(const int*, const bool*, int) { return !coreFails; }
static bool nvml_set_clock_offset_domain(unsigned int, int value, bool* exact, char*, size_t) {
    ++writes;
    if (!setterLeavesUnchanged) hardwareMHz = value;
    *exact = verifyReset && value == 0;
    return !setterFails;
}
static bool nvml_read_clock_offsets(char*, size_t) {
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
    hardwareMHz = writes = releases = 0;
    verifyReset = canVerifyReset;
    setterFails = coreFails = false;
    pstateReadSucceeds = pstateMemoryPresent = setterLeavesUnchanged = false;
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
    int result = memory_recovery_fixture::run();
    if (result) std::fprintf(stderr, "regression assertion failed: %d\n", result);
    return result;
}
#ifdef GC_MEMORY_RECOVERY_FIXTURE_MAIN
int main() { return run_memory_recovery_tests(); }
#endif
