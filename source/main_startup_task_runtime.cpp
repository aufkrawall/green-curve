// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// ============================================================================
// Per-user Windows startup task registration and synchronization
// ============================================================================

static bool xml_escape_wide(const WCHAR* text, WCHAR* out, size_t outCount, bool escapeQuotes) {
    if (!text || !out || outCount == 0) return false;
    size_t pos = 0;
    for (const WCHAR* p = text; *p; ++p) {
        const WCHAR* repl = nullptr;
        switch (*p) {
            case L'&': repl = L"&amp;"; break;
            case L'<': repl = L"&lt;"; break;
            case L'>': repl = L"&gt;"; break;
            case L'\"': repl = escapeQuotes ? L"&quot;" : nullptr; break;
            case L'\'': repl = escapeQuotes ? L"&apos;" : nullptr; break;
            default: break;
        }
        if (repl) {
            size_t replLen = wcslen(repl);
            if (pos + replLen >= outCount) return false;
            memcpy(out + pos, repl, replLen * sizeof(WCHAR));
            pos += replLen;
        } else {
            if (pos + 1 >= outCount) return false;
            out[pos++] = *p;
        }
    }
    out[pos] = 0;
    return true;
}

// Override for the current user's SAM name, used ONLY by the elevated
// startup-task helper so it can register a task scoped to the REQUESTING
// (often standard/restricted) user instead of the approving admin.  Set from
// the --for-user CLI flag in parse_cli_options.  Empty = no override.
static WCHAR g_forcedStartupUserSam[512] = {};

void set_forced_startup_user_sam(const WCHAR* samName) {
    if (samName && samName[0]) {
        StringCchCopyW(g_forcedStartupUserSam, ARRAY_COUNT(g_forcedStartupUserSam), samName);
    } else {
        g_forcedStartupUserSam[0] = 0;
    }
}

static bool get_startup_task_name(WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    WCHAR userName[512] = {};
    // g_forcedStartupUserSam is set when the elevated startup-task helper runs
    // on behalf of a standard/restricted user: the helper process runs as the
    // approving admin, so get_current_user_sam_name() would stamp the ADMIN's
    // identity.  The override forces the task to be scoped to the REQUESTING
    // user instead.  Empty = no override (use the real current user).
    if (g_forcedStartupUserSam[0]) {
        StringCchCopyW(userName, ARRAY_COUNT(userName), g_forcedStartupUserSam);
    } else if (!get_current_user_sam_name(userName, ARRAY_COUNT(userName))) {
        return false;
    }
    for (WCHAR* p = userName; *p; ++p) {
        if (*p == L'\\' || *p == L'/' || *p == L':' || *p == L'*' || *p == L'?' ||
            *p == L'"' || *p == L'<' || *p == L'>' || *p == L'|') {
            *p = L'_';
        }
    }
    HRESULT hr = StringCchPrintfW(out, outCount, L"%S%ls", STARTUP_TASK_PREFIX, userName);
    return SUCCEEDED(hr);
}

