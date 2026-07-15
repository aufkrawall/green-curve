// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Bounded UI waits, elevation helpers, and protected service-binary staging.

static volatile LONG g_serviceAdminWaitCancelled = 0;

static void service_admin_reset_wait_cancel() {
    InterlockedExchange(&g_serviceAdminWaitCancelled, 0);
}

static void service_admin_request_wait_cancel() {
    InterlockedExchange(&g_serviceAdminWaitCancelled, 1);
}

static bool wait_object_pumping_ui(HANDLE waitObject, DWORD timeoutMs) {
    if (!g_app.hMainWnd) {
        if (waitObject) return WaitForSingleObject(waitObject, timeoutMs) == WAIT_OBJECT_0;
        Sleep(timeoutMs);
        return false;
    }
    ULONGLONG start = GetTickCount64();
    for (;;) {
        ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= timeoutMs) return false;
        DWORD remaining = (DWORD)(timeoutMs - elapsed);
        DWORD count = waitObject ? 1u : 0u;
        DWORD wr = MsgWaitForMultipleObjectsEx(count, waitObject ? &waitObject : nullptr,
                                               remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (count == 1 && wr == WAIT_OBJECT_0) return true;          // object signaled
        if (wr == WAIT_OBJECT_0 + count) {                            // messages pending
            MSG msg;
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    PostQuitMessage((int)msg.wParam);                 // re-post; let main loop exit
                    return false;
                }
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
        } else if (wr == WAIT_FAILED) {
            // Degrade to a plain wait for the remainder rather than spin.
            if (waitObject) return WaitForSingleObject(waitObject, remaining) == WAIT_OBJECT_0;
            Sleep(remaining);
            return false;
        }
        // WAIT_TIMEOUT (no object, no messages) loops and re-checks elapsed.
    }
}

static bool wait_for_helper_process_bounded(HANDLE process, const char* description, char* err, size_t errSize) {
    if (!process) return true;
    // Pump the GUI while the elevated helper runs so the window keeps repainting
    // (no stale/corrupted content or "not responding" ghosting). The window is
    // disabled for the duration to block re-entrant input.
    UiInputGuard uiGuard;
    bool signaled = wait_object_pumping_ui(process, ELEVATED_HELPER_TIMEOUT_MS);
    if (!signaled) {
        TerminateProcess(process, 1);
        wait_object_pumping_ui(process, 1000);
        set_message(err, errSize, "%s timed out", description ? description : "Elevated helper");
        return false;
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process, &exitCode)) {
        set_message(err, errSize, "Failed reading %s exit code (error %lu)", description ? description : "elevated helper", GetLastError());
        return false;
    }
    if (exitCode != 0) {
        set_message(err, errSize, "%s failed (exit code %lu)", description ? description : "Elevated helper", exitCode);
        return false;
    }
    return true;
}

static bool wait_for_service_admin_helper(HANDLE process, char* err,
    size_t errSize) {
    if (!process) return true;
    ULONGLONG started = GetTickCount64();
    for (;;) {
        if (InterlockedExchangeAdd(&g_serviceAdminWaitCancelled, 0) != 0) {
            set_message(err, errSize,
                "Elevated service helper wait cancelled during GUI shutdown");
            return false;
        }
        ULONGLONG elapsed = GetTickCount64() - started;
        if (elapsed >= ELEVATED_HELPER_TIMEOUT_MS) {
            TerminateProcess(process, 1);
            set_message(err, errSize, "Elevated service helper timed out");
            return false;
        }
        DWORD remaining = (DWORD)(ELEVATED_HELPER_TIMEOUT_MS - elapsed);
        DWORD waitResult = WaitForSingleObject(process,
            (DWORD)nvmin((int)remaining, 250));
        if (waitResult == WAIT_OBJECT_0) break;
        if (waitResult == WAIT_FAILED) {
            set_message(err, errSize,
                "Failed waiting for elevated service helper (error %lu)",
                GetLastError());
            return false;
        }
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process, &exitCode) || exitCode != 0) {
        set_message(err, errSize, "Elevated service helper failed (exit code %lu)",
            (unsigned long)exitCode);
        return false;
    }
    return true;
}

