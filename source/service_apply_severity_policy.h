// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Hard-pin awareness for apply outcome severity.  A hard NVML pin is verified
// by NVML itself: min=max locked clocks make VF tail readback diagnostic only,
// so it must not turn a successful pin into a warning.  Boost-region partial
// counts still matter, and a failed step still outranks everything.
//
// Mixed-result rollback is also a pure transaction policy.  Individual apply
// helpers report success/failure through the shared counters; in particular,
// fan-control failures increment failCount and return to the common apply path.
// Any request that has both a committed success and a failure must therefore
// enter rollback and surface the mixed-partial warning.

#ifndef GREEN_CURVE_SERVICE_APPLY_SEVERITY_POLICY_H
#define GREEN_CURVE_SERVICE_APPLY_SEVERITY_POLICY_H

#include "service_protocol.h"

static constexpr bool service_apply_requires_mixed_failure_rollback(
    int successCount, int failCount) {
    return successCount > 0 && failCount > 0;
}

static_assert(!service_apply_requires_mixed_failure_rollback(0, 0),
    "an untouched apply must not roll back");
static_assert(!service_apply_requires_mixed_failure_rollback(1, 0),
    "an all-success apply must not roll back");
static_assert(!service_apply_requires_mixed_failure_rollback(0, 1),
    "a failure with no committed success is not a mixed apply");
static_assert(service_apply_requires_mixed_failure_rollback(1, 1),
    "a fan or other failure after an earlier success must roll back");
static_assert(service_apply_requires_mixed_failure_rollback(7, 2),
    "mixed-result rollback must not depend on exact counter values");

static inline gc_u32 service_apply_outcome_severity_for_lock_mode(
    bool hardPin, int failCount, int partialBoostPoints,
    int partialFlattenPoints) {
    if (hardPin) partialFlattenPoints = 0;
    return service_apply_outcome_severity(failCount, partialBoostPoints,
        partialFlattenPoints);
}

#endif  // GREEN_CURVE_SERVICE_APPLY_SEVERITY_POLICY_H
