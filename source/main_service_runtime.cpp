static void service_capture_owner_identity(const char* user, DWORD sessionId) {
    g_app.backgroundServiceOwnerUser[0] = 0;
    if (user && user[0]) {
        StringCchCopyA(g_app.backgroundServiceOwnerUser, ARRAY_COUNT(g_app.backgroundServiceOwnerUser), user);
    }
    g_app.backgroundServiceOwnerSessionId = sessionId;
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli = {};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    g_app.backgroundServiceOwnerUtcMs = uli.QuadPart / 10000ULL;
}

static void ensure_service_runtime_lock() {
    if (g_serviceRuntimeLock) return;
    g_serviceRuntimeLock = CreateMutexA(nullptr, FALSE, nullptr);
}

static void lock_service_runtime() {
    ensure_service_runtime_lock();
    if (g_serviceRuntimeLock) {
        DWORD waitResult = WaitForSingleObject(g_serviceRuntimeLock, INFINITE);
        if (waitResult == WAIT_ABANDONED) {
            debug_log("warning: service runtime lock was abandoned by previous owner\n");
        }
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
            DWORD currentThreadId = GetCurrentThreadId();
            if (g_serviceRuntimeLockOwnerThreadId == currentThreadId) {
                g_serviceRuntimeLockDepth++;
            } else {
                g_serviceRuntimeLockOwnerThreadId = currentThreadId;
                g_serviceRuntimeLockDepth = 1;
            }
        }
    }
}

static void unlock_service_runtime() {
    if (g_serviceRuntimeLock) {
        DWORD currentThreadId = GetCurrentThreadId();
        if (g_serviceRuntimeLockOwnerThreadId == currentThreadId && g_serviceRuntimeLockDepth > 0) {
            g_serviceRuntimeLockDepth--;
            if (g_serviceRuntimeLockDepth == 0) {
                g_serviceRuntimeLockOwnerThreadId = 0;
            }
            ReleaseMutex(g_serviceRuntimeLock);
        }
    }
}

static bool service_runtime_lock_held_by_current_thread() {
    return g_serviceRuntimeLockOwnerThreadId == GetCurrentThreadId() && g_serviceRuntimeLockDepth > 0;
}

static bool get_active_interactive_session_id(DWORD* sessionIdOut) {
    if (sessionIdOut) *sessionIdOut = (DWORD)-1;

    DWORD consoleSessionId = WTSGetActiveConsoleSessionId();
    if (consoleSessionId != 0xFFFFFFFF) {
        if (sessionIdOut) *sessionIdOut = consoleSessionId;
        return true;
    }

    PWTS_SESSION_INFOA sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsA(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        return false;
    }

    bool found = false;
    for (DWORD i = 0; i < count; i++) {
        if (sessions[i].State == WTSActive) {
            if (sessionIdOut) *sessionIdOut = sessions[i].SessionId;
            found = true;
            break;
        }
    }
    WTSFreeMemory(sessions);
    return found;
}

static bool get_token_sam_name(HANDLE token, char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    if (!token) return false;

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) return false;

    TOKEN_USER* user = (TOKEN_USER*)malloc(needed);
    if (!user) return false;

    DWORD actualNeeded = needed;
    if (!GetTokenInformation(token, TokenUser, user, needed, &actualNeeded)) {
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && actualNeeded > needed) {
            free(user);
            user = (TOKEN_USER*)malloc(actualNeeded);
            if (!user) return false;
            if (!GetTokenInformation(token, TokenUser, user, actualNeeded, &actualNeeded)) {
                free(user);
                return false;
            }
        } else {
            free(user);
            return false;
        }
    }

    WCHAR name[256] = {};
    WCHAR domain[256] = {};
    DWORD nameLen = ARRAY_COUNT(name);
    DWORD domainLen = ARRAY_COUNT(domain);
    SID_NAME_USE use = SidTypeUnknown;
    bool ok = false;
    if (LookupAccountSidW(nullptr, user->User.Sid, name, &nameLen, domain, &domainLen, &use)) {
        WCHAR sam[512] = {};
        if (domain[0]) ok = SUCCEEDED(StringCchPrintfW(sam, ARRAY_COUNT(sam), L"%ls\\%ls", domain, name));
        else ok = SUCCEEDED(StringCchCopyW(sam, ARRAY_COUNT(sam), name));
        if (ok) ok = copy_wide_to_ansi(sam, out, (int)outSize);
    }

    free(user);
    return ok;
}

