// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// The folder and binary a LocalSystem service is registered from: judged,
// pinned and hardened through handles, then released again on removal.
//
// Two phases, split so the caller can order them around the SCM:
//
//   service_install_prepare_target  -- touches nothing.  Opens the directory
//     and the service binary WITHOUT FILE_SHARE_DELETE (neither, nor any
//     ancestor, can be renamed while the handles live), then asks every
//     question through those handles: is this folder ours to harden
//     (gc_service_install_location_verdict_for_handle), how well is it
//     protected, is the binary a plain file.  A refusal here happens before a
//     running service is stopped and before a single DACL is written.
//
//   service_install_harden_target   -- writes the protected DACLs through the
//     SAME handles and reads them back, owner included.
//
// Why handles (review 2026-09-22): the old code checked the folder by name
// and then wrote its DACL by name with SetNamedSecurityInfoW, which follows
// reparse points.  In a folder a standard account can rename -- exactly the
// "not protected, install anyway" case the consent flow allows -- that account
// could swap the folder for a junction between the check and the write, and
// the ELEVATED helper would then rewrite the DACL of whatever the junction
// named.

struct ServiceInstallTarget {
    WCHAR installDir[MAX_PATH] = {};
    WCHAR binaryPath[MAX_PATH] = {};
    HANDLE directory = INVALID_HANDLE_VALUE;
    HANDLE binary = INVALID_HANDLE_VALUE;
    bool hardenFiles = false;
    GcPathProtectionReport protection = {};

    ServiceInstallTarget() = default;
    ServiceInstallTarget(const ServiceInstallTarget&) = delete;
    ServiceInstallTarget& operator=(const ServiceInstallTarget&) = delete;
    ~ServiceInstallTarget() {
        if (binary != INVALID_HANDLE_VALUE) CloseHandle(binary);
        if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
    }
};

static void service_install_log_location_refusal(const WCHAR* installDir, int verdict,
                                                 const GcServiceLocationDetail& detail) {
    char dirToken[32] = {};
    gc_log_wide_identifier_token(installDir, dirToken, sizeof(dirToken));
    debug_log("service install: REFUSED location token %s verdict=%s exists=%d "
              "alreadyHardened=%d entriesScanned=%u foreignIsDirectory=%d foreignIsReparse=%d; "
              "hardening it would take write access to a folder that is not ours\n",
              dirToken, gc_service_location_verdict_name(verdict), detail.exists ? 1 : 0,
              detail.alreadyHardened ? 1 : 0, detail.entriesScanned,
              detail.foreignIsDirectory ? 1 : 0, detail.foreignIsReparse ? 1 : 0);
}

