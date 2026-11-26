// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute the production driver context and backend initialization/teardown
// against a reference-counted fake NVML library (assertion codes 14200-14215,
// 14217-14219, 14235-14249, 14251-14257; 14247-14249, 14251-14252, 14255-14257
// in linux_driver_discovery_cases.h).
// No real driver library or physical GPU is opened.
#if !defined(_WIN32)
#include "platform.h"
#include <cstdio>
#include <cstring>

static PlLib lifetime_open_library(int);
static void* lifetime_library_symbol(PlLib, const char*);
static void lifetime_close_library(PlLib);

// Rename the backend's public symbols so this fixture can coexist with other
// production-boundary fixtures without replacing their implementations.
#define linux_backend_init lifetime_backend_init
#define linux_driver_enumerate_nvapi lifetime_driver_enumerate_nvapi
#define linux_driver_open_nvml lifetime_driver_open_nvml
#define linux_driver_open_nvapi lifetime_driver_open_nvapi
#define linux_driver_discover lifetime_driver_discover
#define linux_driver_close lifetime_driver_close
#define linux_backend_capture_current lifetime_backend_capture_current
#define linux_backend_capture_apply_snapshot lifetime_backend_capture_apply_snapshot
#define linux_backend_capture_reset_snapshot lifetime_backend_capture_reset_snapshot
#define linux_backend_fan_stall_sample lifetime_backend_fan_stall_sample
#define linux_backend_shutdown lifetime_backend_shutdown
#define linux_backend_select_target lifetime_backend_select_target
#define linux_backend_capture_snapshot lifetime_backend_capture_snapshot
#define linux_backend_restore_snapshot lifetime_backend_restore_snapshot
#define linux_backend_refresh lifetime_backend_refresh
#define linux_backend_curve_plausible lifetime_backend_curve_plausible
#define linux_backend_self_test lifetime_backend_self_test
#define linux_backend_clk_domain_dump lifetime_backend_clk_domain_dump
#define linux_backend_apply lifetime_backend_apply
#define linux_backend_reset lifetime_backend_reset
#define linux_backend_set_curve_fan_percent lifetime_backend_set_curve_fan_percent
#define linux_backend_set_fan_auto lifetime_backend_set_fan_auto
#define linux_backend_fixed_fan_check lifetime_backend_fixed_fan_check
#define linux_backend_fans_are_auto lifetime_backend_fans_are_auto
#define linux_platform_is_integrated_soc lifetime_platform_is_integrated_soc
#include "linux_backend.h"
#define pl_open_driver_library lifetime_open_library
#define pl_lib_sym lifetime_library_symbol
#define pl_lib_close lifetime_close_library
#include "linux_backend.cpp"
#undef pl_lib_close
#undef pl_lib_sym
#undef pl_open_driver_library
#undef linux_platform_is_integrated_soc
#undef linux_backend_fans_are_auto
#undef linux_backend_fixed_fan_check
#undef linux_backend_set_fan_auto
#undef linux_backend_set_curve_fan_percent
#undef linux_backend_reset
#undef linux_backend_apply
#undef linux_backend_clk_domain_dump
#undef linux_backend_self_test
#undef linux_backend_curve_plausible
#undef linux_backend_refresh
#undef linux_backend_restore_snapshot
#undef linux_backend_capture_snapshot
#undef linux_backend_select_target
#undef linux_backend_shutdown
#undef linux_backend_fan_stall_sample
#undef linux_backend_capture_reset_snapshot
#undef linux_backend_capture_apply_snapshot
#undef linux_backend_capture_current
#undef linux_driver_close
#undef linux_driver_discover
#undef linux_driver_open_nvapi
#undef linux_driver_open_nvml
#undef linux_driver_enumerate_nvapi
#undef linux_backend_init

