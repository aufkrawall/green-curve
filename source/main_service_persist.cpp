// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Service restart/recovery persistence split out of main_service_runtime.cpp
// (F-MAINT-1): TDR/driver-recovery history, the persisted restart-loop history
// ring, and the restart-reapply snapshot (save/load/clear). Compiled as a shard
// included immediately before main_service_runtime.cpp (no behavior change).

// TDR loop detection: if the driver resets 3+ times within 5 minutes and
// OC is lost each time, stop re-applying to avoid a crash loop.
#define RECOVERY_HISTORY_SIZE 8
#define RECOVERY_LOOP_WINDOW_MS 300000
#define MAX_RECOVERIES_BEFORE_BACKOFF 3

static ULONGLONG g_driverRecoveryTimestamp[RECOVERY_HISTORY_SIZE] = {};
static int g_driverRecoveryHead = 0;

static unsigned int count_recent_driver_recoveries() {
    ULONGLONG now = GetTickCount64();
    unsigned int count = 0;
    for (int i = 0; i < RECOVERY_HISTORY_SIZE; i++) {
        if (g_driverRecoveryTimestamp[i] > 0 &&
            now - g_driverRecoveryTimestamp[i] <= RECOVERY_LOOP_WINDOW_MS)
            count++;
    }
    return count;
}

static void record_driver_recovery() {
    g_driverRecoveryTimestamp[g_driverRecoveryHead] = GetTickCount64();
    g_driverRecoveryHead = (g_driverRecoveryHead + 1) % RECOVERY_HISTORY_SIZE;
}

// ---- Driver-restart recovery persistence ----
//
// Recovery after a GPU device reconnect, TDR, or driver upgrade is performed by
// restarting the service PROCESS (see launch_recovery_thread / request_service_
// restart).  Before exiting, the service snapshots the active desired OC/fan
// profile to disk; the freshly relaunched process loads clean driver DLLs and
// service_startup_reapply_thread_proc() re-applies the snapshot.  A normal boot
// without this file still does not auto-apply.

#define SERVICE_ACTIVE_DESIRED_MAGIC   0x47434144u /* 'GCAD' */
#define SERVICE_ACTIVE_DESIRED_VERSION ((DWORD)SERVICE_PROTOCOL_VERSION)

static bool service_active_desired_persist_path(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    char dir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dir, sizeof(dir))) return false;
    return SUCCEEDED(StringCchPrintfA(out, outSize, "%s\\service_restart_reapply.bin", dir));
}

// ---- Restart-loop protection (persisted across process restarts) ----
//
// Recovery restarts the process, so an in-memory counter cannot detect a
// restart loop (a genuinely broken driver that crashes every fresh process).
// Persist a small ring of recent restart tick stamps next to the snapshot.
// On startup, if too many restarts happened within the window, the startup
// reapply skips re-applying (and clears the snapshot) so the service stops
// feeding the snapshot -> apply -> crash -> restart loop.  GetTickCount64 is
// system uptime: monotonic and shared across processes on the same boot, and
// it resets across an OS reboot (which legitimately clears the loop).
#define SERVICE_RESTART_HISTORY_MAX      8u
#define SERVICE_RESTART_LOOP_WINDOW_MS   300000ULL  /* 5 minutes */
#define SERVICE_RESTART_LOOP_THRESHOLD   5u
#define SERVICE_RESTART_HISTORY_MAGIC    0x47435248u /* 'GCRH' */

static bool service_restart_history_path(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    char dir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dir, sizeof(dir))) return false;
    return SUCCEEDED(StringCchPrintfA(out, outSize, "%s\\service_restart_history.bin", dir));
}

static unsigned int service_read_restart_history(ULONGLONG* outTicks, unsigned int maxTicks) {
    if (!outTicks || maxTicks == 0) return 0;
    char path[MAX_PATH] = {};
    if (!service_restart_history_path(path, sizeof(path))) return 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD magic = 0, count = 0, read = 0;
    bool ok = ReadFile(h, &magic, sizeof(magic), &read, nullptr) && read == sizeof(magic);
    ok = ok && ReadFile(h, &count, sizeof(count), &read, nullptr) && read == sizeof(count);
    if (ok && magic != SERVICE_RESTART_HISTORY_MAGIC) ok = false;
    if (ok && count > SERVICE_RESTART_HISTORY_MAX) count = SERVICE_RESTART_HISTORY_MAX;
    unsigned int got = 0;
    if (ok) {
        for (unsigned int i = 0; i < count && got < maxTicks; i++) {
            ULONGLONG t = 0;
            if (!ReadFile(h, &t, sizeof(t), &read, nullptr) || read != sizeof(t)) break;
            outTicks[got++] = t;
        }
    }
    CloseHandle(h);
    return got;
}

static unsigned int service_count_recent_restarts() {
    ULONGLONG ticks[SERVICE_RESTART_HISTORY_MAX] = {};
    unsigned int n = service_read_restart_history(ticks, SERVICE_RESTART_HISTORY_MAX);
    ULONGLONG now = GetTickCount64();
    unsigned int count = 0;
    for (unsigned int i = 0; i < n; i++) {
        // Ignore stamps from a previous boot (tick reset): only count stamps in
        // the past and within the loop window.
        if (ticks[i] != 0 && ticks[i] <= now && (now - ticks[i]) <= SERVICE_RESTART_LOOP_WINDOW_MS) {
            count++;
        }
    }
    return count;
}

