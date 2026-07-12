static bool ensure_directory_recursive_windows(const char* path, char* err, size_t errSize) {
    if (!path || !*path) return true;

    char temp[MAX_PATH] = {};
    StringCchCopyA(temp, ARRAY_COUNT(temp), path);
    trim_ascii(temp);
    size_t len = strlen(temp);
    while (len > 0 && (temp[len - 1] == '\\' || temp[len - 1] == '/')) {
        temp[--len] = 0;
    }
    if (len == 0) return true;

    for (char* p = temp; *p; ++p) {
        if (*p != '\\' && *p != '/') continue;
        if (p == temp) continue;
        if (*(p - 1) == ':') continue;
        char save = *p;
        *p = 0;
        if (temp[0]) {
            if (!CreateDirectoryA(temp, nullptr)) {
                DWORD e = GetLastError();
                if (e != ERROR_ALREADY_EXISTS) {
                    set_message(err, errSize, "Failed creating directory %s (error %lu)", temp, e);
                    return false;
                }
                DWORD attrs = GetFileAttributesA(temp);
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    set_message(err, errSize, "Directory %s is a reparse point, refusing to traverse", temp);
                    return false;
                }
            }
        }
        *p = save;
    }

    if (!CreateDirectoryA(temp, nullptr)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) {
            set_message(err, errSize, "Failed creating directory %s (error %lu)", temp, e);
            return false;
        }
    }
    return true;
}

static bool ensure_parent_directory_for_file(const char* path, char* err, size_t errSize) {
    if (!path || !*path) return false;
    char parent[MAX_PATH] = {};
    StringCchCopyA(parent, ARRAY_COUNT(parent), path);
    char* slash = strrchr(parent, '\\');
    if (!slash) slash = strrchr(parent, '/');
    if (!slash) return true;
    *slash = 0;
    return ensure_directory_recursive_windows(parent, err, errSize);
}

static bool get_known_folder_path_utf8(REFKNOWNFOLDERID folderId, char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    PWSTR wide = nullptr;
    HRESULT hr = SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &wide);
    if (FAILED(hr) || !wide) return false;
    bool ok = copy_wide_to_utf8(wide, out, (int)outSize);
    CoTaskMemFree(wide);
    return ok;
}

static const char* cli_log_path() {
    return g_cliLogPath[0] ? g_cliLogPath : APP_CLI_LOG_FILE;
}

// Resolve the machine-fixed per-process LocalAppData\Green Curve directory used
// for service-side artifacts (restart history, reapply snapshot, early debug
// log, crash dumps).  For the LocalSystem service this is the SYSTEM profile's
// LocalAppData, which is admin-only, consistent across restarts, and available
// before any interactive logon.  PROJECT POLICY: never place SENSITIVE program
// artifacts (crash dumps, service logs, restart/reapply state) under
// %ProgramData% — it is user-readable, which would disclose SYSTEM-process crash
// dumps.  %ProgramData% IS the correct home for deliberately-shared, non-
// sensitive config (the shared profile bank, shared-profiles.ini) protected with
// an explicit SYSTEM+Admins-Full / Users-Read DACL.  Crash-safe: stack buffers
// and environment variables only (callable from the vectored exception handler).
static bool resolve_service_machine_data_dir(char* out, size_t outSize) {
    if (!out || outSize == 0) return false;
    out[0] = 0;
    char base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, ARRAY_COUNT(base));
    if (n == 0 || n >= ARRAY_COUNT(base)) {
        // A LocalSystem service may not have %LOCALAPPDATA% in its environment;
        // construct the SYSTEM profile's LocalAppData from %SystemRoot%.
        char sysRoot[MAX_PATH] = {};
        DWORD m = GetEnvironmentVariableA("SystemRoot", sysRoot, ARRAY_COUNT(sysRoot));
        if (m == 0 || m >= ARRAY_COUNT(sysRoot)) {
            StringCchCopyA(sysRoot, ARRAY_COUNT(sysRoot), "C:\\Windows");
        }
        if (FAILED(StringCchPrintfA(base, ARRAY_COUNT(base),
                "%s\\System32\\config\\systemprofile\\AppData\\Local", sysRoot))) {
            return false;
        }
    }
    return SUCCEEDED(StringCchPrintfA(out, outSize, "%s\\Green Curve", base));
}

// Best-effort removal of legacy SENSITIVE artifacts older builds left in
// %ProgramData%\Green Curve (restart history, reapply snapshot, early debug log,
// and — most importantly — SYSTEM-process crash dumps that were world-readable
// there).  Clears any previously disclosed dumps.  The deliberately-shared
// shared-profiles.ini (MACHINE_CONFIG_FILE_NAME) is PRESERVED — it now lives
// here on purpose, protected by an explicit Users-Read DACL.  Only deletes files
// inside our own directory; never recurses.
static void service_cleanup_legacy_programdata() {
    char programData[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("ProgramData", programData, ARRAY_COUNT(programData));
    if (n == 0 || n >= ARRAY_COUNT(programData)) {
        StringCchCopyA(programData, ARRAY_COUNT(programData), "C:\\ProgramData");
    }
    char legacyDir[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(legacyDir, ARRAY_COUNT(legacyDir), "%s\\Green Curve", programData))) return;
    DWORD attrs = GetFileAttributesA(legacyDir);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) return;

    char pattern[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(pattern, ARRAY_COUNT(pattern), "%s\\*", legacyDir))) return;
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue; // skip ., .., subdirs
            // Preserve the deliberately-shared profile bank; only sweep sensitive
            // legacy artifacts (dumps/logs/restart state) out of this directory.
            if (lstrcmpiA(fd.cFileName, MACHINE_CONFIG_FILE_NAME) == 0) continue;
            char filePath[MAX_PATH] = {};
            if (SUCCEEDED(StringCchPrintfA(filePath, ARRAY_COUNT(filePath), "%s\\%s", legacyDir, fd.cFileName))) {
                DeleteFileA(filePath);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    // RemoveDirectory fails harmlessly if the shared bank file remains; only log
    // when the directory was actually empty and got removed.
    if (RemoveDirectoryA(legacyDir)) {
        debug_log("service: removed legacy ProgramData\\Green Curve directory (no shared bank present)\n");
    }
}

// F-REL-2: cap the number of VEH minidumps kept on disk.  A driver-recovery
// restart loop writes one greencurve_veh_*.dmp per crash cycle; without a cap a
// sustained loop (or a poison-pill) could write hundreds and fill the disk.
// Called once per fresh service process (which is once per restart cycle), so it
// bounds the dumps across an entire loop.  Deletes the lexicographically-oldest
// (the filename embeds a YYYYMMDD_HHMMSS timestamp) until at most maxKeep remain.
static void service_rotate_minidumps(unsigned int maxKeep) {
    char dir[MAX_PATH] = {};
    if (!resolve_service_machine_data_dir(dir, sizeof(dir))) return;
    char pattern[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(pattern, ARRAY_COUNT(pattern), "%s\\greencurve_veh_*.dmp", dir))) return;
    for (;;) {
        WIN32_FIND_DATAA fd = {};
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        unsigned int count = 0;
        char oldest[MAX_PATH] = {};
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            count++;
            if (oldest[0] == 0 || lstrcmpA(fd.cFileName, oldest) < 0) {
                StringCchCopyA(oldest, ARRAY_COUNT(oldest), fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        if (count <= maxKeep || oldest[0] == 0) return;
        char victim[MAX_PATH] = {};
        if (FAILED(StringCchPrintfA(victim, ARRAY_COUNT(victim), "%s\\%s", dir, oldest))) return;
        if (!DeleteFileA(victim)) return; // stop on failure to avoid an infinite loop
    }
}

static bool resolve_data_paths(char* err, size_t errSize) {
    if (g_userDataDir[0] && g_cliLogPath[0] && g_debugLogPath[0] && g_jsonPath[0] && g_errorLogPath[0]) {
        return true;
    }

    char localAppData[MAX_PATH] = {};
    if (!get_known_folder_path_utf8(FOLDERID_LocalAppData, localAppData, sizeof(localAppData))) {
        set_message(err, errSize, "Failed resolving LocalAppData");
        return false;
    }
    if (FAILED(StringCchPrintfA(g_userDataDir, ARRAY_COUNT(g_userDataDir), "%s\\Green Curve", localAppData)) ||
        FAILED(StringCchPrintfA(g_cliLogPath, ARRAY_COUNT(g_cliLogPath), "%s\\%s", g_userDataDir, APP_CLI_LOG_FILE)) ||
        FAILED(StringCchPrintfA(g_debugLogPath, ARRAY_COUNT(g_debugLogPath), "%s\\%s", g_userDataDir, APP_DEBUG_LOG_FILE)) ||
        FAILED(StringCchPrintfA(g_jsonPath, ARRAY_COUNT(g_jsonPath), "%s\\%s", g_userDataDir, APP_JSON_FILE)) ||
        FAILED(StringCchPrintfA(g_errorLogPath, ARRAY_COUNT(g_errorLogPath), "%s\\%s", g_userDataDir, APP_LOG_FILE))) {
        set_message(err, errSize, "Resolved storage paths are too long");
        return false;
    }

    if (!ensure_directory_recursive_windows(g_userDataDir, err, errSize)) return false;
    return true;
}

static bool service_sid_string_from_token(HANDLE token, char* sidOut, size_t sidOutSize, char* err, size_t errSize) {
    if (!sidOut || sidOutSize == 0) return false;
    sidOut[0] = 0;
    if (!token) {
        set_message(err, errSize, "Missing user token");
        return false;
    }

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        set_message(err, errSize, "TokenUser size query failed");
        return false;
    }
    BYTE stackBuf[512] = {};
    void* buf = needed <= sizeof(stackBuf) ? stackBuf : malloc(needed);
    if (!buf) {
        set_message(err, errSize, "TokenUser allocation failed");
        return false;
    }
    DWORD actualNeeded = needed;
    bool ok = GetTokenInformation(token, TokenUser, buf, needed, &actualNeeded) != FALSE;
    if (!ok) {
        set_message(err, errSize, "TokenUser query failed");
        if (buf != stackBuf) free(buf);
        return false;
    }
    TOKEN_USER* user = (TOKEN_USER*)buf;
    LPSTR sidText = nullptr;
    ok = ConvertSidToStringSidA(user->User.Sid, &sidText) != FALSE && sidText && sidText[0];
    if (ok) {
        ok = SUCCEEDED(StringCchCopyA(sidOut, sidOutSize, sidText));
        if (!ok) set_message(err, errSize, "User SID string is too long");
    } else {
        set_message(err, errSize, "User SID conversion failed");
    }
    if (sidText) LocalFree(sidText);
    if (buf != stackBuf) free(buf);
    return ok;
}

