// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The file half of setup's rollback, shared with an unelevated Windows fixture.
#pragma once

#if defined(_WIN32)
#include <windows.h>
#include <strsafe.h>
#include "installer_cli_policy.h"

static inline HRESULT gc_replace_staged_install_file(const WCHAR* staged,
                                                       const WCHAR* destination) {
    WCHAR temporary[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (FAILED(StringCchPrintfW(temporary, GC_INSTALLER_MAX_PATH_CHARS,
                                L"%ls.gcnew", destination)))
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    HANDLE source = CreateFileW(staged, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (source == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    BY_HANDLE_FILE_INFORMATION info = {};
    if (!GetFileInformationByHandle(source, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(source);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    // Exclusive creation never deletes or follows a pre-existing object. The
    // new file inherits the target directory's ACL, not scratch's admin ACL.
    HANDLE output = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        CloseHandle(source);
        return HRESULT_FROM_WIN32(error);
    }
    DWORD copyError = ERROR_SUCCESS;
    BYTE buffer[65536];
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(source, buffer, sizeof(buffer), &count, nullptr)) {
            copyError = GetLastError(); break;
        }
        if (!count) break;
        DWORD offset = 0;
        while (offset < count) {
            DWORD written = 0;
            if (!WriteFile(output, buffer + offset, count - offset, &written, nullptr) || !written) {
                copyError = GetLastError();
                if (!copyError) copyError = ERROR_WRITE_FAULT;
                break;
            }
            offset += written;
        }
        if (copyError) break;
    }
    if (!copyError && !FlushFileBuffers(output)) copyError = GetLastError();
    CloseHandle(output);
    CloseHandle(source);
    if (copyError) { DeleteFileW(temporary); return HRESULT_FROM_WIN32(copyError); }
    if (!MoveFileExW(temporary, destination,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();
        DeleteFileW(temporary);
        return HRESULT_FROM_WIN32(error);
    }
    return S_OK;
}

static inline HRESULT gc_restore_previous_install_file(const WCHAR* backup,
                                                         const WCHAR* destination) {
    WCHAR temporary[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (FAILED(StringCchPrintfW(temporary, GC_INSTALLER_MAX_PATH_CHARS,
                                L"%ls.gcrestore", destination)))
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    // Backup CopyFileW preserved the original security descriptor; copy it
    // back before the atomic replacement.
    if (!CopyFileW(backup, temporary, TRUE)) {
        DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error);
    }
    if (!MoveFileExW(temporary, destination,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();
        DeleteFileW(temporary);
        return HRESULT_FROM_WIN32(error);
    }
    return S_OK;
}

// How many trailing components of `path` do not exist yet, i.e. how many
// folders creating it will add.  0 when it already exists; -1 when no
// existing ancestor can be found (nothing may then be removed later).
static inline int gc_count_missing_directory_components(const WCHAR* path) {
    WCHAR work[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!path || FAILED(StringCchCopyW(work, GC_INSTALLER_MAX_PATH_CHARS, path))) return -1;
    size_t length = wcslen(work);
    while (length > 3 && (work[length - 1] == L'\\' || work[length - 1] == L'/'))
        work[--length] = 0;
    int missing = 0;
    for (;;) {
        DWORD attributes = GetFileAttributesW(work);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? missing : -1;
        }
        WCHAR* slash = wcsrchr(work, L'\\');
        WCHAR* forward = wcsrchr(work, L'/');
        if (forward && (!slash || forward > slash)) slash = forward;
        if (!slash || slash == work) return -1;
        *slash = 0;
        ++missing;
    }
}

// Undo a folder chain this setup run created for a target it then abandoned.
// Removes at most `created` components, leaf first, and only while each one is
// empty (RemoveDirectoryW refuses anything else, and removes a planted junction
// itself rather than what it names).  Returns how many were removed.
static inline int gc_remove_created_directory_chain(const WCHAR* path, int created) {
    WCHAR work[GC_INSTALLER_MAX_PATH_CHARS] = {};
    if (!path || created <= 0 ||
        FAILED(StringCchCopyW(work, GC_INSTALLER_MAX_PATH_CHARS, path))) return 0;
    size_t length = wcslen(work);
    while (length > 3 && (work[length - 1] == L'\\' || work[length - 1] == L'/'))
        work[--length] = 0;
    int removed = 0;
    while (removed < created) {
        if (!RemoveDirectoryW(work)) break;
        ++removed;
        WCHAR* slash = wcsrchr(work, L'\\');
        WCHAR* forward = wcsrchr(work, L'/');
        if (forward && (!slash || forward > slash)) slash = forward;
        if (!slash || slash == work) break;
        *slash = 0;
    }
    return removed;
}
#endif
