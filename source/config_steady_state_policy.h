// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure steady-state config.ini read/probe gates shared by the Windows service
// and GUI.
//
// Routine 1-3 s GPU telemetry (SERVICE_CMD_GET_TELEMETRY and its GUI snapshot
// adoption) observes live sensors and must not lock the cross-session config
// mutex, parse INI sections, or stat config.ini on every tick:
//
//   1. Service pipe dispatch refreshes [debug] logging on startup, on user/
//      session path switches, and on every non-telemetry command (including
//      GET_SNAPSHOT from window activation / Refresh / CLI and all mutations),
//      never on steady-state SERVICE_CMD_GET_TELEMETRY when user paths did not
//      change.
//   2. Both [debug] enabled and [debug] verbose share one file-stamp cache
//      (handling both present and absent config.ini and env-override short-
//      circuits) so non-telemetry commands take zero INI reads when unchanged.
//   3. GUI [gpu] selection maintenance checks its settled adapter list BEFORE
//      touching config.ini, re-checking on steady-state telemetry only while
//      unsettled (first run, adapter change, or failed auto-save retry), and
//      on full syncs only if config.ini's stamp moved.
//   4. GUI applied-profile metadata sync skips GetFileAttributesExW on
//      steady-state telemetry when every in-memory input (including the
//      process-local config write generation) is unchanged; full syncs, tray
//      menu opens, and explicit profile actions still verify the on-disk stamp
//      so external edits remain visible.

#ifndef GREEN_CURVE_CONFIG_STEADY_STATE_POLICY_H
#define GREEN_CURVE_CONFIG_STEADY_STATE_POLICY_H

#include "gpu_core.h"
#include "service_protocol.h"

#include <stddef.h>
#include <string.h>

struct GcConfigFileStamp {
    bool present;
    gc_u32 sizeHigh;
    gc_u32 sizeLow;
    gc_u32 lastWriteLow;
    gc_u32 lastWriteHigh;
};

static inline bool gc_config_file_stamp_equal(const GcConfigFileStamp& a,
    const GcConfigFileStamp& b) {
    if (a.present != b.present) return false;
    if (!a.present) return true;
    return a.sizeHigh == b.sizeHigh &&
        a.sizeLow == b.sizeLow &&
        a.lastWriteLow == b.lastWriteLow &&
        a.lastWriteHigh == b.lastWriteHigh;
}