static bool service_resolve_session_user_sid(DWORD sessionId, char* sidOut, size_t sidOutSize, char* err, size_t errSize) {
    if (!sidOut || sidOutSize == 0) return false;
    sidOut[0] = 0;
    HANDLE token = nullptr;
    if (!WTSQueryUserToken(sessionId, &token)) {
        set_message(err, errSize, "WTSQueryUserToken failed (error %lu)", GetLastError());
        return false;
    }
    bool ok = service_sid_string_from_token(token, sidOut, sidOutSize, err, errSize);
    CloseHandle(token);
    return ok;
}

static bool service_identity_from_token(
    HANDLE token,
    DWORD sessionId,
    ServiceLifecycleIdentity* identityOut,
    char* err,
    size_t errSize)
{
    if (!identityOut) return false;
    memset(identityOut, 0, sizeof(*identityOut));
    if (!service_sid_string_from_token(token, identityOut->sid,
            ARRAY_COUNT(identityOut->sid), err, errSize)) {
        return false;
    }
    TOKEN_STATISTICS statistics = {};
    DWORD returned = 0;
    if (!GetTokenInformation(token, TokenStatistics, &statistics,
            sizeof(statistics), &returned)) {
        set_message(err, errSize, "TokenStatistics query failed (error %lu)", GetLastError());
        memset(identityOut, 0, sizeof(*identityOut));
        return false;
    }
    identityOut->sessionId = sessionId;
    identityOut->authenticationId =
        ((gc_u64)(gc_u32)statistics.AuthenticationId.HighPart << 32) |
        (gc_u64)statistics.AuthenticationId.LowPart;
    identityOut->valid = true;
    return true;
}

static bool service_resolve_session_identity(
    DWORD sessionId,
    ServiceLifecycleIdentity* identityOut,
    char* err,
    size_t errSize)
{
    if (!identityOut) return false;
    memset(identityOut, 0, sizeof(*identityOut));
    HANDLE token = nullptr;
    if (!WTSQueryUserToken(sessionId, &token)) {
        set_message(err, errSize, "WTSQueryUserToken failed (error %lu)", GetLastError());
        return false;
    }
    bool ok = service_identity_from_token(token, sessionId, identityOut, err, errSize);
    CloseHandle(token);
    return ok;
}

static void clear_service_user_data_path_cache() {
    close_debug_log_file();
    g_userDataDir[0] = 0;
    g_cliLogPath[0] = 0;
    g_debugLogPath[0] = 0;
    g_jsonPath[0] = 0;
    g_errorLogPath[0] = 0;
    g_serviceUserProfileDir[0] = 0;
    g_app.configPath[0] = 0;
    g_serviceUserPathsResolved = false;
    g_serviceUserPathsSessionId = (DWORD)-1;
    g_serviceUserPathsSid[0] = 0;
}

static bool service_user_paths_identity_matches(DWORD sessionId) {
    if (!g_serviceUserPathsResolved || g_serviceUserPathsSessionId != sessionId || !g_serviceUserPathsSid[0]) {
        return false;
    }
    char sid[184] = {};
    char err[128] = {};
    if (!service_resolve_session_user_sid(sessionId, sid, sizeof(sid), err, sizeof(err))) {
        debug_log("service user paths: failed to verify cached session identity for session %lu: %s\n",
            (unsigned long)sessionId, err[0] ? err : "unknown");
        return false;
    }
    return strcmp(g_serviceUserPathsSid, sid) == 0;
}

static bool resolve_service_user_data_paths(DWORD sessionId, char* err, size_t errSize) {
    if (service_user_paths_identity_matches(sessionId)) {
        return true;
    }
    HANDLE hToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &hToken)) {
        set_message(err, errSize, "WTSQueryUserToken failed (error %lu)", GetLastError());
        return false;
    }
    char userSid[184] = {};
    if (!service_sid_string_from_token(hToken, userSid, sizeof(userSid), err, errSize)) {
        CloseHandle(hToken);
        return false;
    }
    if (g_serviceUserPathsResolved) {
        bool sameIdentity = g_serviceUserPathsSessionId == sessionId &&
            g_serviceUserPathsSid[0] &&
            strcmp(g_serviceUserPathsSid, userSid) == 0;
        if (sameIdentity) {
            CloseHandle(hToken);
            return true;
        }
        debug_log("service user paths: identity changed, clearing cached paths (oldSession=%lu newSession=%lu oldSid=%s newSid=%s)\n",
            (unsigned long)g_serviceUserPathsSessionId,
            (unsigned long)sessionId,
            g_serviceUserPathsSid[0] ? g_serviceUserPathsSid : "<none>",
            userSid[0] ? userSid : "<none>");
        clear_service_user_data_path_cache();
    }
    WCHAR profileDirW[MAX_PATH] = {};
    DWORD profileSize = ARRAY_COUNT(profileDirW);
    if (!GetUserProfileDirectoryW(hToken, profileDirW, &profileSize)) {
        set_message(err, errSize, "GetUserProfileDirectoryW failed");
        CloseHandle(hToken);
        return false;
    }
    char profileDir[MAX_PATH] = {};
    if (!copy_wide_to_utf8(profileDirW, profileDir, ARRAY_COUNT(profileDir))) {
        set_message(err, errSize, "Profile path conversion failed");
        CloseHandle(hToken);
        return false;
    }
    StringCchCopyA(g_serviceUserProfileDir, ARRAY_COUNT(g_serviceUserProfileDir), profileDir);

    // Resolve the user's LocalAppData using the USER'S token, not the service's
    // own (SYSTEM) token.  SHGetKnownFolderPath with the user token honors
    // redirected/roaming LocalAppData (folder redirection policies), which the
    // naive "<profile>\AppData\Local" string concatenation below does not.  The
    // concatenation is kept as a fallback for the rare case where the known
    // folder cannot be resolved for the token.
    char localAppData[MAX_PATH] = {};
    PWSTR localAppDataW = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, hToken, &localAppDataW)) && localAppDataW) {
        copy_wide_to_utf8(localAppDataW, localAppData, ARRAY_COUNT(localAppData));
        CoTaskMemFree(localAppDataW);
        localAppDataW = nullptr;
    }
    CloseHandle(hToken);
    if (!localAppData[0]) {
        if (FAILED(StringCchPrintfA(localAppData, ARRAY_COUNT(localAppData), "%s\\AppData\\Local", profileDir))) {
            set_message(err, errSize, "Profile path too long");
            return false;
        }
    }
    if (FAILED(StringCchPrintfA(g_userDataDir, ARRAY_COUNT(g_userDataDir), "%s\\Green Curve", localAppData)) ||
        FAILED(StringCchPrintfA(g_cliLogPath, ARRAY_COUNT(g_cliLogPath), "%s\\%s", g_userDataDir, APP_CLI_LOG_FILE)) ||
        FAILED(StringCchPrintfA(g_debugLogPath, ARRAY_COUNT(g_debugLogPath), "%s\\%s", g_userDataDir, APP_DEBUG_LOG_FILE)) ||
        FAILED(StringCchPrintfA(g_jsonPath, ARRAY_COUNT(g_jsonPath), "%s\\%s", g_userDataDir, APP_JSON_FILE)) ||
        FAILED(StringCchPrintfA(g_errorLogPath, ARRAY_COUNT(g_errorLogPath), "%s\\%s", g_userDataDir, APP_LOG_FILE))) {
        set_message(err, errSize, "Resolved storage paths are too long");
        return false;
    }
    if (!ensure_directory_recursive_windows(g_userDataDir, err, errSize)) {
        return false;
    }
    g_serviceUserPathsResolved = true;
    g_serviceUserPathsSessionId = sessionId;
    StringCchCopyA(g_serviceUserPathsSid, ARRAY_COUNT(g_serviceUserPathsSid), userSid);
    return true;
}

