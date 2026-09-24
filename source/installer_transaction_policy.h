// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The installer transaction's ordering is host-neutral so each failure edge
// can be injected by the regression harness without touching a real service.
#pragma once

#include <stdint.h>

// A failed service stop happens before the first file or record write, so it
// has its own recovery: there is nothing on disk to restore, and repeating the
// stop that just failed would only wait out its timeout again and then report
// an untouched installation as an unrecoverable one.
template <typename Stop, typename StopFailed, typename Replace, typename Record,
          typename Register, typename Rollback>
static bool gc_run_install_transaction(uint32_t fileCount, Stop stop,
                                        StopFailed stopFailed,
                                        Replace replace, Record record,
                                        Register service, Rollback rollback) {
    if (!stop()) {
        stopFailed();
        return false;
    }
    for (uint32_t i = 0; i < fileCount; ++i) {
        if (!replace(i)) {
            rollback(false);
            return false;
        }
    }
    // A partial registry write is possible even when the call returns false.
    // From here every failure must restore that saved record as well.
    if (!record()) {
        rollback(true);
        return false;
    }
    if (!service()) {
        rollback(true);
        return false;
    }
    return true;
}

template <typename Changed, typename HadPrevious, typename RestorePrevious,
          typename RemoveNew>
static bool gc_restore_install_files(uint32_t fileCount, Changed changed,
                                     HadPrevious hadPrevious,
                                     RestorePrevious restorePrevious,
                                     RemoveNew removeNew) {
    bool complete = true;
    for (uint32_t i = 0; i < fileCount; ++i) {
        if (!changed(i)) continue;
        // Continue after one failure: every independently restorable file
        // still gets its old bytes back, and the caller keeps all backups.
        if (!(hadPrevious(i) ? restorePrevious(i) : removeNew(i))) complete = false;
    }
    return complete;
}

static inline bool gc_rollback_can_reapply_settings(bool serviceWasRunning,
                                                     bool priorSupportsTransfer,
                                                     bool haveSnapshot,
                                                     bool havePriorDirectory) {
    return serviceWasRunning && priorSupportsTransfer && haveSnapshot &&
           havePriorDirectory;
}

// What the service is doing once a stop request has failed. Host-neutral so
// every combination is reachable from the regression harness.
enum GcStopFailureServiceState {
    GC_STOP_FAILURE_SERVICE_ABSENT,
    GC_STOP_FAILURE_SERVICE_STOPPED,
    GC_STOP_FAILURE_SERVICE_ACTIVE,   // running, or start/stop/pause pending
    GC_STOP_FAILURE_SERVICE_UNKNOWN,  // the SCM could not be queried
};

enum GcStopFailureRecovery {
    GC_STOP_FAILURE_NOTHING_TO_DO,
    GC_STOP_FAILURE_RESTART_PREVIOUS,
    GC_STOP_FAILURE_LEAVE_ACTIVE,
    GC_STOP_FAILURE_STATE_UNKNOWN,
};

// The files and registrations are the previous installation's, untouched.
// The only thing a failed stop can have changed is whether that previous
// service still runs: one that did stop is started again, one that is still
// active is left alone (it keeps running the unchanged old binaries, or is
// finishing its own shutdown), and it is never stopped a second time.
static inline GcStopFailureRecovery gc_stop_failure_recovery(
        bool serviceWasRunning, GcStopFailureServiceState now) {
    switch (now) {
    case GC_STOP_FAILURE_SERVICE_ACTIVE: return GC_STOP_FAILURE_LEAVE_ACTIVE;
    case GC_STOP_FAILURE_SERVICE_STOPPED:
        return serviceWasRunning ? GC_STOP_FAILURE_RESTART_PREVIOUS
                                 : GC_STOP_FAILURE_NOTHING_TO_DO;
    case GC_STOP_FAILURE_SERVICE_ABSENT: return GC_STOP_FAILURE_NOTHING_TO_DO;
    case GC_STOP_FAILURE_SERVICE_UNKNOWN:
    default: return GC_STOP_FAILURE_STATE_UNKNOWN;
    }
}

static inline const char* gc_stop_failure_recovery_name(GcStopFailureRecovery action) {
    switch (action) {
    case GC_STOP_FAILURE_NOTHING_TO_DO: return "nothing-to-do";
    case GC_STOP_FAILURE_RESTART_PREVIOUS: return "restart-previous";
    case GC_STOP_FAILURE_LEAVE_ACTIVE: return "leave-active";
    case GC_STOP_FAILURE_STATE_UNKNOWN: return "state-unknown";
    }
    return "invalid";
}
