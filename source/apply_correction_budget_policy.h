// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// How much of its own time budget a Windows Apply may spend correcting the VF
// curve.
//
// SERVICE_APPLY_HANDLER_BUDGET_MS (service_request_deadline_policy.h) is what
// the service promises the client one mutation may take, and the client's
// response deadline and its operation-result recovery deadline are both
// derived from it.  Nothing inside the apply enforced it: the correction loop
// in gpu_backend_apply.cpp allowed 25 passes, each of up to three batch
// setControl writes plus one ~1 s per-point fallback write for every point the
// batch left unconverged.  A curve that kept improving a little every pass under
// load could therefore run far past the budget while holding the runtime lock:
// the client reported a timeout, every state read and fan pulse queued behind
// the apply, and once the recovery window was also exceeded the user was told
// the outcome was unknown.
//
// The rule below keeps the correction phase inside the budget and leaves a
// fixed reserve for everything that still follows it (final lock/pin, power,
// fan, advanced clocks, the post-apply refresh, and -- on failure -- the
// rollback).  A correction stopped here fails verification exactly like one
// that ran out of passes, so the existing failure path (clamp retained,
// rollback to safe defaults) handles it; nothing new is written.
//
// The FIRST correction pass is never refused.  Logs show nearly every
// under-load miss converging on pass 1, and refusing it would turn a slow-but-
// recoverable apply into a failure purely because the reset settle was slow.

#ifndef GREEN_CURVE_APPLY_CORRECTION_BUDGET_POLICY_H
#define GREEN_CURVE_APPLY_CORRECTION_BUDGET_POLICY_H

#include "service_request_deadline_policy.h"

// Kept back after the last correction pass for the steps listed above.
#define APPLY_POST_CORRECTION_RESERVE_MS 6000u

static_assert(APPLY_POST_CORRECTION_RESERVE_MS < SERVICE_APPLY_HANDLER_BUDGET_MS,
              "the correction reserve must leave the correction phase some time");

// Milliseconds after apply entry by which correction work must stop starting.
static constexpr unsigned long apply_correction_budget_ms() {
    return (unsigned long)SERVICE_APPLY_HANDLER_BUDGET_MS -
        (unsigned long)APPLY_POST_CORRECTION_RESERVE_MS;
}

// Whether correction pass `passIndex` (0-based) may start, `elapsedMs` after
// apply entry, when the previous pass took `previousPassMs`.  The previous
// pass is the best available estimate of the next one: a pass that would
// predictably finish past the budget is not started.
static inline bool apply_correction_pass_may_start(int passIndex,
    unsigned long long elapsedMs, unsigned long long previousPassMs) {
    if (passIndex <= 0) return true;
    return elapsedMs + previousPassMs <= (unsigned long long)apply_correction_budget_ms();
}

// Whether one more per-point fallback write may start.  `deadlineTickMs == 0`
// means unbounded, which is what Reset and rollback pass: returning the GPU to
// stock must never be cut short by an apply's budget.
static inline bool apply_fallback_write_may_start(unsigned long long nowTickMs,
    unsigned long long deadlineTickMs) {
    return deadlineTickMs == 0 || nowTickMs < deadlineTickMs;
}

#endif  // GREEN_CURVE_APPLY_CORRECTION_BUDGET_POLICY_H