static bool service_debug_env_override(bool* enabledOut) {
    if (enabledOut) *enabledOut = false;
    char value[32] = {};
    DWORD n = GetEnvironmentVariableA(APP_DEBUG_ENV, value, ARRAY_COUNT(value));
    if (n == 0) return false;
    value[ARRAY_COUNT(value) - 1] = 0;
    trim_ascii(value);
    bool enabled = true;
    if (n >= ARRAY_COUNT(value) || value[0] == 0) {
        enabled = true;
    } else if (value[0] == '0' && value[1] == 0) {
        enabled = false;
    } else if ((value[0] == 'f' || value[0] == 'F') &&
        (value[1] == 'a' || value[1] == 'A') &&
        (value[2] == 'l' || value[2] == 'L') &&
        (value[3] == 's' || value[3] == 'S') &&
        (value[4] == 'e' || value[4] == 'E') &&
        value[5] == 0) {
        enabled = false;
    }
    if (enabledOut) *enabledOut = enabled;
    return true;
}

static bool service_initial_debug_logging_enabled() {
    bool envEnabled = false;
    if (service_debug_env_override(&envEnabled)) return envEnabled;
    return SERVICE_DEBUG_DEFAULT_ENABLED != 0;
}

static bool service_config_debug_logging_enabled(int* envValueOut, int* configValueOut) {
    bool envEnabled = false;
    bool haveEnv = service_debug_env_override(&envEnabled);
    int configDebug = g_app.configPath[0]
        ? get_config_int(g_app.configPath, "debug", "enabled", SERVICE_DEBUG_DEFAULT_ENABLED)
        : SERVICE_DEBUG_DEFAULT_ENABLED;
    if (envValueOut) *envValueOut = haveEnv ? (envEnabled ? 1 : 0) : -1;
    if (configValueOut) *configValueOut = configDebug;
    return haveEnv ? envEnabled : (configDebug != 0);
}

static bool config_file_exists() {
    if (!g_app.configPath[0]) return false;
    DWORD attrs = GetFileAttributesA(g_app.configPath);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void clear_service_authoritative_state() {
    g_app.serviceSnapshotAuthoritative = false;
    g_app.serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_NONE;
    g_app.serviceActiveProfileSlot = 0;
    g_app.serviceLastLifecycleTrigger = SERVICE_LIFECYCLE_TRIGGER_NONE;
    g_app.serviceLastLifecycleResult = SERVICE_LIFECYCLE_RESULT_NONE;
    g_app.serviceAutoRestoreLockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    g_app.serviceActiveDesiredValid = false;
    memset(&g_app.serviceActiveDesired, 0, sizeof(g_app.serviceActiveDesired));
    g_app.serviceControlStateValid = false;
    memset(&g_app.serviceControlState, 0, sizeof(g_app.serviceControlState));
    g_serviceControlStateValid = false;
    memset(&g_serviceControlState, 0, sizeof(g_serviceControlState));
    g_serviceTelemetryLastHardwarePollTickMs = 0;
    g_serviceTelemetryLastPollSource[0] = 0;
#ifndef GREEN_CURVE_SERVICE_BINARY
    sync_applied_profile_from_service_metadata();
#endif
}

static int format_log_timestamp_prefix(char* out, size_t outSize) {
    if (!out || outSize == 0) return 0;
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    HRESULT hr = StringCchPrintfA(out, outSize,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds);
    if (FAILED(hr)) {
        out[0] = 0;
        return 0;
    }
    return (int)strlen(out);
}


static void set_default_config_path() {
    if (g_app.configPath[0]) return;

    char err[256] = {};
    if (!resolve_data_paths(err, sizeof(err))) {
        // FAIL CLOSED: do NOT fall back to a config.ini next to the executable.
        // That fallback historically created a stray, dysfunctional config file
        // beside the binaries (admin/SYSTEM-owned in %ProgramFiles%, or inside an
        // admin's user profile) that the GUI never reads and that can shadow the
        // real per-user config via the legacy-import read below. Writing beside
        // the binary is never correct, so we leave configPath empty and surface
        // the resolution failure to the caller instead of silently misplacing the
        // file. Logging/debug-log paths may still resolve independently.
        debug_log("config path resolution failed, refusing to fall back to a "
            "config.ini beside the executable: %s\n", err[0] ? err : "unknown");
        invalidate_tray_profile_cache();
        return;
    }

    StringCchPrintfA(g_app.configPath, ARRAY_COUNT(g_app.configPath), "%s\\%s", g_userDataDir, CONFIG_FILE_NAME);

    // Legacy one-time import (READ ONLY): if a config.ini exists beside the
    // executable from a very old portable install AND the user has no config yet,
    // copy it into the user's real config location. This branch only ever reads
    // the beside-binary file; it never creates one.
    char exeConfigPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exeConfigPath, ARRAY_COUNT(exeConfigPath));
    char* slash = strrchr(exeConfigPath, '\\');
    if (!slash) slash = strrchr(exeConfigPath, '/');
    if (slash) {
        slash[1] = 0;
        StringCchCatA(exeConfigPath, ARRAY_COUNT(exeConfigPath), CONFIG_FILE_NAME);
        DWORD legacyAttrs = GetFileAttributesA(exeConfigPath);
        DWORD currentAttrs = GetFileAttributesA(g_app.configPath);
        if (legacyAttrs != INVALID_FILE_ATTRIBUTES && currentAttrs == INVALID_FILE_ATTRIBUTES) {
            CopyFileA(exeConfigPath, g_app.configPath, TRUE);
        }
    }
    invalidate_tray_profile_cache();
}

static void refresh_service_debug_logging_from_config() {
    if (!g_app.isServiceProcess) return;
    bool newDebugLogging = service_config_debug_logging_enabled(nullptr, nullptr);
    g_debug_logging = newDebugLogging;
    if (!g_debug_logging) {
        close_debug_log_file();
    }
}

