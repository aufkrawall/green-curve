// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Retire a previous install only after the new one is fully registered.

#include "installer_common.h"
#include "installer_move_cleanup.h"

// After a committed install, drop known setup-owned leaves this payload did
// not write.  The in-place upgrade path replaces every file the payload
// contains and leaves every file it does not; the pre-rename uninstaller is
// exactly such a leave.  Best effort and never fatal: the new install is
// already registered, and a locked leave is litter, not a broken install.
void gc_remove_stale_payload_leaves(const WCHAR* directory, const GcPayload* payload) {
    if (!directory || !directory[0] || !payload || payload->fileCount == 0) return;
    char label[GC_INSTALLER_LOG_PATH_LABEL_CHARS] = {};
    gc_log_path_label(directory, label, sizeof(label));
    const char* shipped[GC_ARCHIVE_MAX_FILES] = {};
    size_t shippedCount = payload->fileCount;
    if (shippedCount > GC_ARRAY_COUNT(shipped)) shippedCount = GC_ARRAY_COUNT(shipped);
    for (size_t i = 0; i < shippedCount; i++) shipped[i] = payload->files[i].name;
    GcPreviousFileCleanup cleanup =
        gc_remove_stale_setup_files(directory, shipped, shippedCount);
    if (cleanup.deleted == 0 && cleanup.failed == 0) {
        gc_log_step("upgrade reconcile: no stale owned leaves at %s", label);
        return;
    }
    gc_log_step("upgrade reconcile: stale owned leaves deleted=%u failed=%u "
                "firstFailure=%ls firstError=%lu at %s",
                cleanup.deleted, cleanup.failed,
                cleanup.firstFailedName ? cleanup.firstFailedName : L"none",
                cleanup.firstFileError, label);
}

namespace {

// Release the hardening the previous service install applied, so the user
// can delete what is left.  release_service_hardening() only acts on a DACL
// that is exactly the one Green Curve writes, and restores inheritance with an
// EMPTY explicit ACL -- never a null one, which Windows stores as "no DACL",
// i.e. Everyone: Full Control.
void gc_release_previous_dacl(const WCHAR* previous, const char* label) {
    char aclErr[160] = {};
    WCHAR previousService[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (gc_join_path(previous, GC_SETUP_SERVICE_EXE_W, previousService,
                     GC_ARRAY_COUNT(previousService))) {
        int fileResult = release_service_hardening(previousService, GC_SERVICE_ACL_BINARY,
                                                   aclErr, sizeof(aclErr));
        gc_log_step("previous directory: service binary release in %s -> %s%s%s", label,
                    service_release_result_name(fileResult), aclErr[0] ? ": " : "", aclErr);
    }
    aclErr[0] = 0;
    int result = release_service_hardening(previous, GC_SERVICE_ACL_DIRECTORY,
                                           aclErr, sizeof(aclErr));
    gc_log_step("previous directory: folder release %s -> %s%s%s", label,
                service_release_result_name(result), aclErr[0] ? ": " : "", aclErr);
}

} // namespace

void gc_retire_previous_directory(GcInstallContext* context) {
    if (!context || !context->plan.directoryChanged || !context->plan.previousDirectory[0]) return;
    context->previousDirectoryRemoved = false;
    WCHAR previous[GC_INSTALLER_MAX_PATH_CHARS] = {};
    WCHAR target[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!gc_utf8_to_wide(context->plan.previousDirectory, previous, (int)GC_ARRAY_COUNT(previous)) ||
        !gc_utf8_to_wide(context->plan.targetDirectory, target, (int)GC_ARRAY_COUNT(target))) {
        gc_log_step("previous directory: cleanup skipped because a path could not be decoded");
        return;
    }
    char label[GC_INSTALLER_LOG_PATH_LABEL_CHARS] = {};
    gc_log_path_label(previous, label, sizeof(label));
    GcCleanupDirectoryIdentity oldIdentity = {};
    GcCleanupDirectoryIdentity newIdentity = {};
    if (!gc_read_cleanup_directory(previous, &oldIdentity)) {
        gc_log_step("previous directory: absent, unreadable, or a reparse point: %s", label);
        return;
    }
    if (gc_install_input_paths_overlap_or_unresolved(previous, target) ||
        !gc_read_cleanup_directory(target, &newIdentity) ||
        gc_cleanup_directories_overlap(oldIdentity, newIdentity)) {
        // Releasing the old ACL when it is an ancestor of the new service
        // would make that service's binary path writable again.
        gc_log_step("previous directory: cleanup and ACL release skipped because "
                    "the new directory could not be verified as disjoint: %s", label);
        return;
    }

    int location = gc_service_install_location_verdict(previous);
    if (context->plan.cleanupPreviousDirectory && location == GC_SVC_LOCATION_OK) {
        GcPreviousFileCleanup cleanup = gc_remove_previous_setup_files(previous);
        context->previousDirectoryRemoved = cleanup.removed;
        gc_log_step("previous directory: owned files deleted=%u failed=%u "
                    "firstFailure=%ls firstError=%lu folderRemoved=%d folderError=%lu at %s",
                    cleanup.deleted, cleanup.failed,
                    cleanup.firstFailedName ? cleanup.firstFailedName : L"none",
                    cleanup.firstFileError, cleanup.removed ? 1 : 0,
                    cleanup.directoryError, label);
        if (cleanup.removed) return;
    } else {
        gc_log_step("previous directory: retaining files (setupManaged=%d location=%s) at %s",
                    context->plan.cleanupPreviousDirectory ? 1 : 0,
                    gc_service_location_verdict_name(location), label);
    }
    gc_release_previous_dacl(previous, label);
}
