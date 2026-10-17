// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The directory setup is launched from.
//
// Included BEFORE the worker in the amalgamation (main_service_server.cpp), so
// the install orchestration there can call straight into it.
//
// ## Why setup does not run from the staging directory
//
// The staging directory is readable by every account and long-lived: it holds
// a verified download for as long as the user takes to click Install.  The
// process the service creates runs as SYSTEM, and Windows resolves part of an
// executable's imports from the directory it starts in.  So the launch uses a
// directory that exists only for this one launch:
//
//   - a fresh, randomly named folder inside the proven staging directory,
//     created with a SYSTEM + Administrators-only descriptor in the
//     CreateDirectoryW call (never an existing name),
//   - holding exactly one file, a CREATE_NEW copy of the verified installer
//     made with the same descriptor and verified again by SHA-256 through the
//     handle that stays open across CreateProcessW,
//   - and re-proven through its pinned handle -- DACL, owner and that nothing
//     but that one file is inside -- immediately before CreateProcessW.
//
// Both handles deny delete sharing, so neither the folder nor the file can be
// renamed or replaced between the proof and the launch.

#define GC_UPDATE_LAUNCH_DIR_PREFIX "launch-"

struct GcUpdateLaunchDir {
    WCHAR dirW[MAX_PATH];
    WCHAR fileW[MAX_PATH];
    WCHAR leafW[GC_UPDATE_ASSET_NAME_MAX_CHARS];
    char fileUtf8[MAX_PATH];
    HANDLE dir;
    HANDLE file;
};

static void service_update_launch_dir_init(GcUpdateLaunchDir* launch) {
    GcUpdateLaunchDir blank = {};
    *launch = blank;
    launch->dir = INVALID_HANDLE_VALUE;
    launch->file = INVALID_HANDLE_VALUE;
}

static void service_update_launch_dir_close(GcUpdateLaunchDir* launch) {
    if (launch->file != INVALID_HANDLE_VALUE) CloseHandle(launch->file);
    if (launch->dir != INVALID_HANDLE_VALUE) CloseHandle(launch->dir);
    launch->file = INVALID_HANDLE_VALUE;
    launch->dir = INVALID_HANDLE_VALUE;
}

// Remove this launch's folder.  Path operations are safe here: the folder is
// SYSTEM/Administrators-only and was created by this process.
static void service_update_launch_dir_discard(GcUpdateLaunchDir* launch, const char* why) {
    service_update_launch_dir_close(launch);
    if (!launch->dirW[0]) return;
    const bool fileGone = !launch->fileW[0] || DeleteFileW(launch->fileW) ||
        GetLastError() == ERROR_FILE_NOT_FOUND;
    const bool dirGone = RemoveDirectoryW(launch->dirW) != FALSE;
    debug_log("update launch: discarded launch directory (%s) file=%d dir=%d\n",
              why ? why : "no reason", fileGone ? 1 : 0, dirGone ? 1 : 0);
    launch->dirW[0] = 0;
}