static bool hardware_initialize(char* detail, size_t detailSize) {
    if (g_app.gpuHandle && g_app.loaded && g_app.vfBackend) return true;
    set_last_apply_phase("hardware initialize: begin");
    debug_log("hardware_initialize: (re)initializing GPU backend\n");
    g_app.lastApplyUsedGpuOffset = true;
    if (!nvapi_init()) {
        set_message(detail, detailSize, "Failed to initialize NvAPI");
        set_last_apply_phase("hardware initialize: NvAPI init failed");
        return false;
    }
    set_last_apply_phase("hardware initialize: enumerate GPU");
    if (!nvapi_enum_gpu()) {
        set_message(detail, detailSize, "No NVIDIA GPU found");
        set_last_apply_phase("hardware initialize: no GPU found");
        return false;
    }
    nvapi_get_name();
    nvapi_read_gpu_metadata();
    bool offsetsOk = false;
    set_last_apply_phase("hardware initialize: VF curve readback");
    if (!read_live_curve_snapshot_settled(4, 40, &offsetsOk)) {
        set_message(detail, detailSize, "Failed to read VF curve from GPU");
        set_last_apply_phase("hardware initialize: VF curve read failed");
        return false;
    }
    (void)offsetsOk;
    // Skip refresh_global_state() while recovering from a recent NVML crash
    // (GPU device reconnect / driver restart via restart64.exe).
    // refresh_global_state() issues NVML reads (power limit, clock offsets,
    // fans) that can access-violate while the GPU kernel driver is still in a
    // transitional state after the reconnect — even though NVAPI (used for the
    // VF curve readback above) has already recovered.  A crash here kills
    // whichever thread called hardware_initialize(): the fan runtime recovery
    // thread OR the pipe server thread answering a GUI snapshot, breaking the
    // GUI connection.
    //
    // nvml_crash_recovery_active() is true only while the VEH has flagged a
    // recent stale-handle crash and the driver has not yet settled (the fan
    // runtime thread clears g_nvmlCrashCount the instant a recovery reapply
    // succeeds).  On normal startup it is false and the refresh runs as usual.
    // NOTE: an earlier version checked `g_app.gpuHandle == nullptr` here, but
    // nvapi_enum_gpu() above always sets gpuHandle non-null first, so the skip
    // was dead code and refresh_global_state() always ran — that was the NVML
    // access-violation the recovery loop kept hitting.
    if (nvml_crash_recovery_active()) {
        debug_log("hardware_initialize: skipping global state refresh during NVML crash recovery (crashCount=%ld)\n",
            (long)g_nvmlCrashCount);
    } else {
        set_last_apply_phase("hardware initialize: global state refresh");
        refresh_global_state(detail, detailSize);
    }
    initialize_gui_fan_settings_from_live_state(false);
    // Preserve the service active desired state across reinitializations
    // (e.g. after a driver TDR) so the GUI does not lose track of what
    // the service had applied.
    if (g_serviceHasActiveDesired) {
        debug_log("hardware_initialize: preserving existing service active desired state\n");
    } else {
        g_serviceActiveDesired = {};
        g_serviceActiveDesiredGpu = {};
        g_serviceHasActiveDesired = false;
    }
    // Initialization, telemetry and lifecycle readiness probes are strictly
    // read-only.  A stale/non-effective NVML memory-offset register may be
    // diagnosed and displayed conservatively, but uncertainty is never
    // "corrected" with a hardware write.  Only APPLY/RESET or an authorized
    // lifecycle restoration may mutate GPU state.
    if (!g_serviceHasActiveDesired && g_app.memClockOffsetkHz != 0
        && (g_app.smiMemMaxMHz == 0 || g_app.pstateMemMaxMHz == 0))
    {
        debug_log("hardware_initialize: stale mem VF offset %d kHz detected"
            " (smi=%u pstate=%u); diagnostic only, no write\n",
            g_app.memClockOffsetkHz, g_app.smiMemMaxMHz, g_app.pstateMemMaxMHz);
    }
#ifdef GREEN_CURVE_SERVICE_BINARY
    trim_working_set();
#endif
    set_last_apply_phase("hardware initialize: complete");
    return true;
}

static void populate_service_snapshot(ServiceSnapshot* snapshot) {
    if (!snapshot) return;
    DWORD snapshotLockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
    service_auto_restore_is_locked_out(&snapshotLockoutReason);
    EnterCriticalSection(&g_appLock);
    memset(snapshot, 0, sizeof(*snapshot));
    int snapshotGpuOffsetMHz = g_app.appliedGpuOffsetMHz;
    int snapshotGpuOffsetExcludeLowCount = (g_app.appliedGpuOffsetExcludeLowCount > 0 && snapshotGpuOffsetMHz != 0) ? g_app.appliedGpuOffsetExcludeLowCount : 0;
    if (g_serviceControlStateValid && control_state_has_meaningful_gpu(&g_serviceControlState)) {
        snapshotGpuOffsetMHz = g_serviceControlState.gpuOffsetMHz;
        snapshotGpuOffsetExcludeLowCount = (g_serviceControlState.gpuOffsetExcludeLowCount > 0 && snapshotGpuOffsetMHz != 0) ? g_serviceControlState.gpuOffsetExcludeLowCount : 0;
    } else {
        int desiredServiceOffsetMHz = 0;
        int desiredServiceExcludeLowCount = 0;
        if (service_active_desired_gpu_offset_fallback(&desiredServiceOffsetMHz, &desiredServiceExcludeLowCount)) {
            snapshotGpuOffsetMHz = desiredServiceOffsetMHz;
            snapshotGpuOffsetExcludeLowCount = desiredServiceExcludeLowCount;
        }
    }
    snapshot->initialized = g_app.gpuHandle != nullptr;
    snapshot->loaded = g_app.loaded;
    snapshot->fanSupported = g_app.fanSupported;
    snapshot->fanRangeKnown = g_app.fanRangeKnown;
    snapshot->fanIsAuto = g_app.fanIsAuto;
    snapshot->fanCurveRuntimeActive = g_app.fanCurveRuntimeActive;
    snapshot->fanFixedRuntimeActive = g_app.fanFixedRuntimeActive;
    snapshot->gpuOffsetRangeKnown = g_app.gpuOffsetRangeKnown;
    snapshot->memOffsetRangeKnown = g_app.memOffsetRangeKnown;
    snapshot->curveOffsetRangeKnown = g_app.curveOffsetRangeKnown;
    snapshot->gpuTemperatureValid = g_app.gpuTemperatureValid;
    snapshot->vfReadSupported = g_app.vfBackend && g_app.vfBackend->readSupported;
    snapshot->vfWriteSupported = g_app.vfBackend && g_app.vfBackend->writeSupported;
    snapshot->vfBestGuess = vf_backend_is_best_guess(g_app.vfBackend);
    snapshot->adapterCount = g_app.adapterCount;
    snapshot->selectedAdapterIndex = g_app.selectedGpuIndex;
    snapshot->selectedAdapterOrdinalFallback = g_app.selectedGpuOrdinalFallback;
    memcpy(snapshot->adapters, g_app.adapters, sizeof(snapshot->adapters));
    snapshot->gpuFamily = g_app.gpuFamily;
    snapshot->numPopulated = g_app.numPopulated;
    snapshot->gpuClockOffsetkHz = g_app.gpuClockOffsetkHz;
    snapshot->memClockOffsetkHz = g_app.memClockOffsetkHz;
    snapshot->gpuClockOffsetMinMHz = g_app.gpuClockOffsetMinMHz;
    snapshot->gpuClockOffsetMaxMHz = g_app.gpuClockOffsetMaxMHz;
    snapshot->memOffsetMinMHz = g_app.memClockOffsetMinMHz;
    snapshot->memOffsetMaxMHz = g_app.memClockOffsetMaxMHz;
    snapshot->curveOffsetMinkHz = g_app.curveOffsetMinkHz;
    snapshot->curveOffsetMaxkHz = g_app.curveOffsetMaxkHz;
    snapshot->powerLimitPct = g_app.powerLimitPct;
    snapshot->powerLimitDefaultmW = g_app.powerLimitDefaultmW;
    snapshot->powerLimitCurrentmW = g_app.powerLimitCurrentmW;
    snapshot->powerLimitMinmW = g_app.powerLimitMinmW;
    snapshot->powerLimitMaxmW = g_app.powerLimitMaxmW;
    snapshot->appliedGpuOffsetMHz = snapshotGpuOffsetMHz;
    snapshot->appliedGpuOffsetExcludeLowCount = snapshotGpuOffsetExcludeLowCount;
    snapshot->lastApplyUsedGpuOffset = g_app.lastApplyUsedGpuOffset;
    // RC7 fix: only report the lock from active desired if the hardware
    // is actually loaded and the reapply has confirmed the settings stuck.
    // When hardware_initialize fails (e.g. nvapi_init FAILED after recovery),
    // g_app.loaded is false and there is no applied lock regardless of what
    // the stale active desired claims.  The hardware state is unknown/stock.
    // While g_serviceReapplyInProgress is set, the GPU was just reconnected
    // and the desired lock has NOT been reapplied — fall through to live
    // curve detection so the GUI does not falsely display a locked tail.
    bool reapplyPending = InterlockedExchangeAdd(&g_serviceReapplyInProgress, 0) != 0;
    bool snapshotLockFromActiveDesired = g_app.isServiceProcess
        && g_app.loaded
        && g_app.gpuHandle
        && g_serviceHasActiveDesired
        && g_serviceActiveDesired.hasLock
        && g_serviceActiveDesired.lockCi >= 0
        && g_serviceActiveDesired.lockCi < VF_NUM_POINTS
        && g_serviceActiveDesired.lockMHz > 0
        && !reapplyPending;
    if (snapshotLockFromActiveDesired) {
        snapshot->hasLock = true;
        snapshot->lockCi = g_serviceActiveDesired.lockCi;
        snapshot->lockMHz = g_serviceActiveDesired.lockMHz;
        snapshot->lockMode = (int)g_serviceActiveDesired.lockMode;
        snapshot->lockTracksAnchor = g_serviceActiveDesired.lockTracksAnchor;
        if (snapshot->lockCi != g_app.lockedCi || snapshot->lockMHz != g_app.lockedFreq) {
            debug_log("populate_service_snapshot: reporting active desired lock ci=%d mhz=%u mode=%s instead of live-detected ci=%d mhz=%u\n",
                snapshot->lockCi,
                snapshot->lockMHz,
                lock_mode_name(g_serviceActiveDesired.lockMode),
                g_app.lockedCi,
                g_app.lockedFreq);
        }
    } else {
        snapshot->hasLock = (g_app.lockedCi >= 0 && g_app.lockedFreq > 0);
        snapshot->lockCi = g_app.lockedCi;
        snapshot->lockMHz = g_app.lockedFreq;
        snapshot->lockMode = (int)g_app.lockMode;
        snapshot->lockTracksAnchor = g_app.guiLockTracksAnchor;
    }
    int snapshotFanMode = current_green_curve_fan_intent_mode();
    int snapshotFanFixedPercent = current_green_curve_fan_intent_fixed_percent();
    const FanCurveConfig* snapshotFanCurve = current_green_curve_fan_intent_curve();
    if (snapshotFanMode == FAN_MODE_AUTO && !g_app.fanIsAuto) {
        debug_log_on_change("fan intent: external live fan policy observed fanIsAuto=0 gcIntent=Auto; preserving Auto in service snapshot\n");
    }
    snapshot->activeFanMode = snapshotFanMode;
    snapshot->activeFanFixedPercent = snapshotFanFixedPercent;
    snapshot->gpuTemperatureC = g_app.gpuTemperatureC;
    snapshot->fanCount = g_app.fanCount;
    snapshot->fanMinPct = g_app.fanMinPct;
    snapshot->fanMaxPct = g_app.fanMaxPct;
    memcpy(snapshot->fanPercent, g_app.fanPercent, sizeof(snapshot->fanPercent));
    memcpy(snapshot->fanTargetPercent, g_app.fanTargetPercent, sizeof(snapshot->fanTargetPercent));
    memcpy(snapshot->fanRpm, g_app.fanRpm, sizeof(snapshot->fanRpm));
    memcpy(snapshot->fanPolicy, g_app.fanPolicy, sizeof(snapshot->fanPolicy));
    memcpy(snapshot->fanControlSignal, g_app.fanControlSignal, sizeof(snapshot->fanControlSignal));
    memcpy(snapshot->fanTargetMask, g_app.fanTargetMask, sizeof(snapshot->fanTargetMask));
    memcpy(snapshot->curve, g_app.curve, sizeof(snapshot->curve));
    memcpy(snapshot->freqOffsets, g_app.freqOffsets, sizeof(snapshot->freqOffsets));
    copy_fan_curve(&snapshot->activeFanCurve, snapshotFanCurve);
    StringCchCopyA(snapshot->gpuName, ARRAY_COUNT(snapshot->gpuName), g_app.gpuName);
    StringCchCopyA(snapshot->ownerUser, ARRAY_COUNT(snapshot->ownerUser), g_app.backgroundServiceOwnerUser);
    snapshot->ownerSessionId = g_app.backgroundServiceOwnerSessionId;
    snapshot->ownerUtcMs = g_app.backgroundServiceOwnerUtcMs;
    // Recovery is now a process-bound controlled continuation rather than an
    // in-process reload/thread.  Keep the legacy snapshot fields meaningful
    // without reviving the removed cooldown/timestamp state.
    bool reapplyInProgress =
        InterlockedExchangeAdd(&g_serviceReapplyInProgress, 0) != 0;
    snapshot->serviceInRecovery = reapplyInProgress ||
        g_serviceControlledRecoveryValidated ||
        InterlockedExchangeAdd(&g_serviceRestartPreparing, 0) != 0 ||
        InterlockedExchangeAdd(&g_serviceRestartRequested, 0) != 0;
    snapshot->lastRecoveryTickMs = 0;
    snapshot->serviceReapplyInProgress = reapplyInProgress;
    snapshot->activeProfileSource = (gc_u32)g_serviceActiveProfileSource;
    snapshot->activeProfileSlot = (gc_u32)g_serviceActiveProfileSlot;
    snapshot->lastLifecycleTrigger = (gc_u32)g_serviceLastLifecycleTrigger;
    snapshot->lastLifecycleResult = (gc_u32)g_serviceLastLifecycleResult;
    snapshot->autoRestoreLockoutReason = snapshotLockoutReason;
    LeaveCriticalSection(&g_appLock);
}

