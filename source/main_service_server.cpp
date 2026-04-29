static bool wait_for_service_state(SC_HANDLE svc, DWORD desiredState, DWORD timeoutMs) {
    if (!svc) return false;
    ULONGLONG startTick = GetTickCount64();
    SERVICE_STATUS_PROCESS ssp = {};
    DWORD needed = 0;
    while ((GetTickCount64() - startTick) < timeoutMs) {
        ZeroMemory(&ssp, sizeof(ssp));
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) return false;
        if (ssp.dwCurrentState == desiredState) return true;
        Sleep(200);
    }
    ZeroMemory(&ssp, sizeof(ssp));
    return QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed) &&
        ssp.dwCurrentState == desiredState;
}

static bool stop_service_for_binary_update(SC_HANDLE svc, char* err, size_t errSize) {
    if (!svc) return true;
    SERVICE_STATUS_PROCESS ssp = {};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) {
        set_message(err, errSize, "Failed querying service state before repair (error %lu)", GetLastError());
        return false;
    }
    if (ssp.dwCurrentState == SERVICE_STOPPED) return true;
    debug_log("service repair: stopping existing service before binary update (state=%lu pid=%lu)\n",
        (unsigned long)ssp.dwCurrentState,
        (unsigned long)ssp.dwProcessId);
    if (ssp.dwCurrentState != SERVICE_STOP_PENDING) {
        SERVICE_STATUS status = {};
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
            DWORD stopErr = GetLastError();
            if (stopErr != ERROR_SERVICE_NOT_ACTIVE) {
                set_message(err, errSize, "Failed stopping service for binary update (error %lu)", stopErr);
                return false;
            }
            return true;
        }
    }
    if (!wait_for_service_state(svc, SERVICE_STOPPED, 10000)) {
        set_message(err, errSize, "Timed out stopping service for binary update");
        return false;
    }
    debug_log("service repair: existing service stopped for binary update\n");
    return true;
}

static bool service_install_or_remove(bool enable, char* err, size_t errSize) {
    WCHAR exePath[MAX_PATH] = {};
    if (!enable && !get_adjacent_service_binary_path(exePath, ARRAY_COUNT(exePath), err, errSize)) {
        // Removal does not need the adjacent binary to exist; keep going with the secure target path for cleanup.
        err[0] = 0;
        WCHAR installDir[MAX_PATH] = {};
        char ignored[64] = {};
        if (get_secure_service_install_dir_w(installDir, ARRAY_COUNT(installDir), ignored, sizeof(ignored))) {
            StringCchPrintfW(exePath, ARRAY_COUNT(exePath), L"%ls\\%ls", installDir, APP_SERVICE_EXE_NAME_W);
        }
    }
    ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT));
    if (!scm.valid()) {
        set_message(err, errSize, "Failed opening service manager (error %lu)", GetLastError());
        return false;
    }

    bool ok = false;
    if (enable) {
        ScopedServiceHandle svc(OpenServiceW(scm.get(), L"GreenCurveService", SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS));
        if (svc.valid() && !stop_service_for_binary_update(svc.get(), err, errSize)) return false;
        if (!ensure_secure_service_binary_path(exePath, ARRAY_COUNT(exePath), err, errSize)) return false;
        WCHAR binPath[1024] = {};
        if (FAILED(StringCchPrintfW(binPath, ARRAY_COUNT(binPath), L"\"%ls\" --service-run", exePath))) {
            set_message(err, errSize, "Service command line is too long");
            return false;
        }
        if (!svc.valid()) {
            svc.reset(CreateServiceW(
                scm.get(),
                L"GreenCurveService",
                L"Green Curve Background Service",
                SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS,
                SERVICE_WIN32_OWN_PROCESS,
                SERVICE_AUTO_START,
                SERVICE_ERROR_NORMAL,
                binPath,
                nullptr,
                nullptr,
                nullptr,
                L"LocalSystem",
                nullptr));
        } else {
            if (!ChangeServiceConfigW(svc.get(), SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, binPath, nullptr, nullptr, nullptr, nullptr, nullptr, L"Green Curve Background Service")) {
                set_message(err, errSize, "Failed updating service configuration (error %lu)", GetLastError());
                return false;
            }
        }
        if (!svc.valid()) {
            set_message(err, errSize, "Failed installing service (error %lu)", GetLastError());
        } else {
            SERVICE_STATUS_PROCESS ssp = {};
            DWORD needed = 0;
            if (!QueryServiceStatusEx(svc.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) {
                set_message(err, errSize, "Failed querying installed service state (error %lu)", GetLastError());
                return false;
            }
            if (ssp.dwCurrentState != SERVICE_RUNNING) {
                if (!StartServiceW(svc.get(), 0, nullptr)) {
                    DWORD startErr = GetLastError();
                    if (startErr != ERROR_SERVICE_ALREADY_RUNNING) {
                        set_message(err, errSize, "Failed starting service (error %lu)", startErr);
                        return false;
                    }
                }
                wait_for_service_state(svc.get(), SERVICE_RUNNING, 10000);
                ZeroMemory(&ssp, sizeof(ssp));
                QueryServiceStatusEx(svc.get(), SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed);
            }
            if (ssp.dwCurrentState != SERVICE_RUNNING) {
                set_message(err, errSize, "Service install succeeded but the service did not reach RUNNING state");
            } else {
                ok = true;
            }
        }
    } else {
        ScopedServiceHandle svc(OpenServiceW(scm.get(), L"GreenCurveService", SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS));
        if (!svc.valid()) {
            ok = true;
            cleanup_secure_service_binary_after_remove();
        } else {
            SERVICE_STATUS status = {};
            ControlService(svc.get(), SERVICE_CONTROL_STOP, &status);
            wait_for_service_state(svc.get(), SERVICE_STOPPED, 10000);
            if (!DeleteService(svc.get())) {
                set_message(err, errSize, "Failed removing service (error %lu)", GetLastError());
            } else {
                ok = true;
                cleanup_secure_service_binary_after_remove();
            }
        }
    }

    if (ok) refresh_background_service_state();
    return ok;
}