static bool service_install_prepare_target(ServiceInstallTarget* target, char* err,
                                           size_t errSize, int* reasonOut) {
    if (!target) return false;
    if (!get_secure_service_install_dir_w(target->installDir, ARRAY_COUNT(target->installDir),
                                          err, errSize)) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_LOCATION_REFUSED;
        return false;
    }
    if (FAILED(StringCchPrintfW(target->binaryPath, ARRAY_COUNT(target->binaryPath), L"%ls\\%ls",
                                target->installDir, APP_SERVICE_EXE_NAME_W))) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_LOCATION_REFUSED;
        set_message(err, errSize, "Service binary path is too long");
        return false;
    }

    // How well can this location be protected?  (service_path_chain_policy.h)
    // Informational -- the administrator picked it and consented where an
    // interactive flow exists -- except that a filesystem with no ACLs at all
    // cannot be hardened, so the DACL-writing access is not even requested.
    classify_path_protection(target->installDir, &target->protection, true);
    const GcPathProtection& verdict = target->protection.verdict;
    debug_log("service install: path protection protected=%d standardWritable=%d profile=%d "
              "remote=%d noFilesystemPermissions=%d reason=%d\n",
              verdict.chain_protected ? 1 : 0, verdict.standard_writable ? 1 : 0,
              verdict.user_profile ? 1 : 0, verdict.remote ? 1 : 0,
              verdict.no_filesystem_permissions ? 1 : 0, (int)verdict.reason);
    target->hardenFiles = !verdict.no_filesystem_permissions;
    if (!target->hardenFiles) {
        debug_log("service install WARNING: volume has no file permissions (for example "
                  "FAT/exFAT); skipping DACL hardening - the LocalSystem service binary "
                  "cannot be protected here\n");
    }
    const DWORD writeSecurity = target->hardenFiles ? (WRITE_DAC | WRITE_OWNER) : 0;

    // Pin the directory: no FILE_SHARE_DELETE, and FILE_FLAG_OPEN_REPARSE_POINT
    // so a leaf junction is inspected (and refused), never followed.
    target->directory = CreateFileW(target->installDir,
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL | writeSecurity,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (target->directory == INVALID_HANDLE_VALUE) {
        DWORD openErr = GetLastError();
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_DIRECTORY_HARDENING_FAILED;
        set_message(err, errSize, "Failed opening the service directory (error %lu)",
                    (unsigned long)openErr);
        debug_log("service install: opening the service directory failed error=%lu\n",
                  (unsigned long)openErr);
        return false;
    }

    // MAY we harden this folder at all?  Asked of the pinned object.
    GcServiceLocationDetail detail = {};
    int locationVerdict = gc_service_install_location_verdict_detailed(
        target->installDir, target->directory, &detail);
    if (locationVerdict != GC_SVC_LOCATION_OK) {
        service_install_log_location_refusal(target->installDir, locationVerdict, detail);
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_LOCATION_REFUSED;
        set_message(err, errSize, "%s", gc_service_admin_reason_text(GC_SVC_ADMIN_LOCATION_REFUSED));
        return false;
    }
    debug_log("service install: location ok alreadyHardened=%d entriesScanned=%u\n",
              detail.alreadyHardened ? 1 : 0, detail.entriesScanned);

    // Pin the binary the SCM will run: no FILE_SHARE_WRITE, no
    // FILE_SHARE_DELETE, so it can be neither rewritten nor renamed away until
    // the registration is done.  A running image is compatible with this open.
    target->binary = CreateFileW(target->binaryPath,
        READ_CONTROL | FILE_READ_ATTRIBUTES | writeSecurity, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (target->binary == INVALID_HANDLE_VALUE) {
        DWORD openErr = GetLastError();
        // Missing or quarantined is the common case and has its own remedy; a
        // sharing violation means something holds the binary open for write.
        int reason = (openErr == ERROR_FILE_NOT_FOUND || openErr == ERROR_PATH_NOT_FOUND)
            ? GC_SVC_ADMIN_BINARY_MISSING
            : gc_service_admin_classify_win32(GC_SVC_STAGE_STAGE_BINARY, openErr);
        if (reasonOut) *reasonOut = reason;
        set_message(err, errSize, "Failed opening the service binary (error %lu)",
                    (unsigned long)openErr);
        debug_log("service install: opening the service binary failed error=%lu reason=%d\n",
                  (unsigned long)openErr, reason);
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag = {};
    BY_HANDLE_FILE_INFORMATION info = {};
    bool plainFile =
        GetFileInformationByHandleEx(target->binary, FileAttributeTagInfo, &tag, sizeof(tag)) &&
        (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
        GetFileInformationByHandle(target->binary, &info);
    // One link only: hardening a second name of some other file would rewrite
    // THAT file's DACL too, and would register it as the SYSTEM service.
    if (!plainFile || info.nNumberOfLinks != 1) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_BINARY_MISSING;
        set_message(err, errSize, "%s", gc_service_admin_reason_text(GC_SVC_ADMIN_BINARY_MISSING));
        debug_log("service install: service binary is not a plain single-link file "
                  "(attributes=0x%lx links=%lu)\n",
                  (unsigned long)tag.FileAttributes, (unsigned long)info.nNumberOfLinks);
        return false;
    }
    return true;
}

static bool service_install_harden_target(ServiceInstallTarget* target, char* err,
                                          size_t errSize, int* reasonOut) {
    if (!target || target->directory == INVALID_HANDLE_VALUE ||
        target->binary == INVALID_HANDLE_VALUE) return false;
    if (target->hardenFiles) {
        // Elevated here by construction (service_install_or_remove checked),
        // so the Administrators owner is required, not attempted.
        char aclErr[160] = {};
        if (!apply_protected_service_dacl_to_handle(target->directory, GC_SERVICE_ACL_DIRECTORY,
                                                    true, aclErr, sizeof(aclErr))) {
            if (reasonOut) *reasonOut = GC_SVC_ADMIN_DIRECTORY_HARDENING_FAILED;
            set_message(err, errSize, "Failed securing service directory: %s",
                        aclErr[0] ? aclErr : "unknown");
            debug_log("service install: directory hardening failed: %s\n", err);
            return false;
        }
        if (!apply_protected_service_dacl_to_handle(target->binary, GC_SERVICE_ACL_BINARY,
                                                    true, aclErr, sizeof(aclErr))) {
            if (reasonOut) *reasonOut = GC_SVC_ADMIN_DIRECTORY_HARDENING_FAILED;
            set_message(err, errSize, "Failed securing service binary: %s",
                        aclErr[0] ? aclErr : "unknown");
            debug_log("service install: binary hardening failed: %s\n", err);
            return false;
        }
        debug_log("service install: directory and binary carry the protected DACL with an "
                  "Administrators owner (verified through the pinned handles)\n");
    }
    // Whatever the classification vouched for at the start must still hold
    // now that the hardened DACLs are in place.
    GcPathProtectionReport protectionAfter = {};
    classify_path_protection(target->installDir, &protectionAfter, false);
    if (target->protection.verdict.chain_protected && !protectionAfter.verdict.chain_protected) {
        set_message(err, errSize,
            "The service directory lost its protection during hardening (reason %d); "
            "refusing to register the service",
            (int)protectionAfter.verdict.reason);
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_DIRECTORY_HARDENING_FAILED;
        return false;
    }
    if (install_dir_is_under_user_profile_w(target->installDir)) {
        char targetToken[32] = {};
        char dirToken[32] = {};
        gc_log_wide_identifier_token(target->binaryPath, targetToken, sizeof(targetToken));
        gc_log_wide_identifier_token(target->installDir, dirToken, sizeof(dirToken));
        debug_log("service install: prepared LocalSystem binary at token %s (directory token %s)\n",
                  targetToken, dirToken);
        debug_log("service install WARNING: install dir %s is under a user profile. Other users, "
            "including restricted/standard accounts, may be unable to read or execute the Green Curve "
            "GUI binary. Install under %%ProgramFiles%% to make the application available to all users.\n",
            dirToken);
    } else {
        debug_log("service install: prepared LocalSystem binary at %ls (directory %ls)\n",
                  target->binaryPath, target->installDir);
    }
    return true;
}

// Which directories uninstall must NOT revert to inherited permissions, even
// when they carry exactly our DACL: a drive or share root has no parent to
// inherit from, so "inherited" would mean an empty DACL and nobody could open
// the volume.  Install refuses the same shapes (the shared predicate below).
static bool directory_path_is_root_or_share_root_w(const WCHAR* dir) {
    if (!dir || !dir[0]) return true;
    return !gc_service_location_shape_is_acceptable(dir);
}

// Undo the hardening of the folder a service binary was registered from.
// Only what is PROVABLY ours is touched (release_service_hardening compares
// the DACL against exactly the one install writes), so a folder the SCM
// happened to point at -- Program Files itself, a system folder, a folder
// someone re-ACLed since -- is left alone however the registration got there.
static void release_service_binary_location(const WCHAR* servicePath, const char* why) {
    if (!servicePath || !servicePath[0]) return;
    WCHAR directory[MAX_PATH] = {};
    if (FAILED(StringCchCopyW(directory, ARRAY_COUNT(directory), servicePath))) return;
    WCHAR* slash = wcsrchr(directory, L'\\');
    if (!slash) slash = wcsrchr(directory, L'/');
    if (!slash) return;
    *slash = 0;
    char token[32] = {};
    gc_log_wide_identifier_token(directory, token, sizeof(token));
    char aclErr[160] = {};
    int binaryResult = release_service_hardening(servicePath, GC_SERVICE_ACL_BINARY,
                                                 aclErr, sizeof(aclErr));
    debug_log("service %s: binary hardening release in directory token %s -> %s%s%s\n",
              why, token, service_release_result_name(binaryResult),
              aclErr[0] ? ": " : "", aclErr);
    if (directory_path_is_root_or_share_root_w(directory)) {
        debug_log("service %s: skipped directory release for root-like directory token %s\n",
                  why, token);
        return;
    }
    aclErr[0] = 0;
    int directoryResult = release_service_hardening(directory, GC_SERVICE_ACL_DIRECTORY,
                                                    aclErr, sizeof(aclErr));
    debug_log("service %s: directory hardening release token %s -> %s%s%s\n",
              why, token, service_release_result_name(directoryResult),
              aclErr[0] ? ": " : "", aclErr);
}

static void cleanup_secure_service_binary_after_remove(const WCHAR* installedServicePath = nullptr) {
    // Service binary removal intentionally does NOT delete the file from disk:
    // the user manages the binary that sits next to greencurve.exe.  We DO
    // release the hardening install applied, so once the service is
    // unregistered the user can freely delete or replace the files again.
    WCHAR targetPath[MAX_PATH] = {};
    if (installedServicePath && installedServicePath[0]) {
        if (FAILED(StringCchCopyW(targetPath, ARRAY_COUNT(targetPath), installedServicePath))) return;
    } else if (!get_service_binary_path_from_scm(targetPath, ARRAY_COUNT(targetPath))) {
        WCHAR installDir[MAX_PATH] = {};
        char ignored[64] = {};
        if (!get_secure_service_install_dir_w(installDir, ARRAY_COUNT(installDir), ignored, sizeof(ignored))) return;
        if (FAILED(StringCchPrintfW(targetPath, ARRAY_COUNT(targetPath), L"%ls\\%ls", installDir, APP_SERVICE_EXE_NAME_W))) return;
    }
    release_service_binary_location(targetPath, "uninstall");
}

// After a successful re-point, the folder the service USED to run from is no
// longer a service folder, so its hardening is released -- unless that folder
// is the new one, or an ancestor of it: an ancestor that inherits again could
// hand a standard account the right to rename the new service folder away.
static void release_previous_service_location(const WCHAR* previousServicePath,
                                              const ServiceInstallTarget* target) {
    if (!previousServicePath || !previousServicePath[0] || !target) return;
    WCHAR previousDir[MAX_PATH] = {};
    if (FAILED(StringCchCopyW(previousDir, ARRAY_COUNT(previousDir), previousServicePath))) return;
    WCHAR* slash = wcsrchr(previousDir, L'\\');
    if (!slash) return;
    *slash = 0;
    WCHAR previousFinal[GC_PATH_CHAIN_MAX_PATH_CHARS] = {};
    WCHAR targetFinal[GC_PATH_CHAIN_MAX_PATH_CHARS] = {};
    HANDLE previous = CreateFileW(previousDir, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (previous == INVALID_HANDLE_VALUE) {
        debug_log("service install: previous service folder is gone (error %lu); nothing to release\n",
                  (unsigned long)GetLastError());
        return;
    }
    DWORD previousLength = GetFinalPathNameByHandleW(previous, previousFinal,
        ARRAY_COUNT(previousFinal), FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    CloseHandle(previous);
    DWORD targetLength = GetFinalPathNameByHandleW(target->directory, targetFinal,
        ARRAY_COUNT(targetFinal), FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    if (previousLength == 0 || previousLength >= ARRAY_COUNT(previousFinal) ||
        targetLength == 0 || targetLength >= ARRAY_COUNT(targetFinal)) {
        debug_log("service install: previous service folder could not be resolved; "
                  "leaving its permissions unchanged\n");
        return;
    }
    if (gc_service_location_is_within(targetFinal, previousFinal)) {
        debug_log("service install: previous service folder is the new one or contains it; "
                  "its permissions stay as they are\n");
        return;
    }
    release_service_binary_location(previousServicePath, "re-point");
}
