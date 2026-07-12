// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Typed GUI/CLI service commands, including the settings-free logon handoff.

static bool service_client_ping(char* err, size_t errSize) {
    ServiceRequest request = {};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = SERVICE_CMD_PING;
    request.callerPid = GetCurrentProcessId();
    ProcessIdToSessionId(request.callerPid, &request.callerSessionId);
    StringCchCopyA(request.source, ARRAY_COUNT(request.source), "client ping");
    ServiceResponse response = {};
    if (!service_send_request(&request, &response, 500, err, errSize)) return false;
    if (response.status != SERVICE_STATUS_OK) {
        set_message(err, errSize, "%s", response.message[0] ? response.message : "Service ping failed");
        return false;
    }
    response.serviceVersion[ARRAY_COUNT(response.serviceVersion) - 1] = '\0';
    if (strcmp(response.serviceVersion, APP_VERSION) != 0) {
        debug_log("service_client_ping: identity mismatch gui=%s build=%lu service=%s build=%lu protocol=%lu\n",
            APP_VERSION,
            (unsigned long)APP_BUILD_NUMBER,
            response.serviceVersion[0] ? response.serviceVersion : "<missing>",
            (unsigned long)response.serviceBuildNumber,
            (unsigned long)response.version);
        set_message(err, errSize,
            "Background service version mismatch (GUI %s build %lu, service %s build %lu). Reinstall or restart the background service.",
            APP_VERSION,
            (unsigned long)APP_BUILD_NUMBER,
            response.serviceVersion[0] ? response.serviceVersion : "<unknown>",
            (unsigned long)response.serviceBuildNumber);
        return false;
    }
    if (response.serviceBuildNumber != (DWORD)APP_BUILD_NUMBER) {
        debug_log("service_client_ping: compatible build mismatch accepted gui=%s build=%lu service=%s build=%lu protocol=%lu\n",
            APP_VERSION,
            (unsigned long)APP_BUILD_NUMBER,
            response.serviceVersion[0] ? response.serviceVersion : "<missing>",
            (unsigned long)response.serviceBuildNumber,
            (unsigned long)response.version);
    }
    return true;
}

static bool service_client_get_snapshot(ServiceSnapshot* snapshot, char* err, size_t errSize) {
    ServiceRequest request = {};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = SERVICE_CMD_GET_SNAPSHOT;
    request.callerPid = GetCurrentProcessId();
    ProcessIdToSessionId(request.callerPid, &request.callerSessionId);
    StringCchCopyA(request.source, ARRAY_COUNT(request.source), "client snapshot");
    ServiceResponse response = {};
    if (!service_send_request(&request, &response, 2000, err, errSize)) return false;
    if (response.status != SERVICE_STATUS_OK) {
        set_message(err, errSize, "%s", response.message[0] ? response.message : "Service snapshot failed");
        return false;
    }
    if (snapshot) *snapshot = response.snapshot;
    if (response.controlState.valid) apply_control_state_to_gui(&response.controlState);
    return true;
}

static bool service_client_get_telemetry(ServiceSnapshot* snapshot, char* err, size_t errSize) {
    ServiceRequest request = {};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = SERVICE_CMD_GET_TELEMETRY;
    request.callerPid = GetCurrentProcessId();
    ProcessIdToSessionId(request.callerPid, &request.callerSessionId);
    StringCchCopyA(request.source, ARRAY_COUNT(request.source), "client telemetry");
    ServiceResponse response = {};
    if (!service_send_request(&request, &response, 500, err, errSize)) return false;
    if (response.status != SERVICE_STATUS_OK) {
        set_message(err, errSize, "%s", response.message[0] ? response.message : "Service telemetry failed");
        return false;
    }
    if (snapshot) *snapshot = response.snapshot;
    if (response.controlState.valid) apply_control_state_to_gui(&response.controlState);
    return true;
}