static bool service_path_has_reparse_component(const char* absPath, char* err, size_t errSize) {
    if (!absPath || !absPath[0]) return false;
    char probe[MAX_PATH] = {};
    if (FAILED(StringCchCopyA(probe, ARRAY_COUNT(probe), absPath))) {
        set_message(err, errSize, "Path is too long");
        return true;
    }

    size_t len = strlen(probe);
    size_t rootLen = 0;
    if (len >= 3 && probe[1] == ':' && (probe[2] == '\\' || probe[2] == '/')) {
        rootLen = 3;
    } else if (len >= 2 && probe[0] == '\\' && probe[1] == '\\') {
        const char* serverEnd = strpbrk(probe + 2, "\\/");
        if (serverEnd) {
            const char* shareEnd = strpbrk(serverEnd + 1, "\\/");
            if (shareEnd) rootLen = (size_t)(shareEnd - probe + 1);
        }
    }

    if (rootLen == 0 || rootLen >= len) return false;
    for (size_t i = rootLen; i < len; i++) {
        if (probe[i] != '\\' && probe[i] != '/') continue;
        char saved = probe[i];
        probe[i] = 0;
        DWORD attrs = GetFileAttributesA(probe);
        probe[i] = saved;
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            set_message(err, errSize, "Path crosses a reparse point");
            return true;
        }
    }

    char* slash = strrchr(probe, '\\');
    char* slashAlt = strrchr(probe, '/');
    if (!slash || (slashAlt && slashAlt > slash)) slash = slashAlt;
    if (slash && (size_t)(slash - probe) >= rootLen) {
        *slash = 0;
        DWORD attrs = GetFileAttributesA(probe);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            set_message(err, errSize, "Path crosses a reparse point");
            return true;
        }
    }
    return false;
}

static bool service_validate_file_write_path(const char* path, char* err, size_t errSize) {
    if (!path || !path[0]) {
        set_message(err, errSize, "Empty path");
        return false;
    }
    if (strstr(path, "..")) {
        set_message(err, errSize, "Path contains parent directory references");
        return false;
    }
    if (strchr(path, '*') || strchr(path, '?')) {
        set_message(err, errSize, "Path contains wildcard characters");
        return false;
    }
    int colonCount = 0;
    for (const char* p = path; *p; ++p) {
        if (*p == ':') colonCount++;
    }
    if (colonCount > 1) {
        set_message(err, errSize, "Path contains invalid colon characters");
        return false;
    }
    char absPath[MAX_PATH] = {};
    DWORD len = GetFullPathNameA(path, ARRAY_COUNT(absPath), absPath, nullptr);
    if (len == 0 || len >= ARRAY_COUNT(absPath)) {
        set_message(err, errSize, "Invalid path");
        return false;
    }
    if (!g_serviceUserPathsResolved || !g_serviceUserProfileDir[0]) {
        set_message(err, errSize, "User paths not resolved");
        return false;
    }
    size_t profileLen = strlen(g_serviceUserProfileDir);
    if (_strnicmp(absPath, g_serviceUserProfileDir, profileLen) != 0 ||
        (absPath[profileLen] != '\\' && absPath[profileLen] != '\0')) {
        set_message(err, errSize, "Path is outside the caller's profile directory");
        return false;
    }
    if (service_path_has_reparse_component(absPath, err, errSize)) {
        return false;
    }
    return true;
}

