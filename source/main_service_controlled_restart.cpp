// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Process-bound Windows driver-recovery restart. The service launches this
// minimal helper and waits for its validation handshake before it commits to
// the dedicated clean exit. Only that exact exit lets the helper demand-start
// the service with the nonce-bound controlled-recovery argument.

#define SERVICE_CONTROLLED_RECOVERY_EXIT_CODE 0xE0474352u
#define SERVICE_CONTROLLED_HELPER_READY_TIMEOUT_MS 15000u
#define SERVICE_CONTROLLED_HELPER_PARENT_TIMEOUT_MS SERVICE_CONTROLLED_RECOVERY_MAX_AGE_MS

// NotifyServiceStatusChange keeps the caller-supplied SERVICE_NOTIFY storage
// live until its callback is delivered or the service handle is closed.  The
// restart helper performs exactly one such wait and exits immediately
// afterwards, so pin the storage for the helper process lifetime.  This also
// keeps timeout/error cleanup safe without a polling fallback or a racy stack
// notification buffer.
static SERVICE_NOTIFYW* g_serviceControlledHelperStopNotification = nullptr;
static HANDLE g_serviceControlledHelperAlertableWaitEvent = nullptr;

static bool service_parse_decimal_dword(const WCHAR* textValue, DWORD* out) {
    if (!textValue || !textValue[0] || !out) return false;
    WCHAR* end = nullptr; errno = 0;
    unsigned long value = wcstoul(textValue, &end, 10);
    if (errno == ERANGE || !end || *end != 0 || wcsspn(textValue, L"0123456789") != wcslen(textValue) || value == 0 || value > MAXDWORD) return false;
    *out = (DWORD)value;
    return true;
}

static bool service_parse_handle_value(const WCHAR* textValue, HANDLE* out) {
    if (!textValue || !textValue[0] || !out) return false;
    WCHAR* end = nullptr; errno = 0;
    unsigned long long value = _wcstoui64(textValue, &end, 10);
    if (errno == ERANGE || !end || *end != 0 || wcsspn(textValue, L"0123456789") != wcslen(textValue) || value == 0 || value > (unsigned long long)UINTPTR_MAX) return false;
    *out = (HANDLE)(uintptr_t)value;
    return true;
}

static void service_clear_controlled_recovery_files() {
    service_clear_controlled_recovery_authorization();
    service_clear_restart_reapply_snapshot();
}

static VOID CALLBACK service_controlled_helper_status_callback(PVOID) {
    // NotifyServiceStatusChangeW fills the pinned SERVICE_NOTIFYW structure
    // before delivering this APC.  The registering helper thread inspects it
    // after the alertable wait returns; no SCM call is safe or necessary here.
}

static bool service_query_current_status(SC_HANDLE service,
    SERVICE_STATUS_PROCESS* statusOut) {
    if (!service || !statusOut) return false;
    memset(statusOut, 0, sizeof(*statusOut));
    DWORD needed = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(statusOut), sizeof(*statusOut), &needed)) {
        return false;
    }
    return true;
}

