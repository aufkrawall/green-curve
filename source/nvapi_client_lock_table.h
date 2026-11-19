// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_NVAPI_CLIENT_LOCK_TABLE_H
#define GREEN_CURVE_NVAPI_CLIENT_LOCK_TABLE_H
// Read-only decoder for the driver's client clock/voltage lock table: which
// graphics clock floor/ceiling and which VF voltage lock any client (another
// tool, nvidia-smi, or Green Curve itself) currently holds. Reading it needs
// no GPU load, unlike inferring a lock from clocks sampled under a workload.
//
// Layout (version 1, 0x30C bytes): u32 version word, u32 flags, u32 entry
// count (<= 32), then 32 entries of 24 bytes {u32 index, u32, u32 mode, u32,
// u32 value, u32}. Validated 2026-11-19 on an RTX 5070 (driver 621.24) by
// reading the table idle, then with three external locks held one at a time:
//   - NVML locked clocks 1500/1500: entries 0 and 1, mode 2, value 1500000 kHz
//   - NVML locked clocks 0/1500:    entry 0 mode 2 value 1500000 kHz,
//                                   entry 1 mode 2 value 0 (no floor)
//   - a VF curve-point lock at 925 mV: entry 6, mode 3, value 925000 uV
// and every mode returned to 0 after each lock was released. So entry 0 is
// the graphics clock ceiling, entry 1 the floor (kHz), entry 6 the voltage
// lock (uV). Nothing else has been observed; any other nonzero mode, a
// mismatched index or an implausible value makes the whole reading unknown
// rather than guessed (the caller then keeps pin knowledge unknown).
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "gpu_wire_types.h"

#define NVAPI_CLIENT_LOCK_TABLE_GET 0xE440B867u
enum : unsigned int {
    NVAPI_CLIENT_LOCK_TABLE_SIZE = 0x30Cu,
    NVAPI_CLIENT_LOCK_TABLE_VERSION_WORD = (1u << 16) | NVAPI_CLIENT_LOCK_TABLE_SIZE,
    NVAPI_CLIENT_LOCK_TABLE_COUNT_OFFSET = 8u,
    NVAPI_CLIENT_LOCK_TABLE_ENTRIES_OFFSET = 12u,
    NVAPI_CLIENT_LOCK_TABLE_ENTRY_STRIDE = 24u,
    NVAPI_CLIENT_LOCK_TABLE_MAX_ENTRIES = 32u,
    NVAPI_CLIENT_LOCK_ENTRY_MODE_OFFSET = 8u,
    NVAPI_CLIENT_LOCK_ENTRY_VALUE_OFFSET = 16u,
    NVAPI_CLIENT_LOCK_MODE_NONE = 0u,
    NVAPI_CLIENT_LOCK_MODE_FREQUENCY = 2u,
    NVAPI_CLIENT_LOCK_MODE_VOLTAGE = 3u,
    NVAPI_CLIENT_LOCK_ENTRY_GRAPHICS_MAX = 0u,
    NVAPI_CLIENT_LOCK_ENTRY_GRAPHICS_MIN = 1u,
    NVAPI_CLIENT_LOCK_ENTRY_VOLTAGE = 6u,
};
static_assert(NVAPI_CLIENT_LOCK_TABLE_ENTRIES_OFFSET +
                  NVAPI_CLIENT_LOCK_TABLE_MAX_ENTRIES * NVAPI_CLIENT_LOCK_TABLE_ENTRY_STRIDE ==
                  NVAPI_CLIENT_LOCK_TABLE_SIZE,
              "client lock table geometry");

