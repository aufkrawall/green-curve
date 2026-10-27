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
static bool pciLocationValid = true, setterFails = false;
static int fallbackLaunches = 0;
static bool launchSawUncertainty = false;

static void debug_log(const char*, ...) {}
static void set_last_apply_phase(const char*) {}
static void Sleep(unsigned long) {}
static bool nvml_ensure_ready() { return true; }
static const char* nvml_err_name(int) { return "fixture"; }
static bool gpu_adapter_has_valid_pci_location(const GpuAdapterInfo*) {
    return pciLocationValid;
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
    return setterFails ? NVML_ERROR_UNKNOWN : NVML_SUCCESS;
}

// No fixture launches an external process. A failed launch proves whether
// the production setter restores the state from before that attempt.
static BOOL WINAPI fixture_create_process(LPCWSTR, LPWSTR,
    LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
    LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) {
    ++fallbackLaunches;
    launchSawUncertainty = g_app.powerLimitWriteUncertain && !g_app.readback.powerLimit;
    SetLastError(ERROR_FILE_NOT_FOUND);
    return FALSE;
}
#define CreateProcessW fixture_create_process
#include "gpu_backend_power.cpp"
#undef CreateProcessW

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

struct ApplyClockCeilingGuard {
    struct { bool required = false; } plan;
    bool armed = false;
    void arm() {}
    bool must_refuse_transition() const { return false; }
    void retain(const char*) {}
    void adopt(const char*) {}
};
static void refresh_global_state(char*, size_t) {}
#include "gpu_backend_apply_failure.h"

static void initialize() {
    g_app = {};
    g_nvml_api = {};
    hardwareCurrentmW = hardwareDefaultmW = 250000;
    hardwareMinmW = 100000;
    hardwareMaxmW = 300000;
    powerWrites = 0;
    failCurrentReads = failDefaultReads = failAfterWrite = 0;
    smiAvailable = false;
    pciLocationValid = true;
    setterFails = launchSawUncertainty = false;
    fallbackLaunches = 0;
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

static int run_write_boundary_tests() {
    DesiredSettings fan = {};
    fan.hasFan = true;
    char detail[256] = {};
    initialize();
    if (!nvml_read_power_limit()) return 7650;
    g_nvml_api.setPowerLimit = nullptr;
    if (nvapi_set_power_limit(80, false) || powerWrites || fallbackLaunches ||
        g_app.powerLimitWriteUncertain || !g_app.readback.powerLimit ||
        !apply_power_uncertainty_precheck(&fan, detail, sizeof(detail))) return 7650;
    if (rollback_to_safe_defaults().power.attempted) return 7651;

    // A fallback that cannot identify its GPU has no authority to write.
    smiAvailable = true;
    pciLocationValid = false;
    if (nvapi_set_power_limit(80, false) || powerWrites || fallbackLaunches ||
        g_app.powerLimitWriteUncertain || !g_app.readback.powerLimit) return 7652;

    // Failure to create the process preserves a previously verified state.
    pciLocationValid = true;
    if (nvapi_set_power_limit(80, false) || fallbackLaunches != 1 ||
        !launchSawUncertainty || g_app.powerLimitWriteUncertain ||
        !g_app.readback.powerLimit || hardwareCurrentmW != 250000) return 7653;

    // A failed repair cannot clear uncertainty inherited from an earlier write.
    g_app.powerLimitWriteUncertain = true;
    g_app.readback.powerLimit = false;
    if (nvapi_set_power_limit(100, true) || fallbackLaunches != 2 ||
        !g_app.powerLimitWriteUncertain || g_app.readback.powerLimit ||
        apply_power_uncertainty_precheck(&fan, detail, sizeof(detail))) return 7654;
    smiAvailable = false;
    if (nvapi_set_power_limit(100, true) || !g_app.powerLimitWriteUncertain ||
        g_app.readback.powerLimit) return 7655;

    // A real NVML attempt remains uncertain even if fallback cannot start.
    initialize();
    if (!nvml_read_power_limit()) return 7656;
    setterFails = true;
    if (nvapi_set_power_limit(80, false) || powerWrites != 1 ||
        hardwareCurrentmW != 200000 || !g_app.powerLimitWriteUncertain ||
        g_app.readback.powerLimit) return 7656;
    smiAvailable = true;
    if (nvapi_set_power_limit(80, true) || powerWrites != 2 ||
        fallbackLaunches != 1 || !launchSawUncertainty ||
        !g_app.powerLimitWriteUncertain || g_app.readback.powerLimit) return 7657;
    return 0;
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
    return run_write_boundary_tests();
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