static bool get_pipe_client_identity(HANDLE pipe, char* userOut, size_t userOutSize, DWORD* sessionIdOut, DWORD* pidOut, char* err, size_t errSize) {
    if (userOut && userOutSize > 0) userOut[0] = 0;
    if (sessionIdOut) *sessionIdOut = (DWORD)-1;
    if (pidOut) *pidOut = 0;
    if (!pipe) {
        set_message(err, errSize, "Invalid service pipe handle");
        return false;
    }

    ULONG clientPid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &clientPid) || clientPid == 0) {
        set_message(err, errSize, "Failed determining service client process");
        return false;
    }
    if (pidOut) *pidOut = (DWORD)clientPid;

    DWORD sessionId = 0;
    if (!ProcessIdToSessionId((DWORD)clientPid, &sessionId)) {
        set_message(err, errSize, "Failed determining service client session");
        return false;
    }
    if (sessionIdOut) *sessionIdOut = sessionId;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)clientPid);
    if (!process) {
        set_message(err, errSize, "Failed opening service client process");
        return false;
    }

    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        CloseHandle(process);
        set_message(err, errSize, "Failed opening service client token");
        return false;
    }

    bool ok = get_token_sam_name(token, userOut, userOutSize);
    CloseHandle(token);
    CloseHandle(process);
    if (!ok) {
        set_message(err, errSize, "Failed resolving service client identity");
        return false;
    }
    return true;
}

static bool service_caller_is_authorized(HANDLE pipe, const char* source, char* err, size_t errSize, char* callerUserOut, size_t callerUserOutSize, DWORD* callerSessionIdOut, DWORD* callerPidOut) {
    DWORD callerSessionId = (DWORD)-1;
    DWORD callerPid = 0;
    char callerUser[256] = {};
    if (!get_pipe_client_identity(pipe, callerUser, sizeof(callerUser), &callerSessionId, &callerPid, err, errSize)) {
        return false;
    }

    DWORD activeSessionId = (DWORD)-1;
    if (!get_active_interactive_session_id(&activeSessionId)) {
        set_message(err, errSize, "Failed determining the active interactive session");
        return false;
    }
    if (callerSessionId != activeSessionId) {
        set_message(err, errSize, "Service control is restricted to the active interactive session");
        debug_log("service auth reject: source=%s pid=%lu session=%lu activeSession=%lu user=%s\n",
            source ? source : "<none>",
            callerPid,
            callerSessionId,
            activeSessionId,
            callerUser[0] ? callerUser : "<unknown>");
        return false;
    }

    if (callerUserOut && callerUserOutSize > 0) {
        StringCchCopyA(callerUserOut, callerUserOutSize, callerUser);
    }
    if (callerSessionIdOut) *callerSessionIdOut = callerSessionId;
    if (callerPidOut) *callerPidOut = callerPid;
    return true;
}

static void service_set_pending_operation_source(const char* source) {
    char callerUser[256] = {};
    if (source && source[0]) {
        StringCchCopyA(callerUser, ARRAY_COUNT(callerUser), source);
    }
    set_pending_operation_source(callerUser);
}

static bool service_resolve_active_user_paths_for_startup(const char* context) {
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        debug_log("service user path resolve skipped%s%s: no active console session\n",
            context && context[0] ? " for " : "",
            context && context[0] ? context : "");
        return false;
    }

    char pathErr[256] = {};
    if (!resolve_service_user_data_paths(sessionId, pathErr, sizeof(pathErr))) {
        debug_log("service user path resolve failed%s%s: %s\n",
            context && context[0] ? " for " : "",
            context && context[0] ? context : "",
            pathErr[0] ? pathErr : "unknown");
        return false;
    }
    if (!g_app.configPath[0]) {
        set_default_config_path();
    }
    refresh_service_debug_logging_from_config();
    debug_log("service user paths ready%s%s: session=%lu config=%s\n",
        context && context[0] ? " for " : "",
        context && context[0] ? context : "",
        sessionId,
        g_app.configPath[0] ? g_app.configPath : "<unset>");
    return true;
}



static DWORD service_active_fan_runtime_interval_ms() {
    if (g_app.fanFixedRuntimeActive) return FAN_FIXED_RUNTIME_INTERVAL_MS;
    if (g_app.fanCurveRuntimeActive) {
        DWORD intervalMs = (DWORD)g_app.activeFanCurve.pollIntervalMs;
        return intervalMs < 250 ? 250 : intervalMs;
    }
    return 0;
}

