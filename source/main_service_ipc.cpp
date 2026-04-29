static bool background_service_pipe_name(WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        return false;
    }
    return SUCCEEDED(StringCchPrintfW(out, outCount, L"\\\\.\\pipe\\GreenCurveService_%lu", sessionId));
}

static bool service_is_installed() {
    ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.valid()) return false;
    ScopedServiceHandle svc(OpenServiceW(scm.get(), L"GreenCurveService", SERVICE_QUERY_STATUS));
    return svc.valid();
}

static bool query_background_service_state(bool* installedOut, bool* runningOut) {
    if (installedOut) *installedOut = false;
    if (runningOut) *runningOut = false;

    ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.valid()) return false;

    ScopedServiceHandle svc(OpenServiceW(scm.get(), L"GreenCurveService", SERVICE_QUERY_STATUS));
    if (!svc.valid()) return true;

    if (installedOut) *installedOut = true;

    SERVICE_STATUS_PROCESS ssp = {};
    DWORD needed = 0;
    if (QueryServiceStatusEx(svc.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) {
        if (runningOut) *runningOut = (ssp.dwCurrentState == SERVICE_RUNNING || ssp.dwCurrentState == SERVICE_START_PENDING);
    }

    return true;
}

static bool service_is_running() {
    ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.valid()) return false;
    ScopedServiceHandle svc(OpenServiceW(scm.get(), L"GreenCurveService", SERVICE_QUERY_STATUS));
    if (!svc.valid()) return false;
    SERVICE_STATUS_PROCESS ssp = {};
    DWORD needed = 0;
    bool ok = QueryServiceStatusEx(svc.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed) != FALSE &&
        ssp.dwCurrentState == SERVICE_RUNNING;
    return ok;
}

static bool query_background_service_pid(DWORD* pidOut) {
    if (pidOut) *pidOut = 0;
    ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.valid()) return false;
    ScopedServiceHandle svc(OpenServiceW(scm.get(), L"GreenCurveService", SERVICE_QUERY_STATUS));
    if (!svc.valid()) return false;
    SERVICE_STATUS_PROCESS ssp = {};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(svc.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) return false;
    if (ssp.dwCurrentState != SERVICE_RUNNING || ssp.dwProcessId == 0) return false;
    if (pidOut) *pidOut = ssp.dwProcessId;
    return true;
}

static bool validate_service_pipe_server_identity(HANDLE pipe, char* err, size_t errSize) {
    typedef BOOL (WINAPI* get_pipe_server_pid_t)(HANDLE, PULONG);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto getServerPid = kernel ? (get_pipe_server_pid_t)GetProcAddress(kernel, "GetNamedPipeServerProcessId") : nullptr;
    if (!getServerPid) {
        set_message(err, errSize, "Cannot verify service pipe server identity");
        return false;
    }
    ULONG pipePid = 0;
    if (!getServerPid(pipe, &pipePid) || pipePid == 0) {
        set_message(err, errSize, "Cannot query service pipe server process");
        return false;
    }
    DWORD servicePid = 0;
    if (!query_background_service_pid(&servicePid)) {
        set_message(err, errSize, "Cannot query background service process");
        return false;
    }
    if ((DWORD)pipePid != servicePid) {
        set_message(err, errSize, "Service pipe identity mismatch");
        debug_log("service pipe identity mismatch: pipePid=%lu servicePid=%lu\n", (unsigned long)pipePid, (unsigned long)servicePid);
        return false;
    }
    return true;
}

static bool refresh_background_service_state() {
    bool installed = false;
    bool running = false;
    query_background_service_state(&installed, &running);
    g_app.backgroundServiceInstalled = installed;
    g_app.backgroundServiceRunning = running;
    g_app.backgroundServiceAvailable = false;
    g_app.backgroundServiceBroken = false;
    g_app.backgroundServiceError[0] = '\0';
    if (g_app.backgroundServiceInstalled && g_app.backgroundServiceRunning) {
        char err[256] = {};
        g_app.backgroundServiceAvailable = service_client_ping(err, sizeof(err));
        g_app.backgroundServiceBroken = !g_app.backgroundServiceAvailable;
        if (!g_app.backgroundServiceAvailable && err[0]) {
            StringCchCopyA(g_app.backgroundServiceError, ARRAY_COUNT(g_app.backgroundServiceError), err);
        }
    } else if (g_app.backgroundServiceInstalled) {
        g_app.backgroundServiceBroken = true;
        StringCchCopyA(g_app.backgroundServiceError, ARRAY_COUNT(g_app.backgroundServiceError), "Background service is installed but not running");
    }
    g_app.usingBackgroundService = !g_app.isServiceProcess;
    if (!g_app.backgroundServiceAvailable) {
        clear_service_authoritative_state();
    }
    return g_app.backgroundServiceAvailable;
}

