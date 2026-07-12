// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Driver-recovery monitoring, runtime serialization, and caller/session identity.

// ---- NVML close helper (used by the VEH) ----
//
// GPU driver-recovery itself is handled by restarting the service process
// (see launch_recovery_thread() below).  The only NVML teardown that still
// runs in-process is this no-shutdown close, invoked by the VEH so a stale-
// handle access violation does not leave dangling function pointers before
// the process exits and the SCM relaunches a fresh, clean instance.

// Close NVML WITHOUT calling nvmlShutdown — this can hang at 100% CPU on
// a dead/stale driver instance.  Just FreeLibrary the module and zero all
// pointers.  The no-log helper is also used from the VEH, where taking the
// debug-log lock would be unsafe.
static void service_close_nvml_without_shutdown() {
    g_app.nvmlReady = false;
    g_app.nvmlDevice = nullptr;
    if (g_nvml) {
        HMODULE oldMod = g_nvml;
        g_nvml = nullptr;
        memset(&g_nvml_api, 0, sizeof(g_nvml_api));
        FreeLibrary(oldMod);
    } else {
        memset(&g_nvml_api, 0, sizeof(g_nvml_api));
    }
}


// Read a fixed-file-info version (MS + LS DWORDs) from a file on disk.
// Returns true and fills *outMs/*outLs on success, false on any error
// (file missing, no version resource, allocation failure).  Used by the
// on-disk driver version check below.
static bool service_get_file_version(const WCHAR* path, DWORD* outMs, DWORD* outLs) {
    if (!path || !outMs || !outLs) return false;
    DWORD handle = 0;
    DWORD infoSize = GetFileVersionInfoSizeW(path, &handle);
    if (infoSize == 0) return false;
    HeapBuffer buf((size_t)infoSize);
    if (!buf) return false;
    if (!GetFileVersionInfoW(path, handle, infoSize, buf)) return false;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoLen = 0;
    if (!VerQueryValueW((LPVOID)(unsigned char*)buf, L"\\", (LPVOID*)&info, &infoLen)) return false;
    if (!info || infoLen < sizeof(VS_FIXEDFILEINFO)) return false;
    *outMs = info->dwFileVersionMS;
    *outLs = info->dwFileVersionLS;
    return true;
}

// Read the version of an already-loaded module by its HMODULE.
// Returns true and fills *outMs/*outLs on success.
static bool service_get_module_version(HMODULE mod, DWORD* outMs, DWORD* outLs) {
    if (!mod || !outMs || !outLs) return false;
    WCHAR path[MAX_PATH] = {};
    if (GetModuleFileNameW(mod, path, ARRAY_COUNT(path)) == 0) return false;
    return service_get_file_version(path, outMs, outLs);
}

// Build the on-disk path for nvml.dll.  Prefers the NVSMI copy
// (%ProgramFiles%\NVIDIA Corporation\NVSMI\nvml.dll) because the driver
// installer updates both copies in lockstep.  Falls back to system32.
static bool service_get_nvml_disk_path(WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    WCHAR systemDir[MAX_PATH] = {};
    UINT systemLen = GetSystemDirectoryW(systemDir, ARRAY_COUNT(systemDir));
    if (systemLen == 0 || systemLen >= ARRAY_COUNT(systemDir)) return false;
    if (FAILED(StringCchPrintfW(out, outCount, L"%ls\\nvml.dll", systemDir))) return false;
    return true;
}

static bool service_get_nvapi_disk_path(WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    WCHAR systemDir[MAX_PATH] = {};
    UINT systemLen = GetSystemDirectoryW(systemDir, ARRAY_COUNT(systemDir));
    if (systemLen == 0 || systemLen >= ARRAY_COUNT(systemDir)) return false;
    if (FAILED(StringCchPrintfW(out, outCount, L"%ls\\nvapi64.dll", systemDir))) return false;
    return true;
}

