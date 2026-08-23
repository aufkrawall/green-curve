// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure size-cap policy for the greencurve_debug.txt debug logs.
//
// Both the GUI and the service append diagnostics to a file named
// greencurve_debug.txt (the service writes to the active user's file after
// its first authorized request -- see llm-wiki/debug-tools.md). Neither file
// had any bound: by 2026-08 the developer machine's user-side log had reached
// 352 MB and the service-side copy spanned months.
//
// The runtime contract this policy backs: before appending a line, the writer
// checks the current file size; once it reaches GC_DEBUG_LOG_ROTATE_BYTES the
// writer truncates IN PLACE (close -> CREATE_ALWAYS -> reopen-append) and
// writes a one-line rotation marker. Truncation rather than rename is what
// makes this safe for the shared multi-writer case (GUI + service hold
// append-only handles on the same user-side file): FILE_APPEND_DATA writes
// always land at EOF, so after a cooperative truncate every writer's next
// line continues cleanly in the fresh file.

#ifndef GREEN_CURVE_DEBUG_LOG_ROTATION_POLICY_H
#define GREEN_CURVE_DEBUG_LOG_ROTATION_POLICY_H

namespace gc_debug_log_rotation {

enum {
    // Size cap per generation. 32 MiB keeps roughly 2-4 days of steady
    // GUI-polling verbosity on disk while guaranteeing a hard bound for both
    // the user-side merged log and the SYSTEM-profile early log.
    kRotateBytes = 32 * 1024 * 1024,
};

// True when the file has reached its cap and must be truncated before the
// next append. A non-positive cap never rotates (callers pass the compiled-in
// default; the guard only exists so a future config plumbing bug cannot turn
// a zero into "always rotate").
inline bool should_rotate(long long sizeBytes, long long capBytes) {
    if (capBytes <= 0) return false;
    return sizeBytes >= capBytes;
}

inline bool should_rotate(long long sizeBytes) {
    return should_rotate(sizeBytes, (long long)kRotateBytes);
}

// First line written after a truncation so a rotated file explains itself.
inline const char* marker_line() {
    return "debug log truncated here: size cap reached\n";
}

} // namespace gc_debug_log_rotation

#endif // GREEN_CURVE_DEBUG_LOG_ROTATION_POLICY_H
