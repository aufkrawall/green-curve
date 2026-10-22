// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Native snapshot retirement (7190-7198). Every interleaving is driven by
// explicit file operations; no hardware, service, sleeps or timing assumptions.
#if defined(_WIN32)
#include <windows.h>
#include <strsafe.h>
#include <stdio.h>
#include <string.h>
#include "restart_snapshot_file_win32.h"

namespace {
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit File(HANDLE handle) : value(handle) {}
    ~File() { close(); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    void close() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = INVALID_HANDLE_VALUE;
    }
};

HANDLE open_snapshot(const wchar_t* path, DWORD access = GENERIC_READ | DELETE) {
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

bool write_snapshot(const wchar_t* path, const char* bytes, const FILETIME* time = nullptr) {
    File file(CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD written = 0;
    const DWORD size = (DWORD)strlen(bytes);
    return file.value != INVALID_HANDLE_VALUE &&
        WriteFile(file.value, bytes, size, &written, nullptr) && written == size &&
        (!time || SetFileTime(file.value, nullptr, nullptr, time));
}

bool file_contains(const wchar_t* path, const char* expected) {
    File file(open_snapshot(path, GENERIC_READ));
    char bytes[64] = {};
    DWORD read = 0;
    return file.value != INVALID_HANDLE_VALUE &&
        ReadFile(file.value, bytes, sizeof(bytes) - 1, &read, nullptr) &&
        read == strlen(expected) && strcmp(bytes, expected) == 0;
}

struct Fixture {
    wchar_t directory[MAX_PATH] = {}, snapshot[MAX_PATH] = {}, retired[MAX_PATH] = {}, auth[MAX_PATH] = {};
    bool create() {
        wchar_t temp[MAX_PATH] = {};
        DWORD n = GetTempPathW(MAX_PATH, temp);
        return n && n < MAX_PATH &&
            SUCCEEDED(StringCchPrintfW(directory, MAX_PATH, L"%lsgc_snapshot_%lu_%llu", temp,
                (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64())) &&
            CreateDirectoryW(directory, nullptr) &&
            SUCCEEDED(StringCchPrintfW(snapshot, MAX_PATH, L"%ls\\snapshot.bin", directory)) &&
            SUCCEEDED(StringCchPrintfW(retired, MAX_PATH, L"%ls\\retired.bin", directory)) &&
            SUCCEEDED(StringCchPrintfW(auth, MAX_PATH, L"%ls\\authorization.bin", directory));
    }
    void clear() {
        DeleteFileW(snapshot);
        DeleteFileW(retired);
        DeleteFileW(auth);
    }
    ~Fixture() { clear(); if (directory[0]) RemoveDirectoryW(directory); }
};

int cleanup_tests(Fixture& fixture) {
    // 7190: the authorized object is removed, even after a previous read.
    if (!write_snapshot(fixture.snapshot, "original snapshot")) return 7190;
    GcRestartSnapshotIdentity expected = {};
    DWORD error = 0;
    {
        File file(open_snapshot(fixture.snapshot));
        if (!gc_restart_snapshot_fingerprint(file.value, &expected) ||
            gc_restart_snapshot_cleanup(file.value, expected, &error) != GC_RESTART_SNAPSHOT_DELETED || error)
            return 7190;
    }
    if (GetFileAttributesW(fixture.snapshot) != INVALID_FILE_ATTRIBUTES) return 7190;

    // 7191: every identity component matters, including the file ID. A new
    // generation can publish identical bytes with the same recorded timestamp.
    if (!write_snapshot(fixture.snapshot, "original snapshot")) return 7191;
    {
        File file(open_snapshot(fixture.snapshot));
        if (!gc_restart_snapshot_fingerprint(file.value, &expected)) return 7191;
        GcRestartSnapshotIdentity different[6] = { expected, expected, expected, expected, expected, expected };
        ++different[0].fingerprint;
        ++different[1].lastWriteTime100ns;
        ++different[2].size;
        ++different[3].volumeSerial;
        ++different[4].fileIndexHigh;
        ++different[5].fileIndexLow;
        for (const GcRestartSnapshotIdentity& identity : different) {
            if (gc_restart_snapshot_cleanup(file.value, identity, &error) != GC_RESTART_SNAPSHOT_PRESERVED || error)
                return 7191;
        }
    }
    if (!file_contains(fixture.snapshot, "original snapshot")) return 7191;

    // 7192: ordinary startup cannot read/delete the helper-pinned auth, yet
    // continues. It retires the old snapshot and publishes a new object before
    // helper cleanup opens it. Preserve that object even with identical content
    // AND timestamp; a pathname/fingerprint/time-only check is insufficient.
    if (!write_snapshot(fixture.auth, "fixture authorization")) return 7192;
    {
        File auth(CreateFileW(fixture.auth, GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (auth.value == INVALID_HANDLE_VALUE) return 7192;
        File peek(CreateFileW(fixture.auth, GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (peek.value != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) return 7192;
        if (DeleteFileW(fixture.auth) || GetLastError() != ERROR_SHARING_VIOLATION) return 7192;
        if (!MoveFileW(fixture.snapshot, fixture.retired)) return 7192;
        ULARGE_INTEGER time = {};
        time.QuadPart = expected.lastWriteTime100ns;
        const FILETIME stamp = { time.LowPart, time.HighPart };
        if (!write_snapshot(fixture.snapshot, "original snapshot", &stamp)) return 7192;
        File newer(open_snapshot(fixture.snapshot));
        GcRestartSnapshotIdentity actual = {};
        if (!gc_restart_snapshot_fingerprint(newer.value, &actual) ||
            actual.fingerprint != expected.fingerprint || actual.lastWriteTime100ns != expected.lastWriteTime100ns ||
            actual.size != expected.size || gc_restart_snapshot_identity_equal(actual, expected) ||
            gc_restart_snapshot_cleanup(newer.value, expected, &error) != GC_RESTART_SNAPSHOT_PRESERVED || error)
            return 7192;
    }
    if (!file_contains(fixture.snapshot, "original snapshot")) return 7192;
    fixture.clear();

    // 7193: replacement after the helper opens its snapshot is also safe.
    // Delete sharing permits publication; handle-bound deletion retires only
    // the old object, even though the original pathname now names another one.
    if (!write_snapshot(fixture.snapshot, "old generation")) return 7193;
    {
        File old(open_snapshot(fixture.snapshot));
        if (!gc_restart_snapshot_fingerprint(old.value, &expected) ||
            !MoveFileW(fixture.snapshot, fixture.retired) ||
            !write_snapshot(fixture.snapshot, "new generation")) return 7193;
        if (gc_restart_snapshot_cleanup(old.value, expected, &error) != GC_RESTART_SNAPSHOT_DELETED || error)
            return 7193;
    }
    if (!file_contains(fixture.snapshot, "new generation") ||
        GetFileAttributesW(fixture.retired) != INVALID_FILE_ATTRIBUTES) return 7193;
    fixture.clear();

    // 7194: unreadable/empty/oversized inputs never become deletion authority.
    if (gc_restart_snapshot_cleanup(INVALID_HANDLE_VALUE, expected, &error) !=
        GC_RESTART_SNAPSHOT_CLEANUP_FAILED || !error) return 7194;
    if (!write_snapshot(fixture.snapshot, "")) return 7194;
    {
        File empty(open_snapshot(fixture.snapshot));
        if (gc_restart_snapshot_cleanup(empty.value, expected, &error) != GC_RESTART_SNAPSHOT_CLEANUP_FAILED)
            return 7194;
    }
    fixture.clear();
    {
        File large(CreateFileW(fixture.snapshot, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        LARGE_INTEGER size = {};
        size.QuadPart = 1024LL * 1024 + 1;
        if (large.value == INVALID_HANDLE_VALUE || !SetFilePointerEx(large.value, size, nullptr, FILE_BEGIN) ||
            !SetEndOfFile(large.value)) return 7194;
    }
    {
        File large(open_snapshot(fixture.snapshot));
        if (gc_restart_snapshot_cleanup(large.value, expected, &error) != GC_RESTART_SNAPSHOT_CLEANUP_FAILED)
            return 7194;
    }
    fixture.clear();

    // 7195: a deletion failure is observable and leaves the file intact.
    if (!write_snapshot(fixture.snapshot, "original snapshot")) return 7195;
    {
        File readOnly(open_snapshot(fixture.snapshot, GENERIC_READ));
        if (!gc_restart_snapshot_fingerprint(readOnly.value, &expected) ||
            gc_restart_snapshot_cleanup(readOnly.value, expected, &error) != GC_RESTART_SNAPSHOT_CLEANUP_FAILED || !error)
            return 7195;
    }
    return file_contains(fixture.snapshot, "original snapshot") ? 0 : 7195;
}
}  // namespace
#endif

int run_restart_snapshot_cleanup_tests() {
#if defined(_WIN32)
    Fixture fixture;
    if (!fixture.create()) return 7190;
    const int failure = cleanup_tests(fixture);
    if (!failure) printf("restart snapshot cleanup: native generation tests passed\n");
    return failure;
#else
    return 0;
#endif
}

#if defined(GC_RESTART_SNAPSHOT_FIXTURE_MAIN)
int main() { return run_restart_snapshot_cleanup_tests(); }
#endif