// Detect whether the on-disk nvml.dll / nvapi64.dll differ from the version
// currently mapped into the process.  After a driver upgrade, the in-process
// copy is the old version and the on-disk file is the new one.  A simple
// FreeLibrary + LoadLibrary may not pick up the new file if the old file is
// still cached in the loader's mapping table.  Returning true here tells the
// caller to force a fresh load.  This is the RC4 fix.
static bool service_nvml_disk_version_changed() {
    if (!g_nvml) return false;
    DWORD inMs = 0, inLs = 0;
    if (!service_get_module_version(g_nvml, &inMs, &inLs)) {
        debug_log("service_nvml_disk_version_changed: in-process version unavailable\n");
        return false;
    }
    WCHAR diskPath[MAX_PATH] = {};
    if (!service_get_nvml_disk_path(diskPath, ARRAY_COUNT(diskPath))) return false;
    DWORD onMs = 0, onLs = 0;
    if (!service_get_file_version(diskPath, &onMs, &onLs)) {
        debug_log("service_nvml_disk_version_changed: on-disk version unavailable for %ls\n", diskPath);
        return false;
    }
    if (inMs == onMs && inLs == onLs) return false;
    debug_log("service_nvml_disk_version_changed: in-process %u.%u.%u.%u differs from on-disk %ls %u.%u.%u.%u\n",
        (unsigned)(inMs >> 16), (unsigned)(inMs & 0xFFFFu),
        (unsigned)(inLs >> 16), (unsigned)(inLs & 0xFFFFu),
        diskPath,
        (unsigned)(onMs >> 16), (unsigned)(onMs & 0xFFFFu),
        (unsigned)(onLs >> 16), (unsigned)(onLs & 0xFFFFu));
    return true;
}

static bool service_nvapi_disk_version_changed() {
    if (!g_app.hNvApi) return false;
    DWORD inMs = 0, inLs = 0;
    if (!service_get_module_version(g_app.hNvApi, &inMs, &inLs)) {
        debug_log("service_nvapi_disk_version_changed: in-process version unavailable\n");
        return false;
    }
    WCHAR diskPath[MAX_PATH] = {};
    if (!service_get_nvapi_disk_path(diskPath, ARRAY_COUNT(diskPath))) return false;
    DWORD onMs = 0, onLs = 0;
    if (!service_get_file_version(diskPath, &onMs, &onLs)) {
        debug_log("service_nvapi_disk_version_changed: on-disk version unavailable for %ls\n", diskPath);
        return false;
    }
    if (inMs == onMs && inLs == onLs) return false;
    debug_log("service_nvapi_disk_version_changed: in-process %u.%u.%u.%u differs from on-disk %ls %u.%u.%u.%u\n",
        (unsigned)(inMs >> 16), (unsigned)(inMs & 0xFFFFu),
        (unsigned)(inLs >> 16), (unsigned)(inLs & 0xFFFFu),
        diskPath,
        (unsigned)(onMs >> 16), (unsigned)(onMs & 0xFFFFu),
        (unsigned)(onLs >> 16), (unsigned)(onLs & 0xFFFFu));
    return true;
}

// Public entry point: log a single line per DLL when the on-disk version
// differs from the in-process module.  Called from the DBT_DEVICEARRIVAL
// handler (main_service_server.cpp) to correlate the device-reconnect
// event with a possible driver upgrade.  The actual reload happens in the
// recovery thread (Phase B/C) where the runtime lock is held.
void service_check_disk_version_on_device_arrival() {
    bool nvmlChanged = service_nvml_disk_version_changed();
    bool nvapiChanged = service_nvapi_disk_version_changed();
    if (nvmlChanged || nvapiChanged) {
        debug_log("service_check_disk_version_on_device_arrival: driver upgrade detected "
            "(nvmlChanged=%d nvapiChanged=%d) — recovery will pick up new files\n",
            nvmlChanged ? 1 : 0, nvapiChanged ? 1 : 0);
    } else {
        debug_log("service_check_disk_version_on_device_arrival: on-disk driver matches in-process\n");
    }
}

