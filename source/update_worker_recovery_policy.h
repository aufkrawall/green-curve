// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure decisions for the updater's worker recovery paths.  Keeping these out
// of the Win32 orchestration shard makes the "failed network must not erase a
// verified staged package" invariant executable on every host.
#ifndef GREEN_CURVE_UPDATE_WORKER_RECOVERY_POLICY_H
#define GREEN_CURVE_UPDATE_WORKER_RECOVERY_POLICY_H

enum GcUpdateFailedCheckRecovery {
    GC_UPDATE_FAILED_CHECK_MARK_FAILED,
    GC_UPDATE_FAILED_CHECK_KEEP_READY,
};

static inline GcUpdateFailedCheckRecovery gc_update_failed_check_recovery(
    bool packageStaged,
    bool decisionAvailable,
    bool manifestValid,
    bool stagedPackageMatchesManifest) {
    return packageStaged && decisionAvailable && manifestValid &&
                   stagedPackageMatchesManifest
               ? GC_UPDATE_FAILED_CHECK_KEEP_READY
               : GC_UPDATE_FAILED_CHECK_MARK_FAILED;
}

enum GcUpdateStagedCheckAction {
    GC_UPDATE_STAGED_DISCARD,
    GC_UPDATE_STAGED_KEEP,
};

static inline GcUpdateStagedCheckAction gc_update_staged_check_action(
    bool manifestValid, bool stagedPackageMatchesManifest) {
    return manifestValid && stagedPackageMatchesManifest
               ? GC_UPDATE_STAGED_KEEP
               : GC_UPDATE_STAGED_DISCARD;
}

#endif
