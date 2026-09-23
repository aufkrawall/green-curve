// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The installer transaction's ordering is host-neutral so each failure edge
// can be injected by the regression harness without touching a real service.
#pragma once

#include <stdint.h>

template <typename Stop, typename Replace, typename Record,
          typename Register, typename Rollback>
static bool gc_run_install_transaction(uint32_t fileCount, Stop stop,
                                        Replace replace, Record record,
                                        Register service, Rollback rollback) {
    if (!stop()) {
        rollback(false);
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
