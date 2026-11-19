// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// What an explicit Reset leaves of the service's lock record, and which lock
// the service snapshot reports from it (service_reset_lock_policy.h).
//
// Reported 2026-11-19 on a development build: after a successful Reset of a profile
// with a flatten lock, the driver was at stock but the GUI showed the lock
// checkbox, the flattened tail and the OC tray icon again, because the RESET
// envelope still reported the lock from markers the Reset never cleared.

#include <stdio.h>

namespace service_reset_lock_fixture {

enum LockMode : int { LOCK_MODE_NONE = 0, LOCK_MODE_FLATTEN = 1, LOCK_MODE_HARD = 2 };

#include "apply_clock_ceiling_policy.h"
#include "service_reset_lock_policy.h"

struct AppLock {
    int lockedVi;
    int lockedCi;
    unsigned int lockedFreq;
    LockMode lockMode;
    bool guiLockTracksAnchor;
    int appliedLockVi;
    int appliedLockCi;
    unsigned int appliedLockFreq;
    LockMode appliedLockMode;
};

struct Desired {
    bool hasLock;
    int lockCi;
    unsigned int lockMHz;
    LockMode lockMode;
    bool lockTracksAnchor;
};

// The state the service holds after applying the reported profile: a flatten
// lock at 2957 MHz anchored on point 76, recorded both as live and applied.
static AppLock applied_flatten_lock() {
    AppLock app{};
    app.lockedVi = 36;
    app.lockedCi = 76;
    app.lockedFreq = 2957;
    app.lockMode = LOCK_MODE_FLATTEN;
    app.guiLockTracksAnchor = false;
    app.appliedLockVi = 36;
    app.appliedLockCi = 76;
    app.appliedLockFreq = 2957;
    app.appliedLockMode = LOCK_MODE_FLATTEN;
    return app;
}

static ApplyRecoveryResult verified_reset(bool curve, bool gpuOffset, bool released) {
    ApplyRecoveryResult r{};
    r.curve.attempted = true;
    r.curve.verified = curve;
    r.gpuOffset.verified = gpuOffset;
    r.restrictionReleased = released;
    return r;
}

static bool markers_fully_clear(const AppLock& app) {
    return app.lockedVi == -1 && app.lockedCi == -1 && app.lockedFreq == 0 &&
        app.lockMode == LOCK_MODE_NONE && app.appliedLockVi == -1 &&
        app.appliedLockCi == -1 && app.appliedLockFreq == 0 &&
        app.appliedLockMode == LOCK_MODE_NONE && app.guiLockTracksAnchor;
}

static int run() {
    const Desired noIntent{};

    // Before Reset the active desired lock is what the snapshot reports.
    {
        AppLock app = applied_flatten_lock();
        const Desired active{true, 76, 2957, LOCK_MODE_FLATTEN, true};
        ServiceSnapshotLockProjection p =
            service_snapshot_lock_projection(true, active, app);
        if (!p.hasLock || p.lockCi != 76 || p.lockMHz != 2957 ||
            p.lockMode != LOCK_MODE_FLATTEN || !p.lockTracksAnchor) return 12240;
    }

    // The reported defect: Reset dropped the intent but kept the markers, so
    // the snapshot (now reading the markers) still reported the lock.  This is
    // exactly what the RESET envelope carried before the fix.
    {
        AppLock app = applied_flatten_lock();
        ServiceSnapshotLockProjection stale =
            service_snapshot_lock_projection(false, noIntent, app);
        if (!stale.hasLock || stale.lockCi != 76 || stale.lockMHz != 2957) return 12241;
    }

    // A fully verified Reset clears every marker, and the snapshot then
    // reports no lock at all.
    {
        AppLock app = applied_flatten_lock();
        if (!service_lock_markers_claim_lock(app)) return 12242;
        const ApplyRecoveryResult r = verified_reset(true, true, true);
        if (!service_reset_releases_lock_markers(r)) return 12243;
        service_clear_lock_markers(&app);
        if (!markers_fully_clear(app)) return 12244;
        if (service_lock_markers_claim_lock(app)) return 12245;
        ServiceSnapshotLockProjection p =
            service_snapshot_lock_projection(false, noIntent, app);
        if (p.hasLock || p.lockCi != -1 || p.lockMHz != 0 ||
            p.lockMode != LOCK_MODE_NONE) return 12246;
    }

    // A Reset that kept a clock restriction keeps the markers: they are what a
    // retried Reset consults to know a restriction may still be active, and
    // the snapshot keeps reporting the lock that is still holding the clocks.
    if (service_reset_releases_lock_markers(verified_reset(false, true, false)) ||
        service_reset_releases_lock_markers(verified_reset(true, false, false)) ||
        service_reset_releases_lock_markers(verified_reset(true, true, false))) return 12247;
    // A released restriction over a curve or scalar that did not verify is not
    // a stock clock domain either.
    if (service_reset_releases_lock_markers(verified_reset(false, true, true)) ||
        service_reset_releases_lock_markers(verified_reset(true, false, true)) ||
        service_reset_releases_lock_markers(ApplyRecoveryResult{})) return 12248;

    // Any single marker is enough to claim a lock (a HARD pin may be recorded
    // only as an applied mode), and clearing a null state is harmless.
    {
        AppLock app{};
        app.lockedCi = app.appliedLockCi = app.lockedVi = app.appliedLockVi = -1;
        if (service_lock_markers_claim_lock(app)) return 12249;
        app.appliedLockMode = LOCK_MODE_HARD;
        if (!service_lock_markers_claim_lock(app)) return 12250;
        app.appliedLockMode = LOCK_MODE_NONE;
        app.appliedLockCi = 80;
        app.appliedLockFreq = 2800;
        if (!service_lock_markers_claim_lock(app)) return 12251;
        service_clear_lock_markers(static_cast<AppLock*>(nullptr));
    }

    // A marker index without a frequency is not a reported lock.
    {
        AppLock app = applied_flatten_lock();
        app.lockedFreq = 0;
        if (service_snapshot_lock_projection(false, noIntent, app).hasLock) return 12252;
    }
    return 0;
}

}  // namespace service_reset_lock_fixture

int run_service_reset_lock_tests() {
    return service_reset_lock_fixture::run();
}

#ifdef GC_SERVICE_RESET_LOCK_TEST_MAIN
int main() {
    const int result = run_service_reset_lock_tests();
    printf("Service reset lock regression: %d\n", result);
    return result;
}
#endif