// A launch folder left behind by an earlier run (setup still had it open when
// this service was stopped).  Removed only when its handle proves it is one of
// ours -- the private DACL and a privileged owner -- and only its plain files
// are deleted, never recursively.
static bool service_update_remove_stale_launch_dir(const char* dirUtf8) {
    Win32Utf8Path dirW(dirUtf8);
    if (!dirW.valid_for(dirUtf8)) return false;
    HANDLE probe = CreateFileW(dirW.value, READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (probe == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO tag = {};
    const bool ours = GetFileInformationByHandleEx(probe, FileAttributeTagInfo, &tag, sizeof(tag)) &&
        (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        service_handle_dacl_is_ours(probe, GC_SERVICE_ACL_PRIVATE_DIRECTORY) &&
        service_handle_owner_is_privileged(probe);
    CloseHandle(probe);
    if (!ours) return false;
    char pattern[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(pattern, sizeof(pattern), "%s\\*", dirUtf8))) return false;
    WIN32_FIND_DATAA fd = {};
    HANDLE find = gc_FindFirstFileUtf8(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            char filePath[MAX_PATH] = {};
            if (SUCCEEDED(StringCchPrintfA(filePath, sizeof(filePath), "%s\\%s", dirUtf8,
                                           fd.cFileName)))
                gc_DeleteFileUtf8(filePath);
        } while (gc_FindNextFileUtf8(find, &fd));
        FindClose(find);
    }
    return gc_RemoveDirectoryUtf8(dirUtf8) != FALSE;
}

static bool service_update_launch_name_is_ours(const char* name) {
    return name && strncmp(name, GC_UPDATE_LAUNCH_DIR_PREFIX,
                           sizeof(GC_UPDATE_LAUNCH_DIR_PREFIX) - 1) == 0;
}

// Copy the verified bytes from the pinned staged file into the new file.  The
// length is bounded by the signed manifest; the copy is verified afterwards by
// hash anyway, so this bound only stops a runaway read.
static bool service_update_copy_installer(HANDLE from, HANDLE to, unsigned long long expected,
                                          char* err, size_t errSize) {
    LARGE_INTEGER zero = {};
    if (!SetFilePointerEx(from, zero, nullptr, FILE_BEGIN)) {
        set_message(err, errSize, "Cannot rewind the staged installer (error %lu)", GetLastError());
        return false;
    }
    BYTE buffer[16 * 1024];
    unsigned long long total = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(from, buffer, sizeof(buffer), &got, nullptr)) {
            set_message(err, errSize, "Cannot read the staged installer (error %lu)", GetLastError());
            return false;
        }
        if (got == 0) break;
        total += got;
        if (total > expected) {
            set_message(err, errSize, "The staged installer is longer than the signed manifest says");
            return false;
        }
        DWORD put = 0;
        if (!WriteFile(to, buffer, got, &put, nullptr) || put != got) {
            set_message(err, errSize, "Cannot write the launch copy (error %lu)", GetLastError());
            return false;
        }
    }
    if (total != expected || !FlushFileBuffers(to)) {
        set_message(err, errSize, "The launch copy is incomplete");
        return false;
    }
    return true;
}

// The launch file, re-read through the handle that will be held across
// CreateProcessW: exactly our private DACL, an Administrators owner, one link,
// not a reparse point.
static bool service_update_launch_file_is_proven(HANDLE file) {
    FILE_ATTRIBUTE_TAG_INFO tag = {};
    BY_HANDLE_FILE_INFORMATION info = {};
    return GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) &&
        !(tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        GetFileInformationByHandle(file, &info) && info.nNumberOfLinks == 1 &&
        service_handle_dacl_is_ours(file, GC_SERVICE_ACL_PRIVATE_FILE) &&
        service_handle_owner_is_administrators(file);
}