static void populate_control_state(ControlState* state) {
    if (!state) return;
    EnterCriticalSection(&g_appLock);
    memset(state, 0, sizeof(*state));
    state->valid = true;
    state->hasGpuOffset = true;
    state->gpuOffsetMHz = current_applied_gpu_offset_mhz();
    state->gpuOffsetExcludeLowCount = (current_applied_gpu_offset_excludes_low_points() && state->gpuOffsetMHz != 0) ? g_app.appliedGpuOffsetExcludeLowCount : 0;
    state->hasMemOffset = true;
    state->memOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    state->hasPowerLimit = true;
    state->powerLimitPct = g_app.powerLimitPct;
    state->hasFan = true;
    state->fanMode = current_green_curve_fan_intent_mode();
    state->fanFixedPercent = current_green_curve_fan_intent_fixed_percent();
    state->fanCurrentPercent = current_displayed_fan_percent();
    state->fanCurrentTemperatureC = g_app.gpuTemperatureValid ? g_app.gpuTemperatureC : 0;
    copy_fan_curve(&state->fanCurve, current_green_curve_fan_intent_curve());
    ensure_valid_fan_curve_config(&state->fanCurve);
    LeaveCriticalSection(&g_appLock);
}

static void apply_service_snapshot_to_app(const ServiceSnapshot* snapshot) {
    if (!snapshot) return;
    EnterCriticalSection(&g_appLock);
    g_app.serviceSnapshotAuthoritative = true;
    g_app.serviceActiveProfileSource = (ServiceProfileSource)snapshot->activeProfileSource;
    g_app.serviceActiveProfileSlot = snapshot->activeProfileSlot;
    g_app.serviceLastLifecycleTrigger = (ServiceLifecycleTrigger)snapshot->lastLifecycleTrigger;
    g_app.serviceLastLifecycleResult = (ServiceLifecycleResult)snapshot->lastLifecycleResult;
    g_app.serviceAutoRestoreLockoutReason =
        (ServiceAutoRestoreLockoutReason)snapshot->autoRestoreLockoutReason;
    int previousAppliedGpuOffsetMHz = g_app.appliedGpuOffsetMHz;
    int previousAppliedGpuOffsetExcludeLowCount = g_app.appliedGpuOffsetExcludeLowCount;
    ControlState previousServiceControlState = g_app.serviceControlState;
    bool previousServiceGpuMeaningful = g_app.serviceControlStateValid && control_state_has_meaningful_gpu(&g_app.serviceControlState);
    bool previousServiceMemMeaningful = g_app.serviceControlStateValid && control_state_has_meaningful_mem(&g_app.serviceControlState);
    bool previousServicePowerMeaningful = g_app.serviceControlStateValid && control_state_has_meaningful_power(&g_app.serviceControlState);
    bool previousServiceFanMeaningful = g_app.serviceControlStateValid && control_state_has_meaningful_fan(&g_app.serviceControlState);
    g_app.loaded = snapshot->loaded;
    g_app.fanSupported = snapshot->fanSupported;
    g_app.fanRangeKnown = snapshot->fanRangeKnown;
    g_app.fanIsAuto = snapshot->fanIsAuto;
    g_app.fanCurveRuntimeActive = snapshot->fanCurveRuntimeActive;
    g_app.fanFixedRuntimeActive = snapshot->fanFixedRuntimeActive;
    g_app.gpuOffsetRangeKnown = snapshot->gpuOffsetRangeKnown;
    g_app.memOffsetRangeKnown = snapshot->memOffsetRangeKnown;
    g_app.curveOffsetRangeKnown = snapshot->curveOffsetRangeKnown;
    g_app.gpuTemperatureValid = snapshot->gpuTemperatureValid;
    g_app.adapterCount = snapshot->adapterCount > MAX_GPU_ADAPTERS ? MAX_GPU_ADAPTERS : snapshot->adapterCount;
    g_app.selectedGpuIndex = snapshot->selectedAdapterIndex;
    g_app.selectedNvmlIndex = snapshot->selectedAdapterIndex;
    g_app.selectedGpuOrdinalFallback = snapshot->selectedAdapterOrdinalFallback;
    memcpy(g_app.adapters, snapshot->adapters, sizeof(g_app.adapters));
    if (g_app.selectedGpuIndex < g_app.adapterCount) {
        g_app.selectedGpu = g_app.adapters[g_app.selectedGpuIndex];
        g_app.selectedGpuIdentityValid = g_app.selectedGpu.valid;
    }
    g_app.gpuFamily = snapshot->gpuFamily;
    g_app.numPopulated = snapshot->numPopulated;
    g_app.gpuClockOffsetkHz = snapshot->gpuClockOffsetkHz;
    g_app.memClockOffsetkHz = snapshot->memClockOffsetkHz;
    g_app.gpuClockOffsetMinMHz = snapshot->gpuClockOffsetMinMHz;
    g_app.gpuClockOffsetMaxMHz = snapshot->gpuClockOffsetMaxMHz;
    g_app.memClockOffsetMinMHz = snapshot->memOffsetMinMHz;
    g_app.memClockOffsetMaxMHz = snapshot->memOffsetMaxMHz;
    g_app.curveOffsetMinkHz = snapshot->curveOffsetMinkHz;
    g_app.curveOffsetMaxkHz = snapshot->curveOffsetMaxkHz;
    g_app.powerLimitPct = snapshot->powerLimitPct;
    g_app.powerLimitDefaultmW = snapshot->powerLimitDefaultmW;
    g_app.powerLimitCurrentmW = snapshot->powerLimitCurrentmW;
    g_app.powerLimitMinmW = snapshot->powerLimitMinmW;
    g_app.powerLimitMaxmW = snapshot->powerLimitMaxmW;
    bool snapshotGpuMeaningful = snapshot->appliedGpuOffsetMHz != 0 || snapshot->appliedGpuOffsetExcludeLowCount > 0;
    if (snapshotGpuMeaningful || !previousServiceGpuMeaningful || !snapshot->lastApplyUsedGpuOffset) {
        g_app.appliedGpuOffsetMHz = snapshot->appliedGpuOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = snapshot->appliedGpuOffsetExcludeLowCount;
    } else {
        g_app.appliedGpuOffsetMHz = previousAppliedGpuOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = previousAppliedGpuOffsetExcludeLowCount;
    }
    g_app.lastApplyUsedGpuOffset = snapshot->lastApplyUsedGpuOffset;
    g_app.activeFanMode = snapshot->activeFanMode;
    g_app.activeFanFixedPercent = snapshot->activeFanFixedPercent;
    g_app.gpuTemperatureC = snapshot->gpuTemperatureC;
    g_app.fanCount = snapshot->fanCount;
    g_app.fanMinPct = snapshot->fanMinPct;
    g_app.fanMaxPct = snapshot->fanMaxPct;
    memcpy(g_app.fanPercent, snapshot->fanPercent, sizeof(g_app.fanPercent));
    memcpy(g_app.fanTargetPercent, snapshot->fanTargetPercent, sizeof(g_app.fanTargetPercent));
    memcpy(g_app.fanRpm, snapshot->fanRpm, sizeof(g_app.fanRpm));
    memcpy(g_app.fanPolicy, snapshot->fanPolicy, sizeof(g_app.fanPolicy));
    memcpy(g_app.fanControlSignal, snapshot->fanControlSignal, sizeof(g_app.fanControlSignal));
    memcpy(g_app.fanTargetMask, snapshot->fanTargetMask, sizeof(g_app.fanTargetMask));
    memcpy(g_app.curve, snapshot->curve, sizeof(g_app.curve));
    memcpy(g_app.freqOffsets, snapshot->freqOffsets, sizeof(g_app.freqOffsets));
    copy_fan_curve(&g_app.activeFanCurve, &snapshot->activeFanCurve);
    StringCchCopyA(g_app.gpuName, ARRAY_COUNT(g_app.gpuName), snapshot->gpuName);
    StringCchCopyA(g_app.backgroundServiceOwnerUser, ARRAY_COUNT(g_app.backgroundServiceOwnerUser), snapshot->ownerUser);
    g_app.backgroundServiceOwnerSessionId = snapshot->ownerSessionId;
    g_app.backgroundServiceOwnerUtcMs = snapshot->ownerUtcMs;
    rebuild_visible_map();
    // F-REL-2e: clear a stale ADOPTED GUI lock when the service has authoritatively
    // reset to no-lock (e.g. reset-to-defaults after an out-of-band restart) and the
    // user has no unsaved edits.  This runs OUTSIDE should_accept_service_curve_lock_
    // detection() — that gate returns false whenever lockedCi>=0, which would
    // otherwise pin the stale lock checkbox / point value / "Lock:" header forever
    // after a reset.  Narrow on purpose: only fires when the snapshot authoritatively
    // reports no lock (a real recovery still reports the active desired lock, so this
    // never fights an in-flight reapply), the user is NOT dirty-editing, and the
    // current GUI lock matches the last applied/adopted lock (never a fresh,
    // unapplied user edit).
    if (snapshot->loaded && !snapshot->hasLock && !gui_state_dirty()
        && g_app.lockedCi >= 0
        && g_app.lockedCi == g_app.appliedLockCi
        && g_app.lockedFreq == g_app.appliedLockFreq) {
        debug_log("apply_service_snapshot_to_app: service reports no lock and GUI is not dirty — "
            "clearing stale adopted GUI lock ci=%d mhz=%u and forcing a full GUI refresh to match the reset state\n",
            g_app.lockedCi, g_app.lockedFreq);
        g_app.lockedCi = -1;
        g_app.lockedFreq = 0;
        g_app.lockMode = LOCK_MODE_NONE;
        g_app.lockedVi = -1;
        g_app.appliedLockCi = -1;
        g_app.appliedLockFreq = 0;
        g_app.appliedLockVi = -1;
        g_app.appliedLockMode = LOCK_MODE_NONE;
        g_app.guiLockTracksAnchor = true;
        // The per-second telemetry poll deliberately does NOT resync the editable
        // curve/lock controls (so it never wipes in-progress edits).  Since we just
        // cleared an adopted lock to follow a service reset, request the same full
        // visual resync the Refresh button performs (graph, per-point fields, lock
        // checkboxes, "Lock:" header) so the WHOLE GUI reflects the default state —
        // not just internal state.  Without this only field state changes and the
        // graph/checkbox/header stay stale until the user presses Refresh.
        g_guiForceFullRefresh = true;
    }
    // Sync lockMode from snapshot when the lock point matches, even when
    // should_accept_service_curve_lock_detection() returns false (which it
    // does when the GUI already has a lock set).  Gate: NEVER while the GUI
    // holds divergent pending lock intent (lockMode != appliedLockMode, e.g.
    // a FLATTEN->HARD checkbox click or a loaded HARD profile at the same
    // point) or unsaved edits — the per-second telemetry snapshot still
    // carries the previously APPLIED mode and would silently revert the
    // user's pin before Apply ("No changes to apply") and corrupt saves.
    if (snapshot->loaded && snapshot->hasLock && snapshot->lockCi >= 0 && snapshot->lockMHz > 0
        && g_app.lockedCi == snapshot->lockCi && g_app.lockedFreq == snapshot->lockMHz
        && (g_app.lockMode != (LockMode)snapshot->lockMode || g_app.appliedLockMode != (LockMode)snapshot->lockMode)) {
        if (lock_mode_sync_allowed((int)g_app.lockMode, (int)g_app.appliedLockMode, gui_state_dirty())) {
            g_app.lockMode = (LockMode)snapshot->lockMode;
            g_app.appliedLockMode = (LockMode)snapshot->lockMode;
            debug_log("apply_service_snapshot_to_app: synced lockMode=%s from snapshot (same lock point ci=%d mhz=%u)\n",
                lock_mode_name(g_app.lockMode), g_app.lockedCi, g_app.lockedFreq);
        } else {
            // Per-second telemetry calls this; log only when the skipped
            // state changes so a pending pin doesn't spam the debug log.
            static int lastSkipLogged = -1;
            int skipState = ((int)g_app.lockMode << 8) | ((int)g_app.appliedLockMode << 4) | (snapshot->lockMode & 0xF);
            if (skipState != lastSkipLogged) {
                lastSkipLogged = skipState;
                debug_log("apply_service_snapshot_to_app: lockMode sync skipped (pending lock intent gui=%s applied=%s snapshot=%s dirty=%d ci=%d mhz=%u)\n",
                    lock_mode_name(g_app.lockMode),
                    lock_mode_name(g_app.appliedLockMode),
                    lock_mode_name((LockMode)snapshot->lockMode),
                    gui_state_dirty() ? 1 : 0,
                    g_app.lockedCi, g_app.lockedFreq);
            }
        }
    }
    if (snapshot->loaded && should_accept_service_curve_lock_detection()) {
        if (snapshot->hasLock && snapshot->lockCi >= 0 && snapshot->lockMHz > 0) {
            g_app.lockedCi = snapshot->lockCi;
            g_app.lockedFreq = snapshot->lockMHz;
            g_app.lockMode = (LockMode)snapshot->lockMode;
            g_app.lockedVi = -1;
            for (int vi = 0; vi < g_app.numVisible; vi++) {
                if (g_app.visibleMap[vi] == snapshot->lockCi) {
                    g_app.lockedVi = vi;
                    break;
                }
            }
            g_app.guiLockTracksAnchor = snapshot->lockTracksAnchor;
            g_app.appliedLockVi = g_app.lockedVi;
            g_app.appliedLockCi = g_app.lockedCi;
            g_app.appliedLockFreq = g_app.lockedFreq;
            g_app.appliedLockMode = (LockMode)snapshot->lockMode;
            debug_log("apply_service_snapshot_to_app: adopted service lock ci=%d mhz=%u mode=%s visible=%d\n",
                g_app.lockedCi, g_app.lockedFreq, lock_mode_name(g_app.lockMode), g_app.lockedVi);
        } else {
            // RC7 fix: when the snapshot reports hasLock=false (e.g. after
            // a GPU device reconnect where the reapply hasn't completed, or
            // after a RESET), clear the GUI-side lock FIRST so that
            // detect_locked_tail_from_curve() can properly re-detect from
            // the live curve.  Without this, should_accept_service_curve_
            // lock_detection() would return false (lockedCi >= 0 from old
            // snapshot) and detect_locked_tail_from_curve would see
            // should_auto_detect_locked_tail_from_live_curve() return false,
            // preserving the stale lock indefinitely.
            g_app.lockedCi = -1;
            g_app.lockedFreq = 0;
            g_app.lockMode = LOCK_MODE_NONE;
            g_app.guiLockTracksAnchor = true;
            g_app.lockedVi = -1;
            g_app.appliedLockCi = -1;
            g_app.appliedLockFreq = 0;
            g_app.appliedLockVi = -1;
            g_app.appliedLockMode = LOCK_MODE_NONE;
            detect_locked_tail_from_curve();
        }
    }

    // Sync GUI fan mode to the service snapshot only when the user hasn't
    // explicitly changed it (e.g. after loading a profile but before applying).
    if (!gui_state_dirty()) {
        g_app.guiFanMode = snapshot->activeFanMode;
        if (snapshot->activeFanMode == FAN_MODE_FIXED) {
            g_app.guiFanFixedPercent = clamp_percent(snapshot->activeFanFixedPercent);
        } else {
            g_app.guiFanFixedPercent = clamp_percent(current_displayed_fan_percent());
        }
        ensure_valid_fan_curve_config(&g_app.guiFanCurve);
        if (snapshot->activeFanMode == FAN_MODE_CURVE) {
            copy_fan_curve(&g_app.guiFanCurve, &snapshot->activeFanCurve);
        }
    }
    memset(&g_app.serviceControlState, 0, sizeof(g_app.serviceControlState));
    g_app.serviceControlState.valid = true;
    g_app.serviceControlState.hasGpuOffset = true;
    if (snapshotGpuMeaningful || !previousServiceGpuMeaningful) {
        g_app.serviceControlState.gpuOffsetMHz = snapshot->appliedGpuOffsetMHz;
        g_app.serviceControlState.gpuOffsetExcludeLowCount = (snapshot->appliedGpuOffsetExcludeLowCount > 0 && snapshot->appliedGpuOffsetMHz != 0) ? snapshot->appliedGpuOffsetExcludeLowCount : 0;
    } else if (previousServiceGpuMeaningful) {
        g_app.serviceControlState.gpuOffsetMHz = previousAppliedGpuOffsetMHz;
        g_app.serviceControlState.gpuOffsetExcludeLowCount = (previousAppliedGpuOffsetExcludeLowCount > 0 && previousAppliedGpuOffsetMHz != 0) ? previousAppliedGpuOffsetExcludeLowCount : 0;
    }
    g_app.serviceControlState.hasMemOffset = true;
    int snapshotMemOffsetMHz = mem_display_mhz_from_driver_khz(snapshot->memClockOffsetkHz);
    if (snapshotMemOffsetMHz != 0 || !previousServiceMemMeaningful) {
        g_app.serviceControlState.memOffsetMHz = snapshotMemOffsetMHz;
    } else if (previousServiceMemMeaningful) {
        g_app.serviceControlState.memOffsetMHz = previousServiceControlState.memOffsetMHz;
    }
    g_app.serviceControlState.hasPowerLimit = true;
    if (snapshot->powerLimitPct != 100 || !previousServicePowerMeaningful) {
        g_app.serviceControlState.powerLimitPct = snapshot->powerLimitPct;
    } else if (previousServicePowerMeaningful) {
        g_app.serviceControlState.powerLimitPct = previousServiceControlState.powerLimitPct;
    }
    g_app.serviceControlState.hasFan = true;
    bool snapshotFanMeaningful = snapshot->activeFanMode != FAN_MODE_AUTO || snapshot->activeFanFixedPercent != 0 || current_displayed_fan_percent() != 0;
    if (snapshotFanMeaningful || !previousServiceFanMeaningful) {
        g_app.serviceControlState.fanMode = snapshot->activeFanMode;
        g_app.serviceControlState.fanFixedPercent = clamp_percent(snapshot->activeFanFixedPercent);
        g_app.serviceControlState.fanCurrentPercent = current_displayed_fan_percent();
        g_app.serviceControlState.fanCurrentTemperatureC = snapshot->gpuTemperatureValid ? snapshot->gpuTemperatureC : 0;
        copy_fan_curve(&g_app.serviceControlState.fanCurve, &snapshot->activeFanCurve);
        ensure_valid_fan_curve_config(&g_app.serviceControlState.fanCurve);
    } else if (previousServiceFanMeaningful) {
        g_app.serviceControlState.fanMode = previousServiceControlState.fanMode;
        g_app.serviceControlState.fanFixedPercent = previousServiceControlState.fanFixedPercent;
        g_app.serviceControlState.fanCurrentPercent = previousServiceControlState.fanCurrentPercent;
        g_app.serviceControlState.fanCurrentTemperatureC = previousServiceControlState.fanCurrentTemperatureC;
        copy_fan_curve(&g_app.serviceControlState.fanCurve, &previousServiceControlState.fanCurve);
        ensure_valid_fan_curve_config(&g_app.serviceControlState.fanCurve);
    }
    log_locked_tail_drift_diagnostics();
    g_app.serviceControlStateValid = true;
    LeaveCriticalSection(&g_appLock);
#ifndef GREEN_CURVE_SERVICE_BINARY
    sync_applied_profile_from_service_metadata();
#endif
}

