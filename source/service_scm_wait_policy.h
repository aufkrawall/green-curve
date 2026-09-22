// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure policy: while waiting for the SCM to report a service state, is the
// service still making progress, has it arrived, or has it stopped moving?
//
// THE DEFECT THIS EXISTS FOR (review 2026-09-22): the service learned to
// publish START_PENDING/STOP_PENDING checkpoints and wait hints, but every
// waiter still used one fixed deadline and never looked at them.  A start that
// was visibly progressing past the deadline was reported as "did not start",
// and a start that had already FAILED (state back to STOPPED within a second)
// was waited on for the full deadline before being reported as a timeout --
// with the service's own exit code never read.
//
// This is Microsoft's documented service-state protocol: a pending state is
// alive for as long as dwCheckPoint keeps advancing within dwWaitHint of the
// previous advance.  A checkpoint change raises no notification, which is why
// the waiter samples QueryServiceStatusEx instead of blocking on
// NotifyServiceStatusChange -- the sampling interval is derived from the hint,
// exactly as the protocol prescribes, and every decision about it lives here.
//
// Host-neutral: the regression harness drives it with synthetic samples.

#ifndef GREEN_CURVE_SERVICE_SCM_WAIT_POLICY_H
#define GREEN_CURVE_SERVICE_SCM_WAIT_POLICY_H

// The SERVICE_* dwCurrentState values, spelled locally so this stays pure.
#define GC_SCM_STATE_STOPPED 1ul
#define GC_SCM_STATE_START_PENDING 2ul
#define GC_SCM_STATE_STOP_PENDING 3ul
#define GC_SCM_STATE_RUNNING 4ul
#define GC_SCM_STATE_CONTINUE_PENDING 5ul
#define GC_SCM_STATE_PAUSE_PENDING 6ul
#define GC_SCM_STATE_PAUSED 7ul

// A pending state with no hint gets the SCM's own default budget
// (ServicesPipeTimeout, 30 s) between progress reports.
#define GC_SCM_WAIT_NO_HINT_STALL_MS 30000ull
// A hint below this is treated as this: a service that promises the next
// checkpoint within 100 ms is not hung the moment one sample is late.
#define GC_SCM_WAIT_MIN_STALL_MS 2000ull
// However well a service reports progress, one wait never exceeds this.
// service_admin_reason_policy.h derives the elevated helper's own bound from
// it, so the two cannot contradict each other.
#define GC_SCM_WAIT_MAX_TOTAL_MS 60000ull

enum GcScmWaitVerdict {
    GC_SCM_WAIT_CONTINUE = 0,
    GC_SCM_WAIT_REACHED,
    // The service settled in a DIFFERENT non-pending state (the usual case:
    // STOPPED while waiting for RUNNING, i.e. the start failed).  Waiting
    // longer cannot help; the caller reads the exit code.
    GC_SCM_WAIT_SETTLED_ELSEWHERE,
    // Still pending, but no checkpoint advance within the stall budget.
    GC_SCM_WAIT_STALLED,
    // Still progressing, but the overall bound is spent.
    GC_SCM_WAIT_OVERALL_TIMEOUT
};

struct GcScmWaitTracker {
    unsigned long long startMs;
    unsigned long long lastProgressMs;
    unsigned long lastState;
    unsigned long lastCheckPoint;
    bool seeded;
};

static inline const char* gc_scm_wait_verdict_name(int verdict) {
    switch (verdict) {
        case GC_SCM_WAIT_CONTINUE: return "continue";
        case GC_SCM_WAIT_REACHED: return "reached";
        case GC_SCM_WAIT_SETTLED_ELSEWHERE: return "settled-elsewhere";
        case GC_SCM_WAIT_STALLED: return "stalled";
        case GC_SCM_WAIT_OVERALL_TIMEOUT: return "overall-timeout";
        default: return "unknown";
    }
}

static inline bool gc_scm_state_is_pending(unsigned long state) {
    return state == GC_SCM_STATE_START_PENDING || state == GC_SCM_STATE_STOP_PENDING ||
           state == GC_SCM_STATE_CONTINUE_PENDING || state == GC_SCM_STATE_PAUSE_PENDING;
}

static inline void gc_scm_wait_begin(GcScmWaitTracker* tracker, unsigned long long nowMs) {
    if (!tracker) return;
    tracker->startMs = nowMs;
    tracker->lastProgressMs = nowMs;
    tracker->lastState = 0;
    tracker->lastCheckPoint = 0;
    tracker->seeded = false;
}

static inline unsigned long long gc_scm_wait_stall_budget_ms(unsigned long waitHintMs) {
    if (waitHintMs == 0) return GC_SCM_WAIT_NO_HINT_STALL_MS;
    unsigned long long budget = waitHintMs;
    if (budget < GC_SCM_WAIT_MIN_STALL_MS) budget = GC_SCM_WAIT_MIN_STALL_MS;
    if (budget > GC_SCM_WAIT_MAX_TOTAL_MS) budget = GC_SCM_WAIT_MAX_TOTAL_MS;
    return budget;
}

// The protocol's own sampling rule (a tenth of the hint), clamped so a short
// hint does not spin and a long one does not make a finished start look slow.
static inline unsigned long gc_scm_wait_poll_interval_ms(unsigned long waitHintMs) {
    unsigned long interval = waitHintMs / 10ul;
    if (interval < 250ul) interval = 250ul;
    if (interval > 500ul) interval = 500ul;
    return interval;
}

// Feed one QueryServiceStatusEx sample.  Progress is ANY change of state or
// checkpoint; the first sample counts as progress.
static inline int gc_scm_wait_step(GcScmWaitTracker* tracker, unsigned long long nowMs,
                                   unsigned long state, unsigned long checkPoint,
                                   unsigned long waitHintMs, unsigned long desiredState) {
    if (!tracker) return GC_SCM_WAIT_STALLED;
    if (state == desiredState) return GC_SCM_WAIT_REACHED;
    if (!gc_scm_state_is_pending(state)) return GC_SCM_WAIT_SETTLED_ELSEWHERE;
    if (!tracker->seeded || state != tracker->lastState || checkPoint != tracker->lastCheckPoint) {
        tracker->seeded = true;
        tracker->lastState = state;
        tracker->lastCheckPoint = checkPoint;
        tracker->lastProgressMs = nowMs;
    }
    unsigned long long elapsed = nowMs >= tracker->startMs ? nowMs - tracker->startMs : 0ull;
    if (elapsed >= GC_SCM_WAIT_MAX_TOTAL_MS) return GC_SCM_WAIT_OVERALL_TIMEOUT;
    unsigned long long quiet = nowMs >= tracker->lastProgressMs ? nowMs - tracker->lastProgressMs : 0ull;
    if (quiet > gc_scm_wait_stall_budget_ms(waitHintMs)) return GC_SCM_WAIT_STALLED;
    return GC_SCM_WAIT_CONTINUE;
}

#endif  // GREEN_CURVE_SERVICE_SCM_WAIT_POLICY_H
