// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Regression tests for steady-state config.ini read/probe gates
// (config_steady_state_policy.h), assertion codes 14300-14331.

#include "config_steady_state_policy.h"
#if defined(_WIN32)
#include "app_shared.h"
#include <stdio.h>
#endif

#include <string.h>

int run_config_steady_state_tests() {
    // 14300: Service pipe refresh gate skips steady-state GET_TELEMETRY when
    // user paths did not change and no retry is pending, while refreshing on
    // user/session switches, pending lock-timeout retries, and every
    // non-telemetry command.
    if (gc_service_pipe_should_refresh_debug_config(
            SERVICE_CMD_GET_TELEMETRY, false, false)) {
        return 14300;
    }
    if (!gc_service_pipe_should_refresh_debug_config(
            SERVICE_CMD_GET_TELEMETRY, true, false)) {
        return 14301;
    }
    if (!gc_service_pipe_should_refresh_debug_config(
            SERVICE_CMD_GET_TELEMETRY, false, true)) {
        return 14316;
    }
    const gc_u32 nonTelemetryCommands[] = {
        SERVICE_CMD_PING,
        SERVICE_CMD_GET_SNAPSHOT,
        SERVICE_CMD_APPLY,
        SERVICE_CMD_RESET,
        SERVICE_CMD_GET_ACTIVE_DESIRED,
        SERVICE_CMD_CAPTURE_CURRENT,
        SERVICE_CMD_WRITE_LOG_SNAPSHOT,
        SERVICE_CMD_WRITE_JSON_SNAPSHOT,
        SERVICE_CMD_WRITE_PROBE_REPORT,
        SERVICE_CMD_GET_UPDATE_STATE,
    };
    for (gc_u32 cmd : nonTelemetryCommands) {
        if (!gc_service_pipe_should_refresh_debug_config(cmd, false)) {
            return 14302;
        }
    }

    // 14303: Env override short-circuit avoids disk probes when the environment
    // already settles both [debug] flags.
    if (gc_debug_config_needs_disk_probe(true, false, false)) return 14303;
    if (gc_debug_config_needs_disk_probe(true, false, true)) return 14303;
    if (gc_debug_config_needs_disk_probe(true, true, true)) return 14303;
    if (!gc_debug_config_needs_disk_probe(true, true, false)) return 14304;
    if (!gc_debug_config_needs_disk_probe(false, false, false)) return 14304;
    if (!gc_debug_config_needs_disk_probe(false, false, true)) return 14304;

    // 14305-14309: Unified [debug] stamp cache covers both enabled & verbose,
    // handles absent files without repeating misses, compares paths case-
    // insensitively on ASCII, invalidates on size/mtime/defaultEnabled change,
    // and refuses oversized paths rather than caching truncated aliases.
    GcDebugConfigCache cache = {};
    const GcConfigFileStamp absent = { false, 0, 0, 0, 0 };
    const GcConfigFileStamp v1 = { true, 0, 420, 1000, 2000 };
    const GcConfigFileStamp v2Size = { true, 0, 421, 1000, 2000 };
    const GcConfigFileStamp v2Time = { true, 0, 420, 1001, 2000 };

    if (gc_debug_config_cache_hit(&cache, "C:\\Users\\a\\config.ini", absent, true)) {
        return 14305;
    }
    gc_debug_config_cache_store(&cache, "C:\\Users\\a\\config.ini", absent, true, true, false);
    if (!gc_debug_config_cache_hit(&cache, "c:\\users\\A\\CONFIG.INI", absent, true) ||
        !cache.enabled || cache.verbose) {
        return 14306;
    }
    // Creating config.ini flips present=false -> true and misses the cache.
    if (gc_debug_config_cache_hit(&cache, "C:\\Users\\a\\config.ini", v1, true)) {
        return 14307;
    }
    gc_debug_config_cache_store(&cache, "C:\\Users\\a\\config.ini", v1, true, false, true);
    if (!gc_debug_config_cache_hit(&cache, "C:\\Users\\a\\config.ini", v1, true) ||
        cache.enabled || !cache.verbose) {
        return 14307;
    }
    // Any stamp, path, or defaultEnabled change misses the cache.
    if (gc_debug_config_cache_hit(&cache, "C:\\Users\\a\\config.ini", v2Size, true) ||
        gc_debug_config_cache_hit(&cache, "C:\\Users\\a\\config.ini", v2Time, true) ||
        gc_debug_config_cache_hit(&cache, "C:\\Users\\b\\config.ini", v1, true) ||
        gc_debug_config_cache_hit(&cache, "C:\\Users\\a\\config.ini", v1, false)) {
        return 14308;
    }
    // Oversized path invalidates rather than caching a truncated prefix.
    char hugePath[GC_DEBUG_CONFIG_CACHE_PATH_MAX + 8];
    memset(hugePath, 'x', sizeof(hugePath) - 1);
    hugePath[sizeof(hugePath) - 1] = '\0';
    gc_debug_config_cache_store(&cache, hugePath, v1, true, true, false);
    if (cache.valid || gc_debug_config_cache_hit(&cache, hugePath, v1, true)) {
        return 14309;
    }

    // 14310-14312: GUI [gpu] selection maintenance checks before disk I/O.
    // Settled telemetry never probes config.ini; unsettled / adapter change /
    // configPath change always checks; full sync re-checks only when the stamp
    // moved externally.
    if (gc_gui_gpu_selection_maintenance_should_check(true, true, false, false, false)) {
        return 14310;
    }
    if (gc_gui_gpu_selection_maintenance_should_check(true, true, false, false, true)) {
        return 14310;
    }
    if (gc_gui_gpu_selection_maintenance_should_check(true, true, false, true, false)) {
        return 14311;
    }
    if (!gc_gui_gpu_selection_maintenance_should_check(true, true, false, true, true)) {
        return 14311;
    }
    if (!gc_gui_gpu_selection_maintenance_should_check(false, true, false, false, false) ||
        !gc_gui_gpu_selection_maintenance_should_check(true, false, false, false, false) ||
        !gc_gui_gpu_selection_maintenance_should_check(true, true, true, false, false)) {
        return 14312;
    }

    // 14313-14314: GUI applied-profile sync skips disk stat on telemetry ticks
    // when every in-memory input matches, but probes disk on any in-memory
    // change (including local config writes) or explicit verifyDiskStamp=true
    // (full sync, tray menu, profile actions).
    if (gc_applied_profile_sync_should_probe_disk(false, true)) return 14313;
    if (!gc_applied_profile_sync_should_probe_disk(false, false)) return 14314;
    if (!gc_applied_profile_sync_should_probe_disk(true, true)) return 14314;
    if (!gc_applied_profile_sync_should_probe_disk(true, false)) return 14314;

    // 14315: Stamp equality ignores size/mtime fields when both are absent.
    const GcConfigFileStamp absentDirty = { false, 99, 88, 77, 66 };
    if (!gc_config_file_stamp_equal(absent, absentDirty)) return 14315;
    if (gc_config_file_stamp_equal(absent, v1)) return 14315;
    if (!gc_config_file_stamp_equal(v1, v1)) return 14315;

    // 14317-14319: Transient config lock timeout preserves current [debug]
    // opt-out (enabled=0) and verbose=1 instead of clobbering with defaults,
    // while still honoring explicit environment overrides.
    bool effEnabled = true;
    bool effVerbose = true;
    // User opted out (currentEnabled=false); lock timeout returns default
    // configEnabled=true, configVerbose=false -> must keep enabled=false!
    gc_debug_config_resolve_after_read(false, false, false,
        false, true, false, false, false, &effEnabled, &effVerbose);
    if (effEnabled || effVerbose) return 14317;
    // User enabled verbose (currentEnabled=true, currentVerbose=true); lock
    // timeout returns configVerbose=false -> must keep both true!
    gc_debug_config_resolve_after_read(false, false, false,
        false, true, false, true, true, &effEnabled, &effVerbose);
    if (!effEnabled || !effVerbose) return 14318;
    // Env override still wins even when disk read fails.
    gc_debug_config_resolve_after_read(true, false, false,
        false, true, true, true, true, &effEnabled, &effVerbose);
    if (effEnabled || effVerbose) return 14319;
    gc_debug_config_resolve_after_read(true, true, true,
        false, false, false, false, false, &effEnabled, &effVerbose);
    if (!effEnabled || !effVerbose) return 14319;
    // Once a subsequent retry succeeds, disk values apply cleanly.
    gc_debug_config_resolve_after_read(false, false, false,
        true, false, false, true, true, &effEnabled, &effVerbose);
    if (effEnabled || effVerbose) return 14319;

    // 14320-14322: Tri-state [gpu] section maintenance action never settles on
    // a transient config lock failure (single-GPU or multi-GPU).
    if (gc_gui_gpu_selection_section_action(-1, 1, true) != GC_GPU_MAINT_RETRY_LOCK ||
        gc_gui_gpu_selection_section_action(-1, 2, true) != GC_GPU_MAINT_RETRY_LOCK ||
        gc_gui_gpu_selection_section_action(-1, 0, false) != GC_GPU_MAINT_RETRY_LOCK) {
        return 14320;
    }
    if (gc_gui_gpu_selection_section_action(0, 1, true) != GC_GPU_MAINT_AUTO_SAVE_SINGLE ||
        gc_gui_gpu_selection_section_action(0, 1, false) != GC_GPU_MAINT_SETTLE_ABSENT ||
        gc_gui_gpu_selection_section_action(0, 2, true) != GC_GPU_MAINT_SETTLE_ABSENT) {
        return 14321;
    }
    if (gc_gui_gpu_selection_section_action(1, 1, true) != GC_GPU_MAINT_PROCEED_LOAD ||
        gc_gui_gpu_selection_section_action(1, 2, true) != GC_GPU_MAINT_PROCEED_LOAD) {
        return 14322;
    }

    // 14327-14328: Start-on-logon click target flips what the user saw painted
    // on screen when known (avoiding opposite toggles after external edits),
    // and falls back to !diskEnabled when not yet painted.
    if (gc_start_on_logon_click_target(true, false, false) != true ||
        gc_start_on_logon_click_target(true, true, true) != false) {
        return 14327;
    }
    if (gc_start_on_logon_click_target(true, false, true) != true ||
        gc_start_on_logon_click_target(true, true, false) != false ||
        gc_start_on_logon_click_target(false, false, true) != false ||
        gc_start_on_logon_click_target(false, true, false) != true) {
        return 14328;
    }

    // 14329: Opening the tray/Profiles menu syncs applied_slot only once the
    // service has authoritative READY state (never clobbering applied_slot=0
    // before the first READY snapshot).
    if (gc_menu_open_should_sync_applied_profile(false, false) ||
        gc_menu_open_should_sync_applied_profile(true, false) ||
        gc_menu_open_should_sync_applied_profile(false, true) ||
        !gc_menu_open_should_sync_applied_profile(true, true)) {
        return 14329;
    }

    return 0;
}

