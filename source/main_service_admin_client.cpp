// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Bounded UI waits, elevation helpers, and protected service-binary staging.

#include "log_redaction_policy.h"

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

// Render a service-admin failure for the user: the classified sentence first
// (it names the cause AND the next action), then where the technical detail
// lives.  This runs ONCE per failure, at the GUI completion boundary --
// lower layers only ever fill `err` with raw call-site detail, because the
// elevated helper's own message went to ITS greencurve_cli_log.txt under the
// LocalAppData of whichever account approved the UAC prompt (a standard user
// cannot even open that profile), so the reason must survive the process
// boundary on its own.  See service_admin_reason_policy.h.
static void set_service_admin_reason_message(int reason, char* err, size_t errSize) {
    const char* text = gc_service_admin_reason_text(reason);
    if (gc_service_admin_reason_is_user_cancel(reason)) {
        // A declined elevation prompt is the user's own answer, not a fault:
        // no log pointer, nothing to investigate.
        set_message(err, errSize, "%s", text);
        return;
    }
    set_message(err, errSize, "%s\n\n%s", text, GC_SVC_ADMIN_LOG_POINTER);
}

static bool wait_for_service_admin_helper(HANDLE process, char* err,
    size_t errSize, int* reasonOut) {
    if (!process) return true;
    ULONGLONG started = GetTickCount64();
    for (;;) {
        if (InterlockedExchangeAdd(&g_serviceAdminWaitCancelled, 0) != 0) {
            // Abandoning the wait at shutdown is nobody's fault and no click:
            // the helper keeps running and finishes on its own.
            if (reasonOut) *reasonOut = GC_SVC_ADMIN_SHUTDOWN_ABANDONED;
            set_message(err, errSize,
                "Elevated service helper wait cancelled during GUI shutdown");
            return false;
        }
        ULONGLONG elapsed = GetTickCount64() - started;
        if (elapsed >= GC_SVC_ADMIN_HELPER_TIMEOUT_MS) {
            TerminateProcess(process, 1);
            if (reasonOut) *reasonOut = GC_SVC_ADMIN_HELPER_TIMED_OUT;
            set_message(err, errSize,
                "Elevated service helper was stopped after %lu ms without "
                "reporting a result",
                (unsigned long)GC_SVC_ADMIN_HELPER_TIMEOUT_MS);
            debug_log("service admin helper: timed out after %lu ms and was terminated\n",
                (unsigned long)GC_SVC_ADMIN_HELPER_TIMEOUT_MS);
            return false;
        }
        DWORD remaining = (DWORD)(GC_SVC_ADMIN_HELPER_TIMEOUT_MS - elapsed);
        DWORD waitResult = WaitForSingleObject(process,
            (DWORD)nvmin((int)remaining, 250));
        if (waitResult == WAIT_OBJECT_0) break;
        if (waitResult == WAIT_FAILED) {
            if (reasonOut) *reasonOut = GC_SVC_ADMIN_HELPER_LAUNCH_FAILED;
            set_message(err, errSize,
                "Failed waiting for elevated service helper (error %lu)",
                GetLastError());
            return false;
        }
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process, &exitCode)) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_HELPER_LAUNCH_FAILED;
        set_message(err, errSize,
            "Failed reading the elevated service helper's result (error %lu)",
            GetLastError());
        return false;
    }
    int reason = gc_service_admin_reason_from_exit_code(exitCode);
    debug_log("service admin helper: exited after %llu ms exitCode=%lu reason=%d\n",
        (unsigned long long)(GetTickCount64() - started),
        (unsigned long)exitCode, reason);
    if (reasonOut) *reasonOut = reason;
    // On failure `err` deliberately stays EMPTY: the helper's own message is in
    // ITS log, and the GUI composes the sentence plus the pointer to that log
    // (set_service_admin_reason_message).  Filling `err` here would either
    // duplicate the sentence or hide the pointer behind a raw error number.
    return reason == GC_SVC_ADMIN_OK;
}

