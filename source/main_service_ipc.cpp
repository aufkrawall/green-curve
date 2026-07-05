#include "service_acl.h"

static bool background_service_pipe_name(WCHAR* out, size_t outCount) {
    // Fixed pipe name without session ID. Using the session ID in the pipe name
    // breaks after reboot: the service starts at boot (session 0) and creates
    // GreenCurveService_0, but after login the GUI looks for GreenCurveService_1.
    // The security descriptor already restricts access to the active console
    // session user, so session-based pipe names are redundant.
    if (!out || outCount == 0) return false;
    return SUCCEEDED(StringCchPrintfW(out, outCount, L"\\\\.\\pipe\\GreenCurveService"));
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

static bool parse_service_binary_path_from_command_line(const WCHAR* commandLine, WCHAR* out, size_t outCount) {
    if (out && outCount > 0) out[0] = 0;
    if (!commandLine || !commandLine[0] || !out || outCount == 0) return false;

    // lpBinaryPathName is the service command line, e.g.:
    //   "C:\Program Files\greencurve\greencurve-service.exe" --service-run
    // Strip the surrounding quotes and take only the executable path part.
    const WCHAR* src = commandLine;
    while (*src == L' ') src++;
    bool quoted = (*src == L'"');
    if (quoted) src++;
    WCHAR binPath[MAX_PATH] = {};
    size_t dst = 0;
    for (; *src && dst + 1 < ARRAY_COUNT(binPath); src++) {
        if (quoted) {
            if (*src == L'"') break;
        } else {
            if (*src == L' ') break;
        }
        binPath[dst++] = *src;
    }
    if (!binPath[0]) return false;

    WCHAR fullPath[MAX_PATH] = {};
    DWORD pathLen = GetFullPathNameW(binPath, MAX_PATH, fullPath, nullptr);
    if (pathLen == 0 || pathLen >= MAX_PATH) return false;
    return SUCCEEDED(StringCchCopyW(out, outCount, fullPath));
}

static bool get_service_binary_path_from_scm(WCHAR* out, size_t outCount) {
    if (out && outCount > 0) out[0] = 0;
    if (!out || outCount == 0) return false;
    ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.valid()) return false;
    ScopedServiceHandle svc(OpenServiceW(scm.get(), L"GreenCurveService", SERVICE_QUERY_CONFIG));
    if (!svc.valid()) return false;
    DWORD needed = 0;
    QueryServiceConfigW(svc.get(), nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0) return false;
    HeapBuffer buf(needed);
    if (!buf) return false;
    QUERY_SERVICE_CONFIGW* qsc = (QUERY_SERVICE_CONFIGW*)buf.ptr;
    if (!QueryServiceConfigW(svc.get(), qsc, needed, &needed)) return false;
    if (!qsc->lpBinaryPathName || !qsc->lpBinaryPathName[0]) return false;

    bool ok = parse_service_binary_path_from_command_line(qsc->lpBinaryPathName, out, outCount);
    if (ok) debug_log_on_change("scm: service binary path is %ls\n", out);
    return ok;
}

static bool get_service_binary_directory_from_scm(WCHAR* out, size_t outCount) {
    if (out && outCount > 0) out[0] = 0;
    if (!out || outCount == 0) return false;
    WCHAR fullPath[MAX_PATH] = {};
    if (!get_service_binary_path_from_scm(fullPath, ARRAY_COUNT(fullPath))) return false;
    WCHAR* slash = wcsrchr(fullPath, L'\\');
    if (!slash) slash = wcsrchr(fullPath, L'/');
    if (!slash) return false;
    *slash = 0;
    debug_log_on_change("scm: service binary directory is %ls\n", fullPath);
    return SUCCEEDED(StringCchCopyW(out, outCount, fullPath));
}

static bool install_dir_is_under_user_profile_w(const WCHAR* dir);

bool service_install_dir_is_under_user_profile() {
    WCHAR installDir[MAX_PATH] = {};
    if (!get_service_binary_directory_from_scm(installDir, ARRAY_COUNT(installDir))) return false;
    return install_dir_is_under_user_profile_w(installDir);
}