static void service_record_restart_event() {
    ULONGLONG ticks[SERVICE_RESTART_HISTORY_MAX] = {};
    unsigned int n = service_read_restart_history(ticks, SERVICE_RESTART_HISTORY_MAX);
    ULONGLONG now = GetTickCount64();
    ULONGLONG kept[SERVICE_RESTART_HISTORY_MAX] = {};
    unsigned int k = 0;
    for (unsigned int i = 0; i < n; i++) {
        if (ticks[i] != 0 && ticks[i] <= now && (now - ticks[i]) <= SERVICE_RESTART_LOOP_WINDOW_MS) {
            if (k < SERVICE_RESTART_HISTORY_MAX) kept[k++] = ticks[i];
        }
    }
    if (k == SERVICE_RESTART_HISTORY_MAX) {
        for (unsigned int i = 1; i < k; i++) kept[i - 1] = kept[i];
        k = SERVICE_RESTART_HISTORY_MAX - 1;
    }
    kept[k++] = now;

    char path[MAX_PATH] = {};
    if (!service_restart_history_path(path, sizeof(path))) return;
    char pathErr[256] = {};
    ensure_parent_directory_for_file(path, pathErr, sizeof(pathErr));
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD magic = SERVICE_RESTART_HISTORY_MAGIC;
    DWORD count = k;
    DWORD written = 0;
    bool ok = WriteFile(h, &magic, sizeof(magic), &written, nullptr) && written == sizeof(magic);
    ok = ok && WriteFile(h, &count, sizeof(count), &written, nullptr) && written == sizeof(count);
    for (unsigned int i = 0; ok && i < k; i++) {
        ok = WriteFile(h, &kept[i], sizeof(kept[i]), &written, nullptr) && written == sizeof(kept[i]);
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    debug_log("restart history: recorded restart event (recent=%u within %llu ms)\n",
        k, (unsigned long long)SERVICE_RESTART_LOOP_WINDOW_MS);
}

static void service_clear_restart_history() {
    char path[MAX_PATH] = {};
    if (service_restart_history_path(path, sizeof(path))) DeleteFileA(path);
}

// Snapshot the current active desired settings to disk so the post-restart
// process can re-apply them.  Writes nothing (and removes any stale file) when
// there is no active desired — a restart with nothing applied should not
// auto-apply on the next boot.
static void service_write_restart_reapply_snapshot() {
    if (!g_app.isServiceProcess) return;
    char path[MAX_PATH] = {};
    if (!service_active_desired_persist_path(path, sizeof(path))) return;
    if (!g_serviceHasActiveDesired) {
        DeleteFileA(path);
        return;
    }
    char pathErr[256] = {};
    ensure_parent_directory_for_file(path, pathErr, sizeof(pathErr));
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        debug_log("restart reapply snapshot: CreateFile failed (error %lu)\n", GetLastError());
        return;
    }
    DWORD magic = SERVICE_ACTIVE_DESIRED_MAGIC;
    DWORD version = SERVICE_ACTIVE_DESIRED_VERSION;
    DWORD size = (DWORD)sizeof(g_serviceActiveDesired);
    DWORD written = 0;
    bool ok = WriteFile(h, &magic, sizeof(magic), &written, nullptr) && written == sizeof(magic);
    ok = ok && WriteFile(h, &version, sizeof(version), &written, nullptr) && written == sizeof(version);
    ok = ok && WriteFile(h, &size, sizeof(size), &written, nullptr) && written == sizeof(size);
    ok = ok && WriteFile(h, &g_serviceActiveDesired, size, &written, nullptr) && written == size;
    FlushFileBuffers(h);
    CloseHandle(h);
    debug_log("restart reapply snapshot: wrote %s (ok=%d)\n", path, ok ? 1 : 0);
}

static bool service_load_restart_reapply_snapshot(DesiredSettings* out) {
    if (!out) return false;
    char path[MAX_PATH] = {};
    if (!service_active_desired_persist_path(path, sizeof(path))) return false;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD magic = 0, version = 0, size = 0, read = 0;
    bool ok = ReadFile(h, &magic, sizeof(magic), &read, nullptr) && read == sizeof(magic);
    ok = ok && ReadFile(h, &version, sizeof(version), &read, nullptr) && read == sizeof(version);
    ok = ok && ReadFile(h, &size, sizeof(size), &read, nullptr) && read == sizeof(size);
    if (ok && (magic != SERVICE_ACTIVE_DESIRED_MAGIC ||
               version != SERVICE_ACTIVE_DESIRED_VERSION ||
               size != (DWORD)sizeof(*out))) {
        debug_log("restart reapply load: header mismatch magic=%08lX ver=%lu size=%lu (expected size=%lu)\n",
            (unsigned long)magic, (unsigned long)version, (unsigned long)size, (unsigned long)sizeof(*out));
        ok = false;
    }
    if (ok) ok = ReadFile(h, out, size, &read, nullptr) && read == size;
    CloseHandle(h);
    if (!ok) {
        debug_log("restart reapply load: read failed\n");
        return false;
    }
    return true;
}

static void service_clear_restart_reapply_snapshot() {
    char path[MAX_PATH] = {};
    if (service_active_desired_persist_path(path, sizeof(path))) DeleteFileA(path);
}

