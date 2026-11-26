// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// linux_gpu_controller.h -- the Linux daemon's state for one physical GPU.
//
// Everything the daemon knows or owns about a GPU lives here: the bound
// backend, the committed intent and its target, uncertainty, the operation
// journal tracker, startup policy, the crash-loop guard, fan ownership and
// fault, and the identity generation.  Functions that read or change any of
// it, or touch the GPU, take the controller explicitly
// (`LinuxGpuController& ctl`); only the daemon's entry points name the
// instance.  There is no implicit "current GPU".
//
// Process-wide state stays outside: the driver libraries
// (LinuxDriverContext), the request/hardware lock, the fan worker's wake
// signal, the shutdown pipe, socket admission and the service identity.
//
// The daemon still binds one GPU; this is the per-GPU unit a later change can
// instantiate once per card.

#ifndef GREEN_CURVE_LINUX_GPU_CONTROLLER_H
#define GREEN_CURVE_LINUX_GPU_CONTROLLER_H

#include "linux_backend.h"
#include "linux_daemon_state.h"
#include "linux_auto_restore_policy.h"
#include "ownership_handback_policy.h"
#include "service_auto_apply_failure.h"
#include "service_fan_fault.h"
#include "service_operation_tracker.h"

struct LinuxGpuController {
    // The bound backend; gpuReady once an init succeeded.
    LinuxGpuState gpu = {};
    bool gpuReady = false;
    // A read-time retry may initialize an unavailable backend once per
    // automatic attempt (linux_daemon_snapshot_runtime.cpp).
    bool backendReadInitAttempted = false;
    // Bumped whenever the bound GPU identity changes; clients compare it.
    gc_u64 gpuGeneration = 1;

    // The committed intent and the GPU it belongs to.
    DesiredSettings activeDesired = {};
    bool hasActiveDesired = false;
    GpuAdapterInfo activeTarget = {};
    LinuxGpuUuid activeTargetUuid = {};
    GpuLocationRole activeTargetRole = GPU_LOCATION_ROLE_REQUIRED;
    bool stateUncertain = false;
    ServiceOperationTracker operationTracker = {};

    // The committed startup-apply policy.  Zero-initialized means
    // RESTORE_LAST, which is exactly what every build before protocol v13 did.
    LinuxDaemonStartupRecord startupPolicy = {};
    // Whether an unattended write is still allowed.  See
    // linux_auto_restore_policy.h; the persisted copy lives in
    // GC_DAEMON_GUARD_FILE.
    LinuxAutoRestoreGuard autoRestoreGuard = {};
    // The protocol-v29 cause of an AUTOMATIC_APPLY_FAILED latched by THIS
    // daemon process (linux_auto_restore_runtime.h).  In memory only, like
    // Windows: after a restart the persisted lockout is published without a
    // cause.
    ServiceAutoApplyFailure autoApplyFailure = {};

    // Fan ownership (linux_fan_ownership.h) and the worker's failure count.
    bool fanOwnershipMarkerCommitted = false;
    bool fanOwnershipHandbackPending = false;
    // Startup found inherited ownership but no usable backend.
    bool fanOwnershipHandbackDeferred = false;
    LinuxFanOwnershipMarker fanOwnershipRecord = {};
    unsigned int fanFailureCount = 0;
    // This GPU's fan command generation: bumped (under g_fanWakeMutex) by
    // every Apply/Reset/state change that wakes its fan worker. It resets the
    // curve hysteresis and is the stall detector's command identity, so
    // another GPU's wake must not advance it.
    unsigned long long fanWakeGeneration = 0;
    // v39: the last fan the worker handed back (linux_fan_runtime.h).  In
    // memory only; guarded by the daemon lock.  An explicit Apply or Reset
    // clears it, as it clears the uncertainty the lockout entered.  Its
    // monotonic serial counter stays process-wide (g_daemonFanFaultSerial)
    // because clients deduplicate announcements on (serial, kind).
    ServiceFanFault fanFault = {};
};

// Boot-only initialization into canonical defaults (fail-closed target role,
// first generation). Call only on an unbound controller, never over a live
// GPU state without shutting it down first. In-place reset avoids a large
// temporary on the stack.
static inline void linux_gpu_controller_init(LinuxGpuController* ctl) {
    if (!ctl) return;
    memset(ctl, 0, sizeof(*ctl));
    ctl->gpuGeneration = 1;
    ctl->activeTargetRole = GPU_LOCATION_ROLE_REQUIRED;
}

#endif
