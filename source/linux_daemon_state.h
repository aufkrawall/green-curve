// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_DAEMON_STATE_H
#define GREEN_CURVE_LINUX_DAEMON_STATE_H

#include <stddef.h>
#include <string.h>
#include "gpu_core.h"

enum LinuxDaemonRecordState : gc_u32 {
    LINUX_DAEMON_RECORD_PREPARED = 1,
    LINUX_DAEMON_RECORD_ACTIVE = 2,
    LINUX_DAEMON_RECORD_UNCERTAIN = 3,
};

enum {
    LINUX_DAEMON_RECORD_MAGIC = 0x4752434Cu, // "LCRG"
    LINUX_DAEMON_RECORD_VERSION = 2,
};

struct LinuxDaemonStateRecordV1 {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 state;
    GpuAdapterInfo targetGpu;
    DesiredSettings desired;
    gc_u32 checksum;
};

struct LinuxDaemonStateRecord {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 state;
    GpuAdapterInfo targetGpu;
    DesiredSettings desired;
    gc_u64 operationId;
    gc_u32 operationState;
    gc_u32 checksum;
};

enum {
    LINUX_DAEMON_OPERATION_MAGIC = 0x504f4347u, // "GCOP"
    LINUX_DAEMON_OPERATION_VERSION = 1,
};

struct LinuxDaemonOperationRecord {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 state;
    gc_u64 operationId;
    gc_u32 responseStatus;
    char message[512];
    gc_u32 checksum;
};