// Everything fallible about the launch happens here, before any window is
// closed: on failure nothing is left behind and the caller simply refuses.
static bool service_update_prepare_launch_dir(HANDLE verifiedStaged, const GcUpdateAsset* asset,
                                              GcUpdateLaunchDir* launch,
                                              char* err, size_t errSize) {
    service_update_launch_dir_init(launch);
    if (!asset || !asset->present || !asset->file[0] ||
        strpbrk(asset->file, "\\/:") != nullptr) {
        set_message(err, errSize, "The signed manifest names no usable installer file");
        return false;
    }
    char stagingDir[MAX_PATH] = {};
    if (!service_update_staging_dir(stagingDir, sizeof(stagingDir), err, errSize)) return false;
    Win32Utf8Path stagingW(stagingDir);
    if (!stagingW.valid_for(stagingDir) ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, asset->file, -1, launch->leafW,
                            ARRAY_COUNT(launch->leafW)) <= 0) {
        set_message(err, errSize, "The launch paths could not be encoded");
        return false;
    }
    unsigned char random[16] = {};
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        set_message(err, errSize, "Cannot generate a launch directory name");
        return false;
    }
    WCHAR suffix[33] = {};
    for (int i = 0; i < 16; i++)
        StringCchPrintfW(suffix + i * 2, 3, L"%02x", (unsigned)random[i]);
    if (FAILED(StringCchPrintfW(launch->dirW, ARRAY_COUNT(launch->dirW), L"%ls\\%hs%ls",
                                stagingW.value, GC_UPDATE_LAUNCH_DIR_PREFIX, suffix)) ||
        FAILED(StringCchPrintfW(launch->fileW, ARRAY_COUNT(launch->fileW), L"%ls\\%ls",
                                launch->dirW, launch->leafW)) ||
        !copy_wide_to_utf8(launch->fileW, launch->fileUtf8, (int)sizeof(launch->fileUtf8))) {
        launch->dirW[0] = 0;
        set_message(err, errSize, "The launch path is too long");
        return false;
    }
    // Fresh-only: a name that already exists is never used, whatever it is.
    char claimErr[256] = {};
    void* pinned = nullptr;
    const int claim = service_claim_protected_directory(launch->dirW,
        GC_SERVICE_ACL_PRIVATE_DIRECTORY, true, false, &pinned, claimErr, sizeof(claimErr));
    if (claim != GC_SERVICE_CLAIM_CREATED) {
        launch->dirW[0] = 0;  // not ours to remove
        set_message(err, errSize, "Cannot create a private launch directory: %s",
                    claimErr[0] ? claimErr : service_claim_result_name(claim));
        debug_log("update launch: private directory claim=%s (%s)\n",
                  service_claim_result_name(claim), claimErr[0] ? claimErr : "no detail");
        return false;
    }
    launch->dir = (HANDLE)pinned;
    char fileErr[256] = {};
    HANDLE copy = (HANDLE)service_create_protected_file(launch->fileW, GC_SERVICE_ACL_PRIVATE_FILE,
        true, GENERIC_WRITE, 0, fileErr, sizeof(fileErr));
    if (copy == INVALID_HANDLE_VALUE) {
        set_message(err, errSize, "Cannot create the launch copy: %s", fileErr);
        service_update_launch_dir_discard(launch, "launch copy could not be created");
        return false;
    }
    const bool copied = service_update_copy_installer(verifiedStaged, copy, asset->size,
                                                      err, errSize);
    CloseHandle(copy);
    if (!copied) {
        service_update_launch_dir_discard(launch, "launch copy failed");
        return false;
    }
    // Writes and deletes denied from here until the process exists.
    launch->file = CreateFileW(launch->fileW, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (launch->file == INVALID_HANDLE_VALUE) {
        set_message(err, errSize, "Cannot pin the launch copy (error %lu)", GetLastError());
        service_update_launch_dir_discard(launch, "launch copy could not be pinned");
        return false;
    }
    if (!service_update_launch_file_is_proven(launch->file) ||
        !gc_update_staged_file_matches(launch->file, asset, err, errSize)) {
        if (!err[0]) set_message(err, errSize, "The launch copy does not prove as ours");
        service_update_launch_dir_discard(launch, "launch copy failed verification");
        return false;
    }
    debug_log("update launch: prepared a private launch directory holding %s\n", asset->file);
    return true;
}

// Immediately before CreateProcessW, through the pinned handles only.
static bool service_update_reverify_launch_dir(const GcUpdateLaunchDir* launch,
                                               char* err, size_t errSize) {
    char dirErr[256] = {};
    if (!service_directory_holds_only(launch->dir, GC_SERVICE_ACL_PRIVATE_DIRECTORY, true,
                                      launch->leafW, dirErr, sizeof(dirErr))) {
        set_message(err, errSize, "The launch directory changed before launch: %s", dirErr);
        debug_log("update launch: REFUSED at launch; directory proof failed (%s)\n", dirErr);
        return false;
    }
    if (!service_update_launch_file_is_proven(launch->file)) {
        set_message(err, errSize, "The launch copy changed before launch");
        debug_log("update launch: REFUSED at launch; the launch copy no longer proves as ours\n");
        return false;
    }
    return true;
}
