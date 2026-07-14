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
            if (!gc_CreateDirectoryUtf8(temp, nullptr)) {
                DWORD e = GetLastError();
                if (e != ERROR_ALREADY_EXISTS) {
                    set_message(err, errSize, "Failed creating directory %s (error %lu)", temp, e);
                    return false;
                }
                DWORD attrs = gc_GetFileAttributesUtf8(temp);
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    set_message(err, errSize, "Directory %s is a reparse point, refusing to traverse", temp);
                    return false;
                }
            }
        }
        *p = save;
    }

    if (!gc_CreateDirectoryUtf8(temp, nullptr)) {
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
    DWORD attrs = gc_GetFileAttributesUtf8(legacyDir);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) return;

    char pattern[MAX_PATH] = {};
    if (FAILED(StringCchPrintfA(pattern, ARRAY_COUNT(pattern), "%s\\*", legacyDir))) return;
    WIN32_FIND_DATAA fd = {};
    HANDLE h = gc_FindFirstFileUtf8(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue; // skip ., .., subdirs
            // Preserve the deliberately-shared profile bank; only sweep sensitive
            // legacy artifacts (dumps/logs/restart state) out of this directory.
            if (lstrcmpiA(fd.cFileName, MACHINE_CONFIG_FILE_NAME) == 0) continue;
            char filePath[MAX_PATH] = {};
            if (SUCCEEDED(StringCchPrintfA(filePath, ARRAY_COUNT(filePath), "%s\\%s", legacyDir, fd.cFileName))) {
                gc_DeleteFileUtf8(filePath);
            }
        } while (gc_FindNextFileUtf8(h, &fd));
        FindClose(h);
    }
    // RemoveDirectory fails harmlessly if the shared bank file remains; only log
    // when the directory was actually empty and got removed.
    if (gc_RemoveDirectoryUtf8(legacyDir)) {
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
        HANDLE h = gc_FindFirstFileUtf8(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        unsigned int count = 0;
        char oldest[MAX_PATH] = {};
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            count++;
            if (oldest[0] == 0 || lstrcmpA(fd.cFileName, oldest) < 0) {
                StringCchCopyA(oldest, ARRAY_COUNT(oldest), fd.cFileName);
            }
        } while (gc_FindNextFileUtf8(h, &fd));
        FindClose(h);
        if (count <= maxKeep || oldest[0] == 0) return;
        char victim[MAX_PATH] = {};
        if (FAILED(StringCchPrintfA(victim, ARRAY_COUNT(victim), "%s\\%s", dir, oldest))) return;
        if (!gc_DeleteFileUtf8(victim)) return; // stop on failure to avoid an infinite loop
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
    DWORD attrs = gc_GetFileAttributesUtf8(g_app.configPath);
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
    memset(g_app.appliedCurveMHz, 0, sizeof(g_app.appliedCurveMHz));
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
    gc_GetModuleFileNameUtf8(nullptr, exeConfigPath, ARRAY_COUNT(exeConfigPath));
    char* slash = strrchr(exeConfigPath, '\\');
    if (!slash) slash = strrchr(exeConfigPath, '/');
    if (slash) {
        slash[1] = 0;
        StringCchCatA(exeConfigPath, ARRAY_COUNT(exeConfigPath), CONFIG_FILE_NAME);
        DWORD legacyAttrs = gc_GetFileAttributesUtf8(exeConfigPath);
        DWORD currentAttrs = gc_GetFileAttributesUtf8(g_app.configPath);
        if (legacyAttrs != INVALID_FILE_ATTRIBUTES && currentAttrs == INVALID_FILE_ATTRIBUTES) {
            gc_CopyFileUtf8(exeConfigPath, g_app.configPath, TRUE);
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
