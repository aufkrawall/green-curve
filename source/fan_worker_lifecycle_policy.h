// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_FAN_WORKER_LIFECYCLE_POLICY_H
#define GREEN_CURVE_FAN_WORKER_LIFECYCLE_POLICY_H

// Pure rules for starting, stopping and serializing the Windows service fan
// worker thread.  Windows-free so the regression harness exercises them on
// both hosts; main_service_fan_worker.cpp is the only runtime consumer.
//
// Two defects shaped these rules:
//
//  1. The worker escalates its own repeated failures, and that escalation
//     stops the fan runtime, which asked the thread to stop -- from inside the
//     thread.  Joining yourself always times out, and the join used to drop the
//     runtime mutex for those five seconds while the escalation still held
//     g_appLock.  A telemetry request taking the mutex and then g_appLock in
//     that window deadlocked the service (ABBA) until the wedge watchdog forced
//     an emergency restart.  A stop requested by the worker itself is
//     therefore SIGNAL ONLY: the worker leaves at the top of its next loop.
//
//  2. Every other stop released the runtime mutex while it waited, because a
//     worker queued on that mutex could not otherwise see the stop.  That
//     opened a window in the middle of an Apply or a Reset in which the
//     lifecycle worker (logon / resume restore) or the updater could run a
//     whole hardware transaction of their own.  The worker's acquisition now
//     waits on the stop event AND the mutex, so a stop reaches it wherever it
//     waits and the stopper keeps the mutex for the whole join.

enum FanWorkerStopPlan {
    // No worker handle: nothing to stop.
    FAN_WORKER_STOP_NOTHING = 0,
    // The caller IS the worker.  Signal and return; the loop exits on its own.
    FAN_WORKER_STOP_SIGNAL_ONLY = 1,
    // Any other thread: signal, then join WITHOUT releasing the runtime mutex.
    FAN_WORKER_STOP_SIGNAL_AND_JOIN = 2,
};

static inline FanWorkerStopPlan fan_worker_stop_plan(bool workerPresent,
                                                     bool callerIsWorker) {
    if (!workerPresent) return FAN_WORKER_STOP_NOTHING;
    return callerIsWorker ? FAN_WORKER_STOP_SIGNAL_ONLY
                          : FAN_WORKER_STOP_SIGNAL_AND_JOIN;
}

static inline const char* fan_worker_stop_plan_name(FanWorkerStopPlan plan) {
    switch (plan) {
        case FAN_WORKER_STOP_NOTHING: return "nothing";
        case FAN_WORKER_STOP_SIGNAL_ONLY: return "signal-only (self)";
        case FAN_WORKER_STOP_SIGNAL_AND_JOIN: return "signal-and-join";
    }
    return "unknown";
}

enum FanWorkerEnsurePlan {
    // No worker, or its handle was already reaped: create one.
    FAN_WORKER_ENSURE_CREATE = 0,
    // A live worker with no pending stop: keep it.
    FAN_WORKER_ENSURE_ALREADY_RUNNING = 1,
    // The handle names a thread that has exited: close it, then create.
    FAN_WORKER_ENSURE_REAP_THEN_CREATE = 2,
    // A live worker that has been told to stop (for example by its own
    // failure escalation) is on its way out.  Treating it as "already running"
    // left the runtime undriven until the next watchdog tick; resetting the
    // stop event under it would cancel a stop somebody asked for.  Join it,
    // then create a fresh one.
    FAN_WORKER_ENSURE_JOIN_RETIRING_THEN_CREATE = 3,
    // The worker cannot replace itself; the caller is a bug, not a request.
    FAN_WORKER_ENSURE_REFUSE_SELF = 4,
};

static inline FanWorkerEnsurePlan fan_worker_ensure_plan(bool workerPresent,
                                                         bool workerAlive,
                                                         bool stopSignaled,
                                                         bool callerIsWorker) {
    if (!workerPresent) return FAN_WORKER_ENSURE_CREATE;
    if (!workerAlive) return FAN_WORKER_ENSURE_REAP_THEN_CREATE;
    if (!stopSignaled) return FAN_WORKER_ENSURE_ALREADY_RUNNING;
    return callerIsWorker ? FAN_WORKER_ENSURE_REFUSE_SELF
                          : FAN_WORKER_ENSURE_JOIN_RETIRING_THEN_CREATE;
}

static inline const char* fan_worker_ensure_plan_name(FanWorkerEnsurePlan plan) {
    switch (plan) {
        case FAN_WORKER_ENSURE_CREATE: return "create";
        case FAN_WORKER_ENSURE_ALREADY_RUNNING: return "already-running";
        case FAN_WORKER_ENSURE_REAP_THEN_CREATE: return "reap-then-create";
        case FAN_WORKER_ENSURE_JOIN_RETIRING_THEN_CREATE: return "join-retiring-then-create";
        case FAN_WORKER_ENSURE_REFUSE_SELF: return "refuse-self";
    }
    return "unknown";
}

// The worker's runtime-mutex acquisition waits on {stop event, mutex} in that
// order.  The values mirror WAIT_OBJECT_0 / WAIT_ABANDONED_0 so this header
// needs no windows.h; the Windows consumer static_asserts that they match.
#define FAN_WORKER_WAIT_OBJECT_0 0x00000000ul
#define FAN_WORKER_WAIT_ABANDONED_0 0x00000080ul

enum FanWorkerLockWait {
    FAN_WORKER_LOCK_ACQUIRED = 0,
    FAN_WORKER_LOCK_STOP_REQUESTED = 1,
    // The previous owner died holding the mutex: the caller must treat the
    // runtime as poisoned exactly like every other acquisition does.
    FAN_WORKER_LOCK_ABANDONED = 2,
    FAN_WORKER_LOCK_FAILED = 3,
};

static inline FanWorkerLockWait fan_worker_lock_wait_outcome(unsigned long waitResult) {
    // Index 0 (the stop event) wins when both are signaled, so a stop that
    // arrives together with the mutex never runs one more pulse.
    if (waitResult == FAN_WORKER_WAIT_OBJECT_0) return FAN_WORKER_LOCK_STOP_REQUESTED;
    if (waitResult == FAN_WORKER_WAIT_OBJECT_0 + 1) return FAN_WORKER_LOCK_ACQUIRED;
    if (waitResult == FAN_WORKER_WAIT_ABANDONED_0 + 1) return FAN_WORKER_LOCK_ABANDONED;
    // An abandoned EVENT is impossible, and a timeout cannot happen on an
    // INFINITE wait; both, like WAIT_FAILED, are failures.
    return FAN_WORKER_LOCK_FAILED;
}

#endif // GREEN_CURVE_FAN_WORKER_LIFECYCLE_POLICY_H