static bool launch_service_admin_helper(bool enable, const char* configPath,
    char* err, size_t errSize) {
    WCHAR exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, ARRAY_COUNT(exePath));

    // Pass the requesting GUI's config path so the elevated helper resolves the
    // same per-user config as the GUI instead of its own (admin/SYSTEM) path or
    // a beside-binary fallback. Service install/remove itself does not write the
    // user config, but a consistent path prevents stray-config regressions and
    // makes any helper logging reference the correct file.
    WCHAR cfgPath[MAX_PATH] = {};
    if (!utf8_to_wide(configPath, cfgPath, ARRAY_COUNT(cfgPath))) {
        set_message(err, errSize, "Failed converting config path for service helper");
        return false;
    }

    const WCHAR* baseArg = enable ? L"--service-install" : L"--service-remove";
    WCHAR helperArg[1536] = {};
    bool buildArgs = pl_append_quoted_arg_w(helperArg, ARRAY_COUNT(helperArg), baseArg) &&
        pl_append_quoted_arg_w(helperArg, ARRAY_COUNT(helperArg), L"--config") &&
        pl_append_quoted_arg_w(helperArg, ARRAY_COUNT(helperArg), cfgPath);
    if (!buildArgs) {
        set_message(err, errSize, "Service helper command too long");
        return false;
    }

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = helperArg;
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExW(&sei)) {
        set_message(err, errSize, "Failed starting elevated service helper (error %lu)", GetLastError());
        return false;
    }
    if (sei.hProcess) {
        ScopedHandle helperProcess(sei.hProcess);
        bool ok = wait_for_service_admin_helper(
            helperProcess.get(), err, errSize);
        if (!ok) return false;
    }
    return true;
}

static bool launch_startup_task_admin_helper(bool enable, char* err, size_t errSize) {
    WCHAR exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, ARRAY_COUNT(exePath));

    WCHAR cfgPath[MAX_PATH] = {};
    if (!utf8_to_wide(g_app.configPath, cfgPath, ARRAY_COUNT(cfgPath))) {
        set_message(err, errSize, "Failed converting config path for startup task helper");
        return false;
    }

    // Capture the REQUESTING user's SAM name BEFORE elevation.  The elevated
    // helper will run as the approving admin, so without this override the
    // task would be scoped to the admin's logon instead of this (often
    // standard/restricted) user's.  --for-user forces the helper to stamp the
    // requesting user into the task name/UserId/Principal.
    WCHAR requestingUser[512] = {};
    bool haveRequestingUser = get_current_user_sam_name(requestingUser, ARRAY_COUNT(requestingUser));

    WCHAR helperArg[2048] = {};
    bool buildArgs = pl_append_quoted_arg_w(helperArg, ARRAY_COUNT(helperArg), L"--elevated") &&
        pl_append_quoted_arg_w(helperArg, ARRAY_COUNT(helperArg), enable ? L"--startup-task-enable" : L"--startup-task-disable");
    if (haveRequestingUser && requestingUser[0]) {
        buildArgs = buildArgs &&
            pl_append_quoted_arg_w(helperArg, ARRAY_COUNT(helperArg), L"--for-user") &&
            pl_append_quoted_arg_w(helperArg, ARRAY_COUNT(helperArg), requestingUser);
    }
    buildArgs = buildArgs &&
        pl_append_quoted_arg_w(helperArg, ARRAY_COUNT(helperArg), L"--config") &&
        pl_append_quoted_arg_w(helperArg, ARRAY_COUNT(helperArg), cfgPath);
    if (!buildArgs) {
        set_message(err, errSize, "Startup task helper command too long");
        return false;
    }

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = helperArg;
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExW(&sei)) {
        set_message(err, errSize, "Failed starting elevated startup task helper (error %lu)", GetLastError());
        return false;
    }
    if (sei.hProcess) {
        ScopedHandle helperProcess(sei.hProcess);
        bool ok = wait_for_helper_process_bounded(helperProcess.get(), "Elevated startup task helper", err, errSize);
        if (!ok) return false;
    }
    return true;
}