static bool launch_service_admin_helper(bool enable, const char* configPath,
    char* err, size_t errSize, int* reasonOut) {
    if (reasonOut) *reasonOut = GC_SVC_ADMIN_UNKNOWN;
    WCHAR exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, ARRAY_COUNT(exePath));

    // Pass the requesting GUI's config path so the elevated helper resolves the
    // same per-user config as the GUI instead of its own (admin/SYSTEM) path or
    // a beside-binary fallback. Service install/remove itself does not write the
    // user config, but a consistent path prevents stray-config regressions and
    // makes any helper logging reference the correct file.
    WCHAR cfgPath[MAX_PATH] = {};
    if (!utf8_to_wide(configPath, cfgPath, ARRAY_COUNT(cfgPath))) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_HELPER_LAUNCH_FAILED;
        set_message(err, errSize, "Failed converting config path for service helper");
        return false;
    }

    const WCHAR* baseArg = enable ? L"--service-install" : L"--service-remove";
    WCHAR helperArg[1536] = {};
    bool buildArgs = pl_append_quoted_arg_w(helperArg, ARRAY_COUNT(helperArg), baseArg) &&
        pl_append_quoted_arg_w(helperArg, ARRAY_COUNT(helperArg), L"--config") &&
        pl_append_quoted_arg_w(helperArg, ARRAY_COUNT(helperArg), cfgPath);
    if (!buildArgs) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_HELPER_LAUNCH_FAILED;
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
        // Declining the UAC prompt is by far the most common outcome here and
        // it is not a fault: ERROR_CANCELLED got the same "(error 1223)"
        // treatment as a real launch failure, which reads like a bug in the
        // program rather than the answer the user just gave it.  `err` stays
        // raw detail either way; the GUI composes the user message once.
        DWORD launchErr = GetLastError();
        int reason = launchErr == GC_SVC_ERR_CANCELLED
            ? GC_SVC_ADMIN_ELEVATION_DECLINED
            : GC_SVC_ADMIN_HELPER_LAUNCH_FAILED;
        if (reasonOut) *reasonOut = reason;
        debug_log("service admin helper: ShellExecuteEx(runas) failed error=%lu reason=%d\n",
            (unsigned long)launchErr, reason);
        set_message(err, errSize, "ShellExecuteEx(runas) failed (error %lu)",
            (unsigned long)launchErr);
        return false;
    }
    if (sei.hProcess) {
        ScopedHandle helperProcess(sei.hProcess);
        bool ok = wait_for_service_admin_helper(
            helperProcess.get(), err, errSize, reasonOut);
        if (!ok) return false;
    }
    if (reasonOut) *reasonOut = GC_SVC_ADMIN_OK;
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
//
// One implementation, shared with the path classifier's `under_user_profile`
// fact (service_acl.h): both warnings land in the same GUI status line, so
// they must not be able to disagree about what counts as a user profile.
static bool install_dir_is_under_user_profile_w(const WCHAR* dir) {
    return gc_path_is_under_user_profile(dir);
}

static bool get_secure_service_install_dir_w(WCHAR* out, size_t outCount, char* err, size_t errSize) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    return get_current_executable_directory_w(out, outCount, err, errSize);
}

