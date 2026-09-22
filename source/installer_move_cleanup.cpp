// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Retire a previous install only after the new one is fully registered.

#include "installer_common.h"
#include "installer_move_cleanup.h"

namespace {

void gc_release_previous_dacl(const WCHAR* previous) {
    DWORD result = SetNamedSecurityInfoW((LPWSTR)previous, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr);
    if (result == ERROR_SUCCESS)
        gc_log_step("previous directory: %ls reverted to inherited permissions", previous);
    else
        gc_log_step("previous directory: could not revert permissions on %ls (error %lu)",
                    previous, result);
    WCHAR previousService[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (gc_join_path(previous, GC_SETUP_SERVICE_EXE_W, previousService,
                     GC_ARRAY_COUNT(previousService)) && gc_file_exists(previousService)) {
        DWORD fileResult = SetNamedSecurityInfoW(previousService, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, nullptr, nullptr);
        if (fileResult != ERROR_SUCCESS)
            gc_log_step("previous directory: could not revert service file permissions "
                        "on %ls (error %lu)", previousService, fileResult);
    }
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
    GcCleanupDirectoryIdentity oldIdentity = {};
    GcCleanupDirectoryIdentity newIdentity = {};
    if (!gc_read_cleanup_directory(previous, &oldIdentity)) {
        gc_log_step("previous directory: absent, unreadable, or a reparse point: %ls", previous);
        return;
    }
    if (gc_install_input_paths_overlap_or_unresolved(previous, target) ||
        !gc_read_cleanup_directory(target, &newIdentity) ||
        gc_cleanup_directories_overlap(oldIdentity, newIdentity)) {
        // Releasing the old ACL when it is an ancestor of the new service
        // would make that service's binary path writable again.
        gc_log_step("previous directory: cleanup and ACL release skipped because "
                    "the new directory could not be verified as disjoint: %ls", previous);
        return;
    }

    int location = gc_service_install_location_verdict(previous);
    if (context->plan.cleanupPreviousDirectory && location == GC_SVC_LOCATION_OK) {
        GcPreviousFileCleanup cleanup = gc_remove_previous_setup_files(previous);
        context->previousDirectoryRemoved = cleanup.removed;
        gc_log_step("previous directory: owned files deleted=%u failed=%u "
                    "firstFailure=%ls firstError=%lu folderRemoved=%d folderError=%lu at %ls",
                    cleanup.deleted, cleanup.failed,
                    cleanup.firstFailedName ? cleanup.firstFailedName : L"none",
                    cleanup.firstFileError, cleanup.removed ? 1 : 0,
                    cleanup.directoryError, previous);
        if (cleanup.removed) return;
    } else {
        gc_log_step("previous directory: retaining files (setupManaged=%d location=%s) at %ls",
                    context->plan.cleanupPreviousDirectory ? 1 : 0,
                    gc_service_location_verdict_name(location), previous);
    }
    gc_release_previous_dacl(previous);
}