static bool service_wait_for_scm_stopped_notification(
    SC_HANDLE service, DWORD expectedPreviousProcessId, DWORD timeoutMs)
{
    SERVICE_STATUS_PROCESS status = {};
    if (!service_query_current_status(service, &status)) {
        debug_log("controlled recovery helper: initial SCM status query failed (error=%lu)\n",
            GetLastError());
        return false;
    }
    ServiceControlledRecoveryScmStopDisposition disposition =
        service_classify_controlled_recovery_scm_stop_state(
            status.dwCurrentState == SERVICE_STOPPED,
            status.dwCurrentState == SERVICE_STOP_PENDING,
            status.dwProcessId, expectedPreviousProcessId);
    if (disposition == SERVICE_CONTROLLED_RECOVERY_SCM_STOPPED) return true;
    // QueryServiceStatusEx documents dwProcessId as potentially invalid while
    // STOP_PENDING. The helper has already observed the pinned old process's
    // dedicated exit, so that state is safe to wait through without trusting
    // its PID. Every other state still has to name the exact old generation;
    // an intervening service generation is rejected before notification setup.
    if (disposition == SERVICE_CONTROLLED_RECOVERY_SCM_REJECT) {
        debug_log("controlled recovery helper: SCM state is not tied to the expected old generation (state=%lu pid=%lu expectedPid=%lu)\n",
            (unsigned long)status.dwCurrentState,
            (unsigned long)status.dwProcessId,
            (unsigned long)expectedPreviousProcessId);
        return false;
    }
    debug_log("controlled recovery helper: waiting for SCM STOPPED publication (state=%lu pid=%lu expectedPid=%lu stopPending=%d)\n",
        (unsigned long)status.dwCurrentState,
        (unsigned long)status.dwProcessId,
        (unsigned long)expectedPreviousProcessId,
        status.dwCurrentState == SERVICE_STOP_PENDING ? 1 : 0);
    if (timeoutMs == 0 || g_serviceControlledHelperStopNotification) return false;

    SERVICE_NOTIFYW* notification = reinterpret_cast<SERVICE_NOTIFYW*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SERVICE_NOTIFYW)));
    if (!notification) return false;
    // Keep this allocation pinned until the short-lived helper exits.  On a
    // timeout, closing the service handle cancels the subscription while the
    // storage remains valid for any already-queued APC.
    g_serviceControlledHelperStopNotification = notification;
    g_serviceControlledHelperAlertableWaitEvent = CreateEventW(
        nullptr, TRUE, FALSE, nullptr);
    if (!g_serviceControlledHelperAlertableWaitEvent) {
        debug_log("controlled recovery helper: alertable wait event creation failed (error=%lu)\n",
            GetLastError());
        return false;
    }
    notification->dwVersion = SERVICE_NOTIFY_STATUS_CHANGE;
    notification->pfnNotifyCallback = service_controlled_helper_status_callback;
    DWORD notifyResult = NotifyServiceStatusChangeW(service,
        SERVICE_NOTIFY_STOPPED | SERVICE_NOTIFY_DELETE_PENDING, notification);
    if (notifyResult != ERROR_SUCCESS) {
        debug_log("controlled recovery helper: SCM stop notification registration failed (error=%lu)\n",
            (unsigned long)notifyResult);
        return false;
    }

    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (notification->dwNotificationTriggered == 0 &&
           notification->dwNotificationStatus == ERROR_SUCCESS) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            debug_log("controlled recovery helper: timed out waiting for the old SCM generation to become STOPPED\n");
            return false;
        }
        DWORD remaining = static_cast<DWORD>(deadline - now);
        DWORD wait = WaitForSingleObjectEx(
            g_serviceControlledHelperAlertableWaitEvent, remaining, TRUE);
        if (wait == WAIT_TIMEOUT) {
            debug_log("controlled recovery helper: timed out waiting for the old SCM generation to become STOPPED\n");
            return false;
        }
        if (wait != WAIT_IO_COMPLETION) {
            debug_log("controlled recovery helper: alertable SCM wait failed (wait=%lu error=%lu)\n",
                (unsigned long)wait, GetLastError());
            return false;
        }
    }
    if (notification->dwNotificationStatus != ERROR_SUCCESS ||
        (notification->dwNotificationTriggered &
            SERVICE_NOTIFY_DELETE_PENDING) != 0 ||
        (notification->dwNotificationTriggered & SERVICE_NOTIFY_STOPPED) == 0) {
        debug_log("controlled recovery helper: SCM stop notification rejected (status=%lu trigger=0x%08lX)\n",
            (unsigned long)notification->dwNotificationStatus,
            (unsigned long)notification->dwNotificationTriggered);
        return false;
    }

    // Event-driven wakeups still receive one final readback before the single
    // StartServiceW call.  This is verification, not status polling.
    if (!service_query_current_status(service, &status) ||
        status.dwCurrentState != SERVICE_STOPPED) {
        debug_log("controlled recovery helper: final SCM STOPPED readback failed (state=%lu error=%lu)\n",
            (unsigned long)status.dwCurrentState, GetLastError());
        return false;
    }
    return true;
}

static bool service_controlled_recovery_remaining_freshness(
    const ServiceControlledRecoveryAuthorization& authorization,
    DWORD* remainingMsOut)
{
    if (!remainingMsOut || authorization.createdUptimeMs == 0) return false;
    *remainingMsOut = 0;
    ULONGLONG now = GetTickCount64();
    if (now == 0 || authorization.createdUptimeMs > now) return false;
    ULONGLONG ageMs = now - authorization.createdUptimeMs;
    if (ageMs >= SERVICE_CONTROLLED_RECOVERY_MAX_AGE_MS) return false;
    ULONGLONG remainingMs = SERVICE_CONTROLLED_RECOVERY_MAX_AGE_MS - ageMs;
    if (remainingMs == 0 || remainingMs > MAXDWORD) return false;
    *remainingMsOut = static_cast<DWORD>(remainingMs);
    return true;
}