static bool ensure_secure_service_binary_path(WCHAR* out, size_t outCount, char* err, size_t errSize,
                                              int* reasonOut = nullptr) {
    if (!out || outCount == 0) return false;
    out[0] = 0;

    WCHAR sourcePath[MAX_PATH] = {};
    if (!get_adjacent_service_binary_path(sourcePath, ARRAY_COUNT(sourcePath), err, errSize)) {
        // The only way this fails is "greencurve-service.exe is not beside
        // greencurve.exe, or is not a plain file". A partially extracted
        // archive and an antivirus quarantine both land here, and both are
        // things the user can check in ten seconds once told.
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_BINARY_MISSING;
        set_message(err, errSize, "%s", gc_service_admin_reason_text(GC_SVC_ADMIN_BINARY_MISSING));
        return false;
    }

    WCHAR installDir[MAX_PATH] = {};
    if (!get_secure_service_install_dir_w(installDir, ARRAY_COUNT(installDir), err, errSize)) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_LOCATION_REFUSED;
        return false;
    }

    // MAY we harden this folder at all?  This is not the protection
    // classification below -- it is the question that one never asked.
    // Registering the service REPLACES this folder's DACL with an
    // administrators-only-write one and propagates it to everything already
    // inside, so a folder that is not plausibly Green Curve's own must be
    // refused BEFORE the first DACL write, not warned about afterwards.
    // README says "extract the .7z archive anywhere", and 7-Zip's Extract Here
    // into Downloads is the obvious way to do that; setup has refused a bad
    // destination since it existed, the portable path refused nothing.
    int locationVerdict = gc_service_install_location_verdict(installDir);
    if (locationVerdict != GC_SVC_LOCATION_OK) {
        char dirToken[32] = {};
        gc_log_wide_identifier_token(installDir, dirToken, sizeof(dirToken));
        debug_log("service install: REFUSED location token %s verdict=%s; "
                  "hardening it would take write access to a folder that is not ours\n",
                  dirToken, gc_service_location_verdict_name(locationVerdict));
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_LOCATION_REFUSED;
        set_message(err, errSize, "%s", gc_service_admin_reason_text(GC_SVC_ADMIN_LOCATION_REFUSED));
        return false;
    }

    // How well can this location be protected?  (See
    // service_path_chain_policy.h.)  F-SEC-1 in one line: a LocalSystem
    // service binary in a folder standard accounts can write is SYSTEM code
    // execution waiting to happen.  The classification never blocks - the
    // administrator picked this folder and portable use must work everywhere -
    // but every verdict is logged and a filesystem that cannot express a DACL
    // at all is handled as an explicit, loud capability gap.
    GcPathProtectionReport protection = {};
    classify_path_protection(installDir, &protection, true);
    debug_log("service install: path protection protected=%d standardWritable=%d profile=%d "
              "remote=%d noFilesystemPermissions=%d reason=%d\n",
              protection.verdict.chain_protected ? 1 : 0,
              protection.verdict.standard_writable ? 1 : 0,
              protection.verdict.user_profile ? 1 : 0,
              protection.verdict.remote ? 1 : 0,
              protection.verdict.no_filesystem_permissions ? 1 : 0,
              (int)protection.verdict.reason);
    const bool hardenFiles = !protection.verdict.no_filesystem_permissions;
    if (!hardenFiles) {
        debug_log("service install WARNING: volume has no file permissions (for example "
                  "FAT/exFAT); skipping DACL hardening - the LocalSystem service binary "
                  "cannot be protected here\n");
    }
    if (!CreateDirectoryW(installDir, nullptr)) {
        DWORD createErr = GetLastError();
        if (createErr != ERROR_ALREADY_EXISTS) {
            if (reasonOut) *reasonOut = GC_SVC_ADMIN_DIRECTORY_HARDENING_FAILED;
            set_message(err, errSize, "Failed creating secure service directory (error %lu)", createErr);
            return false;
        }
    }
    DWORD dirAttrs = GetFileAttributesW(installDir);
    if (dirAttrs == INVALID_FILE_ATTRIBUTES ||
        (dirAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (dirAttrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_DIRECTORY_HARDENING_FAILED;
        set_message(err, errSize, "Secure service directory is unavailable or unsafe");
        return false;
    }
    char dirAclErr[256] = {};
    if (hardenFiles) {
        if (!apply_protected_service_dir_dacl(installDir, dirAclErr, sizeof(dirAclErr))) {
            if (reasonOut) *reasonOut = GC_SVC_ADMIN_DIRECTORY_HARDENING_FAILED;
            set_message(err, errSize, "Failed securing service directory: %s", dirAclErr[0] ? dirAclErr : "unknown");
            return false;
        }
        if (!machine_config_dacl_is_hardened(installDir)) {
            if (reasonOut) *reasonOut = GC_SVC_ADMIN_DIRECTORY_HARDENING_FAILED;
            set_message(err, errSize, "Service directory DACL did not remain hardened");
            return false;
        }
    }

    WCHAR targetPath[MAX_PATH] = {};
    if (FAILED(StringCchPrintfW(targetPath, ARRAY_COUNT(targetPath), L"%ls\\%ls", installDir, APP_SERVICE_EXE_NAME_W))) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_LOCATION_REFUSED;
        set_message(err, errSize, "Installed service binary path is too long");
        return false;
    }

    WCHAR canonicalPath[MAX_PATH] = {};
    if (GetFullPathNameW(targetPath, ARRAY_COUNT(canonicalPath), canonicalPath, nullptr) == 0) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_LOCATION_REFUSED;
        set_message(err, errSize, "Failed canonicalizing service binary path");
        return false;
    }
    size_t installDirLen = wcslen(installDir);
    if (_wcsnicmp(canonicalPath, installDir, installDirLen) != 0 ||
        (canonicalPath[installDirLen] != L'\\' && canonicalPath[installDirLen] != L'/' && canonicalPath[installDirLen] != 0)) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_LOCATION_REFUSED;
        set_message(err, errSize, "Service binary path escaped the expected installation directory");
        return false;
    }

    if (_wcsicmp(sourcePath, targetPath) != 0) {
        WCHAR tempPath[MAX_PATH] = {};
        if (FAILED(StringCchPrintfW(tempPath, ARRAY_COUNT(tempPath), L"%ls.tmp", targetPath))) {
            if (reasonOut) *reasonOut = GC_SVC_ADMIN_BINARY_STAGING_FAILED;
            set_message(err, errSize, "Temporary service binary path is too long");
            return false;
        }
        DeleteFileW(tempPath);
        if (!CopyFileW(sourcePath, tempPath, FALSE)) {
            DWORD copyErr = GetLastError();
            if (reasonOut) {
                *reasonOut = gc_service_admin_classify_win32(GC_SVC_STAGE_STAGE_BINARY, copyErr);
            }
            set_message(err, errSize, "Failed staging service binary in target directory (error %lu)", copyErr);
            return false;
        }
        if (!MoveFileExW(tempPath, targetPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DWORD moveErr = GetLastError();
            DeleteFileW(tempPath);
            if (reasonOut) {
                *reasonOut = gc_service_admin_classify_win32(GC_SVC_STAGE_STAGE_BINARY, moveErr);
            }
            set_message(err, errSize, "Failed installing service binary in target directory (error %lu)", moveErr);
            return false;
        }
    }

    if (!file_is_regular_no_reparse_w(targetPath)) {
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_BINARY_STAGING_FAILED;
        set_message(err, errSize, "Installed service binary is missing or unsafe");
        return false;
    }

    // F-SEC-1: harden the installed binary's DACL so a standard user cannot
    // overwrite it and gain SYSTEM code execution via the SCM/auto-restart path.
    // Runs elevated (service install requires admin) and BEFORE the service is
    // registered, so a failure to secure the binary fails the install closed.
    // On a filesystem without persistent ACLs there is nothing to harden; that
    // gap was already logged above and acknowledged wherever an interactive
    // flow exists.
    char aclErr[160] = {};
    if (hardenFiles) {
        if (!apply_protected_service_binary_dacl(targetPath, aclErr, sizeof(aclErr))) {
            if (reasonOut) *reasonOut = GC_SVC_ADMIN_DIRECTORY_HARDENING_FAILED;
            set_message(err, errSize, "Failed securing service binary: %s", aclErr[0] ? aclErr : "unknown");
            return false;
        }
        if (!service_binary_dacl_is_hardened(targetPath)) {
            if (reasonOut) *reasonOut = GC_SVC_ADMIN_DIRECTORY_HARDENING_FAILED;
            set_message(err, errSize, "Service binary DACL did not remain hardened");
            return false;
        }
    }
    // Whatever the classification vouched for at the start must still hold now
    // that the hardened binary is in place: a directory planted between the two
    // checks fails the install closed rather than registering a LocalSystem
    // service from a location less protected than reported.
    GcPathProtectionReport protectionAfter = {};
    classify_path_protection(installDir, &protectionAfter, false);
    if (protection.verdict.chain_protected && !protectionAfter.verdict.chain_protected) {
        set_message(err, errSize,
            "The service directory lost its protection during staging (reason %d); "
            "refusing to register the service",
            (int)protectionAfter.verdict.reason);
        if (reasonOut) *reasonOut = GC_SVC_ADMIN_DIRECTORY_HARDENING_FAILED;
        return false;
    }
    if (install_dir_is_under_user_profile_w(installDir)) {
        char targetToken[32] = {};
        char dirToken[32] = {};
        gc_log_wide_identifier_token(targetPath, targetToken, sizeof(targetToken));
        gc_log_wide_identifier_token(installDir, dirToken, sizeof(dirToken));
        debug_log("service install: staged and hardened LocalSystem binary at token %s (directory token %s)\n",
                  targetToken, dirToken);
        debug_log("service install WARNING: install dir %s is under a user profile. Other users, "
            "including restricted/standard accounts, may be unable to read or execute the Green Curve "
            "GUI binary. Install under %%ProgramFiles%% to make the application available to all users.\n",
            dirToken);
    } else {
        debug_log("service install: staged and hardened LocalSystem binary at %ls (directory %ls)\n", targetPath, installDir);
    }

    return SUCCEEDED(StringCchCopyW(out, outCount, targetPath));
}