#if defined(_WIN32)
namespace {

struct ConfigMutexHoldFixture {
    HANDLE heldEvent;
    HANDLE releaseEvent;
    bool acquired;
};

static DWORD WINAPI config_mutex_hold_thread(LPVOID arg) {
    ConfigMutexHoldFixture* fx = static_cast<ConfigMutexHoldFixture*>(arg);
    HANDLE mutex = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
        "Global\\GreenCurveConfigMutex-v2");
    if (mutex) {
        DWORD wait = WaitForSingleObject(mutex, 5000);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            fx->acquired = true;
            SetEvent(fx->heldEvent);
            WaitForSingleObject(fx->releaseEvent, 5000);
            ReleaseMutex(mutex);
        } else {
            SetEvent(fx->heldEvent);
        }
        CloseHandle(mutex);
    } else {
        SetEvent(fx->heldEvent);
    }
    return 0;
}

}  // namespace

int run_config_steady_state_native_tests(const char* scratchConfigPath) {
    // 14323-14326, 14330-14331: Native Windows execution of
    // config_section_presence() and load_configured_gpu_selection(), including
    // a deterministic real Global\GreenCurveConfigMutex-v2 lock timeout.
    if (!scratchConfigPath || !scratchConfigPath[0]) return 14323;
    char iniPath[MAX_PATH] = {};
    snprintf(iniPath, sizeof(iniPath), "%s.steady_state.ini", scratchConfigPath);
    DeleteFileA(iniPath);

    if (config_section_presence(iniPath, "gpu") != CONFIG_SECTION_PRESENCE_ABSENT) {
        return 14323;
    }
    if (!set_config_int(iniPath, "debug", "enabled", 0) ||
        config_section_presence(iniPath, "gpu") != CONFIG_SECTION_PRESENCE_ABSENT ||
        config_section_presence(iniPath, "debug") != CONFIG_SECTION_PRESENCE_PRESENT) {
        DeleteFileA(iniPath);
        return 14324;
    }
    if (!set_config_int(iniPath, "gpu", "selected_index", 0) ||
        !set_config_int(iniPath, "gpu", "selected_identity_version", 99) ||
        config_section_presence(iniPath, "gpu") != CONFIG_SECTION_PRESENCE_PRESENT) {
        DeleteFileA(iniPath);
        return 14325;
    }
    ConfiguredGpuSelection loaded = {};
    char loadErr[160] = {};
    bool lockFailed = true;
    if (load_configured_gpu_selection(iniPath, &loaded, loadErr, sizeof(loadErr), &lockFailed) ||
        lockFailed) {
        DeleteFileA(iniPath);
        return 14325;
    }
    if (!set_config_int(iniPath, "gpu", "selected_identity_version", 0) ||
        !load_configured_gpu_selection(iniPath, &loaded, loadErr, sizeof(loadErr), &lockFailed) ||
        lockFailed || loaded.legacyIndex != 0) {
        DeleteFileA(iniPath);
        return 14326;
    }

    // 14330-14331: Hold Global\GreenCurveConfigMutex-v2 on a peer thread and
    // trigger a deterministic 0 ms WAIT_TIMEOUT with zero sleeps.
    ConfigMutexHoldFixture fx = {};
    fx.heldEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    fx.releaseEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE holder = (fx.heldEvent && fx.releaseEvent)
        ? CreateThread(nullptr, 0, config_mutex_hold_thread, &fx, 0, nullptr)
        : nullptr;
    int timeoutResult = 0;
    if (!holder || WaitForSingleObject(fx.heldEvent, 5000) != WAIT_OBJECT_0 || !fx.acquired) {
        timeoutResult = 14330;
    } else {
        config_storage_lock_set_test_timeout_ms(0);
        if (config_section_presence(iniPath, "gpu") != CONFIG_SECTION_PRESENCE_LOCK_FAILED ||
            config_section_has_keys(iniPath, "gpu")) {
            timeoutResult = 14330;
        } else {
            lockFailed = false;
            if (load_configured_gpu_selection(iniPath, &loaded, loadErr, sizeof(loadErr), &lockFailed) ||
                !lockFailed) {
                timeoutResult = 14331;
            }
        }
        config_storage_lock_set_test_timeout_ms(-1);
    }
    if (fx.releaseEvent) SetEvent(fx.releaseEvent);
    if (holder) {
        WaitForSingleObject(holder, 5000);
        CloseHandle(holder);
    }
    if (fx.heldEvent) CloseHandle(fx.heldEvent);
    if (fx.releaseEvent) CloseHandle(fx.releaseEvent);
    if (timeoutResult != 0) {
        DeleteFileA(iniPath);
        return timeoutResult;
    }

    // Confirm recovery once the peer thread releases the mutex.
    lockFailed = true;
    if (config_section_presence(iniPath, "gpu") != CONFIG_SECTION_PRESENCE_PRESENT ||
        !load_configured_gpu_selection(iniPath, &loaded, loadErr, sizeof(loadErr), &lockFailed) ||
        lockFailed) {
        DeleteFileA(iniPath);
        return 14331;
    }
    DeleteFileA(iniPath);
    return 0;
}
#else
int run_config_steady_state_native_tests(const char*) { return 0; }
#endif