static bool write_startup_task_xml(const WCHAR* xmlPath, const WCHAR* exePath, const WCHAR* cfgPath, char* err, size_t errSize) {
    if (!xmlPath || !exePath || !cfgPath) {
        set_message(err, errSize, "Invalid startup task xml arguments");
        return false;
    }

    const WCHAR* description = L"Notify the Green Curve service of an authenticated user logon.";

    HANDLE h = CreateFileW(xmlPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        set_message(err, errSize, "Cannot create startup task XML (error %lu)", GetLastError());
        return false;
    }

    // The scheduled task is only a bounded per-user service handoff.  All live GPU
    // control, including automatic logon profiles, belongs to the service, so
    // elevation cannot make the task more reliable and would change its token.
    // Resident tray startup is a separate HKCU Run entry using --tray-start.
    const WCHAR* runLevel = L"LeastPrivilege";

    const WCHAR* xmlFmt =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.3\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo>\r\n"
        L"    <Author>%ls</Author>\r\n"
        L"    <Description>%ls</Description>\r\n"
        L"  </RegistrationInfo>\r\n"
        L"  <Triggers>\r\n"
        L"    <LogonTrigger>\r\n"
        L"      <Enabled>true</Enabled>\r\n"
        L"      <UserId>%ls</UserId>\r\n"
        L"    </LogonTrigger>\r\n"
        L"  </Triggers>\r\n"
        L"  <Principals>\r\n"
        L"    <Principal id=\"Author\">\r\n"
        L"      <UserId>%ls</UserId>\r\n"
        L"      <LogonType>InteractiveToken</LogonType>\r\n"
        L"      <RunLevel>%ls</RunLevel>\r\n"
        L"    </Principal>\r\n"
        L"  </Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
        L"    <StartWhenAvailable>true</StartWhenAvailable>\r\n"
        L"    <IdleSettings>\r\n"
        L"      <StopOnIdleEnd>false</StopOnIdleEnd>\r\n"
        L"      <RestartOnIdle>false</RestartOnIdle>\r\n"
        L"    </IdleSettings>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Enabled>true</Enabled>\r\n"
        L"    <Hidden>false</Hidden>\r\n"
        L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
        L"    <WakeToRun>false</WakeToRun>\r\n"
        L"    <ExecutionTimeLimit>%ls</ExecutionTimeLimit>\r\n"
        L"    <Priority>7</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\">\r\n"
        L"    <Exec>\r\n"
        L"      <Command>%ls</Command>\r\n"
        L"      <WorkingDirectory>%ls</WorkingDirectory>\r\n"
        L"      <Arguments>%ls</Arguments>\r\n"
        L"    </Exec>\r\n"
        L"  </Actions>\r\n"
        L"</Task>\r\n";

    WCHAR userName[512] = {};
    // Honor the forced requesting-user override (elevated helper path) so the
    // task UserId/Author reflect the standard user, not the approving admin.
    if (g_forcedStartupUserSam[0]) {
        StringCchCopyW(userName, ARRAY_COUNT(userName), g_forcedStartupUserSam);
    } else if (!get_current_user_sam_name(userName, ARRAY_COUNT(userName))) {
        CloseHandle(h);
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Failed to determine current user");
        return false;
    }

    WCHAR exeEsc[2048] = {};
    WCHAR cfgEsc[2048] = {};
    WCHAR userEsc[1024] = {};
    WCHAR argsEsc[2048] = {};
    WCHAR workDir[MAX_PATH] = {};
    WCHAR workDirEsc[2048] = {};
    // The task can wait up to two minutes for the service to reach RUNNING and
    // deliver its authenticated handoff.  Three minutes leaves a bounded margin
    // for process startup/logging without allowing a stuck task to live forever.
    const WCHAR* executionTimeLimit = L"PT3M";
    StringCchCopyW(workDir, ARRAY_COUNT(workDir), exePath);
    WCHAR* slash = wcsrchr(workDir, L'\\');
    if (!slash) slash = wcsrchr(workDir, L'/');
    if (slash) *slash = 0;
    if (!xml_escape_wide(exePath, exeEsc, ARRAY_COUNT(exeEsc), false) ||
        !xml_escape_wide(cfgPath, cfgEsc, ARRAY_COUNT(cfgEsc), true) ||
        !xml_escape_wide(userName, userEsc, ARRAY_COUNT(userEsc), false) ||
        !xml_escape_wide(workDir, workDirEsc, ARRAY_COUNT(workDirEsc), false)) {
        CloseHandle(h);
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Failed escaping startup task XML");
        return false;
    }

    HRESULT argsHr = StringCchPrintfW(
        argsEsc,
        ARRAY_COUNT(argsEsc),
        L"--logon-start --config &quot;%ls&quot;",
        cfgEsc);
    if (FAILED(argsHr)) {
        CloseHandle(h);
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Startup task arguments too long");
        return false;
    }

    WCHAR xml[8192] = {};
    HRESULT hr = StringCchPrintfW(xml, ARRAY_COUNT(xml), xmlFmt, userEsc, description, userEsc, userEsc, runLevel, executionTimeLimit, exeEsc, workDirEsc, argsEsc);
    if (FAILED(hr)) {
        CloseHandle(h);
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Startup task XML too long");
        return false;
    }

    DWORD bytesToWrite = (DWORD)(wcslen(xml) * sizeof(WCHAR));
    WORD bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(h, &bom, sizeof(bom), &written, nullptr) != 0 && written == sizeof(bom);
    if (ok) ok = WriteFile(h, xml, bytesToWrite, &written, nullptr) != 0 && written == bytesToWrite;
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Failed writing startup task XML (error %lu)", GetLastError());
        return false;
    }
    return true;
}

