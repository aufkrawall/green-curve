// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The service's record of the Green Curve build that last started on this
// machine, as pure logic.
//
// A successful setup run leaves no log of its own, and every process start
// only prints its own version, so "when did this machine change builds?" used
// to mean comparing session headers by eye across a rotated log. Setup
// restarts the service as its last step, so the service compares itself with
// this record at startup and says so once when the build changed. Diagnostics
// only: nothing decides on the record, and a damaged one is simply replaced.
//
// The record is one ASCII line, "version=<version> build=<build>\n".

#ifndef GREEN_CURVE_RUN_VERSION_RECORD_POLICY_H
#define GREEN_CURVE_RUN_VERSION_RECORD_POLICY_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define GC_RUN_VERSION_RECORD_NAME "service_last_version.txt"
// Longest record accepted; a longer file is unreadable, not truncated.
#define GC_RUN_VERSION_RECORD_MAX 64u
#define GC_RUN_VERSION_TEXT_MAX 32u

enum GcRunVersionChange {
    GC_RUN_VERSION_FIRST_START = 0,  // no record: first start, or it was removed
    GC_RUN_VERSION_UNREADABLE,       // a record exists but is not one
    GC_RUN_VERSION_UNCHANGED,
    GC_RUN_VERSION_CHANGED,          // install, update or downgrade
};

struct GcRunVersion {
    char version[GC_RUN_VERSION_TEXT_MAX];
    unsigned long build;
};

static inline bool gc_run_version_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '.' || c == '-' || c == '+' || c == '_';
}

static inline bool gc_run_version_valid_text(const char* version) {
    if (!version || !version[0]) return false;
    size_t length = 0;
    for (; version[length]; ++length) {
        if (length + 1 >= GC_RUN_VERSION_TEXT_MAX || !gc_run_version_char(version[length])) return false;
    }
    return true;
}

// All-or-nothing: false and an empty `out` when the version is not a plain
// token or the record does not fit.
static inline bool gc_run_version_format(const char* version, unsigned long build,
                                         char* out, size_t outSize) {
    if (!out || !outSize) return false;
    out[0] = 0;
    if (!gc_run_version_valid_text(version)) return false;
    const int written = snprintf(out, outSize, "version=%s build=%lu\n", version, build);
    if (written <= 0 || (size_t)written >= outSize || (size_t)written > GC_RUN_VERSION_RECORD_MAX) {
        out[0] = 0;
        return false;
    }
    return true;
}

// Strict: exactly one record line (LF or CRLF, or none), nothing after it, a
// build that fits 32 bits.
static inline bool gc_run_version_parse(const char* text, size_t length, GcRunVersion* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!text || length > GC_RUN_VERSION_RECORD_MAX) return false;
    static const char kVersion[] = "version=";
    static const char kBuild[] = " build=";
    size_t at = sizeof(kVersion) - 1;
    if (length < at || memcmp(text, kVersion, at) != 0) return false;
    size_t versionLength = 0;
    while (at < length && gc_run_version_char(text[at])) {
        if (versionLength + 1 >= GC_RUN_VERSION_TEXT_MAX) {
            memset(out, 0, sizeof(*out));
            return false;
        }
        out->version[versionLength++] = text[at++];
    }
    if (!versionLength || length - at < sizeof(kBuild) - 1 ||
        memcmp(text + at, kBuild, sizeof(kBuild) - 1) != 0) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    at += sizeof(kBuild) - 1;
    size_t digits = 0;
    unsigned long long build = 0;
    while (at < length && text[at] >= '0' && text[at] <= '9' && digits < 10) {
        build = build * 10u + (unsigned long long)(text[at++] - '0');
        ++digits;
    }
    if (at < length && text[at] == '\r') ++at;
    if (at < length && text[at] == '\n') ++at;
    if (!digits || at != length || build > 0xFFFFFFFFull) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->build = (unsigned long)build;
    return true;
}

// `recordPresent`: a record file exists (or could not be opened for a reason
// other than its absence); `text`/`length` are what was read from it.
static inline GcRunVersionChange gc_run_version_compare(bool recordPresent, const char* text,
                                                        size_t length, const char* currentVersion,
                                                        unsigned long currentBuild,
                                                        GcRunVersion* previousOut) {
    GcRunVersion previous = {};
    GcRunVersionChange change = GC_RUN_VERSION_FIRST_START;
    if (recordPresent) {
        if (!gc_run_version_parse(text, length, &previous)) {
            change = GC_RUN_VERSION_UNREADABLE;
        } else {
            change = currentVersion && strcmp(previous.version, currentVersion) == 0 &&
                             previous.build == currentBuild
                ? GC_RUN_VERSION_UNCHANGED
                : GC_RUN_VERSION_CHANGED;
        }
    }
    if (previousOut) *previousOut = previous;
    return change;
}

#endif  // GREEN_CURVE_RUN_VERSION_RECORD_POLICY_H
