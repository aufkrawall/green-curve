// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Session-loss and capture cases (assertion codes 14247-14249, 14251-14252,
// 14255-14257, 14271), included by
// tests/linux_nvml_lifetime_tests.cpp after its fake driver: a driver reset
// must recover in the same init as before the shared context, a cold start
// keeps its single attempt, and passive capture judges each binding.
#ifndef GREEN_CURVE_LINUX_DRIVER_DISCOVERY_CASES_H
#define GREEN_CURVE_LINUX_DRIVER_DISCOVERY_CASES_H

static int linux_driver_discovery_cases(char* err, size_t errSize) {
    // 14247, 14255: a driver reset also invalidates the NvAPI session. The next
    // init's binding initializes NvAPI again (as every binding does) and binds
    // in the same call, as a fresh per-state initialization did before.
    library = {};
    library.provideNvapi = true;
    LinuxDriverContext nvapiCtx = {};
    LinuxGpuState first = {}, afterReset = {};
    if (!lifetime_backend_init(&first, &nvapiCtx, nullptr, err, errSize) ||
        !first.nvapiReady || library.nvapiInits != 1) return 14247;
    library.nvapiSessionLost = true;
    if (!lifetime_backend_init(&afterReset, &nvapiCtx, nullptr, err, errSize) ||
        !afterReset.nvapiReady || library.nvapiInits != 2 ||
        !nvapiCtx.nvapiInitialized) return 14255;
    lifetime_driver_close(&nvapiCtx);

    // 14248: a cold start whose first enumeration finds NVML uninitialized
    // keeps its single init attempt (only a cached session is retried).
    library = {};
    library.loseInit = true;
    LinuxDriverContext coldCtx = {};
    LinuxGpuState cold = {};
    if (lifetime_backend_init(&cold, &coldCtx, nullptr, err, errSize) ||
        library.initCalls != 1) return 14248;
    lifetime_driver_close(&coldCtx);

    // 14249, 14256-14257: passive capture judges each binding: a state whose
    // binding failed is refused even though another state initialized the
    // shared session, and each bound GPU updates only its own lock-log gate.
    library = {};
    library.provideNvapi = true;
    library.providePci = true;
    LinuxDriverContext captureCtx = {};
    static LinuxGpuState boundX, boundY, unboundZ;
    static HardwareCaptureObservation observation;
    if (!lifetime_backend_init(&boundX, &captureCtx, nullptr, err, errSize) ||
        !lifetime_backend_init(&boundY, &captureCtx, nullptr, err, errSize)) return 14249;
    unboundZ = boundX;
    unboundZ.nvmlReady = false;
    unboundZ.nvapiReady = false;
    unboundZ.clientLocksLog = {};
    boundX.clientLocksLog = {};
    boundY.clientLocksLog = {};
    char why[192] = {};
    if (!captureCtx.nvapiInitialized ||
        lifetime_backend_capture_current(&unboundZ, 1, PROFILE_DOMAIN_LOCK, nullptr,
            &observation, why, sizeof(why))) return 14256;
    if (!lifetime_backend_capture_current(&boundX, 1, PROFILE_DOMAIN_LOCK, nullptr,
            &observation, why, sizeof(why)) ||
        !boundX.clientLocksLog.seen || boundY.clientLocksLog.seen ||
        unboundZ.clientLocksLog.seen) return 14257;
    // 14251: per-GPU states never hold nvmlInit_v2 or nvmlShutdown pointers,
    // even indirectly; only the process-wide context owns them.
    if (!captureCtx.nvml.init || !captureCtx.nvml.shutdown ||
        boundX.nvml.init || boundX.nvml.shutdown ||
        boundY.nvml.init || boundY.nvml.shutdown) return 14251;
    // 14252: a replacement rebind of the same GPU preserves its client-lock
    // log gate so unchanged lock state does not repeat in the journal.
    boundX.writeIdentityResolved = false;
    GpuAdapterInfo sameTarget = boundX.selectedGpu;
    if (!lifetime_backend_select_target(&boundX, &sameTarget, err, errSize) ||
        !boundX.clientLocksLog.seen) return 14252;
    // 14271: lock-log carry-over honors GPU UUID when both are valid so a
    // same-model card swapped into the same PCI slot gets a fresh log gate.
    static LinuxGpuState uuidA, uuidB;
    uuidA = boundX;
    uuidB = boundX;
    uuidB.clientLocksLog = {};
    uuidA.adapterUuids[uuidA.selectedAdapterIndex] =
        linux_gpu_uuid_parse("GPU-11111111-1111-1111-1111-111111111111");
    uuidB.adapterUuids[uuidB.selectedAdapterIndex] =
        linux_gpu_uuid_parse("GPU-22222222-2222-2222-2222-222222222222");
    linux_backend_preserve_same_gpu_log_state(&uuidA, &uuidB);
    if (uuidB.clientLocksLog.seen) return 14271;
    uuidB.adapterUuids[uuidB.selectedAdapterIndex] =
        linux_gpu_uuid_parse("GPU-11111111-1111-1111-1111-111111111111");
    linux_backend_preserve_same_gpu_log_state(&uuidA, &uuidB);
    if (!uuidB.clientLocksLog.seen) return 14271;
    lifetime_driver_close(&captureCtx);
    return 0;
}

#endif
