// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

// Service discovery, authenticated pipe transport, and request framing.

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

static DWORD g_verifiedServicePipePid = 0;
static FILETIME g_verifiedServicePipeCreationTime = {};
static SRWLOCK g_verifiedServicePipeLock = SRWLOCK_INIT;

static bool service_process_creation_time(DWORD pid, FILETIME* creationOut) {
    if (!creationOut) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    FILETIME creation = {}, exitTime = {}, kernelTime = {}, userTime = {};
    bool ok = GetProcessTimes(process, &creation, &exitTime,
        &kernelTime, &userTime) != FALSE;
    CloseHandle(process);
    if (ok) *creationOut = creation;
    return ok;
}

static void cache_verified_service_pipe_process(DWORD pid,
    const FILETIME& creationTime) {
    AcquireSRWLockExclusive(&g_verifiedServicePipeLock);
    g_verifiedServicePipePid = pid;
    g_verifiedServicePipeCreationTime = creationTime;
    ReleaseSRWLockExclusive(&g_verifiedServicePipeLock);
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
    FILETIME pipeProcessCreation = {};
    bool haveCreationTime = service_process_creation_time(
        (DWORD)pipePid, &pipeProcessCreation);
    bool exactCachedProcess = false;
    AcquireSRWLockShared(&g_verifiedServicePipeLock);
    exactCachedProcess = haveCreationTime &&
        g_verifiedServicePipePid == (DWORD)pipePid &&
        CompareFileTime(&pipeProcessCreation,
            &g_verifiedServicePipeCreationTime) == 0;
    ReleaseSRWLockShared(&g_verifiedServicePipeLock);
    if (exactCachedProcess) {
        // The initial connection authenticated this exact process generation
        // against SCM and its registered binary. A new named-pipe connection
        // to the same live generation does not need to repeat registry/image
        // queries every telemetry tick.
        return true;
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
        if (haveCreationTime) {
            cache_verified_service_pipe_process(servicePid,
                pipeProcessCreation);
        }
        return true;
    }
    WCHAR expectedPath[MAX_PATH] = {};
    if (!get_service_binary_path_from_scm(expectedPath, ARRAY_COUNT(expectedPath))) {
        // The PID came from the SCM service query. If the service config path is
        // unavailable from this unelevated GUI, keep the PID boundary and log the
        // missing secondary check instead of rejecting a valid LocalSystem pipe.
        debug_log("service pipe identity: cannot query SCM binary path for pid=%lu; accepting PID match\n",
            (unsigned long)servicePid);
        if (haveCreationTime) {
            cache_verified_service_pipe_process(servicePid,
                pipeProcessCreation);
        }
        return true;
    }
    if (_wcsicmp(serverPath, expectedPath) != 0) {
        set_message(err, errSize, "Service pipe server executable does not match expected service binary");
        debug_log("service pipe identity: server path \"%ls\" does not match expected \"%ls\"\n", serverPath, expectedPath);
        return false;
    }
    if (haveCreationTime) {
        cache_verified_service_pipe_process(servicePid, pipeProcessCreation);
    }
    return true;
}

#include "service_health_probe_policy.h"

static bool refresh_background_service_state() {
    bool installed = false;
    bool running = false;
    query_background_service_state(&installed, &running);
    if (service_health_probe_should_defer(g_app.isServiceProcess,
            g_app.applyInFlight, installed, running)) {
        g_app.backgroundServiceInstalled = true;
        g_app.backgroundServiceRunning = true;
        g_app.usingBackgroundService = true;
        debug_log_on_change("refresh_background_service_state: deferred pipe probe during owned GPU mutation; preserving available=%d\n",
            g_app.backgroundServiceAvailable ? 1 : 0);
        return g_app.backgroundServiceAvailable;
    }
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
                return false;
            }
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
                DWORD modeErr = GetLastError();
                CloseHandle(pipe);
                set_message(err, errSize, "Failed configuring service pipe message mode (error %lu)", modeErr);
                return false;
            }
            remainingMs = service_remaining_timeout_ms(startTickMs, timeoutMs);
            if (!service_pipe_write_exact(pipe, request, sizeof(*request), remainingMs, "writing service request", err, errSize)) {
                CloseHandle(pipe);
                return false;
            }
            if (response) {
                remainingMs = service_remaining_timeout_ms(startTickMs, timeoutMs);
                if (!service_pipe_read_exact(pipe, response, sizeof(*response), remainingMs, "reading service response", err, errSize)) {
                    CloseHandle(pipe);
                    return false;
                }
                if (response->magic != SERVICE_PROTOCOL_MAGIC || response->version != SERVICE_PROTOCOL_VERSION) {
                    CloseHandle(pipe);
                    set_message(err, errSize,
                        "Service response protocol mismatch (magic=0x%08lX version=%lu)",
                        (unsigned long)response->magic,
                        (unsigned long)response->version);
                    return false;
                }
                if (!validate_service_response_for_ipc(response)) {
                    CloseHandle(pipe);
                    set_message(err, errSize,
                        "Service response contains an invalid state envelope");
                    return false;
                }
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