static bool service_start_from_controlled_helper(const char* nonceHex,
    const ServiceControlledRecoveryAuthorization& authorization,
    DWORD stopWaitTimeoutMs) {
    WCHAR nonceWide[SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS + 1] = {};
    if (!nonceHex || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            nonceHex, -1, nonceWide, ARRAY_COUNT(nonceWide)) <= 0) return false;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE service = OpenServiceW(scm, L"GreenCurveService",
        SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(scm);
        return false;
    }
    bool stopped = service_wait_for_scm_stopped_notification(
        service, authorization.previousProcessId, stopWaitTimeoutMs);
    // Waiting may consume most of the authorization lifetime or overlap an
    // external state change. Revalidate nonce, snapshot fingerprint, process
    // identity, boot and freshness immediately before the sole start attempt.
    bool authorizationStillValid = stopped &&
        service_read_controlled_recovery_authorization(nonceHex,
            authorization.previousProcessId,
            authorization.previousProcessCreationTime100ns,
            false, nullptr);
    const WCHAR* arguments[2] = {
        L"--controlled-recovery",
        nonceWide,
    };
    bool started = authorizationStillValid &&
        StartServiceW(service, 2, arguments) != FALSE;
    if (!authorizationStillValid) {
        debug_log("controlled recovery helper: authorization became invalid before the sole start attempt\n");
    } else if (!started) {
        debug_log("controlled recovery helper: sole SCM start attempt failed (error=%lu)\n",
            GetLastError());
    }
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return started;
}

static int service_run_controlled_restart_helper(DWORD parentProcessId,
    HANDLE parentProcess, HANDLE readyEvent, const char* nonceHex) {
    g_app.isServiceProcess = true;
    // Keep this pre-handshake process deliberately minimal.  The normal
    // service path resolver reaches WTS/profile/Shell APIs and mutates the
    // process-wide user-path cache; it is initialized as part of service_main,
    // not for this standalone helper mode.  Calling it here caused the helper
    // to AV in ntdll before it could signal the ready event.  Parent-side
    // diagnostics below retain the exact helper exit code without introducing
    // that unrelated startup surface before recovery authorization is pinned.
    ServiceControlledRecoveryAuthorization authorization = {};
    bool valid = service_read_controlled_recovery_authorization(nonceHex,
        parentProcessId, 0, false, &authorization);
    if (valid) valid = service_controlled_recovery_parent_matches(
        parentProcess, authorization);
    if (valid) {
        valid = service_mark_controlled_recovery_helper_validated(nonceHex,
            authorization.previousProcessId,
            authorization.previousProcessCreationTime100ns);
    }
    if (!valid) {
        debug_log("controlled recovery helper: initial nonce/parent validation failed for pid=%lu\n",
            (unsigned long)parentProcessId);
        service_clear_controlled_recovery_files();
        SecureZeroMemory(&authorization, sizeof(authorization));
        return 2;
    }

    // The parent does not commit its restart until this validation handshake.
    if (!SetEvent(readyEvent)) {
        debug_log("controlled recovery helper: parent handshake signal failed (error=%lu)\n",
            GetLastError());
        service_clear_controlled_recovery_files();
        SecureZeroMemory(&authorization, sizeof(authorization));
        return 3;
    }
    CloseHandle(readyEvent);
    readyEvent = nullptr;

    DWORD wait = WaitForSingleObject(parentProcess,
        SERVICE_CONTROLLED_HELPER_PARENT_TIMEOUT_MS);
    DWORD exitCode = 0;
    bool cleanExit = wait == WAIT_OBJECT_0 &&
        GetExitCodeProcess(parentProcess, &exitCode) &&
        exitCode == SERVICE_CONTROLLED_RECOVERY_EXIT_CODE;
    if (!cleanExit) {
        debug_log("controlled recovery helper: parent exit validation failed (wait=%lu exit=0x%08lX expected=0x%08lX error=%lu)\n",
            (unsigned long)wait, (unsigned long)exitCode,
            (unsigned long)SERVICE_CONTROLLED_RECOVERY_EXIT_CODE,
            GetLastError());
        CloseHandle(parentProcess);
        parentProcess = nullptr;
        service_clear_controlled_recovery_files();
        SecureZeroMemory(&authorization, sizeof(authorization));
        return 4;
    }

    // Revalidate freshness, nonce, snapshot fingerprint, and parent identity
    // after the old process exits. A changed/stale file cannot be carried into
    // the new service process.
    valid = service_read_controlled_recovery_authorization(nonceHex,
        authorization.previousProcessId,
        authorization.previousProcessCreationTime100ns,
        false, nullptr);
    DWORD stopWaitTimeoutMs = 0;
    if (valid) {
        valid = service_controlled_recovery_remaining_freshness(
            authorization, &stopWaitTimeoutMs);
    }
    if (valid) {
        valid = service_start_from_controlled_helper(nonceHex,
            authorization, stopWaitTimeoutMs);
    }
    // Keep the exact old process object open through SCM generation
    // verification and the sole start attempt so its PID cannot be recycled
    // into an unrelated process while we compare SERVICE_STATUS_PROCESS.
    CloseHandle(parentProcess);
    parentProcess = nullptr;
    SecureZeroMemory(&authorization, sizeof(authorization));
    if (!valid) {
        debug_log("controlled recovery helper: restart handoff failed closed; clearing continuation state\n");
        service_clear_controlled_recovery_files();
        return 5;
    }
    debug_log("controlled recovery helper: SCM accepted nonce-bound restart\n");
    return 0;
}