static void apply_service_desired_to_gui(const DesiredSettings* desired) {
    if (!desired) return;
    if (desired->hasGpuOffset) {
        g_app.appliedGpuOffsetMHz = desired->gpuOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = (desired->gpuOffsetExcludeLowCount > 0 && desired->gpuOffsetMHz != 0) ? desired->gpuOffsetExcludeLowCount : 0;
        if (!gui_state_dirty()) {
            g_app.guiGpuOffsetMHz = desired->gpuOffsetMHz;
            g_app.guiGpuOffsetExcludeLowCount = (desired->gpuOffsetExcludeLowCount > 0 && desired->gpuOffsetMHz != 0) ? desired->gpuOffsetExcludeLowCount : 0;
        }
    }
    if (!gui_state_dirty()) {
        memset(g_app.guiCurvePointExplicit, 0, sizeof(g_app.guiCurvePointExplicit));
        for (int vi = 0; vi < g_app.numVisible; vi++) {
            int ci = g_app.visibleMap[vi];
            if (ci < 0 || ci >= VF_NUM_POINTS) continue;
            g_app.guiCurvePointExplicit[ci] = desired->hasCurvePoint[ci];
            if (desired->hasCurvePoint[ci] && g_app.hEditsMhz[vi]) {
                set_edit_value(g_app.hEditsMhz[vi], desired->curvePointMHz[ci]);
            }
        }
        if (desired->hasLock && desired->lockCi >= 0 && desired->lockMHz > 0) {
            g_app.lockedCi = desired->lockCi;
            g_app.lockedFreq = desired->lockMHz;
            g_app.lockMode = desired->lockMode;
            g_app.lockedVi = -1;
            for (int vi = 0; vi < g_app.numVisible; vi++) {
                if (g_app.visibleMap[vi] == desired->lockCi) {
                    g_app.lockedVi = vi;
                    break;
                }
            }
            g_app.appliedLockVi = g_app.lockedVi;
            g_app.appliedLockCi = g_app.lockedCi;
            g_app.appliedLockFreq = g_app.lockedFreq;
            g_app.appliedLockMode = desired->lockMode;
            g_app.guiLockTracksAnchor = desired->lockTracksAnchor;
        } else if (g_app.lockedFreq == 0 && g_app.lockedCi < 0) {
            g_app.lockedVi = -1;
            g_app.lockedCi = -1;
            g_app.lockedFreq = 0;
            g_app.lockMode = LOCK_MODE_NONE;
            g_app.appliedLockVi = -1;
            g_app.appliedLockCi = -1;
            g_app.appliedLockFreq = 0;
            g_app.appliedLockMode = LOCK_MODE_NONE;
            g_app.guiLockTracksAnchor = true;
        }
    }
    if (desired->hasFan) {
        if (!gui_state_dirty()) {
            g_app.guiFanMode = desired->fanMode;
            if (desired->fanMode == FAN_MODE_FIXED) {
                g_app.guiFanFixedPercent = clamp_percent(desired->fanPercent);
            } else {
                g_app.guiFanFixedPercent = clamp_percent(current_displayed_fan_percent());
            }
            copy_fan_curve(&g_app.guiFanCurve, &desired->fanCurve);
            ensure_valid_fan_curve_config(&g_app.guiFanCurve);
        }
    }
    // Adopt the service's active curve intent as the drift-free baseline, regardless
    // of GUI dirty state (this is what the hardware is actually set to, not live
    // readback). Keeps fan-only detection and the editor/graph accurate across
    // reconnects and telemetry-driven refreshes without importing boost drift.
    capture_applied_curve_baseline(desired);
}

