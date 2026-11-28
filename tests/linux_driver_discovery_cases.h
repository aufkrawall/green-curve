// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Session-loss and capture cases (assertion codes 14247-14249, 14251-14252,
// 14255-14257, 14271, 14280), included by
// tests/linux_nvml_lifetime_tests.cpp after its fake driver: a driver reset
// must recover in the same init as before the shared context, a cold start
// keeps its single attempt, and passive capture judges each binding.
#ifndef GREEN_CURVE_LINUX_DRIVER_DISCOVERY_CASES_H
#define GREEN_CURVE_LINUX_DRIVER_DISCOVERY_CASES_H

// The selected handle's UUID now reads as another card (14962).
static nvmlReturn_t discovery_fixture_other_uuid(nvmlDevice_t, char* out, unsigned int size) {
    snprintf(out, size, "%s", "GPU-44444444-4444-4444-4444-444444444444");
    return NVML_SUCCESS;
}

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
    // 14271, 14280: lock-log carry-over honors GPU UUID when both are valid so a
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
    if (!uuidB.clientLocksLog.seen) return 14280;
    // Pristine bound states for 14964 and 14969 below, before the cases that follow
    // reshape boundX.
    static LinuxGpuState pristine, sessionSource;
    pristine = boundX;
    sessionSource = boundX;
    // 14958: the selected-GPU fast path keeps the UUID discovery learned when
    // a fresh UUID query fails (this fake driver has no UUID getter), so the
    // evidence that would expose a replacement card is never erased.
    boundX.writeIdentityResolved = true;
    boundX.getUuid = nullptr;
    const LinuxGpuUuid learned = linux_gpu_uuid_parse("GPU-33333333-3333-3333-3333-333333333333");
    boundX.adapterUuids[boundX.selectedAdapterIndex] = learned;
    if (!lifetime_backend_select_target(&boundX, &sameTarget, err, errSize,
            GPU_LOCATION_ROLE_REQUIRED, &learned) ||
        strcmp(boundX.adapterUuids[boundX.selectedAdapterIndex].text, learned.text)) return 14958;
    // 14960: and when that known UUID contradicts the stored binding, the
    // rediscovery that follows keeps it as evidence (a second failed query
    // cannot erase it), so the selection is refused as a mismatch.
    const LinuxGpuUuid other = linux_gpu_uuid_parse("GPU-44444444-4444-4444-4444-444444444444");
    LinuxGpuUuidVerdict mismatchVerdict = LINUX_GPU_UUID_FALLBACK;
    if (lifetime_backend_select_target(&boundX, &sameTarget, err, errSize,
            GPU_LOCATION_ROLE_REQUIRED, &other, &mismatchVerdict) ||
        mismatchVerdict != LINUX_GPU_UUID_MISMATCH ||
        strcmp(boundX.adapterUuids[boundX.selectedAdapterIndex].text, learned.text)) return 14960;
    // 14962: a fresh read on the selected handle that contradicts the cached
    // UUID is the newest evidence; rediscovery (whose reads fail here) must
    // not bring the older, matching UUID back and accept the card.
    boundX.getUuid = discovery_fixture_other_uuid;
    boundX.adapterUuids[boundX.selectedAdapterIndex] = learned;
    mismatchVerdict = LINUX_GPU_UUID_FALLBACK;
    if (lifetime_backend_select_target(&boundX, &sameTarget, err, errSize,
            GPU_LOCATION_ROLE_REQUIRED, &learned, &mismatchVerdict) ||
        mismatchVerdict != LINUX_GPU_UUID_MISMATCH) return 14962;
    // 14924: the earlier discovery saw the requested UUID on two adapters; when
    // rediscovery cannot read UUIDs at all, that known ambiguity still refuses
    // instead of becoming a match by location.
    boundX.getUuid = nullptr;
    boundX.writeIdentityResolved = true;
    boundX.adapterCount = 2;
    boundX.adapters[1] = boundX.adapters[0];
    boundX.adapters[1].pciBus = boundX.adapters[0].pciBus + 1;
    boundX.adapterUuids[0] = learned;
    boundX.adapterUuids[1] = learned;
    mismatchVerdict = LINUX_GPU_UUID_FALLBACK;
    if (lifetime_backend_select_target(&boundX, &sameTarget, err, errSize,
            GPU_LOCATION_ROLE_REQUIRED, &learned, &mismatchVerdict) ||
        mismatchVerdict != LINUX_GPU_UUID_AMBIGUOUS) return 14924;
    // 14925: two adapters earlier reported the same UUID, a different one from
    // the stored binding: the old slot is a known mismatch, and losing both
    // readings does not make it a match by location.
    const LinuxGpuUuid shared = linux_gpu_uuid_parse("GPU-55555555-5555-5555-5555-555555555555");
    boundX.adapterUuids[0] = shared;
    boundX.adapterUuids[1] = shared;
    mismatchVerdict = LINUX_GPU_UUID_FALLBACK;
    if (lifetime_backend_select_target(&boundX, &sameTarget, err, errSize,
            GPU_LOCATION_ROLE_REQUIRED, &learned, &mismatchVerdict) ||
        mismatchVerdict == LINUX_GPU_UUID_MATCHED) return 14925;
    // 14928: a mismatch already known stands when nothing ties it to a
    // location (no BDF on either side) and rediscovery cannot read UUIDs: a
    // failed read never turns it into a match by model.
    boundX.adapterCount = 1;
    boundX.adapters[0].pciDomain = boundX.adapters[0].pciBus = 0;
    boundX.adapters[0].pciDevice = boundX.adapters[0].pciFunction = 0;
    boundX.adapterUuids[0] = shared;
    GpuAdapterInfo modelOnly = boundX.adapters[0];
    mismatchVerdict = LINUX_GPU_UUID_FALLBACK;
    if (lifetime_backend_select_target(&boundX, &modelOnly, err, errSize,
            GPU_LOCATION_ROLE_UNRECORDED, &learned, &mismatchVerdict) ||
        mismatchVerdict != LINUX_GPU_UUID_MISMATCH) return 14928;
    // 14963: with nothing cached and no BDF, a contradiction first read on the
    // selected handle is known evidence: rediscovery that cannot read UUIDs
    // does not accept the card by model.
    boundX.adapterUuids[0] = LinuxGpuUuid{};
    boundX.getUuid = discovery_fixture_other_uuid;
    mismatchVerdict = LINUX_GPU_UUID_FALLBACK;
    if (lifetime_backend_select_target(&boundX, &modelOnly, err, errSize,
            GPU_LOCATION_ROLE_UNRECORDED, &learned, &mismatchVerdict) ||
        mismatchVerdict != LINUX_GPU_UUID_MISMATCH) return 14963;
    // 14964: a UUID carried from the earlier discovery only vetoes. A
    // rediscovered card whose own read failed is bound without it, so it is
    // never published or journaled as that card's physical identity.
    pristine.writeIdentityResolved = false;
    pristine.getUuid = nullptr;
    pristine.adapterUuids[pristine.selectedAdapterIndex] = learned;
    GpuAdapterInfo pristineTarget = pristine.selectedGpu;
    if (!lifetime_backend_select_target(&pristine, &pristineTarget, err, errSize) ||
        linux_gpu_uuid_valid(&pristine.adapterUuids[pristine.selectedAdapterIndex])) return 14964;
    GpuAdapterInfo sessionTarget = sessionSource.selectedGpu;
    // 14969: the bound fast path in a newer NVML session drops the older
    // session's reads before its own fresh read, which then stays current.
    static LinuxGpuState renewed;
    renewed = sessionSource;
    renewed.writeIdentityResolved = true;
    renewed.getUuid = discovery_fixture_other_uuid;
    renewed.adapterUuids[renewed.selectedAdapterIndex] = learned;
    renewed.uuidSession = captureCtx.nvmlSessions;
    ++captureCtx.nvmlSessions;
    const bool renewedBound = lifetime_backend_select_target(&renewed, &sessionTarget, err, errSize,
        GPU_LOCATION_ROLE_REQUIRED, &other);
    linux_backend_expire_session_uuids(&renewed);
    if (!renewedBound || strcmp(renewed.adapterUuids[renewed.selectedAdapterIndex].text, other.text)) return 14969;
    lifetime_driver_close(&captureCtx);
    return 0;
}

#endif
