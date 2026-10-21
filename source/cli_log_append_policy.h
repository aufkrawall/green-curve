// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// One bounded append to greencurve_cli_log.txt.
//
// F-01-003 turned the CLI log into an appending log so a later --help could
// not erase a --service-install record.  The silent scheduled-task paths (the
// logon handoff's success record, its FAILED record, and a command line that
// did not parse) were missed: they still REPLACED the file with an atomic
// whole-file write, so every logon erased the history again -- including the
// one record that says why the previous logon's handoff failed.
//
// This is the append those paths use.  It checks the size on the same handle
// it then writes through, truncates in place with the CLI rotation marker once
// the cap is reached (the cap the interactive CLI path enforces), and never
// replaces a file that is still under the cap.  The opener is injected so the
// app passes its UTF-8 path opener and the regression harness plain fopen.
#ifndef GREEN_CURVE_CLI_LOG_APPEND_POLICY_H
#define GREEN_CURVE_CLI_LOG_APPEND_POLICY_H

#include <stdio.h>

#include "debug_log_rotation_policy.h"
#include "log_path_redaction.h"

typedef FILE* (*GcCliLogOpenFn)(const char* path, const char* mode);

enum GcCliLogAppendResult {
    GC_CLI_LOG_APPEND_FAILED = 0,
    GC_CLI_LOG_APPENDED = 1,
    GC_CLI_LOG_APPENDED_AFTER_ROTATION = 2,
};

// The caller owns the timestamp and newline; the file copy redacts path spans.
// `rotationPrefix` (normally a timestamp) precedes the rotation marker.
static inline GcCliLogAppendResult gc_cli_log_append_record(
    const char* path, const char* record, long long capBytes,
    const char* rotationPrefix, GcCliLogOpenFn openFn) {
    if (!path || !path[0] || !record || !openFn) return GC_CLI_LOG_APPEND_FAILED;
    FILE* f = openFn(path, "a");
    if (!f) return GC_CLI_LOG_APPEND_FAILED;
    long long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = (long long)ftell(f);
    bool rotated = false;
    if (size >= 0 && gc_debug_log_rotation::should_rotate(size, capBytes)) {
        fclose(f);
        f = openFn(path, "w");
        if (!f) return GC_CLI_LOG_APPEND_FAILED;
        if (rotationPrefix) fputs(rotationPrefix, f);
        fputs(gc_debug_log_rotation::cli_marker_line(), f);
        rotated = true;
    }
    char redacted[4096] = {};
    gc_log_redact_paths(record, redacted, sizeof(redacted));
    bool ok = fputs(redacted, f) >= 0;
    ok = fflush(f) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok) return GC_CLI_LOG_APPEND_FAILED;
    return rotated ? GC_CLI_LOG_APPENDED_AFTER_ROTATION : GC_CLI_LOG_APPENDED;
}

#endif // GREEN_CURVE_CLI_LOG_APPEND_POLICY_H
