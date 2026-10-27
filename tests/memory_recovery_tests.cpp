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
    hardwareMHz = value;
    *exact = verifyReset && value == 0;
    return !setterFails;
}
static bool nvml_read_clock_offsets(char*, size_t) {
    if (!verifyReset || hardwareMHz != 0) return false;
    g_app.memClockOffsetkHz = 0;
    g_app.readback.memOffset = true;
    return true;
}
static bool nvapi_read_pstates() { return false; }
static void detect_clock_offsets() {}
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
}
static int run() {
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
    return 0;
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
