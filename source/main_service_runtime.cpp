
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
// and exits the process non-zero; the SCM failure action relaunches us and
// service_launch_startup_reapply() restores the profile on the fresh process —
// i.e. the known-stable fresh-boot path.  The function keeps its historical name
// because all four trigger call sites already call it.
static void launch_recovery_thread() {
    request_service_restart("GPU driver recovery (device reconnect / driver upgrade / TDR)");
}

static void service_maybe_launch_recovery_from_main_loop(const char* source) {
    if (InterlockedExchangeAdd(&g_serviceGpuRecovering, 0) != 0) return;

    bool deviceWasRemoved = false;
    bool pendingRecovery = false;
    EnterCriticalSection(&g_appLock);
    if (g_app.deviceRemoved) deviceWasRemoved = true;
    if (g_app.pendingDeviceRecovery) pendingRecovery = true;
    LeaveCriticalSection(&g_appLock);
    if (deviceWasRemoved) return;

    LONG crashCount = g_nvmlCrashCount;
    if (crashCount <= 0 && !pendingRecovery) return;

    ULONGLONG now = GetTickCount64();
    ULONGLONG lastRecoveryMs = g_serviceLastRecoveryTickMs;
    if (lastRecoveryMs != 0 && (now - lastRecoveryMs) < NVML_CRASH_RECOVERY_WINDOW_MS) {
        return;
    }

    if (lastRecoveryMs != 0 && g_nvmlCrashTickMs != 0 &&
        g_nvmlCrashTickMs <= lastRecoveryMs && !pendingRecovery) {
        debug_log("service recovery monitor: safety window expired, clearing stale crash flags\n");
        g_nvmlCrashCount = 0;
        g_nvmlCrashTickMs = 0;
        g_serviceLastRecoveryTickMs = 0;
        return;
    }

    debug_log("service recovery monitor: %s requesting service restart for driver recovery (crashCount=%ld pending=%d)\n",
        source && source[0] ? source : "main loop",
        (long)crashCount,
        pendingRecovery ? 1 : 0);
    launch_recovery_thread();
}