static bool service_client_apply_desired(const DesiredSettings* desired, const char* source,
    bool interactive, ServiceApplyOrigin origin, ServiceProfileSource profileSource,
    int profileSlot, char* result, size_t resultSize, ServiceSnapshot* snapshotOut) {
    ServiceRequest request = {};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = SERVICE_CMD_APPLY;
    request.flags = interactive ? SERVICE_REQUEST_FLAG_INTERACTIVE : 0u;
    request.applyOrigin = (gc_u32)origin;
    request.profileSource = (gc_u32)profileSource;
    request.profileSlot = (profileSlot >= 1 && profileSlot <= CONFIG_NUM_SLOTS)
        ? (gc_u32)profileSlot : 0u;
    // Shared-only policy: if the editor holds an UNMODIFIED admin shared profile,
    // tag this as an authoritative "apply shared slot N" so the service applies
    // its own copy. Required for a non-admin on a restricted machine; a harmless
    // no-op for admins and unrestricted machines.
    if (g_app.loadedSharedSlot > 0 && !g_app.guiHasUserModifiedValues) {
        request.flags |= SERVICE_REQUEST_FLAG_SHARED_SLOT |
            (((DWORD)g_app.loadedSharedSlot & SERVICE_REQUEST_SHARED_SLOT_MASK) << SERVICE_REQUEST_SHARED_SLOT_SHIFT);
        request.profileSource = SERVICE_PROFILE_SOURCE_SHARED_SLOT;
        request.profileSlot = (gc_u32)g_app.loadedSharedSlot;
    }
    request.callerPid = GetCurrentProcessId();
    ProcessIdToSessionId(request.callerPid, &request.callerSessionId);
    if (desired) {
        request.desired = *desired;
        request.resetOcBeforeApply = desired->resetOcBeforeApply ? 1u : 0u;
    }
    request.targetGpu = g_app.selectedGpu;
    StringCchCopyA(request.source, ARRAY_COUNT(request.source), source && source[0] ? source : "service apply");
    ServiceResponse response = {};
    char err[256] = {};
    if (!service_send_request(&request, &response, SERVICE_APPLY_CLIENT_TIMEOUT_MS, err, sizeof(err))) {
        set_message(result, resultSize, "%s", err);
        return false;
    }
    if (snapshotOut) *snapshotOut = response.snapshot;
    g_app.serviceActiveDesired = response.desired;
    g_app.serviceActiveDesiredValid =
        response.snapshot.activeProfileSource != SERVICE_PROFILE_SOURCE_NONE;
    if (response.controlState.valid) apply_control_state_to_gui(&response.controlState);
    set_message(result, resultSize, "%s", response.message[0] ? response.message : "Background service apply failed");
    return response.status == SERVICE_STATUS_OK;
}

static VOID CALLBACK service_status_change_callback(PVOID) {
    // NotifyServiceStatusChangeW delivers this callback as an APC to the
    // registering thread.  The notification structure itself is inspected by
    // that thread after SleepEx returns; the callback must not call SCM APIs.
}

static bool wait_for_background_service_running_notification(
    DWORD timeoutMs, char* err, size_t errSize)
{
    ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.valid()) {
        set_message(err, errSize, "Could not open the Windows Service Control Manager (error %lu)", GetLastError());
        return false;
    }
    ScopedServiceHandle svc(OpenServiceW(scm.get(), L"GreenCurveService", SERVICE_QUERY_STATUS));
    if (!svc.valid()) {
        set_message(err, errSize, "Green Curve background service is not installed (error %lu)", GetLastError());
        return false;
    }

    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    DWORD lastState = 0;
    DWORD lastServiceError = ERROR_SUCCESS;
    for (;;) {
        SERVICE_STATUS_PROCESS status = {};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(svc.get(), SC_STATUS_PROCESS_INFO,
                (LPBYTE)&status, sizeof(status), &needed)) {
            set_message(err, errSize,
                "Could not query Green Curve service status (error %lu)",
                GetLastError());
            return false;
        }
        lastState = status.dwCurrentState;
        lastServiceError = status.dwWin32ExitCode;
        if (lastState == SERVICE_RUNNING) return true;

        ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            set_message(err, errSize,
                "Green Curve background service did not reach RUNNING within %lu seconds (last state=%lu service error=%lu)",
                (unsigned long)(timeoutMs / 1000),
                (unsigned long)lastState, (unsigned long)lastServiceError);
            return false;
        }

        // A logon task can run in the narrow interval where SCM still reports
        // STOPPED but is about to begin the automatic service start.  Waiting
        // only for RUNNING/STOPPED would fire immediately on that current
        // STOPPED state and recreate the original race.  Instead, subscribe to
        // the next START_PENDING transition while stopped, then subscribe to
        // RUNNING or a terminal STOPPED transition once startup is underway.
        DWORD notifyMask = SERVICE_NOTIFY_RUNNING |
            SERVICE_NOTIFY_DELETE_PENDING;
        if (lastState == SERVICE_STOPPED) {
            notifyMask |= SERVICE_NOTIFY_START_PENDING;
        } else {
            notifyMask |= SERVICE_NOTIFY_STOPPED;
        }

        SERVICE_NOTIFYW notification = {};
        notification.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE;
        notification.pfnNotifyCallback = service_status_change_callback;
        DWORD notifyResult = NotifyServiceStatusChangeW(
            svc.get(), notifyMask, &notification);
        if (notifyResult != ERROR_SUCCESS) {
            set_message(err, errSize,
                "Could not subscribe to Green Curve service readiness (error %lu)",
                notifyResult);
            return false;
        }

        DWORD remaining = (DWORD)(deadline - now);
        while (notification.dwNotificationTriggered == 0 &&
               notification.dwNotificationStatus == ERROR_SUCCESS) {
            DWORD waitResult = SleepEx(remaining, TRUE);
            if (waitResult == 0) {
                // The SERVICE_NOTIFY buffer is stack-owned and must remain
                // valid until delivery or cancellation. Close the service
                // handle explicitly while it is still in scope; relying on
                // return-time destruction would end the buffer lifetime first.
                svc.reset();
                set_message(err, errSize,
                    "Green Curve background service did not reach RUNNING within %lu seconds (last state=%lu service error=%lu)",
                    (unsigned long)(timeoutMs / 1000),
                    (unsigned long)lastState, (unsigned long)lastServiceError);
                return false;
            }
            if (waitResult != WAIT_IO_COMPLETION) {
                DWORD waitError = GetLastError();
                svc.reset();
                set_message(err, errSize,
                    "Waiting for Green Curve service readiness failed (wait=%lu error=%lu)",
                    waitResult, waitError);
                return false;
            }
            now = GetTickCount64();
            if (now >= deadline) {
                svc.reset();
                set_message(err, errSize,
                    "Green Curve background service did not reach RUNNING within %lu seconds (last state=%lu service error=%lu)",
                    (unsigned long)(timeoutMs / 1000),
                    (unsigned long)lastState, (unsigned long)lastServiceError);
                return false;
            }
            remaining = (DWORD)(deadline - now);
        }
        if (notification.dwNotificationStatus != ERROR_SUCCESS) {
            DWORD notificationError = notification.dwNotificationStatus;
            svc.reset();
            set_message(err, errSize,
                "Green Curve service readiness notification failed (error %lu)",
                notificationError);
            return false;
        }
        // Notifications are one-shot.  Loop through a fresh SCM status query
        // and subscription for START_PENDING -> RUNNING/STOPPED without
        // polling or sleeping between states.
    }
}