static void apply_control_state_to_gui(const ControlState* state) {
    if (!state || !state->valid) return;
    bool meaningfulGpuState = control_state_has_meaningful_gpu(state);
    bool meaningfulMemState = control_state_has_meaningful_mem(state);
    bool meaningfulPowerState = control_state_has_meaningful_power(state);
    bool meaningfulFanState = control_state_has_meaningful_fan(state);
    if (!control_state_has_any_meaningful_value(state)) {
        debug_log("apply_control_state_to_gui: ignoring non-meaningful service control update\n");
        return;
    }

    ControlState merged = {};
    if (g_app.serviceControlStateValid) merged = g_app.serviceControlState;
    merged.valid = true;
    if (state->hasGpuOffset && meaningfulGpuState) {
        merged.hasGpuOffset = true;
        merged.gpuOffsetMHz = state->gpuOffsetMHz;
        merged.gpuOffsetExcludeLowCount = (state->gpuOffsetExcludeLowCount > 0 && state->gpuOffsetMHz != 0) ? state->gpuOffsetExcludeLowCount : 0;
    }
    if (state->hasMemOffset && meaningfulMemState) {
        merged.hasMemOffset = true;
        merged.memOffsetMHz = state->memOffsetMHz;
    }
    if (state->hasPowerLimit && meaningfulPowerState) {
        merged.hasPowerLimit = true;
        merged.powerLimitPct = state->powerLimitPct;
    }
    if (state->hasFan && meaningfulFanState) {
        merged.hasFan = true;
        merged.fanMode = state->fanMode;
        merged.fanFixedPercent = state->fanFixedPercent;
        merged.fanCurrentPercent = state->fanCurrentPercent;
        merged.fanCurrentTemperatureC = state->fanCurrentTemperatureC;
        copy_fan_curve(&merged.fanCurve, &state->fanCurve);
        ensure_valid_fan_curve_config(&merged.fanCurve);
    }
    g_app.serviceControlStateValid = true;
    g_app.serviceControlState = merged;
    bool updateGui = !gui_state_dirty();
    if (meaningfulGpuState) {
        g_app.appliedGpuOffsetMHz = state->gpuOffsetMHz;
        g_app.appliedGpuOffsetExcludeLowCount = (state->gpuOffsetExcludeLowCount > 0 && state->gpuOffsetMHz != 0) ? state->gpuOffsetExcludeLowCount : 0;
        if (updateGui) {
            g_app.guiGpuOffsetMHz = state->gpuOffsetMHz;
            g_app.guiGpuOffsetExcludeLowCount = (state->gpuOffsetExcludeLowCount > 0 && state->gpuOffsetMHz != 0) ? state->gpuOffsetExcludeLowCount : 0;
        }
    }
    if (meaningfulMemState) {
        g_app.memClockOffsetkHz = mem_driver_khz_from_display_mhz(state->memOffsetMHz);
    }
    if (meaningfulPowerState) {
        g_app.powerLimitPct = state->powerLimitPct;
    }
    if (meaningfulFanState) {
        g_app.activeFanMode = state->fanMode;
        if (updateGui) {
            g_app.guiFanMode = state->fanMode;
        }
        if (state->fanMode == FAN_MODE_FIXED) {
            g_app.activeFanFixedPercent = clamp_percent(state->fanFixedPercent);
            if (updateGui) g_app.guiFanFixedPercent = g_app.activeFanFixedPercent;
        } else {
            int currentPercent = state->fanCurrentPercent > 0 ? state->fanCurrentPercent : current_displayed_fan_percent();
            g_app.activeFanFixedPercent = clamp_percent(currentPercent);
            if (updateGui) g_app.guiFanFixedPercent = clamp_percent(currentPercent);
        }
        copy_fan_curve(&g_app.activeFanCurve, &state->fanCurve);
        ensure_valid_fan_curve_config(&g_app.activeFanCurve);
        if (updateGui) {
            copy_fan_curve(&g_app.guiFanCurve, &state->fanCurve);
            ensure_valid_fan_curve_config(&g_app.guiFanCurve);
        }
        // state->fanMode is Green Curve intent, not necessarily the live driver
        // fan policy.  FanControl or another external controller may make NVML
        // report manual while Green Curve intent remains Auto; keep fanIsAuto
        // sourced from live snapshots/telemetry.
        g_app.fanCurveRuntimeActive = state->fanMode == FAN_MODE_CURVE;
        g_app.fanFixedRuntimeActive = state->fanMode == FAN_MODE_FIXED;
    }
}

