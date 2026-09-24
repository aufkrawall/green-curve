// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Native fixture for the fan worker's cancellable runtime-mutex wait
// (fan_worker_lock_wait_win32.h), assertion codes 6550-6569.  Real threads,
// events and mutexes; every outcome is forced by ownership, not by timing.
// The bounded joins below only turn a regression (a hang) into a failure
// code; no assertion depends on how long anything takes.
//
// Windows only: the POSIX build of the harness runs nothing here.

#if defined(_WIN32)

#include "fan_worker_lock_wait_win32.h"

namespace {

// A hang detector, never a pass condition.
constexpr DWORD kHangMs = 10000;

struct Waiter {
    HANDLE stopEvent;
    HANDLE mutex;
    HANDLE readyEvent;       // set just before the waiter blocks
    FanWorkerLockWait result;
    DWORD raw;
    bool releasedOwned;      // ReleaseMutex succeeded after an acquisition
};

DWORD WINAPI waiter_proc(void* p) {
    Waiter* w = (Waiter*)p;
    SetEvent(w->readyEvent);
    w->result = fan_worker_wait_for_runtime_lock(w->stopEvent, w->mutex, &w->raw);
    if (w->result == FAN_WORKER_LOCK_ACQUIRED || w->result == FAN_WORKER_LOCK_ABANDONED)
        w->releasedOwned = ReleaseMutex(w->mutex) != FALSE;
    return 0;
}

DWORD WINAPI abandon_proc(void* p) {
    // Take the mutex and exit while owning it.
    return WaitForSingleObject((HANDLE)p, INFINITE) == WAIT_OBJECT_0 ? 0 : 1;
}

struct Handles {
    HANDLE stop = nullptr, mutex = nullptr, ready = nullptr;
    ~Handles() {
        if (stop) CloseHandle(stop);
        if (mutex) CloseHandle(mutex);
        if (ready) CloseHandle(ready);
    }
};

bool make_handles(Handles* h, bool ownMutex) {
    h->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    h->mutex = CreateMutexW(nullptr, ownMutex ? TRUE : FALSE, nullptr);
    h->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return h->stop && h->mutex && h->ready;
}

// Runs a waiter thread; `whileQueued` acts once it has announced itself.
template <typename Act>
int run_waiter(Handles* h, Waiter* w, Act whileQueued) {
    *w = Waiter{h->stop, h->mutex, h->ready, FAN_WORKER_LOCK_FAILED, 0, false};
    HANDLE thread = CreateThread(nullptr, 0, waiter_proc, w, 0, nullptr);
    if (!thread) return 1;
    int rc = 0;
    if (WaitForSingleObject(h->ready, kHangMs) != WAIT_OBJECT_0) rc = 2;
    else whileQueued();
    if (!rc && WaitForSingleObject(thread, kHangMs) != WAIT_OBJECT_0) rc = 3;
    CloseHandle(thread);
    return rc;
}

}  // namespace

int run_fan_worker_lock_tests() {
    // 6550: a free mutex and no stop -> acquired, and really owned.
    {
        Handles h;
        if (!make_handles(&h, false)) return 6550;
        DWORD raw = 0;
        if (fan_worker_wait_for_runtime_lock(h.stop, h.mutex, &raw) != FAN_WORKER_LOCK_ACQUIRED ||
            raw != WAIT_OBJECT_0 + 1 || !ReleaseMutex(h.mutex)) return 6551;
    }
    // 6552: stop and a free mutex together -> the stop wins, and the mutex is
    // NOT taken (a stopped worker never runs one more pulse).
    {
        Handles h;
        if (!make_handles(&h, false)) return 6552;
        SetEvent(h.stop);
        if (fan_worker_wait_for_runtime_lock(h.stop, h.mutex, nullptr) !=
                FAN_WORKER_LOCK_STOP_REQUESTED) return 6553;
        SetLastError(0);
        if (ReleaseMutex(h.mutex) || GetLastError() != ERROR_NOT_OWNER) return 6554;
    }
    // 6555: THE deadlock contract.  The stopper holds the runtime mutex (an
    // Apply in progress), the worker is queued on it, the stopper signals and
    // joins WITHOUT releasing the mutex: the join completes, the worker leaves
    // on the stop, and the stopper still owns the mutex afterwards.
    {
        Handles h;
        if (!make_handles(&h, true)) return 6555;
        Waiter w;
        int rc = run_waiter(&h, &w, [&]() { SetEvent(h.stop); });
        if (rc) return 6555 + rc;                       // 6556..6558
        if (w.result != FAN_WORKER_LOCK_STOP_REQUESTED || w.releasedOwned) return 6559;
        if (!ReleaseMutex(h.mutex)) return 6560;         // still ours throughout
    }
    // 6561: no stop -- the worker queued behind an Apply acquires once the
    // Apply releases, and owns the mutex it was given.
    {
        Handles h;
        if (!make_handles(&h, true)) return 6561;
        Waiter w;
        int rc = run_waiter(&h, &w, [&]() { ReleaseMutex(h.mutex); });
        if (rc) return 6561 + rc;                       // 6562..6564
        if (w.result != FAN_WORKER_LOCK_ACQUIRED || !w.releasedOwned) return 6565;
    }
    // 6566: a previous owner that died holding the mutex is reported as
    // ABANDONED (the service treats the runtime as poisoned) and the waiter
    // owns the mutex.
    {
        Handles h;
        if (!make_handles(&h, false)) return 6566;
        HANDLE dead = CreateThread(nullptr, 0, abandon_proc, h.mutex, 0, nullptr);
        if (!dead) return 6567;
        DWORD joined = WaitForSingleObject(dead, kHangMs);
        DWORD exitCode = 1;
        GetExitCodeThread(dead, &exitCode);
        CloseHandle(dead);
        if (joined != WAIT_OBJECT_0 || exitCode != 0) return 6567;
        DWORD raw = 0;
        if (fan_worker_wait_for_runtime_lock(h.stop, h.mutex, &raw) != FAN_WORKER_LOCK_ABANDONED ||
            raw != WAIT_ABANDONED_0 + 1) return 6568;
        if (!ReleaseMutex(h.mutex)) return 6569;
    }
    return 0;
}

#else

int run_fan_worker_lock_tests() { return 0; }

#endif  // _WIN32