static bool get_adjacent_service_binary_path(WCHAR* out, size_t outCount, char* err, size_t errSize) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    WCHAR exeDir[MAX_PATH] = {};
    if (!get_current_executable_directory_w(exeDir, ARRAY_COUNT(exeDir), err, errSize)) return false;
    if (FAILED(StringCchPrintfW(out, outCount, L"%ls\\%ls", exeDir, APP_SERVICE_EXE_NAME_W))) {
        set_message(err, errSize, "Service binary path is too long");
        return false;
    }
    if (!file_is_regular_no_reparse_w(out)) {
        set_message(err, errSize, "Service binary is missing or unsafe");
        return false;
    }
    return true;
}


// True if `dir` is located under a Windows user profile directory (e.g.
// C:\Users\<name>\...).  This catches the common portable-install mistake of
// running greencurve.exe from inside an admin's profile, which makes the GUI
// binary inaccessible to other restricted users on the same machine.
static bool install_dir_is_under_user_profile_w(const WCHAR* dir) {
    if (!dir || !dir[0]) return false;
    WCHAR fullDir[MAX_PATH] = {};
    if (GetFullPathNameW(dir, MAX_PATH, fullDir, nullptr) == 0) return false;
    size_t fullDirLen = wcslen(fullDir);

    // Check against the current user's profile path.
    PWSTR profileDir = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profileDir)) && profileDir) {
        size_t profileLen = wcslen(profileDir);
        if (fullDirLen >= profileLen &&
            _wcsnicmp(fullDir, profileDir, profileLen) == 0 &&
            (fullDirLen == profileLen || fullDir[profileLen] == L'\\' || fullDir[profileLen] == L'/')) {
            CoTaskMemFree(profileDir);
            return true;
        }
        CoTaskMemFree(profileDir);
    }

    // Also detect any path under C:\Users\ (or the localized equivalent).
    PWSTR profilesDir = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_UserProfiles, 0, nullptr, &profilesDir)) && profilesDir) {
        size_t profilesLen = wcslen(profilesDir);
        if (fullDirLen >= profilesLen &&
            _wcsnicmp(fullDir, profilesDir, profilesLen) == 0 &&
            (fullDirLen == profilesLen || fullDir[profilesLen] == L'\\' || fullDir[profilesLen] == L'/')) {
            CoTaskMemFree(profilesDir);
            return true;
        }
        CoTaskMemFree(profilesDir);
    }
    return false;
}

static bool get_secure_service_install_dir_w(WCHAR* out, size_t outCount, char* err, size_t errSize) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    return get_current_executable_directory_w(out, outCount, err, errSize);
}