static bool service_try_dispatch_controlled_restart_helper(int* exitCodeOut) {
    if (exitCodeOut) *exitCodeOut = 0;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool helper = argc > 1 && argv[1] &&
        _wcsicmp(argv[1], L"--recovery-restart-helper") == 0;
    if (!helper) {
        LocalFree(argv);
        return false;
    }

    int exitCode = 1;
    DWORD parentProcessId = 0;
    HANDLE parentProcess = nullptr;
    HANDLE readyEvent = nullptr;
    char nonceHex[SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS + 1] = {};
    if (argc == 6 &&
        service_parse_decimal_dword(argv[2], &parentProcessId) &&
        service_parse_handle_value(argv[3], &parentProcess) &&
        service_parse_handle_value(argv[4], &readyEvent) &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[5], -1,
            nonceHex, ARRAY_COUNT(nonceHex), nullptr, nullptr) > 0 &&
        strlen(nonceHex) == SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS) {
        exitCode = service_run_controlled_restart_helper(parentProcessId,
            parentProcess, readyEvent, nonceHex);
        parentProcess = nullptr;
        readyEvent = nullptr;
    } else {
        service_clear_controlled_recovery_files();
    }
    if (parentProcess) CloseHandle(parentProcess);
    if (readyEvent) CloseHandle(readyEvent);
    SecureZeroMemory(nonceHex, sizeof(nonceHex));
    LocalFree(argv);
    if (exitCodeOut) *exitCodeOut = exitCode;
    return true;
}

static bool service_launch_controlled_restart_helper(const char* nonceHex,
    HANDLE* helperProcessOut, char* err, size_t errSize) {
    if (helperProcessOut) *helperProcessOut = nullptr;
    if (!nonceHex || strlen(nonceHex) != SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS) {
        set_message(err, errSize, "Controlled-recovery nonce is invalid");
        return false;
    }

    HANDLE inheritedParent = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
            GetCurrentProcess(), &inheritedParent,
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) {
        set_message(err, errSize, "Could not create synchronized parent handle (error %lu)",
            GetLastError());
        return false;
    }
    SECURITY_ATTRIBUTES readySecurity = {};
    readySecurity.nLength = sizeof(readySecurity);
    readySecurity.bInheritHandle = TRUE;
    HANDLE readyEvent = CreateEventW(&readySecurity, TRUE, FALSE, nullptr);
    if (!readyEvent) {
        CloseHandle(inheritedParent);
        set_message(err, errSize, "Could not create helper handshake event (error %lu)",
            GetLastError());
        return false;
    }

    WCHAR modulePath[MAX_PATH] = {};
    DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath, ARRAY_COUNT(modulePath));
    WCHAR nonceWide[SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS + 1] = {};
    if (moduleLength == 0 || moduleLength >= ARRAY_COUNT(modulePath) ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, nonceHex, -1,
            nonceWide, ARRAY_COUNT(nonceWide)) <= 0) {
        CloseHandle(readyEvent);
        CloseHandle(inheritedParent);
        set_message(err, errSize, "Could not resolve controlled-recovery helper path/nonce");
        return false;
    }

    WCHAR commandLine[1024] = {};
    if (FAILED(StringCchPrintfW(commandLine, ARRAY_COUNT(commandLine),
            L"\"%ls\" --recovery-restart-helper %lu %llu %llu %ls",
            modulePath, (unsigned long)GetCurrentProcessId(),
            (unsigned long long)(uintptr_t)inheritedParent,
            (unsigned long long)(uintptr_t)readyEvent, nonceWide))) {
        CloseHandle(readyEvent);
        CloseHandle(inheritedParent);
        set_message(err, errSize, "Controlled-recovery helper command line is too long");
        return false;
    }

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    PPROC_THREAD_ATTRIBUTE_LIST attributes =
        (PPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attributeBytes);
    HANDLE inheritedHandles[2] = { inheritedParent, readyEvent };
    bool attributesReady = attributes &&
        InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes) &&
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr);
    if (!attributesReady) {
        if (attributes) {
            DeleteProcThreadAttributeList(attributes);
            HeapFree(GetProcessHeap(), 0, attributes);
        }
        CloseHandle(readyEvent);
        CloseHandle(inheritedParent);
        set_message(err, errSize, "Could not restrict helper handle inheritance (error %lu)",
            GetLastError());
        return false;
    }

    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process = {};
    bool launched = CreateProcessW(modulePath, commandLine, nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &startup.StartupInfo, &process) != FALSE;
    DWORD launchError = launched ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    HeapFree(GetProcessHeap(), 0, attributes);
    CloseHandle(inheritedParent);
    if (!launched) {
        CloseHandle(readyEvent);
        set_message(err, errSize, "Could not launch controlled-recovery helper (error %lu)",
            launchError);
        return false;
    }
    CloseHandle(process.hThread);

    HANDLE handshake[2] = { readyEvent, process.hProcess };
    DWORD wait = WaitForMultipleObjects(2, handshake, FALSE,
        SERVICE_CONTROLLED_HELPER_READY_TIMEOUT_MS);
    CloseHandle(readyEvent);
    if (wait != WAIT_OBJECT_0) {
        DWORD helperExitCode = STILL_ACTIVE;
        if (wait == WAIT_OBJECT_0 + 1) {
            GetExitCodeProcess(process.hProcess, &helperExitCode);
        } else {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            GetExitCodeProcess(process.hProcess, &helperExitCode);
        }
        CloseHandle(process.hProcess);
        set_message(err, errSize,
            "Controlled-recovery helper validation handshake failed (wait=%lu exit=0x%08lX)",
            wait, (unsigned long)helperExitCode);
        return false;
    }
    if (helperProcessOut) *helperProcessOut = process.hProcess;
    else CloseHandle(process.hProcess);
    return true;
}

