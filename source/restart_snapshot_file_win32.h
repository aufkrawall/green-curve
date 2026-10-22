// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_RESTART_SNAPSHOT_FILE_WIN32_H
#define GREEN_CURVE_RESTART_SNAPSHOT_FILE_WIN32_H

#if defined(_WIN32)
#include <windows.h>

struct GcRestartSnapshotIdentity {
    ULONGLONG fingerprint;
    ULONGLONG lastWriteTime100ns;
    DWORD size;
    DWORD volumeSerial;
    DWORD fileIndexHigh;
    DWORD fileIndexLow;
};

// Read and identify one pinned snapshot. Callers deny in-place writes; delete
// sharing may permit a newer generation to replace its pathname meanwhile.
static inline bool gc_restart_snapshot_fingerprint(HANDLE file, GcRestartSnapshotIdentity* out) {
    if (!out) return false;
    *out = {};
    SetLastError(ERROR_INVALID_DATA);
    LARGE_INTEGER start = {};
    BY_HANDLE_FILE_INFORMATION info = {};
    bool ok = SetFilePointerEx(file, start, nullptr, FILE_BEGIN) != FALSE &&
        GetFileInformationByHandle(file, &info) != FALSE &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        info.nFileSizeHigh == 0 && info.nFileSizeLow > 0 && info.nFileSizeLow <= 1024u * 1024u;
    ULONGLONG fingerprint = 1469598103934665603ULL;
    BYTE buffer[4096] = {};
    ULONGLONG total = 0;
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        total += read;
        for (DWORD i = 0; i < read; ++i) {
            fingerprint ^= buffer[i];
            fingerprint *= 1099511628211ULL;
        }
    }
    ULARGE_INTEGER writeValue = {};
    writeValue.LowPart = info.ftLastWriteTime.dwLowDateTime;
    writeValue.HighPart = info.ftLastWriteTime.dwHighDateTime;
    if (!ok || total != info.nFileSizeLow || fingerprint == 0 || writeValue.QuadPart == 0) return false;
    out->fingerprint = fingerprint;
    out->lastWriteTime100ns = writeValue.QuadPart;
    out->size = (DWORD)total;
    out->volumeSerial = info.dwVolumeSerialNumber;
    out->fileIndexHigh = info.nFileIndexHigh;
    out->fileIndexLow = info.nFileIndexLow;
    return true;
}

static inline bool gc_restart_snapshot_identity_equal(const GcRestartSnapshotIdentity& a,
    const GcRestartSnapshotIdentity& b) {
    return a.fingerprint == b.fingerprint && a.lastWriteTime100ns == b.lastWriteTime100ns &&
        a.size == b.size && a.volumeSerial == b.volumeSerial &&
        a.fileIndexHigh == b.fileIndexHigh && a.fileIndexLow == b.fileIndexLow;
}

enum GcRestartSnapshotCleanup {
    GC_RESTART_SNAPSHOT_PRESERVED,
    GC_RESTART_SNAPSHOT_DELETED,
    GC_RESTART_SNAPSHOT_CLEANUP_FAILED
};

// A helper may retire only the snapshot its authorization fingerprints. Both
// the comparison and delete name this HANDLE, never a pathname that a newer
// service generation may already have rebound. Open with GENERIC_READ | DELETE
// and FILE_SHARE_READ | FILE_SHARE_DELETE (no write sharing).
static inline GcRestartSnapshotCleanup gc_restart_snapshot_cleanup(HANDLE file,
    const GcRestartSnapshotIdentity& expected, DWORD* errorOut) {
    if (errorOut) *errorOut = ERROR_SUCCESS;
    GcRestartSnapshotIdentity actual = {};
    if (!gc_restart_snapshot_fingerprint(file, &actual)) {
        if (errorOut) *errorOut = GetLastError();
        return GC_RESTART_SNAPSHOT_CLEANUP_FAILED;
    }
    if (!expected.fingerprint || !expected.lastWriteTime100ns || !expected.size ||
        !gc_restart_snapshot_identity_equal(actual, expected)) return GC_RESTART_SNAPSHOT_PRESERVED;
    FILE_DISPOSITION_INFO disposition = {};
    disposition.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) {
        if (errorOut) *errorOut = GetLastError();
        return GC_RESTART_SNAPSHOT_CLEANUP_FAILED;
    }
    return GC_RESTART_SNAPSHOT_DELETED;
}

#endif
#endif  // GREEN_CURVE_RESTART_SNAPSHOT_FILE_WIN32_H