static bool service_verify_written_file_path(const char* path, char* err, size_t errSize) {
    if (!path || !path[0]) {
        set_message(err, errSize, "Empty path");
        return false;
    }
    if (!g_serviceUserPathsResolved || !g_serviceUserProfileDir[0]) {
        set_message(err, errSize, "User paths not resolved");
        return false;
    }
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // If we cannot open the file for verification, fail closed.
        set_message(err, errSize, "Cannot verify written file path");
        return false;
    }
    char finalPath[MAX_PATH] = {};
    DWORD len = GetFinalPathNameByHandleA(h, finalPath, ARRAY_COUNT(finalPath), FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (len == 0 || len >= ARRAY_COUNT(finalPath)) {
        set_message(err, errSize, "Cannot resolve written file path");
        return false;
    }
    // GetFinalPathNameByHandleA may return a \\?\ prefix.
    const char* comparePath = finalPath;
    if (strncmp(finalPath, "\\\\?\\", 4) == 0) comparePath = finalPath + 4;
    size_t profileLen = strlen(g_serviceUserProfileDir);
    if (_strnicmp(comparePath, g_serviceUserProfileDir, profileLen) != 0 ||
        (comparePath[profileLen] != '\\' && comparePath[profileLen] != '\0')) {
        set_message(err, errSize, "Written file resolved outside the caller's profile directory");
        return false;
    }
    return true;
}

static bool service_prepare_requested_gpu(const ServiceRequest* request, char* err, size_t errSize) {
    if (!request || !request->targetGpu.valid) return true;
    bool change = !g_app.selectedGpuIdentityValid ||
        g_app.selectedGpuIndex != request->targetGpu.nvapiIndex ||
        !gpu_adapter_has_same_pci_identity(&g_app.selectedGpu, &request->targetGpu);
    if (change) {
        debug_log("service gpu target: selecting nvapi=%u nvml=%u name=%s\n",
            request->targetGpu.nvapiIndex,
            request->targetGpu.nvmlIndex,
            request->targetGpu.name[0] ? request->targetGpu.name : "<unnamed>");
        g_app.selectedGpuIndex = request->targetGpu.nvapiIndex;
        g_app.selectedNvmlIndex = request->targetGpu.nvmlIndex;
        g_app.selectedGpuExplicit = true;
        reset_gpu_runtime_selection();
    }
    if (request->targetGpu.nvapiIndex >= MAX_GPU_ADAPTERS) {
        set_message(err, errSize, "Requested GPU index is invalid");
        return false;
    }
    return true;
}

static bool create_restricted_pipe_security_descriptor(PSECURITY_DESCRIPTOR* outSd) {
    *outSd = nullptr;
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) return false;
    HANDLE hToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &hToken)) return false;

    DWORD needed = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(hToken);
        return false;
    }
    TOKEN_USER* tokenUser = (TOKEN_USER*)malloc(needed);
    if (!tokenUser) {
        CloseHandle(hToken);
        return false;
    }
    bool ok = false;
    if (GetTokenInformation(hToken, TokenUser, tokenUser, needed, &needed) && tokenUser->User.Sid) {
        LPWSTR sidStr = nullptr;
        if (ConvertSidToStringSidW(tokenUser->User.Sid, &sidStr)) {
            WCHAR sddl[512] = {};
            if (SUCCEEDED(StringCchPrintfW(sddl, ARRAY_COUNT(sddl),
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;%s)", sidStr))) {
                if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                        sddl, SDDL_REVISION_1, outSd, nullptr)) {
                    ok = true;
                }
            }
            LocalFree(sidStr);
        }
    }
    free(tokenUser);
    CloseHandle(hToken);
    return ok;
}