static bool service_prepare_controlled_restart(const char* reason,
    char* err, size_t errSize) {
    if (InterlockedCompareExchange(&g_serviceRestartPreparing, 1, 0) != 0) {
        set_message(err, errSize, "A controlled restart is already being prepared");
        return false;
    }
    if (InterlockedExchangeAdd(&g_serviceExternalStopRequested, 0) != 0) {
        InterlockedExchange(&g_serviceRestartPreparing, 0);
        set_message(err, errSize,
            "External SCM stop/shutdown superseded controlled recovery");
        return false;
    }
    char nonceHex[SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS + 1] = {};
    if (!service_create_controlled_recovery_authorization(reason,
            nonceHex, sizeof(nonceHex)) ||
        !service_launch_controlled_restart_helper(nonceHex,
            &g_serviceRestartHelperProcess, err, errSize)) {
        service_clear_controlled_recovery_files();
        service_abandon_current_recovery_evidence();
        InterlockedExchange(&g_serviceRestartRequested, 0);
        InterlockedExchange(&g_serviceRestartPreparing, 0);
        SecureZeroMemory(nonceHex, sizeof(nonceHex));
        return false;
    }
    if (InterlockedExchangeAdd(&g_serviceExternalStopRequested, 0) != 0) {
        service_abort_controlled_restart(
            "external SCM stop/shutdown arrived during helper preparation");
        SecureZeroMemory(nonceHex, sizeof(nonceHex));
        set_message(err, errSize,
            "External SCM stop/shutdown superseded controlled recovery");
        return false;
    }
    service_record_restart_event(); // coalesces with authorization evidence
    SecureZeroMemory(nonceHex, sizeof(nonceHex));
    InterlockedExchange(&g_serviceRestartRequested, 1);
    InterlockedExchange(&g_serviceRestartPreparing, 0);
    return true;
}

