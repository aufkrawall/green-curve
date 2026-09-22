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

bool running_exe_dir_differs_from_registered_service() {
    WCHAR exeDir[MAX_PATH] = {};
    WCHAR registeredDir[MAX_PATH] = {};
    char ignored[64] = {};
    if (!get_current_executable_directory_w(exeDir, ARRAY_COUNT(exeDir), ignored, sizeof(ignored)) ||
        !get_service_binary_directory_from_scm(registeredDir, ARRAY_COUNT(registeredDir))) {
        return false;
    }
    size_t exeLength = wcslen(exeDir);
    size_t registeredLength = wcslen(registeredDir);
    while (exeLength > 0 && (exeDir[exeLength - 1] == L'\\' || exeDir[exeLength - 1] == L'/')) exeLength--;
    while (registeredLength > 0 && (registeredDir[registeredLength - 1] == L'\\' ||
                                    registeredDir[registeredLength - 1] == L'/')) registeredLength--;
    bool differs = exeLength != registeredLength ||
        CompareStringOrdinal(exeDir, (int)exeLength, registeredDir, (int)registeredLength, TRUE) != CSTR_EQUAL;
    if (differs) debug_log("GUI service checkbox: the registered service runs from a different folder\n");
    return differs;
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

// Resolve the machine-wide config DIRECTORY: %ProgramData%\Green Curve.
// %ProgramData% is a fixed known folder, identical for the LocalSystem service
// and for every user's GUI, all-users-readable, and resolvable WITHOUT querying
// the SCM (which a restricted user's GUI may not be able to do).  This replaces
// the old "next to the service binary + parse the SCM command line" scheme.
