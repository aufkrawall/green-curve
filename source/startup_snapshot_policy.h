// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// startup_snapshot_policy.h — keeping the daemon's boot-apply snapshot equal to
// the profile it names.
//
// The daemon runs with `ProtectHome=yes` and cannot read the user's config.ini,
// so a `profile N` boot-apply policy is stored as a *snapshot* of slot N plus
// the exact GPU identity it is bound to.  Until this header existed that
// snapshot was captured once, when the policy was set, and never again: editing
// slot N afterwards left the daemon writing the old values at every boot while
// the TUI's "Startup apply — PROFILE N" control kept claiming the current
// profile.  Observed as a fan hysteresis of 4 °C and a 2000 ms poll interval
// that survived a save, a successful Apply and a reboot, and then came back as
// the 2 °C/1000 ms defaults the snapshot still carried.
//
// The invariant this header exists to enforce:
//
//   while the policy is `profile N`, the stored snapshot equals the on-disk
//   content of profile slot N, or the client says out loud that it does not.
//
// Both halves are pure decisions so they are unit-testable on either host:
//   - client side: what a profile write owes the daemon
//     (`startup_snapshot_sync_after_profile_write`), and whether what the
//     daemon holds still matches the config (`startup_snapshot_state_for`)
//   - daemon side: whether a refresh request may touch the stored record
//     (`startup_snapshot_refresh_allowed`)
//
// A refresh deliberately cannot create, re-slot or re-bind a policy: it only
// replaces the settings of a policy that already names the same slot.  That is
// why saving a profile while a *different* GPU is selected cannot silently move
// the boot-apply to that other GPU.

#ifndef GREEN_CURVE_STARTUP_SNAPSHOT_POLICY_H
#define GREEN_CURVE_STARTUP_SNAPSHOT_POLICY_H

#include "desired_settings_ui_policy.h"
#include "service_protocol.h"

// What a client owes the daemon after it has written profile slot N to disk.
enum StartupSnapshotSync : int {
    // The boot-apply policy does not name the written slot; nothing to do.
    STARTUP_SNAPSHOT_SYNC_NONE = 0,
    // The policy names this slot: push the new content, keeping the stored GPU
    // binding, slot and name exactly as they are.
    STARTUP_SNAPSHOT_SYNC_REFRESH = 1,
    // The policy names this slot but the slot no longer has any content, so the
    // boot-apply would keep writing a profile the user just deleted.  Stop
    // applying at boot instead of resurrecting a ghost.
    STARTUP_SNAPSHOT_SYNC_UNBIND = 2,
    // The policy names this slot but the daemon cannot be reached, so the
    // snapshot stays stale and the user has to be told.
    STARTUP_SNAPSHOT_SYNC_UNREACHABLE = 3,
};

// How the stored snapshot relates to the profile it claims to be.
enum StartupSnapshotState : int {
    // Not a `profile N` policy, so there is no snapshot to diverge.
    STARTUP_SNAPSHOT_STATE_NOT_APPLICABLE = 0,
    // Nothing was read back from the daemon; never claim sync we did not check.
    STARTUP_SNAPSHOT_STATE_UNKNOWN = 1,
    STARTUP_SNAPSHOT_STATE_IN_SYNC = 2,
    // The profile exists but its content differs from what boots.
    STARTUP_SNAPSHOT_STATE_DIVERGED = 3,
    // The policy names a slot that no longer loads at all.
    STARTUP_SNAPSHOT_STATE_PROFILE_MISSING = 4,
};

static inline bool startup_snapshot_slot_is_valid(unsigned int slot) {
    return slot >= 1u && slot <= (unsigned int)CONFIG_NUM_SLOTS;
}

// Is the boot-apply policy bound to the profile slot that was just written?
static inline bool startup_snapshot_policy_binds_slot(unsigned int policyMode,
                                                      unsigned int policySlot,
                                                      int writtenSlot) {
    return policyMode == SERVICE_STARTUP_POLICY_PROFILE &&
           startup_snapshot_slot_is_valid(policySlot) &&
           writtenSlot >= 1 && (unsigned int)writtenSlot == policySlot;
}

static inline StartupSnapshotSync startup_snapshot_sync_after_profile_write(
    bool daemonReachable, unsigned int policyMode, unsigned int policySlot,
    int writtenSlot, bool writtenSlotStillHasContent) {
    if (!startup_snapshot_policy_binds_slot(policyMode, policySlot, writtenSlot))
        return STARTUP_SNAPSHOT_SYNC_NONE;
    // Reachability is checked after the binding so an offline daemon only ever
    // produces a warning for a slot that actually boots.
    if (!daemonReachable) return STARTUP_SNAPSHOT_SYNC_UNREACHABLE;
    return writtenSlotStillHasContent ? STARTUP_SNAPSHOT_SYNC_REFRESH
                                      : STARTUP_SNAPSHOT_SYNC_UNBIND;
}

// Compare what the daemon would write at the next boot against what the config
// says profile `policySlot` contains.  `profileLoaded` is false when the slot
// could not be read (empty or malformed), which is divergence of its own kind:
// the policy names something that no longer exists.
static inline StartupSnapshotState startup_snapshot_state_for(
    unsigned int policyMode, unsigned int policySlot, bool snapshotKnown,
    const DesiredSettings* snapshot, bool profileLoaded,
    const DesiredSettings* profile) {
    if (policyMode != SERVICE_STARTUP_POLICY_PROFILE ||
        !startup_snapshot_slot_is_valid(policySlot))
        return STARTUP_SNAPSHOT_STATE_NOT_APPLICABLE;
    if (!snapshotKnown || !snapshot) return STARTUP_SNAPSHOT_STATE_UNKNOWN;
    if (!profileLoaded || !profile) return STARTUP_SNAPSHOT_STATE_PROFILE_MISSING;
    return desired_settings_equal(snapshot, profile)
        ? STARTUP_SNAPSHOT_STATE_IN_SYNC
        : STARTUP_SNAPSHOT_STATE_DIVERGED;
}

static inline bool startup_snapshot_state_is_stale(StartupSnapshotState state) {
    return state == STARTUP_SNAPSHOT_STATE_DIVERGED ||
           state == STARTUP_SNAPSHOT_STATE_PROFILE_MISSING;
}

// Daemon side.  A refresh may only replace the settings of a policy that
// already is `profile <same slot>`: it can neither create a policy, change its
// mode or slot, nor re-bind it to another GPU.  Anything else must be rejected
// so the boot-apply write stays exactly as authorized.
static inline bool startup_snapshot_refresh_allowed(unsigned int storedMode,
                                                    unsigned int storedSlot,
                                                    unsigned int requestSlot) {
    return storedMode == SERVICE_STARTUP_POLICY_PROFILE &&
           startup_snapshot_slot_is_valid(storedSlot) &&
           storedSlot == requestSlot;
}

#endif  // GREEN_CURVE_STARTUP_SNAPSHOT_POLICY_H
