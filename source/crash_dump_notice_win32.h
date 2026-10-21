// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_CRASH_DUMP_NOTICE_WIN32_H
#define GREEN_CURVE_CRASH_DUMP_NOTICE_WIN32_H

// Win32 half of crash_dump_notice_policy.h: the directory scan and the notice
// write, free of service globals so the regression harness runs them against
// a scratch directory.  Both act with the calling thread's identity; the
// service scans as itself and writes under the user's token
// (main_service_crash_notice.cpp).

#include <windows.h>
#include <strsafe.h>
#include <wchar.h>

#include "crash_dump_notice_policy.h"

// Scans `dir` for regular files matching `pattern` (FindFirstFile syntax) and
// keeps the newest `capacity` in `entries` (unordered).  `*totalOut` counts all
// matches.  False when the directory could not be read at all; a directory
// that does not exist is an empty, successful scan.
static inline bool gc_crash_dump_notice_scan(const WCHAR* dir, const WCHAR* pattern,
                                             GcCrashDumpNoticeEntry* entries, size_t capacity,
                                             size_t* keptOut, size_t* totalOut) {
    if (keptOut) *keptOut = 0;
    if (totalOut) *totalOut = 0;
    if (!dir || !pattern || !entries || capacity == 0 || !keptOut || !totalOut) return false;
    WCHAR query[MAX_PATH] = {};
    if (FAILED(StringCchPrintfW(query, MAX_PATH, L"%ls\\%ls", dir, pattern))) return false;
    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW(query, &found);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    size_t kept = 0, total = 0;
    do {
        // Regular files only: never follow or report a reparse point.
        if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        ++total;
        GcCrashDumpNoticeEntry entry = {};
        if (!WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, entry.name,
                                 (int)sizeof(entry.name), nullptr, nullptr)) continue;
        entry.sizeBytes = ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        entry.sortKey = ((unsigned long long)found.ftLastWriteTime.dwHighDateTime << 32) |
                        found.ftLastWriteTime.dwLowDateTime;
        FILETIME local = {};
        SYSTEMTIME st = {};
        if (FileTimeToLocalFileTime(&found.ftLastWriteTime, &local) &&
            FileTimeToSystemTime(&local, &st)) {
            snprintf(entry.when, sizeof(entry.when), "%04u-%02u-%02u %02u:%02u:%02u",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        } else {
            snprintf(entry.when, sizeof(entry.when), "unknown time");
        }
        if (kept < capacity) {
            entries[kept++] = entry;
        } else {
            size_t oldest = 0;
            for (size_t i = 1; i < kept; ++i)
                if (entries[i].sortKey < entries[oldest].sortKey) oldest = i;
            if (entries[oldest].sortKey < entry.sortKey) entries[oldest] = entry;
        }
    } while (FindNextFileW(search, &found));
    DWORD endError = GetLastError();
    FindClose(search);
    *keptOut = kept;
    *totalOut = total;
    return endError == ERROR_NO_MORE_FILES;
}

// Writes `text` to `path`, or removes `path` when `text` is null or empty.
// Replaces atomically through a sibling temporary file.  `*errorOut` gets the
// failing call's error.
static inline bool gc_crash_dump_notice_store(const WCHAR* path, const char* text,
                                              DWORD* errorOut) {
    if (errorOut) *errorOut = ERROR_SUCCESS;
    if (!path || !path[0]) {
        if (errorOut) *errorOut = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (!text || !text[0]) {
        if (DeleteFileW(path)) return true;
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return true;
        if (errorOut) *errorOut = error;
        return false;
    }
    WCHAR temp[MAX_PATH] = {};
    if (FAILED(StringCchPrintfW(temp, MAX_PATH, L"%ls.tmp", path))) {
        if (errorOut) *errorOut = ERROR_BUFFER_OVERFLOW;
        return false;
    }
    HANDLE file = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (errorOut) *errorOut = GetLastError();
        return false;
    }
    const DWORD length = (DWORD)strlen(text);
    DWORD written = 0;
    bool ok = WriteFile(file, text, length, &written, nullptr) && written == length;
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (ok && !MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING)) {
        ok = false;
        error = GetLastError();
    }
    if (!ok) {
        DeleteFileW(temp);
        if (errorOut) *errorOut = error ? error : ERROR_WRITE_FAULT;
    }
    return ok;
}

#endif  // GREEN_CURVE_CRASH_DUMP_NOTICE_WIN32_H
