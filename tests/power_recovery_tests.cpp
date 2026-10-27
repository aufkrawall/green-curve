// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the production power setter and stock recovery with a scripted
// driver that can mutate before post-write verification fails.
#include "app_shared.h"

#if defined(_WIN32)
#include "win32_raii.h"

#include <cstdio>
#include <cstring>

namespace power_recovery_fixture {
static AppData g_app = {};
static NvmlApi g_nvml_api = {};
static void* g_serviceFanThread = nullptr;

static unsigned int hardwareCurrentmW = 0;
static unsigned int hardwareDefaultmW = 0;
static unsigned int hardwareMinmW = 0;
static unsigned int hardwareMaxmW = 0;
static int powerWrites = 0;
static int failCurrentReads = 0;
static int failDefaultReads = 0;
static int failAfterWrite = 0;
static bool smiAvailable = false;

static void debug_log(const char*, ...) {}
static void set_last_apply_phase(const char*) {}
static void Sleep(unsigned long) {}
static bool nvml_ensure_ready() { return true; }
static const char* nvml_err_name(int) { return "fixture"; }
static bool gpu_adapter_has_valid_pci_location(const GpuAdapterInfo*) {
    return true;
}
static bool find_trusted_nvidia_smi_path_w(WCHAR* out, size_t outCount) {
    if (!smiAvailable || !out || outCount == 0) return false;
    StringCchCopyW(out, outCount, L"fixture-nvidia-smi.exe");
    return true;
}
static nvmlReturn_t get_constraints(nvmlDevice_t, unsigned int* mn,
                                    unsigned int* mx) {
    *mn = hardwareMinmW;
    *mx = hardwareMaxmW;
    return NVML_SUCCESS;
}
static nvmlReturn_t get_current(nvmlDevice_t, unsigned int* value) {
    if (failCurrentReads > 0) {
        --failCurrentReads;
        return NVML_ERROR_UNKNOWN;
    }
    *value = hardwareCurrentmW;
    return NVML_SUCCESS;
}
static nvmlReturn_t get_default(nvmlDevice_t, unsigned int* value) {
    if (failDefaultReads > 0) {
        --failDefaultReads;
        return NVML_ERROR_UNKNOWN;
    }
    *value = hardwareDefaultmW;
    return NVML_SUCCESS;
}
static nvmlReturn_t set_power(nvmlDevice_t, unsigned int value) {
    ++powerWrites;
    hardwareCurrentmW = value;
    if (powerWrites == failAfterWrite) {
        failCurrentReads = 1;
        failDefaultReads = 1;
    }
    return NVML_SUCCESS;
}

#include "gpu_backend_power.cpp"

static bool vf_curve_global_gpu_offset_supported() { return true; }
static bool nvapi_set_gpu_offset(int, bool = false) { return true; }
static bool nvapi_set_mem_offset(int, bool = false) { return true; }
static bool nvapi_read_curve() {
    g_app.curve[0].freq_kHz = 2000000;
    return true;
}
static bool apply_curve_offsets_verified(const int*, const bool*, int) {
    return true;
}
static void stop_fan_curve_runtime() {}
static bool app_is_service_process() { return true; }
static void stop_service_fan_runtime_thread() {}
static bool nvml_set_fan_auto(char*, size_t) { return true; }
static void clear_runtime_selective_gpu_offset_request() {}
static nvmlReturn_t release_locked_clocks(nvmlDevice_t) {
    return NVML_SUCCESS;
}

#include "clock_reset_policy.h"
#include "main_gpu_rollback.h"

static void initialize() {
    g_app = {};
    g_nvml_api = {};
    hardwareCurrentmW = hardwareDefaultmW = 250000;
    hardwareMinmW = 100000;
    hardwareMaxmW = 300000;
    powerWrites = 0;
    failCurrentReads = failDefaultReads = failAfterWrite = 0;
    smiAvailable = false;
    g_app.fanIsAuto = true;
    g_app.activeFanMode = FAN_MODE_AUTO;
    g_app.selectedGpu.valid = true;
    g_app.selectedGpu.pciInfoValid = true;
    g_app.selectedGpu.pciBus = 1;
    g_nvml_api.getPowerConstraints = get_constraints;
    g_nvml_api.getPowerLimit = get_current;
    g_nvml_api.getPowerDefaultLimit = get_default;
    g_nvml_api.setPowerLimit = set_power;
    g_nvml_api.resetGpuLockedClocks = release_locked_clocks;
}

static int run() {
    initialize();
    if (!nvml_read_power_limit() || !g_app.powerLimitSurfaceProven ||
        g_app.powerLimitRecoveryDefaultmW != 250000)
        return 7621;

    // The write reaches the board, but both post-write getters fail. The
    // cached percentage is published as unknown and must not hide the effect.
    failAfterWrite = 1;
    if (nvapi_set_power_limit(80, false) ||
        !g_app.powerLimitWriteUncertain ||
        g_app.readback.powerLimit ||
        hardwareCurrentmW != 200000 ||
        powerWrites != 1)
        return 7622;

    ApplyRecoveryResult recovery = rollback_to_safe_defaults();
    if (!recovery.power.attempted || !recovery.power.verified ||
        g_app.powerLimitWriteUncertain ||
        hardwareCurrentmW != 250000 ||
        powerWrites != 2)
        return 7623;

    // A forced repair cannot invent a denominator after losing all surface
    // evidence.
    initialize();
    g_app.readback.powerLimit = false;
    g_app.powerLimitDefaultmW = 0;
    g_app.powerLimitCurrentmW = 0;
    g_app.powerLimitRecoveryDefaultmW = 0;
    g_app.powerLimitSurfaceProven = false;
    g_app.powerLimitWriteUncertain = true;
    if (nvapi_set_power_limit(100, true) || powerWrites != 0)
        return 7624;

    // A forced write may use the cached default, but still respects the last
    // complete board constraints.
    initialize();
    if (!nvml_read_power_limit()) return 7625;
    g_app.readback.powerLimit = false;
    g_app.powerLimitCurrentmW = 0;
    g_app.powerLimitDefaultmW = 0;
    g_app.powerLimitWriteUncertain = true;
    if (nvapi_set_power_limit(150, true) || powerWrites != 0)
        return 7626;
    return 0;
}
} // namespace power_recovery_fixture

int run_power_recovery_tests() {
    int result = power_recovery_fixture::run();
    if (result) std::fprintf(stderr, "regression assertion failed: %d\n", result);
    return result;
}
#ifdef GC_POWER_RECOVERY_FIXTURE_MAIN
int main() { return run_power_recovery_tests(); }
#endif

#else
int run_power_recovery_tests() { return 0; }
#endif