namespace {
GcPathProtectionReport g_runningExeProtectionCache = {};
bool g_runningExeProtectionCached = false;
}

void running_exe_dir_protection_invalidate() {
    g_runningExeProtectionCached = false;
}

// Classify the RUNNING binary's own directory (the install root in every
// supported layout: the service binary is staged adjacent to the GUI, and the
// updater stages into %ProgramData%).  Used by the GUI's status warnings and
// kept here so GUI shards stay away from the Win32 path details.  Cached after
// first lookup to avoid repeated synchronous SAM/RPC and DACL queries on the UI thread.
bool running_exe_dir_protection(GcPathProtectionReport* out) {
    if (!out) return false;
    if (g_runningExeProtectionCached) {
        *out = g_runningExeProtectionCache;
        return true;
    }
    GcPathProtectionReport blank = {};
    *out = blank;
    WCHAR exeDir[MAX_PATH] = {};
    char ignored[64] = {};
    if (!get_current_executable_directory_w(exeDir, ARRAY_COUNT(exeDir),
                                            ignored, sizeof(ignored))) {
        gc_path_protection_classify(nullptr, &out->verdict);
        return false;
    }
    classify_path_protection(exeDir, out, false);
    g_runningExeProtectionCache = *out;
    g_runningExeProtectionCached = true;
    return true;
}

