// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Reading the payload appended to the setup executable.
//
// Release setup files append the archive verbatim. The footer rejects other
// methods before any allocation or file read.
//
// Everything about the container format, and every bounds check, lives in the
// pure installer_archive_policy.h so it is exercised by `build.py --test`.

#include "installer_common.h"

// A payload larger than this is a corrupt footer, not a release: the whole
// Windows manifest is a few megabytes.  The ceiling is checked before any
// allocation so a damaged file cannot make the installer reserve 4 GB.
#define GC_PAYLOAD_MAX_UNCOMPRESSED (256ull * 1024ull * 1024ull)

static bool gc_read_file_range(HANDLE file, uint64_t offset, void* buffer, uint64_t size) {
    LARGE_INTEGER position = {};
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        gc_log_fail("payload: seek to offset %llu failed (error %lu)",
                    (unsigned long long)offset, GetLastError());
        return false;
    }
    uint8_t* out = (uint8_t*)buffer;
    uint64_t remaining = size;
    while (remaining > 0) {
        DWORD chunk = (DWORD)(remaining > 0x10000000ull ? 0x10000000ull : remaining);
        DWORD read = 0;
        if (!ReadFile(file, out, chunk, &read, nullptr)) {
            gc_log_fail("payload: read failed at offset %llu (error %lu)",
                        (unsigned long long)offset, GetLastError());
            return false;
        }
        if (read == 0) {
            gc_log_fail("payload: unexpected end of file %llu byte(s) short",
                        (unsigned long long)remaining);
            return false;
        }
        out += read;
        remaining -= read;
    }
    return true;
}

bool gc_payload_load(const WCHAR* exePath, GcPayload* payload) {
    if (!payload) return false;
    GcPayload blank = {};
    *payload = blank;
    if (!exePath || !exePath[0]) {
        gc_log_fail("payload: no setup executable path");
        return false;
    }

    GcScopedHandle file(CreateFileW(exePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        gc_log_fail("payload: cannot open %ls (error %lu)", exePath, GetLastError());
        return false;
    }
    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(file.get(), &fileSize) || fileSize.QuadPart <= 0) {
        gc_log_fail("payload: cannot size %ls (error %lu)", exePath, GetLastError());
        return false;
    }
    uint64_t totalSize = (uint64_t)fileSize.QuadPart;
    if (totalSize < sizeof(GcPayloadFooter)) {
        gc_log_fail("payload: %ls is too small to carry a payload", exePath);
        return false;
    }

    GcPayloadFooter footer = {};
    if (!gc_read_file_range(file.get(), totalSize - sizeof(footer), &footer, sizeof(footer))) return false;
    GcPayloadStatus footerStatus =
        gc_payload_validate_footer(&footer, totalSize, GC_PAYLOAD_MAX_UNCOMPRESSED);
    if (footerStatus != GC_PAYLOAD_OK) {
        gc_log_fail("payload: %s", gc_payload_status_text(footerStatus));
        return false;
    }
    gc_log_step("payload: method=%lu compressed=%llu uncompressed=%llu",
                (unsigned long)footer.method,
                (unsigned long long)footer.compressedSize,
                (unsigned long long)footer.uncompressedSize);

    uint8_t* container = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)footer.uncompressedSize);
    if (!container) {
        gc_log_fail("payload: could not reserve %llu byte(s) for the payload",
                    (unsigned long long)footer.uncompressedSize);
        return false;
    }

    bool ok = gc_read_file_range(file.get(), footer.archiveOffset, container,
                                 footer.uncompressedSize);
    if (ok && gc_crc32(container, (size_t)footer.uncompressedSize, 0) != footer.archiveCrc32) {
        gc_log_fail("payload: the extracted payload failed its checksum; this setup file is damaged");
        ok = false;
    }
    if (!ok) {
        HeapFree(GetProcessHeap(), 0, container);
        return false;
    }

    const GcArchiveEntry* entries = nullptr;
    uint32_t count = 0;
    GcArchiveStatus archiveStatus =
        gc_archive_validate(container, footer.uncompressedSize, &entries, &count);
    if (archiveStatus != GC_ARCHIVE_OK) {
        gc_log_fail("payload: %s", gc_archive_status_text(archiveStatus));
        HeapFree(GetProcessHeap(), 0, container);
        return false;
    }

    payload->container = container;
    payload->containerSize = footer.uncompressedSize;
    payload->fileCount = count;
    for (uint32_t i = 0; i < count; i++) {
        StringCchCopyA(payload->files[i].name, GC_ARRAY_COUNT(payload->files[i].name), entries[i].name);
        payload->files[i].data = container + entries[i].dataOffset;
        payload->files[i].size = entries[i].dataSize;
        payload->files[i].flags = entries[i].flags;
        gc_log_step("payload: file %s (%llu bytes, flags 0x%lx)",
                    payload->files[i].name,
                    (unsigned long long)payload->files[i].size,
                    (unsigned long)payload->files[i].flags);
    }
    return true;
}

void gc_payload_release(GcPayload* payload) {
    if (!payload) return;
    if (payload->container) HeapFree(GetProcessHeap(), 0, payload->container);
    GcPayload blank = {};
    *payload = blank;
}

const GcPayloadFile* gc_payload_find(const GcPayload* payload, const char* name) {
    if (!payload || !name) return nullptr;
    for (uint32_t i = 0; i < payload->fileCount; i++) {
        if (lstrcmpiA(payload->files[i].name, name) == 0) return &payload->files[i];
    }
    return nullptr;
}