// ---- GPU driver-recovery: recover by restarting the service process ----
//
// In-process reload of nvml.dll / nvapi64.dll after a GPU device reconnect or an
// in-place driver upgrade is unreliable: the NVIDIA user-mode DLLs stay mapped
// (driver-pinned, with live worker threads), so FreeLibrary does not unmap them
// and a subsequent LoadLibrary returns the OLD image; nvmlInit then reports
// ALREADY_INITIALIZED and hands back a device handle bound to the dead driver
// instance.  NvAPI is entangled with the process-global NVIDIA UMD stack, which
// cannot be force-reloaded and must version-match the kernel driver.  The only
// state that reliably re-binds a version-matched UMD to the new kernel driver is
// a FRESH PROCESS.
//
// So every recovery trigger (DBT_DEVICEARRIVAL after a removal, a VEH-caught
// stale-handle access violation, a fan-pulse wedge, or an on-disk driver-version
// change) requests a controlled service restart.  request_service_restart()
// snapshots the active OC/fan profile, records the restart for loop protection,
// launches a nonce-bound helper and commits the dedicated clean exit. Only
// that helper may demand-start a fresh process with the validated continuation.
// The function keeps its historical name because all four trigger call sites
// already call it.
static void launch_recovery_thread() {
    request_service_restart("GPU driver recovery (device reconnect / driver upgrade / TDR)");
}

static void service_maybe_launch_recovery_from_main_loop(const char* source) {
    bool deviceWasRemoved = false;
    bool pendingRecovery = false;
    EnterCriticalSection(&g_appLock);
    if (g_app.deviceRemoved) deviceWasRemoved = true;
    if (g_app.pendingDeviceRecovery) pendingRecovery = true;
    LeaveCriticalSection(&g_appLock);
    if (deviceWasRemoved) return;

    LONG crashCount = g_nvmlCrashCount;
    if (crashCount <= 0 && !pendingRecovery) return;

    debug_log("service recovery monitor: %s requesting service restart for driver recovery (crashCount=%ld pending=%d)\n",
        source && source[0] ? source : "main loop",
        (long)crashCount,
        pendingRecovery ? 1 : 0);
    launch_recovery_thread();
}

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

static bool ensure_service_runtime_lock() {
    if (g_serviceRuntimeLock) return true;
    g_serviceRuntimeLock = CreateMutexA(nullptr, FALSE, nullptr);
    if (!g_serviceRuntimeLock) {
        DWORD createError = GetLastError();
        debug_log("service runtime serialization: FATAL mutex creation failed (error=%lu)\n",
            createError);
        SetLastError(createError);
    }
    return g_serviceRuntimeLock != nullptr;
}

[[noreturn]] static void service_runtime_lock_fail_closed(
    const char* operation, DWORD error) {
    InterlockedExchange(&g_serviceClientRequestsReady, 0);
    service_latch_auto_restore_lockout(
        SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
        "service runtime serialization failed");
    debug_log("service runtime serialization: FATAL %s failed (error=%lu); terminating without another hardware write\n",
        operation && operation[0] ? operation : "operation",
        (unsigned long)error);
    ExitProcess(ERROR_PROCESS_ABORTED);
}

[[noreturn]] static void service_runtime_mutex_abandoned(
    const char* operation) {
    // WAIT_ABANDONED grants ownership to this thread.  Never expose that
    // inconsistent protected state to another hardware caller.  A VEH marker
    // recorded before the faulting thread exited is the only evidence that
    // authorizes the nonce-bound recovery coordinator; an unexplained
    // abandonment remains a sticky, ordinary fail-closed stop.
    bool corroborated = InterlockedExchangeAdd(&g_nvmlVhCrashed, 0) != 0 &&
        InterlockedExchangeAdd(&g_nvmlCrashCount, 0) > 0;
    InterlockedExchange(&g_serviceRuntimeLockPoisoned, 1);
    if (corroborated) {
        InterlockedExchange(&g_serviceRuntimePoisonCorroborated, 1);
    }
    InterlockedExchange(&g_serviceClientRequestsReady, 0);
    ReleaseMutex(g_serviceRuntimeLock);
    g_serviceRuntimeLockOwnerThreadId = 0;
    g_serviceRuntimeLockDepth = 0;
    debug_log("service runtime serialization: %s observed abandoned mutex (VEH-corroborated=%d); hardware gate is permanently closed for this process\n",
        operation && operation[0] ? operation : "wait",
        corroborated ? 1 : 0);

    if (GetCurrentThreadId() == g_serviceMainThreadId) {
        service_emergency_restart_from_poisoned_runtime(
            corroborated
                ? "VEH-corroborated stale-driver mutex abandonment"
                : "unexplained runtime mutex abandonment",
            corroborated);
    }
    // Only the service main thread may prepare/commit the helper.  Wake it and
    // retire this worker before it can continue from an acquisition whose
    // protected invariants are no longer trustworthy.
    if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
    ExitThread(ERROR_ABANDONED_WAIT_0);
    __builtin_unreachable();
}