static void mark_service_telemetry_cache_updated(const char* source) {
    if (!g_app.isServiceProcess) return;
    g_serviceTelemetryLastHardwarePollTickMs = GetTickCount64();
    if (source && source[0]) {
        StringCchCopyA(g_serviceTelemetryLastPollSource, ARRAY_COUNT(g_serviceTelemetryLastPollSource), source);
    } else {
        g_serviceTelemetryLastPollSource[0] = 0;
    }
}

static bool service_telemetry_cache_is_fresh(ULONGLONG now) {
    if (!g_serviceTelemetryLastHardwarePollTickMs) return false;
    ULONGLONG ageMs = now - g_serviceTelemetryLastHardwarePollTickMs;
    DWORD runtimeIntervalMs = service_active_fan_runtime_interval_ms();
    if (runtimeIntervalMs > 0) {
        return ageMs <= (ULONGLONG)runtimeIntervalMs + SERVICE_TELEMETRY_RUNTIME_STALE_GRACE_MS;
    }
    return ageMs < SERVICE_TELEMETRY_IDLE_REFRESH_INTERVAL_MS;
}

static bool service_refresh_idle_telemetry(char* detail, size_t detailSize) {
    char firstErr[128] = {};
    bool anyOk = false;

    char fanDetail[128] = {};
    if (nvml_read_fans(fanDetail, sizeof(fanDetail))) {
        anyOk = true;
    } else if (fanDetail[0]) {
        StringCchCopyA(firstErr, ARRAY_COUNT(firstErr), fanDetail);
    }

    char tempDetail[128] = {};
    int temperatureC = 0;
    if (nvml_read_temperature(&temperatureC, tempDetail, sizeof(tempDetail))) {
        anyOk = true;
    } else if (!firstErr[0] && tempDetail[0]) {
        StringCchCopyA(firstErr, ARRAY_COUNT(firstErr), tempDetail);
    }

    if (!anyOk) {
        set_message(detail, detailSize, "%s", firstErr[0] ? firstErr : "Telemetry refresh failed");
        return false;
    }

    populate_control_state(&g_serviceControlState);
    g_serviceControlStateValid = true;
    mark_service_telemetry_cache_updated("idle telemetry");
    return true;
}

static bool service_refresh_telemetry_for_request(char* detail, size_t detailSize) {
    if (!hardware_initialize(detail, detailSize)) return false;

    bool runtimeActive = g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive;
    if (runtimeActive) {
        bool needRuntimeThread = g_serviceFanThread == nullptr;
        if (g_serviceFanThread) {
            DWORD waitResult = WaitForSingleObject(g_serviceFanThread, 0);
            needRuntimeThread = waitResult != WAIT_TIMEOUT;
        }
        if (needRuntimeThread) {
            ensure_service_fan_runtime_thread();
        }
    }

    ULONGLONG now = GetTickCount64();
    if (runtimeActive) {
        if (!service_telemetry_cache_is_fresh(now)) {
            bool noRuntimeThread = !g_serviceFanThread;
            bool noCache = g_serviceTelemetryLastHardwarePollTickMs == 0;
            if (noRuntimeThread || noCache) {
                service_runtime_pulse();
            } else {
                debug_log("service telemetry: using stale runtime cache ageMs=%llu source=%s\n",
                    now - g_serviceTelemetryLastHardwarePollTickMs,
                    g_serviceTelemetryLastPollSource[0] ? g_serviceTelemetryLastPollSource : "<none>");
            }
        }
    } else if (!service_telemetry_cache_is_fresh(now)) {
        char telemetryDetail[128] = {};
        if (!service_refresh_idle_telemetry(telemetryDetail, sizeof(telemetryDetail))) {
            debug_log("service telemetry: lightweight refresh failed: %s\n",
                telemetryDetail[0] ? telemetryDetail : "unknown");
            mark_service_telemetry_cache_updated("idle telemetry failed");
        }
    }

    if (!g_serviceControlStateValid) {
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
    }
    if (detail && detailSize > 0) detail[0] = 0;
    return true;
}

