// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_DAEMON_STATE_H
#define GREEN_CURVE_LINUX_DAEMON_STATE_H

#include <stddef.h>
#include <string.h>
#include "gpu_core.h"
#include "linux_auto_restore_policy.h"

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
    // v2 records the answer's ServiceOutcomeSeverity next to its status, so a
    // retry replayed from this file cannot report a warning as a clean success.
    // A v1 file fails the version check and is discarded; that is the existing
    // "no valid persisted result" path, not a new failure mode.
    LINUX_DAEMON_OPERATION_VERSION = 2,
};

struct LinuxDaemonOperationRecord {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 state;
    gc_u64 operationId;
    gc_u32 responseStatus;
    gc_u32 outcomeSeverity;
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
    gc_u32 responseStatus, gc_u32 outcomeSeverity, const char* message) {
    if (!record) return;
    memset(record, 0, sizeof(*record));
    record->magic = LINUX_DAEMON_OPERATION_MAGIC;
    record->version = LINUX_DAEMON_OPERATION_VERSION;
    record->size = sizeof(*record);
    record->state = state;
    record->operationId = operationId;
    record->responseStatus = responseStatus;
    record->outcomeSeverity = service_response_resolve_outcome_severity(
        responseStatus, outcomeSeverity);
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

// Automatic-restore guard.  Stored beside active.bin with the same root-owned
// checksummed atomic write, for the same reason: it decides whether an
// unattended hardware write may happen, so an unprivileged user must not be
// able to forge it and a half-written copy must never be read back as "three
// fresh attempts".
// Version 2 added `lockoutReason`.  A version-1 file is rejected by
// linux_daemon_guard_valid() and therefore read as corrupt, which fails closed
// to locked out until an explicit Apply or Reset -- the safe direction, and the
// only one that does not silently invent a reason for a latch it cannot read.
enum {
    LINUX_DAEMON_GUARD_MAGIC = 0x44475247u, // "GRGD"
    LINUX_DAEMON_GUARD_VERSION = 2,
};

struct LinuxDaemonRestoreGuardRecord {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 lockedOut;
    gc_u32 startAttempts;
    // ServiceAutoRestoreLockoutReason.  Persisted because it is published in
    // every snapshot, not only logged once at the moment of the latch.
    gc_u32 lockoutReason;
    char bootId[LINUX_BOOT_ID_MAX];
    gc_u32 checksum;
};

static inline gc_u32 linux_daemon_guard_checksum(
    const LinuxDaemonRestoreGuardRecord* record) {
    if (!record) return 0;
    const unsigned char* bytes = (const unsigned char*)record;
    gc_u32 hash = 2166136261u;
    for (size_t i = 0; i < offsetof(LinuxDaemonRestoreGuardRecord, checksum); ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static inline void linux_daemon_guard_initialize(
    LinuxDaemonRestoreGuardRecord* record, const LinuxAutoRestoreGuard* guard) {
    if (!record) return;
    memset(record, 0, sizeof(*record));
    record->magic = LINUX_DAEMON_GUARD_MAGIC;
    record->version = LINUX_DAEMON_GUARD_VERSION;
    record->size = (gc_u32)sizeof(*record);
    if (guard) {
        record->lockedOut = guard->lockedOut ? 1u : 0u;
        record->startAttempts = guard->startAttempts;
        // Written through the same coherence rule the validator enforces, so a
        // guard that was mutated field-by-field cannot produce a record this
        // code would refuse to read back.
        record->lockoutReason = record->lockedOut
            ? linux_auto_restore_published_lockout_reason(guard, false)
            : SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
        memcpy(record->bootId, guard->bootId, sizeof(record->bootId));
        record->bootId[sizeof(record->bootId) - 1] = '\0';
    }
    record->checksum = linux_daemon_guard_checksum(record);
}

static inline bool linux_daemon_guard_valid(
    const LinuxDaemonRestoreGuardRecord* record) {
    // lockedOut is a wire-style boolean: anything else means the record was
    // written by something that is not this code, so it is not trusted to be a
    // permission to write the GPU.
    return record && record->magic == LINUX_DAEMON_GUARD_MAGIC &&
           record->version == LINUX_DAEMON_GUARD_VERSION &&
           record->size == sizeof(*record) &&
           record->lockedOut <= 1u &&
           record->lockoutReason <=
               (gc_u32)SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED &&
           // Coherent or rejected, the same rule the wire envelope follows: a
           // latched lockout always names a reason, and a clear guard never
           // carries one.  Either half alone would be published as a fact.
           ((record->lockedOut != 0u) ==
            (record->lockoutReason != SERVICE_AUTO_RESTORE_LOCKOUT_NONE)) &&
           service_wire_string_is_terminated(
               record->bootId, (unsigned int)sizeof(record->bootId)) &&
           record->checksum == linux_daemon_guard_checksum(record);
}

static inline void linux_daemon_guard_to_policy(
    const LinuxDaemonRestoreGuardRecord* record, LinuxAutoRestoreGuard* guard) {
    if (!guard) return;
    memset(guard, 0, sizeof(*guard));
    if (!record) return;
    guard->lockedOut = record->lockedOut ? 1 : 0;
    guard->startAttempts = record->startAttempts;
    guard->lockoutReason = record->lockedOut ? record->lockoutReason
                                             : SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    memcpy(guard->bootId, record->bootId, sizeof(guard->bootId));
    guard->bootId[sizeof(guard->bootId) - 1] = '\0';
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

// Automatic-restore guard.  A missing record means "nothing has gone wrong
// yet"; an unreadable or corrupt one fails CLOSED to locked out, because the
// counter that decides whether replaying the committed intent is still safe is
// exactly the thing that must not be lost by a damaged file.  `outCorrupt`
// distinguishes the two so the daemon can say which happened.
bool linux_daemon_guard_load(const char* path, LinuxAutoRestoreGuard* guard,
                             bool* outCorrupt, char* err, size_t errSize);
bool linux_daemon_guard_store(const char* path,
                              const LinuxAutoRestoreGuard* guard,
                              char* err, size_t errSize);
// Reads /proc/sys/kernel/random/boot_id.  The Linux counterpart of the Windows
// 128-bit BootIdentifier: stable for one real boot, and unaffected by a
// wall-clock correction.
bool linux_read_boot_id(char* out, size_t outSize);

#endif