[[noreturn]] static void service_runtime_reject_poisoned_acquisition(
    const char* operation, bool ownsMutex) {
    if (ownsMutex && g_serviceRuntimeLock) {
        ReleaseMutex(g_serviceRuntimeLock);
    }
    g_serviceRuntimeLockOwnerThreadId = 0;
    g_serviceRuntimeLockDepth = 0;
    InterlockedExchange(&g_serviceClientRequestsReady, 0);
    bool corroborated = InterlockedExchangeAdd(
        &g_serviceRuntimePoisonCorroborated, 0) != 0;
    debug_log("service runtime serialization: rejected %s after runtime mutex poison; no hardware operation may continue\n",
        operation && operation[0] ? operation : "acquisition");
    if (GetCurrentThreadId() == g_serviceMainThreadId) {
        service_emergency_restart_from_poisoned_runtime(
            corroborated ? "corroborated poisoned runtime" :
                "uncorroborated poisoned runtime",
            corroborated);
    }
    if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
    ExitThread(ERROR_PROCESS_ABORTED);
    __builtin_unreachable();
}

static void lock_service_runtime() {
    if (InterlockedExchangeAdd(&g_serviceRuntimeLockPoisoned, 0) != 0) {
        service_runtime_reject_poisoned_acquisition("blocking acquisition", false);
    }
    if (!ensure_service_runtime_lock()) {
        service_runtime_lock_fail_closed("mutex creation", GetLastError());
    }
    DWORD waitResult = WaitForSingleObject(g_serviceRuntimeLock, INFINITE);
    if (waitResult == WAIT_ABANDONED) {
        service_runtime_mutex_abandoned("blocking wait");
    }
    if (waitResult != WAIT_OBJECT_0) {
        service_runtime_lock_fail_closed("mutex wait",
            waitResult == WAIT_FAILED ? GetLastError() : waitResult);
    }
    if (InterlockedExchangeAdd(&g_serviceRuntimeLockPoisoned, 0) != 0) {
        service_runtime_reject_poisoned_acquisition(
            "blocking acquisition", true);
    }
    DWORD currentThreadId = GetCurrentThreadId();
    if (g_serviceRuntimeLockOwnerThreadId == currentThreadId) {
        g_serviceRuntimeLockDepth++;
    } else {
        g_serviceRuntimeLockOwnerThreadId = currentThreadId;
        g_serviceRuntimeLockDepth = 1;
    }
}

// Timed variant: returns true if the lock was acquired, false on timeout.
// Same recursive ownership tracking as lock_service_runtime().  Used by the
// pipe server to avoid blocking the single-instance pipe during recovery
// reapply, so PING and other lightweight commands are not starved.
static bool try_lock_service_runtime(DWORD timeoutMs) {
    if (InterlockedExchangeAdd(&g_serviceRuntimeLockPoisoned, 0) != 0) {
        service_runtime_reject_poisoned_acquisition("timed acquisition", false);
    }
    if (!ensure_service_runtime_lock()) return false;
    DWORD waitResult = WaitForSingleObject(g_serviceRuntimeLock, timeoutMs);
    if (waitResult == WAIT_ABANDONED) {
        service_runtime_mutex_abandoned("timed wait");
    }
    if (waitResult == WAIT_OBJECT_0) {
        if (InterlockedExchangeAdd(&g_serviceRuntimeLockPoisoned, 0) != 0) {
            service_runtime_reject_poisoned_acquisition(
                "timed acquisition", true);
        }
        DWORD currentThreadId = GetCurrentThreadId();
        if (g_serviceRuntimeLockOwnerThreadId == currentThreadId) {
            g_serviceRuntimeLockDepth++;
        } else {
            g_serviceRuntimeLockOwnerThreadId = currentThreadId;
            g_serviceRuntimeLockDepth = 1;
        }
        return true;
    }
    return false;
}

