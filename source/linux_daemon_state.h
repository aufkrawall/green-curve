// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_LINUX_DAEMON_STATE_H
#define GREEN_CURVE_LINUX_DAEMON_STATE_H

#include <stddef.h>
#include <string.h>
#include "gpu_core.h"
#include "linux_auto_restore_policy.h"
#include "ownership_handback_policy.h"

enum LinuxDaemonRecordState : gc_u32 {
    LINUX_DAEMON_RECORD_PREPARED = 1,
    LINUX_DAEMON_RECORD_ACTIVE = 2,
    LINUX_DAEMON_RECORD_UNCERTAIN = 3,
};

enum {
    LINUX_DAEMON_RECORD_MAGIC = 0x4752434Cu, // "LCRG"
    // v4 embeds the schema-2 DesiredSettings (per-point curve provenance,
    // 0.26.0).  The layout GREW, which is why the version had to move: see
    // desired_settings_schema.h for why size, not version, now selects the
    // layout a loader decodes with.
    LINUX_DAEMON_RECORD_VERSION = 4,
    // Semantics markers WITHIN the frozen schema-1 layout.  v3 reinterprets
    // the embedded memOffsetMHz as display MHz (effective/display parity);
    // v1 and v2 stored effective MHz and must be halved exactly once before a
    // restore-last replay, or the overclock is written twice as strong as the
    // user chose.  v1 additionally predates the operation-id fields, so it is
    // a narrower record -- a separate layout, not a separate meaning.
    LINUX_DAEMON_RECORD_PRE_PROVENANCE_VERSION = 3,
    LINUX_DAEMON_RECORD_PRE_DISPLAY_MEM_UNITS_VERSION = 2,
    LINUX_DAEMON_RECORD_PRE_OPERATION_ID_VERSION = 1,
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

// The live record's size is part of the upgrade contract, because the loader
// admits a file by comparing st_size against it.  GpuAdapterInfo is embedded
// here too, so this assert covers a change to either struct.
static_assert(sizeof(LinuxDaemonStateRecord) == 1176,
              "LinuxDaemonStateRecord changed size: freeze the outgoing layout "
              "as LinuxDaemonStateRecordSchema<N>, teach the loader its size, "
              "and bump LINUX_DAEMON_RECORD_VERSION "
              "(see desired_settings_schema.h)");

// FROZEN: what 0.25.2 and earlier wrote for record versions 2 and 3.  Same
// fields as the live record, schema-1 DesiredSettings.  Never edit.
struct LinuxDaemonStateRecordSchema1 {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 state;
    GpuAdapterInfo targetGpu;
    DesiredSettingsSchema1 desired;
    gc_u64 operationId;
    gc_u32 operationState;
    gc_u32 checksum;
};
static_assert(sizeof(LinuxDaemonStateRecordSchema1) == 1048,
              "LinuxDaemonStateRecordSchema1 is a FROZEN on-disk layout");

// FROZEN: record version 1, which predates operationId/operationState.  Only
// the v1 files written between the video-clock field landing and the v2 bump
// carry the schema-1 DesiredSettings and therefore this size; older v1 files
// embed layouts the version number never distinguished, so they are not
// identifiable and are discarded rather than guessed at.
struct LinuxDaemonStateRecordSchema1V1 {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 state;
    GpuAdapterInfo targetGpu;
    DesiredSettingsSchema1 desired;
    gc_u32 checksum;
};
static_assert(sizeof(LinuxDaemonStateRecordSchema1V1) == 1036,
              "LinuxDaemonStateRecordSchema1V1 is a FROZEN on-disk layout");

// Distinct sizes are what makes size-based dispatch unambiguous.  If a future
// layout ever collides with one of these, the loader must gain a different
// discriminator before that layout ships.
static_assert(sizeof(LinuxDaemonStateRecord) != sizeof(LinuxDaemonStateRecordSchema1) &&
              sizeof(LinuxDaemonStateRecord) != sizeof(LinuxDaemonStateRecordSchema1V1) &&
              sizeof(LinuxDaemonStateRecordSchema1) != sizeof(LinuxDaemonStateRecordSchema1V1),
              "daemon state record layouts must be distinguishable by size");

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

// FNV-1a over the leading `length` bytes of a record, i.e. everything up to
// its own checksum member.  Shared by the live and the frozen layouts so a
// migration validates a stored record with exactly the arithmetic that wrote
// it, no transcription of the loop per generation.
static inline gc_u32 linux_daemon_record_hash_bytes(const void* record, size_t length) {
    if (!record) return 0;
    const unsigned char* bytes = (const unsigned char*)record;
    gc_u32 hash = 2166136261u;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

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

// Every invariant except the version constant, evaluated with whatever
// version bytes the record carries: a stored pre-parity checksum was computed
// over bytes that contained the old version, so it validates only before any
// migration mutates the copy.
static inline bool linux_daemon_record_valid_except_version(
    const LinuxDaemonStateRecord* record) {
    return record && record->magic == LINUX_DAEMON_RECORD_MAGIC &&
           record->size == sizeof(*record) &&
           record->state >= LINUX_DAEMON_RECORD_PREPARED &&
           record->state <= LINUX_DAEMON_RECORD_UNCERTAIN &&
           record->operationState <= SERVICE_OPERATION_OUTCOME_UNKNOWN &&
           record->checksum == linux_daemon_record_checksum(record);
}

static inline bool linux_daemon_record_valid(const LinuxDaemonStateRecord* record) {
    return linux_daemon_record_valid_except_version(record) &&
           record->version == LINUX_DAEMON_RECORD_VERSION;
}

// Halve a pre-parity DesiredSettings.memOffsetMHz (stored effective MHz) to
// the canonical display MHz. Returns true when the field was present and
// converted; *oldOut receives the pre-conversion value when non-null. Shared
// by the INI migration gate tests and both daemon record migrations.
static inline bool linux_daemon_migrate_desired_mem_units_to_display(
    DesiredSettings* desired, int* oldOut) {
    if (!desired || !desired->hasMemOffset) return false;
    if (oldOut) *oldOut = desired->memOffsetMHz;
    desired->memOffsetMHz =
        nvml_mem_display_mhz_from_effective_mhz(desired->memOffsetMHz);
    return true;
}

// How a stored record was recognized, for the loader's log line.  A migration
// that cannot say which generation it came from is a migration nobody can
// debug from a support log.
enum LinuxDaemonRecordGeneration : gc_u32 {
    LINUX_DAEMON_RECORD_GENERATION_CURRENT = 0,
    LINUX_DAEMON_RECORD_GENERATION_SCHEMA1 = 1,    // versions 2 and 3
    LINUX_DAEMON_RECORD_GENERATION_SCHEMA1_V1 = 2, // version 1, no operation id
};

// Validate a stored schema-1 state record against its OWN checksum, over its
// OWN bytes, with whatever version it carries.  Nothing may mutate before
// this passes: the stored hash covers the stored version field.
static inline bool linux_daemon_state_schema1_valid(
    const LinuxDaemonStateRecordSchema1* record) {
    return record && record->magic == LINUX_DAEMON_RECORD_MAGIC &&
           record->size == sizeof(*record) &&
           record->version >= LINUX_DAEMON_RECORD_PRE_DISPLAY_MEM_UNITS_VERSION &&
           record->version <= LINUX_DAEMON_RECORD_PRE_PROVENANCE_VERSION &&
           record->state >= LINUX_DAEMON_RECORD_PREPARED &&
           record->state <= LINUX_DAEMON_RECORD_UNCERTAIN &&
           record->operationState <= SERVICE_OPERATION_OUTCOME_UNKNOWN &&
           record->checksum == linux_daemon_record_hash_bytes(
               record, offsetof(LinuxDaemonStateRecordSchema1, checksum));
}

static inline bool linux_daemon_state_schema1_v1_valid(
    const LinuxDaemonStateRecordSchema1V1* record) {
    return record && record->magic == LINUX_DAEMON_RECORD_MAGIC &&
           record->size == sizeof(*record) &&
           record->version == LINUX_DAEMON_RECORD_PRE_OPERATION_ID_VERSION &&
           record->state >= LINUX_DAEMON_RECORD_PREPARED &&
           record->state <= LINUX_DAEMON_RECORD_UNCERTAIN &&
           record->checksum == linux_daemon_record_hash_bytes(
               record, offsetof(LinuxDaemonStateRecordSchema1V1, checksum));
}

// Widen a validated schema-1 state record into the current generation.  The
// memory-offset unit conversion is applied here, once, for the versions that
// stored effective MHz -- the value migration rides on the version, the layout
// migration on the size, and neither is inferred from the other.
static inline bool linux_daemon_state_record_widen_schema1(
    LinuxDaemonStateRecord* out, const LinuxDaemonStateRecordSchema1* in,
    int* oldMemOut, gc_u32* fromVersionOut) {
    if (oldMemOut) *oldMemOut = 0;
    if (fromVersionOut) *fromVersionOut = 0;
    if (!out || !in || !linux_daemon_state_schema1_valid(in)) return false;
    if (fromVersionOut) *fromVersionOut = in->version;
    DesiredSettings widened = {};
    desired_settings_widen_from_schema1(&widened, &in->desired);
    if (in->version <= LINUX_DAEMON_RECORD_PRE_DISPLAY_MEM_UNITS_VERSION) {
        int migratedOld = 0;
        if (linux_daemon_migrate_desired_mem_units_to_display(&widened, &migratedOld) &&
            oldMemOut) *oldMemOut = migratedOld;
    }
    linux_daemon_record_initialize(out, (LinuxDaemonRecordState)in->state,
                                   &in->targetGpu, &widened, in->operationId,
                                   in->operationState);
    return true;
}

static inline bool linux_daemon_state_record_widen_schema1_v1(
    LinuxDaemonStateRecord* out, const LinuxDaemonStateRecordSchema1V1* in,
    int* oldMemOut) {
    if (oldMemOut) *oldMemOut = 0;
    if (!out || !in || !linux_daemon_state_schema1_v1_valid(in)) return false;
    DesiredSettings widened = {};
    desired_settings_widen_from_schema1(&widened, &in->desired);
    int migratedOld = 0;
    if (linux_daemon_migrate_desired_mem_units_to_display(&widened, &migratedOld) &&
        oldMemOut) *oldMemOut = migratedOld;
    // v1 carried no operation identity; a widened record starts with none,
    // which is the same "no operation in flight" state a fresh record has.
    linux_daemon_record_initialize(out, (LinuxDaemonRecordState)in->state,
                                   &in->targetGpu, &widened);
    return true;
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
    // v3 embeds the schema-2 DesiredSettings (per-point curve provenance,
    // 0.26.0) and is a LARGER record than v1/v2.  Within the frozen schema-1
    // layout, v2 reinterprets the embedded memOffsetMHz as display MHz exactly
    // like the state record's v2->v3 bump, so a v1 record is halved once on
    // read.  Size selects the layout, version the meaning; see
    // desired_settings_schema.h.
    LINUX_DAEMON_STARTUP_VERSION = 3,
    LINUX_DAEMON_STARTUP_PRE_PROVENANCE_VERSION = 2,
    LINUX_DAEMON_STARTUP_PRE_DISPLAY_MEM_UNITS_VERSION = 1,
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

static_assert(sizeof(LinuxDaemonStartupRecord) == 1236,
              "LinuxDaemonStartupRecord changed size: freeze the outgoing "
              "layout as LinuxDaemonStartupRecordSchema<N>, teach the loader "
              "its size, and bump LINUX_DAEMON_STARTUP_VERSION "
              "(see desired_settings_schema.h)");

// FROZEN: what 0.25.2 and earlier wrote for startup-policy versions 1 and 2.
// Never edit.
struct LinuxDaemonStartupRecordSchema1 {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 mode;
    gc_u32 profileSlot;
    gc_u32 reserved;
    char profileName[LINUX_DAEMON_STARTUP_NAME_MAX];
    GpuAdapterInfo targetGpu;
    DesiredSettingsSchema1 desired;
    gc_u32 checksum;
};
static_assert(sizeof(LinuxDaemonStartupRecordSchema1) == 1108,
              "LinuxDaemonStartupRecordSchema1 is a FROZEN on-disk layout");
static_assert(sizeof(LinuxDaemonStartupRecord) != sizeof(LinuxDaemonStartupRecordSchema1),
              "startup record layouts must be distinguishable by size");

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

// Every invariant except the version constant, evaluated with whatever
// version bytes the record carries (see the state-record counterpart).
static inline bool linux_daemon_startup_valid_except_version(
    const LinuxDaemonStartupRecord* record) {
    if (!record || record->magic != LINUX_DAEMON_STARTUP_MAGIC ||
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

static inline bool linux_daemon_startup_valid(
    const LinuxDaemonStartupRecord* record) {
    return linux_daemon_startup_valid_except_version(record) &&
           record->version == LINUX_DAEMON_STARTUP_VERSION;
}

// Validate a stored schema-1 startup record against its own checksum, over
// its own bytes, before anything mutates.
static inline bool linux_daemon_startup_schema1_valid(
    const LinuxDaemonStartupRecordSchema1* record) {
    if (!record || record->magic != LINUX_DAEMON_STARTUP_MAGIC ||
        record->size != sizeof(*record) ||
        record->version < LINUX_DAEMON_STARTUP_PRE_DISPLAY_MEM_UNITS_VERSION ||
        record->version > LINUX_DAEMON_STARTUP_PRE_PROVENANCE_VERSION ||
        record->mode >= SERVICE_STARTUP_POLICY_MODE_COUNT ||
        record->checksum != linux_daemon_record_hash_bytes(
            record, offsetof(LinuxDaemonStartupRecordSchema1, checksum)))
        return false;
    if (!service_wire_string_is_terminated(
            record->profileName, (unsigned int)sizeof(record->profileName)))
        return false;
    if (record->mode == SERVICE_STARTUP_POLICY_PROFILE) {
        return record->profileSlot >= 1 &&
               record->profileSlot <= (gc_u32)CONFIG_NUM_SLOTS &&
               record->targetGpu.valid && record->targetGpu.pciInfoValid;
    }
    return record->profileSlot == 0 && !record->targetGpu.valid;
}

// Widen a validated schema-1 startup record into the current generation, with
// the memory-offset unit conversion applied once for the version that stored
// effective MHz.
static inline bool linux_daemon_startup_widen_schema1(
    LinuxDaemonStartupRecord* out, const LinuxDaemonStartupRecordSchema1* in,
    int* oldMemOut, gc_u32* fromVersionOut) {
    if (oldMemOut) *oldMemOut = 0;
    if (fromVersionOut) *fromVersionOut = 0;
    if (!out || !in || !linux_daemon_startup_schema1_valid(in)) return false;
    if (fromVersionOut) *fromVersionOut = in->version;
    DesiredSettings widened = {};
    desired_settings_widen_from_schema1(&widened, &in->desired);
    if (in->version <= LINUX_DAEMON_STARTUP_PRE_DISPLAY_MEM_UNITS_VERSION) {
        int migratedOld = 0;
        if (linux_daemon_migrate_desired_mem_units_to_display(&widened, &migratedOld) &&
            oldMemOut) *oldMemOut = migratedOld;
    }
    linux_daemon_startup_initialize(out, in->mode, in->profileSlot,
                                    in->profileName, &in->targetGpu, &widened);
    return true;
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
                                                   char* err, size_t errSize,
                                                   bool* outMigratedFromLegacy);
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
// than silently falling back to replaying old intent.  `outMigratedFromLegacy`
// reports that an OLDER on-disk generation was widened into the current record
// -- a layout migration, a mem-unit reinterpretation, or both -- so the caller
// can rewrite the file at the current generation; the detail rides in `err`
// even on success.
bool linux_daemon_startup_load(const char* path,
                               LinuxDaemonStartupRecord* record,
                               bool* outCorrupt, char* err, size_t errSize,
                               bool* outMigratedFromLegacy);
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

// Fan ownership marker (ownership_handback_policy.h).  Same root-owned,
// private, atomic store as the records above.  Load returns 0 when absent,
// 1 for a valid marker, -1 for one that is present but unusable.
bool linux_fan_ownership_marker_store(const char* path,
                                      const LinuxFanOwnershipMarker* marker,
                                      char* err, size_t errSize);
int linux_fan_ownership_marker_load(const char* path,
                                    LinuxFanOwnershipMarker* marker,
                                    char* err, size_t errSize);

#endif