static void service_check_oc_persistence(bool isDriverRecovery) {
    if (!g_serviceHasActiveDesired || !nvml_ensure_ready()) return;

    debug_log("oc_persistence: checking%s\n", isDriverRecovery ? " (driver recovery)" : " (standby resume)");

    // Only record driver recovery for actual driver/TDR events, not standby resume.
    if (isDriverRecovery) {
        record_driver_recovery();

        if (count_recent_driver_recoveries() >= MAX_RECOVERIES_BEFORE_BACKOFF) {
            debug_log("oc_persistence: loop detected (%u recoveries in %u ms), blocking re-apply\n",
                count_recent_driver_recoveries(), (unsigned int)RECOVERY_LOOP_WINDOW_MS);
            return;
        }
    }

    // Read live settings via NVML to see if driver reset happened.
    // Check GPU offset, memory offset, and power limit to determine whether
    // settings survived. This avoids false "intact" results from a single field.
    bool settingsLost = false;
    char lostDetail[256] = {};
    auto append_lost = [&](const char* fmt, ...) {
        char part[128] = {};
        va_list ap;
        va_start(ap, fmt);
        StringCchVPrintfA(part, ARRAY_COUNT(part), fmt, ap);
        va_end(ap);
        if (!part[0]) return;
        if (lostDetail[0]) StringCchCatA(lostDetail, ARRAY_COUNT(lostDetail), "; ");
        StringCchCatA(lostDetail, ARRAY_COUNT(lostDetail), part);
    };

    int gpuOffsetMHz = 0;
    int memOffsetMHz = 0;

    // 1. Check GPU clock offset
    if (g_nvml_api.getGpcClkVfOffset) {
        nvmlReturn_t r = g_nvml_api.getGpcClkVfOffset(g_app.nvmlDevice, &gpuOffsetMHz);
        if (r != NVML_SUCCESS) {
            debug_log("oc_persistence: getGpcClkVfOffset failed (result=%d), skipping reapply\n", (int)r);
            return;
        }
    } else {
        debug_log("oc_persistence: getGpcClkVfOffset not available, cannot check persistence\n");
        return;
    }

    int desiredGpu = g_serviceActiveDesired.hasGpuOffset ? g_serviceActiveDesired.gpuOffsetMHz : gpuOffsetMHz;
    if (abs(gpuOffsetMHz - desiredGpu) > 5) {
        settingsLost = true;
        append_lost("GPU offset live=%d desired=%d", gpuOffsetMHz, desiredGpu);
    }

    // 2. Check memory clock offset
    if (g_nvml_api.getMemClkVfOffset && g_serviceActiveDesired.hasMemOffset) {
        nvmlReturn_t r = g_nvml_api.getMemClkVfOffset(g_app.nvmlDevice, &memOffsetMHz);
        if (r == NVML_SUCCESS) {
            int desiredMem = g_serviceActiveDesired.memOffsetMHz;
            if (abs(memOffsetMHz - desiredMem) > 5) {
                settingsLost = true;
                append_lost("mem offset live=%d desired=%d", memOffsetMHz, desiredMem);
            }
        }
    }

    // 3. Check power limit via NVML
    if (g_nvml_api.getPowerLimit && g_nvml_api.getPowerDefaultLimit && g_serviceActiveDesired.hasPowerLimit) {
        unsigned int curMw = 0, defMw = 0;
        if (g_nvml_api.getPowerLimit(g_app.nvmlDevice, &curMw) == NVML_SUCCESS &&
            g_nvml_api.getPowerDefaultLimit(g_app.nvmlDevice, &defMw) == NVML_SUCCESS && defMw > 0) {
            int livePct = (int)((curMw * 100ULL + defMw / 2) / defMw);
            int desiredPct = g_serviceActiveDesired.powerLimitPct;
            if (abs(livePct - desiredPct) > 2) {
                settingsLost = true;
                append_lost("power limit live=%d%% desired=%d%%", livePct, desiredPct);
            }
        }
    }

    if (!settingsLost) {
        debug_log("oc_persistence: all settings intact (gpu=%d mem=%d power=%d), no re-apply\n",
            gpuOffsetMHz, memOffsetMHz, g_app.powerLimitPct);
        return;
    }

    debug_log("oc_persistence: settings lost: %s, re-applying with reset\n", lostDetail);

    char result[256] = {};
    DesiredSettings desired = g_serviceActiveDesired;
    desired.resetOcBeforeApply = true;
    if (!service_reapply_desired_preserving_intent(
            &desired,
            isDriverRecovery ? "driver recovery persistence check" : "standby resume persistence check",
            result,
            sizeof(result))) {
        debug_log("oc_persistence: re-apply failed: %s\n", result[0] ? result : "unknown");
        if (isDriverRecovery) {
            service_queue_recovery_reapply("OC persistence failure", SERVICE_RECOVERY_REAPPLY_RETRY_INTERVAL_MS);
        }
    } else {
        debug_log("oc_persistence: re-apply successful\n");
    }
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

static bool get_pipe_client_identity(HANDLE pipe, char* userOut, size_t userOutSize, DWORD* sessionIdOut, DWORD* pidOut, bool* isAdminOut, char* err, size_t errSize) {
    if (isAdminOut) *isAdminOut = false;
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
    if (ok && isAdminOut) *isAdminOut = token_is_local_admin(token);
    CloseHandle(token);
    CloseHandle(process);
    if (!ok) {
        set_message(err, errSize, "Failed resolving service client identity");
        return false;
    }
    return true;
}

static bool service_caller_is_authorized(HANDLE pipe, const char* source, char* err, size_t errSize, char* callerUserOut, size_t callerUserOutSize, DWORD* callerSessionIdOut, DWORD* callerPidOut, bool* callerIsAdminOut) {
    if (callerIsAdminOut) *callerIsAdminOut = false;
    DWORD callerSessionId = (DWORD)-1;
    DWORD callerPid = 0;
    bool callerIsAdmin = false;
    char callerUser[256] = {};
    if (!get_pipe_client_identity(pipe, callerUser, sizeof(callerUser), &callerSessionId, &callerPid, &callerIsAdmin, err, errSize)) {
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
    if (callerIsAdminOut) *callerIsAdminOut = callerIsAdmin;
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
    // If within the NVML crash recovery window, skip hardware_initialize()
    // entirely.  NVML reads (VF curve, temperature, etc.) access-violate on
    // the transitional driver after a device reconnect, killing this pipe
    // server thread and abandoning g_serviceRuntimeLock.  Return true without
    // refreshing — the caller populates the snapshot from cached state.
    if (nvml_crash_recovery_active()) {
        debug_log("service telemetry: crash recovery active, skipping hardware_initialize\n");
        if (detail && detailSize > 0) detail[0] = 0;
        return true;
    }

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
            // Don't call service_runtime_pulse() from the pipe server
            // telemetry handler while recovering from a recent NVML crash —
            // doing so triggers the recovery-reapply path, which calls
            // NVML/NVAPI writes that can access-violate when the GPU kernel
            // driver is still in a transitional state after a device
            // reconnect.  The VEH kills the pipe server thread, breaking the
            // GUI connection with ERROR_BROKEN_PIPE (109).  The fan runtime
            // thread handles recovery independently.  Uses the shared recovery
            // window so the guard matches the fan thread's crash back-off.
            bool recentCrash = nvml_crash_recovery_active();
            if ((noRuntimeThread || noCache) && !recentCrash) {
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
    // GPU driver restart recovery (restart64.exe / TDR / driver upgrade).
    //
    // When the WDDM driver restarts, the already-loaded nvml.dll/nvapi64.dll
    // retain broken internal state bound to the dead driver instance.  We use
    // IN-PROCESS recovery: close stale handles (FreeLibrary without shutdown),
    // re-init NVML/NvAPI with fresh module instances, and reapply settings.
    // No process restart.
    LONG crashCount = g_nvmlCrashCount;
    bool deviceWasRemoved = false;
    bool pendingRecovery = false;
    EnterCriticalSection(&g_appLock);
    if (g_app.deviceRemoved) deviceWasRemoved = true;
    if (g_app.pendingDeviceRecovery) pendingRecovery = true;
    LeaveCriticalSection(&g_appLock);

    if (deviceWasRemoved) {
        // Device physically gone — NVML is unsafe; wait for arrival.  Skip the
        // fan tick entirely (it would only read stale/zero data).
        if (logPulse) {
            debug_log("service_runtime_pulse: device removed, awaiting arrival (NVML idle)\n");
        }
        return;
    }

    if (crashCount > 0 || pendingRecovery) {
        // Launch in-process recovery, or skip if one just completed.
        //
        // Cooldown: if recovery recently completed (within the crash recovery
        // window), don't re-launch recovery.  The crashCount was restored by
        // recovery (see Phase E above) as a safety measure: it keeps
        // nvml_crash_recovery_active() returning true so the telemetry handler
        // skips service_runtime_pulse().  Let the window expire naturally.
        ULONGLONG lastRecoveryMs = g_serviceLastRecoveryTickMs;
        if (lastRecoveryMs != 0 && (GetTickCount64() - lastRecoveryMs) < NVML_CRASH_RECOVERY_WINDOW_MS) {
            if (logPulse || crashCount > 0) {
                debug_log("service_runtime_pulse: cooling down after recovery (%llums ago), skipping pulse\n",
                    GetTickCount64() - lastRecoveryMs);
            }
            return;
        }

        // Cooldown expired.  Distinguish between:
        //   1. Safety restore from Phase E (crashTickMs <= lastRecoveryMs)
        //      → clear flags and fall through to the healthy path.  If the
        //        recovery reapply failed transiently, the main-loop retry
        //        worker preserves and reapplies the active desired settings.
        //   2. Genuine new VEH crash (crashTickMs > lastRecoveryMs)
        //      → this is a real crash after recovery, launch a new recovery.
        if (lastRecoveryMs != 0 && g_nvmlCrashTickMs != 0 &&
            g_nvmlCrashTickMs <= lastRecoveryMs) {
            // RC7: if the reapply is still in progress, do NOT clear crash
            // flags yet — the fan pulse would immediately touch NVML (fan tick)
            // and trigger another VEH crash before the reapply can finish.
            // Keep the fan pulse on cooldown until the reapply thread either
            // completes or exhausts its retries.
            bool reapplyActive = InterlockedExchangeAdd(&g_serviceReapplyInProgress, 0) != 0
                || InterlockedExchangeAdd(&g_serviceRecoveryReapplyPending, 0) != 0;
            if (reapplyActive) {
                if (logPulse) {
                    debug_log("service_runtime_pulse: safety window expired but reapply still active,"
                        " extending cooldown until reapply completes\n");
                }
                // RC7: DON'T touch g_nvmlCrashCount here — setting it to 1
                // would trigger the main loop's recovery monitor to launch a
                // NEW recovery cycle (which detects crashCount >= 1 and starts
                // Phase A).  Instead, just extend the cooldown timer so the
                // fan pulse stays in the cooldown path (checks
                // g_serviceLastRecoveryTickMs) without starting a new recovery.
                g_serviceLastRecoveryTickMs = now;
                return;
            }
            debug_log("service_runtime_pulse: safety window expired, resuming normal operation\n"
                "  (crashCount=%ld crashTickMs=%llu lastRecoveryMs=%llu)\n",
                (long)crashCount,
                (unsigned long long)g_nvmlCrashTickMs,
                (unsigned long long)lastRecoveryMs);
            g_nvmlCrashCount = 0;
            g_nvmlCrashTickMs = 0;
            g_serviceLastRecoveryTickMs = 0;
            // Fall through to apply_fan_curve_tick()
        } else {
            if (logPulse || crashCount > 0) {
                debug_log("service_runtime_pulse: new VEH crash detected, "
                    "launching in-process recovery (crashCount=%ld)\n", (long)crashCount);
            }
            launch_recovery_thread();
            return;
        }
    }

    // RC7: if the reapply thread is in progress and we reach here (crashCount
    // was cleared by the main loop's safety window expiry handler), skip the
    // fan tick to avoid triggering a new VEH crash on the transitional driver.
    // The reapply thread handles the writes; the fan pulse resumes after it
    // completes or exhausts retries.
    if (InterlockedExchangeAdd(&g_serviceReapplyInProgress, 0) != 0) {
        if (logPulse) {
            debug_log("service_runtime_pulse: reapply active, skipping fan tick\n");
        }
        return;
    }

    // Healthy path: drive the fan curve / fixed runtime.  If the GPU driver
    // restarts during this call, nvml_read_temperature() access-violates, the
    // VEH bumps g_nvmlCrashCount and ExitThreads us; the watchdog recreates the
    // thread and the next pulse takes the restart branch above.
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
            // Heartbeat: stamp BEFORE acquiring the lock / touching NVML so the
            // main-loop watchdog can detect a wedge inside nvml.dll (a hang the
            // VEH cannot catch) and launch in-process recovery.
            g_serviceFanPulseHeartbeatMs = GetTickCount64();
            InterlockedExchange(&g_serviceFanPulseInFlight, 1);
            lock_service_runtime();
            service_runtime_pulse();
            unlock_service_runtime();
            InterlockedExchange(&g_serviceFanPulseInFlight, 0);
            g_serviceFanPulseHeartbeatMs = GetTickCount64();
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
    g_fanRuntimeThreadId = threadId;
    debug_log("ensure_service_fan_runtime_thread: created=%d threadId=%lu\n", g_serviceFanThread ? 1 : 0, threadId);
    return g_serviceFanThread != nullptr;
}

static void stop_service_fan_runtime_thread() {
    if (!g_serviceFanThread) return;
    if (g_serviceFanStopEvent) SetEvent(g_serviceFanStopEvent);
    bool lockHeld = service_runtime_lock_held_by_current_thread();
    if (lockHeld) {
        unlock_service_runtime();
    }
    DWORD waitResult = WaitForSingleObject(g_serviceFanThread, SERVICE_FAN_THREAD_STOP_TIMEOUT_MS);
    if (waitResult != WAIT_OBJECT_0) {
        debug_log("stop_service_fan_runtime_thread: timed out waiting for fan thread (result=%lu); thread handle preserved to prevent replacement\n", waitResult);
        if (lockHeld) {
            lock_service_runtime();
        }
        // Keep g_serviceFanThread handle alive to prevent a new thread from starting
        // while the original may still reference shared events or runtime state.
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
        // Update the active desired state BEFORE capturing the control state
        // so that current_applied_gpu_offset_mhz() (called by populate_control_state)
        // sees the new desired values rather than stale previous-profile values.
        // Otherwise a profile 2 with hasGpuOffset=true/gpuOffsetMHz=0 would fall
        // back to the previous profile's selective offset (e.g. 475/60) because
        // g_serviceActiveDesired had not been updated yet and tail points with
        // non-zero flatten offsets satisfied live_curve_has_any_nonzero_offsets().
        DesiredSettings mergedActiveDesired = {};
        if (g_serviceHasActiveDesired) {
            mergedActiveDesired = g_serviceActiveDesired;
        } else {
            initialize_desired_settings_defaults(&mergedActiveDesired);
        }
        bool replaceOcCurveIntent = desired_updates_curve_or_gpu_offset_state(desired);
        if (replaceOcCurveIntent) {
            for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                mergedActiveDesired.hasCurvePoint[ci] = false;
                mergedActiveDesired.curvePointMHz[ci] = 0;
            }
            mergedActiveDesired.hasLock = false;
            mergedActiveDesired.lockCi = -1;
            mergedActiveDesired.lockMHz = 0;
            mergedActiveDesired.lockTracksAnchor = true;
        }
        merge_desired_settings(&mergedActiveDesired, desired);
        if (replaceOcCurveIntent && desired->hasLock) {
            mergedActiveDesired.hasLock = true;
            mergedActiveDesired.lockCi = desired->lockCi;
            mergedActiveDesired.lockMHz = desired->lockMHz;
            mergedActiveDesired.lockMode = desired->lockMode;
            mergedActiveDesired.lockTracksAnchor = desired->lockTracksAnchor;
        }
        if (desired_is_fan_only_apply_request(desired) && g_serviceHasActiveDesired) {
            debug_log("service apply: merged fan-only request into active desired, preserving lockCi=%d lockMHz=%u curvePoints=%d\n",
                mergedActiveDesired.hasLock ? mergedActiveDesired.lockCi : -1,
                mergedActiveDesired.hasLock ? mergedActiveDesired.lockMHz : 0u,
                desired_curve_point_count(&mergedActiveDesired));
        }
        g_serviceActiveDesired = mergedActiveDesired;
        g_serviceActiveDesired.resetOcBeforeApply = false;
        g_serviceHasActiveDesired = true;
        InterlockedExchange(&g_serviceRecoveryReapplyPending, 0);
        InterlockedExchange(&g_serviceReapplyInProgress, 0); // RC7: reapply confirmed by successful manual apply
        g_serviceRecoveryReapplyAttempts = 0;
        g_serviceRecoveryReapplyNextTickMs = 0;
        service_clear_restart_reapply_snapshot();
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
        mark_service_telemetry_cache_updated("service apply");
        if (g_serviceActiveDesired.hasLock) {
            debug_log("service apply: preserving requested lock intent ci=%d mhz=%u after live readback\n",
                g_serviceActiveDesired.lockCi,
                g_serviceActiveDesired.lockMHz);
        }
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
        // RC3 fix: do NOT clear g_serviceHasActiveDesired / g_serviceActiveDesired
        // on transient apply failure.  Previously, a single failed apply (e.g.
        // mid-recovery) wiped the active desired, so the next recovery had
        // nothing to reapply and the service was stuck in "crash recovery
        // active, skipping hardware_initialize" forever.  Preserving the
        // active desired lets the next recovery reapply the same settings.
        // The disk snapshot is also kept up to date so a service restart
        // mid-recovery can still restore the previous profile.
        debug_log("service apply: apply FAILED, preserving active desired (hasLock=%d lockCi=%d)\n",
            g_serviceActiveDesired.hasLock ? 1 : 0,
            g_serviceActiveDesired.hasLock ? g_serviceActiveDesired.lockCi : -1);
        service_write_restart_reapply_snapshot();
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

    // Reset NVML locked clocks (hard lock)
    if (g_nvml_api.resetGpuLockedClocks) {
        if (nvml_ensure_ready()) {
            nvmlReturn_t r = g_nvml_api.resetGpuLockedClocks(g_app.nvmlDevice);
            if (r == NVML_SUCCESS) {
                successCount++;
                debug_log("service_reset_all: resetGpuLockedClocks ok\n");
            } else {
                // Not a failure if no lock was active
                debug_log("service_reset_all: resetGpuLockedClocks → %s (may be benign)\n", nvml_err_name(r));
            }
        }
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
        InterlockedExchange(&g_serviceRecoveryReapplyPending, 0);
        InterlockedExchange(&g_serviceReapplyInProgress, 0);
        g_serviceRecoveryReapplyAttempts = 0;
        g_serviceRecoveryReapplyNextTickMs = 0;
        service_clear_restart_reapply_snapshot();
        clear_service_authoritative_state();
        populate_control_state(&g_serviceControlState);
        g_serviceControlStateValid = true;
        set_message(result, resultSize, "Reset applied.");
        return true;
    }
    // RC3 fix: on partial reset failure, keep the on-disk snapshot so the
    // next recovery (or service restart) can restore the previous profile.
    // The in-memory g_serviceActiveDesired was already cleared above — that
    // is correct for reset, which is a "stop applying anything" operation;
    // but the disk snapshot is the safety net for a service restart after a
    // partial reset.
    service_write_restart_reapply_snapshot();
    populate_control_state(&g_serviceControlState);
    g_serviceControlStateValid = true;
    mark_service_telemetry_cache_updated("service reset");
    set_message(result, resultSize, "Reset applied %d OK, %d failed: %s", successCount, failCount, failureDetails[0] ? failureDetails : "one or more reset steps failed");
    return false;
}

static bool service_session_logon_resolve_and_load_profile(DWORD sessionId, DesiredSettings* desired, int* slotOut, bool* usedMachineDefault) {
    if (!desired || !slotOut || !usedMachineDefault) return false;
    *slotOut = 0;
    *usedMachineDefault = false;
    char pathErr[256] = {};
    if (!resolve_service_user_data_paths(sessionId, pathErr, sizeof(pathErr))) {
        debug_log("session logon: failed resolving user paths for session %lu: %s\n",
            (unsigned long)sessionId, pathErr[0] ? pathErr : "unknown");
        return false;
    }
    if (!g_app.configPath[0]) {
        set_default_config_path();
    }
    char userConfigPath[MAX_PATH] = {};
    StringCchCopyA(userConfigPath, ARRAY_COUNT(userConfigPath), g_app.configPath);

    char machinePath[MAX_PATH] = {};
    bool haveMachinePath = resolve_machine_config_path(machinePath, sizeof(machinePath));

    // Gather the inputs for the pure logon-source policy decision.
    int userSlot = get_config_int(userConfigPath, "profiles", "logon_slot", 0);
    if (userSlot < 0 || userSlot > CONFIG_NUM_SLOTS) userSlot = 0;
    bool hasPerUserSlot = (userSlot > 0 && is_profile_slot_saved(userConfigPath, userSlot));

    // Per-user "apply admin shared bank slot N at my logon" choice.
    int sharedSlot = get_config_int(userConfigPath, "profiles", "logon_shared_slot", 0);
    if (sharedSlot < 0 || sharedSlot > CONFIG_NUM_SLOTS) sharedSlot = 0;
    bool bankSlotSaved = haveMachinePath && sharedSlot > 0 && is_profile_slot_saved(machinePath, sharedSlot);

    int machineSlot = 0;
    bool hasMachineDefault = get_machine_logon_slot(&machineSlot) && machineSlot > 0 &&
                             haveMachinePath && is_profile_slot_saved(machinePath, machineSlot);

    bool policyActive = false;
    get_machine_restrict_policy(&policyActive);
    bool isAdmin = service_session_user_is_local_admin(sessionId);

    LogonProfileSource src = resolve_logon_profile_source(policyActive, isAdmin, sharedSlot,
        bankSlotSaved, hasPerUserSlot, hasMachineDefault);
    debug_log("session logon resolve: session=%lu policy=%d admin=%d sharedSlot=%d(bank=%d) perUserSlot=%d(saved=%d) machineDefault=%d(have=%d) -> source=%d\n",
        (unsigned long)sessionId, policyActive ? 1 : 0, isAdmin ? 1 : 0,
        sharedSlot, bankSlotSaved ? 1 : 0, userSlot, hasPerUserSlot ? 1 : 0,
        machineSlot, hasMachineDefault ? 1 : 0, (int)src);

    char err[256] = {};
    switch (src) {
        case LOGON_PROFILE_SOURCE_SHARED_BANK:
            if (load_profile_from_config(machinePath, sharedSlot, desired, err, sizeof(err)) &&
                desired_settings_have_explicit_state(desired, true, err, sizeof(err))) {
                *slotOut = sharedSlot;
                *usedMachineDefault = true;  // authoritative bank copy, not per-user custom OC
                debug_log("session logon: applying user-chosen shared bank slot %d for session %lu\n",
                    sharedSlot, (unsigned long)sessionId);
                return true;
            }
            debug_log("session logon: user-chosen shared slot %d failed to load for session %lu: %s\n",
                sharedSlot, (unsigned long)sessionId, err[0] ? err : "unknown");
            return false;

        case LOGON_PROFILE_SOURCE_PER_USER:
            if (load_profile_from_config(userConfigPath, userSlot, desired, err, sizeof(err)) &&
                desired_settings_have_explicit_state(desired, true, err, sizeof(err))) {
                *slotOut = userSlot;
                debug_log("session logon: using per-user logon slot %d for session %lu\n",
                    userSlot, (unsigned long)sessionId);
                return true;
            }
            // Saved-but-corrupt per-user slot: fall back to the machine default
            // (legacy resilience).  Only reached for admin/unrestricted users.
            debug_log("session logon: per-user logon slot %d for session %lu is empty or invalid, checking machine default\n",
                userSlot, (unsigned long)sessionId);
            if (hasMachineDefault &&
                load_profile_from_config(machinePath, machineSlot, desired, err, sizeof(err)) &&
                desired_settings_have_explicit_state(desired, true, err, sizeof(err))) {
                *slotOut = machineSlot;
                *usedMachineDefault = true;
                debug_log("session logon: per-user invalid; using machine-wide default slot %d for session %lu\n",
                    machineSlot, (unsigned long)sessionId);
                return true;
            }
            return false;

        case LOGON_PROFILE_SOURCE_MACHINE_DEFAULT:
            if (load_profile_from_config(machinePath, machineSlot, desired, err, sizeof(err)) &&
                desired_settings_have_explicit_state(desired, true, err, sizeof(err))) {
                *slotOut = machineSlot;
                *usedMachineDefault = true;
                debug_log("session logon: using machine-wide default logon slot %d for session %lu%s\n",
                    machineSlot, (unsigned long)sessionId,
                    (policyActive && !isAdmin) ? " (shared-only policy)" : "");
                return true;
            }
            debug_log("session logon: machine default slot %d failed to load for session %lu: %s\n",
                machineSlot, (unsigned long)sessionId, err[0] ? err : "unknown");
            return false;

        case LOGON_PROFILE_SOURCE_NONE:
        default:
            debug_log("session logon: nothing to apply for session %lu (policy=%d admin=%d)\n",
                (unsigned long)sessionId, policyActive ? 1 : 0, isAdmin ? 1 : 0);
            return false;
    }
}

// service_session_logon_thread_proc() was removed: the active-user session
// router in main_service_sessions.cpp (service_handle_session_change) now owns
// logon/switch handling and calls service_apply_profile_for_session(), which
// shares the resolve+driver-wait+apply logic that used to live here.