// A confirmed stale-driver fault can destroy the thread that owns the runtime
// mutex, and a driver DLL call can wedge that owner forever.  Waiting for that
// mutex here would make recovery impossible.  This emergency path is therefore
// strictly file/process based: close the client gate, bind the already-durable
// active snapshot to a fresh nonce, validate the helper, report a clean SCM
// stop, and exit.  It never reads mutable live intent and never calls hardware.
[[noreturn]] static void service_emergency_restart_from_poisoned_runtime(
    const char* reason, bool corroboratedDriverFailure) {
    InterlockedExchange(&g_serviceRuntimeLockPoisoned, 1);
    if (corroboratedDriverFailure) {
        InterlockedExchange(&g_serviceRuntimePoisonCorroborated, 1);
    }
    InterlockedExchange(&g_serviceClientRequestsReady, 0);

    bool prepared = false;
    char err[256] = {};
    ServiceRecoveryEvidenceKey diagnosticEvidence = {};
    bool evidenceRecorded = !corroboratedDriverFailure ||
        service_record_current_recovery_evidence(
            SERVICE_RECOVERY_EVIDENCE_DRIVER, &diagnosticEvidence);
    if (corroboratedDriverFailure && evidenceRecorded &&
        InterlockedExchangeAdd(&g_serviceExternalStopRequested, 0) == 0) {
        prepared = service_prepare_controlled_restart(reason, err, sizeof(err));
    } else if (corroboratedDriverFailure && !evidenceRecorded) {
        set_message(err, sizeof(err),
            "Recovery evidence could not be persisted");
    }

    if (prepared &&
        InterlockedExchangeAdd(&g_serviceExternalStopRequested, 0) == 0) {
        HANDLE helper = (HANDLE)InterlockedExchangePointer(
            (PVOID volatile*)&g_serviceRestartHelperProcess, nullptr);
        if (helper && WaitForSingleObject(helper, 0) == WAIT_TIMEOUT) {
            debug_log("controlled recovery: poisoned runtime committed through durable snapshot/helper only (%s)\n",
                reason && reason[0] ? reason : "corroborated driver failure");
            g_serviceStatus.dwControlsAccepted = 0;
            g_serviceStatus.dwWin32ExitCode = NO_ERROR;
            g_serviceStatus.dwServiceSpecificExitCode = 0;
            // Do not self-publish STOPPED before the service dispatcher has
            // disconnected.  The helper pins this process and then subscribes
            // to SCM's authoritative STOPPED transition; publishing STOPPED
            // here lets it race StartServiceW against SCM's remaining teardown
            // of this generation (observed as a clean exit followed by a
            // permanently stopped service).  STOP_PENDING accurately describes
            // the interval until ExitProcess completes and SCM owns STOPPED.
            g_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
            CloseHandle(helper);
            ExitProcess(SERVICE_CONTROLLED_RECOVERY_EXIT_CODE);
        }
        if (helper) CloseHandle(helper);
        service_abort_controlled_restart(
            "validated helper exited before poisoned-runtime commit");
        prepared = false;
        set_message(err, sizeof(err),
            "Validated helper exited before poisoned-runtime commit");
    } else if (prepared) {
        service_abort_controlled_restart(
            "external SCM stop superseded poisoned-runtime recovery");
        prepared = false;
    }

    // Continuing this process is unsafe and an ordinary restart must never
    // replay the snapshot.  Persist the safety stop without joining the wedged
    // fan thread or touching any state guarded by the poisoned runtime mutex.
    service_clear_controlled_recovery_files();
    service_clear_restart_reapply_snapshot();
    service_clear_oc_apply_stamp();
    service_latch_auto_restore_lockout(
        SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
        corroboratedDriverFailure
            ? "controlled recovery could not be prepared from a poisoned runtime"
            : "runtime mutex was abandoned without corroborated driver evidence");
    debug_log("controlled recovery: poisoned runtime stopped without replay (%s%s%s)\n",
        reason && reason[0] ? reason : "unspecified",
        err[0] ? ": " : "", err[0] ? err : "");
    g_serviceStatus.dwControlsAccepted = 0;
    g_serviceStatus.dwWin32ExitCode = ERROR_PROCESS_ABORTED;
    g_serviceStatus.dwServiceSpecificExitCode = ERROR_ABANDONED_WAIT_0;
    g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    ExitProcess(ERROR_PROCESS_ABORTED);
}

static void service_abort_controlled_restart(const char* reason) {
    InterlockedExchange(&g_serviceRestartRequested, 0);
    InterlockedExchange(&g_serviceRestartPreparing, 0);
    // A helper that merely loses our handle keeps waiting on its inherited
    // parent handle and can later delete a fresh snapshot written by an
    // explicit Apply. Stop and join this private child before clearing its
    // nonce-bound files so an aborted generation has no future side effects.
    HANDLE helper = (HANDLE)InterlockedExchangePointer(
        (PVOID volatile*)&g_serviceRestartHelperProcess, nullptr);
    if (helper) {
        if (WaitForSingleObject(helper, 0) == WAIT_TIMEOUT) {
            TerminateProcess(helper, 1);
        }
        WaitForSingleObject(helper, INFINITE);
        CloseHandle(helper);
    }
    service_clear_controlled_recovery_files();
    service_abandon_current_recovery_evidence();
    debug_log("controlled recovery: restart aborted%s%s\n",
        reason && reason[0] ? ": " : "", reason && reason[0] ? reason : "");
}

