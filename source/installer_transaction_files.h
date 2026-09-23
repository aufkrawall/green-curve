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
    if (!DeleteFileW(temporary) && GetLastError() != ERROR_FILE_NOT_FOUND)
        return HRESULT_FROM_WIN32(GetLastError());
    // CopyFileW copies scratch's admin-only DACL on Windows 8+. CopyFile2
    // preserves the destination directory's inherited security instead.
    HRESULT copied = CopyFile2(staged, temporary, nullptr);
    if (FAILED(copied)) {
        DeleteFileW(temporary);
        return copied;
    }
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
    if (!DeleteFileW(temporary) && GetLastError() != ERROR_FILE_NOT_FOUND)
        return HRESULT_FROM_WIN32(GetLastError());
    // Backup CopyFileW preserved the original security descriptor; copy it
    // back before the atomic replacement.
    if (!CopyFileW(backup, temporary, TRUE)) {
        DWORD error = GetLastError();
        DeleteFileW(temporary);
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
#endif
