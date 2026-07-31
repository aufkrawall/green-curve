// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// crash_artifact_policy.h — pure, platform-neutral rules for where crash
// artifacts go and which of them may be deleted.
//
// Why this is a header and not inline logic in main_diagnostics.cpp /
// linux_crash_report.cpp: every rule here decides something that is either
// security-relevant (never write a SYSTEM dump somewhere user-readable, never
// delete a file we did not write) or impossible to observe after the fact (a
// dump that landed in the wrong directory is a dump nobody finds).  Keeping the
// decisions pure means the regression harness asserts them on either host,
// without a crash, a GPU, or a filesystem.
//
// Two invariants drive everything below:
//
//   1. FAIL CLOSED ON LOCATION.  A crash artifact goes next to config.ini (or,
//      for a machine-scope process, into the machine data directory) or it does
//      not get written at all.  It must never fall back to the current working
//      directory: for a shell-launched GUI that is wherever the user happened to
//      be, for a service it is %SystemRoot%\System32, and for an installed build
//      it can be a read-only or admin-owned program directory.  Windows already
//      refuses the analogous fallback for config.ini itself
//      (set_default_config_path) — this applies the same rule to dumps.
//
//   2. ROTATION NEVER TOUCHES A FOREIGN FILE.  Only names this project itself
//      formats are rotation candidates, and ordering compares the embedded
//      TIMESTAMP rather than the whole filename.  A plain lexicographic sweep
//      across the two prefixes would be wrong in a way that hides the newest
//      evidence: "greencurve_crash_" < "greencurve_veh_" for every date, so a
//      fresh crash dump would always be deleted before a months-old VEH dump.

#ifndef GREEN_CURVE_CRASH_ARTIFACT_POLICY_H
#define GREEN_CURVE_CRASH_ARTIFACT_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "platform.h"

// ---------------------------------------------------------------------------
// Artifact names
// ---------------------------------------------------------------------------

// Terminal crashes (unhandled exception filter, fast-fail, stack smash).
#define GC_CRASH_DUMP_PREFIX      "greencurve_crash_"
// Survivable NVML/NvAPI access violations handled by the Windows VEH.  Separate
// prefix because these are *recovered* crashes: a driver upgrade can emit a run
// of them while every one of them is expected and none is fatal.
#define GC_VEH_DUMP_PREFIX        "greencurve_veh_"
#define GC_CRASH_DUMP_SUFFIX      ".dmp"
// Linux writes a self-captured text report instead of a minidump; the kernel
// core (core_pattern / systemd-coredump) stays the binary artifact.
#define GC_CRASH_REPORT_SUFFIX    ".txt"
// Append-only one-line-per-crash breadcrumb next to the dumps.
#define GC_CRASH_BREADCRUMB_NAME  "greencurve_crash.txt"

// The timestamp both dump prefixes embed: YYYYMMDD_HHMMSS_mmm (19 chars).
// Fixed width is what makes a plain byte comparison a correct time ordering.
#define GC_CRASH_STAMP_LENGTH     19

// How many artifacts of EACH kind survive a rotation pass.  Ten is enough to
// cover a full driver-upgrade recovery loop (each iteration writes one) while
// bounding the worst case to a few hundred MB of bounded-type minidumps.
#define GC_CRASH_ARTIFACT_MAX_KEEP 10

// Hard cap for the append-only breadcrumb file.  It is one short line per
// crash, so this only ever trips on a sustained restart loop; when it does, the
// file is truncated rather than rotated, because the dumps beside it already
// carry the detail and a second generation would just double the disk cost.
// Suffixed: the comparison below is against a file size in unsigned long long,
// and an int-typed product would be widened only after overflowing if this cap
// were ever raised past 2 GiB.
#define GC_CRASH_BREADCRUMB_MAX_BYTES (1024ULL * 1024ULL)

// ---------------------------------------------------------------------------
// Where a crash artifact is allowed to go
// ---------------------------------------------------------------------------

