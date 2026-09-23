// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Narrow cleanup primitive for a setup-managed folder after an install move.
// The caller proves that the old and new directories are distinct, disjoint,
// ordinary directories and that the old InstallLocation came from setup.

#ifndef GREEN_CURVE_INSTALLER_MOVE_CLEANUP_H
#define GREEN_CURVE_INSTALLER_MOVE_CLEANUP_H

#ifdef _WIN32
#include <windows.h>
#include <strsafe.h>
#include "installer_plan_policy.h"

// Keep this list identical to the release payload manifest. The uninstaller
// uses it too; the uninstaller binaries are separate because they may be the
// running image (and the legacy name is still cleaned up on upgrade).
static const wchar_t* const GC_SETUP_PAYLOAD_FILE_NAMES[] = {
    L"greencurve.exe", L"greencurve-service.exe", L"README.md", L"LICENSE",
};

static inline bool gc_install_path_is_parent_or_same(const wchar_t* parent,
                                                     const wchar_t* child) {
    if (!parent || !child) return false;
    size_t length = wcslen(parent);
    while (length && (parent[length - 1] == L'\\' || parent[length - 1] == L'/')) length--;
    if (!length || wcslen(child) < length) return false;
    return CompareStringOrdinal(parent, (int)length, child, (int)length, TRUE) == CSTR_EQUAL &&
           (!child[length] || child[length] == L'\\' || child[length] == L'/');
}

// A mount point can put the new folder on a different volume while its access
// path still traverses the old directory. Compare the spelled, fully expanded
// paths as well as their final objects. An unresolvable spelling is unsafe.
static inline bool gc_install_input_paths_overlap_or_unresolved(const wchar_t* previous,
                                                                const wchar_t* target) {
    wchar_t oldFull[GC_INSTALLER_MAX_PATH_CHARS] = {};
    wchar_t newFull[GC_INSTALLER_MAX_PATH_CHARS] = {};
    DWORD oldLength = GetFullPathNameW(previous, GC_INSTALLER_MAX_PATH_CHARS, oldFull, nullptr);
    DWORD newLength = GetFullPathNameW(target, GC_INSTALLER_MAX_PATH_CHARS, newFull, nullptr);
    if (!oldLength || oldLength >= GC_INSTALLER_MAX_PATH_CHARS ||
        !newLength || newLength >= GC_INSTALLER_MAX_PATH_CHARS) return true;
    return gc_install_path_is_parent_or_same(oldFull, newFull) ||
           gc_install_path_is_parent_or_same(newFull, oldFull);
}

struct GcCleanupDirectoryIdentity {
    BY_HANDLE_FILE_INFORMATION file = {};
    wchar_t finalPath[GC_INSTALLER_MAX_PATH_CHARS] = {};
};

static inline bool gc_read_cleanup_directory(const wchar_t* path,
                                              GcCleanupDirectoryIdentity* out) {
    if (!path || !out) return false;
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    bool ok = GetFileInformationByHandle(handle, &out->file) != FALSE;
    // Volume GUID paths collapse drive-letter aliases as well as junctions.
    DWORD length = GetFinalPathNameByHandleW(handle, out->finalPath,
        GC_INSTALLER_MAX_PATH_CHARS, FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    CloseHandle(handle);
    return ok && length > 0 && length < GC_INSTALLER_MAX_PATH_CHARS;
}

static inline bool gc_cleanup_directories_overlap(const GcCleanupDirectoryIdentity& previous,
                                                  const GcCleanupDirectoryIdentity& target) {
    if (previous.file.dwVolumeSerialNumber == target.file.dwVolumeSerialNumber &&
        previous.file.nFileIndexHigh == target.file.nFileIndexHigh &&
        previous.file.nFileIndexLow == target.file.nFileIndexLow) return true;
    return gc_install_path_is_parent_or_same(previous.finalPath, target.finalPath) ||
           gc_install_path_is_parent_or_same(target.finalPath, previous.finalPath);
}

struct GcPreviousFileCleanup {
    bool removed;
    unsigned int deleted;
    unsigned int failed;
    const wchar_t* firstFailedName;
    DWORD firstFileError;
    DWORD directoryError;
};

// Delete only payload names and the setup-owned uninstaller. Never recurse;
// RemoveDirectoryW refuses a folder that contains any user-created file.
static inline GcPreviousFileCleanup gc_remove_previous_setup_files(const wchar_t* directory) {
    GcPreviousFileCleanup result = {};
    if (!directory || !directory[0]) return result;
    const wchar_t* const names[] = {
        GC_SETUP_PAYLOAD_FILE_NAMES[0], GC_SETUP_PAYLOAD_FILE_NAMES[1],
        GC_SETUP_PAYLOAD_FILE_NAMES[2], GC_SETUP_PAYLOAD_FILE_NAMES[3],
        L"greencurve-uninstall.exe",
        // Pre-rename uninstaller: still removed so an upgrade from a
        // 0.26.0-era install does not strand it.
        L"uninstall.exe",
    };
    for (const wchar_t* name : names) {
        wchar_t path[GC_INSTALLER_MAX_PATH_CHARS] = {};
        if (FAILED(StringCchPrintfW(path, GC_INSTALLER_MAX_PATH_CHARS,
                                    L"%ls\\%ls", directory, name))) {
            if (!result.failed) {
                result.firstFailedName = name;
                result.firstFileError = ERROR_INSUFFICIENT_BUFFER;
            }
            result.failed++;
            continue;
        }
        DWORD attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                if (!result.failed) {
                    result.firstFailedName = name;
                    result.firstFileError = error;
                }
                result.failed++;
            }
            continue;
        }
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (!result.failed) {
                result.firstFailedName = name;
                result.firstFileError = ERROR_INVALID_DATA;
            }
            result.failed++;
            continue;
        }
        if (DeleteFileW(path)) result.deleted++;
        else {
            if (!result.failed) {
                result.firstFailedName = name;
                result.firstFileError = GetLastError();
            }
            result.failed++;
        }
    }
    // Even if one deletion failed, this call is safe: it succeeds only if the
    // directory is truly empty. No reboot-time directory deletion is scheduled.
    result.removed = RemoveDirectoryW(directory) != FALSE;
    if (!result.removed) result.directoryError = GetLastError();
    return result;
}
#endif // _WIN32

#endif // GREEN_CURVE_INSTALLER_MOVE_CLEANUP_H
