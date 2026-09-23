// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// When the Windows service's wedge watchdog treats hardware work as hung.
//
// Windows itself cannot tell: the service manager only acts when a service
// process exits or misses a start/stop deadline, and the kernel's GPU timeout
// detection only resets a hung GPU, not a user-mode thread deadlocked inside
// nvml.dll/nvapi64 (a stale handle after a driver update).  The service's main
// thread therefore watches its own hardware work.
//
// It used to watch only a queued fan pulse, so an Apply, a Reset or the
// crash handback that hung while no fan curve was running was never detected
// and the service stayed unresponsive until someone restarted it.  Every such
// section now runs inside a ServiceHardwareWorkScope (service_gate_progress.h)
// and stamps progress as it goes; the watchdog restarts the process through
// the existing controlled-recovery path when a scope stops making progress.
//
// Progress, not duration, is the signal (the 2026-09-17 lesson: a slow
// under-load apply is alive and keeps stamping).  The timeout is well above
// the longest single step that does not stamp -- the reset settle (<= 5 s) or
// one driver setControl (~1 s) -- and tolerant of a short disk stall, because a
// false positive costs the user their Apply.

#ifndef GREEN_CURVE_SERVICE_WEDGE_WATCHDOG_POLICY_H
#define GREEN_CURVE_SERVICE_WEDGE_WATCHDOG_POLICY_H

#define SERVICE_HARDWARE_WORK_WEDGE_TIMEOUT_MS 30000ull

// Age of the last progress stamp.  A stamp newer than `nowMs` (written by
// another thread after `nowMs` was read) or no stamp yet is age zero: the
// watchdog may only ever be late, never early.
static inline unsigned long long service_progress_age_ms(
    unsigned long long nowMs, unsigned long long lastProgressMs) {
    if (lastProgressMs == 0 || lastProgressMs >= nowMs) return 0;
    return nowMs - lastProgressMs;
}

static inline bool service_hardware_work_is_wedged(bool workInFlight,
    unsigned long long progressAgeMs) {
    return workInFlight && progressAgeMs > SERVICE_HARDWARE_WORK_WEDGE_TIMEOUT_MS;
}

#endif  // GREEN_CURVE_SERVICE_WEDGE_WATCHDOG_POLICY_H
