// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Which files legitimately live in %ProgramData%\Green Curve, as pure logic.
//
// ## Why this is a policy and not an if-statement
//
// `service_cleanup_legacy_programdata()` sweeps that directory on every service
// start, to remove things older builds left there: restart history, the reapply
// snapshot, an early debug log, and SYSTEM-process crash dumps that were
// world-readable. It deleted **every file except shared-profiles.ini**.
//
// That is a denylist written as "everything I do not recognise", and it means
// any NEW machine-scope file is destroyed by the next service start. The
// update-manifest cache moved into that directory and was swept before it could
// ever be read -- on every restart, silently, because a missing cache is the
// documented ordinary state of a machine that has never checked. Three separate
// bugs had to be fixed before the cache could write at all, and then this
// deleted what it wrote. `update cache: restored` had never appeared in a log
// once, on any machine, at any point in the feature's life. Confirmed
// 2026-08-18.
//
// So the rule is inverted: this names what is CURRENT, the sweeper deletes the
// rest, and adding a machine-scope file means adding it here. A gate requires
// the sweeper to consult this function, because the failure mode is a file that
// silently stops existing rather than anything that breaks visibly.

#ifndef GREEN_CURVE_MACHINE_DIR_POLICY_H
#define GREEN_CURVE_MACHINE_DIR_POLICY_H

#include <stddef.h>

// The signed documents the last update check cached, re-verified on restore.
// Defined here rather than beside the cache implementation so the sweeper and
// the writer cannot disagree about the names -- a divergence between them
// reintroduces exactly the bug this header exists to prevent, and would do it
// silently.
#define GC_UPDATE_CACHE_MANIFEST_NAME "update-manifest.cache"
#define GC_UPDATE_CACHE_SIGNATURE_NAME "update-manifest.cache.sig"

// Case-insensitive ASCII compare. Windows filenames are case-insensitive, and
// a sweeper that only matched the exact case would delete `Update-Manifest.Cache`
// while preserving the one it wrote.
static inline bool gc_machine_dir_name_equals(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t i = 0;
    for (;; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        if (ca == '\0') return true;
    }
}

// True for a file that CURRENTLY belongs in %ProgramData%\Green Curve and must
// survive the legacy sweep.
//
// `configFileName` is passed in rather than included, so this header stays free
// of app_shared.h and can be unit tested on its own.
static inline bool gc_machine_dir_file_is_current(const char* name,
                                                  const char* configFileName) {
    if (!name || !name[0]) return false;
    if (configFileName && gc_machine_dir_name_equals(name, configFileName)) return true;
    if (gc_machine_dir_name_equals(name, GC_UPDATE_CACHE_MANIFEST_NAME)) return true;
    if (gc_machine_dir_name_equals(name, GC_UPDATE_CACHE_SIGNATURE_NAME)) return true;
    return false;
}

#endif  // GREEN_CURVE_MACHINE_DIR_POLICY_H
