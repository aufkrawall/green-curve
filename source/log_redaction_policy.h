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

#endif