static bool service_client_logon_handoff(char* result, size_t resultSize) {
    char waitErr[256] = {};
    if (!wait_for_background_service_running_notification(120000, waitErr, sizeof(waitErr))) {
        set_message(result, resultSize, "%s", waitErr[0] ? waitErr : "Background service was not ready for logon handoff");
        debug_log("logon handoff: service readiness failed: %s\n", result && result[0] ? result : "unknown");
        return false;
    }

    ServiceRequest request = {};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = SERVICE_CMD_LOGON_HANDOFF;
    request.applyOrigin = SERVICE_APPLY_ORIGIN_LOGON;
    request.callerPid = GetCurrentProcessId();
    ProcessIdToSessionId(request.callerPid, &request.callerSessionId);
    StringCchCopyA(request.source, ARRAY_COUNT(request.source), "scheduled task logon handoff");
    ServiceResponse response = {};
    char sendErr[256] = {};
    if (!service_send_request(&request, &response, 5000, sendErr, sizeof(sendErr))) {
        set_message(result, resultSize, "%s", sendErr[0] ? sendErr : "Could not notify the background service");
        debug_log("logon handoff: IPC failed: %s\n", result && result[0] ? result : "unknown");
        return false;
    }
    set_message(result, resultSize, "%s", response.message[0] ? response.message : "Logon handoff failed");
    bool ok = response.status == SERVICE_STATUS_OK;
    debug_log("logon handoff: service response ok=%d message=%s\n", ok ? 1 : 0,
        result && result[0] ? result : "<none>");
    return ok;
}

static bool service_client_reset(char* result, size_t resultSize, ServiceSnapshot* snapshotOut) {
    ServiceRequest request = {};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = SERVICE_CMD_RESET;
    request.callerPid = GetCurrentProcessId();
    ProcessIdToSessionId(request.callerPid, &request.callerSessionId);
    request.targetGpu = g_app.selectedGpu;
    StringCchCopyA(request.source, ARRAY_COUNT(request.source), "client reset");
    ServiceResponse response = {};
    char err[256] = {};
    if (!service_send_request(&request, &response, SERVICE_APPLY_CLIENT_TIMEOUT_MS, err, sizeof(err))) {
        set_message(result, resultSize, "%s", err);
        return false;
    }
    if (snapshotOut) *snapshotOut = response.snapshot;
    g_app.serviceActiveDesired = response.desired;
    g_app.serviceActiveDesiredValid =
        response.snapshot.activeProfileSource != SERVICE_PROFILE_SOURCE_NONE;
    if (response.controlState.valid) apply_control_state_to_gui(&response.controlState);
    set_message(result, resultSize, "%s", response.message[0] ? response.message : "Background service reset failed");
    return response.status == SERVICE_STATUS_OK;
}

