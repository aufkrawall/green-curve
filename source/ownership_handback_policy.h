// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// What a freshly started Green Curve service/daemon must hand back to the
// driver because a PREVIOUS instance of itself exited without doing so.
//
// A graceful stop returns what Green Curve owned: the Windows service resets
// the GPU to stock (main_service_host.cpp), the Linux daemon returns the fan to
// the driver (daemon_release_fan_to_driver).  A crash, a Task Manager kill, a
// SIGKILL or a wedged-runtime exit skips that step, and the restarted instance
// was deliberately non-mutating -- so a custom fan curve stayed frozen at its
// last duty with nothing tracking temperature, and overclock settings stayed
// applied with nobody owning them.
//
// The startup rule this file implements is:
//
//     Startup never REPLAYS settings.  It HANDS BACK, once, what a previous
//     instance in the same boot provably owned and did not return.
//
// "Provably owned" is an ownership marker written durably BEFORE the first
// hardware write and removed only once a graceful stop (or an explicit Reset)
// has returned that state.  Its presence at start is the crash evidence.
//
// "Same boot" matters: a reboot re-initializes the driver, so the hardware is
// already at stock and another tool may have taken the fan since.  A marker
// from another boot is discarded without a write.
//
// "Once": the marker records a handback in flight BEFORE the handback writes.
// If that write itself kills the process, the next start sees the flag and
// gives up instead of looping (SCM and systemd both restart us indefinitely).
//
// Scope is platform policy, chosen to match what that platform's graceful stop
// already does:
//   - Windows ordinary start       -> FULL: fan to driver auto first, then the
//                                     whole GPU back to stock (the graceful-stop
//                                     reset).
//   - Windows validated controlled -> FAN_ONLY: the recovery write will replay
//     driver recovery                 the full intent after its stability proof;
//                                     until then the fan must not sit frozen.
//   - Linux daemon start           -> FAN_ONLY: a graceful Linux stop keeps the
//                                     clocks by design (restore-last) and hands
//                                     back only the fan.

#ifndef GREEN_CURVE_OWNERSHIP_HANDBACK_POLICY_H
#define GREEN_CURVE_OWNERSHIP_HANDBACK_POLICY_H

#include <string.h>
#include "gpu_core.h"
#include "service_recovery_policy.h"

enum OwnershipHandbackScope : gc_u32 {
    OWNERSHIP_HANDBACK_SCOPE_NONE = 0,
    OWNERSHIP_HANDBACK_SCOPE_FAN_ONLY = 1,
    OWNERSHIP_HANDBACK_SCOPE_FULL = 2,
};

enum OwnershipHandbackVerdict : gc_u32 {
    // No marker: the previous instance stopped cleanly or never wrote.
    OWNERSHIP_HANDBACK_NO_MARKER = 0,
    // Hand back now, with the plan's scope.
    OWNERSHIP_HANDBACK_RUN = 1,
    // The marker belongs to another boot: the driver was re-initialized.
    OWNERSHIP_HANDBACK_DISCARD_OTHER_BOOT = 2,
    // Unreadable/corrupt marker, or the current boot cannot be identified.
    // Unproven is never authorization to write.
    OWNERSHIP_HANDBACK_DISCARD_UNPROVEN = 3,
    // The previous instance died INSIDE its own handback: do not loop.
    OWNERSHIP_HANDBACK_GIVE_UP_PREVIOUS_ATTEMPT_DIED = 4,
};

struct OwnershipHandbackInputs {
    bool markerPresent;
    bool markerValid;
    bool currentBootKnown;
    bool sameBoot;
    bool previousHandbackInFlight;
};

struct OwnershipHandbackPlan {
    OwnershipHandbackVerdict verdict;
    OwnershipHandbackScope scope;
    // Whether the marker file must be deleted now (every non-RUN verdict that
    // found one).  A RUN keeps it until the handback has returned the state.
    bool deleteMarker;
};

static inline OwnershipHandbackPlan ownership_handback_plan(
    const OwnershipHandbackInputs& in, OwnershipHandbackScope startScope) {
    OwnershipHandbackPlan plan = {};
    plan.verdict = OWNERSHIP_HANDBACK_NO_MARKER;
    plan.scope = OWNERSHIP_HANDBACK_SCOPE_NONE;
    if (!in.markerPresent) return plan;
    plan.deleteMarker = true;
    if (!in.markerValid || !in.currentBootKnown) {
        plan.verdict = OWNERSHIP_HANDBACK_DISCARD_UNPROVEN;
        return plan;
    }
    if (!in.sameBoot) {
        plan.verdict = OWNERSHIP_HANDBACK_DISCARD_OTHER_BOOT;
        return plan;
    }
    if (in.previousHandbackInFlight) {
        plan.verdict = OWNERSHIP_HANDBACK_GIVE_UP_PREVIOUS_ATTEMPT_DIED;
        return plan;
    }
    if (startScope == OWNERSHIP_HANDBACK_SCOPE_NONE) {
        // A platform that hands nothing back has nothing to do with the marker
        // except retire it.
        plan.verdict = OWNERSHIP_HANDBACK_DISCARD_UNPROVEN;
        return plan;
    }
    plan.verdict = OWNERSHIP_HANDBACK_RUN;
    plan.scope = startScope;
    plan.deleteMarker = false;
    return plan;
}

