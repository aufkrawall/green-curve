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
    LINUX_DAEMON_RECORD_VERSION = 1,
};

struct LinuxDaemonStateRecord {
    gc_u32 magic;
    gc_u32 version;
    gc_u32 size;
    gc_u32 state;
    GpuAdapterInfo targetGpu;
    DesiredSettings desired;
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
                                                  const DesiredSettings* desired) {
    if (!record) return;
    memset(record, 0, sizeof(*record));
    record->magic = LINUX_DAEMON_RECORD_MAGIC;
    record->version = LINUX_DAEMON_RECORD_VERSION;
    record->size = (gc_u32)sizeof(*record);
    record->state = (gc_u32)state;
    if (target) record->targetGpu = *target;
    if (desired) record->desired = *desired;
    record->checksum = linux_daemon_record_checksum(record);
}

static inline bool linux_daemon_record_valid(const LinuxDaemonStateRecord* record) {
    return record && record->magic == LINUX_DAEMON_RECORD_MAGIC &&
           record->version == LINUX_DAEMON_RECORD_VERSION &&
           record->size == sizeof(*record) &&
           record->state >= LINUX_DAEMON_RECORD_PREPARED &&
           record->state <= LINUX_DAEMON_RECORD_UNCERTAIN &&
           record->checksum == linux_daemon_record_checksum(record);
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

#endif
