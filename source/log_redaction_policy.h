// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Stable, non-reversible log tokens for identifiers and filesystem paths.
// Correlating two events for the same account/config remains possible without
// putting a SAM name, SID, authentication LUID, or user-profile path into a
// support log that is enabled by default.
#ifndef GREEN_CURVE_LOG_REDACTION_POLICY_H
#define GREEN_CURVE_LOG_REDACTION_POLICY_H

#include <stddef.h>

static inline unsigned long long gc_log_fingerprint_bytes(
    const char* value) {
    unsigned long long hash = 1469598103934665603ULL;
    if (value) {
        while (*value) {
            hash ^= static_cast<unsigned char>(*value++);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

static inline const char* gc_log_path_token(
    const char* path, char* out, size_t outSize) {
    if (!out || outSize == 0) return "<log-error>";
    if (!path || !path[0]) {
        out[0] = '-';
        out[1] = '\0';
        return out;
    }
    if (outSize < 32) return "<log-error>";
    unsigned long long hash = gc_log_fingerprint_bytes(path);
    out[0] = '['; out[1] = 'p'; out[2] = 'a'; out[3] = 't'; out[4] = 'h';
    out[5] = ' '; out[6] = '#';
    for (int shift = 60, at = 7; shift >= 0; shift -= 4, ++at)
        out[at] = "0123456789abcdef"[(hash >> shift) & 0xfU];
    out[23] = ']';
    out[24] = '\0';
    return out;
}

static inline const char* gc_log_identifier_token(
    const char* value, char* out, size_t outSize) {
    if (!out || outSize == 0) return "<log-error>";
    if (!value || !value[0]) {
        out[0] = '-';
        out[1] = '\0';
        return out;
    }
    if (outSize < 32) return "<log-error>";
    unsigned long long hash = gc_log_fingerprint_bytes(value);
    out[0] = '['; out[1] = 'i'; out[2] = 'd'; out[3] = ' '; out[4] = '#';
    for (int shift = 60, at = 5; shift >= 0; shift -= 4, ++at)
        out[at] = "0123456789abcdef"[(hash >> shift) & 0xfU];
    out[21] = ']';
    out[22] = '\0';
    return out;
}

// UTF-16 identifiers (the Windows Task Scheduler task name is
// "Green Curve Startup - <DOMAIN_account>", which is identity-bearing).  Hashes
// the code units directly rather than converting, so this stays pure and the
// regression harness can pin it on a Linux host with no Win32 API.
static inline unsigned long long gc_log_fingerprint_wide(const wchar_t* value) {
    unsigned long long hash = 1469598103934665603ULL;
    if (value) {
        while (*value) {
            unsigned long long unit = (unsigned long long)(unsigned int)*value++;
            for (unsigned int shift = 0; shift < 32; shift += 8) {
                hash ^= static_cast<unsigned char>((unit >> shift) & 0xffU);
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

static inline const char* gc_log_wide_identifier_token(
    const wchar_t* value, char* out, size_t outSize) {
    if (!out || outSize == 0) return "<log-error>";
    if (!value || !value[0]) {
        out[0] = '-';
        out[1] = '\0';
        return out;
    }
    if (outSize < 32) return "<log-error>";
    unsigned long long hash = gc_log_fingerprint_wide(value);
    out[0] = '['; out[1] = 'i'; out[2] = 'd'; out[3] = ' '; out[4] = '#';
    for (int shift = 60, at = 5; shift >= 0; shift -= 4, ++at)
        out[at] = "0123456789abcdef"[(hash >> shift) & 0xfU];
    out[21] = ']';
    out[22] = '\0';
    return out;
}

static inline unsigned long long gc_log_fingerprint_u64(unsigned long long value) {
    unsigned long long hash = 1469598103934665603ULL;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        hash ^= static_cast<unsigned char>((value >> shift) & 0xffU);
        hash *= 1099511628211ULL;
    }
    return hash;
}

static inline const char* gc_log_u64_token(
    unsigned long long value, char* out, size_t outSize) {
    if (!out || outSize == 0) return "<log-error>";
    if (outSize < 32) return "<log-error>";
    unsigned long long hash = gc_log_fingerprint_u64(value);
    out[0] = '['; out[1] = 'i'; out[2] = 'd'; out[3] = ' '; out[4] = '#';
    for (int shift = 60, at = 5; shift >= 0; shift -= 4, ++at)
        out[at] = "0123456789abcdef"[(hash >> shift) & 0xfU];
    out[21] = ']';
    out[22] = '\0';
    return out;
}

// Short-lived printf arguments. Each call gets its own buffer so multiple
// paths in one record remain distinct regardless of argument evaluation order.
static inline char* gc_log_argument_buffer() {
    static thread_local char buffers[16][32] = {};
    static thread_local unsigned int next = 0;
    return buffers[next++ % 16];
}

static inline const char* gc_log_path(const char* path) {
    return gc_log_path_token(path, gc_log_argument_buffer(), 32);
}

static inline const char* gc_log_path(const wchar_t* path) {
    char* out = gc_log_argument_buffer();
    if (!path || !path[0]) { out[0] = '-'; out[1] = 0; return out; }
    const unsigned long long hash = gc_log_fingerprint_wide(path);
    const char prefix[] = "[path #";
    for (size_t i = 0; i < sizeof(prefix) - 1; ++i) out[i] = prefix[i];
    for (int shift = 60, at = 7; shift >= 0; shift -= 4, ++at)
        out[at] = "0123456789abcdef"[(hash >> shift) & 15U];
    out[23] = ']'; out[24] = 0;
    return out;
}

static inline const char* gc_log_identifier(const wchar_t* name) {
    return gc_log_wide_identifier_token(name, gc_log_argument_buffer(), 32);
}

#endif