typedef enum GcCrashDirSource {
    // No writable location could be established — write nothing.  This is a
    // real outcome, not an error path to paper over: losing one dump is much
    // cheaper than dropping a SYSTEM-process dump into a user-readable or
    // unpredictable directory.
    GC_CRASH_DIR_NONE = 0,
    // Machine scope (Windows service/helper, Linux daemon): the admin-only
    // machine data directory, NOT the user's config directory.
    GC_CRASH_DIR_MACHINE = 1,
    // User scope (GUI, CLI, TUI): the directory holding this user's config.ini.
    GC_CRASH_DIR_USER_CONFIG = 2,
    // User scope, but the cached config directory was never resolved; the
    // caller must re-derive it from the environment WITHOUT COM/known-folder
    // calls, because this decision is reached from a crash handler.
    GC_CRASH_DIR_USER_ENV = 3,
} GcCrashDirSource;

// Decide which directory a crash artifact belongs in.
//
// `machineScope` is true for the Windows service/helper and the Linux daemon.
// `resolvedUserDir` is the already-cached user data directory ("" when it was
// never resolved).  `machineDirResolvable` reports whether the machine
// directory could be derived (environment only — no COM, no allocation).
//
// Note the asymmetry: a machine-scope process that cannot resolve its machine
// directory returns NONE rather than borrowing the user directory.  Writing a
// LocalSystem dump into a user-readable path is the disclosure this whole
// policy exists to prevent, so a missing directory must lose the dump instead.
static inline GcCrashDirSource gc_crash_dir_source(bool machineScope,
                                                   const char* resolvedUserDir,
                                                   bool machineDirResolvable) {
    if (machineScope) {
        return machineDirResolvable ? GC_CRASH_DIR_MACHINE : GC_CRASH_DIR_NONE;
    }
    if (resolvedUserDir && resolvedUserDir[0]) return GC_CRASH_DIR_USER_CONFIG;
    return GC_CRASH_DIR_USER_ENV;
}

