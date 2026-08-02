// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Hard-pin awareness for apply outcome severity.  A hard NVML pin is verified
// by NVML itself: min=max locked clocks make VF tail readback diagnostic only,
// so it must not turn a successful pin into a warning.  Boost-region partial
// counts still matter, and a failed step still outranks everything.

#ifndef GREEN_CURVE_SERVICE_APPLY_SEVERITY_POLICY_H
#define GREEN_CURVE_SERVICE_APPLY_SEVERITY_POLICY_H

#include "service_protocol.h"

static inline gc_u32 service_apply_outcome_severity_for_lock_mode(
    bool hardPin, int failCount, int partialBoostPoints,
    int partialFlattenPoints) {
    if (hardPin) partialFlattenPoints = 0;
    return service_apply_outcome_severity(failCount, partialBoostPoints,
        partialFlattenPoints);
}

#endif  // GREEN_CURVE_SERVICE_APPLY_SEVERITY_POLICY_H
