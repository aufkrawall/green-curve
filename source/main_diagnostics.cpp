// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

static void close_debug_log_file() {
    EnterCriticalSection(&g_debugLogLock);
    if (g_debugLogFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_debugLogFile);
        CloseHandle(g_debugLogFile);
        g_debugLogFile = INVALID_HANDLE_VALUE;
    }
    g_debugLogOpenPath[0] = 0;
    LeaveCriticalSection(&g_debugLogLock);
}

static DWORD debug_log_file_attributes() {
    return FILE_ATTRIBUTE_NORMAL | (g_app.isServiceProcess ? FILE_FLAG_WRITE_THROUGH : 0);
}

static HANDLE open_debug_log_file_locked(const char* debugPath) {
    if (!debugPath || !debugPath[0]) return INVALID_HANDLE_VALUE;
    char pathErr[256] = {};
    ensure_parent_directory_for_file(debugPath, pathErr, sizeof(pathErr));
    HANDLE h = gc_CreateFileUtf8(debugPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, debug_log_file_attributes(), nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        StringCchCopyA(g_debugLogOpenPath, ARRAY_COUNT(g_debugLogOpenPath), debugPath);
    }
    return h;
}

static const char* debug_log_path() {
    return g_debugLogPath[0] ? g_debugLogPath : APP_DEBUG_LOG_FILE;
}

static const char* service_early_debug_log_path() {
    if (g_debugEarlyLogPath[0]) return g_debugEarlyLogPath;

    char dir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dir, sizeof(dir))) {
        return APP_DEBUG_LOG_FILE; // last-resort relative path
    }
    StringCchPrintfA(g_debugEarlyLogPath, ARRAY_COUNT(g_debugEarlyLogPath),
        "%s\\%s", dir, APP_DEBUG_LOG_FILE);
    return g_debugEarlyLogPath;
}

static const char* json_snapshot_path() {
    return g_jsonPath[0] ? g_jsonPath : APP_JSON_FILE;
}

static const char* error_log_path() {
    return g_errorLogPath[0] ? g_errorLogPath : APP_LOG_FILE;
}

static const char* effective_debug_log_path() {
    if (g_app.isServiceProcess && !g_serviceUserPathsResolved) {
        return service_early_debug_log_path();
    }
    return debug_log_path();
}

static void debug_log(const char* fmt, ...) {
    if (!g_debug_logging || !fmt) return;
    char message[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfA(message, ARRAY_COUNT(message), fmt, ap);
    va_end(ap);
    char buf[1200] = {};
    int prefixLen = format_log_timestamp_prefix(buf, ARRAY_COUNT(buf));
    StringCchCatA(buf + prefixLen, ARRAY_COUNT(buf) - prefixLen, message);
    OutputDebugStringA(buf);

    EnterCriticalSection(&g_debugLogLock);
    const char* debugPath = effective_debug_log_path();

    if (!g_app.isServiceProcess && !g_debugLogPath[0]) {
        char pathErr[256] = {};
        resolve_data_paths(pathErr, sizeof(pathErr));
        debugPath = effective_debug_log_path();
    }

    if (g_debugLogFile != INVALID_HANDLE_VALUE && _stricmp(g_debugLogOpenPath, debugPath) != 0) {
        FlushFileBuffers(g_debugLogFile);
        CloseHandle(g_debugLogFile);
        g_debugLogFile = INVALID_HANDLE_VALUE;
        g_debugLogOpenPath[0] = 0;
    }

    if (g_debugLogFile == INVALID_HANDLE_VALUE) {
        g_debugLogFile = open_debug_log_file_locked(debugPath);
    }

    if (g_debugLogFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (!WriteFile(g_debugLogFile, buf, (DWORD)strlen(buf), &written, nullptr)) {
            CloseHandle(g_debugLogFile);
            g_debugLogFile = INVALID_HANDLE_VALUE;
            g_debugLogOpenPath[0] = 0;

            g_debugLogFile = open_debug_log_file_locked(debugPath);
            if (g_debugLogFile != INVALID_HANDLE_VALUE) {
                WriteFile(g_debugLogFile, buf, (DWORD)strlen(buf), &written, nullptr);
            }
        }
        if (g_app.isServiceProcess && g_debugLogFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(g_debugLogFile);
        }
    }

    LeaveCriticalSection(&g_debugLogLock);
}

static void debug_log_session_marker(const char* phase, const char* kind, const char* extra) {
    if (!g_debug_logging) return;
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    DWORD pid = GetCurrentProcessId();
    DWORD sessionId = 0;
    ProcessIdToSessionId(pid, &sessionId);
    const char* configPath = g_app.configPath[0] ? g_app.configPath : "<unset>";
    debug_log("\n===== SESSION %s =====\n", phase ? phase : "MARK");
    debug_log("time=%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu session=%lu kind=%s version=%s build=%lu protocol=%lu elevated=%d serviceProcess=%d serviceInstalled=%d serviceRunning=%d serviceAvailable=%d config=%s\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        pid,
        sessionId,
        kind ? kind : "unknown",
        APP_VERSION,
        (unsigned long)APP_BUILD_NUMBER,
        (unsigned long)SERVICE_PROTOCOL_VERSION,
        is_elevated() ? 1 : 0,
        g_app.isServiceProcess ? 1 : 0,
        g_app.backgroundServiceInstalled ? 1 : 0,
        g_app.backgroundServiceRunning ? 1 : 0,
        g_app.backgroundServiceAvailable ? 1 : 0,
        configPath);
    debug_log("debug logging is enabled by default. This log may contain GPU identifiers, config paths, and applied settings.\n");
    debug_log("Set GREEN_CURVE_DEBUG=0 or [debug] enabled=0 to disable logging.\n");
    if (extra && extra[0]) {
        debug_log("details=%s\n", extra);
    }
    debug_log("========================\n");
}

static void set_last_apply_phase(const char* phase) {
    // Per-phase apply timing (logging only, no behaviour change): log how long the
    // PREVIOUS phase took so profile-apply latency can be attributed precisely
    // (reset/settle vs each ~1s VF setControl vs correction vs fan) when measuring
    // and comparing profile-switch speedups. Single applier thread → a static tick
    // is safe. Reset to 0 on the terminal empty-phase call so a new apply starts fresh.
    static ULONGLONG s_lastPhaseTickMs = 0;
    ULONGLONG nowMs = GetTickCount64();
    if (!phase || !phase[0]) {
        g_lastApplyPhase[0] = 0;
        s_lastPhaseTickMs = 0;
        return;
    }
    unsigned long long prevPhaseMs = (s_lastPhaseTickMs != 0 && nowMs >= s_lastPhaseTickMs)
        ? (unsigned long long)(nowMs - s_lastPhaseTickMs) : 0ull;
    StringCchCopyA(g_lastApplyPhase, ARRAY_COUNT(g_lastApplyPhase), phase);
    debug_log("apply phase: %s (previous phase took +%llums)\n", g_lastApplyPhase, prevPhaseMs);
    s_lastPhaseTickMs = nowMs;
}

// Crash breadcrumbs, minidumps, the fast-fail reporter and the NVML vectored
// handler moved to main_crash_artifacts.cpp — they share one location policy
// and one set of no-lock/no-heap constraints that do not apply to the logging
// above.
static void write_error_report_log_for_user_failure(const char* summary, const char* details) {
    char logErr[256] = {};
    if (!write_error_report_log(summary, details, logErr, sizeof(logErr)) && logErr[0]) {
        debug_log("error report log failed: %s\n", logErr);
    }
}