struct NvapiClientLocks {
    bool known;                 // decoded with only validated entries
    int status;                 // driver status, or a negative decoder refusal
    bool graphicsMaxPresent, graphicsMinPresent, voltagePresent;
    gc_i64 graphicsMaxHz, graphicsMinHz;
    gc_i64 voltageUv;
    unsigned int entryCount;
    // First entry that made the reading unknown, for one diagnostic line.
    int rejectedIndex;
    unsigned int rejectedMode, rejectedValue;
};
enum : int {
    NVAPI_CLIENT_LOCK_REFUSED_HEADER = -100010,
    NVAPI_CLIENT_LOCK_REFUSED_ENTRY = -100011,
    NVAPI_CLIENT_LOCK_REFUSED_VALUE = -100012,
};
// Plausibility bounds, wide enough for any shipped GPU and narrow enough to
// reject a misread word: 1 MHz..10 GHz clocks, 0.2..2 V.
static inline bool nvapi_client_lock_clock_plausible(unsigned int kHz) {
    return kHz >= 1000u && kHz <= 10000000u;
}
static inline bool nvapi_client_lock_voltage_plausible(unsigned int uV) {
    return uV >= 200000u && uV <= 2000000u;
}
static inline unsigned int nvapi_client_lock_u32(const unsigned char* buffer, unsigned int offset) {
    unsigned int value = 0;
    memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

// Pure: decode a buffer the driver returned with `status`.
static inline NvapiClientLocks nvapi_client_locks_decode(const unsigned char* buffer, size_t size, int status) {
    NvapiClientLocks out{};
    out.status = status;
    out.rejectedIndex = -1;
    if (status != 0) return out;
    if (!buffer || size < NVAPI_CLIENT_LOCK_TABLE_SIZE ||
        nvapi_client_lock_u32(buffer, 0) != NVAPI_CLIENT_LOCK_TABLE_VERSION_WORD) {
        out.status = NVAPI_CLIENT_LOCK_REFUSED_HEADER;
        return out;
    }
    const unsigned int count = nvapi_client_lock_u32(buffer, NVAPI_CLIENT_LOCK_TABLE_COUNT_OFFSET);
    if (count > NVAPI_CLIENT_LOCK_TABLE_MAX_ENTRIES) {
        out.status = NVAPI_CLIENT_LOCK_REFUSED_HEADER;
        return out;
    }
    out.entryCount = count;
    const auto reject = [&](int why, unsigned int index, unsigned int mode, unsigned int value) {
        out = NvapiClientLocks{};
        out.status = why; out.entryCount = count;
        out.rejectedIndex = (int)index; out.rejectedMode = mode; out.rejectedValue = value;
        return out;
    };
    for (unsigned int i = 0; i < count; ++i) {
        const unsigned int at = NVAPI_CLIENT_LOCK_TABLE_ENTRIES_OFFSET + i * NVAPI_CLIENT_LOCK_TABLE_ENTRY_STRIDE;
        const unsigned int index = nvapi_client_lock_u32(buffer, at);
        const unsigned int mode = nvapi_client_lock_u32(buffer, at + NVAPI_CLIENT_LOCK_ENTRY_MODE_OFFSET);
        const unsigned int value = nvapi_client_lock_u32(buffer, at + NVAPI_CLIENT_LOCK_ENTRY_VALUE_OFFSET);
        if (index != i) return reject(NVAPI_CLIENT_LOCK_REFUSED_ENTRY, i, mode, value);
        if (mode == NVAPI_CLIENT_LOCK_MODE_NONE) continue;
        const bool graphicsClock = mode == NVAPI_CLIENT_LOCK_MODE_FREQUENCY &&
            (i == NVAPI_CLIENT_LOCK_ENTRY_GRAPHICS_MAX || i == NVAPI_CLIENT_LOCK_ENTRY_GRAPHICS_MIN);
        const bool voltage = mode == NVAPI_CLIENT_LOCK_MODE_VOLTAGE && i == NVAPI_CLIENT_LOCK_ENTRY_VOLTAGE;
        if (!graphicsClock && !voltage) return reject(NVAPI_CLIENT_LOCK_REFUSED_ENTRY, i, mode, value);
        if (graphicsClock) {
            if (!value) continue;  // a frequency entry with no bound (e.g. no floor)
            if (!nvapi_client_lock_clock_plausible(value)) return reject(NVAPI_CLIENT_LOCK_REFUSED_VALUE, i, mode, value);
            if (i == NVAPI_CLIENT_LOCK_ENTRY_GRAPHICS_MAX) {
                out.graphicsMaxPresent = true; out.graphicsMaxHz = (gc_i64)value * 1000;
            } else {
                out.graphicsMinPresent = true; out.graphicsMinHz = (gc_i64)value * 1000;
            }
        } else {
            if (!nvapi_client_lock_voltage_plausible(value)) return reject(NVAPI_CLIENT_LOCK_REFUSED_VALUE, i, mode, value);
            out.voltagePresent = true; out.voltageUv = value;
        }
    }
    // A floor above its ceiling, or a floor with no ceiling, has never been
    // observed; do not guess what the driver enforces then.
    if (out.graphicsMinPresent && (!out.graphicsMaxPresent || out.graphicsMinHz > out.graphicsMaxHz))
        return reject(NVAPI_CLIENT_LOCK_REFUSED_VALUE, NVAPI_CLIENT_LOCK_ENTRY_GRAPHICS_MIN,
            NVAPI_CLIENT_LOCK_MODE_FREQUENCY, (unsigned int)(out.graphicsMinHz / 1000));
    out.known = true;
    return out;
}

// Calls only the getter. `query` resolves an interface id to a function.
template <class Query>
static inline NvapiClientLocks nvapi_client_locks_read(Query query, void* gpuHandle) {
    using Getter = int (*)(void*, void*);
    NvapiClientLocks missing{};
    missing.status = -100001;
    missing.rejectedIndex = -1;
    if (!gpuHandle) return missing;
    const auto get = (Getter)query(NVAPI_CLIENT_LOCK_TABLE_GET);
    if (!get) return missing;
    unsigned char buffer[NVAPI_CLIENT_LOCK_TABLE_SIZE];
    memset(buffer, 0, sizeof(buffer));
    const unsigned int word = NVAPI_CLIENT_LOCK_TABLE_VERSION_WORD;
    memcpy(buffer, &word, sizeof(word));
    const int status = get(gpuHandle, buffer);
    return nvapi_client_locks_decode(buffer, sizeof(buffer), status);
}

// Whether a reading differs from the previous one in anything a log line
// shows. A capture reads the table at least twice; callers log a reading
// only when this returns true, so an unchanged state is logged once.
struct NvapiClientLocksLogGate { bool seen; NvapiClientLocks last; };
static inline bool nvapi_client_locks_log_changed(NvapiClientLocksLogGate& gate, const NvapiClientLocks& l) {
    const auto& p = gate.last;
    const bool same = gate.seen && p.known == l.known && p.status == l.status &&
        p.graphicsMaxPresent == l.graphicsMaxPresent && p.graphicsMinPresent == l.graphicsMinPresent &&
        p.voltagePresent == l.voltagePresent && p.graphicsMaxHz == l.graphicsMaxHz &&
        p.graphicsMinHz == l.graphicsMinHz && p.voltageUv == l.voltageUv && p.entryCount == l.entryCount &&
        p.rejectedIndex == l.rejectedIndex && p.rejectedMode == l.rejectedMode && p.rejectedValue == l.rejectedValue;
    gate.seen = true;
    gate.last = l;
    return !same;
}
// One bounded diagnostic line.
static inline void nvapi_client_locks_describe(const NvapiClientLocks& l, char* out, size_t size) {
    if (!out || !size) return;
    if (!l.known) {
        snprintf(out, size, "client-locks: unknown status=%d entries=%u rejectedIndex=%d mode=%u value=%u",
            l.status, l.entryCount, l.rejectedIndex, l.rejectedMode, l.rejectedValue);
        return;
    }
    snprintf(out, size, "client-locks: known entries=%u graphicsMin=%lld graphicsMax=%lld voltageUv=%lld",
        l.entryCount, l.graphicsMinPresent ? (long long)l.graphicsMinHz : 0LL,
        l.graphicsMaxPresent ? (long long)l.graphicsMaxHz : 0LL, l.voltagePresent ? (long long)l.voltageUv : 0LL);
}
#endif