static void service_runtime_pulse() {
    EnterCriticalSection(&g_appLock);
    bool curveActive = g_app.fanCurveRuntimeActive;
    bool fixedActive = g_app.fanFixedRuntimeActive;
    LeaveCriticalSection(&g_appLock);
    if (!curveActive && !fixedActive) return;
    ULONGLONG now = GetTickCount64();
    bool logPulse = g_serviceRuntimeLastPulseLogTickMs == 0 ||
        now - g_serviceRuntimeLastPulseLogTickMs >= SERVICE_RUNTIME_NOISY_LOG_INTERVAL_MS;
    if (logPulse) {
        g_serviceRuntimeLastPulseLogTickMs = now;
        EnterCriticalSection(&g_appLock);
        debug_log("service_runtime_pulse: curve=%d fixed=%d lastApplyMs=%llu mode=%d fixedPct=%d\n",
            g_app.fanCurveRuntimeActive ? 1 : 0,
            g_app.fanFixedRuntimeActive ? 1 : 0,
            g_app.fanRuntimeLastApplyTickMs,
            g_app.activeFanMode,
            g_app.activeFanFixedPercent);
        LeaveCriticalSection(&g_appLock);
    }
    apply_fan_curve_tick();
    mark_service_telemetry_cache_updated("fan runtime");
    if (logPulse) {
        EnterCriticalSection(&g_appLock);
        debug_log("service_runtime_pulse_done: fanMode=%d currentPct=%d temp=%d runtimeLastApply=%llu failures=%u\n",
            g_app.activeFanMode,
            g_app.activeFanFixedPercent,
            g_app.gpuTemperatureValid ? g_app.gpuTemperatureC : 0,
            g_app.fanRuntimeLastApplyTickMs,
            g_app.fanRuntimeConsecutiveFailures);
        LeaveCriticalSection(&g_appLock);
    }
}

static DWORD WINAPI service_fan_runtime_thread_proc(void*) {
    HANDLE waitHandles[1] = { g_serviceFanStopEvent };
    debug_log("service_fan_runtime_thread: started\n");
    while (true) {
        DWORD waitMs = INFINITE;
        EnterCriticalSection(&g_appLock);
        bool fanFixedRuntimeActive = g_app.fanFixedRuntimeActive;
        bool fanCurveRuntimeActive = g_app.fanCurveRuntimeActive;
        DWORD pollIntervalMs = fanCurveRuntimeActive ? (DWORD)g_app.activeFanCurve.pollIntervalMs : 0;
        LeaveCriticalSection(&g_appLock);
        if (fanFixedRuntimeActive) waitMs = FAN_FIXED_RUNTIME_INTERVAL_MS;
        else if (fanCurveRuntimeActive) {
            waitMs = pollIntervalMs;
            if (waitMs < 250) waitMs = 250;
        }
        bool curveActive = fanCurveRuntimeActive;
        bool fixedActive = fanFixedRuntimeActive;
        ULONGLONG now = GetTickCount64();
        if (g_serviceFanThreadLastWaitLogTickMs == 0 ||
            now - g_serviceFanThreadLastWaitLogTickMs >= SERVICE_RUNTIME_NOISY_LOG_INTERVAL_MS ||
            waitMs != g_serviceFanThreadLastWaitMs ||
            curveActive != g_serviceFanThreadLastWaitCurve ||
            fixedActive != g_serviceFanThreadLastWaitFixed) {
            g_serviceFanThreadLastWaitLogTickMs = now;
            g_serviceFanThreadLastWaitMs = waitMs;
            g_serviceFanThreadLastWaitCurve = curveActive;
            g_serviceFanThreadLastWaitFixed = fixedActive;
            debug_log("service_fan_runtime_thread: waiting %lu ms curve=%d fixed=%d\n",
                waitMs,
                curveActive ? 1 : 0,
                fixedActive ? 1 : 0);
        }
        DWORD waitResult = WaitForMultipleObjects(1, waitHandles, FALSE, waitMs);
        if (waitResult == WAIT_OBJECT_0) break;
        if (waitResult == WAIT_TIMEOUT) {
            lock_service_runtime();
            service_runtime_pulse();
            unlock_service_runtime();
        } else if (waitResult == WAIT_FAILED) {
            debug_log("service_fan_runtime_thread: wait failed error=%lu\n", GetLastError());
            break;
        }
    }
    debug_log("service_fan_runtime_thread: exiting\n");
    return 0;
}