namespace {
struct LibraryLifetime {
    int references, initCalls, shutdownCalls, openCalls, closeCalls;
    int nvmlCloses, nvapiCloses;
    int countCalls, nvapiEnumerations, nvapiInits;
    bool refuseInit, refuseCount, hideShutdown;
    // provideNvapi: a minimal NvAPI with one handle. providePci: NVML exposes
    // the PCI query. loseInit: one driver reset: the next NVML call finds
    // every reference dropped and NVML reports itself uninitialized.
    bool provideNvapi, providePci, loseInit, refuseNvapiEnum, refuseNvapiInit;
    // A driver reset invalidated the NvAPI session until it is initialized again.
    bool nvapiSessionLost;
};
static LibraryLifetime library;

static nvmlReturn_t fake_init() {
    ++library.initCalls;
    if (library.refuseInit) return NVML_ERROR_UNKNOWN;
    ++library.references;
    return NVML_SUCCESS;
}

static nvmlReturn_t fake_shutdown() {
    ++library.shutdownCalls;
    if (library.references <= 0) return NVML_ERROR_UNINITIALIZED;
    --library.references;
    return NVML_SUCCESS;
}

static nvmlReturn_t fake_count(unsigned int* count) {
    ++library.countCalls;
    if (library.loseInit) {
        library.loseInit = false;
        library.references = 0;
    }
    if (library.references <= 0) return NVML_ERROR_UNINITIALIZED;
    if (library.refuseCount) return NVML_ERROR_UNKNOWN;
    *count = 1;
    return NVML_SUCCESS;
}

static nvmlReturn_t fake_handle(unsigned int index, nvmlDevice_t* device) {
    if (library.references <= 0) return NVML_ERROR_UNINITIALIZED;
    if (index != 0) return NVML_ERROR_NOT_FOUND;
    *device = reinterpret_cast<nvmlDevice_t>(1);
    return NVML_SUCCESS;
}
}

static nvmlReturn_t fake_pci(nvmlDevice_t, nvmlPciInfo_t* pci) {
    if (library.references <= 0) return NVML_ERROR_UNINITIALIZED;
    *pci = {};
    pci->pciDeviceId = 0x1e0410deu;
    pci->bus = 1;
    return NVML_SUCCESS;
}
static int fake_nvapi_init() {
    ++library.nvapiInits;
    if (library.refuseNvapiInit) return -3;
    library.nvapiSessionLost = false;
    return 0;
}
static int fake_nvapi_enum(GPU_HANDLE* handles, int* count) {
    ++library.nvapiEnumerations;
    if (library.refuseNvapiEnum) return -1;
    if (library.nvapiSessionLost) return -4;  // NVAPI_API_NOT_INITIALIZED
    handles[0] = reinterpret_cast<GPU_HANDLE>(7);
    *count = 1;
    return 0;
}
static int fake_nvapi_bus(GPU_HANDLE, unsigned int* bus) { *bus = 1; return 0; }
static int fake_nvapi_slot(GPU_HANDLE, unsigned int* slot) { *slot = 0; return 0; }
static int fake_nvapi_pci(GPU_HANDLE, unsigned int* device, unsigned int* subsystem,
                          unsigned int* revision, unsigned int* extended) {
    *device = 0x1e0410deu;
    *subsystem = 0;
    *revision = 0;
    *extended = 0;
    return 0;
}
static void* fake_nvapi_query(unsigned int id) {
    if (id == NVAPI_INIT_ID) return reinterpret_cast<void*>(fake_nvapi_init);
    if (id == NVAPI_ENUM_GPU_ID) return reinterpret_cast<void*>(fake_nvapi_enum);
    if (id == NVAPI_GPU_GET_BUS_ID_ID) return reinterpret_cast<void*>(fake_nvapi_bus);
    if (id == NVAPI_GPU_GET_BUS_SLOT_ID_ID) return reinterpret_cast<void*>(fake_nvapi_slot);
    if (id == NVAPI_GPU_GET_PCI_IDENTIFIERS_ID) return reinterpret_cast<void*>(fake_nvapi_pci);
    return nullptr;
}

static PlLib fake_nvml_lib() { return reinterpret_cast<PlLib>(1); }
static PlLib fake_nvapi_lib() { return reinterpret_cast<PlLib>(2); }

static PlLib lifetime_open_library(int kind) {
    if (kind == PL_DRIVER_NVAPI && library.provideNvapi) {
        ++library.openCalls;
        return fake_nvapi_lib();
    }
    if (kind != PL_DRIVER_NVML) return nullptr;
    ++library.openCalls;
    return fake_nvml_lib();
}