static void unlock_service_runtime() {
    DWORD currentThreadId = GetCurrentThreadId();
    if (!g_serviceRuntimeLock ||
        g_serviceRuntimeLockOwnerThreadId != currentThreadId ||
        g_serviceRuntimeLockDepth == 0) {
        service_runtime_lock_fail_closed("unbalanced mutex release",
            ERROR_INVALID_OWNER);
    }
    g_serviceRuntimeLockDepth--;
    if (g_serviceRuntimeLockDepth == 0) {
        g_serviceRuntimeLockOwnerThreadId = 0;
    }
    if (!ReleaseMutex(g_serviceRuntimeLock)) {
        service_runtime_lock_fail_closed("mutex release", GetLastError());
    }
}

static bool service_runtime_lock_held_by_current_thread() {
    return g_serviceRuntimeLockOwnerThreadId == GetCurrentThreadId() && g_serviceRuntimeLockDepth > 0;
}

static bool get_active_interactive_session_id(DWORD* sessionIdOut) {
    if (sessionIdOut) *sessionIdOut = (DWORD)-1;

    // WTSGetActiveConsoleSessionId identifies the console attachment, not a
    // token-ready logged-on user.  During a console attach/logon transition it
    // can therefore name a session that is not yet WTSActive; selecting it
    // blindly makes a session-change worker give up on WTSQueryUserToken and
    // miss the only logon event.  Enumerate state once, prefer the console only
    // when it is actually active, then fall back to an active RDP session.
    DWORD consoleSessionId = WTSGetActiveConsoleSessionId();
    PWTS_SESSION_INFOA sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsA(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        // Console attachment is not proof of WTSActive state.  Returning it on
        // enumeration failure can authorize a disconnected console account
        // while another interactive session is active.  Lifecycle callers keep
        // their identity pending and retry on the next real session cue.
        debug_log("active session: WTSEnumerateSessions failed (error=%lu); failing closed until a session cue\n",
            GetLastError());
        return false;
    }

    DWORD fallbackActiveSessionId = (DWORD)-1;
    bool found = false;
    for (DWORD i = 0; i < count; i++) {
        if (sessions[i].State == WTSActive) {
            if (sessions[i].SessionId == consoleSessionId) {
                if (sessionIdOut) *sessionIdOut = consoleSessionId;
                found = true;
                break;
            }
            if (fallbackActiveSessionId == (DWORD)-1) {
                fallbackActiveSessionId = sessions[i].SessionId;
            }
        }
    }
    WTSFreeMemory(sessions);
    if (!found && fallbackActiveSessionId != (DWORD)-1) {
        if (sessionIdOut) *sessionIdOut = fallbackActiveSessionId;
        found = true;
    }
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

// True if `token`'s user is a member of BUILTIN\Administrators.  Matches even
// when the group is USE_FOR_DENY_ONLY (an unelevated admin's UAC-filtered token
// carries Administrators deny-only), so this answers "is this account a machine
// administrator" independent of the current elevation state.
static bool token_is_local_admin(HANDLE token) {
    if (!token) return false;
    BYTE adminSid[SECURITY_MAX_SID_SIZE] = {};
    DWORD sidLen = sizeof(adminSid);
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &sidLen)) return false;
    DWORD len = 0;
    GetTokenInformation(token, TokenGroups, nullptr, 0, &len);
    if (len == 0) return false;
    TOKEN_GROUPS* groups = (TOKEN_GROUPS*)malloc(len);
    if (!groups) return false;
    bool isAdmin = false;
    if (GetTokenInformation(token, TokenGroups, groups, len, &len)) {
        for (DWORD i = 0; i < groups->GroupCount; i++) {
            if (groups->Groups[i].Sid && EqualSid(groups->Groups[i].Sid, adminSid)) {
                isAdmin = true;
                break;
            }
        }
    }
    free(groups);
    return isAdmin;
}

bool current_user_is_local_admin() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    bool isAdmin = token_is_local_admin(token);
    CloseHandle(token);
    return isAdmin;
}