// True when a resolved directory string may actually be written to.  The empty
// string and a bare "." are both rejected: "." is the current working
// directory, which is exactly the fallback invariant 1 forbids.
static inline bool gc_crash_dir_is_acceptable(const char* dir) {
    if (!dir || !dir[0]) return false;
    if (dir[0] == '.' && dir[1] == 0) return false;
    if (dir[0] == '.' && (dir[1] == '/' || dir[1] == '\\') && dir[2] == 0) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Linux artifact directory
//
// Mirrors linux_debug_log_resolve_path() deliberately: the crash report has to
// land where the person filing the bug report already knows to look, and that
// is the directory holding config.ini — which, on Linux, defaults to the
// binary's own folder (default_linux_config_path).  The daemon is the one role
// that cannot use it: systemd mounts /usr read-only for the unit
// (ProtectSystem=full), so it uses the same StateDirectory the daemon log does.
// ---------------------------------------------------------------------------

static inline bool gc_linux_crash_dir(const char* configPath,
                                      bool daemonRole,
                                      const char* stateDir,
                                      char* dst, size_t dstSize) {
    if (!dst || dstSize == 0) return false;
    dst[0] = 0;
    if (daemonRole) {
        if (!gc_crash_dir_is_acceptable(stateDir)) return false;
        if (strlen(stateDir) >= dstSize) return false;
        gc_strlcpy(dst, dstSize, stateDir);
        return true;
    }
    const char* lastSlash = configPath ? strrchr(configPath, '/') : nullptr;
    // A config path with no directory component means the caller could not read
    // /proc/self/exe and fell back to a bare "config.ini" — i.e. the CWD.  That
    // is precisely the fallback invariant 1 forbids, so refuse it.
    if (!configPath || !configPath[0] || !lastSlash) return false;
    if (lastSlash == configPath) {
        gc_strlcpy(dst, dstSize, "/");
        return true;
    }
    size_t length = (size_t)(lastSlash - configPath);
    if (length >= dstSize) return false;
    memcpy(dst, configPath, length);
    dst[length] = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Rotation
// ---------------------------------------------------------------------------

// The prefix `name` was written with, or nullptr when this project did not
// write it.  Rotation deletes nothing it cannot attribute here.
static inline const char* gc_crash_artifact_prefix(const char* name) {
    if (!name) return nullptr;
    if (strncmp(name, GC_CRASH_DUMP_PREFIX, sizeof(GC_CRASH_DUMP_PREFIX) - 1) == 0) {
        return GC_CRASH_DUMP_PREFIX;
    }
    if (strncmp(name, GC_VEH_DUMP_PREFIX, sizeof(GC_VEH_DUMP_PREFIX) - 1) == 0) {
        return GC_VEH_DUMP_PREFIX;
    }
    return nullptr;
}

// Copy the embedded YYYYMMDD_HHMMSS_mmm stamp out of `name`.
//
// `stampOut` must hold GC_CRASH_STAMP_LENGTH + 1 bytes.  Returns false for any
// name that is not ours or whose stamp is not exactly the expected shape — a
// truncated or hand-renamed file is not a rotation candidate, because deleting
// on a guessed ordering is worse than keeping one file too many.
static inline bool gc_crash_artifact_stamp(const char* name, char* stampOut) {
    if (!stampOut) return false;
    stampOut[0] = 0;
    const char* prefix = gc_crash_artifact_prefix(name);
    if (!prefix) return false;
    const char* stamp = name + strlen(prefix);
    // Explicit length precondition.  The scan below is in-bounds without it —
    // a NUL fails every character class it tests, so a short name stops the
    // loop at the terminator — but that safety is a side effect of the
    // validation rather than something the code states, and it would quietly
    // stop holding if a class were ever widened (a `c != '_'` check, say).
    // Stating the precondition also makes the post-loop stamp[19] read
    // obviously in bounds instead of provably so.
    if (strlen(stamp) < (size_t)GC_CRASH_STAMP_LENGTH + 1) return false;
    for (size_t i = 0; i < (size_t)GC_CRASH_STAMP_LENGTH; i++) {
        char c = stamp[i];
        // Positions 8, 15 and 18 are the '_' separators of YYYYMMDD_HHMMSS_mmm.
        bool separator = (i == 8 || i == 15);
        if (separator) {
            if (c != '_') return false;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    if (stamp[GC_CRASH_STAMP_LENGTH] != '_') return false; // "_pid<N>" follows
    memcpy(stampOut, stamp, (size_t)GC_CRASH_STAMP_LENGTH);
    stampOut[GC_CRASH_STAMP_LENGTH] = 0;
    return true;
}

// True when `candidate` is strictly older than `incumbent`.
//
// Both are full filenames.  Comparing the stamps rather than the names is the
// whole point: GC_CRASH_DUMP_PREFIX sorts before GC_VEH_DUMP_PREFIX for every
// possible date, so a whole-name comparison would consistently delete the
// newest terminal crash dump and keep stale VEH dumps forever.
static inline bool gc_crash_artifact_is_older(const char* candidate,
                                              const char* incumbent) {
    char candidateStamp[GC_CRASH_STAMP_LENGTH + 1] = {};
    char incumbentStamp[GC_CRASH_STAMP_LENGTH + 1] = {};
    if (!gc_crash_artifact_stamp(candidate, candidateStamp)) return false;
    if (!gc_crash_artifact_stamp(incumbent, incumbentStamp)) return true;
    int order = strcmp(candidateStamp, incumbentStamp);
    if (order != 0) return order < 0;
    // Same millisecond (two processes crashing together): fall back to the full
    // name so the choice is total and the sweep cannot stall on a tie.
    return strcmp(candidate, incumbent) < 0;
}

// Whether a directory holding `count` artifacts of one kind needs a deletion.
static inline bool gc_crash_rotation_needed(unsigned int count,
                                            unsigned int maxKeep) {
    return count > maxKeep;
}

// Whether an append-only breadcrumb of `bytes` must be truncated before the
// next append.
static inline bool gc_crash_breadcrumb_needs_reset(unsigned long long bytes) {
    return bytes >= (unsigned long long)GC_CRASH_BREADCRUMB_MAX_BYTES;
}

#endif // GREEN_CURVE_CRASH_ARTIFACT_POLICY_H