static DWORD WINAPI service_pipe_server_thread_proc(void*) {
    WCHAR pipeName[128] = {};
    if (!background_service_pipe_name(pipeName, ARRAY_COUNT(pipeName))) return 1;

    while (!g_serviceStopEvent || WaitForSingleObject(g_serviceStopEvent, 0) != WAIT_OBJECT_0) {
        PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        if (create_restricted_pipe_security_descriptor(&securityDescriptor)) {
            sa.lpSecurityDescriptor = securityDescriptor;
            debug_log("pipe_server: using restricted ACL for active console session user\n");
        } else if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)",
                SDDL_REVISION_1,
                &securityDescriptor,
                nullptr)) {
            sa.lpSecurityDescriptor = securityDescriptor;
            debug_log("pipe_server: falling back to Interactive Users ACL\n");
        }

        HANDLE pipe = CreateNamedPipeW(
            pipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            4,
            sizeof(ServiceResponse),
            sizeof(ServiceRequest),
            1000,
            sa.lpSecurityDescriptor ? &sa : nullptr);
        if (securityDescriptor) {
            LocalFree(securityDescriptor);
            securityDescriptor = nullptr;
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(250);
            continue;
        }

        OVERLAPPED ov = {};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            CloseHandle(pipe);
            continue;
        }
        BOOL connected = ConnectNamedPipe(pipe, &ov);
        DWORD connectErr = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && connectErr == ERROR_IO_PENDING) {
            HANDLE waitHandles[3] = { g_serviceStopEvent, g_servicePipeWakeEvent, ov.hEvent };
            DWORD waitResult = WaitForMultipleObjects(g_servicePipeWakeEvent ? 3 : 2, waitHandles, FALSE, INFINITE);
            if (waitResult == WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                CloseHandle(ov.hEvent);
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                break;
            }
            if (g_servicePipeWakeEvent && waitResult == WAIT_OBJECT_0 + 1) {
                CancelIoEx(pipe, &ov);
                CloseHandle(ov.hEvent);
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                continue;
            }
            connected = waitResult == WAIT_OBJECT_0 + (g_servicePipeWakeEvent ? 2 : 1);
        } else if (!connected && connectErr == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        }
        CloseHandle(ov.hEvent);
        if (!connected) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }

        ServiceRequest request = {};
        ServiceResponse response = {};
        response.magic = SERVICE_PROTOCOL_MAGIC;
        response.version = SERVICE_PROTOCOL_VERSION;
        response.serviceBuildNumber = (DWORD)APP_BUILD_NUMBER;
        StringCchCopyA(response.serviceVersion, ARRAY_COUNT(response.serviceVersion), APP_VERSION);
        char callerUser[256] = {};
        DWORD callerSessionId = (DWORD)-1;
        DWORD callerPid = 0;
        char pipeErr[256] = {};
        if (!service_pipe_read_exact(pipe, &request, sizeof(request), SERVICE_PIPE_SERVER_IO_TIMEOUT_MS, "reading service request", pipeErr, sizeof(pipeErr))) {
            debug_log("service_pipe_server: dropping stalled or invalid client read: %s\n", pipeErr[0] ? pipeErr : "unknown");
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }
        request.source[ARRAY_COUNT(request.source) - 1] = '\0';
        request.path[ARRAY_COUNT(request.path) - 1] = '\0';

        if (request.magic != SERVICE_PROTOCOL_MAGIC || request.version != SERVICE_PROTOCOL_VERSION) {
            response.status = SERVICE_STATUS_VERSION_MISMATCH;
            StringCchCopyA(response.message, ARRAY_COUNT(response.message), "Service protocol mismatch");
        } else if (!service_caller_is_authorized(pipe, request.source, response.message, ARRAY_COUNT(response.message), callerUser, sizeof(callerUser), &callerSessionId, &callerPid)) {
            response.status = SERVICE_STATUS_ERROR;
        } else {
            if (!g_serviceUserPathsResolved || g_serviceUserPathsSessionId != callerSessionId) {
                char pathErr[256] = {};
                if (resolve_service_user_data_paths(callerSessionId, pathErr, sizeof(pathErr))) {
                    if (!g_app.configPath[0]) {
                        set_default_config_path();
                    }
                    refresh_service_debug_logging_from_config();
                    debug_log("service_pipe_server: user paths resolved for command %u; no implicit startup apply\n",
                        (unsigned int)request.command);
                } else {
                    debug_log("service_pipe_server: failed to resolve user data paths: %s\n", pathErr);
                }
            }
            service_set_pending_operation_source(request.source[0] ? request.source : "service request");
            switch (request.command) {
                case SERVICE_CMD_PING:
                    response.status = SERVICE_STATUS_OK;
                    debug_log("service ping: service version=%s build=%lu protocol=%lu\n",
                        response.serviceVersion,
                        (unsigned long)response.serviceBuildNumber,
                        (unsigned long)response.version);
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), "pong");
                    break;
                case SERVICE_CMD_GET_SNAPSHOT: {
                    debug_log("service_pipe_server: snapshot only command; no profile apply\n");
                    char detail[256] = {};
                    lock_service_runtime();
                    bool ok = hardware_initialize(detail, sizeof(detail));
                    if (!ok) {
                        debug_log("service snapshot: hardware initialize unavailable: %s\n", detail[0] ? detail : "unknown");
                    } else {
                        bool offsetsOk = false;
                        if (!read_live_curve_snapshot_settled(3, 20, &offsetsOk)) {
                            debug_log("service snapshot: live curve refresh failed, returning cached curve\n");
                        } else if (!offsetsOk) {
                            debug_log("service snapshot: curve refresh completed without offset readback confirmation\n");
                        }
                        if (!refresh_global_state(detail, sizeof(detail))) {
                            debug_log("service snapshot: state refresh failed, returning cached globals%s%s\n",
                                detail[0] ? ": " : "",
                                detail[0] ? detail : "");
                        }
                        populate_control_state(&g_serviceControlState);
                        g_serviceControlStateValid = true;
                    }
                    response.status = SERVICE_STATUS_OK;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), ok ? "snapshot ready" : (detail[0] ? detail : "snapshot unavailable"));
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    unlock_service_runtime();
                    break;
                }
                case SERVICE_CMD_GET_TELEMETRY: {
                    debug_log("service_pipe_server: telemetry snapshot only command; no profile apply\n");
                    char detail[256] = {};
                    lock_service_runtime();
                    if (!service_refresh_telemetry_for_request(detail, sizeof(detail))) {
                        debug_log("service telemetry: hardware initialize unavailable: %s\n", detail[0] ? detail : "unknown");
                    } else {
                        debug_log("service telemetry: returning cached snapshot%s%s\n",
                            g_serviceTelemetryLastPollSource[0] ? " from " : "",
                            g_serviceTelemetryLastPollSource[0] ? g_serviceTelemetryLastPollSource : "");
                    }
                    response.status = SERVICE_STATUS_OK;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), detail[0] ? detail : "telemetry ready");
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    unlock_service_runtime();
                    break;
                }
                case SERVICE_CMD_APPLY: {
                    char result[512] = {};
                    debug_log("ipc raw: hasMem=%d memRaw=%d hasGpu=%d gpuRaw=%d exclRaw=%d\n",
                        request.desired.hasMemOffset ? 1 : 0,
                        request.desired.memOffsetMHz,
                        request.desired.hasGpuOffset ? 1 : 0,
                        request.desired.gpuOffsetMHz,
                        request.desired.gpuOffsetExcludeLowCount);
                    validate_desired_settings_for_ipc(&request.desired);
                    request.desired.resetOcBeforeApply = request.resetOcBeforeApply != 0;
                    debug_log("ipc validated: hasMem=%d mem=%d\n",
                        request.desired.hasMemOffset ? 1 : 0,
                        request.desired.memOffsetMHz);
                    lock_service_runtime();
                    bool ok = service_prepare_requested_gpu(&request, result, sizeof(result));
                    if (ok) ok = service_apply_desired_settings(&request.desired, (request.flags & 1u) != 0, result, sizeof(result));
                    if (ok) {
                        service_capture_owner_identity(callerUser, callerSessionId);
                    }
                    response.status = ok ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), result);
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    debug_log("service response APPLY: ok=%d controlValid=%d gpu=%d exclude=%d mem=%d power=%d fanMode=%d fanPct=%d\n",
                        ok ? 1 : 0,
                        response.controlState.valid ? 1 : 0,
                        response.controlState.gpuOffsetMHz,
                        response.controlState.gpuOffsetExcludeLowCount,
                        response.controlState.memOffsetMHz,
                        response.controlState.powerLimitPct,
                        response.controlState.fanMode,
                        response.controlState.fanFixedPercent);
                    unlock_service_runtime();
                    break;
                }
                case SERVICE_CMD_RESET: {
                    char result[512] = {};
                    lock_service_runtime();
                    bool ok = service_prepare_requested_gpu(&request, result, sizeof(result));
                    if (ok) ok = service_reset_all(result, sizeof(result));
                    if (ok) {
                        service_capture_owner_identity(callerUser, callerSessionId);
                    }
                    response.status = ok ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), result);
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    debug_log("service response RESET: ok=%d gpu=%d exclude=%d fanMode=%d fanPct=%d\n",
                        ok ? 1 : 0,
                        response.controlState.gpuOffsetMHz,
                        response.controlState.gpuOffsetExcludeLowCount,
                        response.controlState.fanMode,
                        response.controlState.fanFixedPercent);
                    unlock_service_runtime();
                    break;
                }
                case SERVICE_CMD_GET_ACTIVE_DESIRED:
                    lock_service_runtime();
                    response.status = SERVICE_STATUS_OK;
                    if (g_serviceHasActiveDesired) response.desired = g_serviceActiveDesired;
                    populate_service_snapshot(&response.snapshot);
                    if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    unlock_service_runtime();
                    break;
                case SERVICE_CMD_WRITE_LOG_SNAPSHOT:
                case SERVICE_CMD_WRITE_JSON_SNAPSHOT:
                case SERVICE_CMD_WRITE_PROBE_REPORT: {
                    char pathErr[256] = {};
                    if (!service_validate_file_write_path(request.path, pathErr, sizeof(pathErr))) {
                        response.status = SERVICE_STATUS_ERROR;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message), pathErr);
                        break;
                    }
                    char detail[256] = {};
                    lock_service_runtime();
                    bool ok = hardware_initialize(detail, sizeof(detail));
                    if (!ok && request.command != SERVICE_CMD_WRITE_PROBE_REPORT) {
                        response.status = SERVICE_STATUS_ERROR;
                        StringCchCopyA(response.message, ARRAY_COUNT(response.message), detail[0] ? detail : "Hardware initialization failed");
                    } else {
                        bool offsetsOk = false;
                        if (ok && !read_live_curve_snapshot_settled(4, 40, &offsetsOk)) {
                            debug_log("service file command: live curve refresh failed before file write\n");
                        }
                        if (ok) {
                            refresh_global_state(detail, sizeof(detail));
                        }
                        char fileErr[256] = {};
                        bool writeOk = false;
                        if (request.command == SERVICE_CMD_WRITE_LOG_SNAPSHOT) {
                            writeOk = write_log_snapshot(request.path, fileErr, sizeof(fileErr));
                        } else if (request.command == SERVICE_CMD_WRITE_JSON_SNAPSHOT) {
                            writeOk = write_json_snapshot(request.path, fileErr, sizeof(fileErr));
                        } else {
                            writeOk = write_probe_report(request.path, fileErr, sizeof(fileErr));
                        }
                        if (writeOk) {
                            char verifyErr[256] = {};
                            if (!service_verify_written_file_path(request.path, verifyErr, sizeof(verifyErr))) {
                                writeOk = false;
                                StringCchCopyA(fileErr, sizeof(fileErr), verifyErr);
                            }
                        }
                        response.status = writeOk ? SERVICE_STATUS_OK : SERVICE_STATUS_ERROR;
                        if (writeOk) {
                            StringCchPrintfA(response.message, ARRAY_COUNT(response.message), "Wrote %s", request.path[0] ? request.path : "requested output file");
                        } else {
                            StringCchCopyA(response.message, ARRAY_COUNT(response.message), fileErr[0] ? fileErr : "Failed writing requested file");
                        }
                        populate_service_snapshot(&response.snapshot);
                        if (g_serviceControlStateValid) response.controlState = g_serviceControlState;
                    }
                    unlock_service_runtime();
                    break;
                }
                default:
                    response.status = SERVICE_STATUS_ERROR;
                    StringCchCopyA(response.message, ARRAY_COUNT(response.message), "Unsupported service command");
                    break;
            }
        }

        pipeErr[0] = 0;
        if (!service_pipe_write_exact(pipe, &response, sizeof(response), SERVICE_PIPE_SERVER_IO_TIMEOUT_MS, "writing service response", pipeErr, sizeof(pipeErr))) {
            debug_log("service_pipe_server: response write failed: %s\n", pipeErr[0] ? pipeErr : "unknown");
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    return 0;
}