static void service_fail_closed_controlled_restart(const char* reason) {
    service_abort_controlled_restart(reason);
    service_disable_automatic_restore(
        SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
        reason && reason[0] ? reason
                            : "controlled-recovery helper setup failed");
    EnterCriticalSection(&g_appLock);
    ServiceLifecycleEvent lockout = {};
    lockout.type = SERVICE_LIFECYCLE_EVENT_LOCKOUT;
    service_lifecycle_reduce_locked(&lockout);
    LeaveCriticalSection(&g_appLock);
    g_serviceControlledRecoveryValidated = false;
    memset(&g_serviceControlledRecoveryDesired, 0,
        sizeof(g_serviceControlledRecoveryDesired));
    memset(&g_serviceControlledRecoveryTargetGpu, 0,
        sizeof(g_serviceControlledRecoveryTargetGpu));
    g_serviceControlledRecoveryProfileSource =
        SERVICE_PROFILE_SOURCE_NONE;
    g_serviceControlledRecoveryProfileSlot = 0;
    InterlockedExchange(&g_serviceReapplyInProgress, 0);
    debug_log("controlled recovery: helper/setup failure latched automatic restoration off without a hardware write\n");
}

static bool service_query_scm_start_reason(DWORD* reasonOut) {
    if (!reasonOut || !g_serviceStatusHandle) return false;
    *reasonOut = 0;
    HMODULE advapi = GetModuleHandleW(L"advapi32.dll");
    if (!advapi) return false;
    typedef BOOL (WINAPI *QueryServiceDynamicInformationFn)(
        SERVICE_STATUS_HANDLE, DWORD, PVOID*);
    QueryServiceDynamicInformationFn query =
        reinterpret_cast<QueryServiceDynamicInformationFn>(
            GetProcAddress(advapi, "QueryServiceDynamicInformation"));
    if (!query) return false;
    PVOID information = nullptr;
    if (!query(g_serviceStatusHandle,
            SERVICE_DYNAMIC_INFORMATION_LEVEL_START_REASON, &information) ||
        !information) return false;
    DWORD reason = *(DWORD*)information;
    LocalFree(information);
    *reasonOut = reason;
    return true;
}

static bool service_parse_controlled_recovery_service_args(DWORD argc,
    LPWSTR* argv, char* nonceHexOut, size_t nonceHexOutSize,
    bool* requestedOut) {
    if (requestedOut) *requestedOut = false;
    if (!nonceHexOut || nonceHexOutSize <= SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS) return false;
    nonceHexOut[0] = 0;
    unsigned int matches = 0;
    for (DWORD i = 1; argv && i < argc; ++i) {
        if (argv[i] && _wcsicmp(argv[i], L"--controlled-recovery") == 0) {
            ++matches;
            if (i + 1 >= argc || !argv[i + 1]) return false;
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                    argv[i + 1], -1, nonceHexOut, (int)nonceHexOutSize,
                    nullptr, nullptr) <= 0) return false;
            ++i;
        }
    }
    if (requestedOut) *requestedOut = matches != 0;
    return matches <= 1 && (matches == 0 ||
        strlen(nonceHexOut) == SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS);
}

static void service_prepare_controlled_recovery_startup(DWORD argc, LPWSTR* argv) {
    g_serviceControlledRecoveryValidated = false;
    memset(&g_serviceControlledRecoveryDesired, 0,
        sizeof(g_serviceControlledRecoveryDesired));
    memset(&g_serviceControlledRecoveryTargetGpu, 0,
        sizeof(g_serviceControlledRecoveryTargetGpu));
    g_serviceControlledRecoveryProfileSource = SERVICE_PROFILE_SOURCE_NONE;
    g_serviceControlledRecoveryProfileSlot = 0;

    char nonceHex[SERVICE_CONTROLLED_RECOVERY_NONCE_HEX_CHARS + 1] = {};
    bool requested = false;
    bool parsed = service_parse_controlled_recovery_service_args(argc, argv,
        nonceHex, sizeof(nonceHex), &requested);
    if (!parsed || !requested) {
        service_clear_controlled_recovery_files();
        if (!parsed) debug_log("controlled recovery startup: malformed arguments rejected\n");
        SecureZeroMemory(nonceHex, sizeof(nonceHex));
        return;
    }

    DWORD startReason = 0;
    ServiceControlledRecoveryAuthorization authorization = {};
    ServiceProfileSource profileSource = SERVICE_PROFILE_SOURCE_NONE;
    unsigned int profileSlot = 0;
    bool startReasonKnown = service_query_scm_start_reason(&startReason);
    bool demandStart = startReasonKnown &&
        startReason == SERVICE_START_REASON_DEMAND;
    bool authorizationValid = demandStart &&
        service_read_controlled_recovery_authorization(nonceHex, 0, 0,
            true, &authorization);
    bool helperValidated = authorizationValid &&
        authorization.helperValidationMagic ==
            SERVICE_CONTROLLED_RECOVERY_HELPER_VALIDATION_MAGIC;
    bool snapshotValid = helperValidated &&
        service_load_restart_reapply_snapshot(
            &g_serviceControlledRecoveryDesired,
            &g_serviceControlledRecoveryTargetGpu,
            &profileSource, &profileSlot);
    ServiceControlledRecoveryStartGate gate = {};
    gate.argumentsValid = parsed;
    gate.explicitlyRequested = requested;
    gate.scmStartReasonKnown = startReasonKnown;
    gate.scmDemandStart = demandStart;
    gate.authorizationValid = authorizationValid;
    gate.helperValidated = helperValidated;
    gate.snapshotValid = snapshotValid;
    bool valid = service_controlled_recovery_start_is_authorized(gate);
    if (!valid) {
        debug_log("controlled recovery startup: validation rejected (startReason=0x%08lX); ordinary non-mutating startup\n",
            (unsigned long)startReason);
        service_clear_controlled_recovery_files();
        memset(&g_serviceControlledRecoveryDesired, 0,
            sizeof(g_serviceControlledRecoveryDesired));
        memset(&g_serviceControlledRecoveryTargetGpu, 0,
            sizeof(g_serviceControlledRecoveryTargetGpu));
    } else {
        g_serviceControlledRecoveryProfileSource = profileSource;
        g_serviceControlledRecoveryProfileSlot = profileSlot;
        g_serviceControlledRecoveryValidated = true;
        // The capability has been consumed into process-local memory. Remove
        // both persisted inputs now: a Task-Manager kill or ordinary SCM
        // restart between validation and the write must have nothing to replay.
        service_clear_controlled_recovery_files();
        debug_log("controlled recovery startup: nonce, helper, parent pid=%lu, snapshot, freshness, and SCM demand-start validated before RUNNING\n",
            (unsigned long)authorization.previousProcessId);
    }
    SecureZeroMemory(&authorization, sizeof(authorization));
    SecureZeroMemory(nonceHex, sizeof(nonceHex));
}

