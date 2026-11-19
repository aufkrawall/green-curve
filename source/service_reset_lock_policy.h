// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_SERVICE_RESET_LOCK_POLICY_H
#define GREEN_CURVE_SERVICE_RESET_LOCK_POLICY_H

// What an explicit Reset leaves of the service's own clock-lock record, and
// which lock a service snapshot reports from that record.
//
// The service keeps its lock markers (locked* and appliedLock* on the App
// state) as an explicit record.  Apply restores or clears them once its
// transaction succeeds, and live detection only refines a lock the service
// still owns (gpu_lock_ownership_policy.h): it never clears a lock it no longer
// owns, because it must not touch markers on behalf of an external curve.
// Reset drops the owning intent before its post-reset refresh, so unless Reset
// clears the markers itself they outlive the lock: every snapshot after a
// successful Reset then reports the old lock, and the GUI adopts it again (the
// lock checkbox, the flattened tail, the "Lock:" header and the OC tray icon)
// while the driver is at stock.
//
// Requires apply_clock_ceiling_policy.h (ApplyRecoveryResult) and the LockMode
// enum to be visible at the point of inclusion.

// Whether a Reset leaves no Green Curve clock lock of any kind.  Both halves
// are needed: a VF curve verified back to stock under a retained clock cap, or
// a released cap over a curve that did not verify, still holds the clocks.  In
// either case the markers stay, because they are what a retried Reset consults
// to know that a restriction may still be active.
static inline bool service_reset_releases_lock_markers(
    const ApplyRecoveryResult& recovery) {
    return apply_recovery_permits_release(recovery) &&
        recovery.restrictionReleased;
}

// Clear every lock marker.  Shared by Reset and by a successful Apply that
// requested no lock, so the two cannot drift apart field by field.
template <class AppState>
static inline void service_clear_lock_markers(AppState* app) {
    if (!app) return;
    app->lockedVi = -1;
    app->lockedCi = -1;
    app->lockedFreq = 0;
    app->lockMode = LOCK_MODE_NONE;
    app->appliedLockVi = -1;
    app->appliedLockCi = -1;
    app->appliedLockFreq = 0;
    app->appliedLockMode = LOCK_MODE_NONE;
    app->guiLockTracksAnchor = true;
}

// Whether any lock marker still claims a lock.  Used for the Reset diagnostic.
template <class AppState>
static inline bool service_lock_markers_claim_lock(const AppState& app) {
    return (app.lockedCi >= 0 && app.lockedFreq > 0) ||
        (app.appliedLockCi >= 0 && app.appliedLockFreq > 0) ||
        app.lockMode != LOCK_MODE_NONE || app.appliedLockMode != LOCK_MODE_NONE;
}

// The lock a service snapshot reports.  The active desired lock wins when the
// caller has established that it is in force; otherwise the markers are the
// only source, which is why a Reset must not leave them behind.
struct ServiceSnapshotLockProjection {
    bool hasLock;
    int lockCi;
    unsigned int lockMHz;
    int lockMode;
    bool lockTracksAnchor;
};

template <class AppState, class Desired>
static inline ServiceSnapshotLockProjection service_snapshot_lock_projection(
    bool reportActiveDesired, const Desired& activeDesired,
    const AppState& app) {
    ServiceSnapshotLockProjection out{};
    if (reportActiveDesired) {
        out.hasLock = true;
        out.lockCi = activeDesired.lockCi;
        out.lockMHz = activeDesired.lockMHz;
        out.lockMode = (int)activeDesired.lockMode;
        out.lockTracksAnchor = activeDesired.lockTracksAnchor;
        return out;
    }
    out.hasLock = app.lockedCi >= 0 && app.lockedFreq > 0;
    out.lockCi = app.lockedCi;
    out.lockMHz = app.lockedFreq;
    out.lockMode = (int)app.lockMode;
    out.lockTracksAnchor = app.guiLockTracksAnchor;
    return out;
}

#endif