static bool run_process_wait(const WCHAR* applicationName, WCHAR* commandLine, DWORD timeoutMs, DWORD* exitCode, char* err, size_t errSize) {
    if (exitCode) *exitCode = (DWORD)-1;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(applicationName, commandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        set_message(err, errSize, "CreateProcess failed (%lu)", GetLastError());
        return false;
    }
    DWORD waitResult = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        set_message(err, errSize, "Command timed out");
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exitCode) *exitCode = code;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

static bool run_schtasks_command(const WCHAR* args, DWORD* exitCode, char* err, size_t errSize) {
    WCHAR schtasksPath[MAX_PATH] = {};
    UINT pathLen = GetSystemDirectoryW(schtasksPath, ARRAY_COUNT(schtasksPath));
    if (pathLen == 0 || pathLen >= ARRAY_COUNT(schtasksPath) ||
        FAILED(StringCchCatW(schtasksPath, ARRAY_COUNT(schtasksPath), L"\\schtasks.exe"))) {
        set_message(err, errSize, "Failed locating schtasks.exe");
        return false;
    }

    WCHAR commandLine[2048] = {};
    if (FAILED(StringCchPrintfW(commandLine, ARRAY_COUNT(commandLine), L"\"%ls\" %ls", schtasksPath, args))) {
        set_message(err, errSize, "Scheduled task command too long");
        return false;
    }
    return run_process_wait(schtasksPath, commandLine, 15000, exitCode, err, errSize);
}

static bool is_startup_task_enabled() {
    WCHAR taskName[256] = {};
    if (!get_startup_task_name(taskName, ARRAY_COUNT(taskName))) return false;

    WCHAR queryArgs[512] = {};
    if (FAILED(StringCchPrintfW(queryArgs, ARRAY_COUNT(queryArgs), L"/query /tn \"%ls\"", taskName))) return false;

    DWORD exitCode = 0;
    char err[128] = {};
    if (!run_schtasks_command(queryArgs, &exitCode, err, sizeof(err))) return false;
    return exitCode == 0;
}

static bool wait_for_startup_task_state(bool enabled, DWORD timeoutMs) {
    // Pump the GUI while polling so the window keeps repainting during the wait
    // (same anti-corruption rationale as the service-readiness wait).
    UiInputGuard uiGuard;
    ULONGLONG start = GetTickCount64();
    while ((GetTickCount64() - start) < timeoutMs) {
        if (is_startup_task_enabled() == enabled) return true;
        wait_object_pumping_ui(nullptr, 150);
    }
    return is_startup_task_enabled() == enabled;
}

