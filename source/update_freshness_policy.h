// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <string.h>
#include "update_manifest_policy.h"

// Additive signed envelope: fixed header followed by the unchanged v1 manifest.
// No unsigned fallback. A channel attacker cannot renew an expired signature.
#define GC_UPDATE_FRESH_MANIFEST_ASSET "greencurve-update-v2.txt"
#define GC_UPDATE_FRESH_SIGNATURE_ASSET "greencurve-update-v2.sig"
enum { GC_UPDATE_FRESH_MAX_BYTES = GC_UPDATE_MANIFEST_MAX_BYTES + 128,
       GC_UPDATE_FRESH_MAX_LIFETIME = 30 * 24 * 60 * 60,
       GC_UPDATE_FRESH_CLOCK_SKEW = 300 };
struct GcUpdateFreshness {
    long long issued;
    long long expires;
    size_t manifestOffset;
};
static inline bool gc_update_fresh_time_valid(const GcUpdateFreshness* fresh, long long now) {
    return fresh && fresh->issued > 0 && fresh->expires > fresh->issued &&
        fresh->expires - fresh->issued <= GC_UPDATE_FRESH_MAX_LIFETIME &&
        now > 0 && (fresh->issued <= now || fresh->issued - now <= GC_UPDATE_FRESH_CLOCK_SKEW) &&
        now < fresh->expires;
}
static inline bool gc_update_fresh_number(const char* text, size_t size,
    size_t* position, const char* prefix, long long* value) {
    size_t length = strlen(prefix);
    if (*position > size || length > size - *position ||
        memcmp(text + *position, prefix, length)) return false;
    *position += length;
    *value = 0;
    size_t start = *position;
    while (*position < size && text[*position] >= '0' && text[*position] <= '9') {
        if (*position - start >= 12) return false; // bound before multiplication
        *value = *value * 10 + text[(*position)++] - '0';
    }
    if (*position == start || *position >= size || text[(*position)++] != '\n') return false;
    return true;
}
// Called only AFTER signature verification, both on fetch and cached restore.
static inline bool gc_update_fresh_parse(const char* text, size_t size, long long now,
    GcUpdateFreshness* fresh) {
    if (!fresh) return false;
    memset(fresh, 0, sizeof(*fresh));
    const char header[] = "freshness=1\n";
    size_t position = sizeof(header) - 1;
    if (!text || size > GC_UPDATE_FRESH_MAX_BYTES || size < position ||
        memcmp(text, header, position) ||
        !gc_update_fresh_number(text, size, &position, "issued=", &fresh->issued) ||
        !gc_update_fresh_number(text, size, &position, "expires=", &fresh->expires) ||
        position >= size || size - position > GC_UPDATE_MANIFEST_MAX_BYTES ||
        !gc_update_fresh_time_valid(fresh, now)) return false;
    fresh->manifestOffset = position;
    return true;
}