int running_exe_dir_install_location_verdict() {
    WCHAR exeDir[MAX_PATH] = {};
    char ignored[64] = {};
    if (!get_current_executable_directory_w(exeDir, ARRAY_COUNT(exeDir),
                                            ignored, sizeof(ignored))) {
        // Unknown means unproven, and this gate stands in front of a DACL
        // rewrite: refuse rather than proceed on a path we could not resolve.
        return GC_SVC_LOCATION_NOT_ABSOLUTE;
    }
    return gc_service_install_location_verdict(exeDir);
}

// Service startup: record how well the service binary's own location is
// protected (see service_path_chain_policy.h).  Purely diagnostic - a chain
// someone loosened after the install never stops the service from working -
// but it must be visible in the support log and in the GUI warnings.
void service_log_path_protection_at_startup() {
    WCHAR installDir[MAX_PATH] = {};
    char ignored[64] = {};
    if (!get_secure_service_install_dir_w(installDir, ARRAY_COUNT(installDir),
                                          ignored, sizeof(ignored))) {
        debug_log("path protection: service directory could not be resolved\n");
        return;
    }
    GcPathProtectionReport protection = {};
    classify_path_protection(installDir, &protection, false);
    debug_log("path protection: service directory protected=%d standardWritable=%d profile=%d "
              "remote=%d noFilesystemPermissions=%d reason=%d\n",
              protection.verdict.chain_protected ? 1 : 0,
              protection.verdict.standard_writable ? 1 : 0,
              protection.verdict.user_profile ? 1 : 0,
              protection.verdict.remote ? 1 : 0,
              protection.verdict.no_filesystem_permissions ? 1 : 0,
              (int)protection.verdict.reason);
    if (gc_path_protection_requires_acknowledgment(&protection.verdict)) {
        debug_log("path protection WARNING: %s\n", GC_PATH_PROTECTION_SUMMARY_UNPROTECTED);
    }
}

// Which directories uninstall must NOT revert to inherited permissions.
//
// This used to be a second, hand-written copy of the root/share-root shape
// rule, and the two halves had drifted into an asymmetry that could not be
// undone: install hardened a drive root happily, uninstall refused to revert
// one, so registering the service from D:\ locked that volume down for good.
// Install now refuses the same shapes this skips (the shared predicate below),
// which makes the skip a consistency check rather than a trap: nothing we
// hardened can land here, and anything that does was not hardened by us.
static bool directory_path_is_root_or_share_root_w(const WCHAR* dir) {
    if (!dir || !dir[0]) return true;
    return !gc_service_location_shape_is_acceptable(dir);
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