static bool load_startup_enabled_from_config(const char* path, bool* enabled) {
    if (enabled) *enabled = false;
    if (!path || !enabled) return false;

    if (get_config_int(path, "profiles", "logon_slot", 0) > 0 ||
        get_config_int(path, "profiles", "logon_shared_slot", 0) > 0) {
        *enabled = true;
        return true;
    }

    char buf[16] = {};
    GetPrivateProfileStringA("startup", "apply_on_launch", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (!buf[0]) return false;

    int value = 0;
    if (!parse_int_strict(buf, &value)) return false;
    *enabled = value != 0;
    return true;
}

#ifndef GREEN_CURVE_SERVICE_BINARY
struct StartupSyncThreadContext {
    HWND hwnd;
    LONG generation;
};

static LONG current_startup_sync_generation() {
    return InterlockedCompareExchange(&g_app.startupSyncGeneration, 0, 0);
}

static void invalidate_startup_sync_generation() {
    LONG generation = InterlockedIncrement(&g_app.startupSyncGeneration);
    debug_log("startup sync: invalidated older asynchronous work (generation=%ld)\n",
        generation);
}

static bool load_normalized_logon_selection(
    int* perUserSlotOut, int* sharedSlotOut) {
    if (perUserSlotOut) *perUserSlotOut = 0;
    if (sharedSlotOut) *sharedSlotOut = 0;
    HANDLE configMutex = nullptr;
    if (!g_app.configPath[0] ||
        !enter_config_storage_lock(&configMutex)) return false;
    DWORD attrs = GetFileAttributesA(g_app.configPath);
    int perUserSlot = 0;
    int sharedSlot = 0;
    bool coherent = attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        read_config_int_strict_locked(g_app.configPath, "profiles",
            "logon_slot", 0, &perUserSlot) &&
        read_config_int_strict_locked(g_app.configPath, "profiles",
            "logon_shared_slot", 0, &sharedSlot) &&
        perUserSlot >= 0 && perUserSlot <= CONFIG_NUM_SLOTS &&
        sharedSlot >= 0 && sharedSlot <= CONFIG_NUM_SLOTS &&
        !(perUserSlot > 0 && sharedSlot > 0);
    leave_config_storage_lock(configMutex);
    if (!coherent) {
        debug_log("logon combo sync: config selection is unavailable/incoherent; preserving the saved assignment and current UI selection\n");
        return false;
    }
    // Slot availability is deliberately not normalization authority.  A bank
    // or profile can be transiently absent during an atomic rewrite/mount; only
    // an explicit clear or profile deletion may erase the saved selector.
    if (perUserSlotOut) *perUserSlotOut = perUserSlot;
    if (sharedSlotOut) *sharedSlotOut = sharedSlot;
    return true;
}

static void synchronize_startup_task_preserving_indeterminate(
    const char* configPath, bool taskExists, const char* context) {
    ConfigEnablementState state = startup_task_config_state(configPath);
    if (state == CONFIG_ENABLEMENT_INDETERMINATE) {
        debug_log("startup task sync: config is indeterminate; preserving existing task (%s)\n",
            context && context[0] ? context : "background reconciliation");
        return;
    }
    bool shouldEnable = state == CONFIG_ENABLEMENT_ENABLED;
    if (!shouldEnable && !taskExists) return;
    char err[256] = {};
    if (!set_startup_task_enabled(shouldEnable, err, sizeof(err)) && err[0]) {
        debug_log("startup task sync failed (%s): %s\n",
            context && context[0] ? context : "background reconciliation", err);
    }
}

static void sync_logon_combo_from_system() {
    if (!g_app.hLogonCombo) return;

    char trayErr[256] = {};
    if (!sync_tray_autostart_from_config(g_app.configPath, trayErr,
            sizeof(trayErr))) {
        debug_log("tray autostart sync failed: %s\n",
            trayErr[0] ? trayErr : "unknown error");
    }
    bool taskExists = is_startup_task_enabled();

    // An app-owned task may still exist while carrying an old command, user,
    // run level, or fixed trigger delay.  Ask the task helper to verify/repair every
    // enabled task instead of treating existence as proof of a healthy definition.
    synchronize_startup_task_preserving_indeterminate(
        g_app.configPath, taskExists, "UI-thread reconciliation");

    // Read the selection on the UI thread after synchronization.  Never apply
    // an item-data snapshot captured by background work: the user may have
    // changed the selection while Task Scheduler or UAC was still busy.
    int logonSlot = 0;
    int logonSharedSlot = 0;
    if (load_normalized_logon_selection(&logonSlot, &logonSharedSlot)) {
        LRESULT itemData = logon_combo_item_data_from_slots(
            logonSlot, logonSharedSlot);
        if (!select_logon_combo_item_by_data(g_app.hLogonCombo, itemData)) {
            debug_log("logon combo sync: configured item data %lld is not currently present; keeping the combo unchanged\n",
                (long long)itemData);
        }
    }
    update_profile_state_label();
}

static DWORD WINAPI logon_sync_thread_proc(void* param) {
    StartupSyncThreadContext* context =
        static_cast<StartupSyncThreadContext*>(param);
    if (!context) return 0;
    HWND hwnd = context->hwnd;
    LONG generation = context->generation;
    HeapFree(GetProcessHeap(), 0, context);

    char trayErr[256] = {};
    if (!sync_tray_autostart_from_config(g_app.configPath, trayErr,
            sizeof(trayErr))) {
        debug_log("tray autostart async sync failed (generation=%ld): %s\n",
            generation, trayErr[0] ? trayErr : "unknown error");
    }
    bool taskExists = is_startup_task_enabled();

    // See sync_logon_combo_from_system(): enabled tasks are definition-checked
    // on every sync so legacy/manual scheduler edits self-heal.
    synchronize_startup_task_preserving_indeterminate(
        g_app.configPath, taskExists, "asynchronous reconciliation");
    if (!PostMessageA(hwnd, APP_WM_SYNC_STARTUP, (WPARAM)generation, 0)) {
        debug_log("startup sync: could not post completion for generation %ld (error %lu)\n",
            generation, GetLastError());
    }
    return 0;
}

static void schedule_logon_combo_sync() {
    if (!g_app.hMainWnd) return;
    LONG generation = InterlockedIncrement(&g_app.startupSyncGeneration);
    if (g_app.startupSyncInFlight) {
        debug_log("startup sync: queued generation %ld behind in-flight work\n",
            generation);
        return;
    }
    g_app.startupSyncInFlight = true;
    StartupSyncThreadContext* context = static_cast<StartupSyncThreadContext*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(StartupSyncThreadContext)));
    if (!context) {
        g_app.startupSyncInFlight = false;
        debug_log("startup sync: context allocation failed; using UI-thread synchronization\n");
        sync_logon_combo_from_system();
        return;
    }
    context->hwnd = g_app.hMainWnd;
    context->generation = generation;
    DWORD threadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, logon_sync_thread_proc, context, 0, &threadId);
    if (!thread) {
        HeapFree(GetProcessHeap(), 0, context);
        g_app.startupSyncInFlight = false;
        close_startup_sync_thread_handle();
        debug_log("startup sync: worker creation failed (error %lu); using UI-thread synchronization\n",
            GetLastError());
        sync_logon_combo_from_system();
        return;
    }
    close_startup_sync_thread_handle();
    g_app.hStartupSyncThread = thread;
}
#endif