static inline char gc_config_ascii_tolower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static inline bool gc_config_path_equal_ascii_ci(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (gc_config_ascii_tolower(*a) != gc_config_ascii_tolower(*b)) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// Service pipe gate: steady-state telemetry never touches config.ini once the
// caller's user paths are already resolved and no prior refresh failed to
// acquire the config lock.  Any path/session change, pending lock-timeout
// retry, or non-telemetry request (GET_SNAPSHOT on focus/refresh/startup,
// APPLY, RESET, CLI verbs, update commands) refreshes through the stamp cache.
static inline bool gc_service_pipe_should_refresh_debug_config(
    gc_u32 command, bool userPathsChanged, bool refreshPending = false) {
    return userPathsChanged || refreshPending ||
        command != SERVICE_CMD_GET_TELEMETRY;
}

// Resolve effective [debug] enabled/verbose after a cached or disk read.
// A transient config lock timeout (readSucceeded == false) must NEVER clobber
// an active user opt-out (enabled=0) or verbose=1 with defaults: keep the
// current live flags while still honoring explicit environment overrides.
static inline void gc_debug_config_resolve_after_read(
    bool haveEnvEnabled, bool envEnabled, bool envVerbose,
    bool readSucceeded, bool configEnabled, bool configVerbose,
    bool currentEnabled, bool currentVerbose,
    bool* enabledOut, bool* verboseOut) {
    const bool effectiveEnabled = haveEnvEnabled
        ? envEnabled
        : (readSucceeded ? configEnabled : currentEnabled);
    const bool effectiveVerbose = effectiveEnabled &&
        (envVerbose || (readSucceeded ? configVerbose : currentVerbose));
    if (enabledOut) *enabledOut = effectiveEnabled;
    if (verboseOut) *verboseOut = effectiveVerbose;
}

// Environment overrides can settle both [debug] flags without touching disk:
//   - GREEN_CURVE_DEBUG=0 disables logging (and verbose never outlives logging);
//   - GREEN_CURVE_DEBUG=verbose forces both logging and verbose on.
static inline bool gc_debug_config_needs_disk_probe(
    bool haveEnvEnabled, bool envEnabled, bool envVerbose) {
    if (haveEnvEnabled && !envEnabled) return false;
    if (haveEnvEnabled && envEnabled && envVerbose) return false;
    return true;
}

enum { GC_DEBUG_CONFIG_CACHE_PATH_MAX = 512 };

struct GcDebugConfigCache {
    bool valid;
    char path[GC_DEBUG_CONFIG_CACHE_PATH_MAX];
    GcConfigFileStamp stamp;
    bool defaultEnabled;
    bool enabled;
    bool verbose;
};

static inline void gc_debug_config_cache_invalidate(GcDebugConfigCache* cache) {
    if (!cache) return;
    cache->valid = false;
    cache->path[0] = '\0';
    memset(&cache->stamp, 0, sizeof(cache->stamp));
}

static inline bool gc_debug_config_cache_hit(
    const GcDebugConfigCache* cache, const char* path,
    const GcConfigFileStamp& stamp, bool defaultEnabled) {
    if (!cache || !cache->valid || !path || !path[0]) return false;
    if (cache->defaultEnabled != defaultEnabled) return false;
    if (!gc_config_path_equal_ascii_ci(cache->path, path)) return false;
    return gc_config_file_stamp_equal(cache->stamp, stamp);
}

static inline void gc_debug_config_cache_store(
    GcDebugConfigCache* cache, const char* path,
    const GcConfigFileStamp& stamp, bool defaultEnabled,
    bool enabled, bool verbose) {
    if (!cache) return;
    if (!path || !path[0]) {
        gc_debug_config_cache_invalidate(cache);
        return;
    }
    const size_t len = strlen(path);
    if (len >= GC_DEBUG_CONFIG_CACHE_PATH_MAX) {
        // Never cache a truncated path: two distinct paths could alias.
        gc_debug_config_cache_invalidate(cache);
        return;
    }
    memcpy(cache->path, path, len + 1);
    cache->stamp = stamp;
    cache->defaultEnabled = defaultEnabled;
    cache->enabled = enabled;
    cache->verbose = verbose;
    cache->valid = true;
}

// GUI [gpu] selection maintenance gate: once an adapter set has settled for
// this config path, routine telemetry does zero disk I/O.  A full sync only
// re-checks if config.ini's stamp changed externally (e.g. file deleted or
// replaced while the GUI stayed open).
static inline bool gc_gui_gpu_selection_maintenance_should_check(
    bool settledValid, bool sameConfigPath, bool adaptersChanged,
    bool fromFullSync, bool stampChanged) {
    if (!settledValid || !sameConfigPath || adaptersChanged) return true;
    return fromFullSync && stampChanged;
}

// Tri-state dispatch for the [gpu] section probe during maintenance.
// `sectionPresence` uses ConfigSectionPresence semantics (<0 lock failed,
// 0 genuinely absent, >0 present).  A lock failure MUST NOT settle the adapter
// set: otherwise a transient contention blip skips refreshing moved GPUs.
enum GcGpuSectionMaintenanceAction {
    GC_GPU_MAINT_RETRY_LOCK = 0,
    GC_GPU_MAINT_AUTO_SAVE_SINGLE = 1,
    GC_GPU_MAINT_SETTLE_ABSENT = 2,
    GC_GPU_MAINT_PROCEED_LOAD = 3,
};

static inline GcGpuSectionMaintenanceAction gc_gui_gpu_selection_section_action(
    int sectionPresence, unsigned int adapterCount, bool adapter0Valid) {
    if (sectionPresence < 0) return GC_GPU_MAINT_RETRY_LOCK;
    if (sectionPresence > 0) return GC_GPU_MAINT_PROCEED_LOAD;
    if (adapterCount == 1 && adapter0Valid) return GC_GPU_MAINT_AUTO_SAVE_SINGLE;
    return GC_GPU_MAINT_SETTLE_ABSENT;
}

// GUI applied-profile metadata sync gate: routine telemetry skips
// GetFileAttributesExW whenever every in-memory input (service identity,
// active intent, VF topology, config path, and local config write generation)
// matches the cached evaluation.  Full syncs, tray/Profiles menus, and explicit
// profile actions pass verifyDiskStamp=true to detect external config.ini edits.
static inline bool gc_applied_profile_sync_should_probe_disk(
    bool verifyDiskStamp, bool inMemoryUnchanged) {
    return verifyDiskStamp || !inMemoryUnchanged;
}

#endif