// Called with the runtime lock held by an explicit Apply/Reset. A validated
// snapshot is only provisional intent until its sole recovery write succeeds;
// once the user supersedes it, it must not remain available to a later event.
static bool service_discard_validated_controlled_recovery_locked(
    const char* reason) {
    if (!g_serviceControlledRecoveryValidated) return false;
    g_serviceControlledRecoveryValidated = false;
    memset(&g_serviceControlledRecoveryDesired, 0,
        sizeof(g_serviceControlledRecoveryDesired));
    memset(&g_serviceControlledRecoveryTargetGpu, 0,
        sizeof(g_serviceControlledRecoveryTargetGpu));
    g_serviceControlledRecoveryProfileSource = SERVICE_PROFILE_SOURCE_NONE;
    g_serviceControlledRecoveryProfileSlot = 0;
    InterlockedExchange(&g_serviceReapplyInProgress, 0);
    g_serviceHasActiveDesired = false;
    memset(&g_serviceActiveDesired, 0, sizeof(g_serviceActiveDesired));
    memset(&g_serviceActiveDesiredGpu, 0,
        sizeof(g_serviceActiveDesiredGpu));
    g_serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_NONE;
    g_serviceActiveProfileSlot = 0;
    service_clear_controlled_recovery_files();
    debug_log("controlled recovery: validated provisional intent discarded by %s\n",
        reason && reason[0] ? reason : "explicit supersession");
    return true;
}

static void service_arm_validated_controlled_recovery() {
    if (!g_serviceControlledRecoveryValidated) return;
    lock_service_runtime();
    g_serviceActiveDesired = g_serviceControlledRecoveryDesired;
    g_serviceActiveDesiredGpu = g_serviceControlledRecoveryTargetGpu;
    g_serviceHasActiveDesired = true;
    InterlockedExchange(&g_serviceReapplyInProgress, 1);
    // Ownership is published only after the hardware reapply succeeds.
    g_serviceActiveProfileSource = SERVICE_PROFILE_SOURCE_NONE;
    g_serviceActiveProfileSlot = 0;
    unlock_service_runtime();
    if (!service_lifecycle_post_validated_driver_recovery()) {
        DWORD lockoutReason = SERVICE_AUTO_RESTORE_LOCKOUT_NONE;
        if (!service_auto_restore_is_locked_out(&lockoutReason)) {
            service_disable_automatic_restore(
                SERVICE_AUTO_RESTORE_LOCKOUT_AUTOMATIC_APPLY_FAILED,
                "validated controlled recovery could not be queued");
        }
        g_serviceControlledRecoveryValidated = false;
        memset(&g_serviceControlledRecoveryDesired, 0,
            sizeof(g_serviceControlledRecoveryDesired));
        memset(&g_serviceControlledRecoveryTargetGpu, 0,
            sizeof(g_serviceControlledRecoveryTargetGpu));
        g_serviceControlledRecoveryProfileSource =
            SERVICE_PROFILE_SOURCE_NONE;
        g_serviceControlledRecoveryProfileSlot = 0;
    }
}