// Register/unregister the per-user logon task via a direct (non-elevated)
// schtasks call as the current user.  The task is always LeastPrivilege because
// the service, never the GUI task, owns hardware control.  Returns true on
// success; on failure fills err.
// The caller decides whether a failure warrants an elevated-helper fallback.
// outNeedsElevation (optional) is set true when the direct path failed because
// the operation requires administrator rights (e.g. deleting a task created by
// an admin / at HighestAvailable), so the caller can retry via the elevated
// helper.  nullptr when the caller is already elevated.
static bool set_startup_task_enabled_direct(bool enabled, bool* outNeedsElevation, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (outNeedsElevation) *outNeedsElevation = false;

    WCHAR taskName[256] = {};
    if (!get_startup_task_name(taskName, ARRAY_COUNT(taskName))) {
        set_message(err, errSize, "Failed to determine startup task name");
        return false;
    }

    DWORD exitCode = 0;
    if (!enabled) {
        if (!is_startup_task_enabled()) return true;
        WCHAR deleteArgs[512] = {};
        if (FAILED(StringCchPrintfW(deleteArgs, ARRAY_COUNT(deleteArgs), L"/delete /tn \"%ls\" /f", taskName))) {
            set_message(err, errSize, "Scheduled task delete command too long");
            return false;
        }
        debug_log("startup task: deleting \"%ls\" (direct, elevated=%d)\n", taskName, is_elevated() ? 1 : 0);
        if (!run_schtasks_command(deleteArgs, &exitCode, err, errSize)) {
            debug_log("startup task: schtasks /delete failed to launch: %s\n", err[0] ? err : "unknown");
            return false;
        }
        debug_log("startup task: schtasks /delete exit=%lu\n", exitCode);
        // Verify by STATE, not the exit code.  schtasks returns non-zero for
        // BOTH "already absent" and "access denied", so the old accept-exit-1
        // logic treated an access-denied delete as success, then reported a
        // dead-end "still exists" error instead of retrying elevated.
        if (wait_for_startup_task_state(false, 3000)) {
            debug_log("startup task: confirmed removed\n");
            return true;
        }
        // Task still present: almost always because it requires elevation to
        // delete (created by an admin / at HighestAvailable).  Signal the caller
        // to retry via the elevated helper rather than failing outright.
        if (outNeedsElevation) *outNeedsElevation = true;
        set_message(err, errSize,
            "Startup task could not be removed without administrator rights (schtasks exit %lu)", exitCode);
        debug_log("startup task: still present after direct delete (exit %lu); needs elevation\n", exitCode);
        return false;
    }

    WCHAR exePath[MAX_PATH] = {};
    WCHAR cfgPath[MAX_PATH] = {};
    WCHAR xmlPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, ARRAY_COUNT(exePath));
    if (!utf8_to_wide(g_app.configPath, cfgPath, ARRAY_COUNT(cfgPath))) {
        set_message(err, errSize, "Failed converting config path");
        return false;
    }

    // Ensure user data dir is resolved so we can write the XML to a safe location
    // (the config directory) instead of %TEMP%, which is vulnerable to junction attacks.
    char pathErr[256] = {};
    if (!resolve_data_paths(pathErr, sizeof(pathErr))) {
        set_message(err, errSize, "Failed resolving data paths: %s", pathErr);
        return false;
    }
    WCHAR userDataDirW[MAX_PATH] = {};
    if (!utf8_to_wide(g_userDataDir, userDataDirW, ARRAY_COUNT(userDataDirW))) {
        set_message(err, errSize, "Failed converting user data directory");
        return false;
    }
    if (FAILED(StringCchPrintfW(xmlPath, ARRAY_COUNT(xmlPath), L"%s\\startup_task.xml", userDataDirW))) {
        set_message(err, errSize, "Failed constructing startup task XML path");
        return false;
    }

    // Do not consider a same-named task healthy by existence alone.  Security-
    // relevant identity/action mismatches are broken and require repair.  Older
    // correct-user/correct-action definitions with a delay, elevation, or old
    // execution limit remain functional; we attempt to canonicalize them but do
    // not disable a saved logon choice merely because best-effort cleanup fails.
    bool taskExisted = is_startup_task_enabled();
    bool compatibleLegacyTask = false;
    if (taskExisted) {
        char validation[512] = {};
        StartupTaskDefinitionClass classification = startup_task_definition_classify_current(
            taskName, exePath, cfgPath, validation, sizeof(validation));
        if (classification == STARTUP_TASK_DEFINITION_CANONICAL) {
            debug_log("startup task: definition verified for \"%ls\": %s\n",
                taskName, validation[0] ? validation : "current");
            return true;
        }
        compatibleLegacyTask = classification == STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY;
        debug_log("startup task: definition %s for \"%ls\": %s; canonical repair will be attempted%s\n",
            compatibleLegacyTask ? "compatible legacy" : "broken or unreadable",
            taskName,
            validation[0] ? validation : "XML validation failed",
            compatibleLegacyTask ? " best-effort" : "");
    } else {
        debug_log("startup task: no existing task named \"%ls\"; creating expected definition\n", taskName);
    }

    auto accept_still_functional_legacy_task = [&](const char* repairFailure) -> bool {
        if (!compatibleLegacyTask) return false;
        char validation[512] = {};
        StartupTaskDefinitionClass classification = startup_task_definition_classify_current(
            taskName, exePath, cfgPath, validation, sizeof(validation));
        if (classification != STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY &&
            classification != STARTUP_TASK_DEFINITION_CANONICAL) {
            debug_log("startup task: legacy repair failed and original task is no longer functional: %s (%s)\n",
                repairFailure && repairFailure[0] ? repairFailure : "unknown failure",
                validation[0] ? validation : "definition query failed");
            return false;
        }
        debug_log("startup task: canonical repair was best-effort and failed (%s); preserving functional legacy definition: %s\n",
            repairFailure && repairFailure[0] ? repairFailure : "unknown failure",
            validation[0] ? validation : "compatible legacy task");
        if (err && errSize) err[0] = 0;
        if (outNeedsElevation) *outNeedsElevation = false;
        return true;
    };

    if (!write_startup_task_xml(xmlPath, exePath, cfgPath, err, errSize)) {
        DeleteFileW(xmlPath);
        if (accept_still_functional_legacy_task(err && err[0] ? err : "could not build canonical XML")) return true;
        return false;
    }

    WCHAR createArgs[2048] = {};
    HRESULT hr = StringCchPrintfW(createArgs, ARRAY_COUNT(createArgs),
        L"/create /f /tn \"%ls\" /xml \"%ls\"",
        taskName, xmlPath);
    if (FAILED(hr)) {
        DeleteFileW(xmlPath);
        set_message(err, errSize, "Scheduled task create command too long");
        return false;
    }

    debug_log("startup task: creating \"%ls\" (direct, elevated=%d)\n", taskName, is_elevated() ? 1 : 0);
    bool runOk = run_schtasks_command(createArgs, &exitCode, err, errSize);
    DeleteFileW(xmlPath);
    if (!runOk) {
        debug_log("startup task: schtasks /create failed to launch: %s\n", err[0] ? err : "unknown");
        if (accept_still_functional_legacy_task(err && err[0] ? err : "schtasks did not launch")) return true;
        return false;
    }
    debug_log("startup task: schtasks /create exit=%lu\n", exitCode);
    if (exitCode != 0) {
        char repairFailure[128] = {};
        StringCchPrintfA(repairFailure, ARRAY_COUNT(repairFailure), "schtasks exited %lu", exitCode);
        if (accept_still_functional_legacy_task(repairFailure)) return true;
        // schtasks does not give this helper structured stderr, so a protected
        // legacy task that cannot even be queried looks the same as a known
        // stale HighestAvailable task.  Ask the existing elevated helper for
        // one replacement attempt; it preserves the original requesting user
        // through --for-user instead of leaving that stale task in place.
        if (outNeedsElevation) *outNeedsElevation = true;
        set_message(err, errSize, "Failed creating or replacing startup task (exit %lu)", exitCode);
        return false;
    }
    if (!wait_for_startup_task_state(true, 3000)) {
        if (accept_still_functional_legacy_task("replacement did not become queryable")) return true;
        // A create that ran cleanly but did not persist is most likely an
        // elevation/registration issue — let the caller retry elevated.
        if (outNeedsElevation) *outNeedsElevation = true;
        set_message(err, errSize, "Startup task creation did not persist");
        return false;
    }
    char validation[512] = {};
    StartupTaskDefinitionClass createdClassification = startup_task_definition_classify_current(
        taskName, exePath, cfgPath, validation, sizeof(validation));
    if (createdClassification == STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY) {
        debug_log("startup task: scheduler retained a functional compatible definition for \"%ls\": %s\n",
            taskName, validation[0] ? validation : "compatible legacy task");
        return true;
    }
    if (createdClassification != STARTUP_TASK_DEFINITION_CANONICAL) {
        debug_log("startup task: created task \"%ls\" failed XML verification: %s\n",
            taskName, validation[0] ? validation : "unknown validation error");
        set_message(err, errSize, "Startup task was created but its definition could not be verified: %s",
            validation[0] ? validation : "unknown validation error");
        return false;
    }
    debug_log("startup task: created/repaired and verified \"%ls\": %s\n",
        taskName, validation[0] ? validation : "current");
    return true;
}

