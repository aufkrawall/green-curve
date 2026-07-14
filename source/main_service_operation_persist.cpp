// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#define SERVICE_OPERATION_RECORD_MAGIC 0x47434f50u /* GCOP */
#define SERVICE_OPERATION_RECORD_VERSION 1u

struct ServicePersistedOperationRecord {
    DWORD magic;
    DWORD version;
    DWORD size;
    DWORD state;
    gc_u64 operationId;
    DWORD responseStatus;
    char message[512];
    DWORD checksum;
};

static DWORD service_operation_record_checksum(
    const ServicePersistedOperationRecord* record) {
    const unsigned char* bytes = (const unsigned char*)record;
    DWORD hash = 2166136261u;
    for (size_t i = 0;
         record && i < offsetof(ServicePersistedOperationRecord, checksum); i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool service_operation_record_path(char* out, size_t outSize) {
    char directory[MAX_PATH] = {};
    return out && outSize &&
        resolve_service_machine_data_dir(directory, sizeof(directory)) &&
        SUCCEEDED(StringCchPrintfA(out, outSize,
            "%s\\service_operation.bin", directory));
}

static bool service_store_operation_record(gc_u64 operationId, DWORD state,
    DWORD responseStatus, const char* message) {
    if (!operationId || state < SERVICE_OPERATION_IN_PROGRESS ||
        state > SERVICE_OPERATION_OUTCOME_UNKNOWN) return false;
    ServicePersistedOperationRecord record = {};
    record.magic = SERVICE_OPERATION_RECORD_MAGIC;
    record.version = SERVICE_OPERATION_RECORD_VERSION;
    record.size = sizeof(record);
    record.state = state;
    record.operationId = operationId;
    record.responseStatus = responseStatus;
    StringCchCopyA(record.message, ARRAY_COUNT(record.message),
        message ? message : "");
    record.checksum = service_operation_record_checksum(&record);
    char path[MAX_PATH] = {};
    char directoryError[128] = {};
    if (!service_operation_record_path(path, sizeof(path)) ||
        !ensure_parent_directory_for_file(path, directoryError,
            sizeof(directoryError))) return false;
    gc_u64 suffix = 0;
    if (BCryptGenRandom(nullptr, (PUCHAR)&suffix, sizeof(suffix),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return false;
    char temporary[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(temporary, ARRAY_COUNT(temporary),
            "%s.tmp.%016llx", path, (unsigned long long)suffix))) return false;
    HANDLE file = gc_CreateFileUtf8(temporary, GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, &record, sizeof(record), &written, nullptr) &&
        written == sizeof(record) && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (ok) ok = gc_MoveFileExUtf8(temporary, path,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) gc_DeleteFileUtf8(temporary);
    return ok;
}

static bool service_load_operation_record(ServiceOperationTracker* tracker) {
    if (!tracker) return false;
    char path[MAX_PATH] = {};
    if (!service_operation_record_path(path, sizeof(path))) return false;
    HANDLE file = gc_CreateFileUtf8(path, GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    ServicePersistedOperationRecord record = {};
    DWORD read = 0;
    bool ok = ReadFile(file, &record, sizeof(record), &read, nullptr) &&
        read == sizeof(record);
    CloseHandle(file);
    ok = ok && record.magic == SERVICE_OPERATION_RECORD_MAGIC &&
        record.version == SERVICE_OPERATION_RECORD_VERSION &&
        record.size == sizeof(record) && record.operationId != 0 &&
        record.state >= SERVICE_OPERATION_IN_PROGRESS &&
        record.state <= SERVICE_OPERATION_OUTCOME_UNKNOWN &&
        record.checksum == service_operation_record_checksum(&record);
    if (!ok) return false;
    DWORD restoredState = record.state == SERVICE_OPERATION_IN_PROGRESS
        ? SERVICE_OPERATION_OUTCOME_UNKNOWN : record.state;
    return service_operation_restore(tracker, record.operationId,
        restoredState, record.responseStatus,
        restoredState == SERVICE_OPERATION_OUTCOME_UNKNOWN
            ? "operation outcome became uncertain across service restart"
            : record.message);
}