static bool ensure_secure_service_binary_path(WCHAR* out, size_t outCount, char* err, size_t errSize) {
    if (!out || outCount == 0) return false;
    out[0] = 0;

    WCHAR sourcePath[MAX_PATH] = {};
    if (!get_adjacent_service_binary_path(sourcePath, ARRAY_COUNT(sourcePath), err, errSize)) return false;

    WCHAR installDir[MAX_PATH] = {};
    if (!get_secure_service_install_dir_w(installDir, ARRAY_COUNT(installDir), err, errSize)) return false;
    if (!CreateDirectoryW(installDir, nullptr)) {
        DWORD createErr = GetLastError();
        if (createErr != ERROR_ALREADY_EXISTS) {
            set_message(err, errSize, "Failed creating secure service directory (error %lu)", createErr);
            return false;
        }
    }
    DWORD dirAttrs = GetFileAttributesW(installDir);
    if (dirAttrs == INVALID_FILE_ATTRIBUTES ||
        (dirAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (dirAttrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        set_message(err, errSize, "Secure service directory is unavailable or unsafe");
        return false;
    }
    char dirAclErr[256] = {};
    if (!apply_protected_service_dir_dacl(installDir, dirAclErr, sizeof(dirAclErr))) {
        set_message(err, errSize, "Failed securing service directory: %s", dirAclErr[0] ? dirAclErr : "unknown");
        return false;
    }
    if (!machine_config_dacl_is_hardened(installDir)) {
        set_message(err, errSize, "Service directory DACL did not remain hardened");
        return false;
    }

    WCHAR targetPath[MAX_PATH] = {};
    if (FAILED(StringCchPrintfW(targetPath, ARRAY_COUNT(targetPath), L"%ls\\%ls", installDir, APP_SERVICE_EXE_NAME_W))) {
        set_message(err, errSize, "Installed service binary path is too long");
        return false;
    }

    WCHAR canonicalPath[MAX_PATH] = {};
    if (GetFullPathNameW(targetPath, ARRAY_COUNT(canonicalPath), canonicalPath, nullptr) == 0) {
        set_message(err, errSize, "Failed canonicalizing service binary path");
        return false;
    }
    size_t installDirLen = wcslen(installDir);
    if (_wcsnicmp(canonicalPath, installDir, installDirLen) != 0 ||
        (canonicalPath[installDirLen] != L'\\' && canonicalPath[installDirLen] != L'/' && canonicalPath[installDirLen] != 0)) {
        set_message(err, errSize, "Service binary path escaped the expected installation directory");
        return false;
    }

    if (_wcsicmp(sourcePath, targetPath) != 0) {
        WCHAR tempPath[MAX_PATH] = {};
        if (FAILED(StringCchPrintfW(tempPath, ARRAY_COUNT(tempPath), L"%ls.tmp", targetPath))) {
            set_message(err, errSize, "Temporary service binary path is too long");
            return false;
        }
        DeleteFileW(tempPath);
        if (!CopyFileW(sourcePath, tempPath, FALSE)) {
            set_message(err, errSize, "Failed staging service binary in target directory (error %lu)", GetLastError());
            return false;
        }
        if (!MoveFileExW(tempPath, targetPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DWORD moveErr = GetLastError();
            DeleteFileW(tempPath);
            set_message(err, errSize, "Failed installing service binary in target directory (error %lu)", moveErr);
            return false;
        }
    }

    if (!file_is_regular_no_reparse_w(targetPath)) {
        set_message(err, errSize, "Installed service binary is missing or unsafe");
        return false;
    }

    // F-SEC-1: harden the installed binary's DACL so a standard user cannot
    // overwrite it and gain SYSTEM code execution via the SCM/auto-restart path.
    // Runs elevated (service install requires admin) and BEFORE the service is
    // registered, so a failure to secure the binary fails the install closed.
    char aclErr[160] = {};
    if (!apply_protected_service_binary_dacl(targetPath, aclErr, sizeof(aclErr))) {
        set_message(err, errSize, "Failed securing service binary: %s", aclErr[0] ? aclErr : "unknown");
        return false;
    }
    if (!service_binary_dacl_is_hardened(targetPath)) {
        set_message(err, errSize, "Service binary DACL did not remain hardened");
        return false;
    }
    debug_log("service install: staged and hardened LocalSystem binary at %ls (directory %ls)\n", targetPath, installDir);
    if (install_dir_is_under_user_profile_w(installDir)) {
        debug_log("service install WARNING: install dir \"%ls\" is under a user profile. Other users, "
            "including restricted/standard accounts, may be unable to read or execute the Green Curve "
            "GUI binary. Install under %%ProgramFiles%% to make the application available to all users.\n",
            installDir);
    }

    return SUCCEEDED(StringCchCopyW(out, outCount, targetPath));
}

static bool directory_path_is_root_or_share_root_w(const WCHAR* dir) {
    if (!dir || !dir[0]) return true;
    size_t len = wcslen(dir);
    if (len <= 3 && len >= 2 && dir[1] == L':') return true;
    if ((dir[0] == L'\\' || dir[0] == L'/') && (dir[1] == L'\\' || dir[1] == L'/')) {
        unsigned int components = 0;
        bool inComponent = false;
        for (const WCHAR* p = dir + 2; *p; ++p) {
            bool slash = (*p == L'\\' || *p == L'/');
            if (slash) {
                if (inComponent) components++;
                inComponent = false;
            } else {
                inComponent = true;
            }
        }
        if (inComponent) components++;
        return components <= 2;
    }
    return false;
}

static void cleanup_secure_service_binary_after_remove(const WCHAR* installedServicePath = nullptr) {
    // Service binary removal intentionally does NOT delete the file from disk.
    // With the service installed adjacent to the GUI binary, the user manages
    // the service binary manually. Deleting it on service uninstall would
    // destroy the user's manually-placed binary.
    //
    // F-SEC-1: we DO revert the protected DACLs that install applied, so once
    // the service is unregistered the user can freely delete or replace the
    // adjacent payload again (the in-place hardening is only meaningful while it
    // is a registered SYSTEM service).
    WCHAR targetPath[MAX_PATH] = {};
    if (installedServicePath && installedServicePath[0]) {
        if (FAILED(StringCchCopyW(targetPath, ARRAY_COUNT(targetPath), installedServicePath))) return;
    } else if (!get_service_binary_path_from_scm(targetPath, ARRAY_COUNT(targetPath))) {
        WCHAR installDir[MAX_PATH] = {};
        char ignored[64] = {};
        if (!get_secure_service_install_dir_w(installDir, ARRAY_COUNT(installDir), ignored, sizeof(ignored))) return;
        if (FAILED(StringCchPrintfW(targetPath, ARRAY_COUNT(targetPath), L"%ls\\%ls", installDir, APP_SERVICE_EXE_NAME_W))) return;
    }
    if (GetFileAttributesW(targetPath) == INVALID_FILE_ATTRIBUTES) return;
    WCHAR installDir[MAX_PATH] = {};
    if (SUCCEEDED(StringCchCopyW(installDir, ARRAY_COUNT(installDir), targetPath))) {
        WCHAR* slash = wcsrchr(installDir, L'\\');
        if (!slash) slash = wcsrchr(installDir, L'/');
        if (slash) *slash = 0;
    }
    char aclErr[160] = {};
    if (restore_inherited_dacl(targetPath, aclErr, sizeof(aclErr))) {
        debug_log("service uninstall: reverted service binary DACL to inherited for %ls; user can delete/replace it\n", targetPath);
    } else {
        debug_log("service uninstall: could not revert service binary DACL: %s\n", aclErr[0] ? aclErr : "unknown");
    }
    if (installDir[0] && !directory_path_is_root_or_share_root_w(installDir)) {
        char dirAclErr[160] = {};
        if (restore_inherited_dacl(installDir, dirAclErr, sizeof(dirAclErr))) {
            debug_log("service uninstall: reverted service directory DACL to inherited for %ls\n", installDir);
        } else {
            debug_log("service uninstall: could not revert service directory DACL: %s\n", dirAclErr[0] ? dirAclErr : "unknown");
        }
    } else if (installDir[0]) {
        debug_log("service uninstall: skipped service directory DACL restore for root-like path %ls\n", installDir);
    }
}

// Resolve the machine-wide config DIRECTORY: %ProgramData%\Green Curve.
// %ProgramData% is a fixed known folder, identical for the LocalSystem service
// and for every user's GUI, all-users-readable, and resolvable WITHOUT querying
// the SCM (which a restricted user's GUI may not be able to do).  This replaces
// the old "next to the service binary + parse the SCM command line" scheme.