// Windows: which scope an ordinary start versus a validated controlled
// driver-recovery start hands back.  See the header comment for why.
static inline OwnershipHandbackScope ownership_handback_windows_start_scope(
    bool controlledRecoveryValidated) {
    return controlledRecoveryValidated ? OWNERSHIP_HANDBACK_SCOPE_FAN_ONLY
                                       : OWNERSHIP_HANDBACK_SCOPE_FULL;
}

// Whether a completed handback retires the marker.  FULL returned everything
// the marker vouches for.  FAN_ONLY on Windows leaves a controlled recovery
// that still owns intent (its own writes re-stamp the marker), so the marker
// stays with the in-flight flag cleared; on Linux the fan IS everything the
// marker vouches for.
static inline bool ownership_handback_retires_marker(
    OwnershipHandbackScope scope, bool markerCoversFanOnly) {
    return scope == OWNERSHIP_HANDBACK_SCOPE_FULL ||
        (scope == OWNERSHIP_HANDBACK_SCOPE_FAN_ONLY && markerCoversFanOnly);
}

static inline const char* ownership_handback_verdict_name(
    OwnershipHandbackVerdict verdict) {
    switch (verdict) {
        case OWNERSHIP_HANDBACK_NO_MARKER: return "no-marker";
        case OWNERSHIP_HANDBACK_RUN: return "run";
        case OWNERSHIP_HANDBACK_DISCARD_OTHER_BOOT: return "discard-other-boot";
        case OWNERSHIP_HANDBACK_DISCARD_UNPROVEN: return "discard-unproven";
        case OWNERSHIP_HANDBACK_GIVE_UP_PREVIOUS_ATTEMPT_DIED:
            return "give-up-previous-attempt-died";
    }
    return "unknown";
}

static inline const char* ownership_handback_scope_name(
    OwnershipHandbackScope scope) {
    switch (scope) {
        case OWNERSHIP_HANDBACK_SCOPE_NONE: return "none";
        case OWNERSHIP_HANDBACK_SCOPE_FAN_ONLY: return "fan-only";
        case OWNERSHIP_HANDBACK_SCOPE_FULL: return "full";
    }
    return "unknown";
}

// --- Windows on-disk marker --------------------------------------------------

#define SERVICE_OWNERSHIP_MARKER_MAGIC 0x4B424347u  // "GCBK"
#define SERVICE_OWNERSHIP_MARKER_VERSION 1u

struct ServiceOwnershipMarker {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    // Nonzero while a handback is writing; see the header comment.
    gc_u32 handbackInFlight;
    ServiceBootIdentity bootIdentity;
    // The adapter the owning instance wrote, so a multi-GPU handback targets it.
    GpuAdapterInfo targetGpu;
};

static inline void service_ownership_marker_initialize(
    ServiceOwnershipMarker* marker, const ServiceBootIdentity& boot,
    const GpuAdapterInfo* target) {
    memset(marker, 0, sizeof(*marker));
    marker->magic = SERVICE_OWNERSHIP_MARKER_MAGIC;
    marker->version = SERVICE_OWNERSHIP_MARKER_VERSION;
    marker->size = (gc_u32)sizeof(*marker);
    marker->bootIdentity = boot;
    if (target) marker->targetGpu = *target;
}

static inline bool service_ownership_marker_valid(
    const ServiceOwnershipMarker* marker) {
    return marker && marker->magic == SERVICE_OWNERSHIP_MARKER_MAGIC &&
        marker->version == SERVICE_OWNERSHIP_MARKER_VERSION &&
        marker->size == (gc_u32)sizeof(*marker) &&
        marker->handbackInFlight <= 1u &&
        service_boot_identity_valid(marker->bootIdentity);
}

// --- Linux on-disk marker ----------------------------------------------------

#define LINUX_FAN_OWNERSHIP_MARKER_MAGIC 0x4E414647u  // "GFAN"
#define LINUX_FAN_OWNERSHIP_MARKER_VERSION 1u
#define LINUX_FAN_OWNERSHIP_BOOT_ID_MAX 40u

struct LinuxFanOwnershipMarker {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 handbackInFlight;
    char bootId[LINUX_FAN_OWNERSHIP_BOOT_ID_MAX];
};

static inline void linux_fan_ownership_marker_initialize(
    LinuxFanOwnershipMarker* marker, const char* bootId) {
    memset(marker, 0, sizeof(*marker));
    marker->magic = LINUX_FAN_OWNERSHIP_MARKER_MAGIC;
    marker->version = LINUX_FAN_OWNERSHIP_MARKER_VERSION;
    marker->size = (gc_u32)sizeof(*marker);
    if (bootId) {
        size_t length = strlen(bootId);
        if (length < sizeof(marker->bootId))
            memcpy(marker->bootId, bootId, length);
    }
}

static inline bool linux_fan_ownership_marker_valid(
    const LinuxFanOwnershipMarker* marker) {
    if (!marker || marker->magic != LINUX_FAN_OWNERSHIP_MARKER_MAGIC ||
        marker->version != LINUX_FAN_OWNERSHIP_MARKER_VERSION ||
        marker->size != (gc_u32)sizeof(*marker) ||
        marker->handbackInFlight > 1u || !marker->bootId[0])
        return false;
    // Terminated inside the field, printable, no stray bytes after it.
    bool terminated = false;
    for (size_t i = 0; i < sizeof(marker->bootId); ++i) {
        char c = marker->bootId[i];
        if (terminated) {
            if (c != 0) return false;
            continue;
        }
        if (c == 0) { terminated = true; continue; }
        if (c < 0x21 || c > 0x7e) return false;
    }
    return terminated;
}

#endif  // GREEN_CURVE_OWNERSHIP_HANDBACK_POLICY_H
