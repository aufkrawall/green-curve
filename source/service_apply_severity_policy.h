// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Hard-pin awareness for apply outcome severity.  A hard NVML pin is verified
// by NVML itself: min=max locked clocks make VF tail readback diagnostic only,
// so it must not turn a successful pin into a warning.  Boost-region partial
// counts still matter, and a failed step still outranks everything.
//
// Mixed-result rollback is deliberately scoped to the core apply domain:
// VF/GPU/memory/power/lock/fan.  Individual core helpers report success/failure
// through the shared counters, and the caller snapshots those counters before
// entering the advanced-clock domain.  XBAR/SYS/VIDEO writes happen only after
// this decision and are independently verified; they are intentionally outside
// the core rollback performed by the legacy-named rollback_to_safe_defaults().

#ifndef GREEN_CURVE_SERVICE_APPLY_SEVERITY_POLICY_H
#define GREEN_CURVE_SERVICE_APPLY_SEVERITY_POLICY_H

#include "service_protocol.h"

static constexpr bool service_apply_core_requires_mixed_failure_rollback(
    int coreSuccessCount, int coreFailCount) {
    return coreSuccessCount > 0 && coreFailCount > 0;
}

static_assert(!service_apply_core_requires_mixed_failure_rollback(0, 0),
    "an untouched core apply must not roll back");
static_assert(!service_apply_core_requires_mixed_failure_rollback(1, 0),
    "an all-success core apply must not roll back");
static_assert(!service_apply_core_requires_mixed_failure_rollback(0, 1),
    "a core failure with no committed success is not a mixed apply");
static_assert(service_apply_core_requires_mixed_failure_rollback(1, 1),
    "a fan or other core failure after an earlier core success must roll back");
static_assert(service_apply_core_requires_mixed_failure_rollback(7, 2),
    "core mixed-result rollback must not depend on exact counter values");

static inline gc_u32 service_apply_outcome_severity_for_lock_mode(
    bool hardPin, int failCount, int partialBoostPoints,
    int partialFlattenPoints) {
    if (hardPin) partialFlattenPoints = 0;
    return service_apply_outcome_severity(failCount, partialBoostPoints,
        partialFlattenPoints);
}

#endif  // GREEN_CURVE_SERVICE_APPLY_SEVERITY_POLICY_H