static bool get_effective_control_state(ControlState* stateOut) {
    if (!stateOut) return false;
    memset(stateOut, 0, sizeof(*stateOut));
    if (g_app.usingBackgroundService && g_app.serviceControlStateValid && control_state_has_any_meaningful_value(&g_app.serviceControlState)) {
        *stateOut = g_app.serviceControlState;
        debug_log_on_change("get_effective_control_state: using cached service state gpu=%d exclude=%d fanMode=%d\n",
            stateOut->gpuOffsetMHz,
            stateOut->gpuOffsetExcludeLowCount,
            stateOut->fanMode);
        return stateOut->valid;
    }
    if (g_app.isServiceProcess && g_serviceControlStateValid && control_state_has_any_meaningful_value(&g_serviceControlState)) {
        *stateOut = g_serviceControlState;
        debug_log("get_effective_control_state: using service-local state gpu=%d exclude=%d fanMode=%d\n",
            stateOut->gpuOffsetMHz,
            stateOut->gpuOffsetExcludeLowCount,
            stateOut->fanMode);
        return stateOut->valid;
    }
    populate_control_state(stateOut);
    debug_log("get_effective_control_state: using local state gpu=%d exclude=%d fanMode=%d\n",
        stateOut->gpuOffsetMHz,
        stateOut->gpuOffsetExcludeLowCount,
        stateOut->fanMode);
    return stateOut->valid;
}