static void* lifetime_library_symbol(PlLib lib, const char* name) {
    if (lib == fake_nvapi_lib()) {
        if (strcmp(name, "nvapi_QueryInterface") == 0)
            return library.provideNvapi ? reinterpret_cast<void*>(fake_nvapi_query) : nullptr;
        return nullptr;
    }
    if (lib != fake_nvml_lib()) return nullptr;
    if (strcmp(name, "nvmlInit_v2") == 0) return reinterpret_cast<void*>(fake_init);
    if (strcmp(name, "nvmlShutdown") == 0)
        return library.hideShutdown ? nullptr : reinterpret_cast<void*>(fake_shutdown);
    if (strcmp(name, "nvmlDeviceGetCount_v2") == 0) return reinterpret_cast<void*>(fake_count);
    if (strcmp(name, "nvmlDeviceGetPciInfo_v3") == 0)
        return library.providePci ? reinterpret_cast<void*>(fake_pci) : nullptr;
    if (strcmp(name, "nvmlDeviceGetHandleByIndex_v2") == 0) return reinterpret_cast<void*>(fake_handle);
    return nullptr;
}

static void lifetime_close_library(PlLib lib) {
    ++library.closeCalls;
    if (lib == fake_nvml_lib()) ++library.nvmlCloses;
    else if (lib == fake_nvapi_lib()) ++library.nvapiCloses;
}
bool lifetime_platform_is_integrated_soc() { return false; }

#include "linux_driver_discovery_cases.h"

