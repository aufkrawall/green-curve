// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_FAN_WORKER_LOCK_WAIT_WIN32_H
#define GREEN_CURVE_FAN_WORKER_LOCK_WAIT_WIN32_H

// The Win32 half of the fan worker's cancellable runtime-mutex acquisition,
// kept free of service globals so the native regression fixture
// (tests/fan_worker_lock_tests.cpp) runs this exact call against real threads,
// events and mutexes.
//
// The contract the service depends on: whoever stops the fan worker keeps
// the runtime mutex for the whole join, so a worker queued on that mutex must
// be able to leave on the stop event alone -- and when the stop and the mutex
// are both available, the stop wins, so a stopped worker never runs one more
// pulse.  Both follow from the handle ORDER below: WaitForMultipleObjects
// reports the lowest signaled index, and the stop event is index 0.

#include <windows.h>

#include "fan_worker_lifecycle_policy.h"

static_assert(FAN_WORKER_WAIT_OBJECT_0 == WAIT_OBJECT_0,
    "fan_worker_lifecycle_policy.h mirrors WAIT_OBJECT_0");
static_assert(FAN_WORKER_WAIT_ABANDONED_0 == WAIT_ABANDONED_0,
    "fan_worker_lifecycle_policy.h mirrors WAIT_ABANDONED_0");

// Waits for `stopEvent` or `runtimeMutex`, whichever comes first, stop first.
// On FAN_WORKER_LOCK_ACQUIRED or FAN_WORKER_LOCK_ABANDONED the caller owns the
// mutex.  `rawWaitOut` receives the WaitForMultipleObjects result for logs.
static inline FanWorkerLockWait fan_worker_wait_for_runtime_lock(HANDLE stopEvent,
                                                                 HANDLE runtimeMutex,
                                                                 DWORD* rawWaitOut) {
    HANDLE handles[2] = { stopEvent, runtimeMutex };
    DWORD waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (rawWaitOut) *rawWaitOut = waitResult;
    return fan_worker_lock_wait_outcome(waitResult);
}

#endif  // GREEN_CURVE_FAN_WORKER_LOCK_WAIT_WIN32_H
