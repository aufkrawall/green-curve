// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure outcome policy for the Windows read-only driver/architecture self-test.
// Kept separate from the Win32 report so regression tests can prove that a
// failed CONTROL read or unavailable NVML can never be followed by FULL.

#ifndef GREEN_CURVE_DRIVER_SELF_TEST_POLICY_H
#define GREEN_CURVE_DRIVER_SELF_TEST_POLICY_H

#include "gpu_capability_policy.h"

enum DriverSelfTestVerdict {
    DRIVER_SELF_TEST_FULL = 0,
    DRIVER_SELF_TEST_PARTIAL = 1,
    DRIVER_SELF_TEST_MONITOR_ONLY = 2,
    DRIVER_SELF_TEST_INCONCLUSIVE = 3,
    DRIVER_SELF_TEST_UNUSABLE = 4,
};

struct DriverSelfTestFacts {
    bool elevated;
    bool nvapiInitialized;
    bool gpuEnumerated;
    bool curveRead;
    bool controlRead;
    bool nvmlReady;
    bool vfWritable;
    gc_u32 surface;
};

constexpr DriverSelfTestVerdict driver_self_test_verdict(
    const DriverSelfTestFacts* facts) {
    if (!facts || !facts->nvapiInitialized || !facts->gpuEnumerated)
        return DRIVER_SELF_TEST_UNUSABLE;
    if (!facts->elevated && (!facts->curveRead || !facts->controlRead))
        return DRIVER_SELF_TEST_INCONCLUSIVE;
    if (facts->nvmlReady && !facts->vfWritable &&
        facts->surface == GPU_CONTROL_SURFACE_MONITOR_ONLY)
        return DRIVER_SELF_TEST_MONITOR_ONLY;
    if (!facts->nvmlReady || !facts->curveRead || !facts->controlRead ||
        !facts->vfWritable || facts->surface != GPU_CONTROL_SURFACE_FULL)
        return DRIVER_SELF_TEST_PARTIAL;
    return DRIVER_SELF_TEST_FULL;
}

// Match the Linux CLI convention: initialization failure is 1, a completed but
// degraded/inconclusive test is 2, and only a full pass is 0.
constexpr int driver_self_test_exit_code(DriverSelfTestVerdict verdict) {
    if (verdict == DRIVER_SELF_TEST_FULL) return 0;
    if (verdict == DRIVER_SELF_TEST_UNUSABLE) return 1;
    return 2;
}

#endif // GREEN_CURVE_DRIVER_SELF_TEST_POLICY_H