static bool service_client_get_active_desired(DesiredSettings* desired, ServiceSnapshot* snapshotOut, char* err, size_t errSize) {
    ServiceRequest request = {};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = SERVICE_CMD_GET_ACTIVE_DESIRED;
    request.callerPid = GetCurrentProcessId();
    ProcessIdToSessionId(request.callerPid, &request.callerSessionId);
    StringCchCopyA(request.source, ARRAY_COUNT(request.source), "client active desired");
    ServiceResponse response = {};
    if (!service_send_request(&request, &response, 5000, err, errSize)) return false;
    if (response.status != SERVICE_STATUS_OK) {
        set_message(err, errSize, "%s", response.message[0] ? response.message : "Service desired query failed");
        return false;
    }
    if (desired) *desired = response.desired;
    g_app.serviceActiveDesired = response.desired;
    g_app.serviceActiveDesiredValid =
        response.snapshot.activeProfileSource != SERVICE_PROFILE_SOURCE_NONE;
    if (snapshotOut) *snapshotOut = response.snapshot;
    if (response.controlState.valid) apply_control_state_to_gui(&response.controlState);
    return true;
}

static bool is_safe_output_path(const char* path, char* err, size_t errSize) {
    if (!path || !path[0]) {
        set_message(err, errSize, "Empty output path");
        return false;
    }
    if (strchr(path, '*') || strchr(path, '?')) {
        set_message(err, errSize, "Output path contains wildcard characters");
        return false;
    }
    int colonCount = 0;
    for (const char* p = path; *p; ++p) {
        if (*p == ':') colonCount++;
    }
    if (colonCount > 1) {
        set_message(err, errSize, "Output path contains invalid colon characters");
        return false;
    }
    char absPath[MAX_PATH] = {};
    DWORD len = GetFullPathNameA(path, ARRAY_COUNT(absPath), absPath, nullptr);
    if (len == 0 || len >= ARRAY_COUNT(absPath)) {
        set_message(err, errSize, "Output path is invalid or too long");
        return false;
    }
    for (const char* p = absPath; *p; ++p) {
        if (*p == '<' || *p == '>' || *p == '"' || *p == '|') {
            set_message(err, errSize, "Output path contains invalid characters");
            return false;
        }
    }
    return true;
}

static bool service_client_write_file_command(DWORD command, const char* path, const char* source, char* result, size_t resultSize) {
    if (path && path[0]) {
        char pathErr[256] = {};
        if (!is_safe_output_path(path, pathErr, sizeof(pathErr))) {
            set_message(result, resultSize, "Path validation failed: %s", pathErr);
            return false;
        }
    }
    ServiceRequest request = {};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = command;
    request.callerPid = GetCurrentProcessId();
    ProcessIdToSessionId(request.callerPid, &request.callerSessionId);
    if (path && path[0]) {
        StringCchCopyA(request.path, ARRAY_COUNT(request.path), path);
    }
    StringCchCopyA(request.source, ARRAY_COUNT(request.source), source && source[0] ? source : "client file command");
    ServiceResponse response = {};
    char err[256] = {};
    if (!service_send_request(&request, &response, 10000, err, sizeof(err))) {
        set_message(result, resultSize, "%s", err);
        return false;
    }
    set_message(result, resultSize, "%s", response.message[0] ? response.message : "Background service command failed");
    return response.status == SERVICE_STATUS_OK;
}

// Keep the GUI window painted and non-"hung" while a service install/start or
// elevated helper blocks the GUI thread for several seconds (up to
// ELEVATED_HELPER_TIMEOUT_MS). Without pumping, the window cannot service
// WM_PAINT, so occlusion-revealed regions show stale/garbage content and the DWM
// marks the window "not responding" (ghosting) — the visual corruption seen when
// starting/restarting the service with the window open.
//
// UiInputGuard disables the main window for the duration so the pumped messages
// cannot re-enter command handlers (same pattern a modal dialog uses); the window
// still repaints while disabled. In the service process (no window) it is inert.
struct UiInputGuard {
    HWND hwnd;
    bool disabled;
    UiInputGuard() : hwnd(g_app.hMainWnd), disabled(false) {
        if (hwnd && IsWindow(hwnd) && IsWindowEnabled(hwnd)) {
            EnableWindow(hwnd, FALSE);
            disabled = true;
        }
    }
    ~UiInputGuard() {
        if (disabled && hwnd && IsWindow(hwnd)) {
            EnableWindow(hwnd, TRUE);
        }
    }
    UiInputGuard(const UiInputGuard&) = delete;
    UiInputGuard& operator=(const UiInputGuard&) = delete;
};

// Wait up to timeoutMs for waitObject (optional) to signal while pumping the GUI
// message queue. Returns true iff the object signaled. With waitObject == nullptr
// it is a message-pumping sleep (returns false on timeout). Falls back to a plain
// wait when there is no GUI window (service process).
