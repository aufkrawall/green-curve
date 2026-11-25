// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Execute production backend initialization/teardown against a reference-counted
// fake NVML library (assertion codes 14200-14210). No real driver library or
// physical GPU is opened.
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
#include "../source/linux_backend.cpp"
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
#undef linux_backend_init

namespace {
struct LibraryLifetime {
    int references, initCalls, shutdownCalls, closeCalls;
    bool refuseInit, refuseCount, hideShutdown;
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

static PlLib lifetime_open_library(int kind) {
    return kind == PL_DRIVER_NVML ? reinterpret_cast<PlLib>(1) : nullptr;
}

static void* lifetime_library_symbol(PlLib, const char* name) {
    if (strcmp(name, "nvmlInit_v2") == 0) return reinterpret_cast<void*>(fake_init);
    if (strcmp(name, "nvmlShutdown") == 0)
        return library.hideShutdown ? nullptr : reinterpret_cast<void*>(fake_shutdown);
    if (strcmp(name, "nvmlDeviceGetCount_v2") == 0) return reinterpret_cast<void*>(fake_count);
    if (strcmp(name, "nvmlDeviceGetHandleByIndex_v2") == 0) return reinterpret_cast<void*>(fake_handle);
    return nullptr;
}

static void lifetime_close_library(PlLib) { ++library.closeCalls; }
bool lifetime_platform_is_integrated_soc() { return false; }

int run_linux_nvml_lifetime_tests() {
    g_lbDebug = false;
    library = {};
    char err[256] = {};
    unsigned int count = 0;

    // 14200-14202: the daemon's driver-not-ready retry order. Startup init fails
    // at nvmlInit_v2 and keeps its opened library; a later replacement init
    // succeeds; only then is the stale state shut down. That teardown must not
    // release the replacement's sole reference.
    LinuxGpuState stale = {}, replacement = {};
    library.refuseInit = true;
    if (lifetime_backend_init(&stale, nullptr, err, sizeof(err)) ||
        stale.nvmlInitialized || !stale.nvmlLib || library.references != 0) return 14200;
    library.refuseInit = false;
    if (!lifetime_backend_init(&replacement, nullptr, err, sizeof(err)) ||
        !replacement.nvmlInitialized || library.references != 1) return 14201;
    lifetime_backend_shutdown(&stale);
    if (library.references != 1 || library.shutdownCalls != 0 ||
        library.closeCalls != 1 ||
        replacement.nvml.getCount(&count) != NVML_SUCCESS || count != 1) return 14202;

    // 14203-14204: another state whose init fails beside a live backend (the
    // shape of linux_backend_select_target()'s failed replacement, without
    // invoking the selector), torn down twice, leaves the live reference.
    LinuxGpuState failed = {};
    library.refuseInit = true;
    if (lifetime_backend_init(&failed, nullptr, err, sizeof(err)) ||
        failed.nvmlInitialized) return 14203;
    lifetime_backend_shutdown(&failed);
    lifetime_backend_shutdown(&failed);
    if (library.references != 1 || library.shutdownCalls != 0 ||
        library.closeCalls != 2 ||
        replacement.nvml.getCount(&count) != NVML_SUCCESS) return 14204;

    // 14205-14207: enumeration can fail after NVML acquired its reference but
    // before nvmlReady becomes true. Its teardown still owes exactly one
    // shutdown, and a repeated teardown owes none.
    LinuxGpuState partial = {};
    library.refuseInit = false;
    library.refuseCount = true;
    if (lifetime_backend_init(&partial, nullptr, err, sizeof(err)) ||
        !partial.nvmlInitialized || partial.nvmlReady ||
        library.references != 2) return 14205;
    lifetime_backend_shutdown(&partial);
    if (library.references != 1 || library.shutdownCalls != 1) return 14206;
    lifetime_backend_shutdown(&partial);
    library.refuseCount = false;
    if (library.references != 1 || library.shutdownCalls != 1 ||
        library.closeCalls != 3 ||
        replacement.nvml.getCount(&count) != NVML_SUCCESS || count != 1) return 14207;

    // 14208-14210: two independent successful states each release only their
    // own reference; every opened library handle is closed exactly once.
    LinuxGpuState second = {};
    if (!lifetime_backend_init(&second, nullptr, err, sizeof(err)) ||
        library.references != 2) return 14208;
    lifetime_backend_shutdown(&replacement);
    if (library.references != 1 || library.shutdownCalls != 2 ||
        second.nvml.getCount(&count) != NVML_SUCCESS) return 14209;
    lifetime_backend_shutdown(&second);
    lifetime_backend_shutdown(&second);
    if (library.references != 0 || library.shutdownCalls != 3 ||
        library.closeCalls != 5 || library.initCalls != 5) return 14210;

    // 14211: a library without nvmlShutdown is refused before nvmlInit_v2,
    // so no reference is acquired that teardown could not release.
    LinuxGpuState noShutdown = {};
    library.hideShutdown = true;
    const bool noShutdownInit =
        lifetime_backend_init(&noShutdown, nullptr, err, sizeof(err));
    const bool noShutdownInitialized = noShutdown.nvmlInitialized;
    lifetime_backend_shutdown(&noShutdown);
    library.hideShutdown = false;
    if (noShutdownInit || noShutdownInitialized || library.initCalls != 5 ||
        library.references != 0 || library.closeCalls != 6) return 14211;
    return 0;
}
#else
// The production backend being exercised is Linux-only.
int run_linux_nvml_lifetime_tests() { return 0; }
#endif