// Is the interactive user of `sessionId` a machine administrator?  Queries the
// session's primary token (the SYSTEM service has the privilege) and reuses the
// deny-only-aware token_is_local_admin.  Used to enforce the shared-only policy
// on the service-side logon auto-apply, mirroring the SERVICE_CMD_APPLY caller
// admin check so all apply paths agree on "who is an admin".
static bool service_session_user_is_local_admin(DWORD sessionId) {
    if (sessionId == (DWORD)-1) return false;
    HANDLE token = nullptr;
    if (!WTSQueryUserToken(sessionId, &token)) return false;
    bool isAdmin = token_is_local_admin(token);
    CloseHandle(token);
    return isAdmin;
}

static bool get_pipe_client_identity(HANDLE pipe, char* userOut,
    size_t userOutSize, DWORD* sessionIdOut, DWORD* pidOut, bool* isAdminOut,
    ServiceLifecycleIdentity* lifecycleIdentityOut, char* err, size_t errSize) {
    if (isAdminOut) *isAdminOut = false;
    if (lifecycleIdentityOut) {
        memset(lifecycleIdentityOut, 0, sizeof(*lifecycleIdentityOut));
    }
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

    ServiceLifecycleIdentity lifecycleIdentity = {};
    bool ok = get_token_sam_name(token, userOut, userOutSize) &&
        service_identity_from_token(token, sessionId, &lifecycleIdentity,
            err, errSize);
    if (ok && isAdminOut) *isAdminOut = token_is_local_admin(token);
    CloseHandle(token);
    CloseHandle(process);
    if (!ok) {
        set_message(err, errSize, "Failed resolving service client identity");
        return false;
    }
    if (lifecycleIdentityOut) *lifecycleIdentityOut = lifecycleIdentity;
    return true;
}

static bool service_caller_is_authorized(HANDLE pipe, const char* source,
    bool requireActiveSession, char* err, size_t errSize, char* callerUserOut,
    size_t callerUserOutSize, DWORD* callerSessionIdOut, DWORD* callerPidOut,
    bool* callerIsAdminOut, ServiceLifecycleIdentity* lifecycleIdentityOut) {
    if (callerIsAdminOut) *callerIsAdminOut = false;
    if (lifecycleIdentityOut) {
        memset(lifecycleIdentityOut, 0, sizeof(*lifecycleIdentityOut));
    }
    DWORD callerSessionId = (DWORD)-1;
    DWORD callerPid = 0;
    bool callerIsAdmin = false;
    char callerUser[256] = {};
    ServiceLifecycleIdentity lifecycleIdentity = {};
    if (!get_pipe_client_identity(pipe, callerUser, sizeof(callerUser),
            &callerSessionId, &callerPid, &callerIsAdmin,
            &lifecycleIdentity, err, errSize)) {
        return false;
    }

    if (requireActiveSession) {
        DWORD activeSessionId = (DWORD)-1;
        if (!get_active_interactive_session_id(&activeSessionId)) {
            set_message(err, errSize,
                "Failed determining the active interactive session");
            return false;
        }
        if (callerSessionId != activeSessionId) {
            set_message(err, errSize,
                "Service control is restricted to the active interactive session");
            debug_log("service auth reject: source=%s pid=%lu session=%lu activeSession=%lu user=%s\n",
                source ? source : "<none>",
                callerPid,
                callerSessionId,
                activeSessionId,
                callerUser[0] ? callerUser : "<unknown>");
            return false;
        }
    } else {
        // The settings-free scheduled-task handoff may race the session's
        // transition to ACTIVE. The lifecycle worker retains this exact token
        // identity and rechecks active-session ownership immediately before any
        // write, so rejecting it here would recreate the startup timing bug.
        debug_log("service auth: accepted settings-free lifecycle handoff before active-session gating pid=%lu session=%lu user=%s auth=%llu\n",
            (unsigned long)callerPid, (unsigned long)callerSessionId,
            callerUser[0] ? callerUser : "<unknown>",
            (unsigned long long)lifecycleIdentity.authenticationId);
    }

    if (callerUserOut && callerUserOutSize > 0) {
        StringCchCopyA(callerUserOut, callerUserOutSize, callerUser);
    }
    if (callerSessionIdOut) *callerSessionIdOut = callerSessionId;
    if (callerPidOut) *callerPidOut = callerPid;
    if (callerIsAdminOut) *callerIsAdminOut = callerIsAdmin;
    if (lifecycleIdentityOut) *lifecycleIdentityOut = lifecycleIdentity;
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