static bool ensure_service_fan_runtime_thread() {
    if (g_serviceFanThread) {
        DWORD waitResult = WaitForSingleObject(g_serviceFanThread, 0);
        if (waitResult == WAIT_TIMEOUT) {
            debug_log("ensure_service_fan_runtime_thread: already running\n");
            return true;
        }
        // Thread has exited; close stale handle and recreate.
        CloseHandle(g_serviceFanThread);
        g_serviceFanThread = nullptr;
        debug_log("ensure_service_fan_runtime_thread: stale handle detected, recreating\n");
    }
    if (!g_serviceFanStopEvent) {
        g_serviceFanStopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!g_serviceFanStopEvent) return false;
    }
    ResetEvent(g_serviceFanStopEvent);
    DWORD threadId = 0;
    g_serviceFanThread = CreateThread(nullptr, 64 * 1024, service_fan_runtime_thread_proc, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &threadId);
    debug_log("ensure_service_fan_runtime_thread: created=%d threadId=%lu\n", g_serviceFanThread ? 1 : 0, threadId);
    return g_serviceFanThread != nullptr;
}

static void stop_service_fan_runtime_thread() {
    if (!g_serviceFanThread) return;
    if (g_serviceFanStopEvent) SetEvent(g_serviceFanStopEvent);
    bool lockHeld = service_runtime_lock_held_by_current_thread();
    if (lockHeld) {
        // Release the runtime lock so the fan thread can finish its current
        // pulse and observe the stop event. Without this we would deadlock.
        unlock_service_runtime();
    }
    DWORD waitResult = WaitForSingleObject(g_serviceFanThread, SERVICE_FAN_THREAD_STOP_TIMEOUT_MS);
    if (waitResult != WAIT_OBJECT_0) {
        debug_log("stop_service_fan_runtime_thread: timed out waiting for fan thread (%lu)\n", waitResult);
        if (lockHeld) {
            lock_service_runtime();
        }
        return;
    }
    if (lockHeld) {
        lock_service_runtime();
    }
    CloseHandle(g_serviceFanThread);
    g_serviceFanThread = nullptr;
}

static bool service_apply_desired_settings(const DesiredSettings* desired, bool interactive, char* result, size_t resultSize) {
    if (!desired) {
        set_message(result, resultSize, "No desired settings provided");
        return false;
    }
    char detail[256] = {};
    set_last_apply_phase("service apply: hardware initialize");
    if (!hardware_initialize(detail, sizeof(detail))) {
        set_message(result, resultSize, "%s", detail[0] ? detail : "Hardware initialization failed");
        set_last_apply_phase("service apply: hardware initialize failed");
        return false;
    }
    int requestedCurvePoints = 0;
    if (desired) {
        for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
            if (desired->hasCurvePoint[ci]) requestedCurvePoints++;
        }
    }
    debug_log("service_apply_desired_settings: interactive=%d gpu=%d exclude=%d mem=%d power=%d fanMode=%d lockCi=%d lockMHz=%u curvePoints=%d\n",
        interactive ? 1 : 0,
        desired && desired->hasGpuOffset ? desired->gpuOffsetMHz : 0,
        desired && desired->hasGpuOffset ? desired->gpuOffsetExcludeLowCount : 0,
        desired && desired->hasMemOffset ? desired->memOffsetMHz : 0,
        desired && desired->hasPowerLimit ? desired->powerLimitPct : 0,
        desired && desired->hasFan ? desired->fanMode : -1,
        desired && desired->hasLock ? desired->lockCi : -1,
        desired && desired->hasLock ? desired->lockMHz : 0u,
        requestedCurvePoints);
    set_last_apply_phase("service apply: apply desired settings");
    bool ok = apply_desired_settings(desired, interactive, result, resultSize);
    if (ok) {
        set_last_apply_phase("service apply: capture authoritative state");
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
        mark_service_telemetry_cache_updated("service apply");
        g_serviceActiveDesired = *desired;
        g_serviceActiveDesired.resetOcBeforeApply = false;
        update_desired_lock_from_live_curve(&g_serviceActiveDesired);
        g_serviceHasActiveDesired = true;
        if (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive) {
            ensure_service_fan_runtime_thread();
        } else {
            stop_service_fan_runtime_thread();
        }
    } else {
        clear_service_authoritative_state();
        set_last_apply_phase("service apply: capture partial state");
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
        mark_service_telemetry_cache_updated("service apply partial");
        g_serviceHasActiveDesired = false;
        memset(&g_serviceActiveDesired, 0, sizeof(g_serviceActiveDesired));
    }
    set_last_apply_phase(ok ? "service apply: complete" : "service apply: failed");
    return ok;
}