static void begin_background_service_toggle(bool enable) {
    g_app.backgroundServiceToggleInFlight = true;
    g_app.backgroundServiceToggleTargetEnabled = enable;
}

static void end_background_service_toggle() {
    g_app.backgroundServiceToggleInFlight = false;
}

static DWORD service_remaining_timeout_ms(ULONGLONG startTickMs, DWORD timeoutMs) {
    if (timeoutMs == 0) return 0;
    ULONGLONG elapsed = GetTickCount64() - startTickMs;
    if (elapsed >= timeoutMs) return 0;
    ULONGLONG remaining = timeoutMs - elapsed;
    return remaining > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (DWORD)remaining;
}

static bool service_pipe_io_exact(HANDLE pipe, bool writeOp, void* buffer, DWORD bufferSize, DWORD timeoutMs, const char* label, char* err, size_t errSize) {
    if (!pipe || pipe == INVALID_HANDLE_VALUE || !buffer || bufferSize == 0) {
        set_message(err, errSize, "Invalid service pipe I/O");
        return false;
    }
    if (timeoutMs == 0) {
        set_message(err, errSize, "Timed out during %s", label ? label : "service pipe I/O");
        return false;
    }

    ScopedHandle event(CreateEventA(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED ov = {};
    ov.hEvent = event.get();
    if (!event.valid()) {
        set_message(err, errSize, "Cannot create service pipe event (error %lu)", GetLastError());
        return false;
    }

    DWORD transferred = 0;
    BOOL started = writeOp
        ? WriteFile(pipe, buffer, bufferSize, nullptr, &ov)
        : ReadFile(pipe, buffer, bufferSize, nullptr, &ov);
    DWORD startErr = started ? ERROR_SUCCESS : GetLastError();
    bool ok = false;
    if (started || startErr == ERROR_IO_PENDING) {
        DWORD waitResult = WaitForSingleObject(ov.hEvent, timeoutMs);
        if (waitResult == WAIT_OBJECT_0) {
            if (GetOverlappedResult(pipe, &ov, &transferred, FALSE) && transferred == bufferSize) {
                ok = true;
            } else {
                DWORD e = GetLastError();
                set_message(err, errSize, "Failed %s (error %lu, bytes %lu/%lu)",
                    label ? label : "service pipe I/O",
                    e,
                    transferred,
                    bufferSize);
            }
        } else if (waitResult == WAIT_TIMEOUT) {
            CancelIoEx(pipe, &ov);
            set_message(err, errSize, "Timed out during %s", label ? label : "service pipe I/O");
        } else {
            set_message(err, errSize, "Failed waiting for %s (error %lu)", label ? label : "service pipe I/O", GetLastError());
        }
    } else {
        set_message(err, errSize, "Failed starting %s (error %lu)", label ? label : "service pipe I/O", startErr);
    }
    return ok;
}

static bool service_pipe_write_exact(HANDLE pipe, const void* data, DWORD dataSize, DWORD timeoutMs, const char* label, char* err, size_t errSize) {
    return service_pipe_io_exact(pipe, true, (void*)data, dataSize, timeoutMs, label, err, errSize);
}

static bool service_pipe_read_exact(HANDLE pipe, void* data, DWORD dataSize, DWORD timeoutMs, const char* label, char* err, size_t errSize) {
    return service_pipe_io_exact(pipe, false, data, dataSize, timeoutMs, label, err, errSize);
}

static bool service_send_request(const ServiceRequest* request, ServiceResponse* response, DWORD timeoutMs, char* err, size_t errSize) {
    if (response) memset(response, 0, sizeof(*response));
    if (!request) {
        set_message(err, errSize, "Invalid service request");
        return false;
    }
    WCHAR pipeName[128] = {};
    if (!background_service_pipe_name(pipeName, ARRAY_COUNT(pipeName))) {
        set_message(err, errSize, "Invalid pipe name");
        return false;
    }

    ULONGLONG startTickMs = GetTickCount64();
    while (true) {
        DWORD remainingMs = service_remaining_timeout_ms(startTickMs, timeoutMs);
        if (remainingMs == 0) {
            set_message(err, errSize, "Timed out waiting for the background service");
            return false;
        }

        HANDLE pipe = CreateFileW(
            pipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            if (!validate_service_pipe_server_identity(pipe, err, errSize)) {
                CloseHandle(pipe);
                g_app.backgroundServiceAvailable = false;
                g_app.backgroundServiceBroken = true;
                clear_service_authoritative_state();
                return false;
            }
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
            remainingMs = service_remaining_timeout_ms(startTickMs, timeoutMs);
            if (!service_pipe_write_exact(pipe, request, sizeof(*request), remainingMs, "writing service request", err, errSize)) {
                CloseHandle(pipe);
                g_app.backgroundServiceAvailable = false;
                g_app.backgroundServiceBroken = true;
                clear_service_authoritative_state();
                return false;
            }
            if (response) {
                remainingMs = service_remaining_timeout_ms(startTickMs, timeoutMs);
                if (!service_pipe_read_exact(pipe, response, sizeof(*response), remainingMs, "reading service response", err, errSize)) {
                    CloseHandle(pipe);
                    g_app.backgroundServiceAvailable = false;
                    g_app.backgroundServiceBroken = true;
                    clear_service_authoritative_state();
                    return false;
                }
            }
            CloseHandle(pipe);
            return true;
        }
        DWORD e = GetLastError();
        if (e != ERROR_PIPE_BUSY && e != ERROR_FILE_NOT_FOUND) {
            set_message(err, errSize, "Failed connecting to service pipe (error %lu)", e);
            return false;
        }
        remainingMs = service_remaining_timeout_ms(startTickMs, timeoutMs);
        if (remainingMs == 0) {
            set_message(err, errSize, "Timed out waiting for the background service");
            return false;
        }
        if (e == ERROR_FILE_NOT_FOUND) {
            Sleep(nvmin((int)SERVICE_PIPE_CLIENT_SLEEP_SLICE_MS, (int)remainingMs));
            continue;
        }
        DWORD waitSlice = remainingMs;
        if (waitSlice > SERVICE_PIPE_CLIENT_CONNECT_SLICE_MS) waitSlice = SERVICE_PIPE_CLIENT_CONNECT_SLICE_MS;
        if (!WaitNamedPipeW(pipeName, waitSlice)) {
            DWORD waitErr = GetLastError();
            if (waitErr != ERROR_SEM_TIMEOUT && waitErr != ERROR_FILE_NOT_FOUND) {
                set_message(err, errSize, "Failed waiting for service pipe (error %lu)", waitErr);
                return false;
            }
        }
    }
}

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
    } else {
        debug_log("service_client_ping: identity ok version=%s build=%lu protocol=%lu\n",
            response.serviceVersion,
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

static bool service_client_apply_desired(const DesiredSettings* desired, const char* source, bool interactive, char* result, size_t resultSize, ServiceSnapshot* snapshotOut) {
    ServiceRequest request = {};
    request.magic = SERVICE_PROTOCOL_MAGIC;
    request.version = SERVICE_PROTOCOL_VERSION;
    request.command = SERVICE_CMD_APPLY;
    request.flags = interactive ? 1u : 0u;
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
    if (!service_send_request(&request, &response, 5000, err, sizeof(err))) {
        set_message(result, resultSize, "%s", err);
        return false;
    }
    if (snapshotOut) *snapshotOut = response.snapshot;
    if (response.controlState.valid) apply_control_state_to_gui(&response.controlState);
    set_message(result, resultSize, "%s", response.message[0] ? response.message : "Background service apply failed");
    return response.status == SERVICE_STATUS_OK;
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
    if (!service_send_request(&request, &response, 5000, err, sizeof(err))) {
        set_message(result, resultSize, "%s", err);
        return false;
    }
    if (snapshotOut) *snapshotOut = response.snapshot;
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

static bool wait_for_background_service_ready(DWORD timeoutMs, char* err, size_t errSize) {
    ULONGLONG start = GetTickCount64();
    while ((GetTickCount64() - start) < timeoutMs) {
        refresh_background_service_state();
        if (g_app.backgroundServiceAvailable) return true;
        Sleep(200);
    }
    set_message(err, errSize, "Background service installed, but it did not become ready in time");
    return false;
}

static bool wait_for_helper_process_bounded(HANDLE process, const char* description, char* err, size_t errSize) {
    if (!process) return true;
    DWORD waitResult = WaitForSingleObject(process, ELEVATED_HELPER_TIMEOUT_MS);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 1000);
        set_message(err, errSize, "%s timed out", description ? description : "Elevated helper");
        return false;
    }
    if (waitResult != WAIT_OBJECT_0) {
        set_message(err, errSize, "Failed waiting for %s (error %lu)", description ? description : "elevated helper", GetLastError());
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

static bool launch_service_admin_helper(bool enable, char* err, size_t errSize) {
    WCHAR exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, ARRAY_COUNT(exePath));
    const WCHAR* helperArg = enable ? L"--service-install" : L"--service-remove";
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
        bool ok = wait_for_helper_process_bounded(helperProcess.get(), "Elevated service helper", err, errSize);
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

    WCHAR helperArg[1536] = {};
    HRESULT hr = StringCchPrintfW(helperArg, ARRAY_COUNT(helperArg),
        enable
            ? L"--elevated --startup-task-enable --config \"%ls\""
            : L"--elevated --startup-task-disable --config \"%ls\"",
        cfgPath);
    if (FAILED(hr)) {
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
    WCHAR exePath[MAX_PATH] = {};
    DWORD exeLen = GetModuleFileNameW(nullptr, exePath, ARRAY_COUNT(exePath));
    if (exeLen == 0 || exeLen >= ARRAY_COUNT(exePath) - 1) {
        set_message(err, errSize, "Current executable path is too long or could not be determined");
        return false;
    }
    WCHAR* slash = wcsrchr(exePath, L'\\');
    if (!slash) slash = wcsrchr(exePath, L'/');
    if (!slash) {
        set_message(err, errSize, "Current executable path has no directory");
        return false;
    }
    slash[1] = 0;
    if (FAILED(StringCchCatW(exePath, ARRAY_COUNT(exePath), APP_SERVICE_EXE_NAME_W)) ||
        FAILED(StringCchCopyW(out, outCount, exePath))) {
        set_message(err, errSize, "Service binary path is too long");
        return false;
    }
    if (!file_is_regular_no_reparse_w(out)) {
        set_message(err, errSize, "Service binary is missing or unsafe");
        return false;
    }
    return true;
}

static bool get_secure_service_install_dir_w(WCHAR* out, size_t outCount, char* err, size_t errSize) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    PWSTR programFiles = nullptr;
    bool copied = false;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles)) && programFiles) {
        copied = SUCCEEDED(StringCchPrintfW(out, outCount, L"%ls\\Green Curve", programFiles));
        CoTaskMemFree(programFiles);
    }
    if (!copied) copied = SUCCEEDED(StringCchCopyW(out, outCount, L"C:\\Program Files\\Green Curve"));
    if (!copied) {
        set_message(err, errSize, "Secure service directory path is too long");
        return false;
    }
    return true;
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
    if (_wcsnicmp(canonicalPath, installDir, wcslen(installDir)) != 0) {
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
            set_message(err, errSize, "Failed staging service binary in Program Files (error %lu)", GetLastError());
            return false;
        }
        if (!MoveFileExW(tempPath, targetPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DWORD moveErr = GetLastError();
            DeleteFileW(tempPath);
            set_message(err, errSize, "Failed installing service binary in Program Files (error %lu)", moveErr);
            return false;
        }
    }

    if (!file_is_regular_no_reparse_w(targetPath)) {
        set_message(err, errSize, "Installed service binary is missing or unsafe");
        return false;
    }

    return SUCCEEDED(StringCchCopyW(out, outCount, targetPath));
}

static void cleanup_secure_service_binary_after_remove() {
    WCHAR installDir[MAX_PATH] = {};
    char ignored[64] = {};
    if (!get_secure_service_install_dir_w(installDir, ARRAY_COUNT(installDir), ignored, sizeof(ignored))) return;
    WCHAR targetPath[MAX_PATH] = {};
    if (SUCCEEDED(StringCchPrintfW(targetPath, ARRAY_COUNT(targetPath), L"%ls\\%ls", installDir, APP_SERVICE_EXE_NAME_W))) {
        DeleteFileW(targetPath);
    }
    RemoveDirectoryW(installDir);
}