static bool get_current_executable_directory_w(WCHAR* out, size_t outCount, char* err, size_t errSize) {
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
    *slash = 0;
    if (FAILED(StringCchCopyW(out, outCount, exePath))) {
        set_message(err, errSize, "Current executable directory path is too long");
        return false;
    }
    return true;
}

// True if the RUNNING process's own binary directory sits under a Windows user
// profile.  Unlike service_install_dir_is_under_user_profile() (which keys off
// the SCM-registered service dir and therefore needs the service installed),
// this also fires pre-install / in portable use — the case where a restricted
// user's admin ran greencurve.exe from inside an admin profile and other users
// cannot even launch the GUI binary (ERROR_ACCESS_DENIED on execute).
bool running_exe_dir_is_under_user_profile() {
    WCHAR exeDir[MAX_PATH] = {};
    char ignored[64] = {};
    if (!get_current_executable_directory_w(exeDir, ARRAY_COUNT(exeDir), ignored, sizeof(ignored))) return false;
    return install_dir_is_under_user_profile_w(exeDir);
}

static bool get_secure_service_install_dir_w(WCHAR* out, size_t outCount, char* err, size_t errSize);

static bool get_process_image_path(DWORD pid, WCHAR* out, size_t outCount) {
    if (!out || outCount == 0) return false;
    out[0] = 0;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD pathLen = (DWORD)outCount;
    bool ok = QueryFullProcessImageNameW(process, 0, out, &pathLen) != FALSE;
    CloseHandle(process);
    if (!ok) out[0] = 0;
    return ok;
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
    // Verify the server process executable is the SCM-registered service binary.
    WCHAR serverPath[MAX_PATH] = {};
    bool haveServerPath = get_process_image_path(servicePid, serverPath, ARRAY_COUNT(serverPath));
    if (!haveServerPath) {
        // The GUI may not have permission to query the LocalSystem service's image path.
        // Fall back to querying the SCM for the service binary path instead.
        haveServerPath = get_service_binary_path_from_scm(serverPath, ARRAY_COUNT(serverPath));
    }
    if (!haveServerPath) {
        // Cannot verify the service binary path from an unelevated context.
        // The PID check against the SCM is sufficient for the threat model.
        // Log a warning and accept the connection.
        debug_log("service pipe identity: cannot verify server binary path for pid=%lu from unelevated context; accepting PID match\n", (unsigned long)servicePid);
        return true;
    }
    WCHAR expectedPath[MAX_PATH] = {};
    if (!get_service_binary_path_from_scm(expectedPath, ARRAY_COUNT(expectedPath))) {
        // The PID came from the SCM service query. If the service config path is
        // unavailable from this unelevated GUI, keep the PID boundary and log the
        // missing secondary check instead of rejecting a valid LocalSystem pipe.
        debug_log("service pipe identity: cannot query SCM binary path for pid=%lu; accepting PID match\n",
            (unsigned long)servicePid);
        return true;
    }
    if (_wcsicmp(serverPath, expectedPath) != 0) {
        set_message(err, errSize, "Service pipe server executable does not match expected service binary");
        debug_log("service pipe identity: server path \"%ls\" does not match expected \"%ls\"\n", serverPath, expectedPath);
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
        if (!g_app.backgroundServiceAvailable) {
            debug_log("refresh_background_service_state: ping FAILED err=[%s]\n", err[0] ? err : "(empty)");
            if (err[0]) {
                StringCchCopyA(g_app.backgroundServiceError, ARRAY_COUNT(g_app.backgroundServiceError), err);
            }
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

// Map a service-pipe connect error code to a human-readable reason.  Pure
// helper (no globals) so it can be unit-tested.  The ACCESS_DENIED case is the
// important multi-user signal: only the active console/RDP user is granted
// pipe write access (F-SEC-3), so a different logged-in user's GUI gets denied
// and should see WHY rather than a generic "service not responding".
static void describe_service_connect_error(DWORD err, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    if (err == ERROR_ACCESS_DENIED) {
        StringCchCopyA(out, outSize,
            "Another user is currently the active GPU controller, so this "
            "session has read-only access. Controls are disabled until you "
            "become the active session (switch to / unlock this user).");
        return;
    }
    set_message(out, outSize, "Failed connecting to service pipe (error %lu)", err);
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
            if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
                DWORD modeErr = GetLastError();
                CloseHandle(pipe);
                g_app.backgroundServiceAvailable = false;
                g_app.backgroundServiceBroken = true;
                clear_service_authoritative_state();
                set_message(err, errSize, "Failed configuring service pipe message mode (error %lu)", modeErr);
                return false;
            }
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
                response->message[ARRAY_COUNT(response->message) - 1] = '\0';
                response->serviceVersion[ARRAY_COUNT(response->serviceVersion) - 1] = '\0';
                if (response->magic != SERVICE_PROTOCOL_MAGIC || response->version != SERVICE_PROTOCOL_VERSION) {
                    CloseHandle(pipe);
                    g_app.backgroundServiceAvailable = false;
                    g_app.backgroundServiceBroken = true;
                    clear_service_authoritative_state();
                    set_message(err, errSize,
                        "Service response protocol mismatch (magic=0x%08lX version=%lu)",
                        (unsigned long)response->magic,
                        (unsigned long)response->version);
                    return false;
                }
                validate_service_response_for_ipc(response);
            }
            CloseHandle(pipe);
            return true;
        }
        DWORD e = GetLastError();
        if (e != ERROR_PIPE_BUSY && e != ERROR_FILE_NOT_FOUND) {
            describe_service_connect_error(e, err, errSize);
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
    request.flags = interactive ? SERVICE_REQUEST_FLAG_INTERACTIVE : 0u;
    // Shared-only policy: if the editor holds an UNMODIFIED admin shared profile,
    // tag this as an authoritative "apply shared slot N" so the service applies
    // its own copy. Required for a non-admin on a restricted machine; a harmless
    // no-op for admins and unrestricted machines.
    if (g_app.loadedSharedSlot > 0 && !g_app.guiHasUserModifiedValues) {
        request.flags |= SERVICE_REQUEST_FLAG_SHARED_SLOT |
            (((DWORD)g_app.loadedSharedSlot & SERVICE_REQUEST_SHARED_SLOT_MASK) << SERVICE_REQUEST_SHARED_SLOT_SHIFT);
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
    if (!service_send_request(&request, &response, SERVICE_APPLY_CLIENT_TIMEOUT_MS, err, sizeof(err))) {
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

static bool wait_for_background_service_ready(DWORD timeoutMs, char* err, size_t errSize) {
    UiInputGuard uiGuard;
    ULONGLONG start = GetTickCount64();
    while ((GetTickCount64() - start) < timeoutMs) {
        refresh_background_service_state();
        if (g_app.backgroundServiceAvailable) {
            debug_log("wait_for_background_service_ready: ready after %llu ms (ui pumped=%d)\n",
                GetTickCount64() - start, uiGuard.disabled ? 1 : 0);
            return true;
        }
        wait_object_pumping_ui(nullptr, 200);
    }
    debug_log("wait_for_background_service_ready: timed out after %lu ms\n", (unsigned long)timeoutMs);
    set_message(err, errSize, "Background service installed, but it did not become ready in time");
    return false;
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

static bool launch_service_admin_helper(bool enable, char* err, size_t errSize) {
    WCHAR exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, ARRAY_COUNT(exePath));

    // Pass the requesting GUI's config path so the elevated helper resolves the
    // same per-user config as the GUI instead of its own (admin/SYSTEM) path or
    // a beside-binary fallback. Service install/remove itself does not write the
    // user config, but a consistent path prevents stray-config regressions and
    // makes any helper logging reference the correct file.
    WCHAR cfgPath[MAX_PATH] = {};
    if (!utf8_to_wide(g_app.configPath, cfgPath, ARRAY_COUNT(cfgPath))) {
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
static bool resolve_machine_config_dir_w(WCHAR* outW, size_t outCount, char* err, size_t errSize) {
    if (outW && outCount > 0) outW[0] = 0;
    if (!outW || outCount == 0) {
        set_message(err, errSize, "Invalid machine config dir buffer");
        return false;
    }
    PWSTR programData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &programData)) && programData) {
        HRESULT hr = StringCchPrintfW(outW, outCount, L"%ls\\Green Curve", programData);
        CoTaskMemFree(programData);
        if (FAILED(hr)) {
            set_message(err, errSize, "Machine config directory path is too long");
            return false;
        }
        return true;
    }
    // Fallback to the %ProgramData% environment variable (then the canonical
    // default) if the known folder cannot be resolved.
    WCHAR base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", base, ARRAY_COUNT(base));
    if (n == 0 || n >= ARRAY_COUNT(base)) {
        StringCchCopyW(base, ARRAY_COUNT(base), L"C:\\ProgramData");
    }
    if (FAILED(StringCchPrintfW(outW, outCount, L"%ls\\Green Curve", base))) {
        set_message(err, errSize, "Machine config directory path is too long");
        return false;
    }
    return true;
}

// Resolve the path to the machine-wide config file
// (%ProgramData%\Green Curve\shared-profiles.ini).
static bool resolve_machine_config_path_internal(WCHAR* outW, size_t outCount, char* err, size_t errSize) {
    if (outW && outCount > 0) outW[0] = 0;
    if (!outW || outCount == 0) {
        set_message(err, errSize, "Invalid machine config path buffer");
        return false;
    }
    WCHAR dirW[MAX_PATH] = {};
    if (!resolve_machine_config_dir_w(dirW, ARRAY_COUNT(dirW), err, errSize)) return false;
    if (FAILED(StringCchPrintfW(outW, outCount, L"%ls\\%hs", dirW, MACHINE_CONFIG_FILE_NAME))) {
        set_message(err, errSize, "Machine config path is too long");
        return false;
    }
    return true;
}

bool resolve_machine_config_path(char* out, size_t outSize) {
    if (out && outSize > 0) out[0] = 0;
    if (!out || outSize == 0) return false;
    WCHAR pathW[MAX_PATH] = {};
    if (!resolve_machine_config_path_internal(pathW, ARRAY_COUNT(pathW), out, outSize)) return false;
    return copy_wide_to_utf8(pathW, out, (int)outSize);
}

static bool ensure_machine_config_directory(char* err, size_t errSize) {
    WCHAR pathW[MAX_PATH] = {};
    if (!resolve_machine_config_path_internal(pathW, ARRAY_COUNT(pathW), err, errSize)) return false;
    WCHAR dirW[MAX_PATH] = {};
    StringCchCopyW(dirW, ARRAY_COUNT(dirW), pathW);
    WCHAR* slash = wcsrchr(dirW, L'\\');
    if (!slash) slash = wcsrchr(dirW, L'/');
    if (!slash) {
        set_message(err, errSize, "Machine config path has no directory");
        return false;
    }
    *slash = 0;
    if (!CreateDirectoryW(dirW, nullptr)) {
        DWORD createErr = GetLastError();
        if (createErr != ERROR_ALREADY_EXISTS) {
            set_message(err, errSize, "Failed creating machine config directory (error %lu)", createErr);
            return false;
        }
    }
    DWORD attrs = GetFileAttributesW(dirW);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        set_message(err, errSize, "Machine config directory is unavailable or unsafe");
        return false;
    }
    // Harden the directory so a standard user cannot plant or delete files in
    // the shared bank (the default %ProgramData% ACL grants users create-file
    // rights).  This is a required postcondition for all machine-bank writers.
    char aclErr[256] = {};
    if (!apply_protected_machine_config_dir_dacl(dirW, aclErr, sizeof(aclErr))) {
        set_message(err, errSize, "Machine config directory DACL hardening failed: %s", aclErr[0] ? aclErr : "unknown");
        return false;
    }
    if (!machine_config_dacl_is_hardened(dirW)) {
        set_message(err, errSize, "Machine config directory DACL verification failed");
        return false;
    }
    return true;
}

static bool harden_machine_config_file_required(const WCHAR* pathW, const char* pathForLog, char* err, size_t errSize) {
    char aclErr[256] = {};
    if (!apply_protected_machine_config_dacl(pathW, aclErr, sizeof(aclErr))) {
        set_message(err, errSize, "Machine config file DACL hardening failed: %s", aclErr[0] ? aclErr : "unknown");
        return false;
    }
    if (!machine_config_dacl_is_hardened(pathW)) {
        set_message(err, errSize, "Machine config file DACL verification failed");
        return false;
    }
    debug_log("machine config: verified protected DACL on %s\n", pathForLog ? pathForLog : "<wide path>");
    return true;
}

// One-time migration from the legacy machine.ini location (next to the
// installed service binary) to the %ProgramData%\Green Curve\shared-profiles.ini
// location.  Runs service-side as LocalSystem so it can write %ProgramData% and
// set DACLs.  No-op once the new file exists or when there is no legacy file.
static void migrate_legacy_machine_config() {
    char newPath[MAX_PATH] = {};
    if (!resolve_machine_config_path(newPath, sizeof(newPath))) return;
    if (GetFileAttributesA(newPath) != INVALID_FILE_ATTRIBUTES) {
        return; // already migrated (or freshly created at the new location)
    }
    WCHAR installDir[MAX_PATH] = {};
    if (!get_service_binary_directory_from_scm(installDir, ARRAY_COUNT(installDir))) {
        return; // service binary dir unknown; nothing legacy to migrate
    }
    WCHAR legacyW[MAX_PATH] = {};
    if (FAILED(StringCchPrintfW(legacyW, ARRAY_COUNT(legacyW), L"%ls\\%hs",
            installDir, LEGACY_MACHINE_CONFIG_FILE_NAME))) {
        return;
    }
    if (GetFileAttributesW(legacyW) == INVALID_FILE_ATTRIBUTES) {
        return; // no legacy file to migrate
    }
    char dirErr[256] = {};
    if (!ensure_machine_config_directory(dirErr, sizeof(dirErr))) {
        debug_log("machine config migration: cannot ensure target directory: %s\n",
            dirErr[0] ? dirErr : "unknown");
        return;
    }
    WCHAR newW[MAX_PATH] = {};
    char pathErr[256] = {};
    if (!resolve_machine_config_path_internal(newW, ARRAY_COUNT(newW), pathErr, sizeof(pathErr))) {
        debug_log("machine config migration: cannot resolve target path: %s\n",
            pathErr[0] ? pathErr : "unknown");
        return;
    }
    if (!CopyFileW(legacyW, newW, TRUE)) {
        debug_log("machine config migration: CopyFile failed (error %lu) from %ls\n",
            GetLastError(), legacyW);
        return;
    }
    char aclErr[256] = {};
    if (!apply_protected_machine_config_dacl(newW, aclErr, sizeof(aclErr))) {
        debug_log("machine config migration: DACL hardening failed: %s\n", aclErr[0] ? aclErr : "unknown");
        DeleteFileW(newW);
        return;
    }
    if (!machine_config_dacl_is_hardened(newW)) {
        debug_log("machine config migration: DACL verification failed after copy\n");
        DeleteFileW(newW);
        return;
    }
    if (!DeleteFileW(legacyW)) {
        debug_log("machine config migration: copied to %s but failed deleting legacy file (error %lu)\n",
            newPath, GetLastError());
    } else {
        debug_log("machine config migration: moved legacy %ls -> %s\n", legacyW, newPath);
    }
}

// Harden the %ProgramData% shared bank at service start (SYSTEM, before any
// interactive login).  The default %ProgramData% ACL lets standard users create
// subfolders, so without this a user could pre-create %ProgramData%\Green Curve
// (owning it with full control) and plant a malicious shared-profiles.ini before
// any admin initializes it — which the service would then trust (e.g. apply a
// hostile "shared default" to other users on logon).  Creating + hardening the
// directory at boot wins the race; if a squatted directory/file already exists,
// SYSTEM reclaims the protected DACL (and the file's owner) so the bank is
// admin-controlled from then on.
static void secure_shared_bank_at_startup() {
    char err[256] = {};
    if (!ensure_machine_config_directory(err, sizeof(err))) {
        debug_log("shared bank: startup hardening could not ensure directory: %s\n", err[0] ? err : "unknown");
        return;
    }
    WCHAR fileW[MAX_PATH] = {};
    char perr[256] = {};
    if (resolve_machine_config_path_internal(fileW, ARRAY_COUNT(fileW), perr, sizeof(perr)) &&
        GetFileAttributesW(fileW) != INVALID_FILE_ATTRIBUTES) {
        char aclErr[256] = {};
        if (!apply_protected_machine_config_dacl(fileW, aclErr, sizeof(aclErr))) {
            debug_log("shared bank: startup file DACL reclaim failed: %s\n", aclErr[0] ? aclErr : "unknown");
        } else if (!machine_config_dacl_is_hardened(fileW)) {
            debug_log("shared bank: startup file DACL verification failed after reclaim\n");
        } else {
            debug_log("shared bank: startup hardening verified/reclaimed %ls\n", fileW);
        }
    }
}

bool get_machine_logon_slot(int* slotOut) {
    if (slotOut) *slotOut = 0;
    char path[MAX_PATH] = {};
    if (!resolve_machine_config_path(path, sizeof(path))) return false;
    int slot = get_config_int(path, "profiles", "logon_slot", 0);
    if (slot < 0 || slot > CONFIG_NUM_SLOTS) slot = 0;
    if (slotOut) *slotOut = slot;
    return true;
}

bool set_machine_logon_slot(int slot, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (slot < 1 || slot > CONFIG_NUM_SLOTS) {
        set_message(err, errSize, "Invalid machine logon slot %d", slot);
        return false;
    }
    if (!is_elevated()) {
        set_message(err, errSize, "Setting a machine-wide default profile requires administrator rights");
        return false;
    }
    if (!ensure_machine_config_directory(err, errSize)) return false;
    char path[MAX_PATH] = {};
    if (!resolve_machine_config_path(path, sizeof(path))) {
        set_message(err, errSize, "Cannot resolve machine config path");
        return false;
    }
    if (!set_config_int(path, "profiles", "logon_slot", slot)) {
        set_message(err, errSize, "Failed writing machine config");
        return false;
    }
    WCHAR pathW[MAX_PATH] = {};
    if (!resolve_machine_config_path_internal(pathW, ARRAY_COUNT(pathW), err, errSize)) return false;
    if (!harden_machine_config_file_required(pathW, path, err, errSize)) return false;
    g_app.machineLogonSlotCache = slot;
    debug_log("machine config: set machine-wide logon slot to %d in %s\n", slot, path);
    return true;
}

bool clear_machine_logon_slot(char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!is_elevated()) {
        set_message(err, errSize, "Clearing the machine-wide default profile requires administrator rights");
        return false;
    }
    if (!ensure_machine_config_directory(err, errSize)) return false;
    char path[MAX_PATH] = {};
    if (!resolve_machine_config_path(path, sizeof(path))) {
        set_message(err, errSize, "Cannot resolve machine config path");
        return false;
    }
    if (!set_config_int(path, "profiles", "logon_slot", 0)) {
        set_message(err, errSize, "Failed clearing machine config");
        return false;
    }
    WCHAR pathW[MAX_PATH] = {};
    if (!resolve_machine_config_path_internal(pathW, ARRAY_COUNT(pathW), err, errSize)) return false;
    if (!harden_machine_config_file_required(pathW, path, err, errSize)) return false;
    g_app.machineLogonSlotCache = 0;
    debug_log("machine config: cleared machine-wide logon slot in %s\n", path);
    return true;
}

bool get_machine_restrict_policy(bool* enabledOut) {
    if (enabledOut) *enabledOut = false;
    char path[MAX_PATH] = {};
    if (!resolve_machine_config_path(path, sizeof(path))) return false;
    int v = get_config_int(path, "policy", "restrict_non_admin_to_shared", 0);
    if (enabledOut) *enabledOut = (v != 0);
    return true;
}

bool set_machine_restrict_policy(bool enable, char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!is_elevated()) {
        set_message(err, errSize, "Changing the shared-only policy requires administrator rights");
        return false;
    }
    if (!ensure_machine_config_directory(err, errSize)) return false;
    char path[MAX_PATH] = {};
    if (!resolve_machine_config_path(path, sizeof(path))) {
        set_message(err, errSize, "Cannot resolve machine config path");
        return false;
    }
    if (!set_config_int(path, "policy", "restrict_non_admin_to_shared", enable ? 1 : 0)) {
        set_message(err, errSize, "Failed writing machine config");
        return false;
    }
    WCHAR pathW[MAX_PATH] = {};
    if (!resolve_machine_config_path_internal(pathW, ARRAY_COUNT(pathW), err, errSize)) return false;
    if (!harden_machine_config_file_required(pathW, path, err, errSize)) return false;
    debug_log("machine config: set restrict_non_admin_to_shared=%d in %s\n", enable ? 1 : 0, path);
    return true;
}