static bool service_reset_all(char* result, size_t resultSize) {
    char detail[256] = {};
    if (!hardware_initialize(detail, sizeof(detail))) {
        set_message(result, resultSize, "%s", detail[0] ? detail : "Hardware initialization failed");
        return false;
    }

    int resetOffsets[VF_NUM_POINTS] = {};
    bool resetMask[VF_NUM_POINTS] = {};
    int successCount = 0;
    int failCount = 0;
    char failureDetails[1024] = {};
    auto append_failure = [&](const char* fmt, ...) {
        char part[256] = {};
        va_list ap;
        va_start(ap, fmt);
        StringCchVPrintfA(part, ARRAY_COUNT(part), fmt, ap);
        va_end(ap);
        if (!part[0]) return;
        if (failureDetails[0]) StringCchCatA(failureDetails, ARRAY_COUNT(failureDetails), "; ");
        StringCchCatA(failureDetails, ARRAY_COUNT(failureDetails), part);
    };
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.curve[ci].freq_kHz == 0) continue;
        resetMask[ci] = true;
    }
    bool hadCurveOffsets = false;
    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
        if (g_app.freqOffsets[ci] != 0) {
            hadCurveOffsets = true;
            break;
        }
    }
    if (hadCurveOffsets) {
        if (apply_curve_offsets_verified(resetOffsets, resetMask, 2)) successCount++;
        else {
            failCount++;
            append_failure("VF curve offsets did not reset cleanly");
        }
    }
    if (g_app.gpuClockOffsetkHz != 0) {
        if (nvapi_set_gpu_offset(0)) successCount++;
        else {
            failCount++;
            append_failure("GPU offset did not reset to default");
        }
    }
    if (g_app.memClockOffsetkHz != 0) {
        if (nvapi_set_mem_offset(0)) successCount++;
        else {
            failCount++;
            append_failure("Memory offset did not reset to default");
        }
    }
    if (g_app.powerLimitPct != 100) {
        if (nvapi_set_power_limit(100)) successCount++;
        else {
            failCount++;
            append_failure("Power limit did not reset to default");
        }
    }

    // Stop the service-owned fan maintenance first so it cannot immediately
    // reassert a manual target after we restore driver auto.
    stop_fan_curve_runtime();
    if (g_app.isServiceProcess && g_serviceFanThread) {
        stop_service_fan_runtime_thread();
    }

    if (!g_app.fanIsAuto || g_app.activeFanMode != FAN_MODE_AUTO) {
        char fanDetail[128] = {};
        if (nvml_set_fan_auto(fanDetail, sizeof(fanDetail))) {
            successCount++;
            g_app.fanIsAuto = true;
            g_app.activeFanMode = FAN_MODE_AUTO;
            g_app.activeFanFixedPercent = 0;
        } else {
            failCount++;
            append_failure("Fan control did not return to driver auto%s%s",
                fanDetail[0] ? ": " : "",
                fanDetail[0] ? fanDetail : "");
        }
    }

    // Clear persisted runtime state BEFORE refreshing so the refresh sees the
    // true post-reset hardware state rather than the old persisted request.
    if (failCount == 0) {
        clear_runtime_selective_gpu_offset_request();
    }

    if (!refresh_global_state(detail, sizeof(detail))) {
        append_failure("Failed to refresh live state after reset%s%s",
            detail[0] ? ": " : "",
            detail[0] ? detail : "");
        failCount++;
    }
    if (g_app.fanSupported) {
        char fanDetail[128] = {};
        nvml_read_fans(fanDetail, sizeof(fanDetail));
    }
    initialize_gui_fan_settings_from_live_state(false);
    if (g_app.fanIsAuto) {
        g_app.guiFanMode = FAN_MODE_AUTO;
        g_app.guiFanFixedPercent = 0;
        fan_curve_set_default(&g_app.guiFanCurve);
        fan_curve_set_default(&g_app.activeFanCurve);
    }
    g_serviceHasActiveDesired = false;
    memset(&g_serviceActiveDesired, 0, sizeof(g_serviceActiveDesired));
    if (failCount == 0) {
        g_app.appliedGpuOffsetMHz = 0;
        g_app.appliedGpuOffsetExcludeLowCount = 0;
        clear_service_authoritative_state();
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
        set_message(result, resultSize, "Reset applied.");
        return true;
    }
    populate_control_state(&g_serviceControlState);
    g_serviceControlStateValid = true;
    mark_service_telemetry_cache_updated("service reset");
    set_message(result, resultSize, "Reset applied %d OK, %d failed: %s", successCount, failCount, failureDetails[0] ? failureDetails : "one or more reset steps failed");
    return false;
}