static HANDLE g_servicePipeThread = nullptr;

static void WINAPI service_control_handler(DWORD control) {
    if (control != SERVICE_CONTROL_STOP && control != SERVICE_CONTROL_SHUTDOWN) return;
    g_serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    if (g_serviceStopEvent) SetEvent(g_serviceStopEvent);
    if (g_servicePipeWakeEvent) SetEvent(g_servicePipeWakeEvent);
}

static void WINAPI service_main(DWORD, LPWSTR*) {
    g_app.isServiceProcess = true;
    SetUnhandledExceptionFilter(green_curve_unhandled_exception_filter);

    // Suppress all debug logging until the user's config is read.
    // This guarantees zero file I/O when the user has opted out.
    bool envExplicitlyEnabled = false;
    {
        char debugEnvBuf[16] = {};
        DWORD debugEnvLen = GetEnvironmentVariableA(APP_DEBUG_ENV, debugEnvBuf, ARRAY_COUNT(debugEnvBuf));
        if (debugEnvLen > 0 && !(debugEnvBuf[0] == '0' && debugEnvBuf[1] == '\0')) {
            envExplicitlyEnabled = true;
        }
    }
    g_debug_logging = envExplicitlyEnabled;

    g_serviceStatusHandle = RegisterServiceCtrlHandlerW(L"GreenCurveService", service_control_handler);
    if (!g_serviceStatusHandle) return;

    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

    service_resolve_active_user_paths_for_startup("service_main startup");
    if (!envExplicitlyEnabled) {
        refresh_service_debug_logging_from_config();
    }

    if (g_debug_logging) {
        g_debugSessionStartTickMs = GetTickCount64();
        debug_log_session_marker("BEGIN", "service", "service_main bootstrap");
        debug_log_session_marker("BEGIN", "service", "service_main startup");
    }

    ensure_service_runtime_lock();
    g_serviceStopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_servicePipeWakeEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (g_app.fanCurveRuntimeActive || g_app.fanFixedRuntimeActive) {
        ensure_service_fan_runtime_thread();
    }

    DWORD threadId = 0;
    g_servicePipeThread = CreateThread(nullptr, 128 * 1024, service_pipe_server_thread_proc, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &threadId);
    g_serviceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);

    debug_log("service_main: running; hardware writes only on explicit client request\n");

    if (g_serviceStopEvent) WaitForSingleObject(g_serviceStopEvent, INFINITE);

    stop_service_fan_runtime_thread();
    if (g_servicePipeThread) {
        if (g_servicePipeWakeEvent) SetEvent(g_servicePipeWakeEvent);
        WaitForSingleObject(g_servicePipeThread, INFINITE);
        CloseHandle(g_servicePipeThread);
        g_servicePipeThread = nullptr;
    }
    if (g_servicePipeWakeEvent) {
        CloseHandle(g_servicePipeWakeEvent);
        g_servicePipeWakeEvent = nullptr;
    }
    if (g_serviceFanStopEvent) {
        CloseHandle(g_serviceFanStopEvent);
        g_serviceFanStopEvent = nullptr;
    }
    if (g_serviceStopEvent) {
        CloseHandle(g_serviceStopEvent);
        g_serviceStopEvent = nullptr;
    }
    if (g_serviceRuntimeLock) {
        CloseHandle(g_serviceRuntimeLock);
        g_serviceRuntimeLock = nullptr;
    }
    // Return fan control to driver auto on graceful shutdown.
    char fanDetail[128] = {};
    nvml_set_fan_auto(fanDetail, sizeof(fanDetail));
    close_nvml();
    if (g_app.hNvApi) {
        FreeLibrary(g_app.hNvApi);
        g_app.hNvApi = nullptr;
    }
    g_serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    if (g_debug_logging) {
        ULONGLONG elapsedMs = g_debugSessionStartTickMs ? (GetTickCount64() - g_debugSessionStartTickMs) : 0;
        char extra[128] = {};
        StringCchPrintfA(extra, ARRAY_COUNT(extra), "service_main shutdown uptimeMs=%llu", elapsedMs);
        debug_log_session_marker("END", "service", extra);
    }
    close_debug_log_file();
    DeleteCriticalSection(&g_debugLogLock);
}

static bool should_suppress_startup_ui() {
    return g_app.launchedFromLogon || g_app.startHiddenToTray;
}

static const char* nvml_err_name(nvmlReturn_t r) {
