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

// Why a signed envelope was refused.  The distinction is the user's remedy:
// EXPIRED means the publisher has not renewed the metadata yet (nothing on
// this PC is wrong, and a later check recovers), FUTURE and NO_CLOCK mean this
// PC's clock is the problem, MALFORMED/BAD_LIFETIME mean the release tooling
// produced something this build will never accept.
enum GcUpdateFreshStatus {
    GC_UPDATE_FRESH_OK = 0,
    GC_UPDATE_FRESH_MALFORMED,
    GC_UPDATE_FRESH_BAD_LIFETIME,
    GC_UPDATE_FRESH_NO_CLOCK,
    GC_UPDATE_FRESH_FUTURE,
    GC_UPDATE_FRESH_EXPIRED,
};

static inline GcUpdateFreshStatus gc_update_fresh_time_status(
    const GcUpdateFreshness* fresh, long long now) {
    if (!fresh) return GC_UPDATE_FRESH_MALFORMED;
    if (fresh->issued <= 0 || fresh->expires <= fresh->issued ||
        fresh->expires - fresh->issued > GC_UPDATE_FRESH_MAX_LIFETIME)
        return GC_UPDATE_FRESH_BAD_LIFETIME;
    if (now <= 0) return GC_UPDATE_FRESH_NO_CLOCK;
    if (fresh->issued > now && fresh->issued - now > GC_UPDATE_FRESH_CLOCK_SKEW)
        return GC_UPDATE_FRESH_FUTURE;
    if (now >= fresh->expires) return GC_UPDATE_FRESH_EXPIRED;
    return GC_UPDATE_FRESH_OK;
}
static inline bool gc_update_fresh_time_valid(const GcUpdateFreshness* fresh, long long now) {
    return gc_update_fresh_time_status(fresh, now) == GC_UPDATE_FRESH_OK;
}
static inline const char* gc_update_fresh_status_text(GcUpdateFreshStatus status) {
    switch (status) {
        case GC_UPDATE_FRESH_OK:
            return "Signed update metadata is current";
        case GC_UPDATE_FRESH_EXPIRED:
            return "The published update information has expired and is waiting to be "
                   "renewed; Green Curve will check again later";
        case GC_UPDATE_FRESH_FUTURE:
            return "The published update information is dated in the future; check "
                   "this PC's date and time";
        case GC_UPDATE_FRESH_NO_CLOCK:
            return "This PC's clock could not be read; check its date and time";
        case GC_UPDATE_FRESH_BAD_LIFETIME:
        case GC_UPDATE_FRESH_MALFORMED:
        default:
            return "The published update information is not in a format this version "
                   "accepts";
    }
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
static inline GcUpdateFreshStatus gc_update_fresh_parse_status(const char* text,
    size_t size, long long now, GcUpdateFreshness* fresh) {
    if (!fresh) return GC_UPDATE_FRESH_MALFORMED;
    memset(fresh, 0, sizeof(*fresh));
    const char header[] = "freshness=1\n";
    size_t position = sizeof(header) - 1;
    if (!text || size > GC_UPDATE_FRESH_MAX_BYTES || size < position ||
        memcmp(text, header, position) ||
        !gc_update_fresh_number(text, size, &position, "issued=", &fresh->issued) ||
        !gc_update_fresh_number(text, size, &position, "expires=", &fresh->expires) ||
        position >= size || size - position > GC_UPDATE_MANIFEST_MAX_BYTES) {
        memset(fresh, 0, sizeof(*fresh));
        return GC_UPDATE_FRESH_MALFORMED;
    }
    GcUpdateFreshStatus status = gc_update_fresh_time_status(fresh, now);
    if (status != GC_UPDATE_FRESH_OK) return status;
    fresh->manifestOffset = position;
    return GC_UPDATE_FRESH_OK;
}
static inline bool gc_update_fresh_parse(const char* text, size_t size, long long now,
    GcUpdateFreshness* fresh) {
    return gc_update_fresh_parse_status(text, size, now, fresh) == GC_UPDATE_FRESH_OK;
}