// Heuristic: does an schtasks/CLI error string indicate an access-denied /
// elevation-required condition (rather than a genuine logic failure)?  When the
// direct path is denied we fall back to the elevated helper; for all other
// errors the elevated helper would fail identically, so we surface them.
static bool err_returned_access_denied(const char* err) {
    if (!err) return false;
    // run_schtasks_command surfaces the underlying error in its message; the
    // schtasks tool and CreateProcess report access-denied as "Access is denied"
    // or a system error code.  Match the common phrasings.
    if (strstr(err, "Access is denied")) return true;
    if (strstr(err, "access denied")) return true;
    if (strstr(err, "denied")) return true;
    if (strstr(err, "(0x5)")) return true;     // ERROR_ACCESS_DENIED
    if (strstr(err, "error 5")) return true;   // ERROR_ACCESS_DENIED
    if (strstr(err, "elevation")) return true;
    return false;
}

static bool set_startup_task_enabled(bool enabled, char* err, size_t errSize) {
    if (!is_elevated()) {
        // The logon task is a per-user InteractiveToken task scoped to the
        // current user.  Windows lets a user register this LeastPrivilege task
        // in their own context without elevation.  Bouncing through UAC would
        // stamp the approving admin's identity into the task name/UserId/
        // Principal, so it could fire at the wrong account's logon.  Direct
        // registration is therefore the normal path; elevation only repairs a
        // protected legacy task or a direct access-denied failure.
        bool needsElevation = false;
        if (set_startup_task_enabled_direct(enabled, &needsElevation, err, errSize)) {
            debug_log("startup task: registered via direct (non-elevated) path for the current user\n");
            return true;
        }
        if (!needsElevation && !err_returned_access_denied(err)) {
            return false;
        }
        set_message(err, errSize, "");
        debug_log("startup task: direct path needs elevation to repair a legacy task; falling back to elevated helper\n");
        return launch_startup_task_admin_helper(enabled, err, errSize);
    }

    // Elevated: register directly as the current (admin) user.  When invoked
    // via the elevated helper, the helper must stamp the REQUESTING user, not
    // the approver — handled by launch_startup_task_admin_helper forwarding the
    // original user identity (see write_startup_task_xml override path).
    return set_startup_task_enabled_direct(enabled, nullptr, err, errSize);
}