static inline gc_u32 linux_daemon_record_checksum(const LinuxDaemonStateRecord* record) {
    if (!record) return 0;
    const unsigned char* bytes = (const unsigned char*)record;
    const size_t length = offsetof(LinuxDaemonStateRecord, checksum);
    gc_u32 hash = 2166136261u;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static inline void linux_daemon_record_initialize(LinuxDaemonStateRecord* record,
                                                  LinuxDaemonRecordState state,
                                                  const GpuAdapterInfo* target,
                                                  const DesiredSettings* desired,
                                                  gc_u64 operationId = 0,
                                                  gc_u32 operationState = SERVICE_OPERATION_NONE) {
    if (!record) return;
    memset(record, 0, sizeof(*record));
    record->magic = LINUX_DAEMON_RECORD_MAGIC;
    record->version = LINUX_DAEMON_RECORD_VERSION;
    record->size = (gc_u32)sizeof(*record);
    record->state = (gc_u32)state;
    if (target) record->targetGpu = *target;
    if (desired) record->desired = *desired;
    record->operationId = operationId;
    record->operationState = operationState;
    record->checksum = linux_daemon_record_checksum(record);
}

static inline bool linux_daemon_record_valid(const LinuxDaemonStateRecord* record) {
    return record && record->magic == LINUX_DAEMON_RECORD_MAGIC &&
           record->version == LINUX_DAEMON_RECORD_VERSION &&
           record->size == sizeof(*record) &&
           record->state >= LINUX_DAEMON_RECORD_PREPARED &&
           record->state <= LINUX_DAEMON_RECORD_UNCERTAIN &&
           record->operationState <= SERVICE_OPERATION_OUTCOME_UNKNOWN &&
           record->checksum == linux_daemon_record_checksum(record);
}

static inline gc_u32 linux_daemon_operation_checksum(
    const LinuxDaemonOperationRecord* record) {
    if (!record) return 0;
    const unsigned char* bytes = (const unsigned char*)record;
    gc_u32 hash = 2166136261u;
    for (size_t i = 0; i < offsetof(LinuxDaemonOperationRecord, checksum); ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static inline void linux_daemon_operation_initialize(
    LinuxDaemonOperationRecord* record, gc_u64 operationId, gc_u32 state,
    gc_u32 responseStatus, const char* message) {
    if (!record) return;
    memset(record, 0, sizeof(*record));
    record->magic = LINUX_DAEMON_OPERATION_MAGIC;
    record->version = LINUX_DAEMON_OPERATION_VERSION;
    record->size = sizeof(*record);
    record->state = state;
    record->operationId = operationId;
    record->responseStatus = responseStatus;
    if (message) {
        size_t length = strlen(message);
        if (length >= sizeof(record->message)) length = sizeof(record->message) - 1;
        memcpy(record->message, message, length);
        record->message[length] = 0;
    }
    record->checksum = linux_daemon_operation_checksum(record);
}

static inline bool linux_daemon_operation_valid(
    const LinuxDaemonOperationRecord* record) {
    return record && record->magic == LINUX_DAEMON_OPERATION_MAGIC &&
        record->version == LINUX_DAEMON_OPERATION_VERSION &&
        record->size == sizeof(*record) && record->operationId != 0 &&
        record->state >= SERVICE_OPERATION_IN_PROGRESS &&
        record->state <= SERVICE_OPERATION_OUTCOME_UNKNOWN &&
        record->checksum == linux_daemon_operation_checksum(record);
}

// Startup-apply policy: what the daemon writes to the GPU when it starts.
// Persisted next to active.bin with the same root-owned checksummed atomic
// write, because it authorizes an unattended hardware write and must not be
// forgeable by an unprivileged user or survive as a half-written record.
enum {
    LINUX_DAEMON_STARTUP_MAGIC = 0x50555347u, // "GSUP"
    LINUX_DAEMON_STARTUP_VERSION = 1,
    LINUX_DAEMON_STARTUP_NAME_MAX = 64,
};

struct LinuxDaemonStartupRecord {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 mode;        // ServiceStartupPolicyMode
    gc_u32 profileSlot; // 1..CONFIG_NUM_SLOTS when mode == PROFILE, else 0
    gc_u32 reserved;
    char profileName[LINUX_DAEMON_STARTUP_NAME_MAX];
    GpuAdapterInfo targetGpu;
    DesiredSettings desired;
    gc_u32 checksum;
};

static inline gc_u32 linux_daemon_startup_checksum(
    const LinuxDaemonStartupRecord* record) {
    if (!record) return 0;
    const unsigned char* bytes = (const unsigned char*)record;
    gc_u32 hash = 2166136261u;
    for (size_t i = 0; i < offsetof(LinuxDaemonStartupRecord, checksum); ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static inline void linux_daemon_startup_initialize(
    LinuxDaemonStartupRecord* record, gc_u32 mode, gc_u32 profileSlot,
    const char* profileName, const GpuAdapterInfo* target,
    const DesiredSettings* desired) {
    if (!record) return;
    memset(record, 0, sizeof(*record));
    record->magic = LINUX_DAEMON_STARTUP_MAGIC;
    record->version = LINUX_DAEMON_STARTUP_VERSION;
    record->size = (gc_u32)sizeof(*record);
    record->mode = mode;
    // Only the PROFILE mode owns a slot, a name, a GPU and settings.  Zeroing
    // the rest keeps a "stop applying at boot" record from carrying a stale
    // overclock that a later schema change could resurrect.
    if (mode == SERVICE_STARTUP_POLICY_PROFILE) {
        record->profileSlot = profileSlot;
        if (profileName) {
            size_t length = strlen(profileName);
            if (length >= sizeof(record->profileName))
                length = sizeof(record->profileName) - 1;
            memcpy(record->profileName, profileName, length);
            record->profileName[length] = 0;
        }
        if (target) record->targetGpu = *target;
        if (desired) record->desired = *desired;
    }
    record->checksum = linux_daemon_startup_checksum(record);
}

static inline bool linux_daemon_startup_valid(
    const LinuxDaemonStartupRecord* record) {
    if (!record || record->magic != LINUX_DAEMON_STARTUP_MAGIC ||
        record->version != LINUX_DAEMON_STARTUP_VERSION ||
        record->size != sizeof(*record) ||
        record->mode >= SERVICE_STARTUP_POLICY_MODE_COUNT ||
        record->checksum != linux_daemon_startup_checksum(record)) return false;
    if (!service_wire_string_is_terminated(
            record->profileName, (unsigned int)sizeof(record->profileName)))
        return false;
    if (record->mode == SERVICE_STARTUP_POLICY_PROFILE) {
        // An unattended write needs an exact write target and a real slot;
        // anything less must not reach the hardware at boot.
        return record->profileSlot >= 1 &&
               record->profileSlot <= (gc_u32)CONFIG_NUM_SLOTS &&
               record->targetGpu.valid && record->targetGpu.pciInfoValid;
    }
    return record->profileSlot == 0 && !record->targetGpu.valid;
}

enum LinuxDaemonStateLoadResult {
    LINUX_DAEMON_STATE_MISSING = 0,
    LINUX_DAEMON_STATE_LOADED = 1,
    LINUX_DAEMON_STATE_LEGACY_REMOVED = 2,
    LINUX_DAEMON_STATE_INVALID_REMOVED = 3,
    LINUX_DAEMON_STATE_IO_ERROR = 4,
};

LinuxDaemonStateLoadResult linux_daemon_state_load(const char* path,
                                                   LinuxDaemonStateRecord* out,
                                                   char* err, size_t errSize);
bool linux_daemon_state_store(const char* path, const LinuxDaemonStateRecord* record,
                              char* err, size_t errSize);
bool linux_daemon_state_remove(const char* path, char* err, size_t errSize);
bool linux_daemon_operation_store(const char* path,
                                  const LinuxDaemonOperationRecord* record,
                                  char* err, size_t errSize);
bool linux_daemon_operation_load(const char* path,
                                 LinuxDaemonOperationRecord* record,
                                 char* err, size_t errSize);

// Startup policy.  A missing record is not an error: it means RESTORE_LAST,
// the behaviour every build before protocol v13 had.  `outCorrupt` reports a
// present-but-unusable record so the daemon can refuse to write at boot rather
// than silently falling back to replaying old intent.
bool linux_daemon_startup_load(const char* path,
                               LinuxDaemonStartupRecord* record,
                               bool* outCorrupt, char* err, size_t errSize);
bool linux_daemon_startup_store(const char* path,
                                const LinuxDaemonStartupRecord* record,
                                char* err, size_t errSize);

#endif
