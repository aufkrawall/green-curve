// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_SHORTCUT_FILE_WRITER_H
#define GREEN_CURVE_SHORTCUT_FILE_WRITER_H
#if defined(_WIN32)
#include <windows.h>
#include <objidl.h>
#include <strsafe.h>
#include <wchar.h>
#include "file_relative_open.h"

struct GcShortcutHandle {
    HANDLE value;
    explicit GcShortcutHandle(HANDLE handle) : value(handle) {}
    GcShortcutHandle(const GcShortcutHandle&) = delete;
    GcShortcutHandle& operator=(const GcShortcutHandle&) = delete;
    ~GcShortcutHandle() {
        DWORD error = GetLastError();
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        SetLastError(error);
    }
};

// Keep known-folder redirects, pin their resolved directory, then validate the
// leaf before touching bytes. Opening an existing leaf never truncates it.
static inline bool gc_shortcut_write_to_directory(HANDLE directory, const WCHAR* name, IStream* stream) {
    if (!stream) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    STATSTG stat = {};
    LARGE_INTEGER zero = {};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || !stat.cbSize.QuadPart ||
        stat.cbSize.QuadPart > 1024 * 1024 || FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    GcShortcutHandle file(gc_open_relative_output(directory, name));
    BY_HANDLE_FILE_INFORMATION info = {};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &info)) return false;
    if (GetFileType(file.value) != FILE_TYPE_DISK || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    if (!SetFilePointerEx(file.value, zero, nullptr, FILE_BEGIN)) return false;
    unsigned long long remaining = stat.cbSize.QuadPart;
    while (remaining) {
        unsigned char buffer[4096] = {};
        ULONG count = 0;
        ULONG wanted = remaining < sizeof(buffer) ? (ULONG)remaining : (ULONG)sizeof(buffer);
        if (FAILED(stream->Read(buffer, wanted, &count)) || count != wanted) {
            SetLastError(ERROR_READ_FAULT);
            return false;
        }
        DWORD done = 0;
        while (done < count) {
            DWORD written = 0;
            if (!WriteFile(file.value, buffer + done, count - done, &written, nullptr)) return false;
            if (!written) { SetLastError(ERROR_WRITE_FAULT); return false; }
            done += written;
        }
        remaining -= count;
    }
    return SetEndOfFile(file.value) && FlushFileBuffers(file.value);
}

static inline bool gc_shortcut_write_stream(const WCHAR* path, IStream* stream) {
    if (!path) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    WCHAR parent[32768] = {};
    if (FAILED(StringCchCopyW(parent, 32768, path))) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return false;
    }
    WCHAR* slash = wcsrchr(parent, L'\\');
    if (!slash || !slash[1]) { SetLastError(ERROR_INVALID_NAME); return false; }
    *slash = 0;
    const WCHAR* name = path + (slash - parent) + 1;
    GcShortcutHandle directory(CreateFileW(parent, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    BY_HANDLE_FILE_INFORMATION info = {};
    if (directory.value == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(directory.value, &info)) return false;
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        SetLastError(ERROR_DIRECTORY);
        return false;
    }
    return gc_shortcut_write_to_directory(directory.value, name, stream);
}
#endif
#endif