int run_linux_nvml_lifetime_tests() {
    g_lbDebug = false;
    library = {};
    char err[256] = {};
    unsigned int count = 0;
    LinuxDriverContext driver = {};

    // 14200-14203: the daemon's driver-not-ready retry order. Startup init
    // fails at nvmlInit_v2; a later replacement init succeeds through the same
    // context; only then is the stale state shut down. No GPU state releases
    // the context's reference or library.
    LinuxGpuState stale = {}, replacement = {};
    library.refuseInit = true;
    if (lifetime_backend_init(&stale, &driver, nullptr, err, sizeof(err)) ||
        stale.driver != &driver || driver.nvmlInitialized || !driver.nvmlLib ||
        library.references != 0) return 14200;
    library.refuseInit = false;
    if (!lifetime_backend_init(&replacement, &driver, nullptr, err, sizeof(err)) ||
        !driver.nvmlInitialized || library.references != 1 ||
        library.initCalls != 2 || library.openCalls != 1) return 14201;
    lifetime_backend_shutdown(&stale);
    if (library.references != 1 || library.shutdownCalls != 0 ||
        library.closeCalls != 0) return 14202;
    if (replacement.nvml.getCount(&count) != NVML_SUCCESS || count != 1) return 14203;

    // 14204-14205: further GPU states bound from the context reuse its one
    // load and one init; each binding still enumerates (no behavior change
    // for a single GPU), and the device handle is per state.
    LinuxGpuState second = {}, third = {};
    if (!lifetime_backend_init(&second, &driver, nullptr, err, sizeof(err)) ||
        !lifetime_backend_init(&third, &driver, nullptr, err, sizeof(err))) return 14204;
    if (library.initCalls != 2 || library.openCalls != 1 || library.references != 1 ||
        !second.nvmlReady || !third.nvmlReady || second.driver != &driver) return 14205;

    // 14206-14207: enumeration failing after a successful init leaves the
    // context's reference in place; the failed state releases nothing.
    LinuxGpuState partial = {};
    library.refuseCount = true;
    if (lifetime_backend_init(&partial, &driver, nullptr, err, sizeof(err)) ||
        partial.nvmlReady || library.initCalls != 2 || library.references != 1) return 14206;
    lifetime_backend_shutdown(&partial);
    library.refuseCount = false;
    if (library.references != 1 || library.shutdownCalls != 0 ||
        second.nvml.getCount(&count) != NVML_SUCCESS) return 14207;

    // 14208-14209: shutting down every state releases nothing; closing the
    // context releases its one reference and library handle exactly once.
    lifetime_backend_shutdown(&replacement);
    lifetime_backend_shutdown(&second);
    lifetime_backend_shutdown(&third);
    if (library.references != 1 || library.shutdownCalls != 0 ||
        library.closeCalls != 0) return 14208;
    lifetime_driver_close(&driver);
    lifetime_driver_close(&driver);
    if (library.references != 0 || library.shutdownCalls != 1 ||
        library.closeCalls != 1 || driver.nvmlLib || driver.nvmlInitialized) return 14209;

    // 14210-14211: a library without nvmlShutdown is refused before
    // nvmlInit_v2, so no reference is acquired that close could not release.
    LinuxDriverContext noShutdown = {};
    LinuxGpuState refused = {};
    library.hideShutdown = true;
    const int initsBefore = library.initCalls;
    if (lifetime_backend_init(&refused, &noShutdown, nullptr, err, sizeof(err)) ||
        noShutdown.nvmlInitialized || library.initCalls != initsBefore) return 14210;
    lifetime_backend_shutdown(&refused);
    lifetime_driver_close(&noShutdown);
    library.hideShutdown = false;
    if (library.references != 0 || library.shutdownCalls != 1 ||
        library.closeCalls != 2) return 14211;

    // 14212-14213: a state without a context fails cleanly, and a context
    // closed while never opened is a no-op.
    LinuxGpuState orphan = {};
    if (lifetime_backend_init(&orphan, nullptr, nullptr, err, sizeof(err)) ||
        orphan.nvmlReady || !err[0]) return 14212;
    LinuxDriverContext unopened = {};
    lifetime_driver_close(&unopened);
    if (library.closeCalls != 2 || library.shutdownCalls != 1) return 14213;

    // 14214-14215: every init is today's single-GPU sequence, exactly: one
    // NVML discovery, one NvAPI initialization and one NvAPI enumeration at
    // bind time (as each new GPU state did), while both libraries are loaded
    // and NVML initialized only once for the process.
    library = {};
    library.provideNvapi = true;
    LinuxDriverContext shared = {};
    LinuxGpuState first = {}, rediscovered = {};
    if (!lifetime_backend_init(&first, &shared, nullptr, err, sizeof(err)) ||
        shared.discoveries != 1 || library.countCalls != 1 || library.initCalls != 1 ||
        library.nvapiInits != 1 || library.nvapiEnumerations != 1 ||
        library.openCalls != 2) return 14214;
    if (!lifetime_backend_init(&rediscovered, &shared, nullptr, err, sizeof(err)) ||
        shared.discoveries != 2 || library.countCalls != 2 || library.initCalls != 1 ||
        library.nvapiInits != 2 || library.nvapiEnumerations != 2 ||
        library.openCalls != 2) return 14215;

    // 14217-14219: the driver drops NVML after a successful init (reset or
    // reload). Discovery reports it uninitialized and clears the stale init,
    // and the same init call initializes again and recovers (a one-shot read
    // retry or resume must not need a second attempt); references balanced.
    library.loseInit = true;
    const int discoveriesBefore = (int)shared.discoveries;
    LinuxGpuState recovered = {};
    if (!lifetime_backend_init(&recovered, &shared, nullptr, err, sizeof(err)) ||
        !shared.nvmlInitialized || !recovered.nvmlReady) return 14217;
    if (library.initCalls != 2 || library.references != 1 ||
        (int)shared.discoveries != discoveriesBefore + 2) return 14218;

    // 14235-14236: NvAPI readiness is per binding. A state that never bound
    // is not usable because another GPU on the same context initialized
    // NvAPI; a bound one is. Each binding keeps its own lock-log gate.
    LinuxGpuState unbound = {};
    unbound.driver = &shared;
    if (!shared.nvapiInitialized || unbound.nvapiReady || !recovered.nvapiReady) return 14235;
    NvapiClientLocks reading = {};
    linux_capture_log_client_locks(&recovered.clientLocksLog, reading);
    if (!recovered.clientLocksLog.seen || first.clientLocksLog.seen) return 14236;

    // 14237-14239: every binding initializes NvAPI exactly once before it
    // enumerates (each new GPU state did), so a session a driver reset
    // invalidated is renewed by the next binding; a failed enumeration is
    // published to that binding and leaves the session to be initialized again.
    const int nvapiInitsBefore = library.nvapiInits;
    if (shared.nvapiInitAttempts != (unsigned int)nvapiInitsBefore) return 14237;
    library.refuseNvapiEnum = true;
    LinuxGpuState refusedEnum = {};
    lifetime_backend_init(&refusedEnum, &shared, nullptr, err, sizeof(err));
    if (shared.nvapiInitialized || refusedEnum.nvapiReady ||
        library.nvapiInits != nvapiInitsBefore + 1 ||
        refusedEnum.health.reason != SERVICE_GPU_HEALTH_NVAPI_ENUM_FAILED) return 14238;
    library.refuseNvapiEnum = false;
    LinuxGpuState reinitialized = {};
    if (!lifetime_backend_init(&reinitialized, &shared, nullptr, err, sizeof(err)) ||
        library.nvapiInits != nvapiInitsBefore + 2 || !shared.nvapiInitialized ||
        !reinitialized.nvapiReady) return 14239;
    // 14253-14254: both libraries this context opened are closed exactly once
    // (NVML and NvAPI counted separately by handle).
    const int closesBefore = library.closeCalls;
    const int nvmlClosesBefore = library.nvmlCloses;
    const int nvapiClosesBefore = library.nvapiCloses;
    LinuxDriverContext both = {};
    LinuxGpuState bothState = {};
    if (!lifetime_backend_init(&bothState, &both, nullptr, err, sizeof(err))) return 14253;
    lifetime_driver_close(&both);
    lifetime_driver_close(&both);
    if (library.closeCalls != closesBefore + 2 ||
        library.nvmlCloses != nvmlClosesBefore + 1 ||
        library.nvapiCloses != nvapiClosesBefore + 1) return 14254;

    // 14242-14246: same-GPU recovery re-enumeration notices a modeled NVML
    // session loss and recovers in place via linux_driver_open_nvml(); if the
    // first re-init fails, a later recovery still retries; refresh() renews
    // NvAPI in the same call when NVML had to re-initialize.
    lifetime_driver_close(&shared);
    library = {};
    library.providePci = true;
    library.provideNvapi = true;
    LinuxGpuState pciBound = {};
    if (!lifetime_backend_init(&pciBound, &shared, nullptr, err, sizeof(err)) ||
        !pciBound.selectedGpu.pciInfoValid || !pciBound.nvapiReady) return 14242;
    library.loseInit = true;
    library.refuseInit = true;
    char recoveryErr[192] = {};
    bool nvmlReinitialized = false;
    if (linux_backend_reenumerate_same_nvml_gpu(&pciBound, recoveryErr, sizeof(recoveryErr),
            &nvmlReinitialized) || shared.nvmlInitialized || nvmlReinitialized ||
        library.initCalls != 2 || library.references != 0) return 14243;
    library.refuseInit = false;
    if (!linux_backend_reenumerate_same_nvml_gpu(&pciBound, recoveryErr, sizeof(recoveryErr),
            &nvmlReinitialized) || !shared.nvmlInitialized || !nvmlReinitialized ||
        library.initCalls != 3 || library.references != 1) return 14244;
    // Full refresh() recovery when both NVML and NvAPI sessions were lost:
    // re-initializes NVML and renews NvAPI in a single refresh() call.
    library.loseInit = true;
    library.nvapiSessionLost = true;
    pciBound.nvapiReady = false;
    pciBound.gpuHandle = nullptr;
    const int nvapiInitsBeforeRefresh = library.nvapiInits;
    (void)lifetime_backend_refresh(&pciBound);
    if (!shared.nvmlInitialized || !shared.nvapiInitialized || !pciBound.nvapiReady ||
        !pciBound.gpuHandle || library.initCalls != 4 || library.references != 1 ||
        library.nvapiInits != nvapiInitsBeforeRefresh + 1) return 14245;
    // When NVML stays healthy during refresh() recovery, NvAPI keeps its
    // current session (no extra NvAPI initialize call).
    pciBound.gpuHandle = nullptr;
    (void)lifetime_backend_refresh(&pciBound);
    if (!pciBound.nvapiReady || !pciBound.gpuHandle ||
        library.initCalls != 4 || library.nvapiInits != nvapiInitsBeforeRefresh + 1) return 14246;

    lifetime_driver_close(&shared);
    // One NVML reference released once; both library handles closed once.
    if (library.references != 0 || library.shutdownCalls != 1 ||
        library.closeCalls != 2 || library.nvmlCloses != 1 ||
        library.nvapiCloses != 1) return 14219;

    // 14240-14241: a refused NvAPI initialization still leaves this state the
    // resolved query interface (diagnostics keep using it), while NvAPI is not
    // ready for it and the refusal is its published health.
    library = {};
    library.provideNvapi = true;
    library.refuseNvapiInit = true;
    LinuxDriverContext refusedContext = {};
    LinuxGpuState refusedInit = {};
    lifetime_backend_init(&refusedInit, &refusedContext, nullptr, err, sizeof(err));
    if (!refusedInit.nvmlReady || !refusedInit.nvapiQi || refusedInit.nvapiReady) return 14240;
    if (refusedInit.health.reason != SERVICE_GPU_HEALTH_NVAPI_INIT_FAILED ||
        refusedContext.nvapiInitialized) return 14241;
    lifetime_driver_close(&refusedContext);
    if (int failure = linux_driver_discovery_cases(err, sizeof(err))) return failure;
    return 0;
}
#else
// The production backend being exercised is Linux-only.
int run_linux_nvml_lifetime_tests() { return 0; }
#endif
