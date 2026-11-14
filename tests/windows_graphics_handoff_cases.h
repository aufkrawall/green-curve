// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Production passive review, durable preparation, named writes and final proof.
static int windows_graphics_handoff_cases() {
    DesiredSettings d{}; d.hasGpuOffset = 1; d.gpuOffsetMHz = 100;
    d.authoring.graphicsPrecise = 1; d.authoring.graphicsSurface = DESIRED_GRAPHICS_GLOBAL;
    d.authoring.graphicsOffsetHz = 100000000;
    ScalarCurveSubmissionReview review{}; char why[256]{};
    auto prepare = [&] {
        initialize(false); preciseReadback = true; review = {};
        g_nvml_api.getGpcClkVfOffset = g_nvml_api.getMemClkVfOffset;
        g_nvml_api.setGpcClkVfOffset = g_nvml_api.setMemClkVfOffset;
        g_nvml_api.setGpuLockedClocks = [](nvmlDevice_t, unsigned int, unsigned int) -> nvmlReturn_t { return NVML_SUCCESS; };
        p0HardwareMHz = -50; hardwareMHz = 17;
        g_app.gpuOffsetOwnedSurface = CLOCK_OFFSET_SURFACE_P0;
        g_app.gpuOffsetRecoverySurfaces = GRAPHICS_SCALAR_RECOVERY_P0;
    };
    prepare();
    if (apply_graphics_handoff_precheck(&d, review, why, sizeof(why)) || p0Writes || writes || releases) return 9160;
    d.hasLock = 1; d.lockMode = LOCK_MODE_FLATTEN; d.lockMHz = 1000;
    if (!apply_graphics_handoff_precheck(&d, review, why, sizeof(why)) || !review.installedGuardRequired ||
        review.outgoingSurface != CLOCK_OFFSET_SURFACE_P0 || !review.beforeValid || review.beforeHz != 17000000) return 9161;
    if (nvapi_release_graphics_handoff(d, review, false) || p0Writes || writes ||
        !review.refusedWithoutWrite || durableGraphicsStores) return 9162;
    ++hardwareMHz;
    if (nvapi_release_graphics_handoff(d, review, true) || p0Writes || writes || durableGraphicsStores) return 9163;
    --hardwareMHz; ++p0HardwareMHz;
    if (nvapi_release_graphics_handoff(d, review, true) || p0Writes || writes || durableGraphicsStores) return 9164;
    --p0HardwareMHz; durableGraphicsStoreFails = true;
    if (nvapi_release_graphics_handoff(d, review, true) || p0Writes || writes || !review.refusedWithoutWrite) return 9165;
    durableGraphicsStoreFails = false;
    if (!nvapi_release_graphics_handoff(d, review, true) || p0HardwareMHz || p0Writes != 1 || writes ||
        !review.writeAttempted || review.refusedWithoutWrite ||
        durableGraphicsSurfaces != GRAPHICS_SCALAR_RECOVERY_ALL ||
        g_app.gpuOffsetRecoverySurfaces != GRAPHICS_SCALAR_RECOVERY_ALL) return 9166;
    preciseReadback = false;
    if (nvapi_set_gpu_offset_precise_checked(100000000, CLOCK_OFFSET_SURFACE_GLOBAL, false, &review) ||
        review.refusedWithoutWrite || !review.writeAttempted || writes || p0Writes != 1) return 9167;
    preciseReadback = true;
    if (!nvapi_set_gpu_offset_precise_checked(100000000, CLOCK_OFFSET_SURFACE_GLOBAL, false, &review) ||
        !nvapi_verify_graphics_handoff(d, review) || p0HardwareMHz || hardwareMHz != 100 || writes != 1) return 9168;
    ++p0HardwareMHz; // A later VF write invalidates the earlier release.
    if (nvapi_verify_graphics_handoff(d, review) ||
        g_app.gpuOffsetUncertainSurface != CLOCK_OFFSET_SURFACE_P0 ||
        g_app.gpuOffsetRecoverySurfaces != GRAPHICS_SCALAR_RECOVERY_ALL) return 9169;
    if (!nvapi_reset_graphics_scalar_controls(true) || p0HardwareMHz || hardwareMHz ||
        g_app.gpuOffsetRecoverySurfaces || g_app.gpuOffsetUncertainSurface) return 9170;
    prepare(); p0HardwareMHz = 0; d.hasLock = 0;
    if (!apply_graphics_handoff_precheck(&d, review, why, sizeof(why)) || review.installedGuardRequired ||
        !nvapi_release_graphics_handoff(d, review, false) || p0Writes) return 9171;
    preciseReadback = false;
    if (nvapi_set_gpu_offset_precise_checked(100000000, CLOCK_OFFSET_SURFACE_GLOBAL, false, &review) ||
        !review.refusedWithoutWrite || review.writeAttempted || p0Writes || writes ||
        g_app.gpuOffsetOwnedSurface != CLOCK_OFFSET_SURFACE_P0) return 9181;
    preciseReadback = true;
    globalGraphicsClobbersP0 = true;
    if (!nvapi_set_gpu_offset_precise_checked(100000000, CLOCK_OFFSET_SURFACE_GLOBAL, false, &review) ||
        nvapi_verify_graphics_handoff(d, review) || nvapi_reset_graphics_scalar_controls(true) ||
        g_app.gpuOffsetRecoverySurfaces != GRAPHICS_SCALAR_RECOVERY_ALL) return 9172;
    globalGraphicsClobbersP0 = false;
    if (!nvapi_reset_graphics_scalar_controls(true)) return 9173;
    prepare(); d.hasLock = 1; d.resetOcBeforeApply = 1;
    if (!apply_graphics_handoff_precheck(&d, review, why, sizeof(why)) ||
        !nvapi_release_graphics_handoff(d, review, true) ||
        !nvapi_set_gpu_offset_precise(0, CLOCK_OFFSET_SURFACE_GLOBAL, true) ||
        !nvapi_graphics_baseline_review(d, review) || review.beforeHz ||
        !nvapi_set_gpu_offset_precise_checked(100000000, CLOCK_OFFSET_SURFACE_GLOBAL, false, &review) ||
        !nvapi_verify_graphics_handoff(d, review)) return 9174;
    nvapi_retire_graphics_handoff(d, review);
    if (g_app.gpuOffsetRecoverySurfaces != GRAPHICS_SCALAR_RECOVERY_GLOBAL ||
        g_app.gpuOffsetOwnedSurface != CLOCK_OFFSET_SURFACE_GLOBAL || g_app.gpuOffsetUncertainSurface) return 9175;
    prepare(); d.resetOcBeforeApply = 0; d.authoring.graphicsSurface = DESIRED_GRAPHICS_VF_COMPONENT;
    if (!apply_graphics_handoff_precheck(&d, review, why, sizeof(why)) ||
        !nvapi_release_graphics_handoff(d, review, true) || !nvapi_verify_graphics_handoff(d, review) ||
        p0HardwareMHz || hardwareMHz != 17 || p0Writes != 1 || writes) return 9176;
    nvapi_retire_graphics_handoff(d, review);
    if (g_app.gpuOffsetRecoverySurfaces || g_app.gpuOffsetOwnedSurface) return 9177;
    return 0;
}
